#pragma once
/* input.h — frozen key API (DESIGN.md §7, §17.2)
 *
 * 20 ms polling, 3-sample debounce, DOWN/CLICK/LONG/REPEAT, 32-entry queue.
 * The queue policy distinguishes page keys from non-page keys (DESIGN §7.2):
 * input_dropped counts only discarded PREV/NEXT events.
 */
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    KEY_PREV = 0,
    KEY_NEXT,
    KEY_MENU,
    KEY_OK,
    KEY_FUNC,
    KEY_COUNT,
} key_id_t;

typedef enum {
    KEY_EV_DOWN = 1,
    KEY_EV_CLICK,
    KEY_EV_LONG,
    KEY_EV_REPEAT,
} key_ev_type_t;

typedef struct {
    uint8_t id;
    uint8_t type;
    uint32_t held_ms;
} key_event_t;

int input_init(void);

/* 注册按键入队唤醒回调（供 app 任务从阻塞态即时醒来，回调需极轻量）。 */
void input_set_wake_cb(void (*cb)(void));

/* Wait up to timeout_ms. Returns false on timeout. */
bool input_get(key_event_t *ev, uint32_t timeout_ms);

/* Bitmask of currently held keys (diagnostics / INFO / BLE STATUS). */
uint32_t input_mask(void);
uint32_t input_dropped(void);        /* page events dropped only */
uint32_t input_lost_critical(void);  /* non-page events dropped; should be 0 */

/* 自测/主机工具用:合成一次“短按”,事件类型固定为
 *   PREV/NEXT   → KEY_EV_DOWN(状态机在按下沿翻页,一次 inject 只翻一页)
 *   MENU/OK/FUNC→ KEY_EV_CLICK (状态机对这些键的短按只在 CLICK 上定义)
 * 因此一次 input_inject() 恰好触发一次按键效果,不会再补发 CLICK/LONG/REPEAT。
 * 该函数是线程安全的(内部走与 keys 任务相同的入队路径)。 */
void input_inject(key_id_t id);
