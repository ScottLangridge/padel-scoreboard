#pragma once

// 433 MHz OOK receiver (EV1527 / PT2262 style fixed-code remotes).
// Decoder ported from ../433MHz-receiver-poc, which uses RCSwitch's
// pulse-timing scheme.

#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t code;
    uint8_t bits;
    uint8_t protocol;
    uint16_t pulse_us;
} rx433_frame_t;

// Starts edge capture on `gpio`. Decoded frames are posted to the returned
// queue (one per repeat burst, so a held button produces several).
esp_err_t rx433_start(int gpio, QueueHandle_t *out_queue);

#ifdef __cplusplus
}
#endif
