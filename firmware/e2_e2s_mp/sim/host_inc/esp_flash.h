#pragma once
/* ESP-IDF SPI Flash 接口（本机模拟器桩头）。 */
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct esp_flash_t {
    uint32_t size;
} esp_flash_t;

extern esp_flash_t *esp_flash_default_chip;

esp_err_t esp_flash_get_size(esp_flash_t *chip, uint32_t *out_size);
esp_err_t esp_flash_read(esp_flash_t *chip, void *buffer, uint32_t address, uint32_t length);
esp_err_t esp_flash_write(esp_flash_t *chip, const void *buffer, uint32_t address, uint32_t length);
esp_err_t esp_flash_erase_region(esp_flash_t *chip, uint32_t start, uint32_t len);
