#pragma once
/* mp_store.h — 基于内置 SD 卡的电子书/目录/字库/锁屏/进度存储层。
 *
 * 目录文件 /sdcard/mp/catalog.bin 兼容原版 8 B 头 + N × 32 B 条目布局，
 * 条目数按需动态增长（8 槽步进，旧版 8 槽文件自动无损兼容），书籍数量
 * 不再设人为小上限，仅受设备存储空间限制：
 *
 *   catalog.bin     8 B 头("BOOK", ver=1, font_ok, 0, 0) + N × 32 B 条目
 *   b0.bin..bN.bin  每槽一本电子书正文
 *   font.bin        上传字库（可选，默认使用内置 HZK16）
 *   lock.bin        4000 B 锁屏图 + 0xAA 标志 @0x0FF0
 *   progress.bin    N × 8 B {0x5A, idx_lo, pos[4], idx_hi, xor}
 *   settings.bin    蓝牙/锁屏/局刷/GC/锁屏展示/背光设置
 *   stats.bin       累计阅读统计（累计阅读秒数 / 累计阅读字数）
 *
 * 仅写 /sdcard/mp，不碰 NVS 与内部 Flash。
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MP_SLOT_CHUNK 8u
#define MP_SLOT_INVALID 0xFFFFu
#define MP_MAX_SLOTS 0xFFFEu
#define MP_ENTRY_SIZE 32
#define MP_TITLE_MAX 19

#define MP_FLAG_EMPTY 0xFF
#define MP_FLAG_UPLOADING 0xFE
#define MP_FLAG_DONE 0xFC
#define MP_FLAG_DELETED 0x00

#define MP_ENC_ASCII 0
#define MP_ENC_GB2312 1

#define MP_SLEEP_TIMEOUT_COUNT 4
#define MP_GC_INTERVAL_COUNT 4
#define MP_BACKLIGHT_LEVEL_COUNT 4
#define MP_BACKLIGHT_DEFAULT 0

#define MP_LOCK_DISP_STATS 0  /* 锁屏展示：阅读数据 */
#define MP_LOCK_DISP_IMAGE 1  /* 锁屏展示：锁屏图 */

typedef struct {
    uint8_t flags;
    char title[MP_TITLE_MAX + 1];
    uint32_t len;
    uint8_t enc;
    uint16_t chars_k;   /* 上传时统计的字数 / 1024；0 = 未知（旧目录项） */
} mp_book_t;

typedef struct {
    uint8_t ble_enabled;
    uint8_t sleep_timeout_idx;
    uint8_t epd_partial_enabled;
    uint8_t gc_interval_idx;
    uint8_t lock_display;
    uint8_t backlight_level;   /* 背光亮度 0..3（0=关） */
} mp_settings_t;

typedef struct {
    uint32_t read_secs;    /* 累计阅读时长（秒） */
    uint32_t read_chars;   /* 累计阅读字数（非空白字符数） */
} mp_stats_t;

extern const uint16_t mp_sleep_timeout_s[MP_SLEEP_TIMEOUT_COUNT];
extern const uint16_t mp_gc_interval[MP_GC_INTERVAL_COUNT];
extern mp_settings_t mp_settings;
extern mp_stats_t mp_stats;

/* 挂载完成后调用（幂等）。返回 0 成功。 */
int mp_store_init(void);
const char *mp_store_error(void);

/* 动态目录接口（槽号 0..MP_MAX_SLOTS-1，仅受空间限制）。 */
uint16_t mp_store_slot_capacity(void);
uint16_t mp_store_count_done(void);
uint16_t mp_store_list_done(uint16_t *slots, uint16_t max_slots);
int mp_store_read(uint16_t idx, mp_book_t *out);
bool mp_store_is_done(uint16_t idx);
int32_t mp_store_find_free_slot(void);
int mp_store_begin(uint16_t idx, const char *title, uint32_t len, uint8_t enc);
int mp_store_write(uint16_t idx, uint32_t off, const void *buf, size_t len);
/* 上传完成后、finish 之前记字数（0x1E..0x1F，单位 1024 字）。 */
int mp_store_set_chars(uint16_t idx, uint32_t chars);
int mp_store_finish(uint16_t idx);
void mp_store_abort(uint16_t idx);
int mp_store_delete(uint16_t idx);
int mp_store_set_flag(uint16_t idx, uint8_t flags);
int mp_store_compact(void);

/* 电子书正文读取。 */
int mp_store_read_book(uint16_t idx, uint32_t off, void *buf, size_t len);
uint32_t mp_store_book_size(uint16_t idx);

/* 锁屏图。 */
bool mp_store_lock_valid(void);
int mp_store_lock_begin(void);
int mp_store_lock_write(uint32_t off, const void *buf, size_t len);
int mp_store_lock_finish(void);
int mp_store_lock_read(uint8_t *buf, size_t len);

/* 上传字库（保留 0x16-0x18 / 0x19 兼容）。 */
bool mp_store_font_ok(void);
int mp_store_font_begin(void);
int mp_store_font_write(uint32_t off, const void *buf, size_t len);
int mp_store_font_finish(void);

/* 每本书的阅读进度。 */
int mp_progress_save(uint16_t idx, uint32_t pos);
bool mp_progress_load(uint16_t idx, uint32_t *pos);

/* 设置（默认值只写 RAM，不落盘）。 */
void mp_settings_defaults(void);
int mp_settings_load(void);
int mp_settings_save(void);

/* 阅读统计：只写 /sdcard/mp/stats.bin。 */
void mp_stats_defaults(void);
int mp_stats_load(void);
int mp_stats_save(void);

/* 墙钟时间快照：只写 /sdcard/mp/clock.bin，作为彻底掉电重启后的兜底恢复基准。 */
bool mp_clock_load(uint32_t *unix_s);
int mp_clock_save(uint32_t unix_s);
