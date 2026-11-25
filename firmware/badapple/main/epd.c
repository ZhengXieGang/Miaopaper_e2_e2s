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
 *     BOE(0x42029ed4): 0x12 -> 0x01 FA0000 -> 0x11 03 -> 0x21 0080 -> 0x18 80
 *          -> 0x3C 05 -> 0x3F 22 -> 0x03 17 -> 0x04 41A832 -> 0x2C 58
 *          -> 0x32 153B LUT -> 窗口 -> 0x24/0x26 帧 -> 0x22 C7 -> 0x20
 *     OED(0x4202a424 "OED is all"): 复位 -> 等 47==1 -> 0x00 FF89/01/03/06/30/60
 *          /82/E3/61/65/50 -> 0x20/24/22/23 各 55B LUT -> 等 47==1
 *          -> 0x13 帧 -> 等 47==1 -> 0x17 A5 -> 等 47==0 -> 0x41 07
 *
 * BUSY 极性随机型/命令阶段不同, 全部用"反应式等待": 等电平翻转再等回原电平,
 * 并把 (起始电平, 结束电平, 耗时) 放进 (1,241) 的 ev= 轨迹。
 */
#include "epd.h"
#include "epd_lut_stock.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

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

/* ---- 真机时序测量（仅 BOE） ---------------------------------------------
 * (1,243) 触发一次“全刷 + 局刷”，(1,244) 读回。严格等 BUSY 低电平，
 * 并把每次跳变记成 +毫秒:电平，用来确认极性、真实完成时间。
 * 正常回放路径不受影响（s_strict=0 时走原来的保守等待）。 */
typedef struct {
    int valid;          /* 0=未跑, 1=完成, -1=面板族不支持 */
    int fam;
    uint32_t full_total;
    uint32_t full_ph[4];    /* 0x12 / 配置 / 写帧 / 0x20 后到空闲 */
    uint8_t full_to;        /* 对应阶段超时位图 */
    uint8_t full_busy;      /* 对应阶段看到 BUSY=1 的位图 */
    uint32_t part_ms;
    uint8_t part_to;
    uint8_t part_busy;
    char full_ev[72];
    char part_ev[72];
} epd_tm_t;

static epd_tm_t s_tm;
static volatile int s_strict;
static char *s_ev_buf;
static int s_ev_cap, s_ev_pos;

static void ev_reset(char *buf, int cap)
{
    s_ev_buf = buf;
    s_ev_cap = cap;
    s_ev_pos = 0;
    if (cap > 0) buf[0] = 0;
}

static void ev_note(uint32_t ms, int lv)
{
    if (!s_ev_buf || s_ev_pos < 0) return;
    int room = s_ev_cap - s_ev_pos;
    if (room <= 12) { s_ev_pos = -1; return; }
    int n = snprintf(s_ev_buf + s_ev_pos, (size_t)room, "%s+%u:%d",
                     s_ev_pos ? "," : "", (unsigned)ms, lv);
    if (n <= 0 || n >= room) s_ev_pos = -1;
    else s_ev_pos += n;
}

/* 1 ms 轮询 BUSY，等到它变低并保持 10 ms。返回低电平出现的时刻（相对进入），
 * *timed_out=1 表示超时仍未低；*saw_busy=1 表示期间见过高电平。 */
static int wait_idle_strict(int timeout_ms, uint32_t *done_ms, int *timed_out, int *saw_busy)
{
    int64_t t0 = esp_timer_get_time();
    int last = gpio_get_level(EPD_PIN_BUSY);
    uint32_t stable = 0, low_at = 0;
    int saw_low = 0;
    *timed_out = 0;
    *saw_busy = 0;

    for (;;) {
        uint32_t ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
        if (ms >= (uint32_t)timeout_ms) { *timed_out = 1; break; }
        int lv = gpio_get_level(EPD_PIN_BUSY);
        if (lv != last) {
            ev_note(ms, lv);
            last = lv;
            stable = 0;
        }
        if (lv) {
            *saw_busy = 1;
            stable = 0;
        } else {
            if (stable == 0) { low_at = ms; saw_low = 1; }
            if (++stable >= 10) break;
        }
        vTaskDelay(1);
    }
    *done_ms = saw_low ? low_at : (uint32_t)((esp_timer_get_time() - t0) / 1000);
    return *timed_out ? -1 : 0;
}

/* 原厂 pre_cb: user=0 命令 / 1 数据 / 2 读事务(自行控制 DC)。 */
static void IRAM_ATTR epd_pre_cb(spi_transaction_t *t)
{
    int u = (int)(intptr_t)t->user;
    if (u == 2)
        return;
    gpio_set_level(EPD_PIN_DC, u);
}

static void epd_send(const uint8_t *data, int len, int dc)
{
    gpio_set_level(EPD_PIN_CS, 0);
    spi_transaction_t t = { 0 };
    t.length = len * 8;
    t.tx_buffer = data;
    t.user = (void *)(intptr_t)dc;
    spi_device_polling_transmit(s_spi, &t);
    gpio_set_level(EPD_PIN_CS, 1);
}

static void wcmd(uint8_t c) { epd_send(&c, 1, 0); }

static void wdata(const void *d, int len) { epd_send(d, len, 1); }

/* 等 BUSY 到达指定电平, 记录起始/结束电平与耗时。 */
static int wait_level(int level, int timeout_ms, const char *tag)
{
    int start = gpio_get_level(EPD_PIN_BUSY);
    int64_t t0 = esp_timer_get_time();
    int t = 0;
    while (gpio_get_level(EPD_PIN_BUSY) != level && t < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(5));
        t += 5;
    }
    uint32_t ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    int end = gpio_get_level(EPD_PIN_BUSY);
    ev_add(tag, start, end, 0, ms);
    if (end != level)
        ESP_LOGW(TAG, "%s wait %d timeout (start=%d end=%d %ums)", tag, level, start, end,
                 (unsigned)ms);
    return end == level ? 0 : -1;
}

/* 反应式等待: 等 BUSY 翻转再回到起始电平; 面板完全没反应则超时。 */
static int wait_reaction(int timeout_ms, const char *tag)
{
    int base = gpio_get_level(EPD_PIN_BUSY);
    int64_t t0 = esp_timer_get_time();
    int t = 0, changed = 0;
    while (t < timeout_ms) {
        if (gpio_get_level(EPD_PIN_BUSY) != base) { changed = 1; break; }
        vTaskDelay(pdMS_TO_TICKS(5));
        t += 5;
    }
    if (changed) {
        while (t < timeout_ms) {
            if (gpio_get_level(EPD_PIN_BUSY) == base) break;
            vTaskDelay(pdMS_TO_TICKS(5));
            t += 5;
        }
    }
    uint32_t ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    int end = gpio_get_level(EPD_PIN_BUSY);
    ev_add(tag, base, end, changed, ms);
    ESP_LOGI(TAG, "%s busy %d->%d %s %ums", tag, base, end, changed ? "reacted" : "no reaction",
             (unsigned)ms);
    return changed ? 0 : -1;
}

/* 局部刷新等待: BUSY 应先变忙再回就绪。起始只等 120ms, 面板不响应时不空等 20s。 */
static int wait_busy_cycle(int start_ms, int done_ms, const char *tag)
{
    int base = gpio_get_level(EPD_PIN_BUSY);
    int64_t t0 = esp_timer_get_time();
    int t = 0, changed = 0;
    while (t < start_ms) {
        if (gpio_get_level(EPD_PIN_BUSY) != base) { changed = 1; break; }
        vTaskDelay(pdMS_TO_TICKS(2));
        t += 2;
    }
    if (changed) {
        int limit = t + done_ms;
        while (t < limit) {
            if (gpio_get_level(EPD_PIN_BUSY) == base) break;
            vTaskDelay(pdMS_TO_TICKS(2));
            t += 2;
        }
        if (gpio_get_level(EPD_PIN_BUSY) != base)
            ESP_LOGW(TAG, "%s busy timeout", tag);
    } else {
        ESP_LOGW(TAG, "%s no BUSY reaction", tag);
    }
    return (int)((esp_timer_get_time() - t0) / 1000);
}

/* 原厂 0x420298c8: RST 低 11 tick -> 高 11 tick; 这里放宽到 120ms。 */
static void hw_reset(void)
{
    gpio_set_level(EPD_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(120));
    gpio_set_level(EPD_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
}

/* 0x70 + 5 字节 ID; 原厂读事务 length=0, rxlength=40。 */
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

static void panel_read_temp(void)
{
    uint8_t cmd = 0x40, t[2] = { 0, 0 };
    gpio_set_level(EPD_PIN_DC, 0);
    gpio_set_level(EPD_PIN_CS, 0);
    spi_transaction_t c = { 0 };
    c.length = 8;
    c.tx_buffer = &cmd;
    c.user = (void *)(intptr_t)2;
    spi_device_polling_transmit(s_spi, &c);
    gpio_set_level(EPD_PIN_DC, 1);
    spi_transaction_t r = { 0 };
    r.length = 0;
    r.rxlength = 2 * 8;
    r.rx_buffer = t;
    r.user = (void *)(intptr_t)2;
    spi_device_polling_transmit(s_spi, &r);
    gpio_set_level(EPD_PIN_CS, 1);
    s_temp = (t[0] << 8) | t[1];
    ESP_LOGI(TAG, "epd temp %02x %02x", t[0], t[1]);
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

static void set_full_window(void)
{
    const int ram_x = (EPD_H + 7) / 8;
    const int ram_y = EPD_W;
    wcmd(0x44);
    { uint8_t xr[2] = { 0x00, (uint8_t)(ram_x - 1) }; wdata(xr, 2); }
    wcmd(0x45);
    { uint8_t yr[4] = { 0x00, 0x00, (uint8_t)((ram_y - 1) & 0xFF), 0x00 }; wdata(yr, 4); }
    wcmd(0x4e);
    { uint8_t z = 0; wdata(&z, 1); }
    wcmd(0x4f);
    { uint8_t z[2] = { 0, 0 }; wdata(z, 2); }
}

static void write_lut55(uint8_t cmd, const uint8_t *lut)
{
    wcmd(cmd);
    wdata(lut, 55);
}

/* OED "OED is all" (0x4202a424): 复位 + UC8151 风格初始化 + 0x13 帧 + 0x17 A5。 */
static int oed_full_refresh(const uint8_t *fb)
{
    static const uint8_t p00[] = { 0xff, 0x89 };
    static const uint8_t p01[] = { 0x03, 0x00, 0x3f, 0x3f, 0x03 };
    static const uint8_t p06[] = { 0x27, 0x27, 0x2f };
    static const uint8_t p61[] = { 0x80, 0x00, 0xfa };
    static const uint8_t p65[] = { 0x00, 0x00, 0x00 };
    uint8_t d;

    hw_reset();
    wait_level(1, 2500, "o0");       /* 原厂: 复位后等 47==1 */

    wcmd(0x00); wdata(p00, sizeof(p00));
    wcmd(0x01); wdata(p01, sizeof(p01));
    wcmd(0x03); d = 0x00; wdata(&d, 1);
    wcmd(0x06); wdata(p06, sizeof(p06));
    wcmd(0x30); d = 0x09; wdata(&d, 1);
    wcmd(0x60); d = 0x22; wdata(&d, 1);
    wcmd(0x82); d = 0x00; wdata(&d, 1);
    wcmd(0xe3); d = 0x00; wdata(&d, 1);
    wcmd(0x41); d = 0x00; wdata(&d, 1);
    wcmd(0x61); wdata(p61, sizeof(p61));
    wcmd(0x65); wdata(p65, sizeof(p65));
    wcmd(0x50); d = 0xb7; wdata(&d, 1);
    wcmd(0x50); d = 0xb7; wdata(&d, 1);
    write_lut55(0x20, s_lut20);
    write_lut55(0x24, s_lut24);
    write_lut55(0x22, s_lut22);
    write_lut55(0x23, s_lut23);
    s_stage = 3;

    wait_level(1, 2500, "o1");
    set_full_window();
    /* UC8151: 0x10=DTM1(old), 0x13=DTM2(new)。两个 bank 都写本帧, 避免未初始化
     * 的 old-bank 让全刷驱动成全黑。 */
    wcmd(0x10);
    wdata(fb, EPD_W * ((EPD_H + 7) / 8));
    wcmd(0x13);
    wdata(fb, EPD_W * ((EPD_H + 7) / 8));
    s_written += EPD_W * ((EPD_H + 7) / 8) * 2;
    wait_level(1, 2500, "o2");

    wcmd(0x17); d = 0xa5; wdata(&d, 1);
    s_stage = 4;
    wait_reaction(20000, "o3");      /* 原厂: 等 47==0 */

    wcmd(0x41); d = 0x07; wdata(&d, 1);
    s_stage = 5;
    panel_read_temp();
    s_stage = 6;
    panel_read_id(s_id2);
    s_stage = 7;
    return 0;
}

/* BOE (0x42029ed4): SSD1680 风格全刷。 */
static int boe_refresh(const uint8_t *fb)
{
    static const uint8_t p01[] = { 0xfa, 0x00, 0x00 };
    static const uint8_t p04[] = { 0x41, 0xa8, 0x32 };
    uint8_t d;

    wcmd(0x12);
    wait_reaction(5000, "b0");
    wcmd(0x01); wdata(p01, sizeof(p01));
    wcmd(0x11); d = 0x03; wdata(&d, 1);
    wcmd(0x21); { uint8_t p[2] = { 0x00, 0x80 }; wdata(p, 2); }
    wcmd(0x18); d = 0x80; wdata(&d, 1);
    wcmd(0x3c); d = 0x05; wdata(&d, 1);
    wcmd(0x3f); d = 0x22; wdata(&d, 1);
    wcmd(0x03); d = 0x17; wdata(&d, 1);
    wcmd(0x04); wdata(p04, sizeof(p04));
    wcmd(0x2c); d = 0x58; wdata(&d, 1);
    wcmd(0x32); wdata(s_lut153, sizeof(s_lut153));
    set_full_window();
    wait_reaction(1500, "b1");

    /* 原厂 BOE 分支(0x4202a07d/0x4202a137): 帧写 0x24(B/W RAM) 和 0x26,
     * 不是 0x13 —— 0x13 只出现在 OED/UC8151 分支。 */
    wcmd(0x24);
    wdata(fb, EPD_W * ((EPD_H + 7) / 8));
    wcmd(0x26);
    wdata(fb, EPD_W * ((EPD_H + 7) / 8));
    s_written += EPD_W * ((EPD_H + 7) / 8) * 2;
    wait_reaction(1500, "b2");

    wcmd(0x22); { uint8_t c = 0xc7; wdata(&c, 1); }
    wcmd(0x20);
    s_stage = 8;
    wait_reaction(20000, "b3");
    return 0;
}

/* BOE 严格计时版全刷：每一步都等 BUSY 真正回到低电平，记录各阶段耗时。
 * 序列与 boe_refresh() 相同，只把“5 s/1.5 s/20 s 超时”换成“等到空闲”。 */
static int boe_refresh_timed(const uint8_t *fb)
{
    static const uint8_t p01[] = { 0xfa, 0x00, 0x00 };
    static const uint8_t p04[] = { 0x41, 0xa8, 0x32 };
    const uint32_t n = EPD_W * ((EPD_H + 7) / 8);
    uint8_t d;
    uint32_t done;
    int to, busy;
    int64_t t_start = esp_timer_get_time();

    wcmd(0x12);
    wait_idle_strict(30000, &done, &to, &busy);
    s_tm.full_ph[0] = done;
    if (to) s_tm.full_to |= 1u << 0;
    if (busy) s_tm.full_busy |= 1u << 0;

    wcmd(0x01); wdata(p01, sizeof(p01));
    wcmd(0x11); d = 0x03; wdata(&d, 1);
    wcmd(0x21); { uint8_t p[2] = { 0x00, 0x80 }; wdata(p, 2); }
    wcmd(0x18); d = 0x80; wdata(&d, 1);
    wcmd(0x3c); d = 0x05; wdata(&d, 1);
    wcmd(0x3f); d = 0x22; wdata(&d, 1);
    wcmd(0x03); d = 0x17; wdata(&d, 1);
    wcmd(0x04); wdata(p04, sizeof(p04));
    wcmd(0x2c); d = 0x58; wdata(&d, 1);
    wcmd(0x32); wdata(s_lut153, sizeof(s_lut153));
    set_full_window();
    wait_idle_strict(5000, &done, &to, &busy);
    s_tm.full_ph[1] = done;
    if (to) s_tm.full_to |= 1u << 1;
    if (busy) s_tm.full_busy |= 1u << 1;

    wcmd(0x24);
    wdata(fb, n);
    wcmd(0x26);
    wdata(fb, n);
    s_written += n * 2;
    wait_idle_strict(5000, &done, &to, &busy);
    s_tm.full_ph[2] = done;
    if (to) s_tm.full_to |= 1u << 2;
    if (busy) s_tm.full_busy |= 1u << 2;

    wcmd(0x22); { uint8_t c = 0xc7; wdata(&c, 1); }
    wcmd(0x20);
    ev_reset(s_tm.full_ev, (int)sizeof(s_tm.full_ev));
    wait_idle_strict(60000, &done, &to, &busy);
    s_tm.full_ph[3] = done;
    if (to) s_tm.full_to |= 1u << 3;
    if (busy) s_tm.full_busy |= 1u << 3;

    s_tm.full_total = (uint32_t)((esp_timer_get_time() - t_start) / 1000);
    return 0;
}

/* BOE 严格计时版局刷：0x20 后等 BUSY 真正空闲，再写 0x26。 */
static int boe_partial_timed(const uint8_t *fb)
{
    const uint32_t n = EPD_W * ((EPD_H + 7) / 8);
    uint32_t done;
    int to, busy;

    set_full_window();
    wcmd(0x24);
    wdata(fb, n);
    wcmd(0x22); { uint8_t c = 0xfc; wdata(&c, 1); }
    wcmd(0x20);
    ev_reset(s_tm.part_ev, (int)sizeof(s_tm.part_ev));
    wait_idle_strict(8000, &done, &to, &busy);
    s_tm.part_ms = done;
    s_tm.part_to = to ? 1 : 0;
    s_tm.part_busy = busy ? 1 : 0;

    set_full_window();
    wcmd(0x26);
    wdata(fb, n);
    s_written += n * 2;
    return (int)done;
}

/* SSD1680 局部刷新: 0x24 = 当前帧, 0x22 0xFC + 0x20。
 * 更新完成后把同一帧写回 0x26, 使下一帧的差分基准等于刚显示的帧;
 * 否则 RAM2 一直停留在上次全刷的画面, 多帧之后会出现该刷不刷的残影。 */
static int boe_partial(const uint8_t *fb)
{
    const uint32_t n = EPD_W * ((EPD_H + 7) / 8);
    int64_t t0 = esp_timer_get_time();

    set_full_window();
    wcmd(0x24);
    wdata(fb, n);
    wcmd(0x22); { uint8_t c = 0xfc; wdata(&c, 1); }
    wcmd(0x20);
    wait_busy_cycle(120, 6000, "p");

    set_full_window();
    wcmd(0x26);
    wdata(fb, n);
    s_written += n * 2;
    return (int)((esp_timer_get_time() - t0) / 1000);
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
    hw_reset();
    s_stage = 1;
    vTaskDelay(pdMS_TO_TICKS(100));
    s_pad47 = gpio_get_level(EPD_PIN_BUSY);
    if (s_pad47 == 0) {
        s_family = 1; /* BOE, 原厂不读 ID */
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
    static uint8_t fb[EPD_W * ((EPD_H + 7) / 8)];
    build_fb(img, fb);

    int did_full = full || s_family != 1;
    int64_t t_draw = esp_timer_get_time();
    if (s_strict) {
        /* 测量模式只对 BOE 实现；其他族明确报不支持，不猜时序。 */
        if (s_family != 1) {
            s_tm.fam = s_family;
            s_tm.valid = -1;
            return -2;
        }
        if (did_full)
            boe_refresh_timed(fb);
        else
            boe_partial_timed(fb);
    } else if (did_full) {
        if (s_family == 1)
            boe_refresh(fb);
        else
            oed_full_refresh(fb);
    } else {
        boe_partial(fb);
    }

    s_draw_ms = (uint32_t)((esp_timer_get_time() - t_draw) / 1000);
    if (did_full) {
        s_full_n++;
        s_full_ms = s_draw_ms;
    } else {
        s_partial_n++;
        s_partial_ms = s_draw_ms;
        s_partial_total += s_draw_ms;
    }
    s_draw_err = 0;
    ESP_LOGI(TAG, "draw %s family=%d %ums", did_full ? "full" : "partial", s_family,
             (unsigned)s_draw_ms);
    return 0;
}

int epd_draw(const uint8_t *img) { return epd_draw_ex(img, 1); }

int epd_draw_partial(const uint8_t *img) { return epd_draw_ex(img, 0); }

void epd_deep_sleep(void)
{
    ESP_LOGW(TAG, "deep sleep skipped (r22)");
}

void epd_timing_begin(void)
{
    memset(&s_tm, 0, sizeof(s_tm));
    s_tm.fam = s_family;
    s_tm.valid = 0;
    s_strict = 1;
}

void epd_timing_end(void)
{
    s_strict = 0;
    if (s_tm.fam != 1) s_tm.valid = -1;
    else if (s_tm.valid == 0) s_tm.valid = 1;
}

void epd_timing_diag(char *out, int n)
{
    if (s_strict) { snprintf(out, n, "tm=run"); return; }
    if (!s_tm.valid && !s_tm.fam) { snprintf(out, n, "tm=idle"); return; }
    snprintf(out, n,
             "tm=%d fam=%d full=%u ft=%x bm=%x ph=%u/%u/%u/%u ev=%s "
             "part=%u pt=%d pb=%d ev=%s",
             s_tm.valid, s_tm.fam, (unsigned)s_tm.full_total, s_tm.full_to,
             s_tm.full_busy, (unsigned)s_tm.full_ph[0], (unsigned)s_tm.full_ph[1],
             (unsigned)s_tm.full_ph[2], (unsigned)s_tm.full_ph[3],
             s_tm.full_ev[0] ? s_tm.full_ev : "-", (unsigned)s_tm.part_ms,
             s_tm.part_to, s_tm.part_busy, s_tm.part_ev[0] ? s_tm.part_ev : "-");
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
