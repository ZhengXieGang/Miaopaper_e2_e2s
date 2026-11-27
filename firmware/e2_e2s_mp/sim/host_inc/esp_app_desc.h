#pragma once
/* ESP-IDF 应用描述符结构（与 ESP-IDF v6.1.1 精确 256 B 对齐）。 */
#include <stddef.h>
#include <stdint.h>

#define ESP_APP_DESC_MAGIC_WORD (0xABCD5432u)

typedef struct {
    uint32_t magic_word;
    uint32_t secure_version;
    uint32_t reserv1[2];
    char version[32];
    char project_name[32];
    char time[16];
    char date[16];
    char idf_ver[32];
    uint8_t app_elf_sha256[32];
    uint16_t min_efuse_blk_rev_full;
    uint16_t max_efuse_blk_rev_full;
    uint8_t mmu_page_size;
    uint8_t spi_flash_mode;
    uint8_t reserv3[2];
    uint32_t reserv2[18];
} esp_app_desc_t;

_Static_assert(sizeof(esp_app_desc_t) == 256, "esp_app_desc_t 必须为 256 字节");
_Static_assert(offsetof(esp_app_desc_t, secure_version) == 4, "secure_version 偏移必须为 4");

const esp_app_desc_t *esp_app_get_description(void);
