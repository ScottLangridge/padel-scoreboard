#pragma once

// 433 MHz receiver for fixed-code OOK remotes (EV1527 / PT2262 family).
//
// The receiver's DATA pin is captured with the RMT peripheral. Each bit is a
// high/low pair where one half is ~3x the other (long-high = 1,
// short-high = 0); frames are separated by a long low sync gap. A frame is
// only accepted once two identical copies arrive back to back, and a held
// button's repeats collapse into one PRESS followed by one RELEASE.
//
// Decode only: the component does not know which remotes are "ours". The
// application decides what each code means.

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RX433_EVENT_PRESS,   // a code started arriving
    RX433_EVENT_RELEASE, // that code stopped (button let go / out of range)
} rx433_event_type_t;

typedef struct {
    rx433_event_type_t type;
    uint32_t code;     // received bits, first bit in the MSB position
    uint8_t bits;      // number of bits in the frame (12..32)
    uint16_t pulse_us; // measured short-pulse width, for diagnostics
    uint32_t held_ms;  // RELEASE only: first to last frame of the press. The
                       // event itself arrives RX433_RELEASE_MS after that.
} rx433_event_t;

// Called from the rx433 task. Keep it short (e.g. post to your own queue).
typedef void (*rx433_callback_t)(const rx433_event_t *event, void *user_ctx);

typedef struct {
    int gpio;                  // receiver DATA pin, e.g. CONFIG_RX433_GPIO
    rx433_callback_t callback; // required
    void *user_ctx;            // passed to callback
} rx433_config_t;

// Starts the receiver. Only one instance is supported; a second call returns
// ESP_ERR_INVALID_STATE.
esp_err_t rx433_start(const rx433_config_t *config);

#ifdef __cplusplus
}
#endif
