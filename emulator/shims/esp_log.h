#pragma once

// Emulator stand-in for ESP-IDF's esp_log.h: lines go to the emulator's log.

#include "emulator.h"

#define ESP_LOGE(tag, format, ...) emu_log('E', tag, format, ##__VA_ARGS__)
#define ESP_LOGW(tag, format, ...) emu_log('W', tag, format, ##__VA_ARGS__)
#define ESP_LOGI(tag, format, ...) emu_log('I', tag, format, ##__VA_ARGS__)
#define ESP_LOGD(tag, format, ...) emu_log('D', tag, format, ##__VA_ARGS__)
