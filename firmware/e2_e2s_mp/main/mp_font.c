/* mp_font.c — Adafruit-GFX font renderer and HZK16 draw.
 *
 * font16.h/font8x16-era tables come from Miaopaper (MIT repository; the
 * generated font data itself has no separate license — internal builds only).
 * The UI/clock tables are generated from Inter (SIL OFL 1.1) by
 * tools/gen_ui_font.py; see main/fonts/NOTICE.md.
 */
#include "mp_font.h"

#include <stddef.h>
#include <string.h>

#include "gfx.h"
#include "mp_store.h"
#include "text.h"

#ifndef PROGMEM
#define PROGMEM
#endif

#include "fonts/font16.h"
#include "fonts/font_ui16.h"
#include "fonts/font_num26.h"
#include "fonts/font_clock.h"

/* Adafruit-GFX 字体位图是**位连续**打包的：一个 glyph 的行与行之间不补齐到
 * 字节边界。gfx_blit1() 的契约是"每行 stride 字节"，直接传 (w+7)/8 会让
 * 宽度不是 8 的倍数的字形逐行错位（PORT_SPEC §4.8-1，时钟/ASCII 变噪声即此）。
 * 因此这里按 bit 递增贴图；gfx.h 的 stride 契约保持不变（HZK16 仍用 stride 2）。 */
static void blit_bits(uint8_t *fb, int x, int y, int w, int h,
                      const uint8_t *bits, int black)
{
    size_t bit = 0;
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++, bit++) {
            if (bits[bit >> 3] & (uint8_t)(0x80u >> (bit & 7))) {
                gfx_px(fb, x + i, y + j, black);
            }
        }
    }
}

/* 字形表实际项数 = last - first（font16.h 少最后一项，声明的 last 是 0x7E
 * 而表只到 0x7D；按 last 取会读表外）。新生成的 Inter 表按旧约定把 last
 * 写成“最后一个字形 + 1”，所以这里的算式同样适用。越界字节按空格的
 * xAdvance 前进、不绘制——与 mp_page.c 的 mp_ascii_adv() 同一规则。 */
static int gfx_adv(const GFXfont *font, unsigned c)
{
    if (!font || !font->glyph) {
        return 1;
    }
    int count = (int)font->last - (int)font->first;
    int idx = (int)c - (int)font->first;
    if (idx >= 0 && idx < count) {
        return font->glyph[idx].xAdvance;
    }
    int space = (int)' ' - (int)font->first;
    if (space >= 0 && space < count) {
        return font->glyph[space].xAdvance;
    }
    return 1;
}

static const GFXglyph *gfx_glyph(const GFXfont *font, unsigned c)
{
    if (!font || !font->glyph) {
        return NULL;
    }
    int count = (int)font->last - (int)font->first;
    int idx = (int)c - (int)font->first;
    if (idx < 0 || idx >= count) {
        return NULL;
    }
    return &font->glyph[idx];
}

int mp_gfx_width(const GFXfont *font, const char *s)
{
    if (!font || !s) {
        return 0;
    }
    int w = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        w += gfx_adv(font, *p);
    }
    return w;
}

void mp_gfx_text(uint8_t *fb, const GFXfont *font, int x, int baseline,
                 const char *s)
{
    mp_gfx_text_color(fb, font, x, baseline, s, 1);
}

void mp_gfx_text_color(uint8_t *fb, const GFXfont *font, int x, int baseline,
                       const char *s, int black)
{
    if (!fb || !font || !s) {
        return;
    }
    int cx = x;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        const GFXglyph *g = gfx_glyph(font, *p);
        if (g && g->width && g->height) {
            blit_bits(fb, cx + g->xOffset, baseline + g->yOffset, g->width,
                      g->height, font->bitmap + g->bitmapOffset, black);
        }
        cx += gfx_adv(font, *p);
    }
}

void mp_hz_char(uint8_t *fb, uint8_t hi, uint8_t lo, int x, int y)
{
    mp_hz_char_color(fb, hi, lo, x, y, 1);
}

void mp_hz_char_color(uint8_t *fb, uint8_t hi, uint8_t lo, int x, int y,
                      int black)
{
    const uint8_t *g = text_hz16_glyph(hi, lo);
    if (!g) {
        /* 与 Miaopaper 的 draw_hz_char() 一致：没有字模就不画，仍前进 16 px。
         * 画空心框会让 GB2312 里的半角空格显示成方框（PORT_SPEC §4.8-2）。 */
        return;
    }
    for (int j = 0; j < 16; j++) {
        uint16_t row = (uint16_t)(((uint16_t)g[j * 2] << 8) | g[j * 2 + 1]);
        for (int i = 0; i < 16; i++) {
            if (row & (uint16_t)(0x8000u >> i)) {
                gfx_px(fb, x + i, y + j, black);
            }
        }
    }
}

int mp_text_width(const char *s, uint8_t enc)
{
    if (!s) {
        return 0;
    }
    if (enc == MP_ENC_GB2312) {
        int n = 0;
        for (const unsigned char *p = (const unsigned char *)s; *p;) {
            if (p[0] >= 0xA1 && p[1] >= 0xA1) {
                p += 2;
            } else if (*p == '\r' || *p == '\n') {
                p++;
                continue;
            } else {
                p++;
            }
            n += 16;
        }
        return n;
    }
    return mp_gfx_width(&Dialog_plain_16, s);
}

void mp_text_draw(uint8_t *fb, const char *s, uint8_t enc, int x, int baseline,
                  int max_w)
{
    mp_text_draw_color(fb, s, enc, x, baseline, max_w, 1);
}

void mp_text_draw_color(uint8_t *fb, const char *s, uint8_t enc, int x,
                        int baseline, int max_w, int black)
{
    if (!fb || !s) {
        return;
    }
    if (enc != MP_ENC_GB2312) {
        /* Clip to max_w using glyph advances, then draw with the GFX font. */
        char clipped[64];
        int ci = 0;
        int cx = 0;
        const GFXfont *font = &Dialog_plain_16;
        for (const unsigned char *p = (const unsigned char *)s; *p && ci < 63; p++) {
            int adv = gfx_adv(font, *p);
            if (max_w > 0 && cx + adv > max_w) {
                break;
            }
            clipped[ci++] = (char)*p;
            cx += adv;
        }
        clipped[ci] = '\0';
        mp_gfx_text_color(fb, font, x, baseline, clipped, black);
        return;
    }

    int cx = x;
    int y = baseline - 12;   /* HZK glyph top for a 16 px line with baseline 12 */
    for (const unsigned char *p = (const unsigned char *)s; *p;) {
        if (p[0] >= 0xA1 && p[1] >= 0xA1) {
            if (max_w > 0 && cx - x + 16 > max_w) {
                break;
            }
            mp_hz_char_color(fb, p[0], p[1], cx, y, black);
            cx += 16;
            p += 2;
        } else {
            unsigned c = *p++;
            if (c == '\r' || c == '\n') {
                continue;
            }
            if (c < 0x20 || c > 0x7E) {
                c = ' ';
            }
            if (max_w > 0 && cx - x + 16 > max_w) {
                break;
            }
            /* A3 zone = full-width ASCII in the HZK16 file. */
            mp_hz_char_color(fb, 0xA3, (uint8_t)(c + 0x80), cx, y, black);
            cx += 16;
        }
    }
}
