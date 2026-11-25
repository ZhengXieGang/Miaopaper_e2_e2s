/* gfx.c — 1bpp primitives for the 250x122 logical frame.
 * Skeleton baseline implementation (display work package owns this file).
 * Layout per DESIGN.md §4.1: fb[y*32 + x/8], MSB left, 1=white 0=black.
 */
#include "gfx.h"

#include <stdbool.h>
#include <string.h>

#include "reader_cfg.h"

#define FB_ROW READER_FB_ROW_BYTES

/* 冻结常量自检:每行字节数能容纳 250 位,整帧恰好 32x122 */
_Static_assert(READER_FB_ROW_BYTES * 8 >= READER_FB_W, "row bytes too small");
_Static_assert(READER_FB_ROW_BYTES * READER_FB_H == READER_FB_BYTES,
               "frame size mismatch");

static inline bool inside(int x, int y)
{
    return x >= 0 && x < READER_FB_W && y >= 0 && y < READER_FB_H;
}

void gfx_clear(uint8_t *fb, int black)
{
    if (!fb) {
        return;
    }
    memset(fb, black ? 0x00 : 0xFF, READER_FB_BYTES);
}

void gfx_px(uint8_t *fb, int x, int y, int black)
{
    if (!fb || !inside(x, y)) {
        return;
    }
    uint8_t mask = (uint8_t)(0x80u >> (x & 7));
    uint8_t *p = &fb[y * FB_ROW + (x >> 3)];
    if (black) {
        *p &= (uint8_t)~mask;
    } else {
        *p |= mask;
    }
}

void gfx_hline(uint8_t *fb, int x0, int x1, int y, int black)
{
    if (!fb || y < 0 || y >= READER_FB_H) {
        return;
    }
    if (x0 > x1) {
        int t = x0;
        x0 = x1;
        x1 = t;
    }
    if (x0 < 0) {
        x0 = 0;
    }
    if (x1 >= READER_FB_W) {
        x1 = READER_FB_W - 1;
    }
    for (int x = x0; x <= x1; x++) {
        gfx_px(fb, x, y, black);
    }
}

void gfx_vline(uint8_t *fb, int x, int y0, int y1, int black)
{
    if (!fb || x < 0 || x >= READER_FB_W) {
        return;
    }
    if (y0 > y1) {
        int t = y0;
        y0 = y1;
        y1 = t;
    }
    if (y0 < 0) {
        y0 = 0;
    }
    if (y1 >= READER_FB_H) {
        y1 = READER_FB_H - 1;
    }
    for (int y = y0; y <= y1; y++) {
        gfx_px(fb, x, y, black);
    }
}

void gfx_rect(uint8_t *fb, int x, int y, int w, int h, int black)
{
    if (w < 0) {
        x += w;
        w = -w;
    }
    if (h < 0) {
        y += h;
        h = -h;
    }
    if (!fb || w <= 0 || h <= 0) {
        return;
    }
    for (int j = 0; j < h; j++) {
        gfx_hline(fb, x, x + w - 1, y + j, black);
    }
}

void gfx_frame(uint8_t *fb, int x, int y, int w, int h, int black)
{
    if (!fb || w <= 0 || h <= 0) {
        return;
    }
    gfx_hline(fb, x, x + w - 1, y, black);
    gfx_hline(fb, x, x + w - 1, y + h - 1, black);
    gfx_vline(fb, x, y, y + h - 1, black);
    gfx_vline(fb, x + w - 1, y, y + h - 1, black);
}

void gfx_invert_rect(uint8_t *fb, int x, int y, int w, int h)
{
    if (!fb || w <= 0 || h <= 0) {
        return;
    }
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w - 1;
    int y1 = y + h - 1;
    if (x1 >= READER_FB_W) {
        x1 = READER_FB_W - 1;
    }
    if (y1 >= READER_FB_H) {
        y1 = READER_FB_H - 1;
    }
    for (int j = y0; j <= y1; j++) {
        for (int i = x0; i <= x1; i++) {
            fb[j * FB_ROW + (i >> 3)] ^= (uint8_t)(0x80u >> (i & 7));
        }
    }
}

void gfx_blit1(uint8_t *fb, int x, int y, int w, int h, const uint8_t *bits, int stride)
{
    if (!fb || !bits || w <= 0 || h <= 0 || stride <= 0) {
        return;
    }
    int max_bits = stride * 8;
    for (int j = 0; j < h; j++) {
        const uint8_t *row = bits + (size_t)j * (size_t)stride;
        for (int i = 0; i < w && i < max_bits; i++) {
            if (row[i >> 3] & (uint8_t)(0x80u >> (i & 7))) {
                gfx_px(fb, x + i, y + j, 1);   /* 1 bit = ink = black */
            }
        }
    }
}
