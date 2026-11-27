#pragma once
/* ESP32-S3 ROM Cache 控制桩头。 */
#include <stdint.h>

uint32_t Cache_Disable_ICache(void);
uint32_t Cache_Disable_DCache(void);
