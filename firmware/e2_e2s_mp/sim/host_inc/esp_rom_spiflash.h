#pragma once
/* ESP32-S3 Mask ROM SPI Flash 接口（本机模拟器桩头）。 */
#include <stdint.h>

typedef enum {
    ESP_ROM_SPIFLASH_RESULT_OK = 0,
    ESP_ROM_SPIFLASH_RESULT_ERR = 1,
    ESP_ROM_SPIFLASH_RESULT_TIMEOUT = 2,
} esp_rom_spiflash_result_t;

typedef struct {
    uint32_t device_id;
    uint32_t chip_size;
    uint32_t block_size;
    uint32_t sector_size;
    uint32_t page_size;
    uint32_t status_mask;
} esp_rom_spiflash_chip_t;

extern esp_rom_spiflash_chip_t g_rom_flashchip;

esp_rom_spiflash_result_t esp_rom_spiflash_unlock(void);
esp_rom_spiflash_result_t esp_rom_spiflash_config_param(
    uint32_t device_id, uint32_t chip_size, uint32_t block_size,
    uint32_t sector_size, uint32_t page_size, uint32_t status_mask);
esp_rom_spiflash_result_t esp_rom_spiflash_erase_sector(uint32_t sector_num);
esp_rom_spiflash_result_t esp_rom_spiflash_write(uint32_t dest_addr,
                                                 const uint32_t *src,
                                                 int32_t len);
esp_rom_spiflash_result_t esp_rom_spiflash_read(uint32_t src_addr,
                                                uint32_t *dest,
                                                int32_t len);
