/* sim_store.c — fixture data standing in for /sdcard/mp.
 *
 * Deliberately NOT an implementation of mp_store.c: the UI layer is forbidden
 * to call mp_store_* (PORT_SPEC §10.1), so the simulator only supplies what
 * mp_app would have put into mp_ui_state_t. No file I/O, no ESP headers.
 */
#include "sim_store.h"

#include <string.h>

#include "lock_demo.h"

/* 三体 / 红楼梦 as raw GB2312 (the catalog stores host-transcoded bytes). */
static const char k_title_santi[] = "\xC8\xFD\xCC\xE5";
static const char k_title_hongloumeng[] = "\xBA\xEC\xC2\xA5\xC3\xCE";

static const sim_book_t k_books[SIM_STORE_BOOKS] = {
    { k_title_santi, MP_STR_GB2312, 4096u },
    { "Moby Dick", MP_STR_ASCII, 8192u },
    { k_title_hongloumeng, MP_STR_GB2312, 6144u },
};

/* 7 settings rows without storage item (moved to ABOUT). Values are static. */
static const char *const k_settings_labels[SIM_SETTINGS_ROWS] = {
    "蓝牙", "锁屏", "锁屏展示", "背光", "时间设置", "关于", "恢复原厂固件",
};
static const char *const k_settings_values[SIM_SETTINGS_ROWS] = {
    NULL, "3分钟", "阅读数据", "2档", NULL, NULL, NULL,
};
static const uint8_t k_settings_kind[SIM_SETTINGS_ROWS] = {
    MP_ROW_TOGGLE, MP_ROW_VALUE, MP_ROW_VALUE, MP_ROW_VALUE,
    MP_ROW_ACTION, MP_ROW_ACTION, MP_ROW_ACTION,
};

/* 无背光设备（E2）设置页 6 行：隐藏背光项。 */
static const char *const k_settings_e2_labels[SIM_SETTINGS_E2_ROWS] = {
    "蓝牙", "锁屏", "锁屏展示", "时间设置", "关于", "恢复原厂固件",
};
static const char *const k_settings_e2_values[SIM_SETTINGS_E2_ROWS] = {
    NULL, "3分钟", "阅读数据", NULL, NULL, NULL,
};
static const uint8_t k_settings_e2_kind[SIM_SETTINGS_E2_ROWS] = {
    MP_ROW_TOGGLE, MP_ROW_VALUE, MP_ROW_VALUE, MP_ROW_ACTION,
    MP_ROW_ACTION, MP_ROW_ACTION,
};

/* 关于页单屏四行：版本、构建日期与 CMake PROJECT_VER 同步，第 4 行为存储占用。 */
static const char *const k_about[MP_LIST_ROWS] = {
    "Miaopaper E2S",
    "固件 1.2.0",
    "构建 2026-11-27",
    "存储 12.4/118.5MB",
};

/* 红楼梦, first page: 12 full-width chars per line = 192 px < 246. */
static const char *const k_read_cn[MP_READ_LINES] = {
    "\xC2\xFA\xD6\xBD\xBB\xC4\xCC\xC6\xD1\xD4\xA3\xAC"
    "\xD2\xBB\xB0\xD1\xD0\xC1\xCB\xE1\xC0\xE1\xA1\xA3",
    "\xB6\xBC\xD4\xC6\xD7\xF7\xD5\xDF\xB3\xD5\xA3\xAC"
    "\xCB\xAD\xBD\xE2\xC6\xE4\xD6\xD0\xCE\xB6\xA3\xBF",
    "\xB4\xCB\xBF\xAA\xBE\xED\xB5\xDA\xD2\xBB\xBB\xD8\xD2\xB2"
    "\xA3\xAC\xD7\xF7\xD5\xDF\xD7\xD4\xD4\xC6\xA1\xA3",
    "\xD2\xF2\xD4\xF8\xC0\xFA\xB9\xFD\xD2\xBB\xB7\xAC\xC3\xCE"
    "\xBB\xC3\xD6\xAE\xBA\xF3\xA3\xAC",
    "\xB9\xCA\xBD\xAB\xD5\xE6\xCA\xC2\xD2\xFE\xC8\xA5\xA1\xA3",
};

/* Moby Dick opening, ASCII. Short lines so every one fits 246 px even if a
 * future mp_page fixture feeds them through the real line breaker. */
static const char *const k_read_en[MP_READ_LINES] = {
    "Call me Ishmael.",
    "Some years ago, never",
    "mind how long precisely,",
    "having little or no money",
    "in my purse, I thought",
};

static uint8_t s_lock[SIM_LOCK_BYTES];
static int s_lock_ready;

/* Contiguous books for the real mp_page_measure() path (see sim_read_blob). */
static uint8_t s_book_cn[512];
static uint8_t s_book_en[512];
static sim_book_blob_t s_blob_cn;
static sim_book_blob_t s_blob_en;

static void copy_line(char *dst, const char *src)
{
    size_t n = strlen(src);
    if (n >= MP_LINE_BYTES) {
        n = MP_LINE_BYTES - 1u;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

const sim_book_t *sim_store_books(size_t *count)
{
    if (count) {
        *count = SIM_STORE_BOOKS;
    }
    return k_books;
}

uint8_t sim_store_fill_rows(mp_ui_row_t rows[MP_LIST_ROWS], uint8_t scroll)
{
    const uint8_t logical = SIM_STORE_BOOKS;   /* 列表只有书；返回在底栏 */
    for (uint8_t i = 0; i < MP_LIST_ROWS; i++) {
        memset(&rows[i], 0, sizeof(rows[i]));
        uint8_t idx = (uint8_t)(scroll + i);
        if (idx < SIM_STORE_BOOKS) {
            rows[i].label = k_books[idx].title;
            rows[i].enc = k_books[idx].enc;
        } else {
            rows[i].label = NULL;
            rows[i].enc = MP_STR_UTF8;
        }
    }
    return logical;
}

uint8_t sim_settings_fill_rows(mp_ui_row_t rows[MP_LIST_ROWS], uint8_t scroll)
{
    for (uint8_t i = 0; i < MP_LIST_ROWS; i++) {
        memset(&rows[i], 0, sizeof(rows[i]));
        uint8_t idx = (uint8_t)(scroll + i);
        if (idx < SIM_SETTINGS_ROWS) {
            rows[i].label = k_settings_labels[idx];
            rows[i].enc = MP_STR_UTF8;
            rows[i].kind = k_settings_kind[idx];
            rows[i].value = k_settings_values[idx];
            rows[i].on = (idx == 0) ? 1 : 0;
            rows[i].bar = 0;
        } else {
            rows[i].label = NULL;
            rows[i].enc = MP_STR_UTF8;
        }
    }
    return SIM_SETTINGS_ROWS;
}

uint8_t sim_settings_e2_fill_rows(mp_ui_row_t rows[MP_LIST_ROWS], uint8_t scroll)
{
    for (uint8_t i = 0; i < MP_LIST_ROWS; i++) {
        memset(&rows[i], 0, sizeof(rows[i]));
        uint8_t idx = (uint8_t)(scroll + i);
        if (idx < SIM_SETTINGS_E2_ROWS) {
            rows[i].label = k_settings_e2_labels[idx];
            rows[i].enc = MP_STR_UTF8;
            rows[i].kind = k_settings_e2_kind[idx];
            rows[i].value = k_settings_e2_values[idx];
            rows[i].on = (idx == 0) ? 1 : 0;
            rows[i].bar = 0;
        } else {
            rows[i].label = NULL;
            rows[i].enc = MP_STR_UTF8;
        }
    }
    return SIM_SETTINGS_E2_ROWS;
}

uint8_t sim_about_lines(uint8_t page, mp_str_t out[MP_LIST_ROWS])
{
    (void)page;
    for (uint8_t i = 0; i < MP_LIST_ROWS; i++) {
        out[i].text = k_about[i];
        out[i].enc = MP_STR_UTF8;
    }
    return MP_LIST_ROWS;
}

uint8_t sim_about_e2_lines(mp_str_t out[MP_LIST_ROWS])
{
    for (uint8_t i = 0; i < MP_LIST_ROWS; i++) {
        out[i].text = (i == 0) ? "Miaopaper E2" : k_about[i];
        out[i].enc = MP_STR_UTF8;
    }
    return MP_LIST_ROWS;
}

const uint8_t *sim_store_lock_img(void)
{
    if (!s_lock_ready) {
        _Static_assert(sizeof k_lock_demo == SIM_LOCK_BYTES,
                       "lock demo must be 4000 B");
        memcpy(s_lock, k_lock_demo, sizeof s_lock);
        s_lock_ready = 1;
    }
    return s_lock;
}

uint8_t sim_read_cn(char out[MP_READ_LINES][MP_LINE_BYTES])
{
    for (uint8_t i = 0; i < MP_READ_LINES; i++) {
        copy_line(out[i], k_read_cn[i]);
    }
    return MP_READ_LINES;
}

uint8_t sim_read_en(char out[MP_READ_LINES][MP_LINE_BYTES])
{
    for (uint8_t i = 0; i < MP_READ_LINES; i++) {
        copy_line(out[i], k_read_en[i]);
    }
    return MP_READ_LINES;
}

uint8_t sim_store_percent(uint8_t book_idx)
{
    static const uint8_t pct[SIM_STORE_BOOKS] = { 42u, 7u, 100u };
    return book_idx < SIM_STORE_BOOKS ? pct[book_idx] : 0u;
}

/* Build the two books once. The CN lines already are valid 246 px rows, so
 * page 0 of the real paginator must reproduce them; the EN words are joined
 * with spaces so word-wrap (not just the fixture line breaks) is exercised. */
/* Build one "page" of fixture text, then repeat it 3x so read_cn/read_en are
 * page 0 and read_end is a real last page reached by walking mp_page_measure()
 * (a single-page book would make read_end identical to read_cn). */
static size_t build_rep(uint8_t *dst, const char *const *lines, char sep,
                        uint8_t count)
{
    size_t n = 0;
    for (uint8_t i = 0; i < count; i++) {
        size_t l = strlen(lines[i]);
        memcpy(&dst[n], lines[i], l);
        n += l;
        if (i + 1u < count) {
            dst[n++] = (uint8_t)sep;
        }
    }
    return n;
}

/* Three repetitions; the last one is deliberately short (2 lines) so the
 * last page differs from page 0 and exercises a partial final page. */
static size_t build_book(uint8_t *dst, size_t cap, const char *const *lines,
                         char sep)
{
    size_t n = 0;
    for (int k = 0; k < 3; k++) {
        uint8_t rep[256];
        size_t rl = build_rep(rep, lines, sep, k == 2 ? 2u : MP_READ_LINES);
        if (k && n < cap) {
            dst[n++] = '\n';
        }
        if (n + rl > cap) {
            break;
        }
        memcpy(&dst[n], rep, rl);
        n += rl;
    }
    return n;
}

const sim_book_blob_t *sim_read_blob(uint8_t which)
{
    if (which == 0) {
        if (!s_blob_cn.data) {
            s_blob_cn.len = (uint32_t)build_book(s_book_cn, sizeof(s_book_cn),
                                                 k_read_cn, '\n');
            s_blob_cn.data = s_book_cn;
        }
        return &s_blob_cn;
    }
    if (!s_blob_en.data) {
        s_blob_en.len = (uint32_t)build_book(s_book_en, sizeof(s_book_en),
                                             k_read_en, ' ');
        s_blob_en.data = s_book_en;
    }
    return &s_blob_en;
}

int sim_book_read(void *ctx, uint32_t off, void *buf, size_t len)
{
    const sim_book_blob_t *b = (const sim_book_blob_t *)ctx;
    if (!b || !b->data || !buf || off > b->len || len > (size_t)(b->len - off)) {
        return -1;
    }
    memcpy(buf, b->data + off, len);
    return 0;
}
