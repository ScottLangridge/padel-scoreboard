#pragma once

// Emulator stand-in for ESP-IDF's esp_err.h.

#include <stdio.h>
#include <stdlib.h>

typedef int esp_err_t;

#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103

#define ESP_ERROR_CHECK(expression)                                                                                   \
    do {                                                                                                               \
        esp_err_t err_rc_ = (expression);                                                                              \
        if (err_rc_ != ESP_OK) {                                                                                       \
            fprintf(stderr, "ESP_ERROR_CHECK failed: %s returned %d at %s:%d\n", #expression, err_rc_, __FILE__,      \
                    __LINE__);                                                                                         \
            abort();                                                                                                   \
        }                                                                                                              \
    } while (0)
