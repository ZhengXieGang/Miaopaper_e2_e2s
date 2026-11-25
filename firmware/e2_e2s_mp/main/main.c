/* main.c — e2s_mp boot wiring (PORT_SPEC §10.2).
 *
 * This file only initialises the platform and hands control to mp_app.c;
 * business logic, rendering and every post-boot mp_store_* call live there
 * (PORT_SPEC §10.1). Each init failure is logged and startup continues: a
 * missing panel, SD card or BLE stack must never take the keys, the storage
 * error page or the recovery entry down with it (P §11#8/#9).
 *
 * Hard rules honoured here:
 *   - no NVS / partition table / otadata / flash write happens in this file;
 *     the only writable tree is /sdcard/mp, and only mp_store writes it;
 *   - no device identity is logged: no MAC, SN, efuse or NVS contents
 *     (PORT_SPEC §0.2, docs/research/platform.md §11#5).
 */
#include <stdbool.h>
#include <stdint.h>

#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_system.h"
#include "sdkconfig.h"
#ifdef CONFIG_PM_ENABLE
#include "esp_pm.h"
#endif

#include "reader_cfg.h"

#include "display.h"
#include "input.h"
#include "power.h"
#include "storage.h"

#include "mp_app.h"
#include "mp_ble.h"
#include "mp_store.h"

static const char *TAG = "main";

/* PORT_SPEC §10.2 spells the last step "mp_app_boot(); mp_app_run()";
 * mp_app.h exposes exactly those two (there is no mp_app_start symbol), so
 * the start step is this wrapper rather than a new public API. */
static void mp_app_start(void)
{
    mp_app_boot();  /* catalog view, first frame (scene_commit), display_set_gc() */
    mp_app_run();   /* never returns; BLE drain + keys + 1 Hz tick live inside */
}

void app_main(void)
{
    /* 外设初始化前优先判断深睡唤醒长按防误触，短按误触立刻原样继续深睡。 */
    mp_app_early_wake_check();

    /* Version / build only — never device identity (PORT_SPEC §0.2). */
    ESP_LOGI(TAG, "miaopaper_e2_e2s %d.%d.%d  build %s %s  IDF %s  target %s",
             READER_VERSION_MAJOR, READER_VERSION_MINOR, READER_VERSION_PATCH,
             __DATE__, __TIME__, esp_get_idf_version(), CONFIG_IDF_TARGET);

    /* PORT_SPEC §6.7: unconditional, and before mp_store_init()/mp_ble_start().
     * mp_settings is a zero-initialised global; when the SD fails to mount,
     * mp_store_init() never loads it, and a zeroed ble_enabled would silently
     * disable BLE exactly when the user needs the web tool to recover. */
    mp_settings_defaults();
    mp_stats_defaults();

    /* 1) power — the first ADC sample already happens inside power_init()
     * (power_battery.c:211-213), so it must not be sampled again here. */
    int rc = power_init();
    if (rc != 0) {
        ESP_LOGE(TAG, "power_init: %d (continuing; battery reading invalid)", rc);
    }

    /* 2) storage — a failure is not fatal (P §11#8): no "please insert card",
     * the app shows the storage error page and keeps keys/BLE/recovery alive. */
    rc = storage_init();
    uint8_t storage_ok = (rc == ST_OK && storage_sd_ok()) ? 1u : 0u;
    if (!storage_ok) {
        ESP_LOGE(TAG, "storage_init: %d (%s) (continuing without SD)", rc,
                 storage_last_error());
    }

    /* 3) mp_store — only on a mounted volume; mp_store_* must not be called
     * otherwise (PORT_SPEC §6.7, docs/research/platform.md §12-B6). */
    if (storage_ok) {
        int mrc = mp_store_init();
        if (mrc != 0) {
            storage_ok = 0;
            ESP_LOGE(TAG, "mp_store_init: %d (%s) (continuing; storage error page)",
                     mrc, mp_store_error());
        }
    } else {
        ESP_LOGW(TAG, "mp_store_init skipped: no mounted volume");
    }
    /* Written before mp_app_run() starts; after that the app task owns the
     * struct (PORT_SPEC §10.1 exception 1). */
    mp_app_state.storage_ok = storage_ok;

    /* 4) display — failure is not fatal; display_ready()==false makes every
     * display_post() a no-op, and mp_app refuses restore in that state
     * (PORT_SPEC §0.2 / §8.2). */
    rc = display_init();
    bool ready = display_ready();
    bool display_ok = (rc == 0) && ready;
    if (!display_ok) {
        ESP_LOGE(TAG, "display_init: %d (ready=%d) (continuing; panel output off)",
                 rc, ready ? 1 : 0);
    }

    /* 5) input — 5 keys, 20 ms poll, 1.0 s long press (READER_KEY_LONG_MS). */
    rc = input_init();
    if (rc != 0) {
        ESP_LOGE(TAG, "input_init: %d (continuing; keys unavailable)", rc);
    }

    /* 6) BLE — 按设置按需启动，关闭状态不上电初始化射频。 */
    if (mp_settings.ble_enabled) {
        rc = mp_ble_start();
        if (rc != 0) {
            ESP_LOGE(TAG, "mp_ble_start: %d (continuing without BLE)", rc);
        }
    } else {
        ESP_LOGI(TAG, "ble disabled by settings; radio stays off");
    }

#ifdef CONFIG_PM_ENABLE
    /* 所有外设初始化完成后再开启 DFS 与自动轻睡，防止冷启动挂载与探测被打断 */
    esp_pm_config_t pm_cfg = {
        .max_freq_mhz = 240,
        .min_freq_mhz = 40,
        .light_sleep_enable = true,
    };
    esp_err_t perr = esp_pm_configure(&pm_cfg);
    if (perr != ESP_OK) {
        ESP_LOGW(TAG, "esp_pm_configure: %s", esp_err_to_name(perr));
    }
#endif

    ESP_LOGI(TAG, "init done: storage=%u display=%u free_heap=%u",
             (unsigned)storage_ok, display_ok ? 1u : 0u,
             (unsigned)esp_get_free_heap_size());
    ESP_LOGI(TAG, "failures above are non-fatal (PORT_SPEC §0.2)");

    mp_app_start();
}
