/* input_keys.c — 5 键轮询、去抖、长按/自动重复与事件队列。
 *
 * 依据:DESIGN.md §7.1(引脚→语义)、§7.2(事件模型与队列满策略)、§3.1(keys 任务)。
 *
 * 硬件事实:GPIO 2/4/6/7/8 低有效(盘点 §1/§3.7)。原厂未开内部上拉;这里按
 * badapple/main/key.c 的做法打开内部上拉 —— 按键另一端接 GND,上拉只在松开
 * 时提供确定电平,对闭合状态无影响。
 *
 * 事件模型(key_event_t):
 *   按下沿(3 次采样去抖确认后)          → KEY_EV_DOWN
 *   按住 ≥ READER_KEY_LONG_MS(800 ms)  → KEY_EV_LONG,每按一次只发一次
 *   松开且总时长 < 800 ms               → KEY_EV_CLICK
 *   已发 LONG 且仍按住的 PREV/NEXT      → REPEAT:首个在 LONG 之后一个
 *                                          READER_KEY_REPEAT_MS(300 ms)周期
 *                                          (≈1100 ms)发出,之后每 300 ms 一次
 *                                          (受 20 ms 轮询粒度影响只晚不早)
 *
 * 队列满策略严格照 §7.2 拆分支(见 q_try_push_locked());input_dropped 只统计
 * 翻页事件(PREV/NEXT)被丢弃,非翻页事件被丢弃计入 input_lost_critical(应为 0)。
 */
#include "input.h"

#include <string.h>

#include "board.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "rc_common.h"
#include "reader_cfg.h"

static const char *TAG = "input";

/* 20 ms 轮询;READER_KEY_DEBOUNCE_MS=60 → 连续 3 次采样一致才认。 */
#define INPUT_POLL_MS 20
#define INPUT_DEBOUNCE_SAMPLES (READER_KEY_DEBOUNCE_MS / INPUT_POLL_MS)

/* 去抖时间必须能被轮询周期整除,否则宏改错时语义会漂移。 */
#if (READER_KEY_DEBOUNCE_MS % INPUT_POLL_MS) != 0
#error "READER_KEY_DEBOUNCE_MS must be a multiple of INPUT_POLL_MS"
#endif

/* 非翻页事件在“队列全为非翻页”时最多阻塞重试的时间(§7.2)。 */
#define INPUT_RETRY_MS 20
#define INPUT_RETRY_STEP_MS 2

#define INPUT_TASK_STACK 3072
#define INPUT_TASK_PRIO 3
#define INPUT_TASK_CORE 1

typedef struct {
    int gpio;
    bool raw;      /* 最近一次采样:true = 按下(低电平) */
    bool stable;   /* 去抖确认后的状态 */
    uint8_t same;  /* raw 连续保持同一值的采样次数 */
    uint64_t press_ms;
    uint64_t repeat_ms;
    bool long_sent;
} key_state_t;

static const int s_pins[KEY_COUNT] = {
    BOARD_KEY_PREV_GPIO,
    BOARD_KEY_NEXT_GPIO,
    BOARD_KEY_MENU_GPIO,
    BOARD_KEY_OK_GPIO,
    BOARD_KEY_FUNC_GPIO,
};

/* 事件队列:固定 32 条,线性数组。顺序连续,便于“丢最旧翻页事件”这类按位置
 * 删除;32 条的 memmove 代价可忽略。 */
static key_event_t s_queue[READER_KEY_QUEUE_LEN];
static uint8_t s_count;
static uint32_t s_dropped;        /* 只统计翻页事件 */
static uint32_t s_lost_critical;  /* 非翻页事件被丢弃 */

static SemaphoreHandle_t s_lock;  /* 保护 s_queue/s_count/计数 */
static SemaphoreHandle_t s_sem;   /* 计数 = 队列长度,供 input_get 阻塞 */
static SemaphoreHandle_t s_irq_sem; /* 空闲等待按键按下沿的硬件中断信号量 */
static key_state_t s_keys[KEY_COUNT];
static bool s_inited;
static bool s_irq_ok;
static void (*s_wake_cb)(void);

void input_set_wake_cb(void (*cb)(void))
{
    s_wake_cb = cb;
}

static void IRAM_ATTR key_isr(void *arg)
{
    int pin = (int)(intptr_t)arg;
    gpio_intr_disable((gpio_num_t)pin);
    BaseType_t hp = pdFALSE;
    if (s_irq_sem) {
        xSemaphoreGiveFromISR(s_irq_sem, &hp);
        if (hp) {
            portYIELD_FROM_ISR();
        }
    }
}

static bool key_is_page(uint8_t id)
{
    return id == KEY_PREV || id == KEY_NEXT;
}

static int oldest_page_locked(void)
{
    for (uint8_t i = 0; i < s_count; i++) {
        if (key_is_page(s_queue[i].id)) {
            return (int)i;
        }
    }
    return -1;
}

static void remove_at_locked(uint8_t idx)
{
    for (uint8_t i = idx; (uint8_t)(i + 1) < s_count; i++) {
        s_queue[i] = s_queue[i + 1];
    }
    s_count--;
}

/* 在锁内尝试入队。返回 true 表示已入队(调用者需给出 s_sem);
 * *retry_needed 仅在“非翻页事件遇到全为非翻页的满队列”时为 true。
 * 调用者持有 s_lock。 */
static bool q_try_push_locked(const key_event_t *ev, bool *retry_needed)
{
    *retry_needed = false;

    if (s_count < READER_KEY_QUEUE_LEN) {
        s_queue[s_count++] = *ev;
        return true;
    }

    /* 队列已满。 */
    if (key_is_page(ev->id)) {
        /* 队尾同类 → 合并(同一个翻页键的连续事件对翻页语义等价)。 */
        if (s_queue[s_count - 1].id == ev->id) {
            return false;
        }
        /* 队列里有翻页事件 → 丢最旧的那条,再入队。 */
        int victim = oldest_page_locked();
        if (victim >= 0) {
            remove_at_locked((uint8_t)victim);
            s_dropped++;
            s_queue[s_count++] = *ev;
            return true;
        }
        /* 队列里全是非翻页事件:丢新事件;翻页事件的丢弃计入 input_dropped。 */
        s_dropped++;
        return false;
    }

    /* 非翻页事件:优先牺牲最旧的翻页事件,保证非翻页事件不因翻页而丢。 */
    int victim = oldest_page_locked();
    if (victim >= 0) {
        remove_at_locked((uint8_t)victim);
        s_dropped++;
        s_queue[s_count++] = *ev;
        return true;
    }

    /* 队列里没有任何翻页事件可牺牲 → 交给调用者按 §7.2 限时重试。 */
    *retry_needed = true;
    return false;
}

/* 投递一个事件。生产者:keys 任务 / input_inject。 */
static void q_push(const key_event_t *ev)
{
    uint64_t deadline = rc_now_ms() + INPUT_RETRY_MS;

    for (;;) {
        bool retry = false;
        bool enqueued;

        xSemaphoreTake(s_lock, portMAX_DELAY);
        enqueued = q_try_push_locked(ev, &retry);
        if (enqueued) {
            /* 在锁内 give,保证信号量计数与队列长度始终一致,否则消费者
             * 可能在“计数已加、元素未可见”的窗口里取到空队列。 */
            xSemaphoreGive(s_sem);
        }
        xSemaphoreGive(s_lock);

        if (enqueued) {
            void (*cb)(void) = s_wake_cb;
            if (cb) {
                cb();
            }
            return;
        }
        if (!retry) {
            return;
        }

        /* 队列全是非翻页事件:最多阻塞 20 ms 等消费者腾位置。 */
        if (rc_now_ms() >= deadline) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_lost_critical++;
            xSemaphoreGive(s_lock);
            ESP_LOGE(TAG, "queue full: non-page key id=%u dropped", ev->id);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(INPUT_RETRY_STEP_MS));
    }
}

static bool q_pop(key_event_t *out)
{
    bool ok = false;

    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    if (s_count > 0) {
        *out = s_queue[0];
        remove_at_locked(0);
        ok = true;
    }
    xSemaphoreGive(s_lock);
    return ok;
}

/* 采样一次全部按键,处理去抖边沿、长按与重复。仅在 keys 任务中调用。 */
static void keys_scan(uint64_t now)
{
    for (int i = 0; i < KEY_COUNT; i++) {
        key_state_t *k = &s_keys[i];
        key_event_t ev = {.id = (uint8_t)i, .type = 0, .held_ms = 0};
        bool pressed = gpio_get_level(k->gpio) == 0;

        if (pressed != k->raw) {
            k->raw = pressed;
            k->same = 1;
        } else if (k->same < INPUT_DEBOUNCE_SAMPLES) {
            k->same++;
        }

        /* 连续 INPUT_DEBOUNCE_SAMPLES 次一致 → 确认状态翻转。 */
        if (k->same >= INPUT_DEBOUNCE_SAMPLES && k->stable != k->raw) {
            k->stable = k->raw;
            if (k->stable) {
                k->press_ms = now;
                k->long_sent = false;
                /* REPEAT 的计时基准在发 LONG 时设置:这样首个 REPEAT 一定在
                 * LONG 之后一个 300 ms 周期,而不是紧随 LONG。 */
                k->repeat_ms = 0;
                ev.type = KEY_EV_DOWN;
                q_push(&ev);
            } else if (!k->long_sent) {
                ev.type = KEY_EV_CLICK;
                ev.held_ms = (uint32_t)(now - k->press_ms);
                q_push(&ev);
            }
            /* 长按后松开不发 CLICK(§7.2:只有“松开且 <800 ms”才是 CLICK)。 */
        }

        if (!k->stable) {
            continue;
        }

        uint32_t held = (uint32_t)(now - k->press_ms);
        if (!k->long_sent && held >= READER_KEY_LONG_MS) {
            k->long_sent = true;
            /* 首个 REPEAT 在 LONG 之后一个周期(≈1100 ms),两个事件不会在
             * 同一个 20 ms 轮询里连发(下面的 else if)。 */
            k->repeat_ms = now + READER_KEY_REPEAT_MS;
            ev.type = KEY_EV_LONG;
            ev.held_ms = held;
            q_push(&ev);
        } else if (k->long_sent && key_is_page((uint8_t)i) && now >= k->repeat_ms) {
            k->repeat_ms = now + READER_KEY_REPEAT_MS;
            ev.type = KEY_EV_REPEAT;
            ev.held_ms = held;
            q_push(&ev);
        }
    }
}

static bool keys_all_idle(void)
{
    for (int i = 0; i < KEY_COUNT; i++) {
        const key_state_t *k = &s_keys[i];
        if (k->raw || k->stable || k->same < INPUT_DEBOUNCE_SAMPLES) {
            return false;
        }
        if (gpio_get_level(k->gpio) == 0) {
            return false;
        }
    }
    return true;
}

static void input_task(void *arg)
{
    (void)arg;

    for (;;) {
        keys_scan(rc_now_ms());
        if (s_irq_ok && s_irq_sem && keys_all_idle()) {
            /* 全部按键均已松开且去抖稳定：切入低电平中断+轻睡唤醒，0 轮询待机。 */
            (void)xSemaphoreTake(s_irq_sem, 0);
            bool any_low = false;
            for (int i = 0; i < KEY_COUNT; i++) {
                (void)gpio_intr_enable((gpio_num_t)s_pins[i]);
                if (gpio_get_level(s_pins[i]) == 0) {
                    any_low = true;
                }
            }
            if (!any_low) {
                (void)xSemaphoreTake(s_irq_sem, portMAX_DELAY);
            }
            for (int i = 0; i < KEY_COUNT; i++) {
                (void)gpio_intr_disable((gpio_num_t)s_pins[i]);
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(INPUT_POLL_MS));
        }
    }
}

int input_init(void)
{
    if (s_inited) {
        return 0;
    }

    uint64_t mask = 0;
    for (int i = 0; i < KEY_COUNT; i++) {
        mask |= 1ull << s_pins[i];
    }
    gpio_config_t cfg = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_LOW_LEVEL,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config failed: %s", esp_err_to_name(err));
        return -1;
    }
    for (int i = 0; i < KEY_COUNT; i++) {
        (void)gpio_intr_disable((gpio_num_t)s_pins[i]);
    }

    s_lock = xSemaphoreCreateMutex();
    s_sem = xSemaphoreCreateCounting(READER_KEY_QUEUE_LEN, 0);
    s_irq_sem = xSemaphoreCreateBinary();
    if (!s_lock || !s_sem) {
        ESP_LOGE(TAG, "failed to create queue primitives");
        if (s_lock) {
            vSemaphoreDelete(s_lock);
            s_lock = NULL;
        }
        if (s_sem) {
            vSemaphoreDelete(s_sem);
            s_sem = NULL;
        }
        if (s_irq_sem) {
            vSemaphoreDelete(s_irq_sem);
            s_irq_sem = NULL;
        }
        return -1;
    }

    /* 安装 GPIO 中断与轻睡硬件唤醒；失败时自动退化为 20 ms 轮询。 */
    s_irq_ok = false;
    if (s_irq_sem) {
        esp_err_t ierr = gpio_install_isr_service(0);
        if (ierr == ESP_OK || ierr == ESP_ERR_INVALID_STATE) {
            bool irq_all = true;
            for (int i = 0; i < KEY_COUNT; i++) {
                if (gpio_isr_handler_add((gpio_num_t)s_pins[i], key_isr,
                                         (void *)(intptr_t)s_pins[i]) != ESP_OK ||
                    gpio_wakeup_enable((gpio_num_t)s_pins[i],
                                       GPIO_INTR_LOW_LEVEL) != ESP_OK) {
                    irq_all = false;
                    break;
                }
            }
            if (irq_all && esp_sleep_enable_gpio_wakeup() == ESP_OK) {
                s_irq_ok = true;
            } else {
                ESP_LOGW(TAG, "gpio irq wakeup setup incomplete; falling back to poll");
            }
        } else {
            ESP_LOGW(TAG, "gpio_install_isr_service: %s; falling back to poll",
                     esp_err_to_name(ierr));
        }
    }

    /* 以当前电平做初始状态:上电时已按住的键不会产生一次假按下沿。 */
    for (int i = 0; i < KEY_COUNT; i++) {
        key_state_t *k = &s_keys[i];
        k->gpio = s_pins[i];
        k->raw = gpio_get_level(k->gpio) == 0;
        k->stable = k->raw;
        k->same = (uint8_t)INPUT_DEBOUNCE_SAMPLES;
        k->press_ms = 0;
        k->repeat_ms = 0;
        /* 若上电即按住（如深睡长按唤醒），本次按住视为已消费，松手时不补发 CLICK/LONG。 */
        k->long_sent = k->stable;
    }
    s_count = 0;
    s_dropped = 0;
    s_lost_critical = 0;

    BaseType_t ok = xTaskCreatePinnedToCore(input_task, "keys", INPUT_TASK_STACK, NULL,
                                            INPUT_TASK_PRIO, NULL, INPUT_TASK_CORE);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "failed to create keys task");
        vSemaphoreDelete(s_lock);
        vSemaphoreDelete(s_sem);
        s_lock = NULL;
        s_sem = NULL;
        return -1;
    }

    s_inited = true;
    ESP_LOGI(TAG, "5 keys on GPIO 2/4/6/7/8, poll %d ms, debounce %d samples",
             INPUT_POLL_MS, INPUT_DEBOUNCE_SAMPLES);
    return 0;
}

bool input_get(key_event_t *ev, uint32_t timeout_ms)
{
    if (!ev) {
        return false;
    }
    if (!s_inited) {
        /* 初始化失败时按超时语义等待,避免 app 任务变成忙等。 */
        if (timeout_ms) {
            vTaskDelay(pdMS_TO_TICKS(timeout_ms));
        }
        return false;
    }

    TickType_t ticks = 0;
    if (timeout_ms) {
        /* 手算毫秒→tick,避免 pdMS_TO_TICKS 在超大 timeout 上 32 位溢出。 */
        uint64_t t = (uint64_t)timeout_ms * (uint64_t)configTICK_RATE_HZ / 1000u;
        ticks = (t >= (uint64_t)portMAX_DELAY) ? portMAX_DELAY : (TickType_t)t;
        if (ticks == 0) {
            ticks = 1;
        }
    }

    if (xSemaphoreTake(s_sem, ticks) != pdTRUE) {
        return false;
    }
    /* 计数信号量与队列长度一一对应;这里的失败只可能是内存被破坏。 */
    return q_pop(ev);
}

uint32_t input_mask(void)
{
    uint32_t mask = 0;

    if (!s_inited) {
        return 0;
    }
    for (int i = 0; i < KEY_COUNT; i++) {
        if (gpio_get_level(s_pins[i]) == 0) {
            mask |= 1u << s_pins[i];
        }
    }
    return mask;
}

uint32_t input_dropped(void)
{
    return s_dropped;
}

uint32_t input_lost_critical(void)
{
    return s_lost_critical;
}

void input_inject(key_id_t id)
{
    if (!s_inited) {
        ESP_LOGW(TAG, "inject before init ignored (id=%d)", (int)id);
        return;
    }
    if (id < 0 || id >= KEY_COUNT) {
        ESP_LOGW(TAG, "inject bad id=%d", (int)id);
        return;
    }

    /* 合成一次“短按”,事件类型按状态机的实际消费方式选(§7.3):
     *   - 翻页键:PREV/NEXT 的按下沿(DOWN)就翻页,REPEAT 只是自动重复;只投
     *     DOWN,一次 inject 恰好翻一页,不补 CLICK(否则会额外翻一页)。
     *   - MENU/OK/FUNC:状态机里这些键的“短按”分支只认 CLICK,“长按”分支只认
     *     LONG;注入 CLICK 才能走到短按分支,且不会被当成一次 800 ms 长按。
     * 两种映射在 §7.3 里都没有第二个消费者(INFO/ALERT 也只认 CLICK),所以
     * 不存在“一次 inject 触发两个动作”的歧义。 */
    uint8_t type = key_is_page((uint8_t)id) ? KEY_EV_DOWN : KEY_EV_CLICK;
    key_event_t ev = {.id = (uint8_t)id, .type = type, .held_ms = 0};
    q_push(&ev);
}
