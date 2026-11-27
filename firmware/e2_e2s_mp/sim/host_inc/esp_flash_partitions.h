#pragma once
/* ESP-IDF 分区与 otadata 结构定义（与 ESP-IDF v6.1.1 对齐）。 */
#include <stdint.h>

typedef enum {
    ESP_OTA_IMG_NEW = 0x0u,
    ESP_OTA_IMG_PENDING_VERIFY = 0x1u,
    ESP_OTA_IMG_VALID = 0x2u,
    ESP_OTA_IMG_INVALID = 0x3u,
    ESP_OTA_IMG_ABORTED = 0x4u,
    ESP_OTA_IMG_UNDEFINED = 0xFFFFFFFFu,
} esp_ota_img_states_t;

typedef struct {
    uint32_t ota_seq;
    uint8_t seq_label[20];
    uint32_t ota_state;
    uint32_t crc;
} esp_ota_select_entry_t;

_Static_assert(sizeof(esp_ota_select_entry_t) == 32, "esp_ota_select_entry_t 必须为 32 字节");

typedef struct {
    uint32_t offset;
    uint32_t size;
} esp_partition_pos_t;
