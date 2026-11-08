#include "rx433.h"

#include <stdbool.h>
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_timer.h"

#define TOLERANCE_PCT  60    // timing tolerance when matching pulses
#define SEPARATION_US  4300  // gap longer than this = sync / end of frame
#define MAX_CHANGES    67    // enough edges for a 32-bit code
#define MIN_BITS       12    // ignore "codes" shorter than this (noise)

typedef struct { uint8_t high, low; } pulses_t;
typedef struct {
    uint16_t pulse_us;
    pulses_t sync, zero, one;
    bool inverted;
} protocol_t;

// Protocol table from RCSwitch.
static const DRAM_ATTR protocol_t PROTOCOLS[] = {
    { 350, {  1, 31 }, { 1,  3 }, { 3, 1 }, false },  // 1: EV1527 / PT2262 (most common)
    { 650, {  1, 10 }, { 1,  2 }, { 2, 1 }, false },  // 2
    { 100, { 30, 71 }, { 4, 11 }, { 9, 6 }, false },  // 3
    { 380, {  1,  6 }, { 1,  3 }, { 3, 1 }, false },  // 4
    { 500, {  6, 14 }, { 1,  2 }, { 2, 1 }, false },  // 5
    { 450, { 23,  1 }, { 1,  2 }, { 2, 1 }, true  },  // 6: HT6P20B
    { 150, {  2, 62 }, { 1,  6 }, { 6, 1 }, false },  // 7: HS2303-PT
};
#define NUM_PROTOCOLS (sizeof(PROTOCOLS) / sizeof(PROTOCOLS[0]))

static QueueHandle_t s_queue;
static uint32_t s_timings[MAX_CHANGES];

static inline uint32_t IRAM_ATTR diff(uint32_t a, uint32_t b) { return a > b ? a - b : b - a; }

static bool IRAM_ATTR try_protocol(int p, unsigned changes, rx433_frame_t *out)
{
    const protocol_t *pr = &PROTOCOLS[p];
    unsigned sync_len = pr->sync.low > pr->sync.high ? pr->sync.low : pr->sync.high;
    uint32_t delay = s_timings[0] / sync_len;
    uint32_t tol = delay * TOLERANCE_PCT / 100;
    unsigned first = pr->inverted ? 2 : 1;
    uint32_t code = 0;

    for (unsigned i = first; i < changes - 1; i += 2) {
        code <<= 1;
        if (diff(s_timings[i], delay * pr->zero.high) < tol &&
            diff(s_timings[i + 1], delay * pr->zero.low) < tol) {
            // zero bit
        } else if (diff(s_timings[i], delay * pr->one.high) < tol &&
                   diff(s_timings[i + 1], delay * pr->one.low) < tol) {
            code |= 1;
        } else {
            return false;
        }
    }

    unsigned bits = (changes - 1) / 2;
    if (bits < MIN_BITS || code == 0) {
        return false;
    }
    out->code = code;
    out->bits = bits;
    out->protocol = p + 1;
    out->pulse_us = delay;
    return true;
}

static void IRAM_ATTR rx_isr(void *arg)
{
    static int64_t last_us;
    static unsigned changes;
    static unsigned repeats;

    int64_t now = esp_timer_get_time();
    uint32_t dur = (uint32_t)(now - last_us);
    last_us = now;

    if (dur > SEPARATION_US) {
        // A long gap: either the start of a frame or the gap between repeats.
        // Remotes send each frame several times; decode once we've seen the
        // same-length gap twice, so s_timings holds one complete frame.
        if (diff(dur, s_timings[0]) < 200) {
            if (++repeats == 2) {
                rx433_frame_t f;
                for (int p = 0; p < NUM_PROTOCOLS; p++) {
                    if (try_protocol(p, changes, &f)) {
                        BaseType_t woken = pdFALSE;
                        xQueueSendFromISR(s_queue, &f, &woken);
                        if (woken) portYIELD_FROM_ISR();
                        break;
                    }
                }
                repeats = 0;
            }
        }
        changes = 0;
    }

    if (changes >= MAX_CHANGES) {
        changes = 0;
        repeats = 0;
    }
    s_timings[changes++] = dur;
}

esp_err_t rx433_start(int gpio, QueueHandle_t *out_queue)
{
    s_queue = xQueueCreate(8, sizeof(rx433_frame_t));
    if (!s_queue) {
        return ESP_ERR_NO_MEM;
    }

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        return err;
    }

    // Arduino's attachInterrupt() may have installed the service already.
    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = gpio_isr_handler_add(gpio, rx_isr, NULL);
    if (err != ESP_OK) {
        return err;
    }

    *out_queue = s_queue;
    return ESP_OK;
}
