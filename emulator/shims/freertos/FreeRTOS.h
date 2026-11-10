#pragma once

// Emulator stand-in for FreeRTOS's types and tick macros, at the project's
// CONFIG_FREERTOS_HZ.

#include <stdint.h>

#include "sdkconfig.h"

typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef unsigned int UBaseType_t;

#define pdFALSE 0
#define pdTRUE 1
#define pdPASS pdTRUE
#define portMAX_DELAY ((TickType_t)0xffffffffUL)

#define pdMS_TO_TICKS(ms) ((TickType_t)(((uint64_t)(ms) * CONFIG_FREERTOS_HZ) / 1000))
#define pdTICKS_TO_MS(ticks) ((uint32_t)(((uint64_t)(ticks) * 1000) / CONFIG_FREERTOS_HZ))
