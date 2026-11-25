#pragma once
/* E2S A5 恢复服务：只选择经验证的原厂槽，不接收固件。 */
#include <stdbool.h>
#include <stdint.h>

void a5_server_init(void (*notify)(const uint8_t *data, uint16_t len));
void a5_server_disconnect(void);
bool a5_server_on_ble_write(const uint8_t *data, uint16_t len);
