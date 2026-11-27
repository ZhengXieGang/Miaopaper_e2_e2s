#pragma once
/* ESP-IDF newlib 时间同步桩头，同时将 POSIX gettimeofday/settimeofday
 * 重定向到模拟器虚拟 RTC 时钟，避免读取或修改宿主机系统时间。 */
#include <sys/time.h>

void esp_sync_timekeeping_timers(void);

int sim_hal_gettimeofday(struct timeval *tv, void *tz);
int sim_hal_settimeofday(const struct timeval *tv, const void *tz);

#define gettimeofday sim_hal_gettimeofday
#define settimeofday sim_hal_settimeofday
