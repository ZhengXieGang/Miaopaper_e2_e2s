/* mp_app.c — 应用状态机（CLOCK/SELECT/READ/SETTINGS/ABOUT/LOCK/EMPTY/ERROR/
 * RESTORE/BOOK_INFO/JUMP/DELETE/TIME_SET/TIME_DONE）。
 *
 * 所有权（PORT_SPEC §10.1）：本文件是 mp_store_* 的唯一调用者、display_post 的唯一
 * 调用者、按键归约与恢复流程的唯一实现。BLE 只会把包投进 mp_ble 的队列，由本任务
 * 抽出（mp_app_run 第一步）——因此 mp_store 永远单任务访问。
 *
 * 冻结的行为来源：
 *   - 按键矩阵：PORT_SPEC §2.4（Miaopaper 的三手势：CLICK / LONG / OK 双击）
 *   - OK 的 350 ms 多击门：PORT_SPEC §2.3（逐行照抄那段伪代码；READ 上双击=上一页）
 *   - 文案：docs/COPY_DECK.md（逐字；本文件只放 COPY_DECK 里有的字符串）
 *   - 刷新：PORT_SPEC §5（scene_commit / page_commit；锁屏与错误页 force_full=1）
 *   - 恢复原厂：PORT_SPEC §8（probe 通过才进确认页；确认页真的上屏才 armed；
 *     armed 之后还要见到新的 OK DOWN 才接受 OK LONG 提交）
 *   - 存储：只写 /sdcard/mp（全部经 mp_store_*）；不碰 NVS / 内部 flash / 分区表；
 *     otadata 只有 recovery_stock_commit() 会写。
 *
 * 按键结论（§2.1）：五键都只在 CLICK/LONG 上动作；DOWN 不产生动作（只用来取消
 * 别的键的待发 OK 单击 / 记录 restore_key_ok）；REPEAT 一律丢弃。
 *
 * 屏上不出现 MENU/OK/FUNC/PREV/NEXT 等键名：底栏由 mp_ui_render() 调
 * mp_hints_for()/mp_hints_draw() 画键帽图标（本文件不直接画底栏）。
 */
#include "mp_app.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "esp_log.h"
#include "sdkconfig.h"
#ifdef CONFIG_PM_ENABLE
#include "esp_pm.h"
#endif
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_vfs_fat.h"

#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board.h"
#include "display.h"
#include "input.h"
#include "mp_backlight.h"
#include "mp_hints.h"
#include "mp_page.h"
#include "mp_store.h"
#include "mp_ui.h"
#include "power.h"
#include "rc_common.h"
#include "reader_cfg.h"
#include "recovery.h"
#include "storage.h"
#include "text.h"

/* 五键功能语义宏（对齐物理键位：左/右/胶囊菜单/○确认/×取消）。 */
#define MP_KEY_LEFT   KEY_PREV  /* GPIO2: 左 ◀ */
#define MP_KEY_RIGHT  KEY_NEXT  /* GPIO6: 右 ▶ */
#define MP_KEY_MENU   KEY_FUNC  /* GPIO4: 胶囊菜单 ▭ */
#define MP_KEY_OK     KEY_OK    /* GPIO8: 确认 ○ */
#define MP_KEY_CANCEL KEY_MENU  /* GPIO7: 取消/返回 × */

static const char *TAG = "mp_app";

/* mp_page.h 与 mp_store.h 都定义这两个宏；值必须一致（C 允许同值重复定义）。 */
_Static_assert(MP_ENC_ASCII == 0, "MP_ENC_ASCII drifted");
_Static_assert(MP_ENC_GB2312 == 1, "MP_ENC_GB2312 drifted");
_Static_assert(MP_FB_BYTES == READER_FB_BYTES, "frame size drifted");
_Static_assert(MP_TITLE_MAX == MP_BLE_TITLE_MAX, "title length drifted");

/* COPY_DECK §3.7：构建行用编译期常量，不用 __DATE__（可复现构建）。 */
#define MP_BUILD_DATE "2026-11-25"

/* 未同步时的默认时间：2026-01-01 00:00:00（PORT_SPEC §7.5）。 */
#define MP_TIME_DEFAULT_UNIX 1767225600u
#define MP_TIME_DEFAULT_YEAR 2026
#define MP_TIME_DEFAULT_WEEK 4       /* 2026-01-01 = 周四（1=周一 … 7=周日） */

/* 进度落盘节流：mp_app.h 已给 MP_POS_FLUSH_MS。 */

mp_app_state_t mp_app_state;

/* ------------------------------------------------------------------ *
 * 私有状态
 * ------------------------------------------------------------------ */

static const char *const k_week_cn[8] = {"", "周一", "周二", "周三",
                                         "周四", "周五", "周六", "周日"};

/* 帧 / 锁图：.bss（PORT_SPEC §9.7：4000 B 不许放栈上）。 */
static uint8_t s_fb[MP_FB_BYTES];
static uint8_t s_lock_img[MP_BLE_LOCK_BYTES];
static uint8_t s_lock_valid;

/* 视图模型（~800 B，静态以避免吃主任务栈）。 */
static mp_ui_state_t s_ui;

/* 目录缓存：s_done[i] 是第 i 本 DONE 书的槽号，s_books[i] 是它的目录项。 */
static uint8_t s_done[MP_MAX_BOOKS];
static mp_book_t s_books[MP_MAX_BOOKS];
static uint8_t s_done_n;

/* 当前书的渲染属性（mp_app_state 里没有 enc/title 字段）。 */
typedef struct {
    uint8_t idx;
} book_ctx_t;
static book_ctx_t s_book_ctx;
static uint8_t s_book_enc = MP_ENC_ASCII;
static uint8_t s_title_enc = MP_STR_ASCII;
static char s_book_title[MP_TITLE_MAX + 1];
static uint16_t s_book_chars_k;      /* mp_store 目录项里的字数/1024 */

/* 走时。s_year 只用于闰年二月（PORT_SPEC §7.5 不重算 year，0xDD 会重发）。 */
static uint16_t s_year = MP_TIME_DEFAULT_YEAR;
static uint32_t s_day_index;         /* unix/86400，用于跨日进位 */

/* BLE 时间同步（host 任务写、app 任务读；先拷贝再清标志）。 */
static volatile uint8_t s_time_flag;
static mp_time_t s_time_inbox;
static volatile uint8_t s_conn_lost; /* 断连事件：会话要被回收 */
static volatile uint8_t s_conn_up;   /* 刚连上：2 s 后主动发一次 0x19（§7.6） */
static uint8_t s_status_pending;
static uint32_t s_status_due_ms;

/* BLE 上传会话（同一时刻只允许一个）。 */
enum { UP_NONE = 0, UP_BOOK, UP_FONT, UP_LOCK };
static uint8_t s_up_kind;
static uint8_t s_up_slot = 0xFF;
static uint32_t s_up_written;
static uint32_t s_up_total;
static uint32_t s_up_notified;
static uint32_t s_up_chars;          /* 实时统计的字数（信息页用） */
static uint8_t s_up_enc;
static uint8_t s_up_gbk_lead;

/* 错误页种类（MP_SCREEN_ERROR 的三种来源，PORT_SPEC §1.9.7 / COPY_DECK §2.8/§2.10）。 */
enum { ERR_NONE = 0, ERR_STORAGE, ERR_RECOVERY, ERR_DISPLAY };
static uint8_t s_error_kind;
static int s_error_rc;

/* 电量去重（用于 0x2A19 / 广播）。 */
static uint8_t s_last_batt;
static uint8_t s_batt_valid;

static uint8_t s_dirty;
static uint8_t s_booted;
static uint8_t s_stats_dirty;
static uint32_t s_read_ts_ms;
static uint16_t s_read_rem_ms;
static uint8_t s_read_tracking;
static uint32_t s_stats_last_save_ms;
static TaskHandle_t s_app_task;

/* 跨深睡保持状态：保存在 RTC 慢速内存（重启清零、深睡保留）。 */
#define MP_RTC_MAGIC 0x4D505331u
RTC_DATA_ATTR static struct {
    uint32_t magic;
    uint8_t deep_locked;       /* 1 = 由锁屏进入深睡，醒来按需回原画面 */
    uint8_t locked_from_read;  /* 1 = 唤醒后直接恢复上次阅读页 */
    uint8_t book_idx;
    uint8_t time_synced;       /* 1 = POSIX RTC 已校准过有效时间 */
    uint32_t char_pos;
} s_rtc;
static uint8_t s_woke_unlock;  /* 本次冷启动是否通过长按 ○ 从锁屏深睡成功唤醒 */

#ifdef CONFIG_PM_ENABLE
static esp_pm_lock_handle_t s_pm_cpu_lock;
static esp_pm_lock_handle_t s_pm_nosleep_lock;
static uint8_t s_pm_upload_held;
#endif

/* 视图文本缓冲（mp_ui_state 只存指针；同一时刻只渲染一屏）。 */
static char s_buf_date[48];
static char s_buf_mid[24];
static char s_buf_space[64];
static char s_buf_set[8][32];
static char s_buf_about[4][72];
static char s_buf_info[96];
static char s_buf_info_act[4][32];

/* ------------------------------------------------------------------ *
 * 小工具
 * ------------------------------------------------------------------ */

static uint32_t now_ms(void)
{
    return (uint32_t)rc_now_ms();
}

static void app_wake(void)
{
    if (s_app_task != NULL) {
        xTaskNotifyGive(s_app_task);
    }
}

static void ble_touch(uint32_t now)
{
    mp_app_state.ble_active_ms = now;
}

static void upload_pm_acquire(void)
{
#ifdef CONFIG_PM_ENABLE
    if (s_pm_upload_held) {
        return;
    }
    if (s_pm_cpu_lock) {
        (void)esp_pm_lock_acquire(s_pm_cpu_lock);
    }
    if (s_pm_nosleep_lock) {
        (void)esp_pm_lock_acquire(s_pm_nosleep_lock);
    }
    s_pm_upload_held = 1;
#endif
}

static void upload_pm_release(void)
{
#ifdef CONFIG_PM_ENABLE
    if (!s_pm_upload_held) {
        return;
    }
    s_pm_upload_held = 0;
    if (s_pm_nosleep_lock) {
        (void)esp_pm_lock_release(s_pm_nosleep_lock);
    }
    if (s_pm_cpu_lock) {
        (void)esp_pm_lock_release(s_pm_cpu_lock);
    }
#endif
}

/* 锁存分压控制(GPIO45=低)与 SD 电源脚(GPIO34=高)，消除深睡期间漏电。 */
static void sleep_hold_power_pins(void)
{
    /* 先解除运行期锁存状态，再切向高电平确保 SD 与外设可靠断电 */
    (void)gpio_hold_dis(BOARD_VBAT_EN_GPIO);
    (void)gpio_hold_dis(BOARD_SD_PWR_GPIO);
    (void)gpio_set_direction(BOARD_VBAT_EN_GPIO, GPIO_MODE_OUTPUT);
    (void)gpio_set_level(BOARD_VBAT_EN_GPIO, 0);
    (void)gpio_set_direction(BOARD_SD_PWR_GPIO, GPIO_MODE_OUTPUT);
    (void)gpio_set_level(BOARD_SD_PWR_GPIO, 1);
    (void)gpio_hold_en(BOARD_VBAT_EN_GPIO);
    (void)gpio_hold_en(BOARD_SD_PWR_GPIO);
    gpio_deep_sleep_hold_en();
}

/* 配置 EXT1 低电平唤醒并进入 deep sleep（永不返回）。 */
static void __attribute__((noreturn)) enter_ext1_deep_sleep(uint64_t wake_mask)
{
    if (wake_mask & (1ULL << BOARD_CHARGE_DET_GPIO)) {
        (void)rtc_gpio_init(BOARD_CHARGE_DET_GPIO);
        (void)rtc_gpio_set_direction(BOARD_CHARGE_DET_GPIO,
                                     RTC_GPIO_MODE_INPUT_ONLY);
        (void)rtc_gpio_pulldown_dis(BOARD_CHARGE_DET_GPIO);
        (void)rtc_gpio_pullup_en(BOARD_CHARGE_DET_GPIO);
    }
    if (wake_mask & (1ULL << BOARD_KEY_OK_GPIO)) {
        (void)rtc_gpio_init(BOARD_KEY_OK_GPIO);
        (void)rtc_gpio_set_direction(BOARD_KEY_OK_GPIO,
                                     RTC_GPIO_MODE_INPUT_ONLY);
        (void)rtc_gpio_pulldown_dis(BOARD_KEY_OK_GPIO);
        (void)rtc_gpio_pullup_en(BOARD_KEY_OK_GPIO);
    }
    (void)esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    (void)esp_sleep_enable_ext1_wakeup_io(wake_mask, ESP_EXT1_WAKEUP_ANY_LOW);
    esp_deep_sleep_start();
}

void mp_app_early_wake_check(void)
{
    gpio_deep_sleep_hold_dis();
    (void)gpio_hold_dis(BOARD_SD_PWR_GPIO);
    (void)gpio_hold_dis(BOARD_VBAT_EN_GPIO);
    (void)rtc_gpio_deinit(BOARD_CHARGE_DET_GPIO);
    (void)rtc_gpio_deinit(BOARD_KEY_OK_GPIO);

    if ((esp_sleep_get_wakeup_causes() & (1UL << ESP_SLEEP_WAKEUP_EXT1)) &&
        s_rtc.magic == MP_RTC_MAGIC && s_rtc.deep_locked) {
        gpio_config_t ok_cfg = {
            .pin_bit_mask = 1ULL << BOARD_KEY_OK_GPIO,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        (void)gpio_config(&ok_cfg);

        uint32_t elapsed = 0;
        while (gpio_get_level(BOARD_KEY_OK_GPIO) == 0 &&
               elapsed < MP_WAKE_HOLD_MS) {
            vTaskDelay(pdMS_TO_TICKS(20));
            elapsed += 20;
        }
        if (elapsed >= MP_WAKE_HOLD_MS) {
            s_woke_unlock = 1;
            s_rtc.deep_locked = 0;
            return;
        }

        /* 短按误触：不挂载存储/屏幕，原样回到深睡状态。 */
        sleep_hold_power_pins();
        enter_ext1_deep_sleep(1ULL << BOARD_KEY_OK_GPIO);
    }

    s_woke_unlock = 0;
    if (s_rtc.magic == MP_RTC_MAGIC) {
        s_rtc.deep_locked = 0;
    }
}

static uint32_t frames_now(void)
{
    display_stats_t ds;
    display_get_stats(&ds);
    return ds.frames;
}

/* 电量/时间在状态栏上是每屏都有的；power_get() 在 power_init 之前也可能被调。 */
static const power_status_t *pw(void)
{
    static const power_status_t dummy;
    const power_status_t *p = power_get();
    return p ? p : &dummy;
}

/* 标题的绘制编码：出现任意 >=0xA1 的字节就按 GB2312 画（PORT_SPEC §6.6-14）。
 * 纯 ASCII 书名按 GB2312 画会把每个字母画成全角。 */
static uint8_t title_enc(const char *t)
{
    const uint8_t *p = (const uint8_t *)t;
    if (!p) {
        return MP_STR_ASCII;
    }
    while (*p) {
        if (*p >= 0xA1) {
            return MP_STR_GB2312;
        }
        p++;
    }
    return MP_STR_ASCII;
}

/* ESP 的 FAT VFS 容量查询（mp_store.h 还没有 mp_store_space()；见交付说明）。
 * 只读，不写任何东西。 */
static int store_space(uint64_t *total, uint64_t *free_bytes)
{
    if (!mp_app_state.storage_ok || !total || !free_bytes) {
        return -1;
    }
    return esp_vfs_fat_info("/sdcard", total, free_bytes) == ESP_OK ? 0 : -1;
}

/* 目录重扫：DONE 槽按槽号升序（mp_store_list_done 的顺序）。 */
static void rescan_books(void)
{
    s_done_n = 0;
    if (!mp_app_state.storage_ok) {
        return;
    }
    int n = mp_store_list_done(s_done);
    if (n < 0) {
        n = 0;
    }
    if (n > MP_MAX_BOOKS) {
        n = MP_MAX_BOOKS;
    }
    for (int i = 0; i < n; i++) {
        if (mp_store_read(s_done[i], &s_books[i]) != 0) {
            memset(&s_books[i], 0, sizeof(s_books[i]));
        }
    }
    s_done_n = (uint8_t)n;
}

/* ------------------------------------------------------------------ *
 * 分页
 * ------------------------------------------------------------------ */

static int book_read_cb(void *ctx, uint32_t off, void *buf, size_t len)
{
    const book_ctx_t *c = (const book_ctx_t *)ctx;
    if (!c || !mp_app_state.storage_ok) {
        return -1;
    }
    return mp_store_read_book(c->idx, off, buf, len) == 0 ? 0 : -1;
}

static mp_book_ref_t book_ref(void)
{
    mp_book_ref_t b;
    b.read = book_read_cb;
    b.ctx = &s_book_ctx;
    b.len = mp_app_state.book_len;
    b.enc = s_book_enc;
    return b;
}

/* 量当前书从 pos 开始的一页。返回 0 = 有行；-1 = 读失败（调用方显示错误页）。 */
static int measure_page(uint32_t pos)
{
    mp_book_ref_t b = book_ref();
    (void)mp_page_measure(&b, pos, &mp_app_state.page);
    return mp_app_state.page.lines ? 0 : -1;
}

/* 进度百分比：at_end 必须先进 100，且与进度条共用同一个值（PORT_SPEC §4.6）。 */
static uint8_t read_percent(void)
{
    if (!mp_app_state.book_open) {
        return 0;
    }
    uint8_t pct = mp_app_state.page.at_end
                      ? 100
                      : mp_page_percent(mp_app_state.char_pos, mp_app_state.book_len);
    return pct > 100 ? 100 : pct;
}

/* ------------------------------------------------------------------ *
 * 刷新原语（PORT_SPEC §5.1）
 * ------------------------------------------------------------------ */

static int render(void)
{
    if (!display_ready()) {
        return -1;   /* display_post() 会静默丢帧（display.c 未就绪分支） */
    }
    if (!mp_app_view(&s_ui)) {
        return -1;
    }
    mp_ui_render(&s_ui, s_fb);
    s_dirty = 0;
    return 0;
}

/* 场景切换：按是否强刷一次性提交目标刷新模式，不再先局刷再阻塞等补全刷。
 * force_full：锁屏 / 解锁回阅读 / 开机首帧 / 错误页传 1，其余传 0（自动局刷 + 顺滑页内 GC）。
 * 上传会话期间不重绘（§5.5）：只记脏标志，等会话结束后由主循环补画。 */
static void scene_commit(uint8_t force_full)
{
    if (!display_ready()) {
        ESP_LOGW(TAG, "panel not ready; frame dropped");
        return;
    }
    if (mp_app_state.uploading) {
        s_dirty = 1;
        return;
    }
    if (render() != 0) {
        return;
    }
    display_post(s_fb, force_full ? DISPLAY_MODE_FULL : DISPLAY_MODE_AUTO);
}

/* 屏内动作（翻页 / 移光标 / 改值）：只局刷，不等。上传期间同样跳过。 */
static void page_commit(void)
{
    if (!display_ready() || mp_app_state.debug_mode) {
        return;
    }
    if (mp_app_state.uploading) {
        s_dirty = 1;
        return;
    }
    if (render() != 0) {
        return;
    }
    display_post(s_fb, DISPLAY_MODE_AUTO);
}

static void mark_dirty(void)
{
    s_dirty = 1;
}

/* ------------------------------------------------------------------ *
 * 视图模型
 * ------------------------------------------------------------------ */

static void view_common(mp_ui_state_t *v)
{
    const power_status_t *p = pw();

    memset(v, 0, sizeof(*v));
    v->screen = mp_app_state.screen;
    v->hour = mp_app_state.hour;
    v->min = mp_app_state.min;
    v->year = s_year;
    v->month = mp_app_state.month;
    v->day = mp_app_state.day;
    v->week = mp_app_state.week;
    v->time_valid = 1;
    v->bt_on = mp_settings.ble_enabled ? 1 : 0;
    v->battery = p->percent;
    v->battery_valid = p->valid ? 1 : 0;
    v->charging = p->charging ? 1 : 0;
    v->bl_popup = mp_app_state.has_backlight ? mp_app_state.bl_popup : 0;
    v->bl_level = (v->bl_popup ||
                   (mp_app_state.has_backlight &&
                    mp_app_state.screen == MP_SCREEN_BACKLIGHT))
                      ? mp_app_state.bl_edit_level
                      : mp_settings.backlight_level;
}

static void view_clock(mp_ui_state_t *v)
{
    uint8_t w = mp_app_state.week;
    if (w < 1 || w > 7) {
        w = MP_TIME_DEFAULT_WEEK;
    }
    snprintf(s_buf_date, sizeof(s_buf_date), "%02u-%02u %s",
             (unsigned)(mp_app_state.month & 0xFFu),
             (unsigned)(mp_app_state.day & 0xFFu), k_week_cn[w]);
    v->left.text = s_buf_date;
    v->left.enc = MP_STR_UTF8;
    v->mid = NULL;
    v->time_valid = 0;           /* 主页只有大字时钟 */
}

static void view_select(mp_ui_state_t *v)
{
    /* 书数跟着标题走，状态栏中段留给居中时间（评审：避免 11:30 3/8 挤在
     * 一起）。主页不显示书数。 */
    snprintf(s_buf_mid, sizeof(s_buf_mid), "书库 %u/8", (unsigned)s_done_n);
    v->left.text = s_buf_mid;
    v->left.enc = MP_STR_UTF8;
    v->mid = NULL;

    uint8_t rows = s_done_n;
    uint8_t sel = mp_app_state.sel;
    if (rows == 0) {
        sel = 0;
    } else if (sel >= rows) {
        sel = (uint8_t)(rows - 1);
    }
    v->row_count = rows;
    v->sel = sel;
    v->scroll = mp_ui_list_scroll(sel, rows);

    for (uint8_t i = 0; i < MP_LIST_ROWS; i++) {
        uint8_t idx = (uint8_t)(v->scroll + i);
        if (idx < s_done_n) {
            v->row[i].label = s_books[idx].title;
            v->row[i].enc = title_enc(s_books[idx].title);
        } else {
            v->row[i].label = NULL;
            v->row[i].enc = MP_STR_UTF8;
        }
    }
}

static void view_read(mp_ui_state_t *v)
{
    uint8_t pct = read_percent();

    v->left.text = s_book_title;
    v->left.enc = s_title_enc;
    v->mid = NULL;               /* 顶栏不显示进度（评审） */

    v->line_enc = s_book_enc;
    v->line_count = mp_app_state.page.lines;
    if (v->line_count > MP_READ_LINES) {
        v->line_count = MP_READ_LINES;
    }
    for (uint8_t i = 0; i < v->line_count; i++) {
        memcpy(v->line[i], mp_app_state.page.raw[i], MP_LINE_BYTES);
    }
    v->percent = pct;
}

/* 存储行：只填 s_buf_space（值），总量/已用百分比单独返回。 */
static uint8_t storage_bar_pct(void)
{
    uint64_t total = 0;
    uint64_t freeb = 0;
    if (store_space(&total, &freeb) != 0 || total == 0) {
        return 0;
    }
    uint64_t used = total > freeb ? total - freeb : 0;
    return (uint8_t)((used * 100u + total / 2u) / total);
}

static void build_space_line(void)
{
    uint64_t total = 0;
    uint64_t freeb = 0;
    int rc = store_space(&total, &freeb);

    if (rc != 0 || total == 0) {
        snprintf(s_buf_space, sizeof(s_buf_space), "%s", "不可用");
        return;
    }

    /* COPY_DECK §3.4：GB = 1000^3，十等分四舍五入，避免浮点。 */
    const uint64_t gb = 1000000000ull;
    uint64_t used = total > freeb ? total - freeb : 0;
    uint64_t used_t = (used * 10u + gb / 2u) / gb;
    uint64_t total_t = (total * 10u + gb / 2u) / gb;
    snprintf(s_buf_space, sizeof(s_buf_space), "%u.%u/%u.%uGB",
             (unsigned)(used_t / 10u), (unsigned)(used_t % 10u),
             (unsigned)(total_t / 10u), (unsigned)(total_t % 10u));
}

/* 设置页项定义与动态顺序表：有背光 8 行（存储下标 5），无背光 7 行（存储下标 4）。 */
typedef enum {
    SET_ITEM_BLE = 0,
    SET_ITEM_SLEEP,
    SET_ITEM_LOCK_DISP,
    SET_ITEM_BACKLIGHT,
    SET_ITEM_TIME,
    SET_ITEM_STORAGE,
    SET_ITEM_ABOUT,
    SET_ITEM_RESTORE,
} set_item_t;

/* 有背光（E2S）：8 行，存储行在下标 5（不可选中）。 */
static const uint8_t k_set_items_bl[] = {
    SET_ITEM_BLE,
    SET_ITEM_SLEEP,
    SET_ITEM_LOCK_DISP,
    SET_ITEM_BACKLIGHT,
    SET_ITEM_TIME,
    SET_ITEM_STORAGE,
    SET_ITEM_ABOUT,
    SET_ITEM_RESTORE,
};

/* 无背光（E2）：7 行，隐藏背光项，存储行在下标 4（不可选中）。 */
static const uint8_t k_set_items_nobl[] = {
    SET_ITEM_BLE,
    SET_ITEM_SLEEP,
    SET_ITEM_LOCK_DISP,
    SET_ITEM_TIME,
    SET_ITEM_STORAGE,
    SET_ITEM_ABOUT,
    SET_ITEM_RESTORE,
};

static const uint8_t *settings_items(uint8_t *count)
{
    if (mp_app_state.has_backlight) {
        *count = (uint8_t)(sizeof(k_set_items_bl) / sizeof(k_set_items_bl[0]));
        return k_set_items_bl;
    }
    *count = (uint8_t)(sizeof(k_set_items_nobl) / sizeof(k_set_items_nobl[0]));
    return k_set_items_nobl;
}

static const char *bl_level_value(uint8_t level)
{
    static const char *const names[MP_BACKLIGHT_LEVEL_COUNT] = {
        "关", "1档", "2档", "3档"
    };
    return names[level % MP_BACKLIGHT_LEVEL_COUNT];
}

static void settings_fill(mp_ui_state_t *v, const uint8_t *items, uint8_t count)
{
    uint8_t si = mp_settings.sleep_timeout_idx;
    if (si >= MP_SLEEP_TIMEOUT_COUNT) {
        si = MP_SLEEP_IDX_DEFAULT;
    }
    build_space_line();

    for (uint8_t i = 0; i < MP_LIST_ROWS; i++) {
        uint8_t idx = (uint8_t)(v->scroll + i);
        mp_ui_row_t *r = &v->row[i];
        memset(r, 0, sizeof(*r));
        r->enc = MP_STR_UTF8;
        if (idx >= count) {
            r->label = NULL;
            continue;
        }
        switch ((set_item_t)items[idx]) {
        case SET_ITEM_BLE:
            r->label = "蓝牙";
            r->kind = MP_ROW_TOGGLE;
            r->on = mp_settings.ble_enabled ? 1 : 0;
            break;
        case SET_ITEM_SLEEP: {
            uint16_t sec = mp_sleep_timeout_s[si];
            r->label = "锁屏";
            r->kind = MP_ROW_VALUE;
            if (sec >= 60u && (sec % 60u) == 0u) {
                snprintf(s_buf_set[SET_ITEM_SLEEP],
                         sizeof(s_buf_set[SET_ITEM_SLEEP]), "%u分钟",
                         (unsigned)(sec / 60u));
            } else {
                snprintf(s_buf_set[SET_ITEM_SLEEP],
                         sizeof(s_buf_set[SET_ITEM_SLEEP]), "%u秒",
                         (unsigned)sec);
            }
            r->value = s_buf_set[SET_ITEM_SLEEP];
            break;
        }
        case SET_ITEM_LOCK_DISP:
            r->label = "锁屏展示";
            r->kind = MP_ROW_VALUE;
            r->value = (mp_settings.lock_display == MP_LOCK_DISP_IMAGE)
                           ? "锁屏图"
                           : "阅读数据";
            break;
        case SET_ITEM_BACKLIGHT:
            r->label = "背光";
            r->kind = MP_ROW_VALUE;
            snprintf(s_buf_set[SET_ITEM_BACKLIGHT],
                     sizeof(s_buf_set[SET_ITEM_BACKLIGHT]), "%s",
                     bl_level_value(mp_settings.backlight_level));
            r->value = s_buf_set[SET_ITEM_BACKLIGHT];
            break;
        case SET_ITEM_TIME:
            r->label = "时间设置";
            r->kind = MP_ROW_ACTION;
            break;
        case SET_ITEM_STORAGE:
            r->label = "存储";
            r->kind = MP_ROW_STORAGE;
            r->value = s_buf_space;
            r->bar = storage_bar_pct();
            break;
        case SET_ITEM_ABOUT:
            r->label = "关于";
            r->kind = MP_ROW_ACTION;
            break;
        case SET_ITEM_RESTORE:
            r->label = "恢复原厂固件";
            r->kind = MP_ROW_ACTION;
            break;
        default:
            r->label = NULL;
            break;
        }
    }
}

static void view_settings(mp_ui_state_t *v)
{
    v->left.text = "设置";
    v->left.enc = MP_STR_UTF8;
    v->mid = NULL;

    uint8_t count = 0;
    const uint8_t *items = settings_items(&count);
    uint8_t sel = mp_app_state.set_sel;
    if (sel >= count || items[sel] == SET_ITEM_STORAGE) {
        sel = 0;
    }
    v->row_count = count;
    v->sel = sel;
    v->scroll = mp_ui_list_scroll(sel, count);
    settings_fill(v, items, count);
}

static void view_about(mp_ui_state_t *v)
{
    v->left.text = "关于";
    v->left.enc = MP_STR_UTF8;
    v->mid = NULL;

    snprintf(s_buf_about[0], sizeof(s_buf_about[0]), "%s",
             mp_app_state.has_backlight ? "Miaopaper E2S" : "Miaopaper E2");
    snprintf(s_buf_about[1], sizeof(s_buf_about[1]), "固件 %u.%u.%u",
             (unsigned)READER_VERSION_MAJOR, (unsigned)READER_VERSION_MINOR,
             (unsigned)READER_VERSION_PATCH);
    snprintf(s_buf_about[2], sizeof(s_buf_about[2]), "构建 %s", MP_BUILD_DATE);
    snprintf(s_buf_about[3], sizeof(s_buf_about[3]), "%s",
             text_hz16_glyph(0xB0, 0xA1) ? "字库 内置" : "字库 缺失");
    v->about_lines = 4;

    for (uint8_t i = 0; i < MP_LIST_ROWS; i++) {
        v->about_line[i].text = (i < v->about_lines) ? s_buf_about[i] : NULL;
        v->about_line[i].enc = MP_STR_UTF8;
    }
}

static void format_size(uint32_t len, char *out, size_t cap)
{
    if (len >= 1024u * 1024u) {
        uint32_t whole = len / (1024u * 1024u);
        uint32_t frac = ((len % (1024u * 1024u)) * 10u + 1024u * 512u) /
                        (1024u * 1024u);
        if (frac >= 10) {
            whole++;
            frac = 0;
        }
        snprintf(out, cap, "%u.%uMB", (unsigned)whole, (unsigned)frac);
    } else if (len >= 1024u) {
        snprintf(out, cap, "%uKB", (unsigned)((len + 512u) / 1024u));
    } else {
        snprintf(out, cap, "%uB", (unsigned)len);
    }
}

static void view_book_info(mp_ui_state_t *v)
{
    char size[16];
    char words[24];

    v->left.text = s_book_title;
    v->left.enc = s_title_enc;
    v->mid = NULL;

    if (s_book_chars_k > 0) {
        uint32_t chars = (uint32_t)s_book_chars_k * 1024u;
        if (chars >= 10000u) {
            snprintf(words, sizeof(words), "%u.%u万", (unsigned)(chars / 10000u),
                     (unsigned)((chars % 10000u) / 1000u));
        } else {
            snprintf(words, sizeof(words), "%u", (unsigned)chars);
        }
    } else {
        snprintf(words, sizeof(words), "--");
    }
    format_size(mp_app_state.book_len, size, sizeof(size));

    /* '\t' 分段：mp_ui_book_info 会按可用宽度均分间隔，避免"万进度"贴在一起。 */
    snprintf(s_buf_info, sizeof(s_buf_info), "字数 %s\t进度 %u%%\t大小 %s",
             words, (unsigned)read_percent(), size);
    v->meta.text = s_buf_info;
    v->meta.enc = MP_STR_UTF8;

    snprintf(s_buf_info_act[0], sizeof(s_buf_info_act[0]), "%s", "跳转");
    snprintf(s_buf_info_act[1], sizeof(s_buf_info_act[1]), "%s", "重置阅读进度");
    snprintf(s_buf_info_act[2], sizeof(s_buf_info_act[2]), "%s", "返回书库");
    snprintf(s_buf_info_act[3], sizeof(s_buf_info_act[3]), "%s", "删除本书");

    v->row_count = 4;
    v->sel = mp_app_state.info_sel < 4 ? mp_app_state.info_sel : 0;
    v->scroll = 0;
    for (uint8_t i = 0; i < 4; i++) {
        v->row[i].label = s_buf_info_act[i];
        v->row[i].enc = MP_STR_UTF8;
        v->row[i].kind = MP_ROW_TEXT;
    }
}

static void view_jump(mp_ui_state_t *v)
{
    v->left.text = "跳转到";
    v->left.enc = MP_STR_UTF8;
    v->mid = NULL;
    v->time_valid = 0;
    v->jump_pct = mp_app_state.jump_pct;
}

static void view_time_set(mp_ui_state_t *v)
{
    v->left.text = "时间设置";
    v->left.enc = MP_STR_UTF8;
    snprintf(s_buf_mid, sizeof(s_buf_mid), "%u/5",
             (unsigned)(mp_app_state.time_step % MP_TIME_FIELDS + 1));
    v->mid = s_buf_mid;
    v->time_valid = 0;
    v->year = mp_app_state.set_year;
    v->month = mp_app_state.set_month;
    v->day = mp_app_state.set_day;
    v->hour = mp_app_state.set_hour;
    v->min = mp_app_state.set_min;
    v->time_step = mp_app_state.time_step;
}

static void view_lock(mp_ui_state_t *v)
{
    v->lock_img = s_lock_img;
    v->lock_valid = s_lock_valid;
    v->read_mins = mp_stats.read_secs / 60u;
    v->read_chars = mp_stats.read_chars;
    if (mp_settings.lock_display == MP_LOCK_DISP_IMAGE && s_lock_valid) {
        v->lock_mode = mp_app_state.lock_prompt ? MP_LOCK_MODE_PROMPT
                                                : MP_LOCK_MODE_IMAGE;
    } else {
        v->lock_mode = MP_LOCK_MODE_STATS;
    }
    v->time_valid = 0;           /* 锁屏不画状态栏 */
}

static void view_msg2(mp_ui_state_t *v, const char *title, const char *l1,
                      const char *l2)
{
    v->left.text = title;
    v->left.enc = MP_STR_UTF8;
    v->mid = NULL;
    v->msg_lines = 2;
    v->msg_line[0].text = l1;
    v->msg_line[0].enc = MP_STR_UTF8;
    v->msg_line[1].text = l2;
    v->msg_line[1].enc = MP_STR_UTF8;
    v->msg_line[2].text = NULL;
    v->msg_line[2].enc = MP_STR_UTF8;
}

static void view_msg1(mp_ui_state_t *v, const char *title, const char *l1)
{
    v->left.text = title;
    v->left.enc = MP_STR_UTF8;
    v->mid = NULL;
    v->msg_lines = 1;
    v->msg_line[0].text = l1;
    v->msg_line[0].enc = MP_STR_UTF8;
    v->msg_line[1].text = NULL;
    v->msg_line[2].text = NULL;
}

static void view_empty(mp_ui_state_t *v)
{
    view_msg2(v, "书库", "没有书", "用网页上传");
}

static void view_error(mp_ui_state_t *v)
{
    switch (s_error_kind) {
    case ERR_RECOVERY:
        switch (s_error_rc) {
        case RECOVERY_ERR_NO_STOCK:
            view_msg2(v, "无法恢复", "原厂固件不存在", "先刷原厂固件");
            break;
        case RECOVERY_ERR_LAYOUT:
            view_msg2(v, "无法恢复", "分区表不匹配", "先刷原厂固件");
            break;
        case RECOVERY_ERR_UNSUPPORTED:
            view_msg2(v, "无法恢复", "未开放此功能", "保持当前固件");
            break;
        case RECOVERY_ERR_WRITE:
            view_msg2(v, "无法恢复", "写入失败", "重启后重试");
            break;
        case RECOVERY_ERR_VERIFY:
            view_msg2(v, "无法恢复", "校验失败", "重启后重试");
            break;
        default:
            view_msg2(v, "无法恢复", "写入失败", "重启后重试");
            break;
        }
        break;
    case ERR_DISPLAY:
        view_msg2(v, "刷新失败", "画面停在上一帧", "重启设备");
        break;
    case ERR_STORAGE:
    default:
        view_msg2(v, "存储错误", "读不到内置存储", "重启设备");
        break;
    }
}

static void view_restore(mp_ui_state_t *v)
{
    /* 标题带对象（恢复原厂固件），正文只留后果；原"切到原厂固件"与标题
     * 同义，删除（评审意见）。3 行版式仍由 mp_ui_msg 给（RESTORE 固定）。 */
    v->left.text = "恢复原厂固件";
    v->left.enc = MP_STR_UTF8;
    v->mid = NULL;
    v->msg_lines = 1;
    v->msg_line[0].text = "重启后生效";
    v->msg_line[0].enc = MP_STR_UTF8;
    v->msg_line[1].text = NULL;
    v->msg_line[1].enc = MP_STR_UTF8;
    v->msg_line[2].text = NULL;
    v->msg_line[2].enc = MP_STR_UTF8;
    v->hide_hints = 0;
    v->time_valid = 0;           /* 6 字标题 + 居中时间会挤在一起 */
}

static void view_delete(mp_ui_state_t *v)
{
    /* 标题已说明对象；正文只留后果，避免"删除本书？"与标题重复。 */
    view_msg1(v, "删除本书", "需重新上传");
}

static void view_time_done(mp_ui_state_t *v)
{
    view_msg1(v, "时间设置", "设置完成");
    v->hide_hints = 1;
}

static void view_no_lock_img(mp_ui_state_t *v)
{
    view_msg2(v, "锁屏展示", "未上传锁屏图", "用网页上传");
}

static void view_backlight(mp_ui_state_t *v)
{
    v->left.text = "背光";
    v->left.enc = MP_STR_UTF8;
    v->mid = NULL;
}

bool mp_app_view(mp_ui_state_t *out)
{
    if (!out) {
        return false;
    }
    view_common(out);
    switch (mp_app_state.screen) {
    case MP_SCREEN_CLOCK:
        view_clock(out);
        break;
    case MP_SCREEN_SELECT:
        view_select(out);
        break;
    case MP_SCREEN_READ:
        view_read(out);
        break;
    case MP_SCREEN_SETTINGS:
        view_settings(out);
        break;
    case MP_SCREEN_ABOUT:
        view_about(out);
        break;
    case MP_SCREEN_LOCK:
        view_lock(out);
        break;
    case MP_SCREEN_EMPTY:
        view_empty(out);
        break;
    case MP_SCREEN_ERROR:
        view_error(out);
        break;
    case MP_SCREEN_RESTORE:
        view_restore(out);
        break;
    case MP_SCREEN_BOOK_INFO:
        view_book_info(out);
        break;
    case MP_SCREEN_JUMP:
        view_jump(out);
        break;
    case MP_SCREEN_DELETE:
        view_delete(out);
        break;
    case MP_SCREEN_TIME_SET:
        view_time_set(out);
        break;
    case MP_SCREEN_TIME_DONE:
        view_time_done(out);
        break;
    case MP_SCREEN_NO_LOCK_IMG:
        view_no_lock_img(out);
        break;
    case MP_SCREEN_BACKLIGHT:
        view_backlight(out);
        break;
    case MP_SCREEN_LOW_BATT:
        out->time_valid = 0;
        out->bl_popup = 0;
        break;
    default:
        view_clock(out);
        break;
    }
    return true;
}

/* 恢复成功页：与确认页同为 2 行版式（44/62），靠 hide_hints 去掉底栏
 *（COPY_DECK §2.10）。 */
static void render_restore_ok(void)
{
    view_common(&s_ui);
    s_ui.left.text = "恢复原厂固件";
    s_ui.left.enc = MP_STR_UTF8;
    s_ui.mid = NULL;
    s_ui.msg_lines = 2;
    s_ui.msg_line[0].text = "已切换";
    s_ui.msg_line[0].enc = MP_STR_UTF8;
    s_ui.msg_line[1].text = "3秒后重启";
    s_ui.msg_line[1].enc = MP_STR_UTF8;
    s_ui.msg_line[2].text = NULL;
    s_ui.msg_line[2].enc = MP_STR_UTF8;
    s_ui.hide_hints = 1;
    s_ui.time_valid = 0;             /* 与确认页一致：6 字标题不叠时间 */
    mp_ui_render(&s_ui, s_fb);
}

/* ------------------------------------------------------------------ *
 * 进度与阅读统计落盘（PORT_SPEC §6.4 的五个时机都走这里）
 * ------------------------------------------------------------------ */

static uint8_t copy_space(uint8_t c)
{
    return c == ' ' || c == '\r' || c == '\n' || c == '\t';
}

/* 统计本页非空白字符数，与 upload_count_chars() 同一口径。 */
static uint32_t count_page_chars(const mp_page_t *p, uint8_t enc)
{
    uint32_t chars = 0;
    if (!p) {
        return 0;
    }
    for (uint8_t r = 0; r < p->lines && r < MP_READ_LINES; r++) {
        const uint8_t *s = p->raw[r];
        uint8_t lead = 0;
        for (size_t i = 0; s[i] != '\0' && i < MP_LINE_BYTES; i++) {
            uint8_t c = s[i];
            if (enc == MP_ENC_GB2312) {
                if (lead) {
                    chars++;
                    lead = 0;
                    continue;
                }
                if (c >= 0xA1) {
                    lead = 1;
                } else if (!copy_space(c)) {
                    chars++;
                }
            } else if (!copy_space(c)) {
                chars++;
            }
        }
        if (lead) {
            chars++;
        }
    }
    return chars;
}

static void flush_stats(void)
{
    if (!s_stats_dirty || !mp_app_state.storage_ok) {
        return;
    }
    if (mp_stats_save() == 0) {
        s_stats_dirty = 0;
        s_stats_last_save_ms = now_ms();
    }
}

/* 基于单调毫秒差分累计阅读时长，避免 Tickless Idle 下需要每秒唤醒计秒。 */
static void read_time_flush(uint32_t now)
{
    if (mp_app_state.screen == MP_SCREEN_READ && mp_app_state.book_open &&
        !mp_app_state.uploading && !mp_app_state.debug_mode) {
        if (s_read_tracking) {
            uint32_t dt = (uint32_t)(now - s_read_ts_ms) + s_read_rem_ms;
            uint32_t sec = dt / 1000u;
            s_read_rem_ms = (uint16_t)(dt % 1000u);
            if (sec > 0) {
                mp_stats.read_secs += sec;
                s_stats_dirty = 1;
            }
        }
        s_read_ts_ms = now;
        s_read_tracking = 1;
        if (s_stats_dirty &&
            (uint32_t)(now - s_stats_last_save_ms) >= 60000u) {
            flush_stats();
        }
    } else {
        s_read_tracking = 0;
        s_read_rem_ms = 0;
    }
}

void mp_app_flush_progress(void)
{
    flush_stats();
    if (!mp_app_state.book_open || !mp_app_state.pos_dirty ||
        !mp_app_state.storage_ok) {
        return;
    }
    uint32_t pos = mp_page_clamp(mp_app_state.char_pos, mp_app_state.book_len);
    if (mp_progress_save(mp_app_state.book_idx, pos) == 0) {
        mp_app_state.pos_dirty = 0;
    }
}

static void mark_pos_dirty(uint32_t now)
{
    mp_app_state.pos_dirty = 1;
    mp_app_state.pos_dirty_ms = now;
}

/* ------------------------------------------------------------------ *
 * 屏幕切换
 * ------------------------------------------------------------------ */

static void lock_enter(void);
static void unlock(uint32_t now);
static void restore_enter(void);
static void retry_action(void);
static void upload_abort(void);
static void open_book_info(void);
static void read_enter_jump(void);
static void delete_enter(void);
static void enter_time_set(void);
static uint8_t days_in_month(uint8_t m, uint16_t y);
static void apply_time(uint32_t unix_s, uint16_t year, uint8_t month,
                       uint8_t day, uint8_t week, uint32_t now);

/* §8.3：restore_armed 的**每一条**离开路径都必须清掉（含 BLE 0x21、DEBUG 退出）。 */
static void clear_restore_arm(void)
{
    mp_app_state.restore_armed = 0;
    mp_app_state.restore_key_ok = 0;
}

static void go_screen(uint8_t screen, uint8_t force_full)
{
    if (mp_app_state.screen == MP_SCREEN_RESTORE) {
        clear_restore_arm();
    }
    uint32_t now = now_ms();
    if (mp_app_state.screen == MP_SCREEN_READ && screen != MP_SCREEN_READ) {
        read_time_flush(now);
    }
    mp_app_state.pending_ok = 0;      /* 任何切屏都取消待发的 OK 单击（§2.3） */
    mp_app_state.screen = screen;
    read_time_flush(now);
    mark_dirty();
    scene_commit(force_full);
}

static void close_book(void)
{
    read_time_flush(now_ms());
    mp_app_flush_progress();
    s_read_tracking = 0;
    s_read_rem_ms = 0;
    mp_app_state.book_open = 0;
    mp_page_hist_reset();
    memset(&mp_app_state.page, 0, sizeof(mp_app_state.page));
    mp_app_state.char_pos = 0;
    mp_app_state.book_len = 0;
}

static void open_clock(uint8_t force_full)
{
    if (mp_app_state.book_open) {
        close_book();
    }
    go_screen(MP_SCREEN_CLOCK, force_full);
}

static void show_error_storage(void)
{
    s_error_kind = ERR_STORAGE;
    s_error_rc = 0;
    mp_app_state.pending_ok = 0;
    mp_app_state.screen = MP_SCREEN_ERROR;
    mark_dirty();
    scene_commit(1);                  /* 错误页 force_full=1（§5.1） */
}

static void show_error_display(void)
{
    s_error_kind = ERR_DISPLAY;
    s_error_rc = 0;
    mp_app_state.pending_ok = 0;
    mp_app_state.screen = MP_SCREEN_ERROR;
    mark_dirty();
    scene_commit(1);
}

static void show_error_recovery(int rc)
{
    s_error_kind = ERR_RECOVERY;
    s_error_rc = rc;
    mp_app_state.pending_ok = 0;
    mp_app_state.screen = MP_SCREEN_ERROR;
    mark_dirty();
    scene_commit(1);
}

static void open_settings(void)
{
    if (mp_app_state.screen == MP_SCREEN_CLOCK) {
        mp_app_state.set_sel = 0;     /* 从主时钟进设置时光标归零 */
    }
    go_screen(MP_SCREEN_SETTINGS, 0);
}

static void open_select(void)
{
    if (!mp_app_state.storage_ok) {
        show_error_storage();
        return;
    }
    rescan_books();
    if (s_done_n == 0) {
        mp_app_state.sel = 0;
        go_screen(MP_SCREEN_EMPTY, 0);
        return;
    }
    if (mp_app_state.sel >= s_done_n) {
        mp_app_state.sel = 0;
    }
    go_screen(MP_SCREEN_SELECT, 0);
}

/* 装载一本书（不切屏）。失败不改动当前状态，调用方决定显示什么。 */
static int book_open_slot(uint8_t slot)
{
    mp_book_t b;
    mp_page_t tmp;
    uint32_t len;
    uint32_t pos = 0;
    uint8_t enc;

    if (!mp_app_state.storage_ok) {
        return -1;
    }
    if (mp_app_state.book_open) {
        mp_app_flush_progress();      /* 换书前先落盘（§6.4 时机 2） */
    }
    if (mp_store_read(slot, &b) != 0) {
        return -1;
    }
    len = mp_store_book_size(slot);
    if (len < 16) {
        return -1;                    /* 坏书（PORT_SPEC §6.6-11） */
    }
    enc = (b.enc == MP_ENC_GB2312) ? MP_ENC_GB2312 : MP_ENC_ASCII;

    (void)mp_progress_load(slot, &pos);
    pos = mp_page_clamp(pos, len);
    if (pos >= len) {
        pos = 0;
    }

    book_ctx_t ctx = {slot};
    mp_book_ref_t ref;
    ref.read = book_read_cb;
    ref.ctx = &ctx;
    ref.len = len;
    ref.enc = enc;
    (void)mp_page_measure(&ref, pos, &tmp);
    if (tmp.lines == 0) {
        return -1;
    }

    mp_app_state.book_idx = slot;
    mp_app_state.book_len = len;
    mp_app_state.char_pos = pos;
    mp_app_state.page = tmp;
    mp_app_state.book_open = 1;
    mp_app_state.pos_dirty = 0;
    s_book_ctx.idx = slot;
    s_book_enc = enc;
    s_book_chars_k = b.chars_k;
    if (s_book_chars_k == 0) {
        /* 旧目录项没有字数：按编码粗估，信息页显示"约"。 */
        uint32_t est = (enc == MP_ENC_GB2312) ? (len / 2u) : len;
        s_book_chars_k = (uint16_t)((est + 1023u) / 1024u);
    }
    memcpy(s_book_title, b.title, sizeof(b.title));
    s_book_title[MP_TITLE_MAX] = '\0';
    s_title_enc = title_enc(s_book_title);
    mp_page_hist_reset();
    return 0;
}

static void open_book_from_ui(uint8_t slot)
{
    if (book_open_slot(slot) != 0) {
        show_error_storage();
        return;
    }
    go_screen(MP_SCREEN_READ, 0);
}

/* 退出阅读：保存进度、关书，再切屏（§6.4 时机 1）。 */
static void read_exit(uint8_t to)
{
    close_book();
    go_screen(to, 0);
}

/* ------------------------------------------------------------------ *
 * 阅读翻页
 * ------------------------------------------------------------------ */

static void read_next(void)
{
    uint32_t next = mp_app_state.page.next;
    if (next <= mp_app_state.char_pos || next >= mp_app_state.book_len) {
        return;                       /* 末页：不重画（§1.9.3） */
    }
    uint32_t page_chars = count_page_chars(&mp_app_state.page, s_book_enc);
    mp_page_hist_push(mp_app_state.char_pos);
    mp_app_state.char_pos = next;
    if (measure_page(next) != 0) {
        show_error_storage();
        return;
    }
    uint32_t now = now_ms();
    read_time_flush(now);
    mp_stats.read_chars += page_chars;
    s_stats_dirty = 1;
    mark_pos_dirty(now);
    page_commit();
}

static void read_prev(void)
{
    uint32_t prev = 0;
    if (!mp_page_hist_pop(&prev)) {
        mp_book_ref_t b = book_ref();
        prev = mp_page_prev_start(&b, mp_app_state.char_pos);
    }
    if (prev >= mp_app_state.char_pos) {
        return;                       /* 无上一页（防御，§4.5） */
    }
    mp_app_state.char_pos = prev;
    if (measure_page(prev) != 0) {
        show_error_storage();
        return;
    }
    uint32_t now = now_ms();
    read_time_flush(now);
    mark_pos_dirty(now);
    page_commit();
}

/* ------------------------------------------------------------------ *
 * 阅读信息页 / 跳转 / 删除
 * ------------------------------------------------------------------ */

static void open_book_info(void)
{
    if (!mp_app_state.book_open) {
        return;
    }
    mp_app_state.info_sel = 0;
    go_screen(MP_SCREEN_BOOK_INFO, 0);
}

static void read_enter_jump(void)
{
    if (!mp_app_state.book_open) {
        return;
    }
    mp_app_state.jump_pct = read_percent();
    go_screen(MP_SCREEN_JUMP, 0);
}

static void jump_apply(void)
{
    if (!mp_app_state.book_open) {
        return;
    }
    uint8_t pct = mp_app_state.jump_pct > 100 ? 100 : mp_app_state.jump_pct;
    uint32_t target = (uint32_t)(((uint64_t)mp_app_state.book_len * pct) / 100u);
    mp_book_ref_t b = book_ref();

    if (pct >= 100u || target >= mp_app_state.book_len) {
        target = mp_page_prev_start(&b, mp_app_state.book_len);
    }
    if (target >= mp_app_state.book_len) {
        target = 0;
    }
    mp_app_state.char_pos = target;
    if (measure_page(target) != 0) {
        show_error_storage();
        return;
    }
    mp_page_hist_reset();
    mp_app_state.pos_dirty = 1;
    mp_app_state.pos_dirty_ms = now_ms();
    mp_app_flush_progress();
    go_screen(MP_SCREEN_READ, 0);
}

static void read_reset_progress(void)
{
    if (!mp_app_state.book_open) {
        return;
    }
    mp_app_state.char_pos = 0;
    if (measure_page(0) != 0) {
        show_error_storage();
        return;
    }
    mp_page_hist_reset();
    mp_app_state.pos_dirty = 0;
    (void)mp_progress_save(mp_app_state.book_idx, 0);
    page_commit();
}

static void delete_enter(void)
{
    if (!mp_app_state.book_open) {
        return;
    }
    go_screen(MP_SCREEN_DELETE, 0);
}

static void delete_commit(void)
{
    if (!mp_app_state.book_open) {
        return;
    }
    uint8_t idx = mp_app_state.book_idx;

    /* 先关书：close_book() 会 flush 进度；必须在删除前完成，否则会把刚被
     * mp_store_delete() 清掉的进度又写回该槽。 */
    mp_app_state.pos_dirty = 0;
    close_book();
    if (mp_store_delete(idx) != 0) {
        show_error_storage();
        return;
    }
    rescan_books();
    if (s_done_n == 0) {
        mp_app_state.sel = 0;
        go_screen(MP_SCREEN_EMPTY, 0);
    } else {
        if (mp_app_state.sel >= s_done_n) {
            mp_app_state.sel = 0;
        }
        go_screen(MP_SCREEN_SELECT, 0);
    }
}

static void info_activate(void)
{
    switch (mp_app_state.info_sel) {
    case 0:
        read_enter_jump();
        break;
    case 1:
        read_reset_progress();
        break;
    case 2:
        read_exit(MP_SCREEN_SELECT);
        break;
    case 3:
        delete_enter();
        break;
    default:
        break;
    }
}

static void info_step(int dir)
{
    mp_app_state.info_sel =
        (uint8_t)((mp_app_state.info_sel + dir + 4) % 4);
    page_commit();
}

/* ------------------------------------------------------------------ *
 * 时间日期设置向导
 * ------------------------------------------------------------------ */

static uint32_t days_from_civil(uint16_t y, uint8_t m, uint8_t d)
{
    /* Howard Hinnant 的 days_from_civil；2020..2099 内不会溢出。 */
    uint32_t yy = y;
    yy -= (m <= 2) ? 1u : 0u;
    uint32_t era = yy / 400u;
    uint32_t yoe = yy - era * 400u;
    int mm = (int)m + (m > 2 ? -3 : 9);
    uint32_t doy = (uint32_t)((153 * mm + 2) / 5 + (int)d - 1);
    uint32_t doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097u + doe - 719468u;
}

static void civil_from_days(uint32_t days, uint16_t *year, uint8_t *month,
                            uint8_t *day)
{
    /* Howard Hinnant 的 civil_from_days 算法，与 days_from_civil 互逆。 */
    int64_t z = (int64_t)days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    uint32_t doe = (uint32_t)(z - era * 146097);
    uint32_t yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    int64_t y = (int64_t)yoe + era * 400;
    uint32_t doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    uint32_t mp = (5u * doy + 2u) / 153u;
    uint32_t d = doy - (153u * mp + 2u) / 5u + 1u;
    uint32_t m = mp + (mp < 10u ? 3u : (uint32_t)-9);
    y += (m <= 2u) ? 1 : 0;
    *year = (uint16_t)y;
    *month = (uint8_t)m;
    *day = (uint8_t)d;
}

static void time_clamp_day(void)
{
    uint8_t max = days_in_month(mp_app_state.set_month, mp_app_state.set_year);
    if (mp_app_state.set_day < 1) {
        mp_app_state.set_day = 1;
    } else if (mp_app_state.set_day > max) {
        mp_app_state.set_day = max;
    }
}

static void enter_time_set(void)
{
    mp_app_state.set_year = s_year;
    mp_app_state.set_month = mp_app_state.month;
    mp_app_state.set_day = mp_app_state.day;
    mp_app_state.set_hour = mp_app_state.hour;
    mp_app_state.set_min = mp_app_state.min;
    mp_app_state.time_step = 0;
    go_screen(MP_SCREEN_TIME_SET, 0);
}

static void time_value_step(int dir)
{
    switch (mp_app_state.time_step % MP_TIME_FIELDS) {
    case 0: {
        int y = (int)mp_app_state.set_year + dir;
        if (y < 2020) {
            y = 2099;
        } else if (y > 2099) {
            y = 2020;
        }
        mp_app_state.set_year = (uint16_t)y;
        time_clamp_day();
        break;
    }
    case 1: {
        int m = (int)mp_app_state.set_month + dir;
        if (m < 1) {
            m = 12;
        } else if (m > 12) {
            m = 1;
        }
        mp_app_state.set_month = (uint8_t)m;
        time_clamp_day();
        break;
    }
    case 2: {
        int max = days_in_month(mp_app_state.set_month, mp_app_state.set_year);
        int d = (int)mp_app_state.set_day + dir;
        if (d < 1) {
            d = max;
        } else if (d > max) {
            d = 1;
        }
        mp_app_state.set_day = (uint8_t)d;
        break;
    }
    case 3:
        mp_app_state.set_hour =
            (uint8_t)((mp_app_state.set_hour + dir + 24) % 24);
        break;
    default:
        mp_app_state.set_min =
            (uint8_t)((mp_app_state.set_min + dir + 60) % 60);
        break;
    }
    page_commit();
}

static void time_commit(void)
{
    uint32_t days = days_from_civil(mp_app_state.set_year,
                                    mp_app_state.set_month,
                                    mp_app_state.set_day);
    uint32_t unix_s = days * 86400u +
                      (uint32_t)mp_app_state.set_hour * 3600u +
                      (uint32_t)mp_app_state.set_min * 60u;
    uint8_t week = (uint8_t)((days + 3u) % 7u + 1u);   /* 1970-01-01 = 周四 */
    uint32_t now = now_ms();

    apply_time(unix_s, mp_app_state.set_year, mp_app_state.set_month,
               mp_app_state.set_day, week, now);
    mp_app_state.time_done_until_ms = now + 1200u;
    go_screen(MP_SCREEN_TIME_DONE, 0);
}

static void time_next(void)
{
    if (mp_app_state.time_step + 1u < MP_TIME_FIELDS) {
        mp_app_state.time_step++;
        page_commit();
    } else {
        time_commit();
    }
}

static void load_lock_image(void)
{
    s_lock_valid = 0;
    if (!mp_app_state.storage_ok || !mp_store_lock_valid()) {
        return;
    }
    if (mp_store_lock_read(s_lock_img, sizeof(s_lock_img)) == 0) {
        s_lock_valid = 1;
    }
}

static void lock_enter(void)
{
    if (mp_app_state.screen == MP_SCREEN_LOCK) {
        return;
    }
    uint32_t now = now_ms();
    read_time_flush(now);
    /* 锁屏要先出图：上传会话期间不重绘（§5.5），所以先把会话回滚掉。
     * 半本书的槽本来也不会是 DONE。 */
    if (mp_app_state.uploading) {
        upload_abort();
    }
    mp_app_state.bl_popup = 0;
    mp_app_state.prev_screen = mp_app_state.screen;
    mp_app_state.locked_from_read =
        (mp_app_state.screen == MP_SCREEN_READ ||
         mp_app_state.screen == MP_SCREEN_BOOK_INFO ||
         mp_app_state.screen == MP_SCREEN_JUMP) ? 1 : 0;
    mp_app_state.lock_prompt = 0;
    mp_app_state.lock_prompt_until_ms = 0;
    mp_app_state.pending_ok = 0;
    mp_app_state.lock_enter_ms = now;
    mp_app_state.ble_off_ms = 0;
    clear_restore_arm();

    mp_app_flush_progress();          /* §6.4 时机 3：锁屏（scene_commit 之前） */
    if (mp_app_state.has_backlight) {
        mp_backlight_suspend();       /* 锁屏关闭背光待机省电 */
    }
    load_lock_image();
    mp_app_state.screen = MP_SCREEN_LOCK;
    s_read_tracking = 0;
    s_read_rem_ms = 0;
    mark_dirty();
    scene_commit(1);                  /* 锁屏一律场景全刷（§1.5.6 / §5.2） */
}

/* 纯锁屏图模式下按其他键（或短按 OK）跳转解锁提示页，10 秒后自动复原。 */
static void lock_show_prompt(uint32_t now)
{
    if (mp_app_state.screen != MP_SCREEN_LOCK) {
        return;
    }
    if (mp_settings.lock_display != MP_LOCK_DISP_IMAGE || !s_lock_valid) {
        return;
    }
    mp_app_state.lock_prompt_until_ms = now + 10000u;
    if (!mp_app_state.lock_prompt) {
        mp_app_state.lock_prompt = 1;
        page_commit();
    }
}

static void unlock(uint32_t now)
{
    if (mp_app_state.screen != MP_SCREEN_LOCK) {
        return;
    }
    mp_app_state.lock_prompt = 0;
    mp_app_state.lock_prompt_until_ms = 0;
    mp_app_state.pending_ok = 0;
    if (mp_app_state.locked_from_read && mp_app_state.book_open) {
        mp_app_state.screen = MP_SCREEN_READ;
        mp_page_hist_reset();         /* §4.5：解锁回阅读要清历史 */
        if (measure_page(mp_app_state.char_pos) != 0) {
            show_error_storage();
        } else {
            scene_commit(1);          /* 回阅读并全刷当前页（§2.4） */
        }
    } else {
        mp_app_state.screen = MP_SCREEN_CLOCK;
        scene_commit(0);
    }
    mp_app_state.locked_from_read = 0;
    mp_app_state.last_input_ms = now;
    read_time_flush(now);
    if (mp_app_state.has_backlight) {
        (void)mp_backlight_set(mp_settings.backlight_level);
    }
}

/* ------------------------------------------------------------------ *
 * 恢复原厂
 * ------------------------------------------------------------------ */

static void restore_enter(void)
{
    char detail[64];
    recovery_rc_t rc;

    /* 面板未就绪就拒绝：画什么都不会出现（§8.2），不写 flash、不切屏。 */
    if (!display_ready()) {
        ESP_LOGW(TAG, "restore: panel not ready");
        return;
    }
    if (mp_app_state.debug_mode) {
        return;
    }

    rc = recovery_stock_probe(detail, sizeof(detail));
    if (rc != RECOVERY_OK) {
        ESP_LOGW(TAG, "restore: probe rc=%d (%s)", (int)rc, detail);
        show_error_recovery(rc);
        return;
    }

    if (mp_app_state.uploading) {
        /* 提交前不留任何写会话（mp_store_sync_close() 的语义在这里用 abort 代替）。 */
        mp_store_abort(s_up_slot);
        s_up_kind = UP_NONE;
        s_up_slot = 0xFF;
        mp_app_state.uploading = 0;
        mp_ble_set_upload_active(0);
    }

    mp_app_state.prev_screen = MP_SCREEN_SETTINGS;
    mp_app_state.screen = MP_SCREEN_RESTORE;
    mp_app_state.restore_armed = 0;
    mp_app_state.restore_key_ok = 0;
    mp_app_state.pending_ok = 0;

    uint32_t before = frames_now();
    if (render() != 0) {
        return;
    }
    display_post(s_fb, DISPLAY_MODE_FULL);
    display_flush_wait(8000);
    if (frames_now() == before) {
        /* 面板就绪但这一帧没上屏（§8.3 的 frames 判定）。 */
        ESP_LOGW(TAG, "restore: frame did not reach the panel");
        show_error_display();
        return;
    }

    /* 这 8 s 里 input 任务还在入队：先抽干，再 armed（§8.3 的关键一步）。 */
    key_event_t ev;
    while (input_get(&ev, 0)) {
        /* 丢弃 */
    }
    mp_app_state.restore_armed = 1;
    ESP_LOGI(TAG, "restore armed");
}

static void restore_cancel(void)
{
    mp_app_state.restore_armed = 0;
    mp_app_state.restore_key_ok = 0;
    go_screen(MP_SCREEN_SETTINGS, 0);
}

static void restore_commit(void)
{
    char detail[64];
    recovery_rc_t rc;

    mp_app_flush_progress();          /* §6.4 时机 5：提交前保存进度 */
    rc = recovery_stock_commit(detail, sizeof(detail));
    mp_app_state.restore_armed = 0;
    mp_app_state.restore_key_ok = 0;

    if (rc == RECOVERY_OK) {
        if (!display_ready()) {
            esp_restart();
        }
        render_restore_ok();
        display_post(s_fb, DISPLAY_MODE_AUTO);
        display_flush_wait(8000);
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_restart();
    }

    ESP_LOGW(TAG, "restore: commit rc=%d (%s)", (int)rc, detail);
    show_error_recovery(rc);          /* 失败绝不重启 */
}

/* ------------------------------------------------------------------ *
 * 错误页的重试
 * ------------------------------------------------------------------ */

static void retry_action(void)
{
    switch (s_error_kind) {
    case ERR_STORAGE: {
        if (mp_app_state.uploading) {
            upload_abort();           /* 重新挂载前不能留着写句柄 */
        }
        if (!storage_sd_ok()) {
            (void)storage_init();
        } else {
            (void)storage_rescan();
        }
        if (storage_sd_ok() && mp_store_init() == 0) {
            mp_app_state.storage_ok = 1;
            rescan_books();
            s_error_kind = ERR_NONE;
            open_clock(1);
        } else {
            show_error_storage();
        }
        break;
    }
    case ERR_DISPLAY:
        /* 面板就绪但帧没上屏：重发一次错误页；还不行就保持原样。 */
        scene_commit(1);
        break;
    case ERR_RECOVERY:
        restore_enter();              /* 重新 probe，通过就回到确认页 */
        break;
    default:
        open_clock(0);
        break;
    }
}

/* ------------------------------------------------------------------ *
 * 列表 / 设置动作
 * ------------------------------------------------------------------ */

static void cursor_step(uint8_t *sel, uint8_t count, int dir)
{
    if (count == 0) {
        return;
    }
    int s = (int)*sel + dir;
    if (s < 0) {
        s = (int)count - 1;
    } else if (s >= (int)count) {
        s = 0;
    }
    *sel = (uint8_t)s;
}

static void select_activate(void)
{
    if (!mp_app_state.storage_ok) {
        show_error_storage();
        return;
    }
    if (mp_app_state.sel >= s_done_n) {
        return;                       /* 列表只有书；退出走 MENU/[×]返回 */
    }
    open_book_from_ui(s_done[mp_app_state.sel]);
}

static void enter_backlight(void)
{
    if (!mp_app_state.has_backlight) {
        return;
    }
    uint8_t lv = mp_settings.backlight_level % MP_BACKLIGHT_LEVEL_COUNT;
    mp_app_state.bl_saved_level = lv;
    mp_app_state.bl_edit_level = lv;
    go_screen(MP_SCREEN_BACKLIGHT, 0);
}

static void bl_step(int dir)
{
    if (!mp_app_state.has_backlight) {
        return;
    }
    int cur = (int)(mp_app_state.bl_edit_level % MP_BACKLIGHT_LEVEL_COUNT);
    int nxt = cur + dir;
    if (nxt < 0) {
        nxt = 0;
    } else if (nxt > MP_BL_LEVEL_MAX) {
        nxt = MP_BL_LEVEL_MAX;
    }
    if (nxt != cur) {
        mp_app_state.bl_edit_level = (uint8_t)nxt;
        (void)mp_backlight_set(mp_app_state.bl_edit_level);
        page_commit();
    }
}

static void bl_page_confirm(void)
{
    mp_settings.backlight_level =
        mp_app_state.bl_edit_level % MP_BACKLIGHT_LEVEL_COUNT;
    (void)mp_backlight_set(mp_settings.backlight_level);
    (void)mp_settings_save();
    go_screen(MP_SCREEN_SETTINGS, 0);
}

static void bl_page_cancel(void)
{
    uint8_t saved = mp_app_state.bl_saved_level % MP_BACKLIGHT_LEVEL_COUNT;
    mp_app_state.bl_edit_level = saved;
    (void)mp_backlight_set(saved);
    go_screen(MP_SCREEN_SETTINGS, 0);
}

static void bl_popup_open(void)
{
    if (!mp_app_state.has_backlight) {
        return;
    }
    uint8_t lv = mp_settings.backlight_level % MP_BACKLIGHT_LEVEL_COUNT;
    mp_app_state.bl_saved_level = lv;
    mp_app_state.bl_edit_level = lv;
    mp_app_state.bl_popup = 1;
    mp_app_state.pending_ok = 0;
    page_commit();
}

static void bl_popup_confirm(void)
{
    mp_settings.backlight_level =
        mp_app_state.bl_edit_level % MP_BACKLIGHT_LEVEL_COUNT;
    (void)mp_backlight_set(mp_settings.backlight_level);
    (void)mp_settings_save();
    mp_app_state.bl_popup = 0;
    mp_app_state.pending_ok = 0;
    page_commit();
}

static void bl_popup_cancel(void)
{
    uint8_t saved = mp_app_state.bl_saved_level % MP_BACKLIGHT_LEVEL_COUNT;
    mp_app_state.bl_edit_level = saved;
    (void)mp_backlight_set(saved);
    mp_app_state.bl_popup = 0;
    mp_app_state.pending_ok = 0;
    page_commit();
}

static void settings_step(int dir)
{
    uint8_t count = 0;
    const uint8_t *items = settings_items(&count);
    uint8_t sel = mp_app_state.set_sel;
    if (sel >= count) {
        sel = 0;
    }
    for (uint8_t tries = 0; tries < count; tries++) {
        sel = (uint8_t)(((int)sel + dir + (int)count) % (int)count);
        if (items[sel] != SET_ITEM_STORAGE) {
            break;
        }
    }
    mp_app_state.set_sel = sel;
}

static void settings_activate(void)
{
    uint8_t count = 0;
    const uint8_t *items = settings_items(&count);
    if (mp_app_state.set_sel >= count) {
        return;
    }
    switch ((set_item_t)items[mp_app_state.set_sel]) {
    case SET_ITEM_BLE:
        if (mp_settings.ble_enabled) {
            mp_settings.ble_enabled = 0;
            mp_ble_stop();
            mp_app_state.ble_off_ms = now_ms();
        } else {
            mp_settings.ble_enabled = 1;
            ble_touch(now_ms());
            (void)mp_ble_start();
            mp_ble_adv(1);
        }
        (void)mp_settings_save();
        page_commit();
        break;
    case SET_ITEM_SLEEP:
        mp_settings.sleep_timeout_idx =
            (uint8_t)((mp_settings.sleep_timeout_idx + 1) % MP_SLEEP_TIMEOUT_COUNT);
        (void)mp_settings_save();
        page_commit();
        break;
    case SET_ITEM_LOCK_DISP:
        if (mp_settings.lock_display == MP_LOCK_DISP_IMAGE) {
            mp_settings.lock_display = MP_LOCK_DISP_STATS;
            (void)mp_settings_save();
            page_commit();
        } else if (mp_app_state.storage_ok && mp_store_lock_valid()) {
            mp_settings.lock_display = MP_LOCK_DISP_IMAGE;
            (void)mp_settings_save();
            page_commit();
        } else {
            /* 尚未上传有效锁屏图：保持阅读数据模式并提示先通过网页上传。 */
            go_screen(MP_SCREEN_NO_LOCK_IMG, 0);
        }
        break;
    case SET_ITEM_BACKLIGHT:
        if (mp_app_state.has_backlight) {
            enter_backlight();
        }
        break;
    case SET_ITEM_TIME:
        enter_time_set();
        break;
    case SET_ITEM_ABOUT:
        go_screen(MP_SCREEN_ABOUT, 0);
        break;
    case SET_ITEM_RESTORE:
        restore_enter();              /* 唯一入口：OK 短按进确认页（D15） */
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ *
 * 按键归约
 * ------------------------------------------------------------------ */

static void manual_full(void)
{
    display_request_full();           /* §5.2：不阻塞、不等待 */
}

static void debug_draw(uint16_t y)
{
    if (y > (MP_FB_H - 1)) {
        y = MP_FB_H - 1;
    }
    mp_app_state.debug_mode = 1;
    mp_app_state.debug_until_ms = now_ms() + 30000u;
    mp_app_state.pending_ok = 0;
    if (display_ready() && render() == 0) {
        /* 一条全宽横线：帧 bit=0 是黑。 */
        for (int x = 0; x < MP_FB_W; x++) {
            s_fb[y * MP_FB_ROW_BYTES + (x >> 3)] &= (uint8_t)~(0x80u >> (x & 7));
        }
        display_post(s_fb, DISPLAY_MODE_FULL);
        display_flush_wait(2000);
    }
}

static void debug_exit(void)
{
    if (!mp_app_state.debug_mode) {
        return;
    }
    mp_app_state.debug_mode = 0;
    mp_app_state.debug_until_ms = 0;
    if (mp_app_state.book_open) {
        close_book();
    }
    clear_restore_arm();              /* DEBUG 也是离开确认页的一条路径 */
    mp_app_state.screen = MP_SCREEN_CLOCK;
    mp_app_state.pending_ok = 0;
    mark_dirty();
    scene_commit(1);
}

static void ok_single_action(uint32_t now)
{
    switch (mp_app_state.screen) {
    case MP_SCREEN_CLOCK:
        open_select();
        break;
    case MP_SCREEN_SELECT:
        select_activate();
        break;
    case MP_SCREEN_READ:
        read_next();
        break;
    case MP_SCREEN_SETTINGS:
        settings_activate();
        break;
    case MP_SCREEN_ABOUT:
        go_screen(MP_SCREEN_SETTINGS, 0);
        break;
    case MP_SCREEN_LOCK:
        lock_show_prompt(now);        /* 锁屏图短按 ○ 跳解锁提示页；长按 ○ 才解锁 */
        break;
    case MP_SCREEN_EMPTY:
        rescan_books();               /* [○]刷新：重扫目录 */
        if (s_done_n > 0) {
            open_select();
        } else {
            page_commit();
        }
        break;
    case MP_SCREEN_ERROR:
        retry_action();
        break;
    case MP_SCREEN_RESTORE:
        break;
    case MP_SCREEN_BOOK_INFO:
        info_activate();
        break;
    case MP_SCREEN_JUMP:
        jump_apply();
        break;
    case MP_SCREEN_TIME_SET:
        time_next();
        break;
    case MP_SCREEN_BACKLIGHT:
        bl_page_confirm();
        break;
    case MP_SCREEN_DELETE:
    case MP_SCREEN_TIME_DONE:
    case MP_SCREEN_LOW_BATT:
    default:
        break;
    case MP_SCREEN_NO_LOCK_IMG:
        go_screen(MP_SCREEN_SETTINGS, 0);
        break;
    }
}

/* OK 的 350 ms 多击门（§2.3）。到点后补发本屏的单击动作。 */
static void poll_pending_ok(uint32_t now)
{
    if (!mp_app_state.pending_ok) {
        return;
    }
    if ((uint32_t)(now - mp_app_state.pending_ok_ms) <= MP_DCLICK_MS) {
        return;
    }
    mp_app_state.pending_ok = 0;
    ok_single_action(now);
}

static void on_ok_click(uint32_t now)
{
    if (mp_app_state.pending_ok &&
        (uint32_t)(now - mp_app_state.pending_ok_ms) <= MP_DCLICK_MS) {
        mp_app_state.pending_ok = 0;
        /* 双击：READ 上=上一页；其它屏=无动作且吞掉这对单击（§2.3）。 */
        if (mp_app_state.screen == MP_SCREEN_READ) {
            read_prev();
        }
        return;
    }
    mp_app_state.pending_ok = 1;
    mp_app_state.pending_ok_ms = now;
}

static void on_ok_long(uint32_t now)
{
    mp_app_state.pending_ok = 0;      /* 长按优先（§2.3） */

    if (mp_app_state.screen == MP_SCREEN_RESTORE) {
        /* 唯一例外：armed && 见过新的 OK DOWN 才提交（§8.3）。 */
        if (mp_app_state.restore_armed && mp_app_state.restore_key_ok) {
            restore_commit();
        }
        return;
    }
    if (mp_app_state.screen == MP_SCREEN_LOCK) {
        unlock(now);
        return;
    }
    if (mp_app_state.screen == MP_SCREEN_DELETE) {
        delete_commit();              /* 确认页：长按 OK 才真正删除 */
        return;
    }
    if (mp_app_state.screen == MP_SCREEN_TIME_SET ||
        mp_app_state.screen == MP_SCREEN_TIME_DONE ||
        mp_app_state.screen == MP_SCREEN_JUMP ||
        mp_app_state.screen == MP_SCREEN_BACKLIGHT ||
        mp_app_state.screen == MP_SCREEN_LOW_BATT) {
        return;                       /* 向导弹层/关机屏不把长按当锁屏 */
    }
    lock_enter();                     /* 普通屏 OK 长按 = 锁屏（§2.4） */
}

static void on_click(uint8_t key, uint32_t now)
{
    switch (mp_app_state.screen) {
    case MP_SCREEN_CLOCK:
        if (key == MP_KEY_LEFT) {
            open_settings();
        } else if (key == MP_KEY_RIGHT) {
            open_select();
        } else if (key == MP_KEY_MENU) {
            manual_full();
        }
        break;

    case MP_SCREEN_SELECT:
        if (key == MP_KEY_LEFT) {
            cursor_step(&mp_app_state.sel, s_done_n, -1);
            page_commit();
        } else if (key == MP_KEY_RIGHT) {
            cursor_step(&mp_app_state.sel, s_done_n, 1);
            page_commit();
        } else if (key == MP_KEY_CANCEL) {
            open_clock(0);
        } else if (key == MP_KEY_MENU) {
            manual_full();
        }
        break;

    case MP_SCREEN_READ:
        if (key == MP_KEY_LEFT) {
            read_prev();
        } else if (key == MP_KEY_RIGHT) {
            read_next();
        } else if (key == MP_KEY_MENU) {
            open_book_info();          /* 胶囊菜单短按打开书籍信息 */
        } else if (key == MP_KEY_CANCEL) {
            read_exit(MP_SCREEN_SELECT);
        }
        break;

    case MP_SCREEN_SETTINGS:
        if (key == MP_KEY_LEFT) {
            settings_step(-1);
            page_commit();
        } else if (key == MP_KEY_RIGHT) {
            settings_step(1);
            page_commit();
        } else if (key == MP_KEY_CANCEL) {
            open_clock(0);
        } else if (key == MP_KEY_MENU) {
            manual_full();
        }
        break;

    case MP_SCREEN_ABOUT:
        if (key == MP_KEY_CANCEL) {
            go_screen(MP_SCREEN_SETTINGS, 0);
        } else if (key == MP_KEY_MENU) {
            manual_full();
        }
        break;

    case MP_SCREEN_BOOK_INFO:
        if (key == MP_KEY_LEFT) {
            info_step(-1);
        } else if (key == MP_KEY_RIGHT) {
            info_step(1);
        } else if (key == MP_KEY_CANCEL) {
            go_screen(MP_SCREEN_READ, 0);
        } else if (key == MP_KEY_MENU) {
            manual_full();
        }
        break;

    case MP_SCREEN_JUMP:
        if (key == MP_KEY_LEFT) {
            mp_app_state.jump_pct =
                (uint8_t)(mp_app_state.jump_pct >= 5 ? mp_app_state.jump_pct - 5 : 0);
            page_commit();
        } else if (key == MP_KEY_RIGHT) {
            mp_app_state.jump_pct =
                (uint8_t)(mp_app_state.jump_pct <= 95 ? mp_app_state.jump_pct + 5 : 100);
            page_commit();
        } else if (key == MP_KEY_CANCEL) {
            go_screen(MP_SCREEN_BOOK_INFO, 0);
        }
        break;

    case MP_SCREEN_TIME_SET:
        if (key == MP_KEY_LEFT) {
            time_value_step(-1);
        } else if (key == MP_KEY_RIGHT) {
            time_value_step(1);
        } else if (key == MP_KEY_CANCEL) {
            go_screen(MP_SCREEN_SETTINGS, 0);
        }
        break;

    case MP_SCREEN_BACKLIGHT:
        if (key == MP_KEY_LEFT) {
            bl_step(-1);
        } else if (key == MP_KEY_RIGHT) {
            bl_step(1);
        } else if (key == MP_KEY_CANCEL) {
            bl_page_cancel();
        } else if (key == MP_KEY_MENU) {
            manual_full();
        }
        break;

    case MP_SCREEN_DELETE:
        if (key == MP_KEY_CANCEL) {
            go_screen(MP_SCREEN_BOOK_INFO, 0);
        }
        break;

    case MP_SCREEN_TIME_DONE:
    case MP_SCREEN_LOW_BATT:
        break;

    case MP_SCREEN_EMPTY:
        if (key == MP_KEY_CANCEL) {
            open_clock(0);
        } else if (key == MP_KEY_MENU) {
            rescan_books();
            if (s_done_n > 0) {
                open_select();
            } else {
                page_commit();
            }
        }
        break;

    case MP_SCREEN_ERROR:
        if (key == MP_KEY_CANCEL) {
            open_clock(0);
        } else if (key == MP_KEY_MENU) {
            retry_action();
        }
        break;

    case MP_SCREEN_RESTORE:
        if (key == MP_KEY_CANCEL) {
            restore_cancel();
        }
        break;

    case MP_SCREEN_NO_LOCK_IMG:
        if (key == MP_KEY_CANCEL) {
            go_screen(MP_SCREEN_SETTINGS, 0);
        } else if (key == MP_KEY_MENU) {
            manual_full();
        }
        break;

    case MP_SCREEN_LOCK:
        lock_show_prompt(now);
        break;

    default:
        break;
    }
}

static void on_long(uint8_t key, uint32_t now)
{
    switch (mp_app_state.screen) {
    case MP_SCREEN_CLOCK:
        if (key == MP_KEY_LEFT) {
            open_settings();
        } else if (key == MP_KEY_RIGHT) {
            open_select();
        } else if (key == MP_KEY_CANCEL) {
            manual_full();
        } else if (key == MP_KEY_MENU) {
            bl_popup_open();
        }
        break;

    case MP_SCREEN_SELECT:
        if (key == MP_KEY_RIGHT || key == MP_KEY_CANCEL) {
            open_clock(0);
        } else if (key == MP_KEY_MENU) {
            bl_popup_open();
        }
        break;

    case MP_SCREEN_READ:
        if (key == MP_KEY_RIGHT || key == MP_KEY_CANCEL) {
            read_exit(MP_SCREEN_CLOCK);   /* 保存进度 -> 时钟 */
        } else if (key == MP_KEY_MENU) {
            bl_popup_open();
        }
        break;

    case MP_SCREEN_SETTINGS:
        if (key == MP_KEY_LEFT || key == MP_KEY_CANCEL) {
            open_clock(0);
        } else if (key == MP_KEY_MENU) {
            bl_popup_open();
        }
        break;

    case MP_SCREEN_ABOUT:
        if (key == MP_KEY_CANCEL) {
            open_clock(0);
        } else if (key == MP_KEY_MENU) {
            bl_popup_open();
        }
        break;

    case MP_SCREEN_BOOK_INFO:
        if (key == MP_KEY_CANCEL) {
            go_screen(MP_SCREEN_READ, 0);
        } else if (key == MP_KEY_MENU) {
            bl_popup_open();
        }
        break;

    case MP_SCREEN_JUMP:
        if (key == MP_KEY_CANCEL) {
            go_screen(MP_SCREEN_BOOK_INFO, 0);
        }
        break;

    case MP_SCREEN_TIME_SET:
        if (key == MP_KEY_CANCEL) {
            go_screen(MP_SCREEN_SETTINGS, 0);
        }
        break;

    case MP_SCREEN_BACKLIGHT:
        if (key == MP_KEY_CANCEL) {
            bl_page_cancel();
        }
        break;

    case MP_SCREEN_DELETE:
        if (key == MP_KEY_CANCEL) {
            go_screen(MP_SCREEN_BOOK_INFO, 0);
        }
        break;

    case MP_SCREEN_TIME_DONE:
    case MP_SCREEN_LOW_BATT:
        break;

    case MP_SCREEN_EMPTY:
    case MP_SCREEN_ERROR:
        if (key == MP_KEY_CANCEL) {
            open_clock(0);
        } else if (key == MP_KEY_MENU) {
            bl_popup_open();
        }
        break;

    case MP_SCREEN_RESTORE:
        break;

    case MP_SCREEN_NO_LOCK_IMG:
        if (key == MP_KEY_CANCEL) {
            go_screen(MP_SCREEN_SETTINGS, 0);
        } else if (key == MP_KEY_MENU) {
            bl_popup_open();
        }
        break;

    case MP_SCREEN_LOCK:
        lock_show_prompt(now);
        break;

    default:
        break;
    }
}

void mp_app_key(uint8_t key, uint8_t ev, uint32_t now_ms_in)
{
    uint32_t now = now_ms_in;

    if (!s_booted) {
        return;
    }
    mp_app_state.last_input_ms = now;

    if (ev == KEY_EV_REPEAT) {
        return;                       /* 一律忽略（§2.1） */
    }

    if (mp_app_state.debug_mode) {
        /* DEBUG 期间只留一个按键出口：取消键长按退出。 */
        if (key == MP_KEY_CANCEL && ev == KEY_EV_LONG) {
            debug_exit();
        }
        return;
    }

    if (mp_app_state.bl_popup) {
        if (!mp_app_state.has_backlight) {
            mp_app_state.bl_popup = 0;
        } else {
            /* 快捷背光浮窗拦截按键：左右实时调档，○ 确认关闭，×/▭ 取消恢复。 */
            mp_app_state.pending_ok = 0;
            if (ev == KEY_EV_DOWN) {
                return;
            }
            if (key == MP_KEY_LEFT && ev == KEY_EV_CLICK) {
                bl_step(-1);
            } else if (key == MP_KEY_RIGHT && ev == KEY_EV_CLICK) {
                bl_step(1);
            } else if (key == MP_KEY_OK && ev == KEY_EV_CLICK) {
                bl_popup_confirm();
            } else if (key == MP_KEY_CANCEL || key == MP_KEY_MENU) {
                bl_popup_cancel();
            }
            return;
        }
    }

    /* 到期的 OK 单击先补发，保证"两次 CLICK 间隔 >350 ms"= 两次单击。 */
    poll_pending_ok(now);

    if (ev == KEY_EV_DOWN) {
        if (key == MP_KEY_OK) {
            /* armed 之后必须见到新的 OK DOWN 才认 LONG（§8.3）；
             * 不清 pending_ok——§2.3 的冻结算法只在 CLICK 上判双击。 */
            if (mp_app_state.restore_armed) {
                mp_app_state.restore_key_ok = 1;
            }
        } else {
            /* 别的键按下就把待发的 OK 单击作废（§2.1）。 */
            mp_app_state.pending_ok = 0;
        }
        if (mp_app_state.screen == MP_SCREEN_RESTORE && key != MP_KEY_OK) {
            mp_app_state.restore_key_ok = 0;
        }
        return;
    }

    if (key == MP_KEY_OK) {
        if (ev == KEY_EV_LONG) {
            on_ok_long(now);
        } else if (ev == KEY_EV_CLICK) {
            if (mp_app_state.screen == MP_SCREEN_RESTORE) {
                return;               /* 确认页 OK 短按无动作 */
            }
            on_ok_click(now);
        }
        return;
    }

    /* 非 OK 键：CLICK / LONG 都立刻取消待发的 OK 单击（§2.3）。 */
    mp_app_state.pending_ok = 0;
    if (ev == KEY_EV_CLICK) {
        on_click(key, now);
    } else if (ev == KEY_EV_LONG) {
        on_long(key, now);
    }
}

/* ------------------------------------------------------------------ *
 * BLE 上传会话
 * ------------------------------------------------------------------ */

static void upload_end_session(void)
{
    s_up_kind = UP_NONE;
    s_up_slot = 0xFF;
    s_up_written = 0;
    s_up_total = 0;
    s_up_notified = 0;
    s_up_chars = 0;
    s_up_gbk_lead = 0;
    mp_app_state.uploading = 0;
    mp_ble_set_upload_active(0);
    upload_pm_release();
}

static void upload_abort(void)
{
    if (s_up_kind == UP_BOOK && s_up_slot < MP_MAX_BOOKS) {
        mp_store_abort(s_up_slot);
    }
    /* font / lock 没有 abort API：下次 begin 会重建文件。 */
    ESP_LOGW(TAG, "upload aborted at %u/%u", (unsigned)s_up_written,
             (unsigned)s_up_total);
    upload_end_session();
}

static void notify_u24(uint8_t op, uint32_t v)
{
    /* 0x11 / 0x17：累计已写字节的高水位（不是本包 ack）。 */
    uint8_t p[4];
    p[0] = op;
    p[1] = (uint8_t)((v >> 16) & 0xFF);
    p[2] = (uint8_t)((v >> 8) & 0xFF);
    p[3] = (uint8_t)(v & 0xFF);
    (void)mp_ble_notify(p, sizeof(p));
}

static void reply1(uint8_t op)
{
    uint8_t p = op;
    (void)mp_ble_notify(&p, 1);
}

static void reply_st(uint8_t op, uint8_t st)
{
    uint8_t p[2] = {op, st};
    (void)mp_ble_notify(p, 2);
}

static void reply_st_idx(uint8_t op, uint8_t st, uint8_t idx)
{
    /* 0x10 的应答是 [op][st][slot]。 */
    uint8_t p[3] = {op, st, idx};
    (void)mp_ble_notify(p, 3);
}

/* 0x10 的失败应答也必须是 3 字节（PORT_SPEC §7.3）：原版 web 只在
 * b.length >= 3 时 resolve，2 字节会被当成不匹配、主机耗尽 5 s 超时，
 * 看不到 0x01/0x02/0xFD/0xFE 的具体状态。 */
static void reply_begin_fail(uint8_t st)
{
    reply_st_idx(MP_OP_BOOK_BEGIN, st, 0xFF);
}

static void reply_get_fail(uint8_t idx)
{
    /* 0x15 的失败应答是 [0x15][idx][0xFF]（索引在前，与 0x10 相反）。 */
    uint8_t p[3] = {MP_OP_BOOK_GET, idx, MP_ST_FAIL};
    (void)mp_ble_notify(p, 3);
}

/* 锁屏下被拒绝的 op：按各自的应答形状回失败，主机不会空等超时。 */
static void reply_locked_reject(uint8_t op)
{
    switch (op) {
    case MP_OP_BOOK_BEGIN:
        reply_begin_fail(MP_ST_FAIL);
        break;
    case MP_OP_BOOK_DATA:
    case MP_OP_FONT_DATA:
        notify_u24(op, 0);               /* “无会话”语义：written=0 */
        break;
    case MP_OP_BOOK_END: {
        uint8_t p[6] = {MP_OP_BOOK_END, MP_ST_FAIL, 0xFF, 0, 0, 0};
        (void)mp_ble_notify(p, sizeof(p));
        break;
    }
    default:
        reply_st(op, MP_ST_FAIL);
        break;
    }
}

/* ------------------------------------------------------------------ *
 * BLE：各 op 的实现（PORT_SPEC §7.3 / §7.4）
 * ------------------------------------------------------------------ */

static void ble_book_begin(const mp_ble_req_t *req)
{
    const uint8_t *d = req->data;
    uint8_t n = req->len;
    uint8_t title_len;
    uint8_t take;
    char title[MP_TITLE_MAX + 1];

    if (n < 7) {                      /* 1 + req->len < 8 的等价式（§7.3） */
        reply_begin_fail(MP_ST_BAD_PARAM);
        return;
    }
    title_len = d[0];
    if (title_len < 2 || (uint32_t)n < (uint32_t)title_len + 5u) {
        reply_begin_fail(MP_ST_BAD_PARAM);
        return;
    }
    uint32_t total = ((uint32_t)d[1 + title_len] << 16) |
                     ((uint32_t)d[2 + title_len] << 8) |
                     (uint32_t)d[3 + title_len];
    uint8_t enc = d[4 + title_len];
    if (total == 0) {
        reply_begin_fail(MP_ST_BAD_PARAM);
        return;
    }
    if (mp_app_state.uploading) {
        reply_begin_fail(MP_ST_BUSY);
        return;
    }
    if (!mp_app_state.storage_ok) {
        reply_begin_fail(MP_ST_FAIL);
        return;
    }

    int slot = mp_store_find_free_slot();
    if (slot < 0) {
        reply_begin_fail(MP_ST_SLOTS_FULL);
        return;
    }

    uint64_t total_b = 0;
    uint64_t free_b = 0;
    if (store_space(&total_b, &free_b) == 0) {
        if (free_b < (uint64_t)total + 64u * 1024u) {
            reply_begin_fail(MP_ST_NO_SPACE);
            return;
        }
    }

    /* 标题截到 19 B，且不切断双字节（§6.6-10）。 */
    take = title_len > MP_TITLE_MAX ? MP_TITLE_MAX : title_len;
    if (take == MP_TITLE_MAX && title_len > take) {
        uint8_t last = d[take - 1];
        if (last >= 0xA1 && last <= 0xF7) {
            take--;
        }
    }
    memcpy(title, &d[1], take);
    title[take] = '\0';

    if (mp_store_begin((uint8_t)slot, title, total,
                       enc == MP_ENC_GB2312 ? MP_ENC_GB2312 : MP_ENC_ASCII) != 0) {
        reply_begin_fail(MP_ST_FAIL);
        return;
    }

    s_up_kind = UP_BOOK;
    s_up_slot = (uint8_t)slot;
    s_up_written = 0;
    s_up_total = total;
    s_up_notified = 0;
    s_up_chars = 0;
    s_up_enc = (enc == MP_ENC_GB2312) ? MP_ENC_GB2312 : MP_ENC_ASCII;
    s_up_gbk_lead = 0;
    mp_app_state.uploading = 1;
    mp_app_state.last_pkt_ms = now_ms();
    upload_pm_acquire();
    mp_ble_set_upload_active(1);

    reply_st_idx(MP_OP_BOOK_BEGIN, MP_ST_OK, (uint8_t)slot);
    ESP_LOGI(TAG, "book begin slot=%u len=%u", (unsigned)slot, (unsigned)total);
}

/* 上传时顺带统计字数（信息页用）。GB2312 只把 hi>=0xA1 && lo>=0xA1 当一
 * 个全角字；其余字节按单字节算。空白不算字数。 */
static void upload_count_chars(const uint8_t *d, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        uint8_t c = d[i];
        if (s_up_enc == MP_ENC_GB2312) {
            if (s_up_gbk_lead) {
                if (c >= 0xA1) {
                    s_up_chars++;
                    s_up_gbk_lead = 0;
                    continue;
                }
                s_up_chars++;          /* 落单的 lead 也算一个字符 */
                s_up_gbk_lead = 0;
            }
            if (c >= 0xA1) {
                s_up_gbk_lead = 1;
            } else if (!copy_space(c)) {
                s_up_chars++;
            }
        } else if (!copy_space(c)) {
            s_up_chars++;
        }
    }
}

static void ble_data_chunk(const mp_ble_req_t *req, uint8_t kind)
{
    uint8_t op = req->op;
    if (s_up_kind != kind) {
        notify_u24(op, 0);            /* 无会话：回 written=0（M 的既有行为） */
        return;
    }
    if ((uint32_t)s_up_written + req->len > s_up_total) {
        upload_abort();
        notify_u24(op, s_up_written);
        return;
    }

    int rc;
    if (kind == UP_BOOK) {
        rc = mp_store_write(s_up_slot, s_up_written, req->data, req->len);
    } else {
        rc = mp_store_font_write(s_up_written, req->data, req->len);
    }
    if (rc != 0) {
        upload_abort();
        notify_u24(op, s_up_written);
        return;
    }

    s_up_written += req->len;
    if (kind == UP_BOOK) {
        upload_count_chars(req->data, req->len);
    }
    mp_app_state.last_pkt_ms = now_ms();

    if ((s_up_written / MP_BLE_NOTIFY_STEP) > (s_up_notified / MP_BLE_NOTIFY_STEP)) {
        s_up_notified = s_up_written;
        notify_u24(op, s_up_written);
    }
}

static void ble_book_end(void)
{
    uint8_t out[6] = {MP_OP_BOOK_END, MP_ST_FAIL, 0xFF, 0, 0, 0};

    if (s_up_kind != UP_BOOK) {
        (void)mp_ble_notify(out, sizeof(out));
        return;
    }
    out[2] = s_up_slot;
    out[3] = (uint8_t)((s_up_written >> 16) & 0xFF);
    out[4] = (uint8_t)((s_up_written >> 8) & 0xFF);
    out[5] = (uint8_t)(s_up_written & 0xFF);

    if (s_up_written != s_up_total) {
        ESP_LOGW(TAG, "book end short: %u/%u", (unsigned)s_up_written,
                 (unsigned)s_up_total);
        mp_store_abort(s_up_slot);
        upload_end_session();
        (void)mp_ble_notify(out, sizeof(out));   /* st 仍是 0xFF */
        return;
    }

    uint8_t idx = s_up_slot;
    if (s_up_gbk_lead) {
        s_up_chars++;                 /* 文件末尾落单的 GB2312 lead */
        s_up_gbk_lead = 0;
    }
    if (mp_store_set_chars(idx, s_up_chars) != 0 ||
        mp_store_finish(idx) != 0) {
        mp_store_abort(idx);
        upload_end_session();
        (void)mp_ble_notify(out, sizeof(out));
        return;
    }
    upload_end_session();
    rescan_books();
    out[1] = MP_ST_OK;
    (void)mp_ble_notify(out, sizeof(out));

    if (mp_app_state.screen == MP_SCREEN_SELECT) {
        page_commit();                /* 列表里立刻出现新书 */
    } else if (mp_app_state.screen == MP_SCREEN_EMPTY) {
        open_select();                /* 新书已入库，EMPTY 不再是事实 */
    }
}

static void ble_book_delete(const mp_ble_req_t *req)
{
    uint8_t idx = req->len >= 1 ? req->data[0] : 0xFF;
    if (req->len < 1 || idx >= MP_MAX_BOOKS) {
        reply_st(MP_OP_BOOK_DELETE, MP_ST_FAIL);
        return;
    }
    if (mp_app_state.uploading && idx == s_up_slot) {
        reply_st(MP_OP_BOOK_DELETE, MP_ST_BUSY);
        return;
    }
    if (!mp_app_state.storage_ok || !mp_store_is_done(idx)) {
        reply_st(MP_OP_BOOK_DELETE, MP_ST_FAIL);
        return;
    }

    if (mp_app_state.book_open && mp_app_state.book_idx == idx) {
        uint8_t was_read = (mp_app_state.screen == MP_SCREEN_READ) ? 1 : 0;
        mp_app_flush_progress();
        close_book();
        if (was_read) {
            go_screen(MP_SCREEN_CLOCK, 1);   /* §7.4：先回 CLOCK 再删 */
        }
    }

    if (mp_store_delete(idx) != 0) {
        reply_st(MP_OP_BOOK_DELETE, MP_ST_FAIL);
        return;
    }
    (void)mp_progress_save(idx, 0);
    rescan_books();
    if (mp_app_state.screen == MP_SCREEN_SELECT) {
        if (s_done_n == 0) {
            mp_app_state.sel = 0;
            go_screen(MP_SCREEN_EMPTY, 0);   /* 空列表进 EMPTY（§9.5） */
        } else {
            if (mp_app_state.sel >= s_done_n) {
                mp_app_state.sel = (uint8_t)(s_done_n - 1);
            }
            page_commit();
        }
    }
    reply_st(MP_OP_BOOK_DELETE, MP_ST_OK);
}

static void ble_book_get(const mp_ble_req_t *req)
{
    uint8_t idx = req->len >= 1 ? req->data[0] : 0xFF;
    mp_book_t b;

    memset(&b, 0, sizeof(b));   /* entry_title() 只写到首个 NUL：其余字节必须先清零 */
    if (req->len < 1 || idx >= MP_MAX_BOOKS) {
        reply_get_fail(idx);
        return;
    }
    if (!mp_app_state.storage_ok || !mp_store_is_done(idx) ||
        mp_store_read(idx, &b) != 0) {
        reply_get_fail(idx);
        return;
    }

    uint8_t p1[15];
    uint8_t p2[11];
    uint8_t p3[7];
    memset(p1, 0, sizeof(p1));
    memset(p2, 0, sizeof(p2));
    memset(p3, 0, sizeof(p3));
    p1[0] = MP_OP_BOOK_GET;
    p1[1] = idx;
    p1[2] = 0x00;
    memcpy(&p1[3], b.title, 12);
    p2[0] = MP_OP_BOOK_GET;
    p2[1] = idx;
    p2[2] = 0x01;
    memcpy(&p2[3], &b.title[12], 8);
    p3[0] = MP_OP_BOOK_GET;
    p3[1] = idx;
    p3[2] = 0x02;
    p3[3] = (uint8_t)((b.len >> 16) & 0xFF);
    p3[4] = (uint8_t)((b.len >> 8) & 0xFF);
    p3[5] = (uint8_t)(b.len & 0xFF);
    p3[6] = b.enc == MP_ENC_GB2312 ? MP_ENC_GB2312 : MP_ENC_ASCII;

    (void)mp_ble_notify(p1, sizeof(p1));   /* 三条背靠背，不插延时 */
    (void)mp_ble_notify(p2, sizeof(p2));
    (void)mp_ble_notify(p3, sizeof(p3));
}

static void ble_status(void)
{
    uint16_t mtu = mp_ble_mtu();
    uint8_t p[5];
    p[0] = MP_OP_STATUS;
    /* 内嵌 HZK16 永远可渲染，font_ok 不依赖 SD 是否挂载（PROTOCOL 0x19）。 */
    p[1] = mp_store_font_ok() ? 1 : 0;
    /* book_cnt 永远是真实 DONE 数（§7.3：不用 0xFF 表示异常）。 */
    p[2] = mp_app_state.storage_ok ? (uint8_t)mp_store_count_done() : 0;
    p[3] = mp_store_lock_valid() ? 1 : 0;
    p[4] = mtu > 255 ? 255 : (uint8_t)mtu;
    (void)mp_ble_notify(p, sizeof(p));
}

static void ble_font_begin(const mp_ble_req_t *req)
{
    uint32_t total = 0;
    if (req->len >= 3) {
        total = ((uint32_t)req->data[0] << 16) | ((uint32_t)req->data[1] << 8) |
                (uint32_t)req->data[2];
    } else {
        reply_st(MP_OP_FONT_BEGIN, MP_ST_BAD_PARAM);
        return;
    }
    if (mp_app_state.uploading) {
        reply_st(MP_OP_FONT_BEGIN, MP_ST_BUSY);
        return;
    }
    if (!mp_app_state.storage_ok || mp_store_font_begin() != 0) {
        reply_st(MP_OP_FONT_BEGIN, MP_ST_FAIL);
        return;
    }
    s_up_kind = UP_FONT;
    s_up_slot = 0xFF;
    s_up_written = 0;
    s_up_total = total;
    s_up_notified = 0;
    mp_app_state.uploading = 1;
    mp_app_state.last_pkt_ms = now_ms();
    upload_pm_acquire();
    mp_ble_set_upload_active(1);
    reply_st(MP_OP_FONT_BEGIN, MP_ST_OK);
}

static void ble_font_end(void)
{
    if (s_up_kind != UP_FONT) {
        reply_st(MP_OP_FONT_END, MP_ST_FAIL);
        return;
    }
    if (s_up_written != MP_BLE_FONT_BYTES) {
        upload_end_session();
        reply_st(MP_OP_FONT_END, MP_ST_FAIL);
        return;
    }
    uint8_t st = mp_store_font_finish() == 0 ? MP_ST_OK : MP_ST_FAIL;
    upload_end_session();
    reply_st(MP_OP_FONT_END, st);
}

static void ble_lock_begin(const mp_ble_req_t *req)
{
    uint32_t total = 0;
    if (req->len >= 3) {
        total = ((uint32_t)req->data[0] << 16) | ((uint32_t)req->data[1] << 8) |
                (uint32_t)req->data[2];
    } else {
        reply_st(MP_OP_LOCK_BEGIN, MP_ST_BAD_PARAM);
        return;
    }
    if (total != MP_BLE_LOCK_BYTES) {
        reply_st(MP_OP_LOCK_BEGIN, MP_ST_BAD_PARAM);
        return;
    }
    if (mp_app_state.uploading) {
        reply_st(MP_OP_LOCK_BEGIN, MP_ST_BUSY);
        return;
    }
    if (!mp_app_state.storage_ok || mp_store_lock_begin() != 0) {
        reply_st(MP_OP_LOCK_BEGIN, MP_ST_FAIL);
        return;
    }
    s_up_kind = UP_LOCK;
    s_up_slot = 0xFF;
    s_up_written = 0;
    s_up_total = total;
    s_up_notified = 0;
    mp_app_state.uploading = 1;
    mp_app_state.last_pkt_ms = now_ms();
    upload_pm_acquire();
    mp_ble_set_upload_active(1);
    reply_st(MP_OP_LOCK_BEGIN, MP_ST_OK);
}

static void ble_lock_data(const mp_ble_req_t *req)
{
    if (s_up_kind != UP_LOCK || req->len < 3) {
        reply_st(MP_OP_LOCK_DATA, MP_ST_FAIL);
        return;
    }
    uint32_t off = ((uint32_t)req->data[0] << 8) | (uint32_t)req->data[1];
    uint8_t dlen = (uint8_t)(req->len - 2);
    if (off + dlen > MP_BLE_LOCK_BYTES) {
        reply_st(MP_OP_LOCK_DATA, MP_ST_FAIL);
        return;
    }
    if (mp_store_lock_write(off, &req->data[2], dlen) != 0) {
        reply_st(MP_OP_LOCK_DATA, MP_ST_FAIL);
        return;
    }
    mp_app_state.last_pkt_ms = now_ms();
    reply_st(MP_OP_LOCK_DATA, MP_ST_OK);
}

static void ble_lock_end(void)
{
    if (s_up_kind != UP_LOCK) {
        reply_st(MP_OP_LOCK_END, MP_ST_FAIL);
        return;
    }
    if (mp_store_lock_finish() == 0) {
        /* 0x33 走 RAM 缓存：不重载的话同一连接内还会回上传前的旧图，
         * 而 0x19 已按磁盘标志报 lock_ok=1。 */
        load_lock_image();
        upload_end_session();
        reply_st(MP_OP_LOCK_END, MP_ST_OK);
        return;
    }
    upload_end_session();
    reply_st(MP_OP_LOCK_END, MP_ST_FAIL);
}

static void ble_lock_read(const mp_ble_req_t *req)
{
    if (req->len < 2) {
        reply_st(MP_OP_LOCK_READ, MP_ST_BUSY);   /* 空包 = legacy，不支持 */
        return;
    }
    uint32_t off = ((uint32_t)req->data[0] << 8) | (uint32_t)req->data[1];
    if (off >= MP_BLE_LOCK_BYTES) {
        uint8_t p[3] = {MP_OP_LOCK_READ, MP_ST_LOCK_END_OFF, MP_ST_LOCK_END_OFF};
        (void)mp_ble_notify(p, sizeof(p));
        return;
    }

    uint32_t dlen = MP_BLE_LOCK_BYTES - off;
    if (dlen > MP_BLE_LOCK_CHUNK) {
        dlen = MP_BLE_LOCK_CHUNK;
    }
    /* 0x33 的块长恒为 160（web 固定步进，缩小会留空洞）。MTU 不够时
     * notify 会被 mp_ble 丢掉、主机每块等 2 s；这里立刻回“不支持”，
     * 让主机马上失败（PORT_SPEC §7.7 的“MTU<166 时明示”）。 */
    uint16_t mtu = mp_ble_mtu();
    if ((uint32_t)(3 + dlen) > (uint32_t)((mtu > 3 ? mtu : 3) - 3)) {
        ESP_LOGW(TAG, "0x33 needs MTU>=166, have %u", (unsigned)mtu);
        reply_st(MP_OP_LOCK_READ, MP_ST_BUSY);
        return;
    }
    if (!s_lock_valid) {
        load_lock_image();
    }
    if (!s_lock_valid) {
        memset(s_lock_img, 0, sizeof(s_lock_img));   /* 无图也回全 0 块（M 的怪癖） */
    }
    uint8_t out[3 + MP_BLE_LOCK_CHUNK];
    out[0] = MP_OP_LOCK_READ;
    out[1] = (uint8_t)((off >> 8) & 0xFF);
    out[2] = (uint8_t)(off & 0xFF);
    memcpy(&out[3], &s_lock_img[off], dlen);
    (void)mp_ble_notify(out, (uint16_t)(3 + dlen));
}

static void ble_settings_read(void)
{
    uint8_t p[6];
    p[0] = MP_OP_SETTINGS_READ;
    p[1] = mp_settings.ble_enabled ? 1 : 0;
    p[2] = mp_settings.sleep_timeout_idx;
    p[3] = READER_VERSION_MAJOR;
    p[4] = READER_VERSION_MINOR;
    p[5] = READER_VERSION_PATCH;
    (void)mp_ble_notify(p, sizeof(p));
}

static void ble_settings_write(const mp_ble_req_t *req)
{
    if (req->len < 2) {
        reply_st(MP_OP_SETTINGS_WRITE, MP_ST_BAD_PARAM);
        return;
    }
    uint8_t ble = req->data[0] ? 1 : 0;
    uint8_t idx = req->data[1];
    if (idx >= MP_SLEEP_TIMEOUT_COUNT) {
        idx = MP_SLEEP_IDX_DEFAULT;   /* 越界夹到 2（§7.3） */
    }
    mp_settings.ble_enabled = ble;
    mp_settings.sleep_timeout_idx = idx;
    if (ble) {
        ble_touch(now_ms());
        (void)mp_ble_start();
    }
    mp_ble_adv(ble);
    int rc = mp_settings_save();
    reply_st(MP_OP_SETTINGS_WRITE, rc == 0 ? MP_ST_OK : MP_ST_FAIL);
    if (mp_app_state.screen == MP_SCREEN_SETTINGS && !mp_app_state.uploading) {
        page_commit();
    }
}

static void apply_time(uint32_t unix_s, uint16_t year, uint8_t month, uint8_t day,
                       uint8_t week, uint32_t now)
{
    mp_app_state.time_base_unix = unix_s;
    mp_app_state.time_base_ms = now;
    mp_app_state.month = (month >= 1 && month <= 12) ? month : 1;
    mp_app_state.day = (day >= 1 && day <= 31) ? day : 1;
    mp_app_state.week = (week >= 1 && week <= 7) ? week : MP_TIME_DEFAULT_WEEK;
    if (year >= 2000 && year <= 2100) {
        s_year = year;
    }
    mp_app_state.hour = (uint8_t)((unix_s / 3600u) % 24u);
    mp_app_state.min = (uint8_t)((unix_s / 60u) % 60u);
    s_day_index = unix_s / 86400u;

    /* 同步写入系统 RTC，保证跨深睡继续走时。 */
    struct timeval tv = {.tv_sec = (time_t)unix_s, .tv_usec = 0};
    (void)settimeofday(&tv, NULL);
    s_rtc.magic = MP_RTC_MAGIC;
    s_rtc.time_synced = 1;

    if (mp_app_state.screen == MP_SCREEN_CLOCK) {
        page_commit();
    }
}

static void ble_set_time(const mp_ble_req_t *req)
{
    if (req->len < 9) {
        return;
    }
    const uint8_t *d = req->data;
    uint32_t unix_s = ((uint32_t)d[0] << 24) | ((uint32_t)d[1] << 16) |
                      ((uint32_t)d[2] << 8) | (uint32_t)d[3];
    uint16_t year = (uint16_t)(((uint16_t)d[4] << 8) | d[5]);
    apply_time(unix_s, year, d[6], d[7], d[8], now_ms());
}

void mp_app_ble_req(const mp_ble_req_t *req)
{
    if (!req) {
        return;
    }
    const uint8_t op = req->op;

    /* 锁屏白名单（§7.4）：只读/设置照常，切屏、调试、上传一律失败。
     * GATT 是 No_Security，锁屏期间任何对端都不能 unlink 锁图/字库、
     * 压缩目录或开始上传会话。 */
    if (mp_app_state.screen == MP_SCREEN_LOCK) {
        switch (op) {
        case MP_OP_BOOK_COUNT:
        case MP_OP_BOOK_GET:
        case MP_OP_STATUS:
        case MP_OP_LOCK_READ:
        case MP_OP_SETTINGS_READ:
        case MP_OP_SETTINGS_WRITE:
            break;
        case MP_OP_EBOOK_CLOSE:
            mp_app_flush_progress();
            reply1(MP_OP_EBOOK_CLOSE);   /* 不切屏、不刷新 */
            return;
        default:
            /* 0x20/0x40/0x41/0x13 与全部上传类（0x10..0x12、0x16..0x18、
             * 0x1A、0x30..0x32）都走这里。 */
            reply_locked_reject(op);
            return;
        }
    }

    ble_touch(now_ms());

    switch (op) {
    case MP_OP_BOOK_BEGIN:
        ble_book_begin(req);
        break;
    case MP_OP_BOOK_DATA:
        ble_data_chunk(req, UP_BOOK);
        break;
    case MP_OP_BOOK_END:
        ble_book_end();
        break;
    case MP_OP_BOOK_DELETE:
        ble_book_delete(req);
        break;
    case MP_OP_BOOK_COUNT: {
        if (!mp_app_state.storage_ok) {
            reply_st(MP_OP_BOOK_COUNT, 0);
        } else {
            reply_st(MP_OP_BOOK_COUNT, (uint8_t)mp_store_count_done());
        }
        break;
    }
    case MP_OP_BOOK_GET:
        ble_book_get(req);
        break;
    case MP_OP_FONT_BEGIN:
        ble_font_begin(req);
        break;
    case MP_OP_FONT_DATA:
        ble_data_chunk(req, UP_FONT);
        break;
    case MP_OP_FONT_END:
        ble_font_end();
        break;
    case MP_OP_STATUS:
        ble_status();
        break;
    case MP_OP_CATALOG_COMPACT:
        if (mp_app_state.uploading) {
            reply_st(MP_OP_CATALOG_COMPACT, MP_ST_BUSY);
        } else if (!mp_app_state.storage_ok) {
            reply_st(MP_OP_CATALOG_COMPACT, MP_ST_FAIL);
        } else {
            int rc = mp_store_compact();
            rescan_books();
            reply_st(MP_OP_CATALOG_COMPACT, rc == 0 ? MP_ST_OK : MP_ST_FAIL);
        }
        break;
    case MP_OP_EBOOK_OPEN: {
        uint8_t idx = req->len >= 1 ? req->data[0] : 0xFF;
        if (mp_app_state.debug_mode || mp_app_state.uploading) {
            reply_st(MP_OP_EBOOK_OPEN, MP_ST_BUSY);
            break;
        }
        if (mp_app_state.storage_ok && idx < MP_MAX_BOOKS &&
            book_open_slot(idx) == 0) {
            go_screen(MP_SCREEN_READ, 0);
            reply_st(MP_OP_EBOOK_OPEN, MP_ST_OK);
        } else {
            reply_st(MP_OP_EBOOK_OPEN, MP_ST_FAIL);   /* 保持当前屏 */
        }
        break;
    }
    case MP_OP_EBOOK_CLOSE:
        mp_app_flush_progress();
        if (mp_app_state.debug_mode) {
            reply1(MP_OP_EBOOK_CLOSE);
            break;
        }
        if (mp_app_state.book_open) {
            close_book();
        }
        clear_restore_arm();             /* BLE 0x21 也是离开确认页的一条路径 */
        mp_app_state.pending_ok = 0;     /* 切屏取消待发 OK 单击（§2.3） */
        mp_app_state.screen = MP_SCREEN_CLOCK;
        mark_dirty();
        scene_commit(1);                 /* §7.3：保存进度、回 CLOCK、全刷 */
        reply1(MP_OP_EBOOK_CLOSE);
        break;
    case MP_OP_LOCK_BEGIN:
        ble_lock_begin(req);
        break;
    case MP_OP_LOCK_DATA:
        ble_lock_data(req);
        break;
    case MP_OP_LOCK_END:
        ble_lock_end();
        break;
    case MP_OP_LOCK_READ:
        ble_lock_read(req);
        break;
    case MP_OP_SETTINGS_READ:
        ble_settings_read();
        break;
    case MP_OP_SETTINGS_WRITE:
        ble_settings_write(req);
        break;
    case MP_OP_DEBUG_DRAW: {
        if (req->len < 2) {
            reply_st(MP_OP_DEBUG_DRAW, MP_ST_BAD_PARAM);
            break;
        }
        uint16_t y = (uint16_t)(((uint16_t)req->data[0] << 8) | req->data[1]);
        if (y > (MP_FB_H - 1)) {
            y = MP_FB_H - 1;
        }
        debug_draw(y);
        uint8_t p[3] = {MP_OP_DEBUG_DRAW, MP_ST_OK, (uint8_t)y};
        (void)mp_ble_notify(p, sizeof(p));
        break;
    }
    case MP_OP_DEBUG_EXIT:
        debug_exit();
        reply_st(MP_OP_DEBUG_EXIT, MP_ST_OK);
        break;
    case MP_OP_PC1_WEAK_PD:
    case MP_OP_PC1_STRONG_PD:
    case MP_OP_PC1_HIZ: {
        /* E2S 没有 PC1：回充电检测脚 GPIO5 的电平（1=高，0=低），不驱动引脚。 */
        uint8_t level = pw()->charging ? 0 : 1;
        reply_st(op, level);
        break;
    }
    case MP_OP_SET_TIME:
        ble_set_time(req);
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ *
 * BLE 回调（host 任务：只允许置标志/拷数据）
 * ------------------------------------------------------------------ */

static void on_ble_time(const mp_time_t *t)
{
    if (!t) {
        return;
    }
    s_time_inbox = *t;
    s_time_flag = 1;
}

static void on_ble_conn(uint8_t connected)
{
    if (connected) {
        s_conn_up = 1;
    } else {
        s_conn_lost = 1;
    }
}

/* ------------------------------------------------------------------ *
 * 走时
 * ------------------------------------------------------------------ */

static uint8_t days_in_month(uint8_t m, uint16_t y)
{
    static const uint8_t days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m < 1 || m > 12) {
        return 31;
    }
    if (m == 2 && ((y % 4u == 0 && y % 100u != 0) || y % 400u == 0)) {
        return 29;
    }
    return days[m - 1];
}

static void take_pending_time(uint32_t now)
{
    mp_time_t t;
    if (!s_time_flag) {
        return;
    }
    t = s_time_inbox;                 /* 先拷贝再清标志：期间再来一包会下一拍重放 */
    s_time_flag = 0;
    ble_touch(now);
    apply_time(t.unix, t.year, t.month, t.day, t.week, now);
}

static void time_update(uint32_t now)
{
    uint32_t elapsed_s;
    uint32_t t;
    uint8_t changed = 0;

    if (now < mp_app_state.time_base_ms) {
        mp_app_state.time_base_ms = now;
    }
    elapsed_s = (now - mp_app_state.time_base_ms) / 1000u;
    t = mp_app_state.time_base_unix + elapsed_s;

    uint8_t h = (uint8_t)((t / 3600u) % 24u);
    uint8_t mi = (uint8_t)((t / 60u) % 60u);
    if (h != mp_app_state.hour || mi != mp_app_state.min) {
        mp_app_state.hour = h;
        mp_app_state.min = mi;
        changed = 1;
    }

    uint32_t day = t / 86400u;
    while (s_day_index < day) {
        s_day_index++;
        if (++mp_app_state.day > days_in_month(mp_app_state.month, s_year)) {
            mp_app_state.day = 1;
            if (++mp_app_state.month > 12) {
                mp_app_state.month = 1;
                s_year++;
            }
        }
        mp_app_state.week = (uint8_t)(mp_app_state.week % 7u + 1u);
        changed = 1;
    }

    /* 分钟变化只重绘时钟页（M §7.4）：阅读时不去打扰，锁屏/DEBUG/上传也不画。 */
    if (changed && mp_app_state.screen == MP_SCREEN_CLOCK &&
        !mp_app_state.uploading && !mp_app_state.debug_mode) {
        page_commit();
    }
}

/* ------------------------------------------------------------------ *
 * 自动锁屏 / 蓝牙自动关闭 / 深睡关机 / 事件驱动 tick
 * ------------------------------------------------------------------ */

static void __attribute__((noreturn)) low_batt_shutdown(void)
{
    ESP_LOGW(TAG, "low battery shutdown: flushing state and entering deep sleep");
    mp_app_state.bl_popup = 0;
    mp_app_state.pending_ok = 0;
    clear_restore_arm();
    if (mp_app_state.uploading) {
        upload_abort();
    }
    read_time_flush(now_ms());
    mp_app_flush_progress();
    s_rtc.deep_locked = 0;
    mp_ble_stop();

    /* 终态空电池全刷（不使用 scene_commit 的异步 request_full，必须等刷完再睡）。 */
    mp_app_state.screen = MP_SCREEN_LOW_BATT;
    if (display_ready() && render() == 0) {
        display_post(s_fb, DISPLAY_MODE_FULL);
        display_flush_wait(8000);
    }

    display_sleep();
    mp_backlight_suspend();
    storage_unmount();
    sleep_hold_power_pins();
    vTaskDelay(pdMS_TO_TICKS(1000));

    enter_ext1_deep_sleep((1ULL << BOARD_CHARGE_DET_GPIO) |
                          (1ULL << BOARD_KEY_OK_GPIO));
}

static bool low_batt_triggered(const power_status_t *p)
{
    return p && p->valid && !p->charging && p->percent < MP_LOW_BATT_SHUTDOWN_PCT;
}

static uint8_t auto_lock_blocked(void)
{
    if (mp_app_state.screen == MP_SCREEN_LOCK ||
        mp_app_state.screen == MP_SCREEN_RESTORE ||
        mp_app_state.screen == MP_SCREEN_BOOK_INFO ||
        mp_app_state.screen == MP_SCREEN_JUMP ||
        mp_app_state.screen == MP_SCREEN_DELETE ||
        mp_app_state.screen == MP_SCREEN_TIME_SET ||
        mp_app_state.screen == MP_SCREEN_TIME_DONE ||
        mp_app_state.screen == MP_SCREEN_BACKLIGHT ||
        mp_app_state.screen == MP_SCREEN_LOW_BATT) {
        return 1;
    }
    if (mp_app_state.debug_mode || mp_app_state.uploading) {
        return 1;
    }
    if (mp_ble_connected()) {
        return 1;                     /* 连接期间不自动锁（M app.c:190-208） */
    }
    return 0;
}

static uint8_t auto_lock_due(uint32_t now)
{
    uint8_t idx;
    uint32_t timeout_ms;

    if (auto_lock_blocked()) {
        return 0;
    }
    idx = mp_settings.sleep_timeout_idx;
    if (idx >= MP_SLEEP_TIMEOUT_COUNT) {
        idx = MP_SLEEP_IDX_DEFAULT;
    }
    timeout_ms = (uint32_t)mp_sleep_timeout_s[idx] * 1000u;
    return (uint32_t)(now - mp_app_state.last_input_ms) >= timeout_ms;
}

static void ble_auto_disable(uint32_t now, const char *reason)
{
    mp_settings.ble_enabled = 0;
    mp_ble_stop();
    (void)mp_settings_save();
    mp_app_state.ble_off_ms = now ? now : 1u;
    ESP_LOGI(TAG, "ble auto off: %s", reason);
    if (mp_app_state.screen != MP_SCREEN_LOCK &&
        mp_app_state.screen != MP_SCREEN_READ &&
        mp_app_state.screen != MP_SCREEN_LOW_BATT &&
        mp_app_state.screen != MP_SCREEN_TIME_DONE &&
        !mp_app_state.uploading && !mp_app_state.debug_mode) {
        page_commit();
    }
}

static void ble_auto_off_check(uint32_t now)
{
    if (!mp_settings.ble_enabled || mp_app_state.uploading) {
        return;
    }
    if (mp_app_state.screen == MP_SCREEN_LOCK &&
        (uint32_t)(now - mp_app_state.lock_enter_ms) >= MP_BLE_LOCK_OFF_MS &&
        (uint32_t)(now - mp_app_state.ble_active_ms) >= MP_BLE_LOCK_OFF_MS) {
        ble_auto_disable(now, "lock 2m");
        return;
    }
    if ((uint32_t)(now - mp_app_state.ble_active_ms) >= MP_BLE_IDLE_OFF_MS) {
        ble_auto_disable(now, "idle 10m");
    }
}

static uint8_t lock_deep_sleep_due(uint32_t now)
{
    if (mp_app_state.screen != MP_SCREEN_LOCK || mp_app_state.lock_prompt ||
        mp_app_state.uploading || mp_app_state.debug_mode ||
        mp_settings.ble_enabled || mp_ble_running() || mp_ble_connected()) {
        return 0;
    }
    if (mp_app_state.ble_off_ms != 0) {
        return (uint32_t)(now - mp_app_state.ble_off_ms) >=
               MP_LOCK_SLEEP_AFTER_BLE_MS;
    }
    return (uint32_t)(now - mp_app_state.lock_enter_ms) >=
           MP_LOCK_SLEEP_GRACE_MS;
}

/* 锁屏后无蓝牙时进入超低功耗 deep sleep：完整保存状态与引脚锁存，保留电子纸画面。 */
static void __attribute__((noreturn)) lock_deep_sleep_enter(void)
{
    read_time_flush(now_ms());
    mp_app_flush_progress();

    s_rtc.magic = MP_RTC_MAGIC;
    s_rtc.deep_locked = 1;
    s_rtc.locked_from_read = mp_app_state.locked_from_read;
    s_rtc.book_idx = mp_app_state.book_idx;
    s_rtc.char_pos = mp_app_state.char_pos;

    mp_ble_stop();
    display_sleep();
    if (mp_app_state.has_backlight) {
        mp_backlight_suspend();
    }
    storage_unmount();
    sleep_hold_power_pins();

    /* 避免确认键仍处于按下状态时直接触发 EXT1 立即唤醒。 */
    uint32_t wait = 0;
    while (gpio_get_level(BOARD_KEY_OK_GPIO) == 0 && wait < 2000u) {
        vTaskDelay(pdMS_TO_TICKS(20));
        wait += 20u;
    }
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "enter lock deep sleep");
    enter_ext1_deep_sleep(1ULL << BOARD_KEY_OK_GPIO);
}

void mp_app_tick(uint32_t now)
{
    if (!s_booted) {
        return;
    }

    poll_pending_ok(now);

    if (mp_app_state.screen == MP_SCREEN_LOCK && mp_app_state.lock_prompt &&
        mp_app_state.lock_prompt_until_ms &&
        (int32_t)(now - mp_app_state.lock_prompt_until_ms) >= 0) {
        mp_app_state.lock_prompt = 0;
        mp_app_state.lock_prompt_until_ms = 0;
        page_commit();                /* 解锁提示超时，自动回到纯锁屏图 */
    }

    if (mp_app_state.screen == MP_SCREEN_TIME_DONE &&
        mp_app_state.time_done_until_ms &&
        (int32_t)(now - mp_app_state.time_done_until_ms) >= 0) {
        mp_app_state.time_done_until_ms = 0;
        go_screen(MP_SCREEN_SETTINGS, 0);
    }

    read_time_flush(now);

    if (mp_app_state.debug_mode && (int32_t)(now - mp_app_state.debug_until_ms) >= 0) {
        ESP_LOGI(TAG, "debug timeout");
        debug_exit();
    }

    if (s_conn_lost) {
        s_conn_lost = 0;
        if (mp_app_state.uploading) {
            upload_abort();
        }
        ble_touch(now);
        if (!mp_settings.ble_enabled && mp_ble_running()) {
            mp_ble_stop();
            mp_app_state.ble_off_ms = now ? now : 1u;
        }
    }
    if (s_conn_up) {
        s_conn_up = 0;
        ble_touch(now);
        s_status_due_ms = now + 2000u;
        s_status_pending = 1;
    }
    if (s_status_pending && (int32_t)(now - s_status_due_ms) >= 0) {
        s_status_pending = 0;
        if (mp_ble_connected()) {
            ble_status();              /* 连接后 2 s 主动发一次（§7.6，双保险） */
        }
    }

    take_pending_time(now);
    time_update(now);

    /* 上传期间不采样（power_sample 阻塞 ≥120 ms，会挤掉收包预算，§5.5）。 */
    if (!mp_app_state.uploading) {
        power_tick();
        const power_status_t *p = pw();
        if (p->valid) {
            if (!s_batt_valid || p->percent != s_last_batt) {
                s_last_batt = p->percent;
                s_batt_valid = 1;
                if (mp_app_state.screen == MP_SCREEN_CLOCK) {
                    page_commit();
                }
            }
            if (mp_ble_running()) {
                /* mp_ble 内部按"值变化或 60 s"节流（§7.6）。 */
                mp_ble_publish_battery(p->percent, p->mv);
            }
            if (low_batt_triggered(p)) {
                /* 复采一次防单次 ADC 尖峰误关机。 */
                power_sample();
                if (low_batt_triggered(pw())) {
                    low_batt_shutdown();
                }
            }
        }
        if (mp_ble_running()) {
            mp_ble_publish_temp(25);      /* E2S 无温度传感器；同样 60 s 节流 */
        }
    }

    if (mp_app_state.uploading &&
        (uint32_t)(now - mp_app_state.last_pkt_ms) > 60000u) {
        upload_abort();               /* 60 s 无数据（§7.4） */
    }

    if (mp_app_state.pos_dirty &&
        (uint32_t)(now - mp_app_state.pos_dirty_ms) >= MP_POS_FLUSH_MS) {
        mp_app_flush_progress();      /* §6.4 时机 4：空闲 2 s */
    }

    ble_auto_off_check(now);

    if (auto_lock_due(now)) {
        lock_enter();
    }

    if (lock_deep_sleep_due(now)) {
        lock_deep_sleep_enter();
    }
}

/* ------------------------------------------------------------------ *
 * 启动 / 事件驱动主循环
 * ------------------------------------------------------------------ */

void mp_app_boot(void)
{
    uint8_t ok;

    memset(&mp_app_state, 0, sizeof(mp_app_state));
    memset(s_lock_img, 0, sizeof(s_lock_img));
    memset(&s_ui, 0, sizeof(s_ui));
    s_lock_valid = 0;
    s_done_n = 0;
    s_error_kind = ERR_NONE;
    s_up_kind = UP_NONE;
    s_up_slot = 0xFF;
    s_batt_valid = 0;
    s_last_batt = 0;
    s_stats_dirty = 0;
    s_read_ts_ms = 0;
    s_read_rem_ms = 0;
    s_read_tracking = 0;

    s_app_task = xTaskGetCurrentTaskHandle();
    input_set_wake_cb(app_wake);
    mp_ble_set_wake_cb(app_wake);

#ifdef CONFIG_PM_ENABLE
    if (!s_pm_cpu_lock) {
        (void)esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "up_cpu",
                                 &s_pm_cpu_lock);
    }
    if (!s_pm_nosleep_lock) {
        (void)esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "up_slp",
                                 &s_pm_nosleep_lock);
    }
    s_pm_upload_held = 0;
#endif

    uint32_t boot_now = now_ms();
    mp_app_state.screen = MP_SCREEN_CLOCK;
    mp_app_state.prev_screen = MP_SCREEN_CLOCK;
    mp_app_state.week = MP_TIME_DEFAULT_WEEK;
    mp_app_state.day = 1;
    mp_app_state.month = 1;
    mp_app_state.hour = 0;
    mp_app_state.min = 0;
    mp_app_state.time_base_unix = MP_TIME_DEFAULT_UNIX;
    mp_app_state.time_base_ms = boot_now;
    mp_app_state.set_sel = 0;
    mp_app_state.sel = 0;
    mp_app_state.info_sel = 0;
    mp_app_state.jump_pct = 0;
    mp_app_state.time_step = 0;
    mp_app_state.set_year = MP_TIME_DEFAULT_YEAR;
    mp_app_state.set_month = 1;
    mp_app_state.set_day = 1;
    mp_app_state.set_hour = 0;
    mp_app_state.set_min = 0;
    mp_app_state.time_done_until_ms = 0;
    s_book_chars_k = 0;
    s_year = MP_TIME_DEFAULT_YEAR;
    s_day_index = MP_TIME_DEFAULT_UNIX / 86400u;

    /* 若本周期内曾同步过时间，从跨深睡的 POSIX RTC 恢复最新走时基准。 */
    if (s_rtc.magic == MP_RTC_MAGIC && s_rtc.time_synced) {
        struct timeval tv;
        if (gettimeofday(&tv, NULL) == 0 &&
            tv.tv_sec >= (time_t)MP_TIME_DEFAULT_UNIX &&
            tv.tv_sec < (time_t)4102444800LL) {
            uint32_t ux = (uint32_t)tv.tv_sec;
            uint32_t days = ux / 86400u;
            uint16_t y = MP_TIME_DEFAULT_YEAR;
            uint8_t m = 1;
            uint8_t d = 1;
            civil_from_days(days, &y, &m, &d);
            mp_app_state.time_base_unix = ux;
            mp_app_state.time_base_ms = boot_now;
            mp_app_state.hour = (uint8_t)((ux / 3600u) % 24u);
            mp_app_state.min = (uint8_t)((ux / 60u) % 60u);
            mp_app_state.day = d;
            mp_app_state.month = m;
            mp_app_state.week = (uint8_t)((days + 3u) % 7u + 1u);
            s_year = y;
            s_day_index = days;
        }
    }

    /* 存储：main.c 已按 §10.2 调过 storage_init()/mp_store_init()；这里再确认一次
     * （mp_store_init 幂等），避免"目录没载入"被误判成空书库。 */
    ok = storage_sd_ok() ? 1 : 0;
    if (ok) {
        ok = mp_store_init() == 0 ? 1 : 0;
    }
    mp_app_state.storage_ok = ok;

    if (!ok) {
        /* §6.7：存储坏也必须有一份有效设置（蓝牙开、锁屏 3 分钟、展示阅读数据、关背光、场景全刷、GC 10）。 */
        mp_settings.ble_enabled = 1;
        mp_settings.sleep_timeout_idx = MP_SLEEP_IDX_DEFAULT;
        mp_settings.epd_partial_enabled = 0;
        mp_settings.gc_interval_idx = 0;
        mp_settings.lock_display = MP_LOCK_DISP_STATS;
        mp_settings.backlight_level = MP_BACKLIGHT_DEFAULT;
        mp_stats_defaults();
        ESP_LOGW(TAG, "storage not ready (%s)", storage_last_error());
    }
    if (mp_settings.sleep_timeout_idx >= MP_SLEEP_TIMEOUT_COUNT) {
        mp_settings.sleep_timeout_idx = MP_SLEEP_IDX_DEFAULT;
    }
    if (mp_settings.gc_interval_idx >= MP_GC_INTERVAL_COUNT) {
        mp_settings.gc_interval_idx = 0;
    }
    mp_settings.ble_enabled = mp_settings.ble_enabled ? 1 : 0;
    mp_settings.epd_partial_enabled = mp_settings.epd_partial_enabled ? 1 : 0;
    mp_settings.backlight_level %= MP_BACKLIGHT_LEVEL_COUNT;
    mp_settings.lock_display =
        (mp_settings.lock_display == MP_LOCK_DISP_IMAGE && ok &&
         mp_store_lock_valid())
            ? MP_LOCK_DISP_IMAGE
            : MP_LOCK_DISP_STATS;

    /* 开机必须应用一次，否则用的是 reader_cfg.h 的 16 而不是用户设置（§5.4）。 */
    display_set_gc((uint8_t)mp_gc_interval[mp_settings.gc_interval_idx],
                   READER_GC_IDLE_MS);
    if (mp_ble_running()) {
        mp_ble_adv(mp_settings.ble_enabled);
    }

    mp_ble_set_time_cb(on_ble_time);
    mp_ble_set_conn_cb(on_ble_conn);

    power_sample();                   /* 首帧就要有电量 */
    const power_status_t *p = pw();
    if (low_batt_triggered(p)) {
        low_batt_shutdown();
    }
    if (p->valid) {
        s_last_batt = p->percent;
        s_batt_valid = 1;
        if (mp_ble_running()) {
            mp_ble_publish_battery(p->percent, p->mv);
        }
    }

    (void)mp_backlight_init(mp_settings.backlight_level);
    mp_app_state.has_backlight = mp_backlight_supported() ? 1u : 0u;

    rescan_books();
    if (!ok) {
        s_error_kind = ERR_STORAGE;
        mp_app_state.screen = MP_SCREEN_ERROR;
    } else if (s_woke_unlock) {
        s_woke_unlock = 0;
        if (s_rtc.magic == MP_RTC_MAGIC && s_rtc.locked_from_read &&
            book_open_slot(s_rtc.book_idx) == 0) {
            if (s_rtc.char_pos < mp_app_state.book_len) {
                mp_app_state.char_pos = s_rtc.char_pos;
                if (measure_page(mp_app_state.char_pos) != 0) {
                    mp_app_state.char_pos = 0;
                    (void)measure_page(0);
                }
            }
            mp_app_state.screen = MP_SCREEN_READ;
        }
    }

    uint32_t ready_now = now_ms();
    s_booted = 1;
    mp_app_state.last_input_ms = ready_now;
    ble_touch(ready_now);
    s_stats_last_save_ms = ready_now;
    read_time_flush(ready_now);

    ESP_LOGI(TAG, "boot: books=%u storage=%u backlight=%u",
             (unsigned)s_done_n, (unsigned)ok,
             (unsigned)mp_app_state.has_backlight);

    /* 第一帧：先 AUTO 出帧再补全刷（§10.2）。 */
    mark_dirty();
    scene_commit(1);
}

static void min_wait_until(uint32_t *best, uint32_t now, uint32_t deadline)
{
    int32_t rem = (int32_t)(deadline - now);
    uint32_t w = (rem <= 0) ? 0u : (uint32_t)rem;
    if (w < *best) {
        *best = w;
    }
}

/* 按下一个最近到期事件精确计算阻塞等待时间，无任何周期轮询。 */
static uint32_t app_next_wait_ms(uint32_t now)
{
    uint32_t wait = 60000u;

    if (mp_app_state.pending_ok) {
        min_wait_until(&wait, now,
                       mp_app_state.pending_ok_ms + MP_DCLICK_MS + 1u);
    }
    if (mp_app_state.pos_dirty) {
        min_wait_until(&wait, now,
                       mp_app_state.pos_dirty_ms + MP_POS_FLUSH_MS);
    }
    if (mp_app_state.screen == MP_SCREEN_TIME_DONE &&
        mp_app_state.time_done_until_ms) {
        min_wait_until(&wait, now, mp_app_state.time_done_until_ms);
    }
    if (s_status_pending) {
        min_wait_until(&wait, now, s_status_due_ms);
    }
    if (mp_app_state.debug_mode) {
        min_wait_until(&wait, now, mp_app_state.debug_until_ms);
    }
    if (mp_app_state.screen == MP_SCREEN_LOCK && mp_app_state.lock_prompt &&
        mp_app_state.lock_prompt_until_ms) {
        min_wait_until(&wait, now, mp_app_state.lock_prompt_until_ms);
    }
    if (mp_app_state.uploading && wait > 500u) {
        wait = 500u;
    }
    if (mp_app_state.screen == MP_SCREEN_CLOCK && !mp_app_state.uploading &&
        !mp_app_state.debug_mode) {
        uint32_t elapsed_ms = (uint32_t)(now - mp_app_state.time_base_ms);
        uint32_t sec_mod =
            (mp_app_state.time_base_unix + elapsed_ms / 1000u) % 60u;
        uint32_t sub_ms = elapsed_ms % 1000u;
        uint32_t rem_ms = (60u - sec_mod) * 1000u - sub_ms + 20u;
        if (rem_ms < wait) {
            wait = rem_ms;
        }
    }
    if (!auto_lock_blocked()) {
        uint8_t idx = mp_settings.sleep_timeout_idx;
        if (idx >= MP_SLEEP_TIMEOUT_COUNT) {
            idx = MP_SLEEP_IDX_DEFAULT;
        }
        uint32_t timeout_ms = (uint32_t)mp_sleep_timeout_s[idx] * 1000u;
        min_wait_until(&wait, now, mp_app_state.last_input_ms + timeout_ms);
    }
    if (mp_settings.ble_enabled && !mp_app_state.uploading) {
        min_wait_until(&wait, now,
                       mp_app_state.ble_active_ms + MP_BLE_IDLE_OFF_MS);
        if (mp_app_state.screen == MP_SCREEN_LOCK) {
            uint32_t d_lock = mp_app_state.lock_enter_ms + MP_BLE_LOCK_OFF_MS;
            uint32_t d_ble = mp_app_state.ble_active_ms + MP_BLE_LOCK_OFF_MS;
            uint32_t d = (int32_t)(d_lock - d_ble) >= 0 ? d_lock : d_ble;
            min_wait_until(&wait, now, d);
        }
    }
    if (mp_app_state.screen == MP_SCREEN_LOCK && !mp_app_state.lock_prompt &&
        !mp_app_state.uploading && !mp_app_state.debug_mode &&
        !mp_settings.ble_enabled && !mp_ble_running() && !mp_ble_connected()) {
        if (mp_app_state.ble_off_ms != 0) {
            min_wait_until(&wait, now,
                           mp_app_state.ble_off_ms + MP_LOCK_SLEEP_AFTER_BLE_MS);
        } else {
            min_wait_until(&wait, now,
                           mp_app_state.lock_enter_ms + MP_LOCK_SLEEP_GRACE_MS);
        }
    }

    if (wait > 0u && wait < 10u) {
        wait = 10u;
    }
    return wait;
}

void mp_app_run(void)
{
    s_app_task = xTaskGetCurrentTaskHandle();
    mp_app_state.last_input_ms = now_ms();

    for (;;) {
        mp_ble_req_t r;

        /* 1) 先抽干 BLE 队列（§7.4 的流控：上传期间不重绘）。 */
        while (mp_ble_req_get(&r, 0)) {
            mp_app_ble_req(&r);
        }

        /* 2) 抽干全部待处理按键事件。 */
        key_event_t ev;
        while (input_get(&ev, 0)) {
            mp_app_key(ev.id, ev.type, now_ms());
        }

        /* 3) 统一点验定时事件（到期 OK 单击 / 走时 / 采样 / 自动锁屏 / 关蓝牙 / 深睡）。 */
        uint32_t now = now_ms();
        mp_app_tick(now);

        /* 4) 有脏标志才补一帧（正常路径都由 commit 立即画）。 */
        if (s_dirty && !mp_app_state.uploading && !mp_app_state.debug_mode) {
            if (render() == 0) {
                display_post(s_fb, DISPLAY_MODE_AUTO);
            }
        }

        /* 5) 事件驱动休眠直到下一个定时边界，或被按键/BLE 唤醒通知打断。 */
        uint32_t wait_ms = app_next_wait_ms(now_ms());
        if (wait_ms > 0u) {
            TickType_t ticks = pdMS_TO_TICKS(wait_ms);
            if (ticks == 0) {
                ticks = 1;
            }
            (void)ulTaskNotifyTake(pdTRUE, ticks);
        }
    }
}

uint8_t mp_app_dirty(void)
{
    return s_dirty;
}
