#pragma once
/* ESP-IDF 内存属性宏（本机模拟器将 RTC_DATA_ATTR 映射到独立段 mp_sim_rtc）。 */

#define IRAM_ATTR
#define DRAM_ATTR
#define RTC_DATA_ATTR __attribute__((section("mp_sim_rtc"), aligned(8)))
