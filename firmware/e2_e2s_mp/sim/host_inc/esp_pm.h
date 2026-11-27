#pragma once
/* ESP-IDF 电源管理锁桩头。 */
#include "esp_err.h"

typedef enum {
    ESP_PM_CPU_FREQ_MAX = 0,
    ESP_PM_APB_FREQ_MAX = 1,
    ESP_PM_NO_LIGHT_SLEEP = 2,
} esp_pm_lock_type_t;

typedef struct esp_pm_lock_opaque_ *esp_pm_lock_handle_t;

esp_err_t esp_pm_lock_create(esp_pm_lock_type_t lock_type, int arg,
                             const char *name, esp_pm_lock_handle_t *out_handle);
esp_err_t esp_pm_lock_acquire(esp_pm_lock_handle_t handle);
esp_err_t esp_pm_lock_release(esp_pm_lock_handle_t handle);
