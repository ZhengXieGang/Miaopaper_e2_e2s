#pragma once
/* ESP-IDF 日志宏（本机模拟器桩头，可选输出）。 */

void sim_hal_log(char level, const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

#define ESP_LOGE(tag, fmt, ...) sim_hal_log('E', (tag), (fmt), ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) sim_hal_log('W', (tag), (fmt), ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) sim_hal_log('I', (tag), (fmt), ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) sim_hal_log('D', (tag), (fmt), ##__VA_ARGS__)
