#pragma once
/* mp_store.h — SD-backed Miaopaper storage semantics.
 *
 * Miaopaper keeps its catalog, HZK16, reading progress and lock image in the
 * external NOR flash. The E2S has an SD card instead (and this project never
 * writes internal flash), so the same logical objects live under
 * /sdcard/mp/ with the *same* 8-slot / 32-byte catalog format:
 *
 *   catalog.bin   8 B header + 8 x 32 B entries ("BOOK", ver=1, font_ok, 0, 0)
 *   b0.bin..b7.bin  book data, one file per slot
 *   font.bin       uploaded HZK16 (optional; embedded font is used for render)
 *   lock.bin       4000 B lock image + 0xAA flag at offset 0x0FF0
 *   progress.bin   8 x 8 B {magic, idx, pos[4], xor}
 *   settings.bin   BLE/sleep/partial/GC/lock_display settings
 *   stats.bin      累计阅读统计（累计阅读秒数 / 累计阅读字数）
 *
 * Only /sdcard/mp is written. No NVS, no internal flash.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MP_MAX_BOOKS 8
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

/* Mount must already have succeeded (storage_init). Returns 0 on success. */
int mp_store_init(void);
const char *mp_store_error(void);

/* Catalog. */
int mp_store_count_done(void);
int mp_store_list_done(uint8_t slots[MP_MAX_BOOKS]);
int mp_store_read(uint8_t idx, mp_book_t *out);
bool mp_store_is_done(uint8_t idx);
int mp_store_find_free_slot(void);
int mp_store_begin(uint8_t idx, const char *title, uint32_t len, uint8_t enc);
int mp_store_write(uint8_t idx, uint32_t off, const void *buf, size_t len);
/* 上传完成后、finish 之前记字数（0x1E..0x1F，单位 1024 字）。 */
int mp_store_set_chars(uint8_t idx, uint32_t chars);
int mp_store_finish(uint8_t idx);
void mp_store_abort(uint8_t idx);
int mp_store_delete(uint8_t idx);
int mp_store_set_flag(uint8_t idx, uint8_t flags);
int mp_store_compact(void);

/* Book data (read side). */
int mp_store_read_book(uint8_t idx, uint32_t off, void *buf, size_t len);
uint32_t mp_store_book_size(uint8_t idx);

/* Lock image. */
bool mp_store_lock_valid(void);
int mp_store_lock_begin(void);
int mp_store_lock_write(uint32_t off, const void *buf, size_t len);
int mp_store_lock_finish(void);
int mp_store_lock_read(uint8_t *buf, size_t len);

/* Uploaded font (kept for 0x16-0x18 / 0x19 compatibility). */
bool mp_store_font_ok(void);
int mp_store_font_begin(void);
int mp_store_font_write(uint32_t off, const void *buf, size_t len);
int mp_store_font_finish(void);

/* Progress ring (latest position per book). */
int mp_progress_save(uint8_t idx, uint32_t pos);
bool mp_progress_load(uint8_t idx, uint32_t *pos);

/* Settings. */
/* 填默认值（ble=1 / 锁屏 3 分钟 / 锁屏展示阅读数据 / 场景全刷 / GC 10）。
 * 只写 RAM，不落盘。main.c 必须在 mp_store_init() 之前无条件调用一次
 *（PORT_SPEC §6.7）。 */
void mp_settings_defaults(void);
int mp_settings_load(void);
int mp_settings_save(void);

/* 阅读统计：只写 /sdcard/mp/stats.bin。 */
void mp_stats_defaults(void);
int mp_stats_load(void);
int mp_stats_save(void);
