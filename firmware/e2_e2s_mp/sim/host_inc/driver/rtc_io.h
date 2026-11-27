#pragma once
/* ESP-IDF RTC IO 桩头。 */
#include "driver/gpio.h"
#include "esp_err.h"

typedef enum {
    RTC_GPIO_MODE_INPUT_ONLY = 0,
} rtc_gpio_mode_t;

esp_err_t rtc_gpio_init(gpio_num_t gpio_num);
esp_err_t rtc_gpio_deinit(gpio_num_t gpio_num);
esp_err_t rtc_gpio_set_direction(gpio_num_t gpio_num, rtc_gpio_mode_t mode);
esp_err_t rtc_gpio_pullup_en(gpio_num_t gpio_num);
esp_err_t rtc_gpio_pulldown_dis(gpio_num_t gpio_num);
esp_err_t rtc_gpio_hold_en(gpio_num_t gpio_num);
esp_err_t rtc_gpio_hold_dis(gpio_num_t gpio_num);
