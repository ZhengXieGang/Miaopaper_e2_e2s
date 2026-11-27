#pragma once
/* eFuse 加密与字段读取桩头。 */
#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

typedef struct {
    int id;
} esp_efuse_desc_t;

bool esp_efuse_is_flash_encryption_enabled(void);
esp_err_t esp_efuse_read_field_blob(const esp_efuse_desc_t *field[],
                                    void *dst, size_t dst_size_bits);
