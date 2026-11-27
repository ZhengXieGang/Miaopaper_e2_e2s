#pragma once
/* FreeRTOS 信号量与递归互斥锁桩头。 */
#include "freertos/FreeRTOS.h"

typedef void *SemaphoreHandle_t;

SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void);
BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t xMutex, TickType_t xBlockTime);
BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t xMutex);
