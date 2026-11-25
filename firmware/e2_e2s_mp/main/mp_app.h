#pragma once
/* mp_app.h — 应用状态机（纯 C 声明；.c 里才会碰 ESP-IDF）。
 *
 * 一个任务（app 任务）拥有：按键归约、屏幕状态、mp_store 的全部调用、
 * 面板提交。BLE 只把包投进 mp_ble 的队列，由 app 任务调 mp_app_ble_req() 处理。
 * 这样 mp_store 永远是单任务访问（platform.md §2.3 的硬要求）。
 *
 * 主循环（mp_app_run）每一轮的顺序是**冻结**的：
 *   1) while (mp_ble_req_get(&r, 0)) mp_app_ble_req(&r);   // 先抽干 BLE
 *   2) input_get(&ev, <=2 ms)                              // 再取一个按键
 *   3) 到期的延迟 OK 单击 / 1 Hz 的 tick（含 power_tick）
 *   4) 有脏标志才渲染 + display_post
 * 上传期间跳过 4) 与电量采样；队列深 48（mp_ble.h）保证 web 的 5832 B 窗口不丢。
 * DEBUG（debug_mode）下只处理 0x41 / MENU 长按 / 30 s 超时，不渲染、不响应其它键。
 *
 * 栈：mp_app_run 所在任务栈 **>= 8192 B**（锁图 4000 B 由 static 缓冲提供，
 * 但仍要放 mp_ui_state_t ~800 B + mp_page_t ~470 B + 调用深度）。
 */
#include <stdbool.h>
#include <stdint.h>

#include "mp_ble.h"
#include "mp_page.h"
#include "mp_ui.h"

/* 时序（与 Miaopaper 一致）：1.0 s 长按；350 ms 双击窗口。 */
#define MP_LONG_MS 1000u
#define MP_DCLICK_MS 350u

/* 自动锁屏用 mp_settings.sleep_timeout_idx 索引 mp_sleep_timeout_s[]。 */
#define MP_SLEEP_IDX_DEFAULT 2      /* 3 分钟 (180 s) */

/* 蓝牙自动关闭策略：锁屏 2 分钟无通信，或超过 10 分钟未使用自动关闭。 */
#define MP_BLE_LOCK_OFF_MS 120000u
#define MP_BLE_IDLE_OFF_MS 600000u

/* 锁屏深睡：无蓝牙时锁屏满 10 s 进入深睡；唤醒需持续按住确认键 >= 500 ms 防误触。 */
#define MP_LOCK_SLEEP_GRACE_MS 10000u
#define MP_LOCK_SLEEP_AFTER_BLE_MS 2000u
#define MP_WAKE_HOLD_MS 500u

/* 低电关机门限：未充电且电量 < 3% 时全刷空电池提示屏并进入 deep sleep。 */
#define MP_LOW_BATT_SHUTDOWN_PCT 3u

/* 进度落盘节流：翻页只写 RAM，空闲满 2 s / 关书 / 锁屏 / MENU 才 fsync。 */
#define MP_POS_FLUSH_MS 2000u

typedef struct {
    /* 屏幕 */
    uint8_t screen;                 /* mp_screen_t */
    uint8_t prev_screen;            /* 进 LOCK / RESTORE 前的屏 */
    uint8_t locked_from_read;       /* 1 = 解锁后回阅读页并全刷 */
    uint8_t lock_prompt;            /* 1 = 锁屏图按其他键后正显示解锁提示页 */
    uint32_t lock_prompt_until_ms;  /* 解锁提示页自动恢复锁屏图的到期时刻 */
    uint8_t debug_mode;             /* 0x40 进入：屏蔽时钟重绘与按键，0x41 清除 */
    uint8_t bl_popup;               /* 1 = 叠加快捷背光调整浮窗 */
    uint8_t bl_edit_level;          /* 设置页/浮窗当前预览背光档位 0..3 */
    uint8_t bl_saved_level;         /* 进入背光页/浮窗前保存的原档位 */

    /* 列表光标 */
    uint8_t sel;                    /* SELECT */
    uint8_t set_sel;                /* SETTINGS（显示行号） */
    uint8_t info_sel;               /* BOOK_INFO 动作 0..3 */
    uint8_t jump_pct;               /* JUMP 目标百分比 0..100 */
    uint8_t time_step;              /* TIME_SET 0=年..4=分 */
    uint16_t set_year;              /* TIME_SET 的待提交值 */
    uint8_t set_month, set_day;
    uint8_t set_hour, set_min;
    uint32_t time_done_until_ms;    /* TIME_DONE 自动返回 SETTINGS */

    /* 当前书 */
    uint8_t book_open;              /* 1 = 有打开的书 */
    uint8_t book_idx;               /* 目录槽 0..7 */
    uint32_t char_pos;              /* 本页首字节 */
    uint32_t book_len;              /* min(目录声明长度, stat 真实大小) */
    mp_page_t page;                 /* 当前页（measure 的结果） */

    /* 进度落盘（RAM 优先） */
    uint8_t pos_dirty;              /* 1 = char_pos 还没写进 progress.bin */
    uint32_t pos_dirty_ms;          /* 变脏的时刻；空闲 >= MP_POS_FLUSH_MS 落盘 */

    /* 时钟：同步基准 + 单调毫秒，tick 时重算（跨分钟/跨午夜/跨月） */
    uint32_t time_base_unix;        /* 最近一次 0xDD 的 unix（或默认 1767225600） */
    uint32_t time_base_ms;          /* 设置基准时的 rc_now_ms() */
    uint8_t hour, min, day, month;
    uint8_t week;                   /* 1=周一 … 7=周日；跨日时 week = week%7+1 */

    /* 运行时 */
    uint8_t storage_ok;             /* 0 -> MP_SCREEN_ERROR */
    uint8_t has_backlight;          /* 1 = 检测到 AW2023 背光芯片（E2S），0 = 无背光（E2） */
    uint8_t uploading;              /* 1 = BLE 上传会话中（停重绘/停采样） */
    uint8_t restore_armed;          /* 1 = 确认页已上屏且已抽干输入，可接受提交 */
    uint8_t restore_key_ok;         /* 1 = armed 之后见过一次 OK DOWN */
    uint8_t pending_ok;             /* OK 单击延迟计时中（全局 350 ms 门） */
    uint32_t pending_ok_ms;
    uint32_t last_input_ms;         /* 自动锁屏计时 */
    uint32_t lock_enter_ms;         /* 进入锁屏时刻（用于锁屏 2 分钟关蓝牙与深睡计时） */
    uint32_t ble_active_ms;         /* 最近一次蓝牙开启或通信活跃时刻（用于 10 分钟空闲关蓝牙） */
    uint32_t ble_off_ms;            /* 最近一次关闭蓝牙时刻（用于锁屏关蓝牙后进深睡宽限） */
    uint32_t last_pkt_ms;           /* 最近一次 BLE 上传数据；60 s 无包 -> abort */
    uint32_t debug_until_ms;        /* DEBUG 自动退出时刻（0 = 不在 debug） */
} mp_app_state_t;

extern mp_app_state_t mp_app_state;

/* ---- 启动与主循环 -------------------------------------------------- */

/* 在外设初始化前执行：解除深睡 GPIO 保持、恢复 RTC 引脚，并过滤锁屏深睡短按误触。 */
void mp_app_early_wake_check(void);

/* 由 main.c 在初始化完 display/storage/mp_store/input/power/mp_ble 后调用，
 * 永不返回。循环顺序见文件头。 */
void mp_app_run(void);

/* 进入主循环前的状态初始化：读目录、算首帧、应用 GC 设置（display_set_gc）、
 * 初始化时钟基准。app_main 里调一次。
 *
 * 存储：main.c 已按 PORT_SPEC §10.2 调过 storage_init()/mp_store_init()；这里会
 * 再确认一次（storage_sd_ok() 为真时重跑 mp_store_init()，它是幂等的），
 * 结果写进 mp_app_state.storage_ok。存储不可用时本函数在 RAM 里套用一份默认
 * 设置（蓝牙开 / 锁屏 3 分钟 / 锁屏展示阅读数据 / 场景全刷 / GC 10）并进 ERROR 页，
 * 所以 main.c 漏调 mp_settings_defaults() 也不会把蓝牙静默关掉。 */
void mp_app_boot(void);

/* ---- 事件入口（都只允许 app 任务调用） ------------------------------ */

/* 按键归约。key/ev 取 input.h 的 key_id_t / key_ev_type_t；now_ms=rc_now_ms()。
 * 五键分工（左->右/上->下：左/右/胶囊菜单/○确认/×取消）：
 *   - PREV/NEXT (左◀/右▶)：翻页、选单移动、数值加减、快捷浮窗调档；
 *   - OK (○确认)：单击 350 ms 门（READ 双击=上一页）；长按=锁屏/解锁/提交确认；
 *   - FUNC (▭胶囊菜单)：READ 短按=书籍菜单，其它常规屏短按=整屏刷新，
 *                       长按仅在有背光硬件时弹出快捷背光调整浮窗；
 *   - MENU (×取消/返回)：短按=取消/返回上页（READ 退出回书库）；长按=回时钟/退 DEBUG。
 * KEY_EV_REPEAT 一律忽略。 */
void mp_app_key(uint8_t key, uint8_t ev, uint32_t now_ms);

/* 一条 BLE 请求（op 已剥离）。内部完成 0x10..0x44 的全部语义并调用
 * mp_ble_notify() 回包。**锁屏（MP_SCREEN_LOCK）时只放行
 * 0x14/0x15/0x19/0x33/0x34/0x35**，其余回 MP_ST_FAIL，不切屏、不点屏。 */
void mp_app_ble_req(const mp_ble_req_t *req);

/* 每秒调用一次：自动锁屏计时 + 时钟进位 + 电量刷新（上传中跳过）。 */
void mp_app_tick(uint32_t now_ms);

/* 把 RAM 里的 char_pos 落盘（§6.4 的 flush 时机都走这里；锁屏/关书/恢复提交前各调一次）。 */
void mp_app_flush_progress(void);

/* ---- 供 main.c / 测试查询 ------------------------------------------- */

/* 当前是否需要刷新（调试/日志用）。 */
uint8_t mp_app_dirty(void);

/* 取视图模型（渲染前由 mp_app 自己用；也供诊断工具调用）。
 * 返回 false 表示当前状态无法生成（例如未初始化）。 */
bool mp_app_view(mp_ui_state_t *out);
