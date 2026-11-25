/* storage_upload.c — BLE 传书的 .part 暂存文件。
 *
 * DESIGN.md §6.4 头(64 B):
 *   0 "UPL1" | 4 u16 hdr_len=64 | 6 u8 enc | 7 u8 flags(bit0=bad) |
 *   8 u64 total | 16 u32 crc32 | 20 u32 hdr_crc(前 20 B) | 24 name[40]
 *   数据从偏移 64 开始。
 *
 * §6.5 续传/重试:
 *   BEGIN:total+crc+name+enc 与现有头全等、!bad、本次 !restart →
 *          resume_off = filesize-64;否则截断重建(从 0)。
 *   END  CRC 失败:flags.bad=1 写回头并 fsync,保留 .part;下一次 BEGIN
 *        看到 bad 从 0 截断(不会死循环)。
 *   ABORT:fsync 后删除。
 *   END 成功:把数据区(跳过 64 B 头)拷进 books/<name>.part,边拷边算
 *        CRC;通过后删旧同名书、rename 发布、再删 .part(不能直接 rename
 *        .part,否则 64 B 头会变成书的首 64 字节)。
 *        这一步在整个 FS 锁内完成,期间其它 storage API 返回 ST_ERR_BUSY
 *        (2 s 后),见 storage_up_end 内的长注释。
 *
 * 写入一律用绝对偏移:off < written 视为重传(只补尾部),off > written
 * 是洞 → ST_ERR_IO;超出 total → ST_ERR_BAD_PARAM。总大小 ≤32 MB。
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

#define ST_UP_NAME_TMP 96       /* 净化用临时缓冲(输入名最长 255,超长拒绝) */

static FILE *s_part;
static bool s_part_open;
static uint64_t s_total;
static uint32_t s_crc;
static uint8_t s_enc;
static char s_name[ST_UP_NAME_BYTES];
static uint64_t s_written;      /* 已写入的连续数据字节数(不含 64 B 头) */
static uint64_t s_unsynced;     /* 距上次 f_sync 的字节数 */

/* storage_up_written() 可能被另一个任务读(END 排空屏障),64 位读需加锁 */
static portMUX_TYPE s_stat_mux = portMUX_INITIALIZER_UNLOCKED;

static void up_part_path(char *out, size_t n)
{
    (void)snprintf(out, n, "%s/upload.part", READER_DIR_TMP);
}

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

/* ------------------------------------------------------------------ */
/* 头                                                                 */
/* ------------------------------------------------------------------ */

static void up_hdr_encode(uint8_t h[ST_UP_HEADER_BYTES], uint8_t enc, uint8_t flags,
                          uint64_t total, uint32_t crc, const char *name)
{
    memset(h, 0, ST_UP_HEADER_BYTES);
    memcpy(h, ST_UP_MAGIC, 4);
    put_u16(h + 4, ST_UP_HEADER_BYTES);
    h[6] = enc;
    h[7] = flags;
    put_u64(h + 8, total);
    put_u32(h + 16, crc);
    size_t nl = strlen(name);
    if (nl > ST_UP_NAME_BYTES - 1) {
        nl = ST_UP_NAME_BYTES - 1;
    }
    memcpy(h + 24, name, nl);           /* 其余字节由 memset 补 NUL */
    put_u32(h + 20, rc_crc32_ieee(h, 20));
}

typedef struct {
    uint8_t enc;
    uint8_t flags;
    uint64_t total;
    uint32_t crc;
    char name[ST_UP_NAME_BYTES];
} up_hdr_t;

static bool up_hdr_decode(const uint8_t h[ST_UP_HEADER_BYTES], up_hdr_t *out)
{
    if (memcmp(h, ST_UP_MAGIC, 4) != 0 || get_u16(h + 4) != ST_UP_HEADER_BYTES) {
        return false;
    }
    if (get_u32(h + 20) != rc_crc32_ieee(h, 20)) {
        return false;
    }
    if (memchr(h + 24, 0, ST_UP_NAME_BYTES) == NULL) {
        return false;                   /* name 必须 NUL 结尾 */
    }
    memset(out, 0, sizeof(*out));
    out->enc = h[6];
    out->flags = h[7];
    out->total = get_u64(h + 8);
    out->crc = get_u32(h + 16);
    memcpy(out->name, h + 24, ST_UP_NAME_BYTES);
    out->name[ST_UP_NAME_BYTES - 1] = '\0';
    return true;
}

/* ------------------------------------------------------------------ */
/* 名字净化(§6.1:净化基名 ≤35 B + ".txt" = 40 B)                     */
/* ------------------------------------------------------------------ */

static void utf8_truncate(char *s, size_t max_bytes)
{
    size_t l = strlen(s);
    if (l <= max_bytes) {
        return;
    }
    size_t i = max_bytes;
    while (i > 0 && ((uint8_t)s[i] & 0xC0) == 0x80) {
        i--;                            /* 退到多字节字符的首字节 */
    }
    s[i] = '\0';
}

static int up_sanitize_name(const char *in, char out[ST_UP_NAME_BYTES])
{
    if (!in || !in[0]) {
        return -1;
    }
    if (strlen(in) >= ST_UP_NAME_TMP) {
        return -1;                      /* 超长输入直接拒绝 */
    }

    char tmp[ST_UP_NAME_TMP];
    if (rc_sanitize_filename(in, tmp, sizeof(tmp)) <= 0) {
        return -1;
    }

    /* 去掉已有的 .txt 后缀(大小写不敏感),避免 "a.txt" → "a.txt.txt" */
    size_t l = strlen(tmp);
    if (l > 4) {
        const char *e = tmp + l - 4;
        if (e[0] == '.' && (e[1] == 't' || e[1] == 'T') && (e[2] == 'x' || e[2] == 'X') &&
            (e[3] == 't' || e[3] == 'T')) {
            tmp[l - 4] = '\0';
        }
    }

    /* 基名 ≤ READER_UPLOAD_NAME_MAX-4 = 35 B,截断落在 UTF-8 边界 */
    utf8_truncate(tmp, READER_UPLOAD_NAME_MAX - 4);
    /* 截断后可能的尾随空格/点再去掉 */
    l = strlen(tmp);
    while (l > 0 && (tmp[l - 1] == ' ' || tmp[l - 1] == '.')) {
        tmp[--l] = '\0';
    }
    if (l == 0) {
        return -1;
    }

    int w = snprintf(out, ST_UP_NAME_BYTES, "%s.txt", tmp);
    if (w <= 0 || (size_t)w >= ST_UP_NAME_BYTES || (size_t)w > READER_UPLOAD_NAME_MAX) {
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* 会话状态                                                           */
/* ------------------------------------------------------------------ */

static void up_set_written(uint64_t v)
{
    portENTER_CRITICAL(&s_stat_mux);
    s_written = v;
    portEXIT_CRITICAL(&s_stat_mux);
}

static void up_close_locked(bool sync)
{
    if (s_part) {
        if (sync) {
            (void)fflush(s_part);
            (void)fsync(fileno(s_part));
        }
        fclose(s_part);
        s_part = NULL;
    }
    s_part_open = false;
    s_unsynced = 0;
}

void st_up_force_close(void)
{
    /* 调用方持有 FS 锁(storage_remount)。.part 保留,下次 BEGIN 续传。 */
    bool had = (s_part != NULL) || s_part_open;
    up_close_locked(true);
    if (had) {
        ESP_LOGW(TAG, "upload session closed for remount (part kept for resume)");
    }
}

static int up_io_fail(const char *what)
{
    up_close_locked(false);
    st_sd_fail(what);
    return ST_ERR_IO;
}

/* 写失败:卡满不是卡故障 —— 返回 ST_ERR_NO_SPACE 并保留会话;
 * 其余错误按 §6.6 关句柄并标记 SD 失效。 */
static int up_write_fail(const char *what)
{
    if (errno == ENOSPC) {
        /* 清掉 FILE* 的错误标志,腾出空间后同一会话还能重试 */
        if (s_part) {
            clearerr(s_part);
        }
        st_sd_set_error("card full");
        return ST_ERR_NO_SPACE;
    }
    return up_io_fail(what);
}

/* ------------------------------------------------------------------ */
/* 公共 API                                                           */
/* ------------------------------------------------------------------ */

int storage_up_begin(const char *name, uint64_t total, uint32_t crc32v, uint8_t enc,
                     uint8_t flags, uint64_t *resume_off)
{
    if (resume_off) {
        *resume_off = 0;
    }
    if (!name || !name[0]) {
        return ST_ERR_BAD_PARAM;
    }
    if (total > READER_UPLOAD_MAX_BYTES) {
        return ST_ERR_SIZE;             /* 上限 32 MB */
    }

    char clean[ST_UP_NAME_BYTES];
    if (up_sanitize_name(name, clean) != 0) {
        return ST_ERR_NAME;
    }

    bool want_restart = (flags & ST_UP_FLAG_RESTART) != 0;
    enc = (uint8_t)(enc & 0x3);

    int rc = st_fs_take(ST_LOCK_MS);
    if (rc != ST_OK) {
        return rc;
    }

    up_close_locked(true);              /* 旧会话(若有)先落盘关闭 */

    char path[ST_PATH_MAX];
    up_part_path(path, sizeof(path));

    uint64_t resume = 0;
    bool reuse = false;

    /* 1) 读现有头,判断能否续传(§6.5 "头全等且 !bad 且 !restart") */
    FILE *f = fopen(path, "rb");
    if (f) {
        uint8_t h[ST_UP_HEADER_BYTES];
        up_hdr_t old;
        if (fread(h, 1, sizeof(h), f) == sizeof(h) && up_hdr_decode(h, &old)) {
            if (old.total == total && old.crc == crc32v && old.enc == enc &&
                strcmp(old.name, clean) == 0 && (old.flags & ST_UP_FLAG_BAD) == 0 &&
                !want_restart) {
                if (fseek(f, 0, SEEK_END) == 0) {
                    long sz = ftell(f);
                    if (sz >= (long)ST_UP_HEADER_BYTES &&
                        (uint64_t)sz - ST_UP_HEADER_BYTES <= total) {
                        resume = (uint64_t)sz - ST_UP_HEADER_BYTES;
                        reuse = true;
                    }
                }
            }
        }
        fclose(f);
    } else if (errno != ENOENT) {
        rc = up_io_fail("fopen(.part rb) failed");
        st_unlock();
        return rc;
    }

    if (reuse) {
        f = fopen(path, "r+b");
        if (!f) {
            rc = up_io_fail("fopen(.part r+b) failed");
            st_unlock();
            return rc;
        }
        /* 报给主机的 resume_off 必须是真正落盘的字节:先 f_sync */
        if (fflush(f) != 0 || fsync(fileno(f)) != 0) {
            fclose(f);
            rc = up_io_fail("fsync(.part resume) failed");
            st_unlock();
            return rc;
        }
        if (fseek(f, (long)(ST_UP_HEADER_BYTES + resume), SEEK_SET) != 0) {
            fclose(f);
            rc = up_io_fail("fseek(.part resume) failed");
            st_unlock();
            return rc;
        }
        ESP_LOGI(TAG, "upload resume %s at %llu/%llu B", clean, (unsigned long long)resume,
                 (unsigned long long)total);
    } else {
        /* 从头重建:空头 + 新头,立即 fsync */
        f = fopen(path, "w+b");
        if (!f) {
            rc = up_io_fail("fopen(.part wb) failed");
            st_unlock();
            return rc;
        }
        uint8_t h[ST_UP_HEADER_BYTES];
        up_hdr_encode(h, enc, 0, total, crc32v, clean);
        bool ok = fwrite(h, 1, sizeof(h), f) == sizeof(h) && fflush(f) == 0 &&
                  fsync(fileno(f)) == 0;
        if (!ok || fseek(f, (long)ST_UP_HEADER_BYTES, SEEK_SET) != 0) {
            fclose(f);
            rc = up_io_fail("write(.part header) failed");
            st_unlock();
            return rc;
        }
        resume = 0;
        ESP_LOGI(TAG, "upload begin %s (%llu B)", clean, (unsigned long long)total);
    }

    s_part = f;
    s_part_open = true;
    s_total = total;
    s_crc = crc32v;
    s_enc = enc;
    memcpy(s_name, clean, sizeof(s_name));
    up_set_written(resume);
    s_unsynced = 0;

    if (resume_off) {
        *resume_off = resume;
    }
    st_unlock();
    return ST_OK;
}

int storage_up_write(uint64_t off, const uint8_t *data, size_t len)
{
    if (len > 0 && !data) {
        return ST_ERR_BAD_PARAM;
    }

    int rc = st_fs_take(ST_LOCK_MS);
    if (rc != ST_OK) {
        return rc;
    }
    if (!s_part || !s_part_open) {
        st_unlock();
        return ST_ERR_NOT_FOUND;        /* 没有 BEGIN 会话 */
    }
    if (len == 0) {
        st_unlock();
        return ST_OK;
    }
    if (off > s_total || len > s_total - off) {
        st_unlock();
        return ST_ERR_BAD_PARAM;        /* 越界 */
    }

    uint64_t written = storage_up_written();
    if (off > written) {
        /* 洞:不补零,让调用方从 storage_up_written() 重发(§9.4) */
        st_unlock();
        return ST_ERR_IO;
    }
    if (off + len <= written) {
        st_unlock();
        return ST_OK;                   /* 纯重传,数据已在 */
    }

    size_t skip = (size_t)(written - off);
    const uint8_t *p = data + skip;
    size_t n = len - skip;
    long abs_off = (long)(ST_UP_HEADER_BYTES + off + skip);

    if (fseek(s_part, abs_off, SEEK_SET) != 0) {
        st_unlock();
        return up_io_fail("fseek(.part) failed");
    }
    errno = 0;
    if (fwrite(p, 1, n, s_part) != n) {
        st_unlock();
        return up_write_fail("fwrite(.part) failed");
    }
    errno = 0;
    if (fflush(s_part) != 0) {
        st_unlock();
        return up_write_fail("fflush(.part) failed");
    }
    up_set_written(written + n);
    s_unsynced += n;
    if (s_unsynced >= ST_UP_SYNC_BYTES) {
        errno = 0;
        if (fsync(fileno(s_part)) != 0) {
            st_unlock();
            return up_write_fail("fsync(.part) failed");
        }
        s_unsynced = 0;
    }
    st_unlock();
    return ST_OK;
}

uint64_t storage_up_written(void)
{
    portENTER_CRITICAL(&s_stat_mux);
    uint64_t v = s_written;
    portEXIT_CRITICAL(&s_stat_mux);
    return v;
}

int storage_up_end(uint32_t crc32v, char *final_path, size_t n)
{
    if (final_path && n > 0) {
        final_path[0] = '\0';
    }

    int rc = st_fs_take(ST_LOCK_MS);
    if (rc != ST_OK) {
        return rc;
    }
    if (!s_part || !s_part_open) {
        st_unlock();
        return ST_ERR_NOT_FOUND;
    }
    if (storage_up_written() != s_total) {
        /* 数据还没收齐;保留会话,主机可继续发(END 屏障在 ble_upload) */
        st_unlock();
        return ST_ERR_SIZE;
    }

    char path[ST_PATH_MAX];
    up_part_path(path, sizeof(path));

    /* 落盘,然后把数据区(跳过 64 B 头)拷进 books/<name>.part 并同步算 CRC。
     * 不能直接 rename .part:那会把 64 B 头也带进书里(S2 确认:这一步的
     * 全量写换的是"发布前 CRC 复验 + 书文件头 0 字节",掉电恢复路径
     * 完全不变 —— tmp 的 .part 还在,BOOKS 里只有写完整的 <name>.part)。
     *
     * 整个拷贝在 FS 锁内完成(≤32 MB,可达数十秒)。放锁分块做不到,是因为
     * 锁一放,storage_remount 可能直接卸载挂载点,而这里还握着 .part 和
     * 新书的 FILE* —— 悬空句柄比 UI 卡顿严重得多。
     * 代价(S3):期间 app 的 storage_pos_save/storage_read_at/storage_list
     * 和其它 storage API 会等 ST_LOCK_MS(2 s)后返回 ST_ERR_BUSY;
     * 上传完成是低频路径,BLE 侧已把 BUSY 当"可重试",UI 侧由
     * storage_close_book 的有限等待保证不再冻死。 */
    if (fflush(s_part) != 0 || fsync(fileno(s_part)) != 0) {
        st_unlock();
        return up_io_fail("fsync(.part end) failed");
    }
    if (fseek(s_part, (long)ST_UP_HEADER_BYTES, SEEK_SET) != 0) {
        st_unlock();
        return up_io_fail("fseek(.part verify) failed");
    }

    char dst[ST_PATH_MAX];
    char tmp[ST_PATH_MAX];
    int w = snprintf(dst, sizeof(dst), "%s/%s", READER_DIR_BOOKS, s_name);
    int w2 = snprintf(tmp, sizeof(tmp), "%s/%s.part", READER_DIR_BOOKS, s_name);
    if (w <= 0 || (size_t)w >= sizeof(dst) || w2 <= 0 || (size_t)w2 >= sizeof(tmp)) {
        st_sd_set_error("upload final path too long");
        st_unlock();
        return ST_ERR_NAME;
    }

    errno = 0;
    FILE *out = fopen(tmp, "wb");
    if (!out) {
        st_unlock();
        return up_write_fail("fopen(upload temp) failed");
    }

    static uint8_t s_copy_buf[4096];
    uint32_t crc = 0;
    uint64_t left = s_total;
    int chunks = 0;
    bool ok = true;
    bool full = false;
    while (left > 0) {
        size_t want = (left > sizeof(s_copy_buf)) ? sizeof(s_copy_buf) : (size_t)left;
        size_t got = fread(s_copy_buf, 1, want, s_part);
        if (got != want) {
            ok = false;
            break;
        }
        errno = 0;
        if (fwrite(s_copy_buf, 1, got, out) != got) {
            ok = false;
            full = (errno == ENOSPC);
            break;
        }
        crc = rc_crc32_update(crc, s_copy_buf, got);
        left -= got;
        /* 每 256 KB 让出一次 CPU:拷贝是纯阻塞 I/O 之外的紧循环,
         * 别让 IDLE 任务整段饿着(TWDT 只看不 panic,但日志很难看)。 */
        if (++chunks >= 64) {
            chunks = 0;
            vTaskDelay(1);
        }
    }
    if (ok) {
        errno = 0;
        if (fflush(out) != 0 || fsync(fileno(out)) != 0) {
            ok = false;
            full = (errno == ENOSPC);
        }
    }
    if (fclose(out) != 0) {
        ok = false;
    }

    if (!ok) {
        (void)unlink(tmp);
        if (full) {
            /* 卡满不是卡故障:保留上传会话,提示 NO_SPACE */
            st_sd_set_error("card full");
            st_unlock();
            return ST_ERR_NO_SPACE;
        }
        st_unlock();
        return up_io_fail("upload copy failed");
    }

    if (crc != crc32v || crc != s_crc) {
        /* §6.5:置 bad 写回头并 fsync,保留 .part;下次 BEGIN 见 bad 从 0 重建 */
        ESP_LOGE(TAG, "upload CRC mismatch: got %08" PRIx32 ", want %08" PRIx32 "/%08" PRIx32,
                 crc, crc32v, s_crc);
        (void)unlink(tmp);
        uint8_t h[ST_UP_HEADER_BYTES];
        up_hdr_encode(h, s_enc, ST_UP_FLAG_BAD, s_total, s_crc, s_name);
        bool bad_ok = fseek(s_part, 0, SEEK_SET) == 0 &&
                      fwrite(h, 1, sizeof(h), s_part) == sizeof(h) && fflush(s_part) == 0 &&
                      fsync(fileno(s_part)) == 0;
        up_close_locked(false);
        if (!bad_ok) {
            rc = up_io_fail("write(.part bad flag) failed");
        } else {
            st_sd_set_error("upload crc mismatch");
            rc = ST_ERR_CRC;
        }
        st_unlock();
        return rc;
    }

    up_close_locked(true);              /* .part 已消费,先关闭 */

    /* FatFs 的 f_rename 在目标存在时返回 FR_EXIST(§6.3 注释),先删旧书。
     * 这只会发生在"同名书重新上传"的场景,窗口内旧书短暂消失。 */
    if (unlink(dst) != 0 && errno != ENOENT) {
        ESP_LOGW(TAG, "unlink old %s failed (errno=%d)", dst, errno);
        (void)unlink(tmp);
        st_unlock();
        return ST_ERR_IO;
    }
    if (rename(tmp, dst) != 0) {
        ESP_LOGE(TAG, "rename(%s -> %s) failed (errno=%d)", tmp, dst, errno);
        (void)unlink(tmp);
        st_sd_fail("upload rename failed");
        st_unlock();
        return ST_ERR_IO;
    }
    /* .part 已完成使命;删除失败只告警(下次 BEGIN 会按文件大小续传/重建) */
    if (unlink(path) != 0 && errno != ENOENT) {
        ESP_LOGW(TAG, "unlink(%s) after publish failed (errno=%d)", path, errno);
    }

    if (final_path && n > 0) {
        (void)snprintf(final_path, n, "%s", dst);
    }
    ESP_LOGI(TAG, "upload complete: %s (%llu B)", dst, (unsigned long long)s_total);
    st_unlock();
    return ST_OK;
}

int storage_up_abort(void)
{
    int rc = st_fs_take(ST_LOCK_MS);
    if (rc != ST_OK) {
        return rc;
    }

    up_close_locked(true);              /* fsync 后再删(§6.5) */

    char path[ST_PATH_MAX];
    up_part_path(path, sizeof(path));
    if (unlink(path) != 0 && errno != ENOENT) {
        ESP_LOGW(TAG, "unlink(%s) failed (errno=%d)", path, errno);
        rc = ST_ERR_IO;
    } else {
        rc = ST_OK;
    }
    up_set_written(0);
    st_unlock();
    return rc;
}
