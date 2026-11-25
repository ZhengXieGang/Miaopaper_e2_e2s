#pragma once
/* frames.bin 的 RLE 解码器（不依赖 ESP-IDF，方便在主机上做比对测试）。 */
#include <stdbool.h>
#include <stdint.h>

/* 把一段 RLE 游程解成 w*h 的 1bpp 图像:
 * 行宽 (w+7)/8, MSB 在前, 1=白; 游程 0/1 交替且从 0 开始,
 * 长度为 LEB128。返回 false 表示数据损坏。 */
bool frames_decode(const uint8_t *p, const uint8_t *end, uint8_t *img,
                   uint16_t w, uint16_t h);
