#pragma once
/* ESP-IDF 看门狗 HAL 桩头。 */
#include <stdbool.h>
#include <stdint.h>

#include "soc/timer_group_struct.h"

typedef enum {
    WDT_MWDT0 = 0,
    WDT_MWDT1 = 1,
    WDT_RWDT = 2,
} wdt_inst_t;

typedef struct {
    wdt_inst_t inst;
    timg_dev_t *mwdt_dev;
} wdt_hal_context_t;

void wdt_hal_init(wdt_hal_context_t *hal, wdt_inst_t wdt_inst,
                  uint32_t prescaler, bool enable_intr);
void wdt_hal_write_protect_disable(wdt_hal_context_t *hal);
void wdt_hal_disable(wdt_hal_context_t *hal);
void wdt_hal_write_protect_enable(wdt_hal_context_t *hal);
