#pragma once
/* FreeRTOS 基础类型与常量桩头。 */
#include <stdint.h>

typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef uint32_t TickType_t;

#define pdFALSE 0
#define pdTRUE 1
#define pdPASS pdTRUE
#define pdFAIL pdFALSE
#define portMAX_DELAY 0xFFFFFFFFu
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))

BaseType_t xPortInIsrContext(void);
void portYIELD_FROM_ISR(void);
