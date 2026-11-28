#pragma once
/* reader_cfg.h — frozen public configuration (DESIGN.md §17.2)
 *
 * Every tunable that can be overridden with -D is guarded by #ifndef.
 * Work packages must not add macros here; put package-private constants in
 * their own headers.
 */

#define READER_VERSION_MAJOR 1
#define READER_VERSION_MINOR 2
#define READER_VERSION_PATCH 2

/* Logical frame: 250x122, 1bpp, row stride 32 B, MSB left, 1=white 0=black
 * (DESIGN §4.1; same convention as badapple/main/playback.c:167). */
#define READER_FB_W 250
#define READER_FB_H 122
#define READER_FB_ROW_BYTES 32
#define READER_FB_BYTES 3904

/* Text grid (DESIGN §5.1). 5 lines x 18 px fits 122 px; 6 would need 130. */
#define READER_TEXT_MARGIN_X 2
#define READER_TEXT_MAX_W 246
#define READER_STATUS_H 19
#define READER_CONTENT_TOP 22
#define READER_LINE_H 18
#define READER_LINES_PER_PAGE 5
#define READER_CHAR_W_CJK 16
#define READER_CHAR_W_ASCII 8
/* 8x16 的字形基线在字格第 12 行（墨迹 2..11），HZK16 的基线是第 15 行；
 * +3 让两套字形的**基线**对齐（数字墨迹落到 5..14，汉字 0..15）。+4 会把
 * 基线推到 16，数字整体比汉字低 1 px。 */
#define READER_ASCII_DY 3
#define READER_TAB_SPACES 4

/* Line/page buffers (DESIGN §5.4/§5.6). */
#define READER_CHARS_PER_LINE 32
#define READER_LINE_MAX_BYTES 128
#define READER_PAGE_WINDOW 1024
#define READER_PAGE_AHEAD 128
#define READER_PAGE_CANDIDATES 64

/* Ghosting control (DESIGN §4.4). */
#ifndef READER_GC_PARTIALS
#define READER_GC_PARTIALS 16
#endif
#ifndef READER_GC_IDLE_MS
#define READER_GC_IDLE_MS 45000
#endif
#define READER_GC_MIN_GAP_MS 5000
#define READER_FULL_ON_BOOK_OPEN 1

/* Keys (DESIGN §7.2). */
#define READER_KEY_DEBOUNCE_MS 60
#ifndef READER_KEY_LONG_MS
#define READER_KEY_LONG_MS 800
#endif
#define READER_KEY_REPEAT_MS 300
#define READER_KEY_QUEUE_LEN 32

/* Power (DESIGN §10). */
#define READER_BATTERY_PERIOD_MS 60000
#define READER_ADC_SETTLE_MS 120
#ifndef READER_ADC_OVERRIDE_MV
#define READER_ADC_OVERRIDE_MV 0
#endif
#define READER_LOW_WARN_PCT 5
#define READER_LOW_WARN_EXIT_PCT 7
#define READER_LOW_CRIT_PCT 2
#define READER_LOW_CRIT_EXIT_PCT 5
#define READER_ADV_IDLE_MS 120000
#define READER_ENABLE_DEEP_SLEEP 0

#ifndef READER_DEBUG_ASSERTS
#define READER_DEBUG_ASSERTS 1
#endif

/* Storage layout (DESIGN §6.1). Only these two directories are ever written. */
#define READER_DIR_BOOKS  "/sdcard/books"
#define READER_DIR_STATE  "/sdcard/reader/state"
#define READER_DIR_TMP    "/sdcard/reader/tmp"
#define READER_UPLOAD_MAX_BYTES (32ull * 1024 * 1024)
#define READER_UPLOAD_NAME_MAX 39
#define READER_LIB_NAME_MAX 60

/* BLE ring and credit (DESIGN §9.4). */
#define READER_BLE_RING_BYTES 16384
#define READER_BLE_CREDIT_BYTES 8192

#define READER_MAX_BOOKMARKS 8
