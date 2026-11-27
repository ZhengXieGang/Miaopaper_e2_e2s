#pragma once
/* ESP-IDF FAT VFS 空间查询桩头。 */
#include <stdint.h>

#include "esp_err.h"

esp_err_t esp_vfs_fat_info(const char *base_path,
                           uint64_t *out_total_bytes,
                           uint64_t *out_free_bytes);
