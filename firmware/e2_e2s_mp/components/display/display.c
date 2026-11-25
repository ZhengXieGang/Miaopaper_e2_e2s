/* display.c — display task and refresh policy (DESIGN.md §3.2/§4.2/§4.4/§4.5).
 *
 * 三块逻辑帧的所有权(DESIGN §3.2):
 *   - app 写自己的 app_fb;display_post() 在 s_lock 内 memcpy 到 s_pending,
 *     返回后 app 可立刻复用 app_fb;
 *   - s_pending 只由 s_lock 保护,新 post 覆盖旧的(latest-wins);
 *   - s_work 只由 display 任务读写:任务在锁内把 s_pending 拷到 s_work 后
 *     立刻放锁,再调用 epd_draw_ex();只有 display 任务碰面板。
 *
 * 全刷点(DESIGN §4.4):冷启动首帧、打开书(调用方 post FULL)、手动
 * (display_request_full / FUNC 长按 / BLE 0x40)、GC、刷新失败后的单次自愈。
 * 其余都是局刷。局刷之后的第一次全刷至少间隔 READER_GC_MIN_GAP_MS
 * (5 s);边界处若 GC 待办且间隔不足则降级局刷并保留 pending。
 * GC 在**全刷启动时**就清 gc_pending(D9),避免 GC 全刷(~1.3 s)期间 app
 * 读到"待办+间隔足够"又把下一帧标成 FULL;GC 完成后 5 s 内到达的非强制
 * FULL 也降级为局刷(D4),两道闸一起保证不会出现 GC 全刷紧接第二次全刷。
 */
#include "display.h"

#include <limits.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#ifdef CONFIG_PM_ENABLE
#include "esp_pm.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "display_internal.h"
#include "epd.h"
#include "reader_cfg.h"

#if READER_GC_PARTIALS > 255
#error "READER_GC_PARTIALS must fit display_stats_t.gc_partials (u8)"
#endif
#if READER_GC_IDLE_MS > 65535
#error "READER_GC_IDLE_MS must fit display_stats_t.gc_idle_ms (u16)"
#endif

static const char *TAG = "display";

static bool s_ready;
static bool s_sleeping;
static display_stats_t s_stats;
static SemaphoreHandle_t s_lock;     /* 保护 s_pending/s_mode/s_dirty/s_stats/s_work 交接 */
static SemaphoreHandle_t s_wake;     /* 有新帧或参数变化时唤醒 display 任务 */
static TaskHandle_t s_task;
#ifdef CONFIG_PM_ENABLE
static esp_pm_lock_handle_t s_pm_lock; /* 刷屏 SPI 传输期间禁止自动轻睡 */
#endif

/* 逻辑帧:调用方的 app_fb + s_pending + s_work(DESIGN §3.2,共 3 块)。 */
static uint8_t s_pending[READER_FB_BYTES];
static uint8_t s_work[READER_FB_BYTES];

static bool s_dirty;             /* s_pending 里有一帧未取走 */
static display_mode_t s_mode;    /* 该帧的请求模式 */
static bool s_force_full;        /* display_request_full() 的一次性请求 */
static bool s_heal_pending;      /* 刷新失败后的单次全刷自愈 */
static bool s_busy;              /* display 任务正在刷面板 */
static bool s_attempted;         /* 至少完成过一次刷新尝试 */
static uint32_t s_last_refresh_ms; /* 上一次刷新尝试的结束时刻(开机毫秒) */

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* 一次刷新的计划。plan_refresh() 必须在 s_lock 内调用。 */
typedef struct {
    bool draw;
    bool full;
    bool use_work;      /* 无新帧:重刷 s_work(GC / 手动 / 自愈) */
    bool is_heal;       /* 这是失败后的自愈重试,失败后不再自动重试 */
    bool is_gc;         /* 本次全刷属于自动清残影 */
    const char *why;
} refresh_plan_t;

/* 选择本次动作。只看状态,不改状态、不拷帧。所有分支均无阻塞等待。 */
static void plan_refresh(refresh_plan_t *p)
{
    const uint32_t now = now_ms();
    const bool has_frame = s_stats.frames > 0;
    const bool baseline = s_stats.baseline_valid;
    const bool non_boe = s_stats.family != 1;
    const bool gc_pending = s_stats.gc_pending;
    const bool force = s_force_full;
    const uint32_t since = s_attempted ? (uint32_t)(now - s_last_refresh_ms)
                                       : READER_GC_MIN_GAP_MS;

    memset(p, 0, sizeof(*p));

    /* 1) 失败自愈:立即重刷最后一次尝试的帧,只自动重试一次。 */
    if (s_heal_pending && !s_dirty) {
        p->draw = true;
        p->full = true;
        p->use_work = true;
        p->is_heal = true;
        p->why = "heal";
        return;
    }

    /* 2) 有新帧:决定直接全刷、页内合并 GC 全刷,还是局刷。 */
    if (s_dirty) {
        if (force || !baseline || s_mode == DISPLAY_MODE_FULL || non_boe) {
            p->draw = true;
            p->full = true;
            if (force)
                p->why = "manual";
            else if (!baseline)
                p->why = has_frame ? "recover" : "first";
            else if (s_mode == DISPLAY_MODE_FULL)
                p->why = "request";
            else
                p->why = "family";    /* family!=1 一律全刷(§4.5) */
            return;
        }

        /* 累计局刷满阈值且未切强刷时,在下一次新帧绘制直接一步合并成全刷,
         * 既消残影又不触发操作后突发卡顿或闪两遍。 */
        if (gc_pending && s_mode == DISPLAY_MODE_AUTO && s_stats.gc_partials != 0) {
            p->draw = true;
            p->full = true;
            p->is_gc = true;
            p->why = "gc_page";
            return;
        }

        p->draw = true;
        p->full = false;
        p->why = "page";
        return;
    }

    /* 3) 没有新帧:还没有任何已显示画面时无事可做。 */
    if (!has_frame)
        return;

    /* 4) 手动全刷:立即重刷当前 s_work。 */
    if (force) {
        p->draw = true;
        p->full = true;
        p->use_work = true;
        p->why = "manual";
        return;
    }

    /* 5) 空闲自动 GC:用户停留在同一页超过 idle_ms 后再后台清残影,
     * 等待期间可随时被新帧唤醒打断。 */
    if (gc_pending && baseline && s_stats.gc_partials != 0) {
        uint32_t idle = s_stats.gc_idle_ms;
        if (idle < READER_GC_MIN_GAP_MS)
            idle = READER_GC_MIN_GAP_MS;
        if (since >= idle) {
            p->draw = true;
            p->full = true;
            p->use_work = true;
            p->is_gc = true;
            p->why = "gc";
        }
    }
}

/* 刷新结束后更新统计(在 s_lock 内调用)。
 * last_ms / last_full_ms 是**时刻**(开机毫秒,esp_timer 单调时钟),不是耗时。 */
static void stats_update(const refresh_plan_t *p, int err, uint32_t ms, uint32_t end_ms)
{
    const uint32_t now = end_ms;
    s_last_refresh_ms = now;
    s_attempted = true;
    s_busy = false;
    s_stats.last_ms = now;
    s_stats.last_err = err;

    if (p->full) {
        s_force_full = false;
        s_heal_pending = false;
        s_stats.last_full_ms = now;
        if (err == 0) {
            s_stats.frames++;
            s_stats.fulls++;
            s_stats.partial_since_full = 0;
            s_stats.baseline_valid = true;   /* 只有成功的全刷才建立基准 */
            s_stats.gc_pending = false;      /* 任何成功的全刷都消费 GC */
        } else {
            s_stats.baseline_valid = false;
            /* 自愈重试本身失败后清零:最多自动补一次全刷,之后回到空闲,
             * 等下一次 display_post()(D5,不会无限重试)。 */
            s_heal_pending = !p->is_heal;
        }
    } else if (err == 0) {
        s_stats.frames++;
        s_stats.partials++;
        s_stats.partial_since_full++;
        if (s_stats.gc_partials != 0 && !s_stats.gc_pending &&
            s_stats.partial_since_full >= s_stats.gc_partials) {
            s_stats.gc_pending = true;
            ESP_LOGW(TAG, "gc pending after %u partials", (unsigned)s_stats.partial_since_full);
        }
    } else {
        s_stats.baseline_valid = false;
        s_heal_pending = true;
    }

    if (err != 0) {
        ESP_LOGE(TAG, "%s failed err=%d; baseline invalid (next refresh is full)", p->why, err);
        return;
    }
    if (p->full) {
        ESP_LOGI(TAG, "full ms=%u why=%s frames=%u family=%d", (unsigned)ms, p->why,
                 (unsigned)s_stats.frames, s_stats.family);
    } else {
        ESP_LOGI(TAG, "partial ms=%u why=%s since_full=%u", (unsigned)ms, p->why,
                 (unsigned)s_stats.partial_since_full);
    }
}

static void display_task(void *arg)
{
    (void)arg;
    for (;;) {
        refresh_plan_t plan;
        bool wait_forever = true;
        uint32_t wake_wait_ms = 0;

        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_sleeping) {
            memset(&plan, 0, sizeof(plan));
        } else {
            plan_refresh(&plan);
        }
        if (plan.draw) {
            if (!plan.use_work) {
                /* 锁内交接:之后 app 可以立刻复用自己的 app_fb */
                memcpy(s_work, s_pending, READER_FB_BYTES);
                s_dirty = false;
                s_mode = DISPLAY_MODE_AUTO;
            }
            if (plan.full) {
                /* 全刷一旦启动就消费 GC 待办(D9),防止全刷期间新帧重复排队全刷。 */
                s_stats.gc_pending = false;
            }
            s_busy = true;
        } else if (!s_sleeping && s_stats.frames > 0 && s_stats.gc_pending &&
                   s_stats.baseline_valid && s_stats.gc_partials != 0) {
            /* 有待办空闲 GC 时按精确到期毫秒等待；等待期间可随时被新帧打断。 */
            uint32_t idle = s_stats.gc_idle_ms;
            if (idle < READER_GC_MIN_GAP_MS) {
                idle = READER_GC_MIN_GAP_MS;
            }
            uint32_t since = s_attempted ? (uint32_t)(now_ms() - s_last_refresh_ms)
                                         : idle;
            wake_wait_ms = (since >= idle) ? 50u : (idle - since);
            wait_forever = false;
        }
        xSemaphoreGive(s_lock);

        if (!plan.draw) {
            if (wait_forever) {
                xSemaphoreTake(s_wake, portMAX_DELAY);
            } else {
                TickType_t ticks = pdMS_TO_TICKS(wake_wait_ms);
                if (ticks == 0) {
                    ticks = 1;
                }
                xSemaphoreTake(s_wake, ticks);
            }
            continue;
        }

#ifdef CONFIG_PM_ENABLE
        if (s_pm_lock) {
            (void)esp_pm_lock_acquire(s_pm_lock);
        }
#endif
        const int64_t t0 = esp_timer_get_time();
        int err = epd_draw_ex(s_work, plan.full ? 1 : 0);
        const int64_t t1 = esp_timer_get_time();
#ifdef CONFIG_PM_ENABLE
        if (s_pm_lock) {
            (void)esp_pm_lock_release(s_pm_lock);
        }
#endif
        const uint32_t ms = (uint32_t)((t1 - t0) / 1000);
        const uint32_t end_ms = (uint32_t)(t1 / 1000);

        xSemaphoreTake(s_lock, portMAX_DELAY);
        stats_update(&plan, err, ms, end_ms);
        xSemaphoreGive(s_lock);
    }
}

/* 初始化 display 任务与面板(D8)。返回 0 = epd_init 成功且任务创建成功;
 * 失败非致命,只把原因记进 s_stats.last_err(-1:epd_init/同步对象/任务
 * 创建失败)并让 display_ready() 保持 false。本函数只做一次:重复调用
 * 直接返回首次结果,不会重试。 */
int display_init(void)
{
    if (s_lock)
        return s_ready ? 0 : -1;   /* 重复调用直接返回上次结果 */

    memset(&s_stats, 0, sizeof(s_stats));
    s_stats.gc_partials = READER_GC_PARTIALS;
    s_stats.gc_idle_ms = (uint16_t)READER_GC_IDLE_MS;

    s_lock = xSemaphoreCreateMutex();
    s_wake = xSemaphoreCreateBinary();
#ifdef CONFIG_PM_ENABLE
    if (!s_pm_lock) {
        (void)esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "epd", &s_pm_lock);
    }
#endif
    if (!s_lock || !s_wake) {
        ESP_LOGE(TAG, "sync object alloc failed");
        s_stats.last_err = -1;
        if (s_lock) {
            vSemaphoreDelete(s_lock);
            s_lock = NULL;
        }
        if (s_wake) {
            vSemaphoreDelete(s_wake);
            s_wake = NULL;
        }
        return -1;
    }

    int err = epd_init();
    s_stats.family = epd_family();   /* 1=BOE, 3=OED/非 BOE, 0=未知(§4.5) */
    s_stats.last_err = err;
    if (err != 0) {
        ESP_LOGE(TAG, "epd_init failed (%d); display offline", err);
        s_ready = false;
        return err;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(display_task, DISPLAY_TASK_NAME,
                                            DISPLAY_TASK_STACK, NULL,
                                            DISPLAY_TASK_PRIO, &s_task,
                                            DISPLAY_TASK_CORE);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "display task create failed");
        s_stats.last_err = -1;
        s_ready = false;
        return -1;
    }

    s_ready = true;
    ESP_LOGI(TAG, "ready family=%d gc_partials=%u gc_idle_ms=%u", s_stats.family,
             s_stats.gc_partials, s_stats.gc_idle_ms);
    return 0;
}

/* D8:true 只表示"epd_init 成功且 display 任务已创建"。未调用 init、epd_init
 * 失败(如 SPI 分配失败)或任务创建失败时都是 false,此时 display_post()
 * 静默丢帧;具体原因看 display_get_stats().last_err。 */
bool display_ready(void)
{
    return s_ready;
}

void display_post(const uint8_t *fb, display_mode_t mode)
{
    if (!fb || !s_lock || !s_ready)
        return;
    /* Latest-wins:返回后 app 可立刻复用 fb(DESIGN §3.2 item 1)。 */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_sleeping) {
        xSemaphoreGive(s_lock);
        return;
    }
    memcpy(s_pending, fb, READER_FB_BYTES);
    s_mode = mode;
    s_dirty = true;
    xSemaphoreGive(s_lock);
    if (s_wake)
        xSemaphoreGive(s_wake);
}

void display_flush_wait(uint32_t timeout_ms)
{
    if (!s_lock)
        return;
    const uint32_t t0 = now_ms();
    for (;;) {
        bool idle;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        idle = !s_dirty && !s_busy;   /* 队列空 + 面板 idle */
        xSemaphoreGive(s_lock);
        if (idle)
            return;
        if (timeout_ms == 0 || (uint32_t)(now_ms() - t0) >= timeout_ms)
            return;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

void display_sleep(void)
{
    if (!s_ready || !s_lock)
        return;
    display_flush_wait(8000);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_sleeping = true;
    s_dirty = false;
    s_force_full = false;
    s_heal_pending = false;
    if (!s_busy) {
        epd_deep_sleep();
    }
    xSemaphoreGive(s_lock);
}

void display_get_stats(display_stats_t *out)
{
    if (!out)
        return;
    if (!s_lock) {
        /* 未初始化,或初始化在创建同步对象阶段就失败(此时没有 display
         * 任务,也没有并发写者)。直接返回静态快照,保留 display_init()
         * 记录的 last_err,便于 INFO/日志看到失败原因(D8)。 */
        *out = s_stats;
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_stats;
    xSemaphoreGive(s_lock);
}

/* D6:把 epd_diag() 透传给 app(INFO/日志)。纯诊断快照:与 display 任务的
 * 刷新并发时,驱动内部计数和 BUSY 电平可能不是同一时刻的值,不要用它判断
 * 时序;它也不会读写 s_pending/s_work。 */
void display_epd_diag(char *out, size_t cap)
{
    if (!out || cap == 0)
        return;
    out[0] = '\0';
    if (cap > (size_t)INT_MAX)
        cap = (size_t)INT_MAX;
    epd_diag(out, (int)cap);
}

void display_request_full(void)
{
    if (!s_lock)
        return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_force_full = true;
    xSemaphoreGive(s_lock);
    if (s_wake)
        xSemaphoreGive(s_wake);
    ESP_LOGI(TAG, "full refresh requested");
}

void display_set_gc(uint8_t partials, uint16_t idle_ms)
{
    if (!s_lock)
        return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_stats.gc_partials = partials;
    s_stats.gc_idle_ms = idle_ms;
    if (partials == 0) {
        s_stats.gc_pending = false;      /* 0 关闭自动 GC */
    } else if (s_stats.partial_since_full >= partials) {
        s_stats.gc_pending = true;
    }
    xSemaphoreGive(s_lock);
    if (s_wake)
        xSemaphoreGive(s_wake);
    ESP_LOGI(TAG, "gc set partials=%u idle_ms=%u", partials, idle_ms);
}
