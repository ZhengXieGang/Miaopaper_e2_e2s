/* sim_main.c — build every screen's view model, call the real mp_ui_render()
 * and write raw 1bpp frames as PGM (P5, 250x122, 255 = white / 0 = black).
 *
 * Polarity follows the firmware contract (reader_cfg.h:13-18): frame bit 1 is
 * WHITE, PNG/PGM 255; bit 0 is BLACK, 0. Never invert after mp_ui_render().
 *
 * Frames (order also written to <out>/order.txt):
 *   clock select read_cn read_en read_end settings settings_scroll
 *   book_info jump time_set time_done delete about lock_img lock_none
 *   empty error restore restore_ok
 *
 * Exit codes: 0 ok, 1 --check failure, 2 usage/IO error.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "expected.h"
#include "mp_page.h"
#include "mp_ui.h"
#include "rc_common.h"
#include "sim_store.h"
#include "sim_text.h"

#ifndef SIM_UI_KIND
#define SIM_UI_KIND "unknown"
#endif
#ifndef SIM_HINTS_KIND
#define SIM_HINTS_KIND "unknown"
#endif

/* ------------------------------------------------------------------ *
 * Fixtures
 * ------------------------------------------------------------------ */
enum {
    FX_READ = 1u << 0,
    FX_LOCK_IMG = 1u << 1,
    FX_LOCK_STATS = 1u << 2,
};

typedef struct {
    const char *name;
    uint8_t screen;
    uint8_t flags;
    void (*build)(mp_ui_state_t *st);
} fixture_t;

static void base_state(mp_ui_state_t *st, mp_screen_t screen)
{
    memset(st, 0, sizeof *st);
    st->screen = (uint8_t)screen;
    st->hour = 11;
    st->min = 30;
    st->year = 2026;
    st->month = 11;
    st->day = 25;
    st->week = 3;
    st->time_valid = 1;
    st->bt_on = 1;
    st->battery = 85;
    st->battery_valid = 1;
}

static void left_text(mp_ui_state_t *st, const char *text, uint8_t enc)
{
    st->left.text = text;
    st->left.enc = enc;
}

static void msg2(mp_ui_state_t *st, const char *a, const char *b)
{
    st->msg_lines = 2;
    st->msg_line[0].text = a;
    st->msg_line[0].enc = MP_STR_UTF8;
    st->msg_line[1].text = b;
    st->msg_line[1].enc = MP_STR_UTF8;
}

static void fx_clock(mp_ui_state_t *st)
{
    base_state(st, MP_SCREEN_CLOCK);
    st->time_valid = 0;              /* 主页只有大字时钟 */
    left_text(st, "11-25 周三", MP_STR_UTF8);
}

static void fx_select(mp_ui_state_t *st)
{
    base_state(st, MP_SCREEN_SELECT);
    left_text(st, "书库 3/8", MP_STR_UTF8);
    st->row_count = SIM_STORE_BOOKS; /* 列表只有书；退出走底栏返回 */
    st->sel = 1;                     /* ASCII "Moby Dick" selected */
    st->scroll = mp_ui_list_scroll(st->sel, st->row_count);
    sim_store_fill_rows(st->row, st->scroll);
}

static void fx_settings(mp_ui_state_t *st, uint8_t sel)
{
    base_state(st, MP_SCREEN_SETTINGS);
    left_text(st, "设置", MP_STR_UTF8);
    st->row_count = SIM_SETTINGS_ROWS;
    st->sel = sel;
    st->scroll = mp_ui_list_scroll(sel, st->row_count);
    sim_settings_fill_rows(st->row, st->scroll);
}

static void fx_settings_sel0(mp_ui_state_t *st) { fx_settings(st, 0); }
static void fx_settings_sel7(mp_ui_state_t *st) { fx_settings(st, 7); }

static void fx_settings_e2(mp_ui_state_t *st)
{
    base_state(st, MP_SCREEN_SETTINGS);
    left_text(st, "设置", MP_STR_UTF8);
    st->row_count = SIM_SETTINGS_E2_ROWS;
    st->sel = 0;
    st->scroll = mp_ui_list_scroll(st->sel, st->row_count);
    sim_settings_e2_fill_rows(st->row, st->scroll);
}

static void read_common(mp_ui_state_t *st, uint8_t book_idx, uint8_t enc,
                        int last_page);

static void fx_backlight(mp_ui_state_t *st)
{
    base_state(st, MP_SCREEN_BACKLIGHT);
    left_text(st, "背光", MP_STR_UTF8);
    st->bl_level = 2;
}

static void fx_backlight_popup(mp_ui_state_t *st)
{
    read_common(st, 2, MP_ENC_GB2312, 0);
    st->percent = 42;
    st->battery = 42;
    st->bl_level = 2;
    st->bl_popup = 1;
}

static void fx_low_batt(mp_ui_state_t *st)
{
    base_state(st, MP_SCREEN_LOW_BATT);
    st->time_valid = 0;
}

static void fx_settings_off(mp_ui_state_t *st)
{
    fx_settings(st, 0);
    st->bt_on = 0;
    st->row[0].on = 0;
}

static void fx_about(mp_ui_state_t *st)
{
    base_state(st, MP_SCREEN_ABOUT);
    left_text(st, "关于", MP_STR_UTF8);
    st->about_lines = sim_about_lines(0, st->about_line);
}

static void fx_about_e2(mp_ui_state_t *st)
{
    base_state(st, MP_SCREEN_ABOUT);
    left_text(st, "关于", MP_STR_UTF8);
    st->about_lines = sim_about_e2_lines(st->about_line);
}

/* READ fixtures are produced by the real mp_page_measure() over the fixture
 * book bytes, not by hand-written lines. --check then asserts the paginator
 * invariants below; a mismatch means the SIM, not just the renderer, is stale. */
static mp_page_t s_page;
static int s_page_bad;

static void read_common(mp_ui_state_t *st, uint8_t book_idx, uint8_t enc,
                        int last_page)
{
    size_t n = 0;
    const sim_book_t *books = sim_store_books(&n);
    base_state(st, MP_SCREEN_READ);
    if (book_idx < n) {
        left_text(st, books[book_idx].title, books[book_idx].enc);
    }

    const sim_book_blob_t *blob = sim_read_blob(enc == MP_ENC_GB2312 ? 0 : 1);
    mp_book_ref_t book = {
        .read = sim_book_read,
        .ctx = (void *)blob,
        .len = blob->len,
        .enc = enc,
    };

    uint32_t pos = 0;
    if (last_page) {
        for (;;) {
            uint32_t next = mp_page_measure(&book, pos, &s_page);
            if (next >= book.len || next <= pos) {
                break;                  /* s_page = last page, pos = its start */
            }
            pos = next;
        }
    } else {
        (void)mp_page_measure(&book, 0, &s_page);
    }

    if (s_page.lines == 0 || s_page.lines > MP_READ_LINES) {
        s_page_bad = 1;
    }
    for (uint8_t i = 0; i < s_page.lines && i < MP_READ_LINES; i++) {
        if (s_page.line_w[i] > MP_MAX_LINE_W ||
            strlen((const char *)s_page.raw[i]) >= MP_LINE_BYTES) {
            s_page_bad = 1;
        }
        snprintf(st->line[i], sizeof(st->line[i]), "%s",
                 (const char *)s_page.raw[i]);
    }
    st->line_enc = enc;
    st->line_count = s_page.lines;
    st->percent = s_page.at_end ? 100
                                : mp_page_percent(pos, book.len);
}

/* 电量取三个不同档位，便于人工核对电池内的百分比（与阅读进度无关）。 */
static void fx_read_cn(mp_ui_state_t *st)
{
    read_common(st, 2, MP_ENC_GB2312, 0);
    st->percent = 42;
    st->battery = 42;
}
static void fx_read_en(mp_ui_state_t *st)
{
    read_common(st, 1, MP_ENC_ASCII, 0);
    st->percent = 7;
    st->battery = 7;
}
static void fx_read_end(mp_ui_state_t *st)
{
    read_common(st, 2, MP_ENC_GB2312, 1);
    st->battery = 100;
}

static void fx_lock_img(mp_ui_state_t *st)
{
    base_state(st, MP_SCREEN_LOCK);
    st->lock_img = sim_store_lock_img();
    st->lock_valid = 1;
    st->lock_mode = MP_LOCK_MODE_IMAGE;
}

static void fx_lock_stats(mp_ui_state_t *st)
{
    base_state(st, MP_SCREEN_LOCK);
    st->lock_mode = MP_LOCK_MODE_STATS;
    st->read_mins = 128;
    st->read_chars = 36800;
}

static void fx_book_info(mp_ui_state_t *st)
{
    static const char *const actions[4] = {
        "跳转", "重置阅读进度", "返回书库", "删除本书",
    };
    base_state(st, MP_SCREEN_BOOK_INFO);
    left_text(st, "红楼梦", MP_STR_UTF8);
    st->meta.text = "字数 1.2万\t进度 42%\t大小 1.2MB";
    st->meta.enc = MP_STR_UTF8;
    st->row_count = 4;
    st->sel = 0;
    st->battery = 42;
    for (uint8_t i = 0; i < 4; i++) {
        st->row[i].label = actions[i];
        st->row[i].enc = MP_STR_UTF8;
        st->row[i].kind = MP_ROW_TEXT;
    }
}

static void fx_jump(mp_ui_state_t *st)
{
    base_state(st, MP_SCREEN_JUMP);
    left_text(st, "跳转到", MP_STR_UTF8);
    st->time_valid = 0;
    st->jump_pct = 42;
}

static void fx_time_set(mp_ui_state_t *st)
{
    base_state(st, MP_SCREEN_TIME_SET);
    left_text(st, "时间设置", MP_STR_UTF8);
    st->mid = "1/5";
    st->time_valid = 0;
    st->time_step = 0;
}

static void fx_time_done(mp_ui_state_t *st)
{
    base_state(st, MP_SCREEN_TIME_DONE);
    left_text(st, "时间设置", MP_STR_UTF8);
    st->msg_lines = 1;
    st->msg_line[0].text = "设置完成";
    st->msg_line[0].enc = MP_STR_UTF8;
    st->hide_hints = 1;
}

static void fx_delete(mp_ui_state_t *st)
{
    base_state(st, MP_SCREEN_DELETE);
    left_text(st, "删除本书", MP_STR_UTF8);
    st->msg_lines = 1;
    st->msg_line[0].text = "需重新上传";
    st->msg_line[0].enc = MP_STR_UTF8;
}

static void fx_lock_none(mp_ui_state_t *st)
{
    base_state(st, MP_SCREEN_LOCK);
    st->lock_valid = 0;
    st->lock_mode = MP_LOCK_MODE_PROMPT;
}

static void fx_no_lock_img(mp_ui_state_t *st)
{
    base_state(st, MP_SCREEN_NO_LOCK_IMG);
    left_text(st, "锁屏展示", MP_STR_UTF8);
    msg2(st, "未上传锁屏图", "用网页上传");
}

static void fx_empty(mp_ui_state_t *st)
{
    base_state(st, MP_SCREEN_EMPTY);
    left_text(st, "书库", MP_STR_UTF8);
    msg2(st, "没有书", "用网页上传");
}

static void fx_error(mp_ui_state_t *st)
{
    base_state(st, MP_SCREEN_ERROR);
    left_text(st, "存储错误", MP_STR_UTF8);
    msg2(st, "读不到内置存储", "重启设备");
}

static void fx_restore(mp_ui_state_t *st)
{
    base_state(st, MP_SCREEN_RESTORE);
    left_text(st, "恢复原厂固件", MP_STR_UTF8);
    st->time_valid = 0;              /* 6 字标题 + 时间会挤 */
    st->msg_lines = 1;
    st->msg_line[0].text = "重启后生效";
    st->msg_line[1].text = NULL;
    st->msg_line[2].text = NULL;
    for (int i = 0; i < 3; i++) {
        st->msg_line[i].enc = MP_STR_UTF8;
    }
}

/* RECOVERY_OK: 2 lines at 44/62, no bottom bar (COPY_DECK §2.10). */
static void fx_restore_ok(mp_ui_state_t *st)
{
    base_state(st, MP_SCREEN_RESTORE);
    left_text(st, "恢复原厂固件", MP_STR_UTF8);
    st->time_valid = 0;
    st->msg_lines = 2;
    st->msg_line[0].text = "已切换";
    st->msg_line[1].text = "3秒后重启";
    st->msg_line[2].text = NULL;
    for (int i = 0; i < 3; i++) {
        st->msg_line[i].enc = MP_STR_UTF8;
    }
    st->hide_hints = 1;
}

static const fixture_t k_fixtures[] = {
    { "clock",           MP_SCREEN_CLOCK,    0,          fx_clock },
    { "select",          MP_SCREEN_SELECT,   0,          fx_select },
    { "read_cn",         MP_SCREEN_READ,     FX_READ,    fx_read_cn },
    { "read_en",         MP_SCREEN_READ,     FX_READ,    fx_read_en },
    { "read_end",        MP_SCREEN_READ,     FX_READ,    fx_read_end },
    { "settings",        MP_SCREEN_SETTINGS, 0,          fx_settings_sel0 },
    { "settings_off",    MP_SCREEN_SETTINGS, 0,          fx_settings_off },
    { "settings_scroll", MP_SCREEN_SETTINGS, 0,           fx_settings_sel7 },
    { "settings_e2",     MP_SCREEN_SETTINGS, 0,           fx_settings_e2 },
    { "backlight",       MP_SCREEN_BACKLIGHT, 0,          fx_backlight },
    { "backlight_popup", MP_SCREEN_READ,      0,          fx_backlight_popup },
    { "book_info",       MP_SCREEN_BOOK_INFO, 0,          fx_book_info },
    { "jump",            MP_SCREEN_JUMP,      0,          fx_jump },
    { "time_set",        MP_SCREEN_TIME_SET,  0,          fx_time_set },
    { "time_done",       MP_SCREEN_TIME_DONE, 0,          fx_time_done },
    { "delete",          MP_SCREEN_DELETE,    0,          fx_delete },
    { "about",           MP_SCREEN_ABOUT,     0,          fx_about },
    { "about_e2",        MP_SCREEN_ABOUT,     0,          fx_about_e2 },
    { "lock_img",        MP_SCREEN_LOCK,      FX_LOCK_IMG,   fx_lock_img },
    { "lock_stats",      MP_SCREEN_LOCK,        FX_LOCK_STATS, fx_lock_stats },
    { "lock_none",       MP_SCREEN_LOCK,        0,             fx_lock_none },
    { "no_lock_img",     MP_SCREEN_NO_LOCK_IMG, 0,             fx_no_lock_img },
    { "empty",           MP_SCREEN_EMPTY,    0,          fx_empty },
    { "error",           MP_SCREEN_ERROR,    0,          fx_error },
    { "low_batt",        MP_SCREEN_LOW_BATT, 0,          fx_low_batt },
    { "restore",         MP_SCREEN_RESTORE,  0,          fx_restore },
    { "restore_ok",      MP_SCREEN_RESTORE,  0,          fx_restore_ok },
};

#define FIXTURE_COUNT ((int)(sizeof k_fixtures / sizeof k_fixtures[0]))

/* ------------------------------------------------------------------ *
 * Frame helpers
 * ------------------------------------------------------------------ */
static int px_is_black(const uint8_t *fb, int x, int y)
{
    uint8_t mask = (uint8_t)(0x80u >> (x & 7));
    return (fb[y * MP_FB_ROW_BYTES + (x >> 3)] & mask) == 0;
}

static int frame_ink(const uint8_t *fb)
{
    int ink = 0;
    for (int y = 0; y < MP_FB_H; y++) {
        for (int x = 0; x < MP_FB_W; x++) {
            if (px_is_black(fb, x, y)) {
                ink++;
            }
        }
    }
    return ink;
}

/* gfx_clear(fb, 0) fills 0xFF; bits 250..255 of each row byte are padding
 * and must stay white. Any renderer writing past x=249 would flip them. */
static int padding_ok(const uint8_t *fb)
{
    for (int y = 0; y < MP_FB_H; y++) {
        if ((fb[y * MP_FB_ROW_BYTES + MP_FB_ROW_BYTES - 1] & 0x3Fu) != 0x3Fu) {
            return 0;
        }
    }
    return 1;
}

static int lock_exact(const uint8_t *fb)
{
    const uint8_t *img = sim_store_lock_img();
    for (int y = 0; y < MP_FB_H; y++) {
        for (int x = 0; x < MP_FB_W; x++) {
            uint8_t b = img[(((y + 6) >> 3) * MP_FB_W) + x];
            int expect_black = (b >> ((y + 6) & 7)) & 1;
            if (expect_black != px_is_black(fb, x, y)) {
                return 0;
            }
        }
    }
    return 1;
}

/* READ：正文墨迹止于 y=109（行顶 22 + 5×18）；110..117 空白；2 px 进度条
 * 占 118..119；120..121 空白。空态是 1 px 底线，已读是 2 px 实心。 */
static int read_layout_ok(const uint8_t *fb)
{
    for (int y = 110; y <= 117; y++) {
        for (int x = 0; x < MP_FB_W; x++) {
            if (px_is_black(fb, x, y)) {
                return 0;
            }
        }
    }
    if (!px_is_black(fb, 2, 119) || !px_is_black(fb, 247, 119)) {
        return 0;
    }
    if (!px_is_black(fb, 2, 118)) {
        return 0;
    }
    for (int y = 120; y < MP_FB_H; y++) {
        for (int x = 0; x < MP_FB_W; x++) {
            if (px_is_black(fb, x, y)) {
                return 0;
            }
        }
    }
    return 1;
}

/* y=18 是 UI v3 的细分隔线：CLOCK 主页不画，其余屏整行黑；y=19 留白。
 * LOCK 兜底页没有状态栏，y=0..18 全白。 */
static int status_rule_ok(const uint8_t *fb, const fixture_t *fx)
{
    int ink = 0;
    for (int x = 0; x < MP_FB_W; x++) {
        if (px_is_black(fb, x, MP_RULE_Y)) {
            ink++;
        }
    }
    if (fx->screen == MP_SCREEN_CLOCK) {
        return ink == 0;
    }
    return ink == MP_FB_W;
}

static int status_gap_ok(const uint8_t *fb)
{
    for (int x = 0; x < MP_FB_W; x++) {
        if (px_is_black(fb, x, MP_RULE_Y + 1)) {
            return 0;
        }
    }
    return 1;
}

static int status_band_blank(const uint8_t *fb)
{
    for (int y = 0; y <= MP_RULE_Y; y++) {
        for (int x = 0; x < MP_FB_W; x++) {
            if (px_is_black(fb, x, y)) {
                return 0;
            }
        }
    }
    return 1;
}

/* 反色选中条：SELECT/SETTINGS 行顶起 20 px（1 px 圆角）；BOOK_INFO 2x2
 * 按钮；TIME_SET 字段名 22x18。 */
static int selected_bar_ok(const uint8_t *fb, const fixture_t *fx)
{
    mp_ui_state_t st;
    int row;
    int top;

    memset(&st, 0, sizeof st);
    fx->build(&st);

    if (fx->screen == MP_SCREEN_SETTINGS || fx->screen == MP_SCREEN_SELECT) {
        int bar_w = (st.row_count > MP_LIST_ROWS) ? 233 : MP_SEL_BAR_W;
        row = (int)st.sel - (int)st.scroll;
        if (row < 0 || row >= MP_LIST_ROWS) {
            return 0;
        }
        top = MP_LIST_TOP + row * MP_LIST_STEP;
        return px_is_black(fb, MP_SEL_BAR_X + 1, top) &&
               px_is_black(fb, MP_SEL_BAR_X + bar_w - 2, top);
    }
    if (fx->screen == MP_SCREEN_BOOK_INFO) {
        row = st.sel;
        if (row < 0 || row >= MP_INFO_ACT_ROWS) {
            return 0;
        }
        int col = row % 2;
        int r = row / 2;
        int x = MP_CONTENT_X + col * (MP_INFO_BTN_W + MP_INFO_BTN_GAP_X);
        int y = MP_INFO_BTN_TOP + r * (MP_INFO_BTN_H + MP_INFO_BTN_GAP_Y);
        return px_is_black(fb, x + 1, y) &&
               px_is_black(fb, x + MP_INFO_BTN_W - 2, y);
    }
    if (fx->screen == MP_SCREEN_TIME_SET) {
        static const int box_x[MP_TIME_FIELDS] = {
            MP_TIME_DATE_X,
            MP_TIME_DATE_X + MP_TIME_YEAR_W + MP_TIME_SEP_W,
            MP_TIME_DATE_X + MP_TIME_YEAR_W + MP_TIME_SEP_W + MP_TIME_TWO_W +
                MP_TIME_SEP_W,
            MP_TIME_CLOCK_X,
            MP_TIME_CLOCK_X + MP_TIME_TWO_W + MP_TIME_SEP_W,
        };
        static const int box_y[MP_TIME_FIELDS] = {
            MP_TIME_DATE_Y, MP_TIME_DATE_Y, MP_TIME_DATE_Y,
            MP_TIME_CLOCK_Y, MP_TIME_CLOCK_Y,
        };
        static const int box_w[MP_TIME_FIELDS] = {
            MP_TIME_YEAR_W, MP_TIME_TWO_W, MP_TIME_TWO_W,
            MP_TIME_TWO_W, MP_TIME_TWO_W,
        };
        int step = st.time_step % MP_TIME_FIELDS;
        int bx = box_x[step];
        int by = box_y[step];
        int bw = box_w[step];
        return px_is_black(fb, bx + 1, by) &&
               px_is_black(fb, bx + bw - 2, by + MP_TIME_BOX_H - 1);
    }
    return 1;
}

static int check_frame(const fixture_t *fx, const uint8_t *fb)
{
    int ok = 1;
    const int ink = frame_ink(fb);
    const int max_ink = MP_FB_W * MP_FB_H - 200;

    if (ink < 200 || ink > max_ink) {
        printf("FAIL %-15s blank frame: ink=%d (want 200..%d)\n", fx->name,
               ink, max_ink);
        ok = 0;
    }
    if (!padding_ok(fb)) {
        printf("FAIL %-15s padding bits x=250..255 not white\n", fx->name);
        ok = 0;
    }
    if (fx->screen == MP_SCREEN_LOCK) {
        if (!(fx->flags & (FX_LOCK_IMG | FX_LOCK_STATS)) &&
            !status_band_blank(fb)) {
            printf("FAIL %-15s LOCK fallback must not draw a status bar\n",
                   fx->name);
            ok = 0;
        }
    } else if (fx->screen == MP_SCREEN_LOW_BATT) {
        if (!status_band_blank(fb)) {
            printf("FAIL %-15s LOW_BATT must not draw a status bar\n",
                   fx->name);
            ok = 0;
        }
    } else {
        if (!status_rule_ok(fb, fx)) {
            printf("FAIL %-15s status rule y=18 wrong (CLOCK=none, others=full)\n",
                   fx->name);
            ok = 0;
        }
        if (!status_gap_ok(fb)) {
            printf("FAIL %-15s status gap y=19 is not blank\n", fx->name);
            ok = 0;
        }
    }
    if ((fx->flags & FX_LOCK_IMG) && !lock_exact(fb)) {
        printf("FAIL %-15s lock image mapping differs from "
               "epd_temp[y+6] (bit 1 = black)\n", fx->name);
        ok = 0;
    }
    if ((fx->flags & FX_READ) && !read_layout_ok(fb)) {
        printf("FAIL %-15s READ layout: need blank 110..117, 2 px progress "
               "track at (2,118)-(247,119), blank 120..121\n", fx->name);
        ok = 0;
    }
    if ((fx->flags & FX_READ) && s_page_bad) {
        printf("FAIL %-15s mp_page_measure: lines=0/%u or line_w>%d\n",
               fx->name, (unsigned)s_page.lines, MP_MAX_LINE_W);
        ok = 0;
    }
    if (!selected_bar_ok(fb, fx)) {
        printf("FAIL %-15s inverse selection bar missing\n", fx->name);
        ok = 0;
    }
    uint32_t want = sim_expected_crc(fx->name);
    uint32_t got = rc_crc32_ieee(fb, MP_FB_BYTES);
    if (want == 0) {
        printf("FAIL %-15s no golden hash for this fixture\n", fx->name);
        ok = 0;
    } else if (got != want) {
        printf("FAIL %-15s pixel hash 0x%08X != 0x%08X (run --hash only "
               "after an intentional UI change)\n", fx->name,
               (unsigned)got, (unsigned)want);
        ok = 0;
    }
    if (ok) {
        printf("PASS %-15s ink=%d\n", fx->name, ink);
    }
    return ok;
}

/* ------------------------------------------------------------------ *
 * Output
 * ------------------------------------------------------------------ */
static int mkdir_p(const char *path)
{
    char tmp[1024];
    int n = snprintf(tmp, sizeof tmp, "%s", path);
    if (n <= 0 || (size_t)n >= sizeof tmp) {
        return -1;
    }
    size_t len = strlen(tmp);
    while (len > 1 && tmp[len - 1] == '/') {
        tmp[--len] = '\0';
    }
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') {
            continue;
        }
        *p = '\0';
        if (mkdir(tmp, 0777) != 0 && errno != EEXIST) {
            return -1;
        }
        *p = '/';
    }
    if (mkdir(tmp, 0777) != 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

static int path_join(char *dst, size_t cap, const char *dir, const char *name,
                     const char *suffix)
{
    int n = snprintf(dst, cap, "%s/%s%s", dir, name, suffix);
    if (n < 0 || (size_t)n >= cap) {
        fprintf(stderr, "sim: output path too long for %u B buffer\n",
                (unsigned)cap);
        return -1;
    }
    return 0;
}

static int write_pgm(const char *path, const uint8_t *fb)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "sim: cannot write %s: %s\n", path, strerror(errno));
        return -1;
    }
    fprintf(f, "P5\n%d %d\n255\n", MP_FB_W, MP_FB_H);
    uint8_t row[MP_FB_W];
    for (int y = 0; y < MP_FB_H; y++) {
        for (int x = 0; x < MP_FB_W; x++) {
            row[x] = px_is_black(fb, x, y) ? 0u : 255u;
        }
        if (fwrite(row, 1, sizeof row, f) != sizeof row) {
            fclose(f);
            fprintf(stderr, "sim: short write to %s\n", path);
            return -1;
        }
    }
    if (fclose(f) != 0) {
        fprintf(stderr, "sim: close failed for %s\n", path);
        return -1;
    }
    return 0;
}

static int write_order(const char *dir)
{
    char path[1200];
    if (path_join(path, sizeof path, dir, "order.txt", "") != 0) {
        return -1;
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "sim: cannot write %s: %s\n", path, strerror(errno));
        return -1;
    }
    for (int i = 0; i < FIXTURE_COUNT; i++) {
        fprintf(f, "%s\n", k_fixtures[i].name);
    }
    fclose(f);
    return 0;
}

static uint8_t s_fb[MP_FB_BYTES];

static void render(const fixture_t *fx)
{
    mp_ui_state_t st;
    memset(&st, 0, sizeof st);
    s_page_bad = 0;
    fx->build(&st);
    mp_ui_render(&st, s_fb);
}

static void usage(FILE *out, const char *argv0)
{
    fprintf(out,
            "usage: %s [--all] [--one NAME] [--out DIR] [--check] [--list]\n"
            "          [--assets DIR]\n"
            "\n"
            "  --all           render every fixture as <out>/pgm/NAME.pgm\n"
            "  --one NAME      render a single fixture\n"
            "  --out DIR       output directory (default: ./out)\n"
            "  --check         validate frames in memory, write nothing\n"
            "  --hash          print each frame's CRC32 (for expected.h)\n"
            "  --list          print fixture names\n"
            "  --assets DIR    override the font asset directory\n"
            "                  (default $MP_SIM_ASSETS or "
            "components/text/assets)\n",
            argv0);
}

int main(int argc, char **argv)
{
    const char *out_dir = "out";
    const char *one = NULL;
    const char *assets = NULL;
    int do_all = 0;
    int do_check = 0;
    int do_list = 0;
    int do_hash = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--all") == 0) {
            do_all = 1;
        } else if (strcmp(argv[i], "--check") == 0) {
            do_check = 1;
        } else if (strcmp(argv[i], "--hash") == 0) {
            do_hash = 1;
        } else if (strcmp(argv[i], "--list") == 0) {
            do_list = 1;
        } else if (strcmp(argv[i], "--one") == 0 && i + 1 < argc) {
            one = argv[++i];
        } else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            out_dir = argv[++i];
        } else if (strcmp(argv[i], "--assets") == 0 && i + 1 < argc) {
            assets = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(stdout, argv[0]);
            return 0;
        } else {
            fprintf(stderr, "sim: unknown argument '%s'\n", argv[i]);
            usage(stderr, argv[0]);
            return 2;
        }
    }

    if (do_list) {
        for (int i = 0; i < FIXTURE_COUNT; i++) {
            printf("%s\n", k_fixtures[i].name);
        }
        return 0;
    }
    if (!do_all && !one && !do_check) {
        do_all = 1;
    }

    printf("sim_ui: %s   sim_hints: %s\n", SIM_UI_KIND, SIM_HINTS_KIND);
    if (strcmp(SIM_UI_KIND, "real") != 0 || strcmp(SIM_HINTS_KIND, "real") != 0) {
        printf("sim: NOTE placeholder sources in use (UI=%s, HINTS=%s) — "
               "frames are schematic, not the shipped UI\n",
               SIM_UI_KIND, SIM_HINTS_KIND);
    }

    int asset_state = sim_text_init(assets);
    if (asset_state != SIM_TEXT_OK) {
        printf("sim: WARNING font assets degraded (state=%d); CJK is drawn as "
               "16 px hollow boxes\n", asset_state);
    }

    if (do_hash) {
        for (int i = 0; i < FIXTURE_COUNT; i++) {
            render(&k_fixtures[i]);
            printf("%s 0x%08X\n", k_fixtures[i].name,
                   (unsigned)rc_crc32_ieee(s_fb, MP_FB_BYTES));
        }
        return 0;
    }

    if (do_check) {
        int failures = 0;
        for (int i = 0; i < FIXTURE_COUNT; i++) {
            render(&k_fixtures[i]);
            if (!check_frame(&k_fixtures[i], s_fb)) {
                failures++;
            }
        }
        printf("check: %d frames, %d failure(s)\n", FIXTURE_COUNT, failures);
        return failures ? 1 : 0;
    }

    if (one) {
        const fixture_t *fx = NULL;
        for (int i = 0; i < FIXTURE_COUNT; i++) {
            if (strcmp(k_fixtures[i].name, one) == 0) {
                fx = &k_fixtures[i];
                break;
            }
        }
        if (!fx) {
            fprintf(stderr, "sim: unknown fixture '%s' (try --list)\n", one);
            return 2;
        }
        char pgm_dir[1200];
        if (path_join(pgm_dir, sizeof pgm_dir, out_dir, "pgm", "") != 0) {
            return 2;
        }
        if (mkdir_p(pgm_dir) != 0) {
            fprintf(stderr, "sim: cannot create %s: %s\n", pgm_dir,
                    strerror(errno));
            return 2;
        }
        render(fx);
        char path[1400];
        if (path_join(path, sizeof path, pgm_dir, fx->name, ".pgm") != 0) {
            return 2;
        }
        if (write_pgm(path, s_fb) != 0) {
            return 2;
        }
        printf("frame %s ink=%d pgm=%s\n", fx->name, frame_ink(s_fb), path);
        return 0;
    }

    char pgm_dir[1200];
    if (path_join(pgm_dir, sizeof pgm_dir, out_dir, "pgm", "") != 0) {
        return 2;
    }
    if (mkdir_p(pgm_dir) != 0) {
        fprintf(stderr, "sim: cannot create %s: %s\n", pgm_dir,
                strerror(errno));
        return 2;
    }
    for (int i = 0; i < FIXTURE_COUNT; i++) {
        render(&k_fixtures[i]);
        char path[1400];
        if (path_join(path, sizeof path, pgm_dir, k_fixtures[i].name,
                      ".pgm") != 0) {
            return 2;
        }
        if (write_pgm(path, s_fb) != 0) {
            return 2;
        }
        printf("frame %-15s ink=%d pgm=%s\n", k_fixtures[i].name,
               frame_ink(s_fb), path);
    }
    if (write_order(out_dir) != 0) {
        return 2;
    }
    printf("order:");
    for (int i = 0; i < FIXTURE_COUNT; i++) {
        printf(" %s", k_fixtures[i].name);
    }
    printf("\n");
    return 0;
}
