/* sim/stub_hints.c — schematic placeholder for mp_hints.c.
 *
 * Compiled ONLY when ../main/mp_hints.c does not exist (Makefile auto-detect)
 * or when HINTS=dummy is forced.
 * It implements the frozen bottom-bar table from PORT_SPEC §2.5 with the
 * 14x14 keycaps from §1.7.1 so the dummy frames and --check have real ink in
 * the hint band. The shipped implementation is main/mp_hints.c.
 */
#include "mp_hints.h"

#include <stddef.h>

#include "gfx.h"
#include "text.h"

/* PORT_SPEC §1.7.1: 14 rows x 2 B, MSB left, bit 1 = ink. */
const uint8_t mp_keycap_bits[MP_HINT_KEY_COUNT][MP_KEYCAP_H * 2] = {
    /* PREV ◀ */
    { 0xFF, 0xFC, 0x80, 0x04, 0x80, 0x04, 0x81, 0x84, 0x83, 0x84, 0x87, 0x84,
      0x8F, 0x84, 0x8F, 0x84, 0x87, 0x84, 0x83, 0x84, 0x81, 0x84, 0x80, 0x04,
      0x80, 0x04, 0xFF, 0xFC },
    /* NEXT ▶ */
    { 0xFF, 0xFC, 0x80, 0x04, 0x80, 0x04, 0x86, 0x04, 0x87, 0x04, 0x87, 0x84,
      0x87, 0xC4, 0x87, 0xC4, 0x87, 0x84, 0x87, 0x04, 0x86, 0x04, 0x80, 0x04,
      0x80, 0x04, 0xFF, 0xFC },
    /* OK ○ */
    { 0xFF, 0xFC, 0x80, 0x04, 0x87, 0x84, 0x8C, 0xC4, 0x90, 0x24, 0xB0, 0x34,
      0xA0, 0x14, 0xA0, 0x14, 0xB0, 0x34, 0x90, 0x24, 0x8C, 0xC4, 0x87, 0x84,
      0x80, 0x04, 0xFF, 0xFC },
    /* MENU × */
    { 0xFF, 0xFC, 0x80, 0x04, 0x80, 0x04, 0x80, 0x04, 0x8C, 0xC4, 0x87, 0x84,
      0x83, 0x04, 0x83, 0x04, 0x87, 0x84, 0x8C, 0xC4, 0x80, 0x04, 0x80, 0x04,
      0x80, 0x04, 0xFF, 0xFC },
    /* 胶囊菜单 ▭ (FUNC slot) */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F, 0xE0, 0x20, 0x10,
      0x40, 0x08, 0x40, 0x08, 0x20, 0x10, 0x1F, 0xE0, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00 },
};

void mp_hints_keycap(uint8_t *fb, mp_hint_key_t key, int x, int ytop)
{
    if (!fb || key < 0 || key >= MP_HINT_KEY_COUNT) {
        return;
    }
    gfx_blit1(fb, x, ytop, MP_KEYCAP_W, MP_KEYCAP_H, mp_keycap_bits[key], 2);
}

static int item_width(const mp_hint_item_t *it)
{
    int w = 0;
    int first = 1;
    if (it->pre) {
        w += text_measure(it->pre);
        first = 0;
    }
    for (uint8_t k = 0; k < it->nkeys; k++) {
        if (!first) {
            w += MP_KEYCAP_GAP;
        }
        w += MP_KEYCAP_W;
        first = 0;
    }
    if (it->post) {
        if (!first) {
            w += MP_KEYCAP_GAP;
        }
        w += text_measure(it->post);
    }
    return w;
}

int mp_hints_width(const mp_hint_set_t *s)
{
    if (!s || s->n == 0) {
        return 0;
    }
    int w = 0;
    for (uint8_t i = 0; i < s->n && i < MP_HINT_MAX; i++) {
        w += item_width(&s->item[i]);
        if (i + 1 < s->n) {
            w += MP_HINT_GAP;
        }
    }
    return w;
}

void mp_hints_draw(uint8_t *fb, const mp_hint_set_t *s)
{
    if (!fb || !s) {
        return;
    }
    int x = MP_HINT_X;
    const int limit = MP_HINT_X + MP_HINT_MAX_W;
    for (uint8_t i = 0; i < s->n && i < MP_HINT_MAX; i++) {
        const mp_hint_item_t *it = &s->item[i];
        int w = item_width(it);
        if (x + w > limit) {
            break;   /* drop the whole tail group, never wrap */
        }
        int cx = x;
        int first = 1;
        if (it->pre) {
            text_draw_line(fb, cx, MP_HINT_TEXT_TOP, it->pre, MP_HINT_MAX_W, 1);
            cx += text_measure(it->pre);
            first = 0;
        }
        for (uint8_t k = 0; k < it->nkeys; k++) {
            if (!first) {
                cx += MP_KEYCAP_GAP;
            }
            mp_hints_keycap(fb, (mp_hint_key_t)it->key[k], cx, MP_KEYCAP_TOP);
            cx += MP_KEYCAP_W;
            first = 0;
        }
        if (it->post) {
            if (!first) {
                cx += MP_KEYCAP_GAP;
            }
            text_draw_line(fb, cx, MP_HINT_TEXT_TOP, it->post,
                           limit - cx, 1);
        }
        x += w + MP_HINT_GAP;
    }
}

static void set_item(mp_hint_set_t *s, uint8_t *n, uint8_t nkeys,
                     const uint8_t *keys, const char *pre, const char *post)
{
    if (*n >= MP_HINT_MAX) {
        return;
    }
    mp_hint_item_t *it = &s->item[*n];
    it->nkeys = nkeys;
    for (uint8_t k = 0; k < 2; k++) {
        it->key[k] = (k < nkeys) ? keys[k] : 0;
    }
    it->pre = pre;
    it->post = post;
    (*n)++;
}

/* Frozen table, PORT_SPEC §2.5 / COPY_DECK §4. */
uint8_t mp_hints_for(mp_screen_t screen, mp_hint_set_t *out)
{
    if (!out) {
        return 0;
    }
    out->n = 0;
    uint8_t n = 0;
    static const uint8_t prev[1] = { MP_HINT_PREV };
    static const uint8_t next[1] = { MP_HINT_NEXT };
    static const uint8_t prev_next[2] = { MP_HINT_PREV, MP_HINT_NEXT };
    static const uint8_t ok[1] = { MP_HINT_OK };
    static const uint8_t menu[1] = { MP_HINT_MENU };

    switch (screen) {
    case MP_SCREEN_CLOCK:
        set_item(out, &n, 1, prev, NULL, "设置");
        set_item(out, &n, 1, next, NULL, "书库");
        break;
    case MP_SCREEN_SELECT:
    case MP_SCREEN_TOC:
        set_item(out, &n, 2, prev_next, NULL, "选择");
        set_item(out, &n, 1, ok, NULL, "打开");
        set_item(out, &n, 1, menu, NULL, "返回");
        break;
    case MP_SCREEN_TOC_EMPTY:
        set_item(out, &n, 1, ok, NULL, "跳转");
        set_item(out, &n, 1, menu, NULL, "返回");
        break;
    case MP_SCREEN_SETTINGS:
        set_item(out, &n, 2, prev_next, NULL, "移动");
        set_item(out, &n, 1, ok, NULL, "确定");
        set_item(out, &n, 1, menu, NULL, "返回");
        break;
    case MP_SCREEN_ABOUT:
        set_item(out, &n, 2, prev_next, NULL, "翻页");
        set_item(out, &n, 1, ok, NULL, "返回");
        break;
    case MP_SCREEN_EMPTY:
        set_item(out, &n, 1, ok, NULL, "刷新");
        set_item(out, &n, 1, menu, NULL, "返回");
        break;
    case MP_SCREEN_ERROR:
        set_item(out, &n, 1, ok, NULL, "重试");
        set_item(out, &n, 1, menu, NULL, "返回");
        break;
    case MP_SCREEN_BACKLIGHT:
        set_item(out, &n, 2, prev_next, NULL, "调整");
        set_item(out, &n, 1, ok, NULL, "确定");
        set_item(out, &n, 1, menu, NULL, "取消");
        break;
    case MP_SCREEN_RESTORE:
        set_item(out, &n, 1, ok, "长按", "恢复");
        set_item(out, &n, 1, menu, NULL, "取消");
        break;
    case MP_SCREEN_NO_LOCK_IMG:
        set_item(out, &n, 1, menu, NULL, "返回");
        break;
    case MP_SCREEN_READ:
    case MP_SCREEN_LOCK:
    case MP_SCREEN_LOW_BATT:
    default:
        break;
    }
    out->n = n;
    return n;
}
