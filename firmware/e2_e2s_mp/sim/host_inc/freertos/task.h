#pragma once
/* FreeRTOS 任务与延时桩头（vTaskDelay 会精确推进虚拟时钟）。 */
#include "freertos/FreeRTOS.h"

typedef void *TaskHandle_t;

TaskHandle_t xTaskGetCurrentTaskHandle(void);
void vTaskDelay(TickType_t xTicksToDelay);
BaseType_t xTaskNotifyGive(TaskHandle_t xTaskToNotify);
void vTaskNotifyGiveFromISR(TaskHandle_t xTaskToNotify,
                            BaseType_t *pxHigherPriorityTaskWoken);
uint32_t ulTaskNotifyTake(BaseType_t xClearCountOnExit, TickType_t xTicksToWait);
