#include "rx433.h"

#include <stdbool.h>
#include <string.h>
#include "driver/rmt_rx.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "soc/soc_caps.h"
#include "sdkconfig.h"

static const char *TAG = "rx433";

/* ---------- tuning ---------- */

#define RESOLUTION_HZ    1000000 // 1 tick = 1 us
#define GLITCH_NS        3000    // pulses shorter than this are dropped by the RMT filter
#define GAP_NS           4000000 // a low longer than this ends a capture (sync gap)
#define CHUNK_SYMBOLS    64      // RMT -> ISR chunk size (partial receive)
#define STREAM_BYTES     (512 * sizeof(rmt_symbol_word_t))

#define MIN_BITS         12
#define MAX_BITS         32
#define MIN_RATIO_X10    16      // long half must be >= 1.6x the short half
#define PERIOD_TOL_PCT   25      // bit periods within a frame must agree to +-25%
#define MIN_PULSE_US     80
#define MAX_PULSE_US     1500

#define CONFIRM_MS       300     // two identical frames this close = a real frame
#define RELEASE_MS       CONFIG_RX433_RELEASE_MS

// Written into the stream after a capture ends (both durations 0 never
// occurs in real data).
#define GAP_MARKER       0u

/* ---------- state ---------- */

static rmt_channel_handle_t s_chan;
static StreamBufferHandle_t s_stream;
static rmt_symbol_word_t s_chunk[CHUNK_SYMBOLS];
static rx433_config_t s_cfg;

static const rmt_receive_config_t s_rx_cfg = {
    .signal_range_min_ns = GLITCH_NS,
    .signal_range_max_ns = GAP_NS,
    .flags.en_partial_rx = true,
};

/* ---------- capture (ISR) ---------- */

// Hands every chunk to the task through a stream buffer. The receiver outputs
// noise continuously when nothing is transmitting, so captures can be long;
// partial receive delivers them piece by piece instead of truncating.
static bool IRAM_ATTR on_recv(rmt_channel_handle_t chan, const rmt_rx_done_event_data_t *ed, void *ctx)
{
    BaseType_t woken = pdFALSE;
    // Only ever write whole symbols, so a full buffer drops data but never
    // misaligns the stream.
    size_t room = xStreamBufferSpacesAvailable(s_stream) & ~(sizeof(rmt_symbol_word_t) - 1);
    size_t len = ed->num_symbols * sizeof(rmt_symbol_word_t);
    if (ed->flags.is_last && room >= sizeof(rmt_symbol_word_t)) {
        room -= sizeof(rmt_symbol_word_t); // keep space for the gap marker
    }
    xStreamBufferSendFromISR(s_stream, ed->received_symbols, len < room ? len : room, &woken);
    if (ed->flags.is_last) {
        const uint32_t marker = GAP_MARKER;
        xStreamBufferSendFromISR(s_stream, &marker, sizeof(marker), &woken);
        rmt_receive(chan, s_chunk, sizeof(s_chunk), &s_rx_cfg);
    }
    return woken == pdTRUE;
}

/* ---------- decoder ---------- */

// Accumulates consistent bit symbols. Anything that doesn't look like a bit
// restarts the run, so noise in front of a frame falls away and the frame is
// whatever run is in progress when the sync gap arrives.
typedef struct {
    uint32_t code;
    uint8_t bits;
    bool overflow;
    uint32_t period_us; // of the first bit in the run
    uint32_t short_sum; // sum of short halves, for pulse_us
} run_t;

static void run_reset(run_t *r)
{
    memset(r, 0, sizeof(*r));
}

static bool is_bit(const rmt_symbol_word_t *s, uint32_t *period, uint32_t *shrt, bool *one)
{
    if (s->level0 != 1 || s->level1 != 0 || !s->duration0 || !s->duration1) {
        return false;
    }
    uint32_t hi = s->duration0, lo = s->duration1;
    uint32_t sh = hi < lo ? hi : lo;
    uint32_t lg = hi < lo ? lo : hi;
    if (sh < MIN_PULSE_US || sh > MAX_PULSE_US || lg * 10 < sh * MIN_RATIO_X10) {
        return false;
    }
    *period = hi + lo;
    *shrt = sh;
    *one = hi > lo;
    return true;
}

static void run_add(run_t *r, const rmt_symbol_word_t *s)
{
    uint32_t period, sh;
    bool one;

    if (!is_bit(s, &period, &sh, &one)) {
        run_reset(r);
        return;
    }
    if (r->bits && (period * 100 < r->period_us * (100 - PERIOD_TOL_PCT) ||
                    period * 100 > r->period_us * (100 + PERIOD_TOL_PCT))) {
        run_reset(r); // timing changed: start a new run at this symbol
    }
    if (!r->bits) {
        r->period_us = period;
    }
    if (r->bits == MAX_BITS) {
        r->overflow = true;
        return;
    }
    r->code = (r->code << 1) | one;
    r->short_sum += sh;
    r->bits++;
}

/* ---------- press tracking ---------- */

typedef struct {
    uint32_t code;
    uint8_t bits;
    TickType_t at;
    bool valid;
} seen_t;

static seen_t s_candidate; // last decoded frame, waiting for a matching copy
static seen_t s_held;      // code currently reported as pressed; .at = last frame
static TickType_t s_pressed_at;

static void emit(rx433_event_type_t type, const seen_t *f, uint16_t pulse_us)
{
    rx433_event_t ev = {
        .type = type,
        .code = f->code,
        .bits = f->bits,
        .pulse_us = pulse_us,
        .held_ms = type == RX433_EVENT_RELEASE ? pdTICKS_TO_MS(f->at - s_pressed_at) : 0,
    };
    s_cfg.callback(&ev, s_cfg.user_ctx);
}

static void frame_decoded(uint32_t code, uint8_t bits, uint16_t pulse_us)
{
    TickType_t now = xTaskGetTickCount();
    bool confirmed = s_candidate.valid && s_candidate.code == code && s_candidate.bits == bits &&
                     now - s_candidate.at <= pdMS_TO_TICKS(CONFIRM_MS);
    s_candidate = (seen_t){ .code = code, .bits = bits, .at = now, .valid = true };
    if (!confirmed) {
        return;
    }

    if (s_held.valid && (s_held.code != code || s_held.bits != bits)) {
        emit(RX433_EVENT_RELEASE, &s_held, 0);
        s_held.valid = false;
    }
    if (!s_held.valid) {
        s_held = s_candidate;
        s_pressed_at = now;
        emit(RX433_EVENT_PRESS, &s_held, pulse_us);
    }
    s_held.at = now;
}

static void check_release(void)
{
    if (s_held.valid && xTaskGetTickCount() - s_held.at > pdMS_TO_TICKS(RELEASE_MS)) {
        emit(RX433_EVENT_RELEASE, &s_held, 0);
        s_held.valid = false;
    }
}

/* ---------- task ---------- */

static void rx_task(void *arg)
{
    run_t run;
    run_reset(&run);
    rmt_symbol_word_t buf[32];

    while (true) {
        size_t n = xStreamBufferReceive(s_stream, buf, sizeof(buf), pdMS_TO_TICKS(50));
        for (size_t i = 0; i < n / sizeof(buf[0]); i++) {
            const rmt_symbol_word_t *s = &buf[i];

            if (s->val == GAP_MARKER) {
                if (!run.overflow && run.bits >= MIN_BITS) {
                    frame_decoded(run.code, run.bits, run.short_sum / run.bits);
                }
                run_reset(&run);
            } else if (s->level0 == 1 && s->duration1 == 0) {
                // Lone high pulse cut off by the idle threshold: the sync
                // pulse that ends the frame. Leave the run for the marker.
            } else {
                run_add(&run, s);
            }
        }
        check_release();
    }
}

/* ---------- public ---------- */

esp_err_t rx433_start(const rx433_config_t *config)
{
    ESP_RETURN_ON_FALSE(config && config->callback, ESP_ERR_INVALID_ARG, TAG, "callback required");
    ESP_RETURN_ON_FALSE(!s_chan, ESP_ERR_INVALID_STATE, TAG, "already started");
    s_cfg = *config;

    s_stream = xStreamBufferCreate(STREAM_BYTES, sizeof(rmt_symbol_word_t));
    ESP_RETURN_ON_FALSE(s_stream, ESP_ERR_NO_MEM, TAG, "stream buffer");

    rmt_rx_channel_config_t ch = {
        .gpio_num = config->gpio,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RESOLUTION_HZ,
        .mem_block_symbols = SOC_RMT_MEM_WORDS_PER_CHANNEL,
    };
    ESP_RETURN_ON_ERROR(rmt_new_rx_channel(&ch, &s_chan), TAG, "rx channel");

    rmt_rx_event_callbacks_t cbs = { .on_recv_done = on_recv };
    ESP_RETURN_ON_ERROR(rmt_rx_register_event_callbacks(s_chan, &cbs, NULL), TAG, "callbacks");
    ESP_RETURN_ON_ERROR(rmt_enable(s_chan), TAG, "enable");

    BaseType_t ok = xTaskCreate(rx_task, "rx433", 3072, NULL, CONFIG_RX433_TASK_PRIORITY, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "task");

    ESP_RETURN_ON_ERROR(rmt_receive(s_chan, s_chunk, sizeof(s_chunk), &s_rx_cfg), TAG, "receive");
    ESP_LOGI(TAG, "listening on GPIO%d", config->gpio);
    return ESP_OK;
}
