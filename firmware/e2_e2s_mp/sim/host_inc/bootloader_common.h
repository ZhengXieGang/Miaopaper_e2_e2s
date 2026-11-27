#pragma once
/* ESP-IDF Bootloader 公共 CRC 接口（本机模拟器桩头）。 */
#include <stdint.h>

#include "esp_flash_partitions.h"

uint32_t bootloader_common_ota_select_crc(const esp_ota_select_entry_t *s);
