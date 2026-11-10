#pragma once

// Emulator stand-in for FreeRTOS queues (implemented in emulator.cpp).
// Waiting on one is where the emulator lets time pass and remote events
// arrive.

#include "freertos/FreeRTOS.h"

typedef struct EmuQueue *QueueHandle_t;

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size);
BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t ticks_to_wait);
BaseType_t xQueueReceive(QueueHandle_t queue, void *buffer, TickType_t ticks_to_wait);
