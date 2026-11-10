#pragma once

// Emulator stand-in for ESP-IDF's esp_timer.h. The clock is the emulator's.
// Timers are created but never fire: the only one is Hub12's panel scan,
// which the emulator doesn't need because it shows each frame as it is
// copied to the panel.

#include <stdint.h>

#include "emulator.h"
#include "esp_err.h"

typedef struct {
    void (*callback)(void *arg);
    void *arg;
    const char *name;
} esp_timer_create_args_t;

typedef struct esp_timer *esp_timer_handle_t;

inline int64_t esp_timer_get_time()
{
    return emu_now_us();
}

inline esp_err_t esp_timer_create(const esp_timer_create_args_t *, esp_timer_handle_t *timer)
{
    *timer = nullptr;
    return ESP_OK;
}

inline esp_err_t esp_timer_start_periodic(esp_timer_handle_t, uint64_t)
{
    return ESP_OK;
}
