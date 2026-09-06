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
#define pdMS_TO_TICKS(value) ((TickType_t)(value))
#define taskENTER_CRITICAL() do {} while (0)
#define taskEXIT_CRITICAL() do {} while (0)
