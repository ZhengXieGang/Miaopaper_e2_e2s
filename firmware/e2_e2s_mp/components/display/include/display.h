#pragma once
/* display.h — frozen display API (DESIGN.md §3.2, §4, §17.2)
 *
 * Only the display task ever touches the panel. app owns app_fb, calls
 * display_post(), and may reuse app_fb as soon as the call returns
 * (memcpy into s_pending under the display lock).
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    DISPLAY_MODE_AUTO = 0,   /* partial when baseline is valid, else full */
    DISPLAY_MODE_FULL,
    DISPLAY_MODE_PARTIAL,
} display_mode_t;

typedef struct {
    uint32_t frames, fulls, partials;
    /* 时间戳(开机毫秒,esp_timer 单调时钟),**不是耗时**:
     *   last_ms      = 最近一次刷新尝试的结束时刻(成功、失败都更新);
     *   last_full_ms = 最近一次全刷尝试的结束时刻(成功、失败都更新)。
     * app 侧用 rc_now_ms() - last_ms 判 DESIGN §4.4 的 5 s 间隔,两者同源。
     * 每次刷新的耗时只进日志(display 任务的 "full/partial ms=")。 */
    uint32_t last_ms, last_full_ms;
    uint32_t partial_since_full;
    bool baseline_valid, gc_pending;
    uint8_t gc_partials;
    uint16_t gc_idle_ms;
    int family;              /* 1=BOE, 3=OED/non-BOE, 0=unknown/failed (§4.5) */
    int last_err;
} display_stats_t;

/* 初始化 display 任务与面板。返回 0 表示 epd_init 成功且 display 任务创建
 * 成功。失败非致命:app 继续运行,display_ready() 保持 false(DESIGN §13)。
 * 只应调用一次;重复调用不再重试,直接返回首次结果。失败原因记录在
 * display_get_stats().last_err(非 0;当前实现:epd_init/同步对象/任务
 * 创建失败都返回 -1)。 */
int display_init(void);

/* true 仅当 display_init() 已执行且 epd_init() 与 display 任务创建都成功。
 * 未初始化、epd_init 失败或任务创建失败时均为 false;此时 display_post()
 * 会静默丢弃帧。失败原因见 display_get_stats().last_err。 */
bool display_ready(void);

/* Copy a 250x122 logical frame and request a refresh. Latest wins: a pending
 * frame is overwritten by a newer post. Safe to call from the app task. */
void display_post(const uint8_t *fb, display_mode_t mode);

/* Block until the queue is empty and the panel is idle, or timeout_ms. */
void display_flush_wait(uint32_t timeout_ms);

/* 刷新队列全部上屏后令面板进入深睡并停止后续刷新，保持当前画面。 */
void display_sleep(void);

void display_get_stats(display_stats_t *out);

/* Force the next refresh to be full (FUNC long press / BLE 0x40). */
void display_request_full(void);

/* Runtime GC tuning, RAM only (BLE 0x41 / DESIGN §4.4). partials=0 disables
 * automatic GC. */
void display_set_gc(uint8_t partials, uint16_t idle_ms);

/* EPD 驱动内部诊断(init/stage/family/BUSY 电平/写入字节/耗时/温度/ID/
 * 最近 8 次 BUSY 等待轨迹),写成一行以 NUL 结尾的文本,超长截断。
 * 供 app 的 INFO 页或日志使用;epd 初始化失败时也可调用,会输出失败现场。
 * 纯诊断快照:与 display 任务的刷新并发调用时,各字段可能不是同一时刻的
 * 值,不要用它判断时序。out==NULL 或 cap==0 时不做任何事。 */
void display_epd_diag(char *out, size_t cap);
