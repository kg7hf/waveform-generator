// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#pragma once
#include <stdint.h>
typedef unsigned int UBaseType_t;
typedef int BaseType_t;
typedef uint32_t TickType_t;
typedef uint32_t StackType_t;
typedef struct { unsigned int placeholder; } StaticTask_t;
typedef void* TaskHandle_t;
#define pdTRUE 1
#define pdFALSE 0
#define portMAX_DELAY UINT32_MAX
// One tick per millisecond, as rt1170/os/FreeRTOSConfig.h.
#define configTICK_RATE_HZ ((TickType_t)1000U)
#define pdMS_TO_TICKS(value) ((TickType_t)(value))
// The host runs every context on one thread, so a critical section masks
// nothing and the ISR form has no mask to save.
#define taskENTER_CRITICAL() ((void)0)
#define taskEXIT_CRITICAL() ((void)0)
#ifdef __cplusplus
// Only C++ code uses the ISR forms (rt1170/os/critical_section.hpp).
inline UBaseType_t taskENTER_CRITICAL_FROM_ISR() noexcept
{
    return 0U;
}
inline void taskEXIT_CRITICAL_FROM_ISR(UBaseType_t /*saved_mask*/) noexcept {}
#endif
