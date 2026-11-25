/* 板级只读探测，见 board.h。 */
#include "board.h"
#include "driver/gpio.h"
#include <stdio.h>

#define BOARD_PROBE_PIN_SD_PWR 34
#define BOARD_PROBE_PIN_CHG    5

void board_probe_init(void)
{
    gpio_config_t in = {
        .pin_bit_mask = (1ULL << BOARD_PROBE_PIN_SD_PWR) | (1ULL << BOARD_PROBE_PIN_CHG),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&in);
}

void board_probe_diag(char *out, int n)
{
    snprintf(out, n, "g34=%d g5=%d",
             gpio_get_level(BOARD_PROBE_PIN_SD_PWR),
             gpio_get_level(BOARD_PROBE_PIN_CHG));
}
