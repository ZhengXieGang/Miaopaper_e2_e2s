/* sim/stub_ui.c — schematic placeholder for mp_ui.c.
 *
 * Compiled ONLY when ../main/mp_ui.c does not exist (Makefile auto-detect)
 * or when UI=dummy is forced. Purpose: prove the PGM -> PNG -> contact-sheet
 * pipeline and the real gfx / text / HZK stack end to end before the
 * integration phase lands the real renderer. It is NOT the shipped UI: it draws "SIM" in the status bar and
 * keeps the frozen geometry (status 0..17, list 22+18i, read 20+18i,
 * progress 2,115,246x4, hints 104..121) so --check can already assert those
 * invariants.
 *
 * Deliberate simplification: ASCII strings go through text_draw_line (8x16)
 * instead of mp_text_draw(Dialog_plain_16). mp_font.c still has the P0
 * bit-continuous blit bug from PORT_SPEC §4.8; the placeholder must not look
 * broken, and main/ is out of the simulator's write scope.
 */
#include "mp_ui.h"

#include <stdio.h>
#include <string.h>

#include "gfx.h"
#include "mp_font.h"
#include "mp_hints.h"
#include "text.h"

/* 16x16 lock icon, PORT_SPEC §1.7.3 (bit 1 = ink). */
static const uint8_t k_lock_icon[32] = {
    0x00, 0x00, 0x07, 0xE0, 0x07, 0xE0, 0x0C, 0x30, 0x0C, 0x30, 0x0C, 0x30,
    0x3F, 0xFC, 0x20, 0x04, 0x21, 0x84, 0x21, 0x84, 0x21, 0x84, 0x21, 0x84,
    0x21, 0x84, 0x20, 0x04, 0x3F, 0xFC, 0x00, 0x00,
};

/* ------------------------------------------------------------------ *
 * Text helpers
 * ------------------------------------------------------------------ */
void mp_ui_clear(uint8_t *fb)
{
    gfx_clear(fb, 0);
}

int mp_ui_text_w(const char *utf8)
{
    return text_measure(utf8);
}

void mp_ui_text(uint8_t *fb, int x, int top, const char *utf8, int max_w)
{
    if (!fb || !utf8) {
        return;
    }
    text_draw_line(fb, x, top, utf8, max_w, 1);
}

void mp_ui_text_center(uint8_t *fb, int top, const char *utf8)
{
    if (!fb || !utf8) {
        return;
    }
    int w = text_measure(utf8);
    int x = (MP_FB_W - w) / 2;
    if (x < 0) {
        x = 0;
    }
    text_draw_line(fb, x, top, utf8, MP_FB_W - x, 1);
}

/* Contract version: raw bytes + MP_ENC_*, baseline is always top+12. */
void mp_ui_gb_text(uint8_t *fb, int x, int top, const char *s, uint8_t enc,
                   int max_w)
{
    if (!fb || !s) {
        return;
    }
    mp_text_draw(fb, s, enc, x, top + 12, max_w);
}

static int row_w(const mp_ui_row_t *r)
{
    if (!r || !r->label) {
        return 0;
    }
    if (r->enc == MP_STR_GB2312) {
        return mp_text_width(r->label, MP_ENC_GB2312);
    }
    if (r->enc == MP_STR_ASCII) {
        return mp_text_width(r->label, MP_ENC_ASCII);
    }
    return text_measure(r->label);
}

/* Placeholder drawing: GB2312 uses the real HZK path (mp_text_draw), ASCII
 * uses the already-correct 8x16 path so the buggy Dialog blit is not
 * exercised here. */
static void draw_line_enc(uint8_t *fb, int x, int top, const char *text,
                          uint8_t enc, int max_w)
{
    if (!text) {
        return;
    }
    if (enc == MP_STR_GB2312) {
        mp_text_draw(fb, text, MP_ENC_GB2312, x, top + 12, max_w);
    } else {
        text_draw_line(fb, x, top, text, max_w, 1);
    }
}

static void draw_str(uint8_t *fb, int x, int top, const mp_str_t *s, int max_w)
{
    if (!s) {
        return;
    }
    draw_line_enc(fb, x, top, s->text, s->enc, max_w);
}

static void draw_row(uint8_t *fb, int x, int top, const mp_ui_row_t *r,
                     int max_w)
{
    if (!r) {
        return;
    }
    draw_line_enc(fb, x, top, r->label, r->enc, max_w);
}

void mp_ui_underline(uint8_t *fb, int row_top, int text_w)
{
    int w = text_w;
    if (w > MP_CONTENT_W) {
        w = MP_CONTENT_W;
    }
    int x = MP_CONTENT_X + (w - MP_SEL_RULE_W) / 2;
    if (x < MP_CONTENT_X) {
        x = MP_CONTENT_X;
    }
    gfx_rect(fb, x, row_top + 16, MP_SEL_RULE_W, MP_SEL_RULE_H, 1);
}

/* ------------------------------------------------------------------ *
 * Status bar / battery
 * ------------------------------------------------------------------ */
void mp_ui_battery(uint8_t *fb, int x, int y, uint8_t pct, uint8_t valid,
                   uint8_t charging)
{
    if (!fb) {
        return;
    }
    gfx_frame(fb, x, y, MP_BATT_W, MP_BATT_H, 1);
    gfx_rect(fb, x + MP_BATT_W, y + 3, MP_BATT_CAP_W, MP_BATT_CAP_H, 1);
    if (!valid) {
        return;
    }
    int w;
    if (pct < 5) {
        w = 0;
    } else if (pct < 20) {
        w = 3;
    } else if (pct < 40) {
        w = 7;
    } else if (pct < 60) {
        w = 11;
    } else if (pct < 80) {
        w = 15;
    } else {
        w = 19;
    }
    if (w > 0) {
        gfx_rect(fb, x + 1, y + 1, w, 8, 1);
    }
    if (charging) {
        gfx_hline(fb, x - 6, x - 2, y + 5, 1);
        gfx_vline(fb, x - 4, y + 3, y + 7, 1);
    }
}

void mp_ui_status_bar(uint8_t *fb, const mp_ui_state_t *st)
{
    if (!fb || !st) {
        return;
    }
    /* Placeholder marker: makes dummy frames unmistakable. */
    text_draw_line(fb, 2, MP_STATUS_TEXT_TOP, "SIM", 40, 1);
    draw_str(fb, 32, MP_STATUS_TEXT_TOP, &st->left,
             MP_STATUS_TITLE_MAX_W - 30);
    if (st->mid && st->mid[0]) {
        text_draw_line(fb, MP_STATUS_MID_X, MP_STATUS_TEXT_TOP, st->mid, 46, 1);
    }
    if (st->time_valid) {
        char buf[8];
        snprintf(buf, sizeof buf, "%02u:%02u", (unsigned)st->hour,
                 (unsigned)st->min);
        text_draw_line(fb, 108, MP_STATUS_TEXT_TOP, buf, 40, 1);
    }
    mp_ui_battery(fb, MP_BATT_X, MP_BATT_Y, st->battery,
                  st->battery_valid, st->charging);
    gfx_hline(fb, 0, MP_FB_W - 1, MP_RULE_Y, 1);
}

/* ------------------------------------------------------------------ *
 * Screens
 * ------------------------------------------------------------------ */
void mp_ui_clock(uint8_t *fb, const mp_ui_state_t *st)
{
    if (!fb || !st) {
        return;
    }
    char buf[32];
    snprintf(buf, sizeof buf, "%02u:%02u", (unsigned)st->hour,
             (unsigned)st->min);
    text_draw_line(fb, 52, 30, buf, 200, 1);
    /* UI v2: home page has the clock only (no rule, no info line). */
}

void mp_ui_list(uint8_t *fb, const mp_ui_state_t *st)
{
    if (!fb || !st) {
        return;
    }
    for (uint8_t i = 0; i < MP_LIST_ROWS; i++) {
        int top = MP_LIST_TOP + MP_LIST_STEP * (int)i;
        draw_row(fb, MP_CONTENT_X, top, &st->row[i], MP_CONTENT_W);
    }
    int sel_row = (int)st->sel - (int)st->scroll;
    if (sel_row >= 0 && sel_row < MP_LIST_ROWS) {
        mp_ui_underline(fb, MP_LIST_TOP + MP_LIST_STEP * sel_row,
                        row_w(&st->row[sel_row]));
    }
}

void mp_ui_about(uint8_t *fb, const mp_ui_state_t *st)
{
    if (!fb || !st) {
        return;
    }
    for (uint8_t i = 0; i < st->about_lines && i < MP_LIST_ROWS; i++) {
        draw_str(fb, MP_CONTENT_X, MP_LIST_TOP + MP_LIST_STEP * (int)i,
                 &st->about_line[i], MP_CONTENT_W);
    }
}

void mp_ui_read(uint8_t *fb, const mp_ui_state_t *st)
{
    if (!fb || !st) {
        return;
    }
    for (uint8_t i = 0; i < st->line_count && i < MP_READ_LINES; i++) {
        int top = MP_READ_TOP + MP_READ_STEP * (int)i;
        if (st->line_enc == MP_ENC_GB2312) {
            mp_text_draw(fb, st->line[i], MP_ENC_GB2312, MP_READ_X, top + 12,
                         MP_READ_W);
        } else {
            /* See file header: keep the placeholder free of the known
             * Dialog blit bug. text_draw_line adds READER_ASCII_DY(=4)
             * internally, so pass top-2 to keep the 8x16 ink inside the
             * 18 px row (ink lands at top+4..top+13). */
            text_draw_line(fb, MP_READ_X, top - 2, st->line[i], MP_READ_W, 1);
        }
    }
}

void mp_ui_progress(uint8_t *fb, uint8_t pct)
{
    if (!fb) {
        return;
    }
    gfx_frame(fb, 2, MP_PROGRESS_Y, 246, MP_PROGRESS_H, 1);
    int w = (244 * (int)pct + 50) / 100;
    if (pct > 0 && w == 0) {
        w = 1;
    }
    if (w > 244) {
        w = 244;
    }
    if (w > 0) {
        gfx_rect(fb, 3, MP_PROGRESS_Y + 1, w, 2, 1);
    }
}

void mp_ui_msg(uint8_t *fb, const mp_ui_state_t *st)
{
    if (!fb || !st) {
        return;
    }
    int top = (st->msg_lines == 3) ? MP_MSG_TOP3 : MP_MSG_TOP2;
    for (uint8_t i = 0; i < st->msg_lines && i < 3; i++) {
        mp_ui_text_center(fb, top + MP_MSG_STEP * (int)i, st->msg_line[i].text);
    }
}

void mp_ui_lock(uint8_t *fb, const mp_ui_state_t *st)
{
    if (!fb || !st) {
        return;
    }
    if (st->lock_valid && st->lock_img) {
        /* epd_temp -> logical frame, PORT_SPEC §1.5.6: source rows 6..127,
         * bit 1 = black, pass it straight to gfx_px's black argument. */
        for (int y = 0; y < MP_FB_H; y++) {
            for (int x = 0; x < MP_FB_W; x++) {
                uint8_t b = st->lock_img[(((y + 6) >> 3) * MP_FB_W) + x];
                int black = (b >> ((y + 6) & 7)) & 1;
                gfx_px(fb, x, y, black);
            }
        }
        return;
    }
    gfx_blit1(fb, MP_LOCK_ICON_X, MP_LOCK_ICON_Y, 16, 16, k_lock_icon, 2);
    mp_ui_text_center(fb, MP_LOCK_TEXT_TOP, "已锁定");
    int x = MP_LOCK_HINT_X;
    x += text_draw_line(fb, x, MP_HINT_TEXT_TOP, "长按", 82, 1) + MP_KEYCAP_GAP;
    mp_hints_keycap(fb, MP_HINT_OK, x, MP_KEYCAP_TOP);
    x += MP_KEYCAP_W + MP_KEYCAP_GAP;
    text_draw_line(fb, x, MP_HINT_TEXT_TOP, "解锁", 82, 1);
}

void mp_ui_backlight(uint8_t *fb, const mp_ui_state_t *st)
{
    if (!fb || !st) {
        return;
    }
    char buf[24];
    snprintf(buf, sizeof(buf), "背光 %u/3", (unsigned)(st->bl_level & 3));
    mp_ui_text_center(fb, 46, buf);
}

void mp_ui_backlight_popup(uint8_t *fb, const mp_ui_state_t *st)
{
    if (!fb || !st) {
        return;
    }
    gfx_rect(fb, 30, 26, 190, 68, 0);
    gfx_frame(fb, 30, 26, 190, 68, 1);
    char buf[24];
    snprintf(buf, sizeof(buf), "BL %u/3", (unsigned)(st->bl_level & 3));
    mp_ui_text_center(fb, 50, buf);
}

void mp_ui_charge_popup(uint8_t *fb, const mp_ui_state_t *st)
{
    if (!fb || !st) {
        return;
    }
    gfx_rect(fb, 30, 26, 190, 68, 0);
    gfx_frame(fb, 30, 26, 190, 68, 1);
    char buf[24];
    snprintf(buf, sizeof(buf), "CHG %u%%", (unsigned)st->battery);
    mp_ui_text_center(fb, 50, buf);
}

void mp_ui_low_batt(uint8_t *fb)
{
    if (!fb) {
        return;
    }
    gfx_frame(fb, 105, 28, 40, 20, 1);
    mp_ui_text_center(fb, 60, "电量耗尽");
    mp_ui_text_center(fb, 80, "请充电");
}

/* ------------------------------------------------------------------ *
 * Entry point
 * ------------------------------------------------------------------ */
void mp_ui_render(const mp_ui_state_t *st, uint8_t *fb)
{
    if (!fb) {
        return;
    }
    gfx_clear(fb, 0);
    if (!st) {
        return;
    }
    if (st->screen == MP_SCREEN_LOCK) {
        mp_ui_lock(fb, st);
        if (st->charge_popup) {
            mp_ui_charge_popup(fb, st);
        }
        return;
    }
    if (st->screen == MP_SCREEN_LOW_BATT) {
        mp_ui_low_batt(fb);
        return;
    }
    mp_ui_status_bar(fb, st);
    switch (st->screen) {
    case MP_SCREEN_CLOCK:
        mp_ui_clock(fb, st);
        break;
    case MP_SCREEN_SELECT:
    case MP_SCREEN_SETTINGS:
    case MP_SCREEN_TOC:
        mp_ui_list(fb, st);
        break;
    case MP_SCREEN_READ:
        mp_ui_read(fb, st);
        mp_ui_progress(fb, st->percent);
        break;
    case MP_SCREEN_ABOUT:
        mp_ui_about(fb, st);
        break;
    case MP_SCREEN_BACKLIGHT:
        mp_ui_backlight(fb, st);
        break;
    case MP_SCREEN_EMPTY:
    case MP_SCREEN_TOC_EMPTY:
    case MP_SCREEN_ERROR:
    case MP_SCREEN_RESTORE:
    case MP_SCREEN_NO_LOCK_IMG:
        mp_ui_msg(fb, st);
        break;
    default:
        break;
    }
    if (!st->hide_hints) {
        mp_hint_set_t hints;
        if (mp_hints_for((mp_screen_t)st->screen, &hints) > 0) {
            mp_hints_draw(fb, &hints);
        }
    }
    if (st->bl_popup) {
        mp_ui_backlight_popup(fb, st);
    } else if (st->charge_popup) {
        mp_ui_charge_popup(fb, st);
    }
}
