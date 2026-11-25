/* storage_sd.c — SDMMC 卡挂载/卸载、GPIO34 上电时序与全局 FS 互斥量。
 *
 * DESIGN.md §6.6 / §13。引脚与总线宽度抄自盘点 §1/§3.4:
 *   SDMMC 4-bit、slot 1、CLK=11 / CMD=12 / D0=13 / D1=14 / D2=9 / D3=10,
 *   内部上拉;GPIO34 在挂载期间保持低、卸载后置高(电平已由原厂反汇编
 *   0x42009b34/0x42009ce5 确认;仍可用 -D 覆盖,见下面的 ST_SD_PWR_*)。
 *
 * 硬性约束:
 *   - esp_vfs_fat_sdmmc_mount(..., format_if_mount_failed=false),绝不格式化卡;
 *   - 挂载失败只记录日志并返回 ST_ERR_NO_SD,不中止启动(§13);
 *   - 只挂 /sdcard,不写 NVS/分区/裸 flash(§6.7)。
 *
 * 时序:3 次挂载、每次前重新上电、间隔 500 ms。所有 storage API 先经过
 * st_fs_take() 检查 s_sd_ok,卡失效时统一返回 ST_ERR_NO_SD。
 */
#include "storage_internal.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdkconfig.h"
#ifdef CONFIG_PM_ENABLE
#include "esp_pm.h"
#endif

#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "board.h"

static const char *TAG = ST_TAG;

#define ST_SD_MOUNT_POINT "/sdcard"
#define ST_SD_TRY_COUNT 3
#define ST_SD_TRY_GAP_MS 500
#define ST_SD_PWR_OFF_MS 80     /* 重试上电前断电放电时间 */
#define ST_SD_PWR_ON_MS 80      /* 拉低上电后等待稳压与控制器复位时间 */
#define ST_SD_MAX_FILES 8       /* 书 + 进度 + 书签 + .part + opendir 余量 */

/* ---- SD 电源时序(电平与引脚锁存已由原厂反汇编确认)------------------
 * 原厂流程(0x42009b34-0x42009b76 / 0x42009ce5):
 *   挂载: gpio_set_level(34, 0) → gpio_hold_en(34) → 延时 → 挂载
 *   卸载: gpio_hold_dis(34) → gpio_set_level(34, 1)
 * 即 **GPIO34 低 = 卡使能,高 = 断电/复位**；轻睡期间必须 hold 低电平
 * 防止外部上拉导致 SD NAND 热掉电。
 * 电平仍可用 -D 覆盖,便于不同批次的硬件验证。 */
#ifndef ST_SD_PWR_KEEP_LEVEL
#define ST_SD_PWR_KEEP_LEVEL 0
#endif
#ifndef ST_SD_PWR_OFF_LEVEL
#define ST_SD_PWR_OFF_LEVEL 1      /* 卸载后断电,原厂 0x42009ce5 */
#endif

static SemaphoreHandle_t s_lock;      /* 递归互斥:保护 VFS 与包内状态 */
static sdmmc_card_t *s_card;
static bool s_mounted;
static bool s_sd_ok;
static bool s_pwr_ready;
static char s_last_error[64] = "not initialised";
#ifdef CONFIG_PM_ENABLE
static esp_pm_lock_handle_t s_pm_nosleep; /* SD 挂载与读写期间禁止自动轻睡 */
static esp_pm_lock_handle_t s_pm_apb;     /* SD 操作期间锁定 APB 满频保证 SDMMC 时序 */
#endif

/* 懒初始化电源管理锁，确保在首次加锁前幂等就绪。 */
static void sd_pm_init(void)
{
#ifdef CONFIG_PM_ENABLE
    if (!s_pm_nosleep) {
        (void)esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "sd_noslp", &s_pm_nosleep);
    }
    if (!s_pm_apb) {
        (void)esp_pm_lock_create(ESP_PM_APB_FREQ_MAX, 0, "sd_apb", &s_pm_apb);
    }
#endif
}

/* 获取 SD 操作所需的防轻睡与满频锁，允许递归配对调用。 */
static void sd_pm_acquire(void)
{
#ifdef CONFIG_PM_ENABLE
    sd_pm_init();
    if (s_pm_apb) {
        (void)esp_pm_lock_acquire(s_pm_apb);
    }
    if (s_pm_nosleep) {
        (void)esp_pm_lock_acquire(s_pm_nosleep);
    }
#endif
}

/* 成对释放防轻睡与满频锁。 */
static void sd_pm_release(void)
{
#ifdef CONFIG_PM_ENABLE
    if (s_pm_nosleep) {
        (void)esp_pm_lock_release(s_pm_nosleep);
    }
    if (s_pm_apb) {
        (void)esp_pm_lock_release(s_pm_apb);
    }
#endif
}

/* ------------------------------------------------------------------ */
/* 错误记录与 SD 状态                                                 */
/* ------------------------------------------------------------------ */

void st_sd_set_error(const char *msg)
{
    if (!msg) {
        msg = "";
    }
    strncpy(s_last_error, msg, sizeof(s_last_error) - 1);
    s_last_error[sizeof(s_last_error) - 1] = '\0';
}

void st_sd_fail(const char *why)
{
    s_sd_ok = false;
    st_sd_set_error(why ? why : "sd i/o error");
    ESP_LOGE(TAG, "SD failure: %s", s_last_error);
}

bool st_sd_is_ok(void)
{
    return s_sd_ok && s_mounted;
}

/* ------------------------------------------------------------------ */
/* 互斥量                                                             */
/* ------------------------------------------------------------------ */

int st_lock(uint32_t timeout_ms)
{
    if (!s_lock) {
        return ST_ERR_NO_SD;        /* storage_init 之前没有任何句柄 */
    }
    TickType_t ticks = (timeout_ms == ST_WAIT_FOREVER) ? portMAX_DELAY
                                                       : pdMS_TO_TICKS(timeout_ms);
    if (xSemaphoreTakeRecursive(s_lock, ticks) != pdTRUE) {
        return ST_ERR_BUSY;
    }
    /* 与递归互斥量严格配对，保护整个文件/重挂持有期免于轻睡与降频 */
    sd_pm_acquire();
    return ST_OK;
}

void st_unlock(void)
{
    if (s_lock) {
        sd_pm_release();
        (void)xSemaphoreGiveRecursive(s_lock);
    }
}

int st_fs_take(uint32_t timeout_ms)
{
    if (!s_sd_ok || !s_mounted) {
        return ST_ERR_NO_SD;
    }
    int rc = st_lock(timeout_ms);
    if (rc != ST_OK) {
        return rc;
    }
    /* 取锁期间卡可能被别的任务标记失效,复查一次 */
    if (!s_sd_ok || !s_mounted) {
        st_unlock();
        return ST_ERR_NO_SD;
    }
    return ST_OK;
}

/* ------------------------------------------------------------------ */
/* 卡电源管理(先解除锁存再下电复位，上电拉低后立刻锁存以扛过轻睡)     */
/* ------------------------------------------------------------------ */

/* 先解除 GPIO34 锁存，再置高电平断开外设电源。 */
static void sd_power_off(void)
{
    (void)gpio_hold_dis(BOARD_SD_PWR_GPIO);
    if (!s_pwr_ready) {
        gpio_config_t io = {
            .pin_bit_mask = (1ULL << BOARD_SD_PWR_GPIO),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        esp_err_t err = gpio_config(&io);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "GPIO%d config failed: %s", BOARD_SD_PWR_GPIO,
                     esp_err_to_name(err));
        } else {
            s_pwr_ready = true;
        }
    }
    if (s_pwr_ready) {
        (void)gpio_set_level(BOARD_SD_PWR_GPIO, ST_SD_PWR_OFF_LEVEL);
    }
}

/* 先彻底下电放电复位，再拉低上电并立即锁存 GPIO34 防止轻睡期间掉电。 */
static void sd_power_cycle_on(void)
{
    sd_power_off();
    vTaskDelay(pdMS_TO_TICKS(ST_SD_PWR_OFF_MS));

    if (s_pwr_ready) {
        (void)gpio_set_level(BOARD_SD_PWR_GPIO, ST_SD_PWR_KEEP_LEVEL);
        (void)gpio_hold_en(BOARD_SD_PWR_GPIO);
    }
    vTaskDelay(pdMS_TO_TICKS(ST_SD_PWR_ON_MS));
}

/* ------------------------------------------------------------------ */
/* 挂载 + 目录准备                                                    */
/* ------------------------------------------------------------------ */

static esp_err_t sd_mount_once(void)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_1;         /* 盘点 §3.4:slot 1 */
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk = (gpio_num_t)BOARD_SD_PIN_CLK;
    slot.cmd = (gpio_num_t)BOARD_SD_PIN_CMD;
    slot.d0 = (gpio_num_t)BOARD_SD_PIN_D0;
    slot.d1 = (gpio_num_t)BOARD_SD_PIN_D1;
    slot.d2 = (gpio_num_t)BOARD_SD_PIN_D2;
    slot.d3 = (gpio_num_t)BOARD_SD_PIN_D3;
    /* 4-bit 模式下 D4..D7 被驱动忽略;显式置 NC,避免默认值落在别的功能脚上
     * (S3 默认 d5=GPIO34,正是卡电源脚) */
    slot.d4 = GPIO_NUM_NC;
    slot.d5 = GPIO_NUM_NC;
    slot.d6 = GPIO_NUM_NC;
    slot.d7 = GPIO_NUM_NC;
    slot.width = BOARD_SD_WIDTH;
    slot.flags = SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mcfg = {
        .format_if_mount_failed = false,   /* 绝不格式化卡(§13) */
        .max_files = ST_SD_MAX_FILES,
        .allocation_unit_size = 0,
        .disk_status_check_enable = false,
        .use_one_fat = false,
    };

    s_card = NULL;
    esp_err_t err = esp_vfs_fat_sdmmc_mount(ST_SD_MOUNT_POINT, &host, (const void *)&slot,
                                            &mcfg, &s_card);
    if (err != ESP_OK) {
        s_card = NULL;
        s_mounted = false;
        return err;
    }
    s_mounted = true;
    return ESP_OK;
}

static void sd_ensure_dirs(void)
{
    static const char *dirs[] = {
        READER_DIR_BOOKS,
        "/sdcard/reader",
        READER_DIR_STATE,
        READER_DIR_TMP,
    };
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        if (mkdir(dirs[i], 0777) != 0 && errno != EEXIST) {
            /* FatFs 的 mkdir 失败不致命:书目/进度 API 会各自报错并降级 */
            ESP_LOGW(TAG, "mkdir %s failed (errno=%d)", dirs[i], errno);
        }
    }
}

/* ------------------------------------------------------------------ */
/* 公共 API                                                           */
/* ------------------------------------------------------------------ */

int storage_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateRecursiveMutex();
        if (!s_lock) {
            st_sd_set_error("mutex alloc failed");
            ESP_LOGE(TAG, "cannot create storage mutex");
            return ST_ERR_NO_SD;
        }
    }
    if (s_mounted) {
        /* 重复调用:已挂载就返回;若上次扫描失败导致 s_sd_ok=false,
         * 走一次重挂把挂载点与状态对齐 */
        return s_sd_ok ? ST_OK : storage_remount();
    }

    s_sd_ok = false;
    s_mounted = false;
    s_card = NULL;

    sd_pm_acquire();

    esp_err_t last = ESP_FAIL;
    for (int i = 0; i < ST_SD_TRY_COUNT; i++) {
        sd_power_cycle_on();
        last = sd_mount_once();
        if (last == ESP_OK) {
            s_sd_ok = true;
            st_sd_set_error("ok");
            ESP_LOGI(TAG, "SD mounted: %d-bit, slot %d", BOARD_SD_WIDTH, SDMMC_HOST_SLOT_1);
            sd_ensure_dirs();
            int rc = storage_rescan();   /* 先建书目缓存,便于 BOOT 直接 storage_list */
            if (rc != ST_OK || !st_sd_is_ok()) {
                st_sd_fail("initial book scan failed");
                sd_pm_release();
                return ST_ERR_NO_SD;
            }
            sd_pm_release();
            return ST_OK;
        }
        ESP_LOGW(TAG, "SD mount attempt %d/%d failed: %s", i + 1, ST_SD_TRY_COUNT,
                 esp_err_to_name(last));
        /* 失败后立刻断电保持到下轮冷上电或退出，避免带电悬空 */
        sd_power_off();
        if (i + 1 < ST_SD_TRY_COUNT) {
            vTaskDelay(pdMS_TO_TICKS(ST_SD_TRY_GAP_MS));
        }
    }

    s_sd_ok = false;
    s_mounted = false;
    sd_power_off();
    sd_pm_release();
    /* 保留最后一次的真实挂载错误(ESP_ERR_TIMEOUT / ESP_FAIL ...),
     * 显示在 ALERT/INFO 上,便于区分“没插卡”和“供电/引脚/文件系统”问题。 */
    char why[64];
    snprintf(why, sizeof(why), "mount %s", esp_err_to_name(last));
    st_sd_set_error(why);
    ESP_LOGE(TAG, "no SD card after %d attempts: %s (continuing, DESIGN §13)",
             ST_SD_TRY_COUNT, why);
    return ST_ERR_NO_SD;
}

bool storage_sd_ok(void)
{
    return s_sd_ok && s_mounted;
}

static void unmount_locked(void)
{
    (void)storage_close_book();  /* 关当前书，避免悬空 FILE* */
    st_up_force_close();         /* 上传句柄也不能跨卸载存活 */

    if (s_mounted && s_card) {
        esp_err_t err = esp_vfs_fat_sdcard_unmount(ST_SD_MOUNT_POINT, s_card);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "unmount failed: %s", esp_err_to_name(err));
        }
    }
    s_card = NULL;               /* unmount 内部已释放 card */
    s_mounted = false;
    s_sd_ok = false;
    sd_power_off();              /* 原厂顺序:卸载完成后 GPIO34 置高 */
}

int storage_remount(void)
{
    if (!s_lock) {
        return ST_ERR_NO_SD;
    }
    /* 整个重挂过程独占 FS:期间的读/写会等到超时返回 ST_ERR_BUSY,而不是
     * 撞上正在卸载的挂载点(§6.6)。 */
    if (st_lock(ST_WAIT_FOREVER) != ST_OK) {
        return ST_ERR_NO_SD;
    }

    unmount_locked();

    esp_err_t last = ESP_FAIL;
    for (int i = 0; i < ST_SD_TRY_COUNT; i++) {
        sd_power_cycle_on();
        esp_err_t err = sd_mount_once();
        last = err;
        if (err == ESP_OK) {
            s_sd_ok = true;
            st_sd_set_error("ok");
            ESP_LOGI(TAG, "SD remounted");
            sd_ensure_dirs();
            int rc = storage_rescan();
            if (rc != ST_OK || !st_sd_is_ok()) {
                st_sd_fail("rescan after remount failed");
                st_unlock();
                return ST_ERR_NO_SD;
            }
            st_unlock();
            return ST_OK;
        }
        ESP_LOGW(TAG, "remount attempt %d/%d failed: %s", i + 1, ST_SD_TRY_COUNT,
                 esp_err_to_name(err));
        sd_power_off();
        if (i + 1 < ST_SD_TRY_COUNT) {
            vTaskDelay(pdMS_TO_TICKS(ST_SD_TRY_GAP_MS));
        }
    }

    sd_power_off();
    char why[64];
    snprintf(why, sizeof(why), "remount %s", esp_err_to_name(last));
    st_sd_set_error(why);
    st_unlock();
    return ST_ERR_NO_SD;
}

void storage_unmount(void)
{
    if (!s_lock) {
        sd_power_off();
        return;
    }
    if (st_lock(ST_LOCK_MS) != ST_OK) {
        sd_power_off();
        return;
    }
    unmount_locked();
    ESP_LOGI(TAG, "SD unmounted for sleep");
    st_unlock();
}

int storage_rescan(void)
{
    int rc = st_fs_take(ST_LOCK_MS);
    if (rc != ST_OK) {
        return rc;
    }
    rc = st_lib_scan();
    st_unlock();
    return rc;
}

const char *storage_last_error(void)
{
    return s_last_error;
}
