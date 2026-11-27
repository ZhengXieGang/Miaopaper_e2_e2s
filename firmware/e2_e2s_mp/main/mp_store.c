/* mp_store.c — SD-backed Miaopaper catalog/book/font/lock/progress storage.
 *
 * The on-SD catalog keeps Miaopaper's 8-entry / 32-byte layout so the
 * behaviour (and the BLE 0x15 replies) match the reference. Book payloads are
 * one file per slot instead of flash offsets; nothing outside /sdcard/mp is
 * touched.
 */
#include "mp_store.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "mp_toc.h"
#include "text.h"

static const char *TAG = "mp_store";

#ifndef MP_DIR
#define MP_DIR "/sdcard/mp"
#endif
#define MP_CATALOG MP_DIR "/catalog.bin"
#define MP_CATALOG_TMP MP_DIR "/catalog.tmp"
#define MP_CATALOG_BAK MP_DIR "/catalog.bak"
#define MP_CATALOG_BAD MP_DIR "/catalog.bad"
#define MP_FONT MP_DIR "/font.bin"
#define MP_LOCK MP_DIR "/lock.bin"
#define MP_LOCK_TMP MP_DIR "/lock.tmp"
#define MP_LOCK_BAK MP_DIR "/lock.bak"
#define MP_PROGRESS MP_DIR "/progress.bin"
#define MP_SETTINGS MP_DIR "/settings.bin"
#define MP_STATS MP_DIR "/stats.bin"
#define MP_CLOCK MP_DIR "/clock.bin"
#define MP_CLOCK_TMP MP_DIR "/clock.tmp"
#define MP_CLOCK_BAK MP_DIR "/clock.bak"
#define MP_BOOK_FMT MP_DIR "/b%u.bin"

#define CATALOG_HDR 8
#define LOCK_IMAGE_BYTES 4000
#define LOCK_FILE_BYTES 4096      /* 4000 B 图 + 标志 @0x0FF0 */
#define LOCK_FLAG_OFF 0x0FF0
#define LOCK_FLAG_VALUE 0xAA
#define FONT_BYTES 261696u
#define SETTINGS_MAGIC0 'M'
#define SETTINGS_MAGIC1 'P'
#define SETTINGS_MAGIC2 'S'
#define SETTINGS_MAGIC3 '1'
#define STATS_MAGIC0 'M'
#define STATS_MAGIC1 'P'
#define STATS_MAGIC2 'R'
#define STATS_MAGIC3 '1'
#define CLOCK_MAGIC0 'M'
#define CLOCK_MAGIC1 'P'
#define CLOCK_MAGIC2 'C'
#define CLOCK_MAGIC3 '1'

const uint16_t mp_sleep_timeout_s[MP_SLEEP_TIMEOUT_COUNT] = {60, 120, 180, 300};
const uint16_t mp_gc_interval[MP_GC_INTERVAL_COUNT] = {10, 20, 50, 100};
mp_settings_t mp_settings;
mp_stats_t mp_stats;

static SemaphoreHandle_t s_lock;
static uint8_t s_hdr[CATALOG_HDR];
static uint8_t (*s_entries)[MP_ENTRY_SIZE];
static uint16_t s_slot_cap;
static bool s_ready;
static char s_err[72] = "not initialised";
static int s_wfd = -1;
static uint16_t s_wslot = MP_SLOT_INVALID;
/* 0x10 声明的总长与已写高水位：write 用来拒绝越界/空洞，finish 用来核对
 * 文件真实大小（PORT_SPEC §6.6-1）。 */
static uint32_t s_wdeclared;
static uint32_t s_wwritten;

static void set_err(const char *msg)
{
    snprintf(s_err, sizeof(s_err), "%s", msg ? msg : "");
}

static int take(void)
{
    if (!s_lock) {
        return -1;
    }
    return xSemaphoreTakeRecursive(s_lock, portMAX_DELAY) == pdTRUE ? 0 : -1;
}

static void give(void)
{
    if (s_lock) {
        (void)xSemaphoreGiveRecursive(s_lock);
    }
}

static int write_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n <= 0) {
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int read_all(int fd, void *buf, size_t len)
{
    uint8_t *p = (uint8_t *)buf;
    while (len > 0) {
        ssize_t n = read(fd, p, len);
        if (n <= 0) {
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static void book_path(uint16_t idx, char *out, size_t cap)
{
    snprintf(out, cap, MP_BOOK_FMT, (unsigned)idx);
}

static uint16_t align_slot_cap(uint32_t need)
{
    if (need < MP_SLOT_CHUNK) {
        return MP_SLOT_CHUNK;
    }
    uint32_t aligned = ((need + MP_SLOT_CHUNK - 1u) / MP_SLOT_CHUNK) * MP_SLOT_CHUNK;
    if (aligned > MP_MAX_SLOTS) {
        aligned = MP_MAX_SLOTS;
    }
    return (uint16_t)aligned;
}

/* 确保内存目录容量至少达到 min_cap，新增条目统一初始化为 MP_FLAG_EMPTY(0xFF)。 */
static int ensure_slot_cap_locked(uint16_t min_cap)
{
    if (min_cap > MP_MAX_SLOTS) {
        return -1;
    }
    if (s_entries && s_slot_cap >= min_cap) {
        return 0;
    }
    uint16_t new_cap = align_slot_cap(min_cap);
    uint8_t (*grown)[MP_ENTRY_SIZE] =
        (uint8_t (*)[MP_ENTRY_SIZE])realloc(s_entries, (size_t)new_cap * MP_ENTRY_SIZE);
    if (!grown) {
        set_err("catalog oom");
        return -1;
    }
    if (new_cap > s_slot_cap) {
        memset(grown + s_slot_cap, 0xFF, (size_t)(new_cap - s_slot_cap) * MP_ENTRY_SIZE);
    }
    s_entries = grown;
    s_slot_cap = new_cap;
    return 0;
}

static int reset_empty_catalog_locked(void)
{
    uint16_t cap = MP_SLOT_CHUNK;
    uint8_t (*fresh)[MP_ENTRY_SIZE] =
        (uint8_t (*)[MP_ENTRY_SIZE])malloc((size_t)cap * MP_ENTRY_SIZE);
    if (!fresh) {
        set_err("catalog oom");
        return -1;
    }
    memset(fresh, 0xFF, (size_t)cap * MP_ENTRY_SIZE);
    free(s_entries);
    s_entries = fresh;
    s_slot_cap = cap;
    memset(s_hdr, 0, sizeof(s_hdr));
    s_hdr[0] = 'B';
    s_hdr[1] = 'O';
    s_hdr[2] = 'O';
    s_hdr[3] = 'K';
    s_hdr[4] = 0x01;
    return 0;
}

static void entry_title(const uint8_t *e, char *out)
{
    size_t n = 0;
    for (; n < MP_TITLE_MAX; n++) {
        char c = (char)e[1 + n];
        if (!c) {
            break;
        }
        out[n] = c;
    }
    out[n] = '\0';
}

static uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void wr_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void build_entry(uint8_t *e, uint8_t flags, const char *title, uint32_t len,
                        uint8_t enc)
{
    memset(e, 0, MP_ENTRY_SIZE);
    e[0] = flags;
    if (title) {
        size_t n = strlen(title);
        if (n > MP_TITLE_MAX) {
            n = MP_TITLE_MAX;
        }
        memcpy(&e[1], title, n);
    }
    wr_le32(&e[0x15], 0); /* book data lives in b<idx>.bin; start is unused */
    wr_le32(&e[0x19], len);
    e[0x1D] = enc;
}

/* 写头 + 动态条目表 + fsync + close，零大栈缓冲，任一步失败都返回 -1。 */
static int write_catalog_tmp_sync(void)
{
    if (!s_entries || s_slot_cap == 0) {
        return -1;
    }
    int fd = open(MP_CATALOG_TMP, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        return -1;
    }
    int rc = write_all(fd, s_hdr, CATALOG_HDR);
    if (rc == 0) {
        rc = write_all(fd, s_entries, (size_t)s_slot_cap * MP_ENTRY_SIZE);
    }
    if (rc == 0 && fsync(fd) != 0) {
        rc = -1;
    }
    if (close(fd) != 0) {
        rc = -1;
    }
    return rc;
}

/* 目录项同步（尽力而为）。FatFs 的 f_sync/f_rename 本身会写目录项；VFS 上
 * open() 一个目录不一定成功，失败不影响本函数的正确性。 */
static void sync_dir(void)
{
    int fd = open(MP_DIR, O_RDONLY);
    if (fd >= 0) {
        (void)fsync(fd);
        close(fd);
    }
}

/* catalog 原子替换（PORT_SPEC §6.6-3）：
 *   1) 写 catalog.tmp + fsync；
 *   2) 旧 catalog.bin 改名 catalog.bak（保留上一版）；
 *   3) rename(catalog.tmp, catalog.bin)。
 * 任一步掉电，最坏留下 tmp + bak；load_catalog_locked() 会从它们恢复。 */
static int flush_catalog_locked(void)
{
    if (write_catalog_tmp_sync() != 0) {
        set_err("catalog tmp write failed");
        return -1;
    }

    (void)unlink(MP_CATALOG_BAK);
    struct stat st;
    if (stat(MP_CATALOG, &st) == 0) {
        if (rename(MP_CATALOG, MP_CATALOG_BAK) != 0) {
            set_err("catalog bak rename failed");
            (void)unlink(MP_CATALOG_TMP);
            return -1;
        }
    }
    if (rename(MP_CATALOG_TMP, MP_CATALOG) != 0) {
        set_err("catalog rename failed");
        (void)rename(MP_CATALOG_BAK, MP_CATALOG);   /* 尽力恢复上一版 */
        return -1;
    }
    sync_dir();
    return 0;
}

static void set_font_flag(uint8_t v)
{
    s_hdr[5] = v;
}

/* 读取动态长度 catalog 文件：支持旧版 8 槽(264 B)以及任意扩容后的 N 槽目录。 */
static int read_catalog_file(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < (off_t)(CATALOG_HDR + MP_ENTRY_SIZE) ||
        ((st.st_size - CATALOG_HDR) % MP_ENTRY_SIZE) != 0) {
        close(fd);
        return -1;
    }
    uint64_t slots = (uint64_t)(st.st_size - CATALOG_HDR) / MP_ENTRY_SIZE;
    if (slots == 0 || slots > MP_MAX_SLOTS) {
        close(fd);
        return -1;
    }

    uint8_t hdr[CATALOG_HDR];
    if (read_all(fd, hdr, sizeof(hdr)) != 0 || hdr[0] != 'B' || hdr[1] != 'O' ||
        hdr[2] != 'O' || hdr[3] != 'K' || hdr[4] != 0x01) {
        close(fd);
        return -1;
    }

    uint16_t cap = (uint16_t)slots;
    uint8_t (*tmp)[MP_ENTRY_SIZE] =
        (uint8_t (*)[MP_ENTRY_SIZE])malloc((size_t)cap * MP_ENTRY_SIZE);
    if (!tmp) {
        close(fd);
        set_err("catalog oom");
        return -1;
    }
    int rc = read_all(fd, tmp, (size_t)cap * MP_ENTRY_SIZE);
    close(fd);
    if (rc != 0) {
        free(tmp);
        return -1;
    }

    memcpy(s_hdr, hdr, CATALOG_HDR);
    free(s_entries);
    s_entries = tmp;
    s_slot_cap = cap;
    return 0;
}

/* 返回 0 = 正常（含首次上电、以及从 .tmp/.bak 恢复）；1 = catalog.bin
 * 存在但无效，已改名 catalog.bad —— 目录内容不再可信，init 必须把错误
 * 报给 app（PORT_SPEC §6.6-2）；-1 = 连重建都写不下去。 */
static int load_catalog_locked(void)
{
    struct stat st;
    if (stat(MP_CATALOG, &st) == 0) {
        if (read_catalog_file(MP_CATALOG) == 0) {
            return 0;
        }
        /* 存在但无效：改名保留现场，绝不 memset 覆盖（PORT_SPEC §6.6-2）。 */
        (void)unlink(MP_CATALOG_BAD);
        if (rename(MP_CATALOG, MP_CATALOG_BAD) != 0) {
            ESP_LOGE(TAG, "catalog corrupt; rename to .bad failed");
            set_err("catalog corrupt; rename to .bad failed");
        } else {
            ESP_LOGW(TAG, "catalog corrupt -> catalog.bad");
            set_err("catalog corrupt (kept as catalog.bad)");
        }

        /* 掉电可能停在 rename 序列中间：catalog.tmp 是本次新内容，
         * catalog.bak 是上一版，两者都比“清空重建”好。 */
        if (read_catalog_file(MP_CATALOG_TMP) == 0 ||
            read_catalog_file(MP_CATALOG_BAK) == 0) {
            if (flush_catalog_locked() == 0) {
                ESP_LOGW(TAG, "catalog recovered from .tmp/.bak");
                return 1;
            }
            set_err("catalog recovery failed");
            return -1;
        }

        if (reset_empty_catalog_locked() != 0 || flush_catalog_locked() != 0) {
            return -1;
        }
        ESP_LOGW(TAG, "catalog rebuilt: empty");
        return 1;
    }

    /* 文件不存在：首次上电 / 新卡，或掉电停在 rename 序列中间。 */
    if (read_catalog_file(MP_CATALOG_TMP) == 0 ||
        read_catalog_file(MP_CATALOG_BAK) == 0) {
        if (flush_catalog_locked() != 0) {
            set_err("catalog recovery failed");
            return -1;
        }
        ESP_LOGW(TAG, "catalog recovered from .tmp/.bak");
        return 0;
    }

    if (reset_empty_catalog_locked() != 0 || flush_catalog_locked() != 0) {
        return -1;
    }
    return 0;
}

int mp_store_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateRecursiveMutex();
        if (!s_lock) {
            set_err("mutex alloc failed");
            return -1;
        }
    }
    if (mkdir(MP_DIR, 0777) != 0 && errno != EEXIST) {
        set_err("mkdir /sdcard/mp failed");
        return -1;
    }
    if (take() != 0) {
        return -1;
    }
    int rc = load_catalog_locked();
    if (rc < 0) {
        give();
        return -1;
    }
    /* The embedded HZK16 is byte-identical to Miaopaper's HZK16_87.bin. */
    if (text_hz16_glyph(0xB0, 0xA1) != NULL) {
        set_font_flag(1);
    }
    s_ready = true;
    bool damaged = (rc != 0);
    if (!damaged) {
        set_err("ok");
    }
    give();
    if (!damaged) {
        (void)mp_settings_load();
        (void)mp_stats_load();
        ESP_LOGI(TAG, "store ready: %u books (cap %u)",
                 (unsigned)mp_store_count_done(),
                 (unsigned)mp_store_slot_capacity());
    } else {
        ESP_LOGE(TAG, "store damaged: %s (storage error reported to app)",
                 mp_store_error());
    }
    return damaged ? -1 : 0;
}

const char *mp_store_error(void)
{
    return s_err;
}

uint16_t mp_store_slot_capacity(void)
{
    uint16_t cap = 0;
    if (take() != 0) {
        return 0;
    }
    cap = s_slot_cap;
    give();
    return cap;
}

uint16_t mp_store_count_done(void)
{
    uint16_t n = 0;
    if (take() != 0) {
        return 0;
    }
    for (uint16_t i = 0; i < s_slot_cap; i++) {
        if (s_entries[i][0] == MP_FLAG_DONE) {
            n++;
        }
    }
    give();
    return n;
}

uint16_t mp_store_list_done(uint16_t *slots, uint16_t max_slots)
{
    uint16_t n = 0;
    if (take() != 0) {
        return 0;
    }
    for (uint16_t i = 0; i < s_slot_cap; i++) {
        if (s_entries[i][0] == MP_FLAG_DONE) {
            if (slots && n < max_slots) {
                slots[n] = i;
            }
            n++;
        }
    }
    give();
    return n;
}

bool mp_store_is_done(uint16_t idx)
{
    if (take() != 0) {
        return false;
    }
    bool ok = (idx < s_slot_cap) && (s_entries[idx][0] == MP_FLAG_DONE);
    give();
    return ok;
}

int mp_store_read(uint16_t idx, mp_book_t *out)
{
    if (!out || take() != 0) {
        return -1;
    }
    if (idx >= s_slot_cap) {
        give();
        return -1;
    }
    const uint8_t *e = s_entries[idx];
    out->flags = e[0];
    entry_title(e, out->title);
    out->len = rd_le32(&e[0x19]);
    out->enc = e[0x1D] == MP_ENC_GB2312 ? MP_ENC_GB2312 : MP_ENC_ASCII;
    out->chars_k = (uint16_t)((uint16_t)e[0x1E] | ((uint16_t)e[0x1F] << 8));
    give();
    return out->flags == MP_FLAG_DONE ? 0 : -1;
}

int32_t mp_store_find_free_slot(void)
{
    int32_t slot = -1;
    if (take() != 0) {
        return -1;
    }
    for (uint16_t i = 0; i < s_slot_cap; i++) {
        if (s_entries[i][0] != MP_FLAG_DONE && i != s_wslot) {
            slot = (int32_t)i;
            break;
        }
    }
    /* 现有槽已满时自动按 8 槽步进动态扩容，不再限制 8 本上限。 */
    if (slot < 0 && s_slot_cap < MP_MAX_SLOTS) {
        uint16_t next_slot = s_slot_cap;
        if (ensure_slot_cap_locked((uint16_t)(next_slot + 1u)) == 0) {
            slot = (int32_t)next_slot;
        }
    }
    give();
    return slot;
}

int mp_store_set_flag(uint16_t idx, uint8_t flags)
{
    if (idx >= MP_MAX_SLOTS || take() != 0) {
        return -1;
    }
    if (idx >= s_slot_cap && ensure_slot_cap_locked((uint16_t)(idx + 1u)) != 0) {
        give();
        return -1;
    }
    s_entries[idx][0] = flags;
    int rc = flush_catalog_locked();
    give();
    return rc;
}

int mp_store_begin(uint16_t idx, const char *title, uint32_t len, uint8_t enc)
{
    if (idx >= MP_MAX_SLOTS || take() != 0) {
        return -1;
    }
    if (idx >= s_slot_cap && ensure_slot_cap_locked((uint16_t)(idx + 1u)) != 0) {
        give();
        return -1;
    }
    if (s_wfd >= 0) {
        close(s_wfd);
        s_wfd = -1;
    }
    build_entry(s_entries[idx], MP_FLAG_UPLOADING, title, len,
                enc == MP_ENC_GB2312 ? MP_ENC_GB2312 : MP_ENC_ASCII);
    int rc = flush_catalog_locked();
    if (rc == 0) {
        char path[64];
        book_path(idx, path, sizeof(path));
        (void)unlink(path);
        mp_toc_delete(idx);
        s_wfd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0666);
        if (s_wfd < 0) {
            set_err("book open failed");
            s_entries[idx][0] = MP_FLAG_EMPTY;
            (void)flush_catalog_locked();
            rc = -1;
        } else {
            s_wslot = idx;
            s_wdeclared = len;
            s_wwritten = 0;
        }
    }
    give();
    return rc;
}

int mp_store_write(uint16_t idx, uint32_t off, const void *buf, size_t len)
{
    if (!buf || idx >= MP_MAX_SLOTS || take() != 0) {
        return -1;
    }
    int rc = -1;
    if (s_wfd >= 0 && s_wslot == idx) {
        if (off > s_wdeclared || len > (size_t)(s_wdeclared - off)) {
            set_err("book write beyond declared length");   /* 含 32 位溢出 */
        } else if (off != s_wwritten) {
            set_err("book write gap/overlap");              /* 空洞：主机丢了包 */
        } else if (lseek(s_wfd, (off_t)off, SEEK_SET) < 0) {
            set_err("book write seek failed");
        } else if (write_all(s_wfd, buf, len) != 0) {
            set_err("book write failed");
        } else {
            s_wwritten = off + (uint32_t)len;
            rc = 0;
        }
    }
    give();
    return rc;
}

int mp_store_set_chars(uint16_t idx, uint32_t chars)
{
    if (idx >= MP_MAX_SLOTS || take() != 0) {
        return -1;
    }
    int rc = -1;
    if (s_wfd >= 0 && s_wslot == idx && idx < s_slot_cap) {
        uint32_t k = (chars + 1023u) / 1024u;
        if (k > 0xFFFFu) {
            k = 0xFFFFu;
        }
        if (chars > 0 && k == 0) {
            k = 1;
        }
        s_entries[idx][0x1E] = (uint8_t)(k & 0xFFu);
        s_entries[idx][0x1F] = (uint8_t)((k >> 8) & 0xFFu);
        rc = flush_catalog_locked();
    }
    give();
    return rc;
}

int mp_store_finish(uint16_t idx)
{
    if (idx >= MP_MAX_SLOTS || take() != 0) {
        return -1;
    }
    int rc = -1;
    if (s_wfd >= 0 && s_wslot == idx && idx < s_slot_cap) {
        struct stat st;
        bool ok = false;
        if (s_wwritten != s_wdeclared) {
            set_err("book finish: written != declared");
        } else if (fstat(s_wfd, &st) != 0) {
            set_err("book finish: fstat failed");
        } else if (st.st_size != (off_t)s_wdeclared) {
            set_err("book finish: file size != declared");
        } else if (fsync(s_wfd) != 0) {
            set_err("book finish: fsync failed");
        } else {
            ok = true;
        }
        if (close(s_wfd) != 0) {
            ok = false;
            set_err("book finish: close failed");
        }
        s_wfd = -1;
        s_wslot = MP_SLOT_INVALID;
        s_wdeclared = 0;
        s_wwritten = 0;
        if (ok) {
            s_entries[idx][0] = MP_FLAG_DONE;
            if (flush_catalog_locked() == 0) {
                rc = 0;
            } else {
                /* 目录没落盘：绝不能把槽留在 DONE。 */
                memset(s_entries[idx], 0, MP_ENTRY_SIZE);
                s_entries[idx][0] = MP_FLAG_EMPTY;
                (void)flush_catalog_locked();
            }
        } else {
            /* 长度不符/写失败：按 abort 处理，不留半本书（§6.6-1）。 */
            char path[64];
            book_path(idx, path, sizeof(path));
            (void)unlink(path);
            memset(s_entries[idx], 0, MP_ENTRY_SIZE);
            s_entries[idx][0] = MP_FLAG_EMPTY;
            (void)flush_catalog_locked();
            ESP_LOGW(TAG, "book finish rejected: %s", mp_store_error());
        }
    }
    give();
    return rc;
}

void mp_store_abort(uint16_t idx)
{
    if (idx >= MP_MAX_SLOTS || take() != 0) {
        return;
    }
    if (s_wfd >= 0 && s_wslot == idx) {
        close(s_wfd);
        s_wfd = -1;
        s_wslot = MP_SLOT_INVALID;
        s_wdeclared = 0;
        s_wwritten = 0;
        char path[64];
        book_path(idx, path, sizeof(path));
        (void)unlink(path);
        mp_toc_delete(idx);
        if (idx < s_slot_cap) {
            s_entries[idx][0] = MP_FLAG_EMPTY;
            (void)flush_catalog_locked();
        }
    }
    give();
}

int mp_store_delete(uint16_t idx)
{
    if (idx >= MP_MAX_SLOTS || take() != 0) {
        return -1;
    }
    if (idx >= s_slot_cap) {
        give();
        return -1;
    }
    if (s_wfd >= 0 && s_wslot == idx) {
        close(s_wfd);
        s_wfd = -1;
        s_wslot = MP_SLOT_INVALID;
        s_wdeclared = 0;
        s_wwritten = 0;
    }
    char path[64];
    book_path(idx, path, sizeof(path));
    (void)unlink(path);
    mp_toc_delete(idx);
    memset(s_entries[idx], 0, MP_ENTRY_SIZE);
    s_entries[idx][0] = MP_FLAG_DELETED;
    int rc = flush_catalog_locked();
    give();
    (void)mp_progress_save(idx, 0);
    return rc;
}

int mp_store_compact(void)
{
    if (take() != 0) {
        return -1;
    }
    uint32_t active_end = 0;
    for (uint16_t i = 0; i < s_slot_cap; i++) {
        uint8_t f = s_entries[i][0];
        if (f != MP_FLAG_DONE && f != MP_FLAG_EMPTY) {
            memset(s_entries[i], 0, MP_ENTRY_SIZE);
            s_entries[i][0] = MP_FLAG_EMPTY;
        }
        if (s_entries[i][0] == MP_FLAG_DONE) {
            active_end = (uint32_t)i + 1u;
        }
    }
    /* 收缩尾部连续空槽至最小 8 的倍数，保持目录文件精简。 */
    uint16_t target_cap = align_slot_cap(active_end);
    if (target_cap < s_slot_cap) {
        uint8_t (*shrunk)[MP_ENTRY_SIZE] =
            (uint8_t (*)[MP_ENTRY_SIZE])realloc(s_entries, (size_t)target_cap * MP_ENTRY_SIZE);
        if (shrunk) {
            s_entries = shrunk;
        }
        s_slot_cap = target_cap;
    }
    int rc = flush_catalog_locked();
    give();
    return rc;
}

int mp_store_read_book(uint16_t idx, uint32_t off, void *buf, size_t len)
{
    if (!buf || idx >= MP_MAX_SLOTS || take() != 0) {
        return -1;
    }
    if (idx >= s_slot_cap) {
        give();
        return -1;
    }
    char path[64];
    book_path(idx, path, sizeof(path));
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        set_err("book read open failed");
        give();
        return -1;
    }
    int rc = -1;
    if (lseek(fd, (off_t)off, SEEK_SET) >= 0) {
        rc = read_all(fd, buf, len) == 0 ? 0 : -1;
    }
    close(fd);
    give();
    return rc;
}

uint32_t mp_store_book_size(uint16_t idx)
{
    mp_book_t b;
    struct stat st;
    char path[64];

    if (mp_store_read(idx, &b) != 0) {
        return 0;
    }
    book_path(idx, path, sizeof(path));
    if (stat(path, &st) != 0 || st.st_size < 16) {
        return 0;                    /* 坏书：文件缺失或短到不可能有内容 */
    }
    uint32_t real = (uint32_t)st.st_size;
    /* 目录里的 len 是上传时的声明值；外部截断/老数据会让分页读到 EOF，
     * 所以一律以文件真实大小为上限（PORT_SPEC §6.6-11）。 */
    return real < b.len ? real : b.len;
}

/* ---- lock image ---- */

/* 0x31 允许乱序/覆盖（PORT_SPEC §7.3），所以用 4000 bit 的覆盖位图判断是否
 * 每个字节都收到过，而不是高水位。上传写 lock.tmp；只有覆盖完整、fsync 完成、
 * 0xAA 标志写好后才 rename 成 lock.bin，旧图在此之前一直有效。 */
static uint8_t s_lock_cov[LOCK_IMAGE_BYTES / 8];
static bool s_lock_open;

static void lock_cov_reset(void)
{
    memset(s_lock_cov, 0, sizeof(s_lock_cov));
}

static void lock_cov_mark(uint32_t off, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        uint32_t b = off + (uint32_t)i;
        s_lock_cov[b >> 3] |= (uint8_t)(1u << (b & 7));
    }
}

static bool lock_cov_complete(void)
{
    for (size_t i = 0; i < sizeof(s_lock_cov); i++) {
        if (s_lock_cov[i] != 0xFF) {
            return false;
        }
    }
    return true;
}

bool mp_store_lock_valid(void)
{
    uint8_t v = 0;
    bool ok = false;
    if (take() != 0) {
        return false;
    }
    int fd = open(MP_LOCK, O_RDONLY);
    if (fd >= 0) {
        ok = lseek(fd, LOCK_FLAG_OFF, SEEK_SET) >= 0 &&
             read_all(fd, &v, 1) == 0 && v == LOCK_FLAG_VALUE;
        close(fd);
    }
    give();
    return ok;
}

int mp_store_lock_begin(void)
{
    if (take() != 0) {
        return -1;
    }
    (void)unlink(MP_LOCK_TMP);
    int fd = open(MP_LOCK_TMP, O_RDWR | O_CREAT | O_TRUNC, 0666);
    int rc = -1;
    if (fd < 0) {
        set_err("lock tmp open failed");
    } else if (ftruncate(fd, LOCK_FILE_BYTES) != 0) {
        set_err("lock tmp truncate failed");
        close(fd);
        (void)unlink(MP_LOCK_TMP);
    } else if (close(fd) != 0) {
        set_err("lock tmp close failed");
        (void)unlink(MP_LOCK_TMP);
    } else {
        lock_cov_reset();
        s_lock_open = true;
        rc = 0;
    }
    give();
    return rc;
}

int mp_store_lock_write(uint32_t off, const void *buf, size_t len)
{
    if (!buf || len > LOCK_IMAGE_BYTES ||
        off > (uint32_t)LOCK_IMAGE_BYTES - (uint32_t)len) {
        return -1;
    }
    if (take() != 0) {
        return -1;
    }
    int rc = -1;
    if (!s_lock_open) {
        set_err("lock write without begin");
    } else {
        int fd = open(MP_LOCK_TMP, O_RDWR);
        if (fd < 0) {
            set_err("lock tmp open failed");
        } else {
            if (lseek(fd, (off_t)off, SEEK_SET) >= 0 &&
                write_all(fd, buf, len) == 0) {
                lock_cov_mark(off, len);
                rc = 0;
            } else {
                set_err("lock write failed");
            }
            close(fd);
        }
    }
    give();
    return rc;
}

int mp_store_lock_finish(void)
{
    int fd = -1;
    int rc = -1;
    if (take() != 0) {
        return -1;
    }
    if (!s_lock_open) {
        set_err("lock finish without begin");
        goto done;
    }
    if (!lock_cov_complete()) {
        set_err("lock incomplete");                /* 0x32 回 0xFF，旧图保留 */
        goto done;
    }
    fd = open(MP_LOCK_TMP, O_RDWR);
    if (fd < 0) {
        set_err("lock tmp open failed");
        goto done;
    }
    if (fsync(fd) != 0) {
        set_err("lock fsync failed");
        goto done;
    }
    uint8_t flag = LOCK_FLAG_VALUE;
    if (lseek(fd, LOCK_FLAG_OFF, SEEK_SET) < 0 ||
        write_all(fd, &flag, 1) != 0 || fsync(fd) != 0) {
        set_err("lock flag write failed");
        goto done;
    }
    if (close(fd) != 0) {
        fd = -1;
        set_err("lock close failed");
        goto done;
    }
    fd = -1;

    (void)unlink(MP_LOCK_BAK);
    struct stat st;
    if (stat(MP_LOCK, &st) == 0 && rename(MP_LOCK, MP_LOCK_BAK) != 0) {
        set_err("lock bak rename failed");
        goto done;
    }
    if (rename(MP_LOCK_TMP, MP_LOCK) != 0) {
        set_err("lock rename failed");
        (void)rename(MP_LOCK_BAK, MP_LOCK);        /* 旧图放回 */
        goto done;
    }
    sync_dir();
    s_lock_open = false;
    rc = 0;

done:
    if (fd >= 0) {
        close(fd);
    }
    if (rc != 0) {
        (void)unlink(MP_LOCK_TMP);
        s_lock_open = false;
    }
    give();
    return rc;
}

int mp_store_lock_read(uint8_t *buf, size_t len)
{
    if (!buf || len < LOCK_IMAGE_BYTES || take() != 0) {
        return -1;
    }
    int fd = open(MP_LOCK, O_RDONLY);
    int rc = -1;
    if (fd >= 0) {
        rc = read_all(fd, buf, LOCK_IMAGE_BYTES) == 0 ? 0 : -1;
        close(fd);
    }
    give();
    return rc;
}

/* ---- uploaded font ---- */

bool mp_store_font_ok(void)
{
    bool embedded = text_hz16_glyph(0xB0, 0xA1) != NULL;
    struct stat st;
    bool uploaded;
    if (take() != 0) {
        return embedded;
    }
    uploaded = stat(MP_FONT, &st) == 0 && st.st_size == (off_t)FONT_BYTES;
    give();
    return embedded || uploaded;
}

int mp_store_font_begin(void)
{
    if (take() != 0) {
        return -1;
    }
    (void)unlink(MP_FONT);
    int fd = open(MP_FONT, O_RDWR | O_CREAT, 0666);
    int rc = -1;
    if (fd >= 0) {
        if (close(fd) == 0) {
            rc = 0;
        }
    }
    give();
    return rc;
}

int mp_store_font_write(uint32_t off, const void *buf, size_t len)
{
    if (!buf || len > FONT_BYTES || off > (uint32_t)FONT_BYTES - (uint32_t)len) {
        return -1;
    }
    if (take() != 0) {
        return -1;
    }
    int fd = open(MP_FONT, O_RDWR);
    int rc = -1;
    if (fd >= 0) {
        if (lseek(fd, (off_t)off, SEEK_SET) >= 0) {
            rc = write_all(fd, buf, len) == 0 ? 0 : -1;
        }
        close(fd);
    }
    give();
    return rc;
}

int mp_store_font_finish(void)
{
    struct stat st;
    int fd = -1;
    int rc = -1;
    if (take() != 0) {
        return -1;
    }
    fd = open(MP_FONT, O_RDWR);
    if (fd < 0) {
        set_err("font open failed");
    } else if (fstat(fd, &st) != 0 || st.st_size != (off_t)FONT_BYTES) {
        set_err("font size mismatch");
    } else if (fsync(fd) != 0) {
        set_err("font fsync failed");               /* 数据落盘后才改标志 */
    } else {
        uint8_t old = s_hdr[5];
        set_font_flag(1);
        rc = flush_catalog_locked();
        if (rc != 0) {
            s_hdr[5] = old;
        }
    }
    if (fd >= 0) {
        close(fd);
    }
    give();
    return rc;
}

/* ---- progress ---- */

static uint8_t prog_xor(const uint8_t *r)
{
    uint8_t x = 0;
    for (int i = 0; i < 7; i++) {
        x ^= r[i];
    }
    return x;
}

int mp_progress_save(uint16_t idx, uint32_t pos)
{
    if (idx >= MP_MAX_SLOTS || take() != 0) {
        return -1;
    }
    int fd = open(MP_PROGRESS, O_RDWR | O_CREAT, 0666);
    int rc = -1;
    if (fd < 0) {
        set_err("progress open failed");
    } else {
        uint8_t r[8];
        memset(r, 0, sizeof(r));
        r[0] = 0x5A;
        r[1] = (uint8_t)(idx & 0xFFu);
        wr_le32(&r[2], pos);
        r[6] = (uint8_t)((idx >> 8) & 0xFFu);
        r[7] = prog_xor(r);
        if (lseek(fd, (off_t)idx * 8, SEEK_SET) >= 0 &&
            write_all(fd, r, sizeof(r)) == 0) {
            (void)fsync(fd);
            rc = 0;
        }
        close(fd);
    }
    give();
    return rc;
}

bool mp_progress_load(uint16_t idx, uint32_t *pos)
{
    if (idx >= MP_MAX_SLOTS || !pos || take() != 0) {
        return false;
    }
    int fd = open(MP_PROGRESS, O_RDONLY);
    bool ok = false;
    if (fd >= 0) {
        uint8_t r[8];
        ok = lseek(fd, (off_t)idx * 8, SEEK_SET) >= 0 &&
             read_all(fd, r, sizeof(r)) == 0 && r[0] == 0x5A &&
             r[1] == (uint8_t)(idx & 0xFFu) &&
             r[6] == (uint8_t)((idx >> 8) & 0xFFu) &&
             r[7] == prog_xor(r);
        close(fd);
        if (ok) {
            *pos = rd_le32(&r[2]);
        }
    }
    give();
    return ok;
}

/* ---- settings ---- */

static uint8_t settings_crc(const uint8_t *p, size_t n)
{
    uint8_t x = 0;
    for (size_t i = 0; i < n; i++) {
        x ^= p[i];
    }
    return x;
}

/* PORT_SPEC §10.4: 原 static，必须公开 —— main.c 在 mp_store_init() 之前
 * 无条件调用（§6.7），否则设置零初始化会让 BLE 被静默关闭。
 * 只写 RAM，不落盘（此时 SD 可能还没挂载）。 */
void mp_settings_defaults(void)
{
    mp_settings.ble_enabled = 1;
    mp_settings.sleep_timeout_idx = 2;      /* 3 分钟 (180 s) */
    mp_settings.epd_partial_enabled = 0;    /* scene change = full (Miaopaper) */
    mp_settings.gc_interval_idx = 0;        /* GC every 10 partials */
    mp_settings.lock_display = MP_LOCK_DISP_STATS;
    mp_settings.backlight_level = MP_BACKLIGHT_DEFAULT;
}

int mp_settings_load(void)
{
    uint8_t buf[16];
    bool ok = false;
    if (take() != 0) {
        return -1;
    }
    int fd = open(MP_SETTINGS, O_RDONLY);
    if (fd >= 0) {
        ok = read_all(fd, buf, sizeof(buf)) == 0 && buf[0] == SETTINGS_MAGIC0 &&
             buf[1] == SETTINGS_MAGIC1 && buf[2] == SETTINGS_MAGIC2 &&
             buf[3] == SETTINGS_MAGIC3 && buf[15] == settings_crc(buf, 15);
        close(fd);
    }
    if (ok) {
        mp_settings.ble_enabled = buf[4] ? 1 : 0;
        mp_settings.sleep_timeout_idx = buf[5] % MP_SLEEP_TIMEOUT_COUNT;
        mp_settings.epd_partial_enabled = buf[6] ? 1 : 0;
        mp_settings.gc_interval_idx = buf[7] % MP_GC_INTERVAL_COUNT;
        mp_settings.lock_display =
            (buf[8] == MP_LOCK_DISP_IMAGE) ? MP_LOCK_DISP_IMAGE : MP_LOCK_DISP_STATS;
        mp_settings.backlight_level = buf[9] % MP_BACKLIGHT_LEVEL_COUNT;
    }
    give();
    if (ok) {
        return 0;
    }
    mp_settings_defaults();
    return mp_settings_save();
}

int mp_settings_save(void)
{
    uint8_t buf[16];
    if (take() != 0) {
        return -1;
    }
    memset(buf, 0, sizeof(buf));
    buf[0] = SETTINGS_MAGIC0;
    buf[1] = SETTINGS_MAGIC1;
    buf[2] = SETTINGS_MAGIC2;
    buf[3] = SETTINGS_MAGIC3;
    buf[4] = mp_settings.ble_enabled;
    buf[5] = mp_settings.sleep_timeout_idx;
    buf[6] = mp_settings.epd_partial_enabled;
    buf[7] = mp_settings.gc_interval_idx;
    buf[8] = mp_settings.lock_display;
    buf[9] = mp_settings.backlight_level % MP_BACKLIGHT_LEVEL_COUNT;
    buf[15] = settings_crc(buf, 15);

    (void)unlink(MP_SETTINGS);
    int fd = open(MP_SETTINGS, O_WRONLY | O_CREAT, 0666);
    int rc = -1;
    if (fd >= 0) {
        if (write_all(fd, buf, sizeof(buf)) == 0) {
            (void)fsync(fd);
            rc = 0;
        }
        close(fd);
    }
    give();
    return rc;
}

/* ---- stats ---- */

void mp_stats_defaults(void)
{
    mp_stats.read_secs = 0;
    mp_stats.read_chars = 0;
}

int mp_stats_load(void)
{
    uint8_t buf[16];
    bool ok = false;
    if (take() != 0) {
        return -1;
    }
    int fd = open(MP_STATS, O_RDONLY);
    if (fd >= 0) {
        ok = read_all(fd, buf, sizeof(buf)) == 0 && buf[0] == STATS_MAGIC0 &&
             buf[1] == STATS_MAGIC1 && buf[2] == STATS_MAGIC2 &&
             buf[3] == STATS_MAGIC3 && buf[15] == settings_crc(buf, 15);
        close(fd);
    }
    if (ok) {
        mp_stats.read_secs = rd_le32(&buf[4]);
        mp_stats.read_chars = rd_le32(&buf[8]);
    }
    give();
    if (ok) {
        return 0;
    }
    mp_stats_defaults();
    return mp_stats_save();
}

int mp_stats_save(void)
{
    uint8_t buf[16];
    if (take() != 0) {
        return -1;
    }
    memset(buf, 0, sizeof(buf));
    buf[0] = STATS_MAGIC0;
    buf[1] = STATS_MAGIC1;
    buf[2] = STATS_MAGIC2;
    buf[3] = STATS_MAGIC3;
    wr_le32(&buf[4], mp_stats.read_secs);
    wr_le32(&buf[8], mp_stats.read_chars);
    buf[15] = settings_crc(buf, 15);

    (void)unlink(MP_STATS);
    int fd = open(MP_STATS, O_WRONLY | O_CREAT, 0666);
    int rc = -1;
    if (fd >= 0) {
        if (write_all(fd, buf, sizeof(buf)) == 0) {
            (void)fsync(fd);
            rc = 0;
        }
        close(fd);
    }
    give();
    return rc;
}

/* ---- clock snapshot ---- */

static bool clock_read_file(const char *path, uint32_t *out)
{
    uint8_t buf[16];
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return false;
    }
    bool ok = read_all(fd, buf, sizeof(buf)) == 0 &&
              buf[0] == CLOCK_MAGIC0 && buf[1] == CLOCK_MAGIC1 &&
              buf[2] == CLOCK_MAGIC2 && buf[3] == CLOCK_MAGIC3 &&
              buf[15] == settings_crc(buf, 15);
    close(fd);
    if (!ok) {
        return false;
    }
    uint32_t ux = rd_le32(&buf[4]);
    uint32_t inv = rd_le32(&buf[8]);
    if ((ux ^ inv) != 0xFFFFFFFFu) {
        return false;
    }
    *out = ux;
    return true;
}

bool mp_clock_load(uint32_t *unix_s)
{
    if (!unix_s || take() != 0) {
        return false;
    }
    uint32_t ux = 0;
    bool ok = clock_read_file(MP_CLOCK, &ux) || clock_read_file(MP_CLOCK_BAK, &ux);
    give();
    if (ok) {
        *unix_s = ux;
    }
    return ok;
}

int mp_clock_save(uint32_t unix_s)
{
    uint8_t buf[16];
    if (take() != 0) {
        return -1;
    }
    memset(buf, 0, sizeof(buf));
    buf[0] = CLOCK_MAGIC0;
    buf[1] = CLOCK_MAGIC1;
    buf[2] = CLOCK_MAGIC2;
    buf[3] = CLOCK_MAGIC3;
    wr_le32(&buf[4], unix_s);
    wr_le32(&buf[8], ~unix_s);
    buf[15] = settings_crc(buf, 15);

    int rc = -1;
    int fd = open(MP_CLOCK_TMP, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0) {
        if (write_all(fd, buf, sizeof(buf)) == 0 && fsync(fd) == 0) {
            rc = 0;
        }
        if (close(fd) != 0) {
            rc = -1;
        }
    }
    if (rc == 0) {
        (void)unlink(MP_CLOCK_BAK);
        struct stat st;
        if (stat(MP_CLOCK, &st) == 0) {
            (void)rename(MP_CLOCK, MP_CLOCK_BAK);
        }
        if (rename(MP_CLOCK_TMP, MP_CLOCK) != 0) {
            (void)rename(MP_CLOCK_BAK, MP_CLOCK);
            (void)unlink(MP_CLOCK_TMP);
            rc = -1;
        } else {
            sync_dir();
        }
    } else {
        (void)unlink(MP_CLOCK_TMP);
    }
    give();
    return rc;
}
