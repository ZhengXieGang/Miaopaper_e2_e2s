/* sim_epd_regress.c — 本机 EPD 驱动协议回归测试（直接编译真实 epd.c）。
 *
 * 重点覆盖 Paperang E2 / E2S 两类面板（OED JD79653 与 BOE/GD SSD1680）的
 * SPI 命令流、波形表逐字节一致性、BUSY 极性与深睡释放，确保 E2 OED 屏
 * 不会再次发生漏 LUT / 错位 1 字节 / 误发 SSD1680 命令 / BUSY 死等 20 s 等黑屏缺陷。
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "epd.h"

/* ------------------------------------------------------------------ *
 * 虚拟硬件状态机与 SPI 事务记录
 * ------------------------------------------------------------------ */

#define MAX_TX_OPS 2048

typedef struct {
    uint8_t dc;       /* 0=命令, 1=数据 */
    uint8_t is_rx;    /* 0=写, 1=读 */
    uint32_t len;     /* 字节数 */
    uint8_t first;    /* 首字节（写事务） */
    uint8_t last;     /* 末字节（写事务） */
    uint8_t data[64]; /* 短包完整载荷（<=64 B） */
} tx_op_t;

typedef enum {
    PANEL_OED_NORMAL = 0,     /* 0x17 0xA5 后延迟 3 ms 拉低 BUSY，再持续 25 ms 后回高 */
    PANEL_OED_INSTANT_BUSY,   /* 0x17 0xA5 传输当下 BUSY 已立即变低，持续 25 ms 后回高 */
    PANEL_OED_NO_BUSY,        /* 异常面板：0x17 0xA5 后 BUSY 始终为 1（测 800 ms 兜底） */
    PANEL_BOE_NORMAL,         /* BOE (SSD1680): 复位后 BUSY=0，0x20 后拉高 25 ms 回低 */
} panel_model_t;

static panel_model_t s_model = PANEL_OED_NORMAL;
static int64_t s_now_us;
static int s_gpio_out[64];
static int s_busy_level;
static int64_t s_busy_drop_us = -1; /* OED: 从该时刻起 BUSY=0 */
static int64_t s_busy_rise_us = -1; /* OED: 从该时刻起 BUSY=1 */
static int64_t s_boe_idle_us = -1;  /* BOE: 从该时刻起 BUSY=0 */
static uint8_t s_last_cmd;
static transaction_cb_t s_pre_cb;
static int s_spi_bus_open;
static int s_spi_dev_open;
static int s_cs_pulses;

static tx_op_t s_ops[MAX_TX_OPS];
static int s_op_n;

static void hw_reset_model(panel_model_t m)
{
    s_model = m;
    s_now_us = 0;
    memset(s_gpio_out, 0, sizeof(s_gpio_out));
    s_busy_level = (m == PANEL_BOE_NORMAL) ? 0 : 1;
    s_busy_drop_us = -1;
    s_busy_rise_us = -1;
    s_boe_idle_us = -1;
    s_last_cmd = 0;
    s_pre_cb = NULL;
    s_spi_bus_open = 0;
    s_spi_dev_open = 0;
    s_cs_pulses = 0;
    s_op_n = 0;
}

static void hw_clear_ops(void)
{
    s_op_n = 0;
    s_cs_pulses = 0;
}

/* ------------------------------------------------------------------ *
 * ESP-IDF 桩实现
 * ------------------------------------------------------------------ */

void sim_hal_log(char level, const char *tag, const char *fmt, ...)
{
    (void)level;
    (void)tag;
    (void)fmt;
}

int64_t esp_timer_get_time(void) { return s_now_us; }

void vTaskDelay(TickType_t ticks)
{
    s_now_us += (int64_t)ticks * 1000LL;
}

esp_err_t gpio_config(const gpio_config_t *cfg)
{
    (void)cfg;
    return ESP_OK;
}
esp_err_t gpio_reset_pin(gpio_num_t n) { (void)n; return ESP_OK; }
esp_err_t gpio_set_pull_mode(gpio_num_t n, gpio_pull_mode_t p)
{
    (void)n;
    (void)p;
    return ESP_OK;
}
esp_err_t gpio_set_direction(gpio_num_t n, gpio_mode_t m)
{
    (void)n;
    (void)m;
    return ESP_OK;
}
esp_err_t gpio_hold_en(gpio_num_t n) { (void)n; return ESP_OK; }
esp_err_t gpio_hold_dis(gpio_num_t n) { (void)n; return ESP_OK; }
void gpio_deep_sleep_hold_en(void) {}
void gpio_deep_sleep_hold_dis(void) {}

esp_err_t gpio_set_level(gpio_num_t n, uint32_t level)
{
    if (n >= 0 && n < 64) {
        if (n == EPD_PIN_CS && s_gpio_out[n] == 0 && level != 0) {
            s_cs_pulses++;
        }
        s_gpio_out[n] = level ? 1 : 0;
    }
    return ESP_OK;
}

int gpio_get_level(gpio_num_t n)
{
    if (n != EPD_PIN_BUSY) {
        return (n >= 0 && n < 64) ? s_gpio_out[n] : 0;
    }
    if (s_model == PANEL_BOE_NORMAL) {
        if (s_boe_idle_us >= 0 && s_now_us >= s_boe_idle_us) {
            s_busy_level = 0;
            s_boe_idle_us = -1;
        }
        return s_busy_level;
    }
    if (s_busy_drop_us >= 0 && s_now_us >= s_busy_drop_us) {
        s_busy_level = 0;
        s_busy_drop_us = -1;
    }
    if (s_busy_rise_us >= 0 && s_now_us >= s_busy_rise_us) {
        s_busy_level = 1;
        s_busy_rise_us = -1;
    }
    return s_busy_level;
}

esp_err_t spi_bus_initialize(spi_host_device_t host_id,
                             const spi_bus_config_t *bus_config,
                             int dma_chan)
{
    (void)host_id;
    (void)bus_config;
    (void)dma_chan;
    s_spi_bus_open = 1;
    return ESP_OK;
}

esp_err_t spi_bus_free(spi_host_device_t host_id)
{
    (void)host_id;
    s_spi_bus_open = 0;
    return ESP_OK;
}

esp_err_t spi_bus_add_device(spi_host_device_t host_id,
                             const spi_device_interface_config_t *dev_config,
                             spi_device_handle_t *handle)
{
    (void)host_id;
    s_pre_cb = dev_config ? dev_config->pre_cb : NULL;
    s_spi_dev_open = 1;
    *handle = (spi_device_handle_t)(uintptr_t)0x1234;
    return ESP_OK;
}

esp_err_t spi_bus_remove_device(spi_device_handle_t handle)
{
    (void)handle;
    s_spi_dev_open = 0;
    return ESP_OK;
}

esp_err_t spi_device_polling_transmit(spi_device_handle_t handle,
                                      spi_transaction_t *t)
{
    (void)handle;
    if (s_pre_cb) {
        s_pre_cb(t);
    }
    uint8_t dc = (uint8_t)(s_gpio_out[EPD_PIN_DC] ? 1 : 0);

    if (t->rxlength > 0 && t->rx_buffer) {
        uint32_t rx_n = (uint32_t)(t->rxlength / 8);
        uint8_t *dst = (uint8_t *)t->rx_buffer;
        memset(dst, 0, rx_n);
        if (s_last_cmd == 0x70 && rx_n >= 5 && s_model != PANEL_BOE_NORMAL) {
            /* 模拟原厂 OED 2.13" 用户 ID（id[2] == 0x05）。 */
            dst[0] = 0x11;
            dst[1] = 0x22;
            dst[2] = 0x05;
            dst[3] = 0x00;
            dst[4] = 0x00;
        }
        if (s_op_n < MAX_TX_OPS) {
            tx_op_t *op = &s_ops[s_op_n++];
            memset(op, 0, sizeof(*op));
            op->dc = dc;
            op->is_rx = 1;
            op->len = rx_n;
        }
        return ESP_OK;
    }

    uint32_t tx_n = (uint32_t)(t->length / 8);
    const uint8_t *src = (const uint8_t *)t->tx_buffer;
    if (s_op_n < MAX_TX_OPS) {
        tx_op_t *op = &s_ops[s_op_n++];
        memset(op, 0, sizeof(*op));
        op->dc = dc;
        op->is_rx = 0;
        op->len = tx_n;
        if (tx_n > 0 && src) {
            op->first = src[0];
            op->last = src[tx_n - 1];
            uint32_t cpy = tx_n < sizeof(op->data) ? tx_n : (uint32_t)sizeof(op->data);
            memcpy(op->data, src, cpy);
        }
    }

    if (dc == 0 && tx_n == 1 && src) {
        s_last_cmd = src[0];
        if (s_model == PANEL_BOE_NORMAL && src[0] == 0x20) {
            s_busy_level = 1;
            s_boe_idle_us = s_now_us + 25000LL;
        }
    } else if (dc == 1 && tx_n == 1 && src) {
        if (s_last_cmd == 0x17 && src[0] == 0xa5) {
            if (s_model == PANEL_OED_NORMAL) {
                s_busy_drop_us = s_now_us + 3000LL;
                s_busy_rise_us = s_now_us + 28000LL;
            } else if (s_model == PANEL_OED_INSTANT_BUSY) {
                /* 模拟芯片瞬间拉低 BUSY_N：在 wdata_byte(0xA5) 返回前就已变 0。 */
                s_busy_level = 0;
                s_busy_drop_us = -1;
                s_busy_rise_us = s_now_us + 25000LL;
            }
        }
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ *
 * 断言辅助
 * ------------------------------------------------------------------ */

static int s_fail;

#define EXPECT(cond, msg, ...)                                                     \
    do {                                                                           \
        if (!(cond)) {                                                             \
            fprintf(stderr, "FAIL [%s:%d]: " msg "\n", __FILE__, __LINE__,         \
                    ##__VA_ARGS__);                                                \
            s_fail++;                                                              \
            return;                                                                \
        }                                                                          \
    } while (0)

/* 按顺序消费一个命令及紧随其后的逐字节参数（每字节独立一次 SPI 写）。 */
static void expect_cmd_bytes(int *pos, uint8_t cmd, const uint8_t *want, int n)
{
    if (s_fail) return;
    EXPECT(*pos < s_op_n, "命令 0x%02x 越界 (pos=%d/%d)", cmd, *pos, s_op_n);
    EXPECT(s_ops[*pos].dc == 0 && s_ops[*pos].len == 1 && s_ops[*pos].first == cmd,
           "期望命令 0x%02x，实际 dc=%u len=%u first=0x%02x @op[%d]",
           cmd, s_ops[*pos].dc, s_ops[*pos].len, s_ops[*pos].first, *pos);
    (*pos)++;
    for (int i = 0; i < n; i++) {
        EXPECT(*pos < s_op_n, "命令 0x%02x 参数[%d] 越界", cmd, i);
        EXPECT(s_ops[*pos].dc == 1 && s_ops[*pos].len == 1 &&
                   s_ops[*pos].first == want[i],
               "命令 0x%02x 参数[%d] 期望 0x%02x，实际 dc=%u len=%u val=0x%02x @op[%d]",
               cmd, i, want[i], s_ops[*pos].dc, s_ops[*pos].len,
               s_ops[*pos].first, *pos);
        (*pos)++;
    }
}

static void expect_lut56(int *pos, uint8_t cmd, uint8_t b1)
{
    uint8_t lut[56];
    memset(lut, 0, sizeof(lut));
    lut[0] = 0x01;
    lut[1] = b1;
    lut[2] = 0x14;
    lut[3] = 0x14;
    lut[4] = 0x01;
    lut[5] = 0x01;
    lut[6] = 0x00;
    lut[7] = 0x01;
    expect_cmd_bytes(pos, cmd, lut, 56);
}

/* ------------------------------------------------------------------ *
 * 回归测试用例 E1..E6
 * ------------------------------------------------------------------ */

static void test_e1_e2_oed_init_and_full_sequence(void)
{
    hw_reset_model(PANEL_OED_NORMAL);
    EXPECT(epd_init() == 0, "epd_init(OED) 失败");
    EXPECT(epd_family() == 3, "OED family 应为 3，实际 %d", epd_family());

    /* 确认识别阶段发出了 0x70 读 5 字节 ID，且没污染刷屏状态。 */
    EXPECT(s_op_n == 2 && s_ops[0].dc == 0 && s_ops[0].first == 0x70 &&
               s_ops[1].is_rx == 1 && s_ops[1].len == 5,
           "OED discern 读 ID 序列异常 (op_n=%d)", s_op_n);

    hw_clear_ops();
    static uint8_t img[EPD_W * ((EPD_H + 7) / 8)];
    memset(img, 0xFF, sizeof(img));
    int64_t t0 = s_now_us;
    int rc = epd_draw_partial(img); /* OED 上局刷请求应走全刷路径 */
    int64_t elapsed_ms = (s_now_us - t0) / 1000LL;
    EXPECT(rc == 0, "epd_draw_partial(OED) 返回错误 %d", rc);
    EXPECT(elapsed_ms < 500, "OED 正常全刷虚拟耗时过长: %lld ms", (long long)elapsed_ms);

    /* 逐条逐字节核对原厂 E2 1.3.759 / E2S 1.3.739 "OED is all" 命令序列。 */
    int p = 0;
    const uint8_t p00[] = { 0xff, 0x89 };
    const uint8_t p01[] = { 0x03, 0x00, 0x3f, 0x3f, 0x03 };
    const uint8_t p03[] = { 0x00 };
    const uint8_t p06[] = { 0x27, 0x27, 0x2f };
    const uint8_t p30[] = { 0x09 };
    const uint8_t p60[] = { 0x22 };
    const uint8_t p82[] = { 0x00 };
    const uint8_t pe3[] = { 0x00 };
    const uint8_t p41_0[] = { 0x00 };
    const uint8_t p61[] = { 0x80, 0x00, 0xfa };
    const uint8_t p65[] = { 0x00, 0x00, 0x00 };
    const uint8_t p50[] = { 0xb7 };
    const uint8_t p17[] = { 0xa5 };
    const uint8_t p41_7[] = { 0x07 };

    expect_cmd_bytes(&p, 0x00, p00, 2);
    expect_cmd_bytes(&p, 0x01, p01, 5);
    expect_cmd_bytes(&p, 0x03, p03, 1);
    expect_cmd_bytes(&p, 0x06, p06, 3);
    expect_cmd_bytes(&p, 0x30, p30, 1);
    expect_cmd_bytes(&p, 0x60, p60, 1);
    expect_cmd_bytes(&p, 0x82, p82, 1);
    expect_cmd_bytes(&p, 0xe3, pe3, 1);
    expect_cmd_bytes(&p, 0x41, p41_0, 1);
    expect_cmd_bytes(&p, 0x61, p61, 3);
    expect_cmd_bytes(&p, 0x65, p65, 3);
    expect_cmd_bytes(&p, 0x50, p50, 1);
    expect_cmd_bytes(&p, 0x50, p50, 1);

    /* 5 张 56B 全刷 LUT 严格按原厂顺序 0x20 -> 0x21 -> 0x24 -> 0x22 -> 0x23。 */
    expect_lut56(&p, 0x20, 0x00);
    expect_lut56(&p, 0x21, 0x60);
    expect_lut56(&p, 0x24, 0x90);
    expect_lut56(&p, 0x22, 0x60);
    expect_lut56(&p, 0x23, 0x90);
    if (s_fail) return;

    /* 紧接仅发 0x13 + 单次 4000 B 连续帧数据（不含 SSD1680 窗口 0x44/45/4E/4F，不含 0x10）。 */
    EXPECT(p + 1 < s_op_n, "缺少 0x13 帧写入");
    EXPECT(s_ops[p].dc == 0 && s_ops[p].len == 1 && s_ops[p].first == 0x13,
           "期望 0x13，实际 dc=%u first=0x%02x", s_ops[p].dc, s_ops[p].first);
    p++;
    /* EPD_H=122 占每行 16 字节中的前 122 位，末字节仅高 2 位有效，故全白帧末字节为 0xC0。 */
    EXPECT(s_ops[p].dc == 1 && s_ops[p].len == 4000 && s_ops[p].first == 0xFF &&
               s_ops[p].last == 0xC0,
           "0x13 帧数据异常: dc=%u len=%u first=0x%02x last=0x%02x",
           s_ops[p].dc, s_ops[p].len, s_ops[p].first, s_ops[p].last);
    p++;

    /* 触发刷新 0x17 0xA5 -> 等 BUSY_N=0 -> 0x41 0x07 -> 等 BUSY_N=1 完成 -> 0x41 0x07。 */
    expect_cmd_bytes(&p, 0x17, p17, 1);
    expect_cmd_bytes(&p, 0x41, p41_7, 1);
    expect_cmd_bytes(&p, 0x41, p41_7, 1);
    if (s_fail) return;

    EXPECT(p == s_op_n, "OED 全刷末尾出现多余 SPI 事务 (%d vs %d)", p, s_op_n);
    printf("PASS E1/E2: OED discern + full refresh exact stock sequence (%d ops, %lld ms)\n",
           s_op_n, (long long)elapsed_ms);
}

static void test_e3_oed_instant_busy_no_20s_hang(void)
{
    hw_reset_model(PANEL_OED_INSTANT_BUSY);
    EXPECT(epd_init() == 0, "epd_init(OED instant) 失败");
    hw_clear_ops();

    static uint8_t img[EPD_W * ((EPD_H + 7) / 8)];
    memset(img, 0xAA, sizeof(img));
    int64_t t0 = s_now_us;
    int rc = epd_draw(img);
    int64_t elapsed_ms = (s_now_us - t0) / 1000LL;
    EXPECT(rc == 0, "OED 瞬时忙态刷新失败 %d", rc);
    EXPECT(elapsed_ms < 200, "OED 瞬时拉低 BUSY 不应死等 20s，实际 %lld ms",
           (long long)elapsed_ms);
    printf("PASS E3: OED instant BUSY low completed in %lld ms (no 20s hang)\n",
           (long long)elapsed_ms);
}

static void test_e4_oed_no_busy_800ms_fallback(void)
{
    hw_reset_model(PANEL_OED_NO_BUSY);
    EXPECT(epd_init() == 0, "epd_init(OED no-busy) 失败");
    hw_clear_ops();

    static uint8_t img[EPD_W * ((EPD_H + 7) / 8)];
    memset(img, 0x55, sizeof(img));
    int64_t t0 = s_now_us;
    int rc = epd_draw(img);
    int64_t elapsed_ms = (s_now_us - t0) / 1000LL;
    EXPECT(rc == 0, "OED 无忙信号兜底刷新失败 %d", rc);
    /* 500 ms 等 0 超时 + 10 ms 稳态 + 800 ms 兜底保护窗 ≈ 1350 ms。 */
    EXPECT(elapsed_ms >= 1200 && elapsed_ms < 2000,
           "OED 无忙信号兜底耗时异常: %lld ms", (long long)elapsed_ms);
    printf("PASS E4: OED no-BUSY fallback settled in %lld ms\n", (long long)elapsed_ms);
}

static void test_e5_oed_deep_sleep(void)
{
    hw_reset_model(PANEL_OED_NORMAL);
    EXPECT(epd_init() == 0, "epd_init(OED sleep) 失败");
    hw_clear_ops();

    epd_deep_sleep();
    int p = 0;
    const uint8_t p41[] = { 0x07 };
    const uint8_t p07[] = { 0xa5 };
    expect_cmd_bytes(&p, 0x41, p41, 1);
    expect_cmd_bytes(&p, 0x07, p07, 1);
    if (s_fail) return;
    EXPECT(p == s_op_n, "OED 深睡多余事务 (%d vs %d)", p, s_op_n);
    EXPECT(s_spi_dev_open == 0 && s_spi_bus_open == 0, "OED 深睡未释放 SPI 总线");
    printf("PASS E5: OED deep sleep (0x41 0x07 + 0x07 0xA5 + SPI deinit)\n");
}

static void test_e6_boe_regression(void)
{
    hw_reset_model(PANEL_BOE_NORMAL);
    EXPECT(epd_init() == 0, "epd_init(BOE) 失败");
    EXPECT(epd_family() == 1, "BOE family 应为 1，实际 %d", epd_family());
    EXPECT(s_op_n == 0, "BOE discern 不应发 SPI 读 ID");

    static uint8_t img[EPD_W * ((EPD_H + 7) / 8)];
    memset(img, 0xFF, sizeof(img));
    hw_clear_ops();
    EXPECT(epd_draw(img) == 0, "BOE 全刷失败");
    /* 校验 0x32 写入 153 B LUT，且 0x24 与 0x26 各写 4000 B。 */
    int saw_lut153 = 0, saw_24 = 0, saw_26 = 0, saw_20 = 0;
    for (int i = 0; i < s_op_n; i++) {
        if (s_ops[i].dc == 0 && s_ops[i].first == 0x32 && i + 1 < s_op_n &&
            s_ops[i + 1].dc == 1 && s_ops[i + 1].len == 153)
            saw_lut153++;
        if (s_ops[i].dc == 0 && s_ops[i].first == 0x24 && i + 1 < s_op_n &&
            s_ops[i + 1].dc == 1 && s_ops[i + 1].len == 4000)
            saw_24++;
        if (s_ops[i].dc == 0 && s_ops[i].first == 0x26 && i + 1 < s_op_n &&
            s_ops[i + 1].dc == 1 && s_ops[i + 1].len == 4000)
            saw_26++;
        if (s_ops[i].dc == 0 && s_ops[i].first == 0x20)
            saw_20++;
    }
    EXPECT(saw_lut153 == 1 && saw_24 == 1 && saw_26 == 1 && saw_20 == 1,
           "BOE 全刷关键命令计数异常: lut=%d 24=%d 26=%d 20=%d",
           saw_lut153, saw_24, saw_26, saw_20);

    hw_clear_ops();
    EXPECT(epd_draw_partial(img) == 0, "BOE 局刷失败");
    hw_clear_ops();
    epd_deep_sleep();
    EXPECT(s_op_n == 2 && s_ops[0].dc == 0 && s_ops[0].first == 0x10 &&
               s_ops[1].dc == 1 && s_ops[1].first == 0x01,
           "BOE 深睡命令异常");
    EXPECT(s_spi_dev_open == 0 && s_spi_bus_open == 0, "BOE 深睡未释放 SPI 总线");
    printf("PASS E6: BOE full/partial/deep-sleep zero regression\n");
}

int main(void)
{
    test_e1_e2_oed_init_and_full_sequence();
    test_e3_oed_instant_busy_no_20s_hang();
    test_e4_oed_no_busy_800ms_fallback();
    test_e5_oed_deep_sleep();
    test_e6_boe_regression();
    if (s_fail) {
        fprintf(stderr, "sim_epd_regress: %d FAILED\n", s_fail);
        return 1;
    }
    printf("sim_epd_regress: ALL 6 EPD TESTS PASSED\n");
    return 0;
}
