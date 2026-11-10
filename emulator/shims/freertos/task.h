#pragma once

// Emulator stand-in for FreeRTOS tasks: just vTaskDelay (in emulator.cpp).

#include "freertos/FreeRTOS.h"

void vTaskDelay(TickType_t ticks);
