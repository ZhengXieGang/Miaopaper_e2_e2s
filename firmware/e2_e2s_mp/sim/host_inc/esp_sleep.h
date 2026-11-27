#pragma once
/* ESP-IDF 休眠与 EXT1 唤醒桩头。 */
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    ESP_SLEEP_WAKEUP_UNDEFINED = 0,
    ESP_SLEEP_WAKEUP_ALL = 1,
    ESP_SLEEP_WAKEUP_EXT0 = 2,
    ESP_SLEEP_WAKEUP_EXT1 = 3,
} esp_sleep_source_t;

typedef enum {
    ESP_EXT1_WAKEUP_ANY_LOW = 0,
    ESP_EXT1_WAKEUP_ANY_HIGH = 1,
} esp_sleep_ext1_wakeup_mode_t;

typedef enum {
    ESP_PD_DOMAIN_RTC_PERIPH = 0,
    ESP_PD_DOMAIN_RTC_SLOW_MEM = 1,
    ESP_PD_DOMAIN_XTAL32K = 2,
} esp_sleep_pd_domain_t;

typedef enum {
    ESP_PD_OPTION_OFF = 0,
    ESP_PD_OPTION_ON = 1,
    ESP_PD_OPTION_AUTO = 2,
} esp_sleep_pd_option_t;

esp_err_t esp_sleep_disable_wakeup_source(esp_sleep_source_t source);
esp_err_t esp_sleep_pd_config(esp_sleep_pd_domain_t domain,
                              esp_sleep_pd_option_t option);
esp_err_t esp_sleep_enable_ext1_wakeup_io(uint64_t io_mask,
                                          esp_sleep_ext1_wakeup_mode_t level_mode);
uint32_t esp_sleep_get_wakeup_causes(void);
uint64_t esp_sleep_get_ext1_wakeup_status(void);
void __attribute__((noreturn)) esp_deep_sleep_start(void);
