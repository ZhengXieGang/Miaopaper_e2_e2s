#pragma once
/* sim_store.h — in-memory stand-in for the SD-backed store.
 *
 * The simulator never touches /sdcard and never links mp_store.c: it only
 * needs the *view* of the library that mp_app would produce (book rows,
 * settings rows, about lines, progress values) plus a 4000 B lock image for
 * the two LOCK fixtures. Everything here is fixture data, not storage code.
 *
 * Titles that are GB2312 are stored as raw GB2312 bytes (the firmware keeps
 * them that way: the host uploader transcodes, PORT_SPEC §4.7).
 */
#include <stddef.h>
#include <stdint.h>

#include "mp_ui.h"

#define SIM_STORE_BOOKS 3          /* done books in the fake catalog (of 8) */
#define SIM_LOCK_BYTES 4000        /* epd_temp layout: idx=(y>>3)*250+x, bit=y&7 */

typedef struct {
    const char *title;   /* raw GB2312 or ASCII bytes, NUL terminated */
    uint8_t enc;         /* MP_STR_GB2312 / MP_STR_ASCII */
    uint32_t len;        /* declared book length (not used by the UI) */
} sim_book_t;

/* Fake catalog: 三体 (GB2312), Moby Dick (ASCII), 红楼梦 (GB2312). */
const sim_book_t *sim_store_books(size_t *count);

/* SELECT window: 3 books + the synthetic 返回 item = 4 logical rows.
 * rows[] receives the visible window starting at logical row `scroll`; the
 * returned value is the logical row count (4). */
uint8_t sim_store_fill_rows(mp_ui_row_t rows[MP_LIST_ROWS], uint8_t scroll);

/* SETTINGS window: E2S 7 rows, E2 6 rows (无背光), 4-row window. */
#define SIM_SETTINGS_ROWS 7
#define SIM_SETTINGS_E2_ROWS 6
uint8_t sim_settings_fill_rows(mp_ui_row_t rows[MP_LIST_ROWS], uint8_t scroll);
uint8_t sim_settings_e2_fill_rows(mp_ui_row_t rows[MP_LIST_ROWS], uint8_t scroll);

/* ABOUT: one page, 4 lines. `page` is ignored (kept for the old call sites). */
uint8_t sim_about_lines(uint8_t page, mp_str_t out[MP_LIST_ROWS]);
uint8_t sim_about_e2_lines(mp_str_t out[MP_LIST_ROWS]);

/* 4000 B lock image in the epd_temp layout (bit 1 = black, 128 rows).
 * Static storage: the caller must not free it (mp_ui_state_t keeps a pointer). */
const uint8_t *sim_store_lock_img(void);

/* READ fixtures. Each returns 5 NUL-terminated lines copied into out[][].
 * read_cn: 红楼梦 GB2312 text (24-26 B per line).
 * read_en: ASCII prose (valid Dialog_plain_16 input). */
uint8_t sim_read_cn(char out[MP_READ_LINES][MP_LINE_BYTES]);
uint8_t sim_read_en(char out[MP_READ_LINES][MP_LINE_BYTES]);

/* Contiguous book bytes for the real mp_page_measure() path.
 * which: 0 = 红楼梦 GB2312 (lines joined by '\n'), 1 = Moby Dick ASCII
 * (words joined by ' '). The blob is static and NUL-free; len excludes a
 * trailing NUL. */
typedef struct {
    const uint8_t *data;
    uint32_t len;
} sim_book_blob_t;

const sim_book_blob_t *sim_read_blob(uint8_t which);
int sim_book_read(void *ctx, uint32_t off, void *buf, size_t len);

/* Progress fixture, 0..100. */
uint8_t sim_store_percent(uint8_t book_idx);
