#pragma once

// Emulator stand-in for ESP-IDF's driver/gpio.h: pin writes are no-ops.

#include <stdint.h>

#include "esp_err.h"

typedef int gpio_num_t;

inline esp_err_t gpio_set_level(gpio_num_t, uint32_t)
{
    return ESP_OK;
}
