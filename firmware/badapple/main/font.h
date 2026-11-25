#pragma once
#include <stdint.h>

/* 8x16 点阵字体(ASCII 32..127),返回字形指针 */
const uint8_t *font8x16_glyph(unsigned char ch);
