#pragma once
/* ESP-IDF 无 OS 软复位桩头。 */
#include "esp_system.h"

void __attribute__((noreturn)) esp_restart_noos(void);
