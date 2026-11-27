#pragma once
/* CPU 调试/中断/多核控制桩头。 */
#include <stdbool.h>
#include <stdint.h>

bool esp_cpu_dbgr_is_attached(void);
void esp_cpu_intr_disable(uint32_t intr_mask);
uint32_t esp_cpu_get_core_id(void);
void esp_cpu_stall(int core_id);
