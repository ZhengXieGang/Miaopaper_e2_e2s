#pragma once
/* 系统重启桩头。 */
#include "esp_attr.h"

void __attribute__((noreturn)) esp_restart(void);
