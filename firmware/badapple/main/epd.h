#pragma once
/* E2S 2.13" e-paper 驱动(引脚来自官方固件 1.3.739 逆向) */
#include <stdint.h>

#define EPD_PIN_CS    40
#define EPD_PIN_DC    39
#define EPD_PIN_RST   48
#define EPD_PIN_BUSY  47
#define EPD_PIN_MOSI  41
#define EPD_PIN_SCLK  42

#define EPD_W 250
#define EPD_H 122

/* 返回 0 成功;非 0 失败(不影响其他功能) */
int epd_init(void);
/* 显示一帧全屏图像(1bpp,每行 EPD_W 位,高位在前),自动全刷 */
int epd_draw(const uint8_t *img);
/* 追加一帧: BOE 走局部刷新; 其他面板族回退到全刷。 */
int epd_draw_partial(const uint8_t *img);
/* full=1 强制全刷, full=0 尝试局部刷新。 */
int epd_draw_ex(const uint8_t *img, int full);
void epd_deep_sleep(void);

/* 真机时序测量：只对 BOE 族有效，用 1 ms 轮询记录 GPIO47 的每次翻转，
 * 严格等到 BUSY 回到低电平（不再用 120 ms/20 s 的保守超时）。
 * 调用顺序：epd_timing_begin() -> epd_draw_ex(...,1) -> epd_draw_ex(...,0)
 * -> epd_timing_end()；结果通过 epd_timing_diag() 读回。 */
void epd_timing_begin(void);
void epd_timing_end(void);
void epd_timing_diag(char *out, int n);
