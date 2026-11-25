/* Bad Apple 回放任务。
 *
 * 帧数据不再放进 App 镜像：657 KB 的 frames.bin 放在 flash 原始区
 * 0x530000（原厂固件未使用、整段 0xFF），App 只有 ~470 KB。
 * 启动时先画条纹并全刷一次：既完成面板初始化，也让人直接看出固件在跑，
 * 不依赖 BLE 是否可用。
 *
 * frames.bin 格式（little-endian）:
 *   0  "BAPL"
 *   4  u8  version = 1
 *   5  u8  flags
 *   6  u16 fps
 *   8  u16 width
 *  10  u16 height
 *  12  u32 frame_count
 *  16  frame_count * { u32 payload_offset; u8 flags; u8[3] reserved; }
 *  ... payload: 每帧 RLE，变长 LEB128 游程，0/1 交替且从 0(黑) 开始，
 *      解出 250*122 位、行宽 32 字节、MSB 在前、1=白。
 *
 * flags bit0 = 播放该帧前先做一次全刷（清残影/重同步 RAM2）。
 */
#include "playback.h"
#include "epd.h"
#include "font.h"
#include "frames_rle.h"
#include "key.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "playback";

/* main.c: 清理僵死连接并确保广播。 */
extern void ble_service_tick(void);
/* main.c: 启动阶段码，画在待机画面底部，不用日志就能看出 BLE 卡在哪一步。 */
extern int ble_boot_status(void);
/* main.c: 待机开广播 / 播放关广播。 */
extern void ble_set_advertising(bool on);

/* 帧区：原厂字体/图案数据结束于 0x528884，0x530000 起整段空白。 */
#define FRAMES_ADDR    0x530000u
#define FRAMES_HDR_LEN 16u
/* 实测单帧 RLE 最大 1183 B；多读一点，尾部填充不会参与解码。 */
#define FRAME_BUF_MAX  2048u

/* 帧区里是 6 fps / 1314 帧的内容。面板局部刷新实测远达不到 6 fps，
 * 直接按 6 fps 推会撕裂。这里每 3 帧取 1 帧、按 2 fps 播放，
 * 等价于 438 帧 / 219 秒的低帧率版本。 */
#define PLAYBACK_FPS    2
#define PLAYBACK_STRIDE 3

#define ROW_BYTES ((EPD_W + 7) / 8)
#define FB_BYTES  (ROW_BYTES * EPD_H)
#define BAND_Y    (EPD_H - 32)

static volatile bool s_full_request;
static volatile bool s_timing_req;
static char s_diag[64] = "fr=init";

void playback_request_full(void) { s_full_request = true; }
void playback_request_timing(void) { s_timing_req = true; }

void playback_diag(char *out, int n) { snprintf(out, n, "%s", s_diag); }

typedef struct {
    uint32_t count;
    uint16_t fps;
    const uint8_t *table;
    uint8_t *frame;
    uint32_t payload_addr;
} blob_t;

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static bool flash_read(uint32_t addr, void *dst, uint32_t len)
{
    /* esp_flash_read 要求 4 字节长度；多读的尾部字节不会进入解码窗口。 */
    uint32_t n = (len + 3u) & ~3u;
    return esp_flash_read(NULL, dst, addr, n) == ESP_OK;
}

static void set_diag(const char *s) { snprintf(s_diag, sizeof(s_diag), "%s", s); }

static bool blob_open(blob_t *b)
{
    uint8_t hdr[FRAMES_HDR_LEN] __attribute__((aligned(4)));
    memset(b, 0, sizeof(*b));

    if (!flash_read(FRAMES_ADDR, hdr, sizeof(hdr))) {
        set_diag("fr=hdr_io");
        return false;
    }
    if (memcmp(hdr, "BAPL", 4) || hdr[4] != 1) {
        set_diag("fr=no_data");
        return false;
    }
    b->fps = rd16(hdr + 6);
    uint16_t w = rd16(hdr + 8), h = rd16(hdr + 10);
    b->count = rd32(hdr + 12);
    if (w != EPD_W || h != EPD_H || b->fps < 1 || b->fps > 10 ||
        b->count == 0 || b->count > 4000) {
        set_diag("fr=bad_hdr");
        return false;
    }

    uint32_t tlen = b->count * 8u;
    b->table = heap_caps_malloc(tlen + 4u, MALLOC_CAP_8BIT);
    b->frame = heap_caps_malloc(FRAME_BUF_MAX + 4u, MALLOC_CAP_8BIT);
    if (!b->table || !b->frame) {
        set_diag("fr=nomem");
        return false;
    }
    if (!flash_read(FRAMES_ADDR + FRAMES_HDR_LEN, (void *)b->table, tlen)) {
        set_diag("fr=tab_io");
        return false;
    }
    b->payload_addr = FRAMES_ADDR + FRAMES_HDR_LEN + tlen;

    for (uint32_t i = 0; i < b->count; i++) {
        uint32_t off = rd32(b->table + i * 8);
        if (off > 0x200000u) {
            set_diag("fr=bad_tab");
            return false;
        }
        if (i + 1 < b->count) {
            uint32_t next = rd32(b->table + (i + 1) * 8);
            if (next <= off || next - off > FRAME_BUF_MAX) {
                set_diag("fr=bad_tab");
                return false;
            }
        }
    }

    snprintf(s_diag, sizeof(s_diag), "fr=ok,c=%u,fps=%u", (unsigned)b->count, b->fps);
    return true;
}

static bool frame_load(const blob_t *b, uint32_t i, const uint8_t **end)
{
    uint32_t off = rd32(b->table + i * 8);
    uint32_t span;
    if (i + 1 < b->count) {
        span = rd32(b->table + (i + 1) * 8) - off;
    } else {
        /* 最后一帧没有下一项；多读的 0xFF 填充在 250*122 位填满后不会再被读取。 */
        span = FRAME_BUF_MAX;
    }
    if (!span || span > FRAME_BUF_MAX) return false;
    if (!flash_read(b->payload_addr + off, b->frame, span)) return false;
    *end = b->frame + span;
    return true;
}

static bool frame_full(const blob_t *b, uint32_t i)
{
    return (b->table[i * 8 + 4] & 1) != 0;
}

/* 黑字白底，1=白。 */
static void draw_text(uint8_t *img, int x0, int y0, const char *text, int scale)
{
    const int gw = 8 * scale;
    for (int ci = 0; text[ci]; ci++) {
        const uint8_t *g = font8x16_glyph((unsigned char)text[ci]);
        for (int gy = 0; gy < 16; gy++) {
            for (int gx = 0; gx < 8; gx++) {
                if (!(g[gy] & (0x80 >> gx))) continue;
                for (int sy = 0; sy < scale; sy++) {
                    for (int sx = 0; sx < scale; sx++) {
                        int px = x0 + ci * gw + gx * scale + sx;
                        int py = y0 + gy * scale + sy;
                        if (px >= 0 && px < EPD_W && py >= 0 && py < EPD_H)
                            img[(size_t)py * ROW_BYTES + (px >> 3)] &= (uint8_t)~(0x80 >> (px & 7));
                    }
                }
            }
        }
    }
}

/* 待机画面：沿用 hello_paperang 的样式（8px 黑边框 + 2x 居中标题），
 * 下面一行显示 BLE 状态码：1=广播已起，2..6=启动失败阶段。 */
static void idle_screen(uint8_t *img, int status)
{
    memset(img, 0xff, FB_BYTES);
    for (int y = 0; y < EPD_H; y++) {
        for (int x = 0; x < EPD_W; x++) {
            if (x < 8 || x >= EPD_W - 8 || y < 8 || y >= EPD_H - 8)
                img[(size_t)y * ROW_BYTES + (x >> 3)] &= (uint8_t)~(0x80 >> (x & 7));
        }
    }

    const char *hello = "Hello Paperang";
    int x0 = (EPD_W - (int)strlen(hello) * 8 * 2) / 2;
    draw_text(img, x0, 36, hello, 2);

    char st[16];
    snprintf(st, sizeof(st), "BLE:%d", status);
    x0 = (EPD_W - (int)strlen(st) * 8) / 2;
    draw_text(img, x0, 84, st, 1);
}

/* 时序测量：全刷一次待机画面，再改一块像素做局刷。
 * 结果在 epd.c 的 s_tm 里，由 (1,244) 读回；不写 flash、不改 otadata。 */
static void run_timing(uint8_t *img)
{
    idle_screen(img, ble_boot_status());
    ESP_LOGW(TAG, "timing test: full refresh");
    epd_timing_begin();
    int rc = epd_draw_ex(img, 1);
    if (rc == 0) {
        for (int y = 24; y < 64; y++) {
            for (int x = 24; x < 96; x++)
                img[(size_t)y * ROW_BYTES + (x >> 3)] ^= (uint8_t)(0x80 >> (x & 7));
        }
        ESP_LOGW(TAG, "timing test: partial refresh");
        epd_draw_ex(img, 0);
    }
    epd_timing_end();
    ESP_LOGW(TAG, "timing test done rc=%d", rc);
}

static void playback_task(void *arg)
{
    (void)arg;
    static uint8_t img[FB_BYTES];
    blob_t b;

    if (epd_init() != 0) {
        ESP_LOGE(TAG, "epd init failed, playback stopped");
        vTaskDelete(NULL);
        return;
    }

    key_init();
    idle_screen(img, ble_boot_status());
    int64_t t0 = esp_timer_get_time();
    if (epd_draw_ex(img, 1) != 0) ESP_LOGW(TAG, "idle screen draw failed");
    ESP_LOGI(TAG, "idle screen %lldms ble_status=%d",
             (long long)((esp_timer_get_time() - t0) / 1000), ble_boot_status());

    if (!blob_open(&b)) {
        ESP_LOGE(TAG, "frames unavailable: %s", s_diag);
        for (;;) {
            if (s_timing_req) {
                s_timing_req = false;
                ble_set_advertising(true);
                run_timing(img);
            }
            if (key_take_event()) ESP_LOGI(TAG, "key ignored: no frames");
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
    ESP_LOGI(TAG, "playback %u frames @ %u fps (stride %u) from %#x", (unsigned)b.count,
             b.fps, PLAYBACK_STRIDE, (unsigned)FRAMES_ADDR);
    const int64_t period_us = 1000000 / PLAYBACK_FPS;

    bool playing = false;
    uint32_t i = 0;
    unsigned idle_ticks = 0;

    for (;;) {
        if (s_timing_req) {
            s_timing_req = false;
            playing = false;
            ble_set_advertising(true);
            run_timing(img);
            continue;
        }

        if (key_take_event()) {
            if (!playing) {
                playing = true;
                i = 0;
                ble_set_advertising(false);
                ESP_LOGI(TAG, "key: start playback, advertising off");
            } else {
                playing = false;
                ble_set_advertising(true);
                idle_screen(img, ble_boot_status());
                ESP_LOGI(TAG, "key: stop playback, clear screen, advertising on");
                epd_draw_ex(img, 1);
            }
        }

        if (!playing) {
            if (++idle_ticks >= 20) {
                idle_ticks = 0;
                ble_service_tick();
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        const uint8_t *end;
        if (!frame_load(&b, i, &end) ||
            !frames_decode(b.frame, end, img, EPD_W, EPD_H)) {
            ESP_LOGE(TAG, "frame %u decode failed", (unsigned)i);
            playing = false;
            ble_set_advertising(true);
            idle_screen(img, ble_boot_status());
            epd_draw_ex(img, 1);
            continue;
        }
        int64_t t = esp_timer_get_time();
        bool full = frame_full(&b, i);
        if (s_full_request) {
            s_full_request = false;
            full = true;
        }
        if (epd_draw_ex(img, full ? 1 : 0) != 0) {
            ESP_LOGE(TAG, "frame %u draw failed", (unsigned)i);
            playing = false;
            ble_set_advertising(true);
            idle_screen(img, ble_boot_status());
            epd_draw_ex(img, 1);
            continue;
        }
        int64_t spent = esp_timer_get_time() - t;
        if (spent < period_us) {
            vTaskDelay(pdMS_TO_TICKS((uint32_t)((period_us - spent + 999) / 1000)));
        }
        if ((i % 30) == 0)
            ble_service_tick();
        if ((i % 24) == 0)
            ESP_LOGI(TAG, "frame %u/%u %s %lldms", (unsigned)i, (unsigned)b.count,
                     full ? "full" : "part", (long long)(spent / 1000));
        i += PLAYBACK_STRIDE;
        if (i >= b.count) {
            i = 0;
            vTaskDelay(pdMS_TO_TICKS(3000));
        }
    }
}

void playback_start(void)
{
    xTaskCreatePinnedToCore(playback_task, "playback", 6144, NULL, 5, NULL, 1);
}
