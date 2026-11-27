#pragma once
/* ESP-IDF 镜像校验接口定义（本机模拟器桩头）。 */
#include <stdint.h>

#include "esp_app_format.h"
#include "esp_err.h"
#include "esp_flash_partitions.h"

typedef struct {
    uint32_t start_addr;
    esp_image_header_t image;
    esp_image_segment_header_t segments[ESP_IMAGE_MAX_SEGMENTS];
    uint32_t segment_data[ESP_IMAGE_MAX_SEGMENTS];
    uint32_t image_len;
    uint8_t image_digest[32];
    uint32_t secure_version;
    uint32_t mmu_page_size;
} esp_image_metadata_t;

typedef enum {
    ESP_IMAGE_VERIFY = 0,
    ESP_IMAGE_VERIFY_SILENT = 1,
} esp_image_load_mode_t;

esp_err_t esp_image_verify(esp_image_load_mode_t mode,
                           const esp_partition_pos_t *part,
                           esp_image_metadata_t *data);
