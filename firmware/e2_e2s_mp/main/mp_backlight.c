/* mp_backlight.c — AWINIC AW2023 背光驱动与策略（IDF v6 i2c_master）。 */
#include "mp_backlight.h"

#include <stdbool.h>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "sdkconfig.h"
#ifdef CONFIG_PM_ENABLE
#include "esp_pm.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "mp_bl";

#define AW2023_ADDR7 0x45
#define AW2023_PIN_SDA 35
#define AW2023_PIN_SCL 18
#define AW2023_CLK_HZ 400000

#define AW2023_REG_RSTR 0x00
#define AW2023_REG_GCR1 0x01
#define AW2023_REG_GCR2 0x04
#define AW2023_REG_LCTR 0x30
#define AW2023_REG_LCFG0 0x31
#define AW2023_REG_PWM0 0x34

#define AW2023_RESET_MAGIC 0x55
#define AW2023_CHIP_ID 0x09
#define AW2023_STANDBY_MODE 0
#define AW2023_ACTIVE_MODE 1
#define AW2023_OUTPUT_CURRENT_5MA 2

/* 官方四档 PWM 查表（DROM 0x3C1A0184）。 */
static const uint8_t k_pwm_level[MP_BL_LEVEL_COUNT] = {0, 5, 20, 35};

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static uint8_t s_level;
static bool s_probed;
static bool s_supported;
#ifdef CONFIG_PM_ENABLE
static esp_pm_lock_handle_t s_bl_on_lock; /* 背光常亮期间禁止自动轻睡 */
static esp_pm_lock_handle_t s_bl_io_lock; /* 背光 I2C 通信期间禁止自动轻睡 */
static bool s_bl_on_held;
#endif

/* 懒初始化背光电源管理锁，确保开启亮灯与 I2C 访问期间不被自动轻睡打断。 */
static void bl_pm_init(void)
{
#ifdef CONFIG_PM_ENABLE
    if (!s_bl_on_lock) {
        (void)esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "bl_on",
                                 &s_bl_on_lock);
    }
    if (!s_bl_io_lock) {
        (void)esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "bl_i2c",
                                 &s_bl_io_lock);
    }
#endif
}

/* 获取背光 I2C 事务锁，防止读改写及软复位轮询中途进轻睡导致失能。 */
static void bl_io_acquire(void)
{
#ifdef CONFIG_PM_ENABLE
    bl_pm_init();
    if (s_bl_io_lock) {
        (void)esp_pm_lock_acquire(s_bl_io_lock);
    }
#endif
}

/* 释放背光 I2C 事务锁。 */
static void bl_io_release(void)
{
#ifdef CONFIG_PM_ENABLE
    if (s_bl_io_lock) {
        (void)esp_pm_lock_release(s_bl_io_lock);
    }
#endif
}

/* 按背光点亮状态获取或释放常亮锁，关灯/待机时放锁恢复轻睡省电。 */
static void bl_hold_awake(bool hold)
{
#ifdef CONFIG_PM_ENABLE
    bl_pm_init();
    if (!s_bl_on_lock) {
        return;
    }
    if (hold && !s_bl_on_held) {
        if (esp_pm_lock_acquire(s_bl_on_lock) == ESP_OK) {
            s_bl_on_held = true;
        }
    } else if (!hold && s_bl_on_held) {
        (void)esp_pm_lock_release(s_bl_on_lock);
        s_bl_on_held = false;
    }
#else
    (void)hold;
#endif
}

static esp_err_t bus_open(void)
{
    if (s_bus) {
        return ESP_OK;
    }
    i2c_master_bus_config_t cfg = {
        .i2c_port = -1,
        .sda_io_num = AW2023_PIN_SDA,
        .scl_io_num = AW2023_PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&cfg, &s_bus);
}

static esp_err_t dev_open(void)
{
    if (s_dev) {
        return ESP_OK;
    }
    esp_err_t err = bus_open();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "i2c bus open err=%s", esp_err_to_name(err));
        return err;
    }
    i2c_device_config_t dcfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AW2023_ADDR7,
        .scl_speed_hz = AW2023_CLK_HZ,
    };
    err = i2c_master_bus_add_device(s_bus, &dcfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "aw2023 add dev err=%s", esp_err_to_name(err));
        s_dev = NULL;
    }
    return err;
}

static void dev_close(void)
{
    if (s_dev) {
        (void)i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
    }
}

static void bus_close(void)
{
    dev_close();
    if (s_bus) {
        (void)i2c_del_master_bus(s_bus);
        s_bus = NULL;
    }
}

static esp_err_t reg_write(uint8_t reg, uint8_t val)
{
    if (!s_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t buf[2] = {reg, val};
    esp_err_t err = i2c_master_transmit(s_dev, buf, sizeof(buf), 100);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "wr 0x%02x=0x%02x err=%s", reg, val, esp_err_to_name(err));
    }
    return err;
}

static esp_err_t reg_read(uint8_t reg, uint8_t *out)
{
    if (!s_dev || !out) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = i2c_master_transmit_receive(s_dev, &reg, 1, out, 1, 100);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "rd 0x%02x err=%s", reg, esp_err_to_name(err));
    }
    return err;
}

static uint8_t clamp_level(uint8_t level)
{
    return level > MP_BL_LEVEL_MAX ? MP_BL_LEVEL_MAX : level;
}

static uint8_t chip_id_get(void)
{
    uint8_t id = 0;
    if (reg_read(AW2023_REG_RSTR, &id) != ESP_OK) {
        return 0;
    }
    return id;
}

static uint8_t work_mode_get(void)
{
    uint8_t v = 0;
    if (reg_read(AW2023_REG_GCR1, &v) != ESP_OK) {
        return 0;
    }
    return (uint8_t)(v & 0x01u);
}

static esp_err_t work_mode_set(uint8_t mode)
{
    uint8_t v = 0;
    esp_err_t err = reg_read(AW2023_REG_GCR1, &v);
    if (err != ESP_OK) {
        v = 0;
    }
    v = (mode == AW2023_ACTIVE_MODE) ? (uint8_t)(v | 0x01u)
                                     : (uint8_t)(v & ~0x01u);
    return reg_write(AW2023_REG_GCR1, v);
}

static esp_err_t led0_turn_onoff(uint8_t on)
{
    uint8_t v = 0;
    esp_err_t err = reg_read(AW2023_REG_LCTR, &v);
    if (err != ESP_OK) {
        v = 0;
    }
    v = on ? (uint8_t)(v | 0x01u) : (uint8_t)(v & ~0x01u);
    return reg_write(AW2023_REG_LCTR, v);
}

/* 清零高 4 位呼吸/淡出控制位并拉满低 4 位恒流，确保恒为手动常亮 PWM 模式。 */
static esp_err_t led0_current_set(uint8_t imax)
{
    (void)imax;
    return reg_write(AW2023_REG_LCFG0, 0x0Fu);
}

static esp_err_t global_current_set(uint8_t sel)
{
    uint8_t v = 0;
    if (reg_read(AW2023_REG_GCR2, &v) != ESP_OK) {
        v = 0;
    }
    v = (uint8_t)(((v & ~0x03u) | (sel & 0x03u)) | 0x03u);
    return reg_write(AW2023_REG_GCR2, v);
}

static bool soft_reset_ready(void)
{
    if (reg_write(AW2023_REG_RSTR, AW2023_RESET_MAGIC) != ESP_OK) {
        ESP_LOGW(TAG, "aw2023 reset write failed");
    }
    for (int i = 0; i < 4; i++) {
        vTaskDelay(pdMS_TO_TICKS(5));
        if (chip_id_get() == AW2023_CHIP_ID) {
            return true;
        }
    }
    ESP_LOGW(TAG, "aw2023 chip id probe failed");
    return false;
}

/* 开机只读探测 0x45，确认 AW2023 在位后再核对芯片 ID，无芯片时释放总线。 */
static bool probe_chip(void)
{
    if (bus_open() != ESP_OK) {
        ESP_LOGW(TAG, "i2c bus open failed; backlight disabled");
        return false;
    }

    esp_err_t err = ESP_ERR_NOT_FOUND;
    for (int i = 0; i < 2; i++) {
        err = i2c_master_probe(s_bus, AW2023_ADDR7, 50);
        if (err == ESP_OK) {
            break;
        }
        if (i == 0) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "aw2023 probe 0x%02x miss (%s); backlight disabled (E2 mode)",
                 AW2023_ADDR7, esp_err_to_name(err));
        bus_close();
        return false;
    }

    if (dev_open() != ESP_OK) {
        bus_close();
        return false;
    }
    uint8_t id = chip_id_get();
    bool id_ok = (id == AW2023_CHIP_ID) || soft_reset_ready();
    dev_close();
    if (!id_ok) {
        ESP_LOGW(TAG, "aw2023 id mismatch (0x%02x); backlight disabled",
                 (unsigned)id);
        bus_close();
        return false;
    }

    ESP_LOGI(TAG, "aw2023 detected (id=0x%02x); backlight enabled (E2S mode)",
             AW2023_CHIP_ID);
    return true;
}

int mp_backlight_set(uint8_t level)
{
    if (!s_supported) {
        s_level = 0;
        bl_hold_awake(false);
        return -1;
    }

    uint8_t lv = clamp_level(level);
    s_level = lv;

    bl_io_acquire();
    if (dev_open() != ESP_OK) {
        dev_close();
        bl_io_release();
        if (lv == 0) {
            bl_hold_awake(false);
        }
        return -1;
    }

    esp_err_t err = ESP_OK;
    if (lv == 0) {
        /* 关灯不能只写 PWM=0，必须关闭 LED0 使能、清 PWM 并切入 STANDBY。 */
        err |= led0_turn_onoff(0);
        err |= reg_write(AW2023_REG_PWM0, 0);
        err |= led0_current_set(0);
        err |= global_current_set(AW2023_OUTPUT_CURRENT_5MA);
        esp_err_t st = work_mode_set(AW2023_STANDBY_MODE);
        dev_close();
        bl_io_release();
        /* 无论 I2C 是否异常都释放常亮锁，允许进入自动轻睡省电 */
        bl_hold_awake(false);
        if (st == ESP_OK && err == ESP_OK) {
            ESP_LOGI(TAG, "backlight off (standby)");
            return 0;
        }
        ESP_LOGW(TAG, "backlight off err=%d/%d", (int)err, (int)st);
        return -1;
    }

    if (work_mode_get() != AW2023_ACTIVE_MODE) {
        if (!soft_reset_ready()) {
            dev_close();
            bl_io_release();
            return -1;
        }
        err |= work_mode_set(AW2023_ACTIVE_MODE);
    }
    err |= global_current_set(AW2023_OUTPUT_CURRENT_5MA);
    err |= led0_current_set(15);
    err |= reg_write(AW2023_REG_PWM0, k_pwm_level[lv]);
    err |= led0_turn_onoff(1);
    dev_close();
    bl_io_release();

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "backlight level=%u err=%d", (unsigned)lv, (int)err);
        return -1;
    }
    /* 点亮成功后持续禁止自动轻睡，保证背光供电与 PWM 常亮稳定 */
    bl_hold_awake(true);
    ESP_LOGI(TAG, "backlight level=%u pwm=%u", (unsigned)lv,
             (unsigned)k_pwm_level[lv]);
    return 0;
}

int mp_backlight_init(uint8_t level)
{
    if (!s_probed) {
        bl_io_acquire();
        s_supported = probe_chip();
        bl_io_release();
        s_probed = true;
    }
    if (!s_supported) {
        s_level = 0;
        return -1;
    }
    return mp_backlight_set(level);
}

bool mp_backlight_supported(void)
{
    return s_supported;
}

void mp_backlight_suspend(void)
{
    if (!s_probed) {
        bl_io_acquire();
        s_supported = probe_chip();
        bl_io_release();
        s_probed = true;
    }
    if (!s_supported) {
        bl_hold_awake(false);
        return;
    }
    bl_io_acquire();
    if (dev_open() == ESP_OK) {
        (void)led0_turn_onoff(0);
        (void)reg_write(AW2023_REG_PWM0, 0);
        (void)work_mode_set(AW2023_STANDBY_MODE);
    }
    dev_close();
    bl_io_release();
    /* 锁屏前无条件释放常亮锁，恢复完整轻睡/深睡功耗控制 */
    bl_hold_awake(false);
}

void mp_backlight_deep_sleep(void)
{
    mp_backlight_suspend();
    bl_io_acquire();
    bus_close();
    bl_io_release();
}

uint8_t mp_backlight_level(void)
{
    return s_level;
}
