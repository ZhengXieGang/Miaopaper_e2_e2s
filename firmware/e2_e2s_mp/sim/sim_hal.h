#pragma once
/* sim_hal.h — 本机全链路固件回归模拟器虚拟硬件层（HAL）控制与观测接口。 */
#include <setjmp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_app_desc.h"
#include "esp_partition.h"
#include "mp_ble.h"
#include "mp_ui.h"

/* 虚拟 8 MiB Flash 总容量与关键安全区域边界。 */
#define SIM_FLASH_SIZE 0x800000u
#define SIM_FLASH_SECTOR 0x1000u
#define SIM_PART_NVS_ADDR 0x9000u
#define SIM_PART_OTADATA_ADDR 0xF000u
#define SIM_PART_PHY_ADDR 0x11000u
#define SIM_PART_OTA0_ADDR 0x20000u
#define SIM_PART_OTA1_ADDR 0x210000u
#define SIM_PART_OTA_SIZE 0x1F0000u
#define SIM_STOCK_FONT_ADDR 0x480000u
#define SIM_STOCK_FONT_SIZE 0x0A8884u

/* 拦截 noreturn 系统退出路径的返回码。 */
enum {
    SIM_TRAP_NONE = 0,
    SIM_TRAP_DEEP_SLEEP = 1,
    SIM_TRAP_RESTART = 2,
    SIM_TRAP_RESTART_NOOS = 3,
};

/* 单条 BLE Notify 录制包。 */
typedef struct {
    uint16_t len;
    uint8_t data[MP_BLE_PKT_MAX];
} sim_ble_pkt_t;

/* Flash 允许写入白名单区间 [start, end)。 */
typedef struct {
    uint32_t start;
    uint32_t end;
} sim_flash_range_t;

/* IRAM 原子自刷写硬件动作观测快照。 */
typedef struct {
    bool intr_disabled;
    bool wdt_disabled[3];
    uint32_t cpu_reset_mask;
    uint32_t cpu_stalled_mask;
    bool icache_disabled;
    bool dcache_disabled;
    bool rom_flash_unlocked;
    uint32_t rom_erase_count;
    uint32_t rom_write_count;
} sim_iram_audit_t;

/* 深睡配置观测快照。 */
typedef struct {
    bool deep_sleep_entered;
    uint64_t ext1_mask;
    bool rtc_periph_kept;
    bool rtc_slow_mem_kept;
    bool xtal32k_kept;
    bool deep_sleep_hold;
    bool vbat_en_held;
    bool sd_pwr_held;
    bool bl_deep_slept;
    int vbat_en_level;
    int sd_pwr_level;
    uint64_t rtc_pad_held_mask;
    uint64_t floated_pins_mask;
} sim_sleep_audit_t;

/* 初始化/重置整个虚拟硬件环境（新建 8 MiB Flash、默认分区表与默认传感器）。 */
void sim_hal_init(void);
void sim_hal_shutdown(void);

/* 日志开关（默认静默，出错或设置 MP_SIM_VERBOSE=1 时打印）。 */
void sim_hal_set_verbose(bool on);

/* ---- 虚拟时钟与 RTC ------------------------------------------------- */
uint64_t sim_hal_now_ms(void);
void sim_hal_set_now_ms(uint64_t ms);
void sim_hal_advance_ms(uint64_t delta_ms);
/* 模拟深睡停机经过的时间：只推进 RTC 墙上时钟，单调开机时钟归零。 */
void sim_hal_sleep_advance_ms(uint64_t delta_ms);
uint64_t sim_hal_rtc_unix_ms(void);
void sim_hal_set_rtc_unix_ms(uint64_t unix_ms);

/* 跨重启持久化/恢复 RTC_DATA_ATTR 段（mp_sim_rtc）及墙上时间。 */
int sim_hal_rtc_save(const char *path);
int sim_hal_rtc_load(const char *path);
/* 模拟彻底断电 POR：清空 RTC_DATA_ATTR 段并把硬件墙钟归零。 */
void sim_hal_clear_rtc_mem(void);

/* ---- 系统重启/深睡拦截（setjmp/longjmp） ----------------------------- */
jmp_buf *sim_hal_trap_buf(void);
void sim_hal_arm_trap(bool armed);
int sim_hal_last_trap(void);

/* ---- GPIO / 唤醒原因 ------------------------------------------------ */
void sim_hal_set_wakeup(uint32_t causes, uint64_t ext1_status);
void sim_hal_set_gpio(int gpio, int level);
/* 模拟唤醒后 OK 键持续按住 hold_ms 毫秒后松开（GPIO8 低电平持续 hold_ms）。 */
void sim_hal_set_ok_hold_ms(uint32_t hold_ms);
/* 强制注入硬件 RTC_CNTL_PAD_HOLD 锁存掩码，用于回归检验唤醒前必须 rtc_gpio_hold_dis。 */
void sim_hal_force_rtc_pad_hold(uint64_t mask);
uint64_t sim_hal_rtc_pad_held_mask(void);
const sim_sleep_audit_t *sim_hal_sleep_audit(void);

/* ---- 电源与充电 ----------------------------------------------------- */
void sim_hal_set_battery(uint8_t pct, uint16_t mv, bool valid, bool charging);
void sim_hal_set_charging(bool charging, bool fire_irq);

/* ---- 背光（E2S/E2） ------------------------------------------------- */
void sim_hal_set_backlight_hw(bool supported);
bool sim_hal_backlight_is_suspended(void);
uint8_t sim_hal_backlight_hw_level(void);
uint32_t sim_hal_backlight_set_calls(void);
uint32_t sim_hal_backlight_suspend_calls(void);

/* ---- 显示屏 --------------------------------------------------------- */
void sim_hal_set_display_ready(bool ready);
const uint8_t *sim_hal_last_fb(void);
uint32_t sim_hal_display_frames(void);
uint32_t sim_hal_display_fulls(void);
uint32_t sim_hal_display_partials(void);
bool sim_hal_display_asleep(void);
uint32_t sim_hal_display_padding_violations(void);
void sim_hal_display_get_gc(uint8_t *partials, uint16_t *idle_ms);

/* ---- 存储（SD/FAT） ------------------------------------------------- */
void sim_hal_set_sd_ok(bool ok);
void sim_hal_set_fat_space(uint64_t total_bytes, uint64_t free_bytes);

/* ---- BLE 虚拟传输层 ------------------------------------------------- */
void sim_hal_ble_connect(bool connected, uint16_t mtu);
void sim_hal_ble_inject_time(const mp_time_t *t);
void sim_hal_ble_clear_tx(void);
size_t sim_hal_ble_tx_count(void);
const sim_ble_pkt_t *sim_hal_ble_tx_at(size_t idx);
bool sim_hal_ble_adv_on(void);

/* ---- Flash / 分区 / 镜像 / 安全红线 --------------------------------- */
uint8_t *sim_hal_flash_raw(void);
void sim_hal_flash_set_running_slot(unsigned slot); /* 0=ota_0, 1=ota_1 */
void sim_hal_flash_set_app_desc(const char *project, const char *version,
                                const char *date, const char *time_str);
void sim_hal_flash_set_otadata(unsigned sector, uint32_t seq, uint32_t state);
void sim_hal_flash_set_security(bool secure_boot, bool flash_enc,
                                uint16_t secure_version, bool dbgr);
/* 开启写保护红线：只放行 ranges[0..count-1]，向其他任意地址擦写即记为违规。 */
void sim_hal_flash_set_write_guard(const sim_flash_range_t *ranges, size_t count);
void sim_hal_flash_clear_write_guard(void);
uint32_t sim_hal_flash_guard_violations(void);
uint32_t sim_hal_flash_last_violation_addr(void);
const sim_iram_audit_t *sim_hal_iram_audit(void);

/* 工具函数：计算标准 SHA-256 与生成合法 ESP32-S3 测试镜像。 */
void sim_sha256(const void *data, size_t len, uint8_t out[32]);
size_t sim_build_esp_image(uint8_t *out, size_t cap,
                           uint16_t chip_id,
                           const char *project_name,
                           const char *version,
                           size_t payload_bytes,
                           bool append_sha256);
