/* 来源:firmware/badapple/main/epd.c(E2S r22 驱动,原厂 1.3.739 反汇编还原)。
 * 原始内容按 DESIGN.md §4.2 做了三处必要修改(波形 LUT 与命令序列未改):
 *   1) epd_send()/wcmd()/wdata() 返回 esp_err_t,错误经各刷新函数逐层传播,
 *      任何 SPI 错误让 epd_draw_ex() 返回非 0;BUSY 超时不算失败(§4.2);
 *   2) BOE 路径改为"等 BUSY 连续 10 ms 为低"再写 0x26;若没观察到忙电平
 *      或等待超时,才补 EPD_PARTIAL_SETTLE_MS=500 ms(面板无反应的兜底);
 *   3) 新增 epd_family() 访问器;family!=1 时局刷请求一律降级全刷(§4.5)。
 *
 * 真机实测(2026-11-24,BOE,两次逐位一致):全刷完成 1,312 ms(0x20 后
 * 1,266 ms),局刷完成 454 ms;BUSY 极性为高=忙、低=空闲(0x20 拉高,完成后
 * 回低)。早期文档里的 28 s / 126 ms 是旧等待策略(等"回到起始电平" + 120 ms
 * 起始窗口)的返回值,不是面板耗时。本版 BOE 路径改为严格等待 BUSY 回低,
 * 再写 0x26;OED 分支仍沿用原厂 wait_level(1,...),极性假设需 OED 实机确认
 * (docs/DESIGN.md §4.3)。
 */
/* E2S 2.13" e-paper 驱动 (r22: 按原厂 discern 分 BOE/OED 两族)
 *
 * 反汇编原厂 1.3.739 + 本地 e2s_re 对照结论:
 *
 *   GPIO: CS=40, DC=39(pre_cb), RST=48, BUSY/类型脚=47(输入上拉), MOSI=41, SCLK=42
 *         SPI3_HOST(host id 2), 3WIRE|HALFDUPLEX, mode0, 10MHz, spics=-1
 *         (e2s_re 里写的 CS=39/DC=46/PWR=40 与汇编不符, 不能照抄)
 *
 *   discern(0x4202aa40): 复位后读 GPIO47
 *     - 47==0 -> BOE(type 1), 不读 ID
 *     - 47==1 -> 读 0x70 5B; id[2]==5 -> OED(type 3), 否则 GD(type 0, 原厂不刷新)
 *
 *   两套刷新路径:
 *     BOE/GD(0x42029ed4): 0x12 -> 0x01 FA0000 -> 0x11 03 -> 0x21 0080 -> 0x18 80
 *          -> 0x3C 05 -> 0x3F 22 -> 0x03 17 -> 0x04 41A832 -> 0x2C 58
 *          -> 0x32 153B LUT -> 窗口 -> 0x24/0x26 帧 -> 0x22 C7 -> 0x20
 *     OED(0x4202a424 "OED is all"): 复位 -> 等 47==1 -> 0x00 FF89/01/03/06/30/60
 *          /82/E3/41/61/65/50/50 -> 0x20/21/24/22/23 各 56B LUT(含首字节 0x01)
 *          -> 等 47==1 -> 仅 0x13 帧(4000B) -> 等 47==1 -> 0x17 A5
 *          -> 等 47==0 -> 0x41 07 -> 等 47==1 完成 -> 0x41 07
 *
 * BOE 实测 BUSY 高=忙/低=空闲,用严格空闲等待(连续 10 ms 低,期间变高就
 * 重置重等)并记录 ev= 轨迹;OED (JD79653) 依据原厂 E2 1.3.759 / E2S 1.3.739
 * 反汇编为 BUSY_N 低=忙/高=空闲,发 0x17 A5 后先等变低确认进入刷新,再等连续
 * 10 ms 回高确认刷新完成(未见到低电平则按原厂 800 ms 保护窗兜底)。
 */
#include "epd.h"
#include "epd_lut_stock.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

/* 局刷 0x26 回写前的额外 settle 常量唯一定义在 epd.h(EPD_PARTIAL_SETTLE_MS),
 * 这里不再复制同值宏;display 侧包含 epd.h 即可(D7)。 */

static const char *TAG = "epd";
static spi_device_handle_t s_spi;
static int s_ok = -1;
static int s_init_ok;
static int s_draw_err = -1000;
static uint32_t s_written;
static uint32_t s_draw_ms;
static uint8_t s_id[5], s_id2[5];
static int s_rd_err = -1000;
static int s_temp = -1000;
static int s_stage;
static int s_pad47 = -1;     /* 复位后 GPIO47 电平: 原厂用它分族 */
static int s_family;         /* 1=BOE, 3=OED, 0=GD/unknown */
static uint32_t s_partial_n, s_partial_ms, s_partial_total;
static uint32_t s_full_n, s_full_ms;

/* 等待事件轨迹: 给 (1,241) 诊断用, 最多 8 条。 */
typedef struct {
    char tag[4];
    uint8_t start, end, changed;
    uint16_t ms;
} wait_ev_t;
static wait_ev_t s_ev[8];
static int s_evn;

static void ev_add(const char *tag, int start, int end, int changed, uint32_t ms)
{
    if (s_evn >= 8) return;
    snprintf(s_ev[s_evn].tag, sizeof(s_ev[s_evn].tag), "%s", tag);
    s_ev[s_evn].start = (uint8_t)start;
    s_ev[s_evn].end = (uint8_t)end;
    s_ev[s_evn].changed = (uint8_t)changed;
    s_ev[s_evn].ms = ms > 65535 ? 65535 : (uint16_t)ms;
    s_evn++;
}

/* 记录本轮刷新的第一个 SPI 错误;不覆盖已有错误。 */
static void err_record(esp_err_t err)
{
    if (err != ESP_OK && s_draw_err <= 0)
        s_draw_err = (int)err;
}

/* 刷新序列中任一命令失败即短路返回,不再空等 BUSY。
 * 返回累计错误;s_draw_err 意外为 0 时退回本次的错误码,保证非 0。 */
#define EPD_CHECK(expr)                                                          \
    do {                                                                         \
        esp_err_t _e = (expr);                                                   \
        if (_e != ESP_OK)                                                        \
            return s_draw_err ? s_draw_err : (int)_e;                            \
    } while (0)

/* 原厂 pre_cb: user=0 命令 / 1 数据 / 2 读事务(自行控制 DC)。 */
static void IRAM_ATTR epd_pre_cb(spi_transaction_t *t)
{
    int u = (int)(intptr_t)t->user;
    if (u == 2)
        return;
    gpio_set_level(EPD_PIN_DC, u);
}

/* 发送一段数据;返回 SPI 错误并记入本轮错误(DESIGN §4.2)。 */
static esp_err_t epd_send(const uint8_t *data, int len, int dc)
{
    if (!s_spi) {
        err_record(ESP_ERR_INVALID_STATE);
        return ESP_ERR_INVALID_STATE;
    }
    gpio_set_level(EPD_PIN_CS, 0);
    spi_transaction_t t = { 0 };
    t.length = (size_t)len * 8;
    t.tx_buffer = data;
    t.user = (void *)(intptr_t)dc;
    esp_err_t err = spi_device_polling_transmit(s_spi, &t);
    gpio_set_level(EPD_PIN_CS, 1);
    err_record(err);
    return err;
}

static esp_err_t wcmd(uint8_t c) { return epd_send(&c, 1, 0); }

static esp_err_t wdata(const void *d, int len) { return epd_send((const uint8_t *)d, len, 1); }

/* 单字节数据写（对齐原厂 epd_spi_write_data，每字节独立拉一次 CS）。 */
static esp_err_t wdata_byte(uint8_t b) { return epd_send(&b, 1, 1); }

/* 逐字节发送控制寄存器参数或波形表（100% 复刻原厂 OED 总线波形）。 */
static esp_err_t wdata_each(const uint8_t *d, int len)
{
    for (int i = 0; i < len; i++) {
        esp_err_t err = epd_send(&d[i], 1, 1);
        if (err != ESP_OK)
            return err;
    }
    return ESP_OK;
}

/* 等 BUSY 到达指定电平, 记录起始/结束电平与耗时。
 * OED (JD79653) 路径依据原厂 E2 1.3.759 / E2S 1.3.739 反汇编为 0=忙、1=空闲;
 * 超时只告警并继续,不当作刷新失败。 */
static int wait_level(int level, int timeout_ms, const char *tag)
{
    int start = gpio_get_level(EPD_PIN_BUSY);
    int64_t t0 = esp_timer_get_time();
    int t = 0;
    while (gpio_get_level(EPD_PIN_BUSY) != level && t < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(1));
        t++;
    }
    uint32_t ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    int end = gpio_get_level(EPD_PIN_BUSY);
    ev_add(tag, start, end, 0, ms);
    if (end != level)
        ESP_LOGW(TAG, "%s wait %d timeout (start=%d end=%d %ums)", tag, level, start, end,
                 (unsigned)ms);
    return end == level ? 0 : -1;
}

/* OED 严格空闲等待（BUSY_N 低=忙、高=空闲）：
 * 连续 10 ms 为高电平才认为刷新与自动断电彻底结束，期间变低即重置计数。 */
static int wait_oed_idle_ms(int timeout_ms, const char *tag, int *saw_busy, int *timed_out)
{
    int start = gpio_get_level(EPD_PIN_BUSY);
    int64_t t0 = esp_timer_get_time();
    int t = 0, stable = 0, busy_seen = (start == 0) ? 1 : 0;
    if (timed_out)
        *timed_out = 0;

    while (t < timeout_ms) {
        int lv = gpio_get_level(EPD_PIN_BUSY);
        if (lv == 0) {
            busy_seen = 1;
            stable = 0;
        } else if (++stable >= 10) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
        t++;
    }
    if (stable < 10) {
        if (timed_out)
            *timed_out = 1;
        ESP_LOGW(TAG, "%s OED BUSY still low after %d ms", tag, t);
    }
    uint32_t ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    int end = gpio_get_level(EPD_PIN_BUSY);
    ev_add(tag, start, end, busy_seen, ms);
    if (saw_busy)
        *saw_busy = busy_seen;
    return (int)ms;
}

/* 严格空闲等待(真机实测: BUSY 高=忙、低=空闲)。
 * BOE 全刷 0x20 后约 1.27 s 回低,局刷约 454 ms 回低。这里 1 ms 轮询,
 * 只有 BUSY 连续 10 ms 为低才算完成;期间任何一次变高都重置稳定计数并
 * 继续等(否则 0x20 拉高稍有延迟就会在 10 ms 内误判完成,提前写 0x26)。
 * saw_busy 输出是否见过忙电平:局刷用它判断"面板完全没反应",从而补 settle。
 * 超时只告警、不算刷新失败;timed_out 非 NULL 时输出是否超时。 */
static int wait_idle_ms(int timeout_ms, const char *tag, int *saw_busy, int *timed_out)
{
    int start = gpio_get_level(EPD_PIN_BUSY);
    int64_t t0 = esp_timer_get_time();
    int t = 0, stable = 0, busy_seen = (start != 0) ? 1 : 0;
    if (timed_out)
        *timed_out = 0;

    while (t < timeout_ms) {
        int lv = gpio_get_level(EPD_PIN_BUSY);
        if (lv != 0) {
            busy_seen = 1;
            stable = 0;
        } else if (++stable >= 10) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
        t++;
    }
    if (stable < 10) {
        if (timed_out)
            *timed_out = 1;
        ESP_LOGW(TAG, "%s BUSY still busy after %d ms", tag, t);
    }
    uint32_t ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    int end = gpio_get_level(EPD_PIN_BUSY);
    ev_add(tag, start, end, busy_seen, ms);
    if (saw_busy)
        *saw_busy = busy_seen;
    return (int)ms;
}

/* 原厂 0x420298c8: RST 低 11 tick -> 高 11 tick；可按场景传入脉冲宽度。 */
static void hw_reset_ms(int ms)
{
    gpio_set_level(EPD_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(ms));
    gpio_set_level(EPD_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(ms));
}

/* 0x70 + 5 字节 ID; 原厂读事务 length=0, rxlength=40。
 * 仅用于 epd_init() 面板识别诊断日志，不污染刷新期 s_draw_err。 */
static void panel_read_id(uint8_t *buf)
{
    uint8_t cmd = 0x70;
    gpio_set_level(EPD_PIN_DC, 0);
    gpio_set_level(EPD_PIN_CS, 0);
    spi_transaction_t t = { 0 };
    t.length = 8;
    t.tx_buffer = &cmd;
    t.user = (void *)(intptr_t)2;
    esp_err_t e1 = spi_device_polling_transmit(s_spi, &t);
    gpio_set_level(EPD_PIN_DC, 1);
    memset(buf, 0, 5);
    spi_transaction_t r = { 0 };
    r.length = 0;
    r.rxlength = 5 * 8;
    r.rx_buffer = buf;
    r.user = (void *)(intptr_t)2;
    esp_err_t e2 = spi_device_polling_transmit(s_spi, &r);
    gpio_set_level(EPD_PIN_CS, 1);
    s_rd_err = (e1 != ESP_OK) ? e1 : (e2 != ESP_OK ? e2 : 0);
    ESP_LOGI(TAG, "epd id %02x %02x %02x %02x %02x", buf[0], buf[1], buf[2], buf[3], buf[4]);
}

/* 1bpp 帧 -> 面板 RAM 字节序 (每行 16B)。实机 r25 显示上下翻转,
 * 说明面板 X 扫描方向是从下往上, 这里把 y 取反。 */
static void build_fb(const uint8_t *img, uint8_t *fb)
{
    const int row_bytes = (EPD_W + 7) / 8;
    const int ram_x = (EPD_H + 7) / 8;
    const int ram_y = EPD_W;
    for (int y = 0; y < ram_y; y++) {
        uint8_t *line = fb + y * ram_x;
        memset(line, 0, ram_x);
        for (int u = 0; u < EPD_H; u++) {
            int px = y;
            int py = EPD_H - 1 - u;
            uint8_t v = img[py * row_bytes + px / 8] & (0x80 >> (px & 7));
            if (v) line[u / 8] |= 0x80 >> (u & 7);
        }
    }
}

static int set_full_window(void)
{
    const int ram_x = (EPD_H + 7) / 8;
    const int ram_y = EPD_W;
    EPD_CHECK(wcmd(0x44));
    { uint8_t xr[2] = { 0x00, (uint8_t)(ram_x - 1) }; EPD_CHECK(wdata(xr, 2)); }
    EPD_CHECK(wcmd(0x45));
    { uint8_t yr[4] = { 0x00, 0x00, (uint8_t)((ram_y - 1) & 0xFF), 0x00 }; EPD_CHECK(wdata(yr, 4)); }
    EPD_CHECK(wcmd(0x4e));
    { uint8_t z = 0; EPD_CHECK(wdata(&z, 1)); }
    EPD_CHECK(wcmd(0x4f));
    { uint8_t z[2] = { 0, 0 }; EPD_CHECK(wdata(z, 2)); }
    return 0;
}

/* 按原厂 OED (JD79653) 序列逐字节写入完整 56 字节波形表（含首字节 0x01）。 */
static int write_lut56(uint8_t cmd, const uint8_t lut[56])
{
    EPD_CHECK(wcmd(cmd));
    EPD_CHECK(wdata_each(lut, 56));
    return 0;
}

/* OED "OED is all"（严格对齐原厂 E2 1.3.759 @0x42024368 / E2S 1.3.739 @0x4202a424）：
 *   1) 硬件复位 + 等 BUSY_N==1 空闲；
 *   2) 逐字节写 JD79653 全套初始化寄存器（0x00/01/03/06/30/60/82/E3/41/61/65/50/50）；
 *   3) 按原厂顺序写入 5 张 56 字节全刷 LUT（0x20/0x21/0x24/0x22/0x23）；
 *   4) 等 BUSY_N==1 -> 仅向 0x13 (DTM2) 连续写入 4000 字节帧缓冲 -> 等 BUSY_N==1；
 *   5) 发 0x17 0xA5 触发自动升压刷新断电序列 -> 等 BUSY_N==0 确认开始刷新 -> 发 0x41 0x07
 *      -> 等 BUSY_N 连续 10 ms 回高确认刷新完成（若未见到低电平则按原厂 800 ms 兜底）
 *      -> 补发 0x41 0x07（对齐原厂 sm_sleep @0x42029d34）。
 * 返回 0 成功；非 0 为累计的 SPI 错误。 */
static int oed_full_refresh(const uint8_t *fb)
{
    static const uint8_t p00[] = { 0xff, 0x89 };
    static const uint8_t p01[] = { 0x03, 0x00, 0x3f, 0x3f, 0x03 };
    static const uint8_t p06[] = { 0x27, 0x27, 0x2f };
    static const uint8_t p61[] = { 0x80, 0x00, 0xfa };
    static const uint8_t p65[] = { 0x00, 0x00, 0x00 };
    const int n = EPD_W * ((EPD_H + 7) / 8);

    hw_reset_ms(20);
    wait_level(1, 2500, "o0");       /* 原厂 0x4202a448: 复位后等 47==1 */

    EPD_CHECK(wcmd(0x00)); EPD_CHECK(wdata_each(p00, (int)sizeof(p00)));
    EPD_CHECK(wcmd(0x01)); EPD_CHECK(wdata_each(p01, (int)sizeof(p01)));
    EPD_CHECK(wcmd(0x03)); EPD_CHECK(wdata_byte(0x00));
    EPD_CHECK(wcmd(0x06)); EPD_CHECK(wdata_each(p06, (int)sizeof(p06)));
    EPD_CHECK(wcmd(0x30)); EPD_CHECK(wdata_byte(0x09));
    EPD_CHECK(wcmd(0x60)); EPD_CHECK(wdata_byte(0x22));
    EPD_CHECK(wcmd(0x82)); EPD_CHECK(wdata_byte(0x00));
    EPD_CHECK(wcmd(0xe3)); EPD_CHECK(wdata_byte(0x00));
    EPD_CHECK(wcmd(0x41)); EPD_CHECK(wdata_byte(0x00));
    EPD_CHECK(wcmd(0x61)); EPD_CHECK(wdata_each(p61, (int)sizeof(p61)));
    EPD_CHECK(wcmd(0x65)); EPD_CHECK(wdata_each(p65, (int)sizeof(p65)));
    EPD_CHECK(wcmd(0x50)); EPD_CHECK(wdata_byte(0xb7));
    EPD_CHECK(wcmd(0x50)); EPD_CHECK(wdata_byte(0xb7));
    EPD_CHECK(write_lut56(0x20, s_oed_full_lut20));
    EPD_CHECK(write_lut56(0x21, s_oed_full_lut21));
    EPD_CHECK(write_lut56(0x24, s_oed_full_lut24));
    EPD_CHECK(write_lut56(0x22, s_oed_full_lut22));
    EPD_CHECK(write_lut56(0x23, s_oed_full_lut23));
    s_stage = 3;

    /* 原厂 0x4202a5b0..0x4202a5b9：仅写 0x13 (DTM2) 4000 字节，不发 SSD1680 窗口与 0x10。 */
    wait_level(1, 2500, "o1");
    EPD_CHECK(wcmd(0x13));
    EPD_CHECK(wdata(fb, n));
    s_written += (uint32_t)n;
    wait_level(1, 2500, "o2");

    /* 原厂 epd_hal_reflash_oed_213 (0x42029ac8)：发送 0x17 0xA5 自动刷新。 */
    EPD_CHECK(wcmd(0x17)); EPD_CHECK(wdata_byte(0xa5));
    s_stage = 4;

    /* 先确认 BUSY_N 拉低（0=忙），发 0x41 0x07 后等 BUSY_N 连续 10 ms 回高完成刷新。 */
    int started = (wait_level(0, 500, "o3") == 0);
    EPD_CHECK(wcmd(0x41)); EPD_CHECK(wdata_byte(0x07));
    s_stage = 5;

    int saw_busy = started;
    int timed_out = 0;
    wait_oed_idle_ms(5000, "o4", &saw_busy, &timed_out);
    if (!started && !saw_busy) {
        /* 面板未观测到忙电平（如忙脉冲极短或异常）：按原厂 800 ms 刷新保护窗兜底。 */
        ESP_LOGW(TAG, "o4: no BUSY low observed; fallback settle 800ms");
        vTaskDelay(pdMS_TO_TICKS(800));
    }
    EPD_CHECK(wcmd(0x41)); EPD_CHECK(wdata_byte(0x07));
    s_stage = 6;
    return s_draw_err;
}

/* BOE (0x42029ed4): SSD1680 风格全刷。返 0 成功,非 0 为累计 SPI 错误。 */
static int boe_refresh(const uint8_t *fb)
{
    static const uint8_t p01[] = { 0xfa, 0x00, 0x00 };
    static const uint8_t p04[] = { 0x41, 0xa8, 0x32 };
    const int n = EPD_W * ((EPD_H + 7) / 8);
    uint8_t d;

    EPD_CHECK(wcmd(0x12));
    wait_idle_ms(5000, "b0", NULL, NULL);
    EPD_CHECK(wcmd(0x01)); EPD_CHECK(wdata(p01, (int)sizeof(p01)));
    EPD_CHECK(wcmd(0x11)); d = 0x03; EPD_CHECK(wdata(&d, 1));
    EPD_CHECK(wcmd(0x21)); { uint8_t p[2] = { 0x00, 0x80 }; EPD_CHECK(wdata(p, 2)); }
    EPD_CHECK(wcmd(0x18)); d = 0x80; EPD_CHECK(wdata(&d, 1));
    EPD_CHECK(wcmd(0x3c)); d = 0x05; EPD_CHECK(wdata(&d, 1));
    EPD_CHECK(wcmd(0x3f)); d = 0x22; EPD_CHECK(wdata(&d, 1));
    EPD_CHECK(wcmd(0x03)); d = 0x17; EPD_CHECK(wdata(&d, 1));
    EPD_CHECK(wcmd(0x04)); EPD_CHECK(wdata(p04, (int)sizeof(p04)));
    EPD_CHECK(wcmd(0x2c)); d = 0x58; EPD_CHECK(wdata(&d, 1));
    EPD_CHECK(wcmd(0x32)); EPD_CHECK(wdata(s_lut153, (int)sizeof(s_lut153)));
    EPD_CHECK(set_full_window());
    wait_idle_ms(1500, "b1", NULL, NULL);

    /* 原厂 BOE 分支(0x4202a07d/0x4202a137): 帧写 0x24(B/W RAM) 和 0x26,
     * 不是 0x13 —— 0x13 只出现在 OED/UC8151 分支。 */
    EPD_CHECK(wcmd(0x24));
    EPD_CHECK(wdata(fb, n));
    EPD_CHECK(wcmd(0x26));
    EPD_CHECK(wdata(fb, n));
    s_written += (uint32_t)n * 2;
    wait_idle_ms(1500, "b2", NULL, NULL);

    EPD_CHECK(wcmd(0x22)); { uint8_t c = 0xc7; EPD_CHECK(wdata(&c, 1)); }
    EPD_CHECK(wcmd(0x20));
    s_stage = 8;
    int saw_busy = 0;
    wait_idle_ms(5000, "b3", &saw_busy, NULL);   /* 真机 0x20 后 1,266 ms 回低 */
    if (!saw_busy)
        ESP_LOGW(TAG, "b3: no BUSY observed during full refresh");
    return s_draw_err;
}

/* SSD1680 局部刷新: 0x24 = 当前帧, 0x22 0xFC + 0x20。
 * 更新完成后把同一帧写回 0x26, 使下一帧的差分基准等于刚显示的帧;
 * 否则 RAM2 一直停留在上次全刷的画面, 多帧之后会出现该刷不刷的残影。
 *
 * 0x26 的时机(真机实测):0x20 后 BUSY 拉高,约 454 ms 回低;必须等它连续
 * 10 ms 为低再写 0x26,否则回写会被波形驱动期间的面板忽略,造成残影。
 * 没观察到忙电平或 2 s 超时仍未回低(面板无反应)才补
 * EPD_PARTIAL_SETTLE_MS=500 ms 兜底。
 * 返 0 成功,非 0 为累计 SPI 错误(耗时由 epd_draw_ex 统计)。 */
static int boe_partial(const uint8_t *fb)
{
    const uint32_t n = EPD_W * ((EPD_H + 7) / 8);

    EPD_CHECK(set_full_window());
    EPD_CHECK(wcmd(0x24));
    EPD_CHECK(wdata(fb, (int)n));
    EPD_CHECK(wcmd(0x22)); { uint8_t c = 0xfc; EPD_CHECK(wdata(&c, 1)); }
    EPD_CHECK(wcmd(0x20));
    int saw_busy = 0, timed_out = 0;
    wait_idle_ms(2000, "p", &saw_busy, &timed_out);   /* 真机 0x20 后约 454 ms 回低 */
    if (timed_out || !saw_busy) {
        /* 面板没拉 BUSY 或 2 s 内没回空闲:补 settle 再写 0x26,避免回写被忽略 */
        ESP_LOGW(TAG, "partial settle %dms before 0x26 (busy=%d timeout=%d)",
                 EPD_PARTIAL_SETTLE_MS, saw_busy, timed_out);
        vTaskDelay(pdMS_TO_TICKS(EPD_PARTIAL_SETTLE_MS));
    }

    EPD_CHECK(set_full_window());
    EPD_CHECK(wcmd(0x26));
    EPD_CHECK(wdata(fb, (int)n));
    s_written += n * 2;
    return 0;
}

int epd_init(void)
{
    s_ok = 1;
    gpio_deep_sleep_hold_dis();
    gpio_hold_dis(EPD_PIN_CS);
    gpio_hold_dis(EPD_PIN_DC);
    gpio_hold_dis(EPD_PIN_RST);
    gpio_hold_dis(EPD_PIN_MOSI);
    gpio_hold_dis(EPD_PIN_SCLK);
    gpio_hold_dis(EPD_PIN_BUSY);
    s_stage = 0;

    /* 原厂 0x4202a828: 39/40/48 输出(不带输入通路), 47 输入上拉。 */
    gpio_config_t out = { .pin_bit_mask = (1ULL << EPD_PIN_CS) | (1ULL << EPD_PIN_DC) |
                                          (1ULL << EPD_PIN_RST),
                          .mode = GPIO_MODE_OUTPUT };
    gpio_config(&out);
    gpio_set_level(EPD_PIN_RST, 1);
    gpio_set_level(EPD_PIN_CS, 1);
    gpio_set_level(EPD_PIN_DC, 1);
    gpio_config_t in = { .pin_bit_mask = (1ULL << EPD_PIN_BUSY),
                         .mode = GPIO_MODE_INPUT,
                         .pull_up_en = GPIO_PULLUP_ENABLE };
    gpio_config(&in);

    spi_bus_config_t bus = { .mosi_io_num = EPD_PIN_MOSI, .miso_io_num = -1,
                             .sclk_io_num = EPD_PIN_SCLK,
                             .quadwp_io_num = -1, .quadhd_io_num = -1,
                             .max_transfer_sz = 4096 };
    esp_err_t err = spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi3 init %d", err);
        return -1;
    }
    spi_device_interface_config_t dev = { .clock_speed_hz = 10 * 1000 * 1000,
                                          .mode = 0, .spics_io_num = -1,
                                          .flags = SPI_DEVICE_3WIRE | SPI_DEVICE_HALFDUPLEX,
                                          .queue_size = 8,
                                          .pre_cb = epd_pre_cb };
    err = spi_bus_add_device(SPI3_HOST, &dev, &s_spi);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi3 dev %d", err);
        return -1;
    }

    /* 原厂 discern: 复位 -> 等 ~100ms -> 读 47 分族。 */
    hw_reset_ms(120);
    s_stage = 1;
    vTaskDelay(pdMS_TO_TICKS(100));
    s_pad47 = gpio_get_level(EPD_PIN_BUSY);
    if (s_pad47 == 0) {
        s_family = 1; /* BOE/GD (SSD1680), 原厂不读 ID */
    } else {
        panel_read_id(s_id);
        /* 读事务本身可能受 3 线半双工限制而不可靠; 47==1 时按 OED 处理。 */
        s_family = 3;
    }
    ESP_LOGW(TAG, "epd discern pad47=%d id=%02x%02x%02x%02x%02x family=%d",
             s_pad47, s_id[0], s_id[1], s_id[2], s_id[3], s_id[4], s_family);
    s_stage = 2;

    s_ok = 0;
    s_init_ok = 1;
    ESP_LOGI(TAG, "epd init ok");
    return 0;
}

/* img: 每行 row_bytes=(W+7)/8, 高位在前, 1=白 */
int epd_draw_ex(const uint8_t *img, int full)
{
    if (s_ok != 0)
        return -1;
    if (!img)
        return -1;
    static uint8_t fb[EPD_W * ((EPD_H + 7) / 8)];
    build_fb(img, fb);

    s_draw_err = 0;               /* 本轮刷新的错误累计从 0 开始 */
    int did_full = full || s_family != 1;
    int64_t t_draw = esp_timer_get_time();
    int err;
    if (did_full) {
        if (s_family == 1)
            err = boe_refresh(fb);
        else
            err = oed_full_refresh(fb);
    } else {
        err = boe_partial(fb);
    }
    if (err == 0)
        err = s_draw_err;
    s_draw_err = err;             /* 保留给 epd_diag */

    s_draw_ms = (uint32_t)((esp_timer_get_time() - t_draw) / 1000);
    if (err == 0) {
        if (did_full) {
            s_full_n++;
            s_full_ms = s_draw_ms;
        } else {
            s_partial_n++;
            s_partial_ms = s_draw_ms;
            s_partial_total += s_draw_ms;
        }
        ESP_LOGI(TAG, "draw %s family=%d %ums", did_full ? "full" : "partial", s_family,
                 (unsigned)s_draw_ms);
    } else {
        ESP_LOGE(TAG, "draw %s failed family=%d err=%d %ums", did_full ? "full" : "partial",
                 s_family, err, (unsigned)s_draw_ms);
    }
    return err;
}

int epd_draw(const uint8_t *img) { return epd_draw_ex(img, 1); }

int epd_draw_partial(const uint8_t *img) { return epd_draw_ex(img, 0); }

/* 面板族:1=BOE,3=OED/非 BOE,0=未知或初始化失败(DESIGN §4.5)。 */
int epd_family(void)
{
    return s_init_ok ? s_family : 0;
}

void epd_deep_sleep(void)
{
    if (!s_init_ok || !s_spi) {
        return;
    }
    if (s_family == 1) {
        /* BOE (SSD1680): 等 BUSY 空闲后发 0x10 0x01 进 Deep Sleep Mode 1 并保持画面。 */
        wait_idle_ms(1000, "slp", NULL, NULL);
        uint8_t d = 0x01;
        esp_err_t e1 = wcmd(0x10);
        esp_err_t e2 = wdata(&d, 1);
        ESP_LOGI(TAG, "epd deep sleep BOE err=%d/%d", (int)e1, (int)e2);
    } else if (s_family == 3) {
        /* OED (JD79653)：对齐原厂 sm_sleep(0x42029d31) + deep_sleep(0x4202ac0a)，
         * 先等 BUSY_N==1 空闲，写 0x41 0x07 再发 0x07 0xA5 进深睡并保持画面。 */
        wait_level(1, 1500, "oslp");
        esp_err_t e0 = wcmd(0x41);
        esp_err_t e0d = wdata_byte(0x07);
        esp_err_t e1 = wcmd(0x07);
        esp_err_t e2 = wdata_byte(0xa5);
        ESP_LOGI(TAG, "epd deep sleep OED err=%d/%d/%d/%d",
                 (int)e0, (int)e0d, (int)e1, (int)e2);
    }
    vTaskDelay(pdMS_TO_TICKS(20));

    /* 对齐原厂 epd_hal_spi_deinit：释放 SPI 总线，避免深睡期高电平驱动向面板倒灌电流。 */
    (void)spi_bus_remove_device(s_spi);
    s_spi = NULL;
    (void)spi_bus_free(SPI3_HOST);
    s_init_ok = 0;

    static const gpio_num_t epd_pins[] = {
        (gpio_num_t)EPD_PIN_CS,   (gpio_num_t)EPD_PIN_DC,
        (gpio_num_t)EPD_PIN_RST,  (gpio_num_t)EPD_PIN_BUSY,
        (gpio_num_t)EPD_PIN_MOSI, (gpio_num_t)EPD_PIN_SCLK,
    };
    for (size_t i = 0; i < sizeof(epd_pins) / sizeof(epd_pins[0]); i++) {
        (void)gpio_reset_pin(epd_pins[i]);
        (void)gpio_set_pull_mode(epd_pins[i], GPIO_FLOATING);
    }
}

void epd_diag(char *out, int n)
{
    int len = snprintf(out, n, "init=%d st=%d fam=%d p47=%d rd=%d draw=%d busy=%d wr=%u ms=%u temp=%04x id=%02x%02x%02x%02x%02x id2=%02x%02x%02x%02x%02x an=%u pm=%u/%u fm=%u fms=%u",
                       s_init_ok, s_stage, s_family, s_pad47, s_rd_err, s_draw_err,
                       gpio_get_level(EPD_PIN_BUSY), (unsigned)s_written, (unsigned)s_draw_ms, s_temp,
                       s_id[0], s_id[1], s_id[2], s_id[3], s_id[4],
                       s_id2[0], s_id2[1], s_id2[2], s_id2[3], s_id2[4],
                       (unsigned)s_partial_n, (unsigned)s_partial_ms,
                       s_partial_n ? (unsigned)(s_partial_total / s_partial_n) : 0,
                       (unsigned)s_full_n, (unsigned)s_full_ms);
    if (len < 0 || len >= n) return;
    len += snprintf(out + len, (size_t)(n - len), " ev=");
    for (int i = 0; i < s_evn && len > 0 && len < n; i++) {
        len += snprintf(out + len, (size_t)(n - len), "%s%s:%d>%d/%u",
                        i ? "," : "", s_ev[i].tag, s_ev[i].start, s_ev[i].end,
                        (unsigned)s_ev[i].ms);
    }
}
