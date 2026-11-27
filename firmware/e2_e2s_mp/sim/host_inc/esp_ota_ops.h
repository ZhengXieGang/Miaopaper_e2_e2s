#pragma once
/* ESP-IDF OTA 运行/启动槽查询接口（本机模拟器桩头）。 */
#include "esp_partition.h"

const esp_partition_t *esp_ota_get_running_partition(void);
const esp_partition_t *esp_ota_get_boot_partition(void);
const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *start_from);
