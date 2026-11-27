#pragma once
/* ESP-IDF 微秒单调时钟桩头。 */
#include <stdint.h>

int64_t esp_timer_get_time(void);
