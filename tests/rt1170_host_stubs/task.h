// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Paul R. Decker

#pragma once
#include "FreeRTOS.h"
#ifdef __cplusplus
extern "C" {
#endif
TaskHandle_t xTaskCreateStatic(void (*entry)(void*), const char* name, uint32_t stack_words,
                            void* argument, UBaseType_t priority, StackType_t* stack, StaticTask_t* control);
void xTaskNotifyGive(TaskHandle_t task);
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t ticks);
void vTaskDelay(TickType_t ticks);
void vTaskSuspend(TaskHandle_t task);
#ifdef __cplusplus
}
#endif
