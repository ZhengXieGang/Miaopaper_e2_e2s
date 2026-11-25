/* 来源:firmware/badapple/main/epd.h(E2S r22 驱动,原厂 1.3.739 反汇编还原)。
 * 原始内容按 DESIGN.md §4.2/§4.5 增加 epd_family() 与 epd_diag() 原型;
 * 其余引脚/尺寸定义未改。
 */
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

/* 局刷 0x26 的兜底 settle:真机实测 0x20 后约 454 ms BUSY 回低;正常情况下
 * 严格等空闲后立即写 0x26,只有等待超时(面板 3 s 无反应)才额外等这个值。
 * 这是该常量的唯一定义处:display 侧(display_internal.h)直接包含本头,
 * 不再复制同值常量,避免改一处漏一处;允许 -D 覆盖做真机调参。 */
#ifndef EPD_PARTIAL_SETTLE_MS
#define EPD_PARTIAL_SETTLE_MS 500
#endif

/* 返回 0 成功;非 0 失败(不影响其他功能) */
int epd_init(void);
/* 显示一帧全屏图像(1bpp,每行 EPD_W 位,高位在前),自动全刷 */
int epd_draw(const uint8_t *img);
/* 追加一帧: BOE 走局部刷新; 其他面板族回退到全刷。
 * 返回 0 成功,非 0 为 SPI 错误(BUSY 超时不算失败)。 */
int epd_draw_partial(const uint8_t *img);
/* full=1 强制全刷, full=0 尝试局部刷新(family!=1 时内部降级为全刷)。
 * 任何 SPI 错误返回非 0 (DESIGN §4.2)。 */
int epd_draw_ex(const uint8_t *img, int full);
/* 面板族: 1=BOE, 3=OED/非 BOE, 0=未知或初始化失败 (DESIGN §4.5)。 */
int epd_family(void);
/* 把驱动内部状态写成一行文本(out 以 NUL 结尾,超长截断),供 INFO/诊断使用。 */
void epd_diag(char *out, int n);
void epd_deep_sleep(void);
