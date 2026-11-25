/* storage_state.c — 阅读进度(双 32 B 槽)与书签(双文件 + 代次)。
 *
 * DESIGN.md §6.2 进度:
 *   0 'P' | 1 ver=1 | 2..3 flags(bit4-5 enc) | 4..7 seq u32 LE |
 *   8..15 offset u64 LE | 16..19 page u32 | 20 percent | 21..27 零 |
 *   28..31 crc32(前 28 B)
 *   写 slot=(seq+1)&1 → 32 B → fflush → fsync → 回读校验,成功才 seq++;
 *   失败重试 1 次,仍失败返回 ST_ERR_IO。读两槽取有效且 seq 大者。
 *   打开书不得清零:storage_pos_set_enc() 保留 offset/page/percent、只改
 *   flags 的 enc 位并重写同一槽(§6.2 / 修订 #23)。
 *   fsync 只到 FatFs 层,属 best-effort:正常掉电最多丢 1 页,CRC 保证
 *   永远不会读到错误偏移。代码只做"写后 fsync + 回读校验";真实掉电
 *   20 次是 §15 #10 的真机验收项(S10),软件模拟(工具断电/复位)只能
 *   验证 CRC 路径,不能替代。
 *
 * DESIGN.md §6.3 书签:
 *   0 "BMK1" | 4 gen u32 | 8 hdr_crc(前 8 B) | 12 reserved |
 *   16 N×24 B 记录 | 末尾 8 B:magic 0x424D4B45 + crc32(除末尾 8 B)
 *   记录:0 'B' | 1 flags(bit0 墓碑)| 2..3 保留 | 4..11 offset u64 |
 *   12..15 page u32 | 16..19 保留 | 20..23 crc32(前 20 B)
 *   读取以记录 CRC 为准;尾部只做诊断。两文件都扫,gen 大者胜(同代次
 *   比活跃记录数);新文件头坏则回退旧文件。删除 = 追加墓碑。
 *   压缩:文件 >8 KB 或活跃 >8 时写另一个文件(头+≤8 条+尾),gen+1,
 *   不 rename/unlink(§6.3 解释:FatFs 的 f_rename 目标存在即失败)。
 */
#include "storage_internal.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "rc_common.h"

static const char *TAG = ST_TAG;

/* 扫描时最多记住的活跃书签数(活跃上限 8,留余量给外来文件) */
#define ST_BMK_SCAN_MAX 32

/* ------------------------------------------------------------------ */
/* 小端读写                                                           */
/* ------------------------------------------------------------------ */

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void put_u64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) {
        p[i] = (uint8_t)((v >> (8 * i)) & 0xFF);
    }
}

static uint64_t get_u64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v |= ((uint64_t)p[i]) << (8 * i);
    }
    return v;
}

/* 例:/sdcard/reader/state/1a2b3c4d.pos */
static int state_path(uint32_t id, const char *suffix, char *out, size_t n)
{
    int w = snprintf(out, n, "%s/%08" PRIx32 "%s", READER_DIR_STATE, id, suffix);
    if (w <= 0 || (size_t)w >= n) {
        out[0] = '\0';
        return -1;
    }
    return 0;
}

/* ================================================================== */
/* 进度(§6.2)                                                        */
/* ================================================================== */

typedef struct {
    bool valid[2];
    storage_pos_t pos[2];
    uint32_t seq[2];
    uint8_t raw[2][ST_POS_SLOT_BYTES];
    int best;                    /* 有效且 seq 最大的槽;-1 = 无 */
} pos_file_t;

static void pos_encode_slot(uint8_t *s, uint32_t seq, uint64_t offset, uint32_t page,
                            uint8_t percent, uint8_t enc)
{
    memset(s, 0, ST_POS_SLOT_BYTES);
    s[0] = (uint8_t)ST_POS_MAGIC;
    s[1] = (uint8_t)ST_POS_VERSION;
    uint16_t flags = (uint16_t)(((uint16_t)enc & 0x3u) << ST_POS_FLAG_ENC_SHIFT);
    put_u16(s + 2, flags);
    put_u32(s + 4, seq);
    put_u64(s + ST_POS_OFF_OFFSET, offset);
    put_u32(s + ST_POS_OFF_PAGE, page);
    s[ST_POS_OFF_PERCENT] = percent;
    /* 21..27 由 memset 置 0 */
    put_u32(s + ST_POS_OFF_CRC, rc_crc32_ieee(s, ST_POS_OFF_CRC));
}

static bool pos_decode_slot(const uint8_t *s, storage_pos_t *out, uint32_t *seq)
{
    if (s[0] != (uint8_t)ST_POS_MAGIC || s[1] != (uint8_t)ST_POS_VERSION) {
        return false;
    }
    if (get_u32(s + ST_POS_OFF_CRC) != rc_crc32_ieee(s, ST_POS_OFF_CRC)) {
        return false;
    }
    uint16_t flags = get_u16(s + 2);
    out->enc = (uint8_t)((flags & ST_POS_FLAG_ENC_MASK) >> ST_POS_FLAG_ENC_SHIFT);
    out->offset = get_u64(s + ST_POS_OFF_OFFSET);
    out->page = get_u32(s + ST_POS_OFF_PAGE);
    out->percent = s[ST_POS_OFF_PERCENT];
    if (out->percent > 100) {
        out->percent = 100;              /* 防御:外部写坏的值 */
    }
    out->valid = true;
    *seq = get_u32(s + 4);
    return true;
}

static int pos_read_file(uint32_t id, pos_file_t *pf)
{
    memset(pf, 0, sizeof(*pf));
    pf->best = -1;

    char path[ST_PATH_MAX];
    if (state_path(id, ".pos", path, sizeof(path)) != 0) {
        return ST_ERR_BAD_PARAM;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        if (errno == ENOENT) {
            return ST_ERR_NOT_FOUND;     /* 还没有进度 */
        }
        st_sd_fail("fopen(.pos) failed");
        return ST_ERR_IO;
    }

    uint8_t buf[ST_POS_FILE_BYTES];
    memset(buf, 0, sizeof(buf));
    size_t got = fread(buf, 1, sizeof(buf), f);
    bool ioerr = ferror(f) != 0;
    fclose(f);
    if (ioerr) {
        st_sd_fail("fread(.pos) failed");
        return ST_ERR_IO;
    }

    for (int i = 0; i < 2; i++) {
        size_t need = (size_t)(i + 1) * ST_POS_SLOT_BYTES;
        if (got < need) {
            continue;                    /* 槽未落盘 */
        }
        if (!pos_decode_slot(buf + (size_t)i * ST_POS_SLOT_BYTES, &pf->pos[i], &pf->seq[i])) {
            continue;
        }
        pf->valid[i] = true;
        memcpy(pf->raw[i], buf + (size_t)i * ST_POS_SLOT_BYTES, ST_POS_SLOT_BYTES);
        if (pf->best < 0 || pf->seq[i] > pf->seq[pf->best]) {
            pf->best = i;
        }
    }
    return (pf->best >= 0) ? ST_OK : ST_ERR_NOT_FOUND;
}

/* 写一个槽:写 → fflush → fsync → 回读比对,失败重试 1 次。
 * 只在成功时返回 ST_OK;两次都失败视为卡故障。 */
static int pos_write_slot(uint32_t id, int slot, const uint8_t raw[ST_POS_SLOT_BYTES])
{
    char path[ST_PATH_MAX];
    if (state_path(id, ".pos", path, sizeof(path)) != 0) {
        return ST_ERR_BAD_PARAM;
    }
    long off = (long)slot * ST_POS_SLOT_BYTES;

    for (int attempt = 0; attempt < 2; attempt++) {
        if (attempt > 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        FILE *f = fopen(path, "r+b");
        if (!f && errno == ENOENT) {
            f = fopen(path, "w+b");      /* 新建 .pos(只为写 offset 0/首次进度) */
        }
        if (!f) {
            continue;
        }

        bool ok = true;
        uint8_t back[ST_POS_SLOT_BYTES];
        if (fseek(f, off, SEEK_SET) != 0) {
            ok = false;
        } else if (fwrite(raw, 1, ST_POS_SLOT_BYTES, f) != ST_POS_SLOT_BYTES) {
            ok = false;
        } else if (fflush(f) != 0) {
            ok = false;
        } else if (fsync(fileno(f)) != 0) {
            ok = false;
        } else if (fseek(f, off, SEEK_SET) != 0) {
            ok = false;
        } else if (fread(back, 1, sizeof(back), f) != sizeof(back)) {
            ok = false;
        } else if (memcmp(back, raw, sizeof(back)) != 0) {
            ok = false;                  /* 回读不一致:重写一次 */
        }
        if (fclose(f) != 0) {
            ok = false;
        }
        if (ok) {
            return ST_OK;
        }
        ESP_LOGW(TAG, "pos slot write attempt %d failed (id=%08" PRIx32 ")", attempt + 1, id);
    }

    st_sd_fail("pos write verify failed");
    return ST_ERR_IO;
}

int storage_pos_load(uint32_t id, storage_pos_t *out)
{
    if (!out) {
        return ST_ERR_BAD_PARAM;
    }
    memset(out, 0, sizeof(*out));
    out->valid = false;

    int rc = st_fs_take(ST_LOCK_MS);
    if (rc != ST_OK) {
        return rc;
    }

    pos_file_t pf;
    rc = pos_read_file(id, &pf);
    if (rc == ST_OK) {
        *out = pf.pos[pf.best];
        out->valid = true;
    }
    st_unlock();
    return rc;                           /* NOT_FOUND:调用方从第 0 页开始 */
}

int storage_pos_save(uint32_t id, uint64_t offset, uint32_t page, uint8_t percent, uint8_t enc)
{
    if (percent > 100) {
        percent = 100;
    }
    enc = (uint8_t)(enc & 0x3);

    int rc = st_fs_take(ST_LOCK_MS);
    if (rc != ST_OK) {
        return rc;
    }

    pos_file_t pf;
    rc = pos_read_file(id, &pf);
    uint32_t seq = 0;
    if (rc == ST_OK) {
        seq = pf.seq[pf.best];
    } else if (rc == ST_ERR_IO) {
        st_unlock();
        return rc;
    }
    /* NOT_FOUND:没有有效槽,seq 从 0 起,新 seq=1 → slot 1 */

    uint32_t new_seq = seq + 1u;
    uint8_t raw[ST_POS_SLOT_BYTES];
    pos_encode_slot(raw, new_seq, offset, page, percent, enc);
    int slot = (int)(new_seq & 1u);

    rc = pos_write_slot(id, slot, raw);
    st_unlock();
    return rc;
}

int storage_pos_set_enc(uint32_t id, uint8_t enc)
{
    enc = (uint8_t)(enc & 0x3);

    int rc = st_fs_take(ST_LOCK_MS);
    if (rc != ST_OK) {
        return rc;
    }

    pos_file_t pf;
    rc = pos_read_file(id, &pf);
    if (rc != ST_OK) {
        /* 没有有效进度(文件不存在/两槽都坏)就不建文件(S6 确认)。
         * 凭空写一个 offset 0 的槽会把调用方刚恢复的位置冲掉,所以这里只
         * 报 ST_ERR_NOT_FOUND(或 IO/BUSY),由调用方决定初始位置:
         *   - 还没有 .pos:storage_pos_save(id, 起始 offset, page, percent,
         *     enc) 一次写全(enc 就在这条记录里),不需要再调本函数;
         *   - 已有进度、只想改 enc:本函数(保留 offset/page/percent、
         *     重写同一槽)。
         * 前置条件:pos_load 返回 NOT_FOUND,或 valid=false。 */
        st_unlock();
        return rc;
    }

    int slot = pf.best;
    uint8_t raw[ST_POS_SLOT_BYTES];
    memcpy(raw, pf.raw[slot], sizeof(raw));
    uint16_t flags = get_u16(raw + 2);
    flags = (uint16_t)((flags & ~(uint16_t)ST_POS_FLAG_ENC_MASK) |
                       ((uint16_t)(enc & 0x3u) << ST_POS_FLAG_ENC_SHIFT));
    put_u16(raw + 2, flags);
    put_u32(raw + ST_POS_OFF_CRC, rc_crc32_ieee(raw, ST_POS_OFF_CRC));

    /* 同一槽、同一 seq 重写:保留 offset/page/percent(修订 #23) */
    rc = pos_write_slot(id, slot, raw);
    st_unlock();
    return rc;
}

int storage_pos_flush(uint32_t timeout_ms)
{
    (void)timeout_ms;
    /* 所有写路径都已 fflush+fsync+回读,没有待刷缓冲;这里只确认卡仍可用 */
    if (!st_sd_is_ok()) {
        return ST_ERR_NO_SD;
    }
    return ST_OK;
}

/* ================================================================== */
/* 书签(§6.3)                                                        */
/* ================================================================== */

typedef struct {
    bool header_ok;
    uint32_t gen;
    uint32_t prefix_crc;         /* [0, 16+N*24) 的 CRC,追加时续算尾部 CRC */
    int records;                 /* 扫描到的有效记录数(含墓碑)= N */
    int active;
    bool overflow;               /* 活跃记录超过 ST_BMK_SCAN_MAX */
    uint64_t offs[ST_BMK_SCAN_MAX];
    uint32_t pages[ST_BMK_SCAN_MAX];
    long size;                   /* 文件物理大小(诊断) */
} bmk_scan_t;

static bmk_scan_t s_bmk[2];
static char s_bmk_path[2][ST_PATH_MAX];

static int bmk_find(const bmk_scan_t *s, uint64_t off)
{
    for (int i = 0; i < s->active; i++) {
        if (s->offs[i] == off) {
            return i;
        }
    }
    return -1;
}

static void bmk_drop(bmk_scan_t *s, uint64_t off)
{
    int w = 0;
    for (int i = 0; i < s->active; i++) {
        if (s->offs[i] == off) {
            continue;                /* 删除(墓碑)可能重复出现 */
        }
        if (w != i) {
            s->offs[w] = s->offs[i];
            s->pages[w] = s->pages[i];
        }
        w++;
    }
    s->active = w;
}

static void bmk_make_record(uint8_t rec[ST_BMK_RECORD_BYTES], uint64_t off, uint32_t page,
                            bool tombstone)
{
    memset(rec, 0, ST_BMK_RECORD_BYTES);
    rec[0] = 'B';
    rec[1] = tombstone ? 0x01 : 0x00;
    put_u64(rec + 4, off);
    put_u32(rec + 12, page);
    put_u32(rec + 20, rc_crc32_ieee(rec, 20));
}

/* 扫一个书签文件。以记录 CRC 为准;遇到第一条 CRC 错/不足 24 B 即停止。
 *
 * 墓碑(S4):DESIGN §6.3 原文写"遇墓碑即结束",这里故意改成
 * "按删除处理并继续扫描"。若在墓碑处结束,删除第 1 条之后再追加的书签
 * 会永远不可见(墓碑之后的所有记录都被丢弃),§6.3 自己的验收项
 * "删 1 可加"就不成立。记录级 CRC 已经保证墓碑之后的记录可独立校验,
 * 继续扫不会引入歧义。建议文档改成:
 *   从偏移 16 起逐条读 24 B,CRC 正确则采纳(墓碑 = 删除该 offset),
 *   CRC 错/不足 24 B 处结束;尾部 magic/CRC 只做诊断。
 */
static int bmk_scan_file(const char *path, bmk_scan_t *out)
{
    memset(out, 0, sizeof(*out));

    errno = 0;
    FILE *f = fopen(path, "rb");
    if (!f) {
        if (errno != ENOENT) {
            st_sd_fail("fopen(bmk) failed");
            return ST_ERR_IO;
        }
        return ST_OK;                /* 还没有书签文件 */
    }

    uint8_t hdr[ST_BMK_HEADER_BYTES];
    size_t got = fread(hdr, 1, sizeof(hdr), f);
    if (got == sizeof(hdr) && memcmp(hdr, ST_BMK_FILE_MAGIC, 4) == 0 &&
        get_u32(hdr + 8) == rc_crc32_ieee(hdr, 8)) {
        out->header_ok = true;
        out->gen = get_u32(hdr + 4);
        out->prefix_crc = rc_crc32_ieee(hdr, ST_BMK_HEADER_BYTES);

        for (;;) {
            uint8_t rec[ST_BMK_RECORD_BYTES];
            size_t r = fread(rec, 1, sizeof(rec), f);
            if (r != sizeof(rec)) {
                break;               /* 不足 24 B(含尾部 8 B) */
            }
            if (rec[0] != 'B') {
                break;
            }
            if (get_u32(rec + 20) != rc_crc32_ieee(rec, 20)) {
                break;               /* 第一条坏记录:撕裂点 */
            }
            out->records++;
            out->prefix_crc = rc_crc32_update(out->prefix_crc, rec, sizeof(rec));

            uint64_t off = get_u64(rec + 4);
            uint32_t page = get_u32(rec + 12);
            if (rec[1] & 0x01) {
                bmk_drop(out, off);
            } else if (bmk_find(out, off) < 0) {
                if (out->active < ST_BMK_SCAN_MAX) {
                    out->offs[out->active] = off;
                    out->pages[out->active] = page;
                    out->active++;
                } else {
                    out->overflow = true;
                }
            }
        }
        /* 尾部的 magic/CRC 只做诊断,不决定整文件是否可用(§6.3 修订 #29) */
        if (fseek(f, 0, SEEK_END) == 0) {
            out->size = ftell(f);
        }
    }

    bool ioerr = ferror(f) != 0;
    fclose(f);
    if (ioerr) {
        st_sd_fail("fread(bmk) failed");
        return ST_ERR_IO;
    }
    return ST_OK;
}

static int bmk_paths(uint32_t id)
{
    if (state_path(id, ".bmk0", s_bmk_path[0], sizeof(s_bmk_path[0])) != 0 ||
        state_path(id, ".bmk1", s_bmk_path[1], sizeof(s_bmk_path[1])) != 0) {
        return -1;
    }
    return 0;
}

/* 扫两个文件并选出赢家:两个头都有效时 gen 大者胜(同代次比活跃记录数);
 * 较新的文件头坏则回退旧文件。返回 0/1,或 -1(两个都没有有效头)。
 *
 * S5 确认:gen 优先、同代再比活跃数,不会让"删除后旧代复活":
 *   - 文件只有两种写动作:向当前赢家追加(新记录/墓碑),或把另一份文件
 *     整写成 gen+1 的压缩结果。压缩不动当前赢家,所以上一代文件里的
 *     墓碑一直都在,一旦回退到它就仍是"已删除",不会复活。
 *   - 回退的代价只是丢掉"最近一次压缩之后追加的记录",即 §6.3 写明的
 *     "新文件头坏则回退旧"。
 *   - 两边都有效且 gen 相同在实际写入里不会出现(压缩至少把 gen+1);
 *     "同代比活跃数"只是防御,取信息更多的那个。
 */
static int bmk_load(uint32_t id, int *err)
{
    *err = ST_OK;
    if (bmk_paths(id) != 0) {
        *err = ST_ERR_BAD_PARAM;
        return -1;
    }
    for (int i = 0; i < 2; i++) {
        int rc = bmk_scan_file(s_bmk_path[i], &s_bmk[i]);
        if (rc != ST_OK) {
            *err = rc;
            return -1;
        }
    }
    if (s_bmk[0].header_ok && s_bmk[1].header_ok) {
        if (s_bmk[0].gen != s_bmk[1].gen) {
            return (s_bmk[0].gen > s_bmk[1].gen) ? 0 : 1;
        }
        if (s_bmk[0].active != s_bmk[1].active) {
            return (s_bmk[0].active > s_bmk[1].active) ? 0 : 1;
        }
        return 0;
    }
    if (s_bmk[0].header_ok) {
        return 0;
    }
    if (s_bmk[1].header_ok) {
        return 1;
    }
    return -1;
}

/* 整个文件一次成型。先写体(记录+尾)并 fsync,最后写 16 B 头:
 * 掉电时新文件因头无效而不会被选中(§6.3 "新文件头坏则回退旧")。 */
static int bmk_write_file(const char *path, const uint8_t *img, size_t total)
{
    if (total < ST_BMK_HEADER_BYTES + ST_BMK_TAIL_BYTES) {
        return ST_ERR_BAD_PARAM;
    }
    size_t body = total - ST_BMK_HEADER_BYTES;

    FILE *f = fopen(path, "wb");    /* 截断重建(§6.3:压缩/建文件都是整写) */
    if (!f) {
        st_sd_fail("fopen(bmk wb) failed");
        return ST_ERR_IO;
    }

    bool ok = true;
    /* 体(记录+尾)写在偏移 16;头最后写,见函数注释 */
    if (fseek(f, (long)ST_BMK_HEADER_BYTES, SEEK_SET) != 0) {
        ok = false;
    } else if (fwrite(img + ST_BMK_HEADER_BYTES, 1, body, f) != body) {
        ok = false;
    } else if (fflush(f) != 0) {
        ok = false;
    } else if (fsync(fileno(f)) != 0) {
        ok = false;
    } else if (fseek(f, 0, SEEK_SET) != 0) {
        ok = false;
    } else if (fwrite(img, 1, ST_BMK_HEADER_BYTES, f) != ST_BMK_HEADER_BYTES) {
        ok = false;
    } else if (fflush(f) != 0) {
        ok = false;
    } else if (fsync(fileno(f)) != 0) {
        ok = false;
    }
    if (fclose(f) != 0) {
        ok = false;
    }
    if (!ok) {
        st_sd_fail("bmk write failed");
        return ST_ERR_IO;
    }
    return ST_OK;
}

/* 追加一条记录:一次 fwrite 写 24 B 新记录 + 8 B 新尾(§6.3)。
 * 尾部 CRC 由扫描时累计的前缀 CRC 续算,不需要重读整个文件。 */
static int bmk_append(int idx, const uint8_t rec[ST_BMK_RECORD_BYTES])
{
    bmk_scan_t *s = &s_bmk[idx];
    long base = ST_BMK_HEADER_BYTES + (long)s->records * ST_BMK_RECORD_BYTES;
    uint32_t tail_crc = rc_crc32_update(s->prefix_crc, rec, ST_BMK_RECORD_BYTES);

    uint8_t buf[ST_BMK_RECORD_BYTES + ST_BMK_TAIL_BYTES];
    memcpy(buf, rec, ST_BMK_RECORD_BYTES);
    put_u32(buf + ST_BMK_RECORD_BYTES, ST_BMK_MAGIC);
    put_u32(buf + ST_BMK_RECORD_BYTES + 4, tail_crc);

    FILE *f = fopen(s_bmk_path[idx], "r+b");
    if (!f) {
        st_sd_fail("fopen(bmk r+b) failed");
        return ST_ERR_IO;
    }

    bool ok = true;
    if (fseek(f, base, SEEK_SET) != 0) {
        ok = false;
    } else if (fwrite(buf, 1, sizeof(buf), f) != sizeof(buf)) {
        ok = false;
    } else if (fflush(f) != 0) {
        ok = false;
    } else if (ftruncate(fileno(f), (off_t)(base + (long)sizeof(buf))) != 0) {
        ok = false;                  /* 丢弃撕裂点之后的旧垃圾 */
    } else if (fsync(fileno(f)) != 0) {
        ok = false;
    }
    if (fclose(f) != 0) {
        ok = false;
    }
    if (!ok) {
        st_sd_fail("bmk append failed");
        return ST_ERR_IO;
    }

    s->records++;
    s->prefix_crc = tail_crc;
    s->size = base + (long)sizeof(buf);
    return ST_OK;
}

static bool bmk_needs_compact(const bmk_scan_t *s)
{
    return s->size > ST_BMK_MAX_FILE_BYTES || s->active > ST_BMK_ACTIVE_MAX;
}

/* 压缩到另一个文件:头 + ≤8 条有效记录 + 尾,gen+1;不 rename/unlink。
 * 调用方已持有 FS 锁,并保证 s_bmk[] 是刚扫过的状态。 */
static int bmk_compact(uint32_t id, int idx)
{
    (void)id;
    int dst = idx ^ 1;
    bmk_scan_t *s = &s_bmk[idx];
    int n = (s->active > ST_BMK_ACTIVE_MAX) ? ST_BMK_ACTIVE_MAX : s->active;
    if (s->active > ST_BMK_ACTIVE_MAX) {
        ESP_LOGW(TAG, "bmk compaction drops %d record(s) beyond the limit of %d",
                 s->active - ST_BMK_ACTIVE_MAX, ST_BMK_ACTIVE_MAX);
    }
    uint32_t gen = s->gen + 1u;

    uint8_t img[ST_BMK_HEADER_BYTES + ST_BMK_ACTIVE_MAX * ST_BMK_RECORD_BYTES +
                ST_BMK_TAIL_BYTES];
    memset(img, 0, sizeof(img));
    memcpy(img, ST_BMK_FILE_MAGIC, 4);
    put_u32(img + 4, gen);
    put_u32(img + 8, rc_crc32_ieee(img, 8));   /* 8..11 尚为 0 */
    put_u32(img + 12, 0);

    for (int i = 0; i < n; i++) {
        uint8_t *p = img + ST_BMK_HEADER_BYTES + (size_t)i * ST_BMK_RECORD_BYTES;
        p[0] = 'B';
        p[1] = 0x00;
        put_u64(p + 4, s->offs[i]);
        put_u32(p + 12, s->pages[i]);
        put_u32(p + 20, rc_crc32_ieee(p, 20));
    }

    size_t body = (size_t)n * ST_BMK_RECORD_BYTES;
    uint8_t *tail = img + ST_BMK_HEADER_BYTES + body;
    put_u32(tail, ST_BMK_MAGIC);
    put_u32(tail + 4, rc_crc32_ieee(img, ST_BMK_HEADER_BYTES + body));
    size_t total = ST_BMK_HEADER_BYTES + body + ST_BMK_TAIL_BYTES;

    int rc = bmk_write_file(s_bmk_path[dst], img, total);
    if (rc != ST_OK) {
        return rc;
    }

    /* 回读校验:新文件必须头有效、gen 正确、活跃条数一致 */
    rc = bmk_scan_file(s_bmk_path[dst], &s_bmk[dst]);
    if (rc != ST_OK) {
        return rc;
    }
    if (!s_bmk[dst].header_ok || s_bmk[dst].gen != gen || s_bmk[dst].active != n) {
        ESP_LOGE(TAG, "bmk compaction verify failed (gen=%" PRIu32 ", active=%d/%d)", gen,
                 s_bmk[dst].active, n);
        st_sd_set_error("bmk compaction verify failed");
        return ST_ERR_IO;
    }
    ESP_LOGI(TAG, "bmk compacted to .bmk%d (gen=%" PRIu32 ", %d record(s))", dst, gen, n);
    return ST_OK;
}

int storage_bmk_list(uint32_t id, uint64_t *offs, uint32_t *pages, int max)
{
    if (max < 0 || (max > 0 && (!offs || !pages))) {
        return ST_ERR_BAD_PARAM;
    }
    int rc = st_fs_take(ST_LOCK_MS);
    if (rc != ST_OK) {
        return rc;
    }

    int err = ST_OK;
    int idx = bmk_load(id, &err);
    if (err != ST_OK) {
        st_unlock();
        return err;
    }

    int n = 0;
    if (idx >= 0) {
        n = (s_bmk[idx].active < max) ? s_bmk[idx].active : max;
        for (int i = 0; i < n; i++) {
            offs[i] = s_bmk[idx].offs[i];
            pages[i] = s_bmk[idx].pages[i];
        }
    }
    st_unlock();
    return n;
}

int storage_bmk_add(uint32_t id, uint64_t off, uint32_t page)
{
    int rc = st_fs_take(ST_LOCK_MS);
    if (rc != ST_OK) {
        return rc;
    }

    int err = ST_OK;
    int idx = bmk_load(id, &err);
    if (err != ST_OK) {
        st_unlock();
        return err;
    }

    if (idx < 0) {
        /* 首次添加:建 .bmk0,gen=1,只有头+尾 */
        if (bmk_paths(id) != 0) {
            st_unlock();
            return ST_ERR_BAD_PARAM;
        }
        uint8_t img[ST_BMK_HEADER_BYTES + ST_BMK_TAIL_BYTES];
        memset(img, 0, sizeof(img));
        memcpy(img, ST_BMK_FILE_MAGIC, 4);
        put_u32(img + 4, 1u);
        put_u32(img + 8, rc_crc32_ieee(img, 8));
        put_u32(img + ST_BMK_HEADER_BYTES, ST_BMK_MAGIC);
        put_u32(img + ST_BMK_HEADER_BYTES + 4, rc_crc32_ieee(img, ST_BMK_HEADER_BYTES));
        rc = bmk_write_file(s_bmk_path[0], img, sizeof(img));
        if (rc != ST_OK) {
            st_unlock();
            return rc;
        }
        rc = bmk_scan_file(s_bmk_path[0], &s_bmk[0]);
        if (rc != ST_OK) {
            st_unlock();
            return rc;
        }
        idx = 0;
    }

    bmk_scan_t *s = &s_bmk[idx];
    if (bmk_find(s, off) >= 0) {
        st_unlock();
        return ST_OK;                /* 幂等:同一位置不重复添加 */
    }
    if (s->active >= ST_BMK_ACTIVE_MAX || s->overflow) {
        st_unlock();
        return ST_ERR_NO_SPACE;      /* 第 9 条:UI 显示"书签已满(8)" */
    }

    uint8_t rec[ST_BMK_RECORD_BYTES];
    bmk_make_record(rec, off, page, false);
    rc = bmk_append(idx, rec);
    if (rc != ST_OK) {
        st_unlock();
        return rc;
    }
    s->offs[s->active] = off;
    s->pages[s->active] = page;
    s->active++;

    if (bmk_needs_compact(s)) {
        /* 记录已落盘;压缩失败不把这次添加判失败(下轮再试) */
        int crc2 = bmk_compact(id, idx);
        if (crc2 != ST_OK) {
            ESP_LOGW(TAG, "bmk compaction deferred: %d", crc2);
        }
    }
    st_unlock();
    return ST_OK;
}

int storage_bmk_remove(uint32_t id, uint64_t off)
{
    int rc = st_fs_take(ST_LOCK_MS);
    if (rc != ST_OK) {
        return rc;
    }

    int err = ST_OK;
    int idx = bmk_load(id, &err);
    if (err != ST_OK) {
        st_unlock();
        return err;
    }
    if (idx < 0) {
        st_unlock();
        return ST_ERR_NOT_FOUND;
    }

    bmk_scan_t *s = &s_bmk[idx];
    int pos = bmk_find(s, off);
    if (pos < 0) {
        st_unlock();
        return ST_ERR_NOT_FOUND;
    }

    /* 删除 = 追加墓碑(带原 page,便于诊断) */
    uint8_t rec[ST_BMK_RECORD_BYTES];
    bmk_make_record(rec, off, s->pages[pos], true);
    rc = bmk_append(idx, rec);
    if (rc != ST_OK) {
        st_unlock();
        return rc;
    }
    bmk_drop(s, off);

    if (bmk_needs_compact(s)) {
        int crc2 = bmk_compact(id, idx);
        if (crc2 != ST_OK) {
            ESP_LOGW(TAG, "bmk compaction deferred: %d", crc2);
        }
    }
    st_unlock();
    return ST_OK;
}
