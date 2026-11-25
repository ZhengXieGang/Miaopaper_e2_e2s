/* E2S 按键扫描。
 *
 * 原厂 components/key/key.c 的 KeyInit（0x4201DE5C）对 GPIO 2/4/6/7/8
 * 逐个 gpio_config(mode=INPUT) + gpio_set_direction()，没有开内部上拉
 * （反汇编里 pull_up_en=0；badapple 开上拉是为了让悬空脚有确定电平）。
 * 这里只做轮询 + 消抖：连续 3 次（约 60ms）低电平算按下，松开同理。
 *
 * 每个脚单独累计按下次数和最后一次按住时长，供 (1,241) 读回：
 * 按下哪个物理键就只会有一个计数增加，用来建立 GPIO→丝印键的映射。
 */
#include "key.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>

static const int s_pins[] = { 2, 4, 6, 7, 8 };
#define KEY_N (sizeof(s_pins) / sizeof(s_pins[0]))

static volatile bool s_event;
static volatile uint32_t s_mask;
static volatile uint32_t s_count[KEY_N];
static volatile uint32_t s_hold_ms[KEY_N];
static volatile int s_last = -1;
static int64_t s_press_us[KEY_N];

static void key_task(void *arg)
{
    (void)arg;
    bool pressed[KEY_N] = { false };
    uint8_t low[KEY_N] = { 0 }, high[KEY_N] = { 0 };

    for (;;) {
        uint32_t mask = 0;
        for (size_t i = 0; i < KEY_N; i++) {
            if (gpio_get_level(s_pins[i]) == 0) {
                if (low[i] < 3) low[i]++;
                high[i] = 0;
                if (!pressed[i] && low[i] >= 3) {
                    pressed[i] = true;
                    s_event = true;
                    s_count[i]++;
                    s_press_us[i] = esp_timer_get_time();
                }
            } else {
                if (high[i] < 3) high[i]++;
                low[i] = 0;
                if (pressed[i] && high[i] >= 3) {
                    pressed[i] = false;
                    int64_t d = (esp_timer_get_time() - s_press_us[i]) / 1000;
                    s_hold_ms[i] = d > 65535 ? 65535u : (uint32_t)d;
                    s_last = (int)i;
                }
            }
            if (pressed[i]) mask |= 1u << s_pins[i];
        }
        s_mask = mask;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void key_init(void)
{
    uint64_t pins = 0;
    for (size_t i = 0; i < KEY_N; i++) pins |= 1ull << s_pins[i];
    gpio_config_t cfg = {
        .pin_bit_mask = pins,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    xTaskCreatePinnedToCore(key_task, "keys", 2048, NULL, 3, NULL, 1);
}

bool key_take_event(void)
{
    bool e = s_event;
    s_event = false;
    return e;
}

uint32_t key_mask(void) { return s_mask; }

/* kc=2/4/6/7/8 各自的按下次数，last=最后松开的脚/按住毫秒数。 */
void key_stats(char *out, int n)
{
    int len = snprintf(out, n, "kc=");
    for (size_t i = 0; i < KEY_N && len > 0 && len < n; i++)
        len += snprintf(out + len, (size_t)(n - len), "%s%u", i ? "," : "",
                        (unsigned)s_count[i]);
    if (len > 0 && len < n) {
        if (s_last >= 0 && s_last < (int)KEY_N)
            snprintf(out + len, (size_t)(n - len), " last=%d/%ums",
                     s_pins[s_last], (unsigned)s_hold_ms[s_last]);
        else
            snprintf(out + len, (size_t)(n - len), " last=-");
    }
}
