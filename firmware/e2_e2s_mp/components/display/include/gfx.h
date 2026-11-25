#pragma once
/* gfx.h — 1bpp drawing on the logical frame (DESIGN.md §4.1, §17.2)
 *
 * Frame layout: fb[y * READER_FB_ROW_BYTES + x / 8], MSB is the leftmost
 * pixel, bit 1 = white, bit 0 = black. Coordinates are clipped; functions
 * accept out-of-range input and draw the visible part.
 */
#include <stdint.h>

void gfx_clear(uint8_t *fb, int black);
void gfx_px(uint8_t *fb, int x, int y, int black);
void gfx_hline(uint8_t *fb, int x0, int x1, int y, int black);   /* inclusive */
void gfx_vline(uint8_t *fb, int x, int y0, int y1, int black);   /* inclusive */
void gfx_rect(uint8_t *fb, int x, int y, int w, int h, int black);
void gfx_frame(uint8_t *fb, int x, int y, int w, int h, int black);
void gfx_invert_rect(uint8_t *fb, int x, int y, int w, int h);

/* Blit a 1bpp MSB-first bitmap. Bit 1 means ink (black), bit 0 leaves the
 * destination untouched. stride is in bytes per source row. */
void gfx_blit1(uint8_t *fb, int x, int y, int w, int h, const uint8_t *bits, int stride);
