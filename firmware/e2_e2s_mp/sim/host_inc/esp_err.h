#pragma once
/* ESP-IDF 错误码与名称映射（本机模拟器桩头）。 */

typedef int esp_err_t;

#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_NOT_SUPPORTED 0x106
#define ESP_ERR_TIMEOUT 0x107
#define ESP_ERR_INVALID_CRC 0x109
#define ESP_ERR_IMAGE_INVALID 0x2002

const char *esp_err_to_name(esp_err_t code);
