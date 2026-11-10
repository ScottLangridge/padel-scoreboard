#include "rx433.h"

#include <stdbool.h>
#include <stdio.h>
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
#define SYNC_MIN_US      3000    // a low this long is a sync gap. Real gaps are ~10 ms but
                                 // only their first ~3-4 ms is quiet before the receiver's
                                 // gain recovers and interference returns, so don't raise
                                 // this. Interference itself never has a low this long.
#define MIN_PULSE_US     80
#define MAX_PULSE_US     1500

#define CONFIRM_MS       300     // a new code needs a second frame within this
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

// Symbols are buffered until a sync gap (a low of at least SYNC_MIN_US, or
// the RMT idle timeout), then the frame is decoded backwards from the gap:
//
//   - The bit period comes from the last few symbols, the cleanest part of
//     the frame.
//   - Each symbol that is a bit of that period is taken. One that isn't is
//     joined with the symbol before it, which repairs a bit whose low half
//     was split by an interference spike.
//   - Decoding stops at the first symbol that can't be fixed, or at
//     CONFIG_RX433_BITS bits.
//
// Interference never produces a gap that long, so it can't form frames;
// the receiver turns the LED panel's 1 kHz row scan, for one, into long
// trains of plausible-looking bits. Spikes still hit real frames, mostly
// the first bits after the gap while the receiver's gain is settling, so a
// frame can come out "clipped" (its first bits lost). Press tracking below
// matches those against codes already seen in full.

#define FRAME_BUF       64
#define PERIOD_SAMPLES  8
#define MAX_MISSED      3   // consecutive missed short highs the decoder will repair

static rmt_symbol_word_t s_frame[FRAME_BUF];
static int s_frame_len;

static void frame_seen(uint32_t code, uint8_t bits, uint16_t pulse_us);

static bool is_bit(const rmt_symbol_word_t *sym, uint32_t *period, uint32_t *shrt, bool *one)
{
    if (sym->level0 != 1 || sym->level1 != 0 || !sym->duration0 || !sym->duration1) {
        return false;
    }
    uint32_t hi = sym->duration0, lo = sym->duration1;
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

static bool near(uint32_t period, uint32_t ref)
{
    return period * 100 >= ref * (100 - PERIOD_TOL_PCT) && period * 100 <= ref * (100 + PERIOD_TOL_PCT);
}

// Median bit period of the last few symbols that are bits.
static uint32_t frame_period(void)
{
    uint32_t periods[PERIOD_SAMPLES];
    int count = 0;
    for (int i = s_frame_len - 1; i >= 0 && count < PERIOD_SAMPLES; i--) {
        uint32_t period, sh;
        bool one;
        if (is_bit(&s_frame[i], &period, &sh, &one)) {
            periods[count++] = period;
        }
    }
    if (count < 3) {
        return 0;
    }
    for (int i_insert = 1; i_insert < count; i_insert++) { // insertion sort, count <= 8
        for (int i_slot = i_insert; i_slot > 0 && periods[i_slot - 1] > periods[i_slot]; i_slot--) {
            uint32_t tmp = periods[i_slot];
            periods[i_slot] = periods[i_slot - 1];
            periods[i_slot - 1] = tmp;
        }
    }
    return periods[count / 2];
}

typedef struct {
    uint32_t code;
    uint32_t short_sum;
    int bits;
    int repairs;
    int stop; // index of the symbol that stopped decoding, -1 = reached the frame start
} segment_t;

// Decodes backwards from s_frame[from] until a symbol can't be made into
// bits of period `ref`, or `max_bits` bits.
static segment_t decode_segment(int from, uint32_t ref, int max_bits)
{
    segment_t seg = { 0 };
    int i = from;

    while (i >= 0 && seg.bits < max_bits) {
        uint32_t period, sh;
        bool one;
        int step_bits = 1; // bits this step produces

        if (is_bit(&s_frame[i], &period, &sh, &one) && near(period, ref)) {
            i--;
        } else {
            // Split repair: a spike cut this bit's low in two.
            bool ok = false;
            if (i >= 1) {
                rmt_symbol_word_t joined = s_frame[i - 1];
                uint32_t lo = joined.duration1 + s_frame[i].duration0 + s_frame[i].duration1;
                joined.duration1 = lo > 0x7fff ? 0x7fff : lo;
                if (is_bit(&joined, &period, &sh, &one) && near(period, ref)) {
                    i -= 2;
                    ok = true;
                }
            }
            // Missed-pulse repair: the receiver missed the (short) high of the
            // following bit(s), merging their lows into this symbol, which is
            // then a whole number of bit periods long. Missed highs are the
            // short ones, so the hidden bits are 0s.
            const rmt_symbol_word_t *sym = &s_frame[i];
            if (!ok && sym->level0 == 1 && sym->level1 == 0 && sym->duration1) {
                uint32_t len = sym->duration0 + sym->duration1;
                for (step_bits = 2; step_bits <= MAX_MISSED + 1; step_bits++) {
                    if (near(len, ref * step_bits) && seg.bits + step_bits <= max_bits) {
                        one = sym->duration0 * 2 > ref;
                        sh = one ? ref / 4 : sym->duration0;
                        i--;
                        ok = true;
                        break;
                    }
                }
            }
            if (!ok) {
                break;
            }
            seg.repairs++;
        }

        if (step_bits > 1) {
            seg.bits += step_bits - 1; // the hidden 0s come after this bit; walking backwards they come first
            seg.short_sum += sh * (step_bits - 1);
        }
        seg.code |= (uint32_t)one << seg.bits;
        seg.short_sum += sh;
        seg.bits++;
        step_bits = 1;
    }
    seg.stop = i;
    return seg;
}

static void log_segment(const char *what, const segment_t *seg, int from, uint32_t ref)
{
#if CONFIG_RX433_LOG_FRAMES
    static char line[FRAME_BUF * 12]; // static: too big for the task stack
    int len = 0;
    for (int i = seg->stop + 1; i <= from && len < (int)sizeof(line) - 12; i++) {
        len += snprintf(line + len, sizeof(line) - len, "%u/%u ", s_frame[i].duration0, s_frame[i].duration1);
    }
    ESP_LOGI(TAG, "%s %d bits period=%lu repairs=%d: %s", what, seg->bits, (unsigned long)ref, seg->repairs, line);
#endif
}

static void frame_decode(void)
{
    const int max_bits = CONFIG_RX433_BITS ? CONFIG_RX433_BITS : MAX_BITS;
    uint32_t ref = frame_period();
    if (!ref) {
#if CONFIG_RX433_LOG_FRAMES
        if (s_frame_len >= 8) {
            ESP_LOGI(TAG, "gap after %d symbols, no bit period found", s_frame_len);
        }
#endif
        return;
    }

    // From the gap backwards: the whole frame, or its end if damage stops it.
    segment_t end = decode_segment(s_frame_len - 1, ref, max_bits);
    if (end.bits >= MIN_BITS) {
        log_segment("run", &end, s_frame_len - 1, ref);
        frame_seen(end.code, end.bits, end.short_sum / end.bits);
    } else if (s_frame_len >= 8) {
        // Too short to use. Only real transmissions end in a gap, so log it:
        // this is what a failed press looks like.
        segment_t all = end;
        all.stop = -1; // dump the whole capture, not just the decoded part
        log_segment("short", &all, s_frame_len - 1, ref);
    }

    // Damage that couldn't be repaired: the part before it may still be the
    // start of the frame (it is if decoding reaches the buffer start, which
    // follows the previous gap).
    if (end.stop >= 1 && end.bits < max_bits) {
        segment_t head = decode_segment(end.stop - 1, ref, max_bits - end.bits);
        if (head.stop < 0 && head.bits >= MIN_BITS) {
            log_segment("head", &head, end.stop - 1, ref);
            frame_seen(head.code, head.bits, head.short_sum / head.bits);
        }
    }
}

static void frame_feed(const rmt_symbol_word_t *sym)
{
    // Idle marker, the final high pulse the idle threshold cut off, or a
    // sync gap: decode what came before. None of them is a data bit.
    bool gap = sym->val == GAP_MARKER || (sym->level0 == 1 && sym->duration1 == 0) ||
               (sym->level1 == 0 && sym->duration1 >= SYNC_MIN_US);
    if (gap) {
        frame_decode();
        s_frame_len = 0;
        return;
    }
    if (s_frame_len == FRAME_BUF) { // keep the most recent half; a frame is at the end
        memmove(s_frame, s_frame + FRAME_BUF / 2, (FRAME_BUF / 2) * sizeof(s_frame[0]));
        s_frame_len = FRAME_BUF / 2;
    }
    s_frame[s_frame_len++] = *sym;
}

/* ---------- press tracking ---------- */

// A frame with the full bit count is "full"; a shorter one is "clipped"
// (lost its first or last bits) and can only be identified as the start or
// end of a code already seen in full.
//
// A press is confirmed by:
//   - a full frame of a code confirmed before (it is in s_known), or
//   - a full frame followed within CONFIRM_MS by another frame (full or
//     clipped) of the same code, or
//   - a clipped frame that matches exactly one code in s_known.
// Any frame of the held code keeps the press alive.

#define FULL_BITS   (CONFIG_RX433_BITS ? CONFIG_RX433_BITS : 0)
#define KNOWN_CODES   8
#define MIN_PART_BITS 14 // a clipped frame needs this many bits to be matched

typedef struct {
    uint32_t code;
    uint8_t bits;
    TickType_t at;
    bool valid;
} seen_t;

static seen_t s_candidate; // last full frame of an unconfirmed code
static seen_t s_held;      // code currently reported as pressed; .at = last frame
static TickType_t s_pressed_at;
static bool s_long_sent;   // LONG already emitted for the current press
static seen_t s_known[KNOWN_CODES]; // confirmed codes, most recent first

static void emit(rx433_event_type_t type, const seen_t *seen, uint16_t pulse_us)
{
    rx433_event_t ev = {
        .type = type,
        .code = seen->code,
        .bits = seen->bits,
        .pulse_us = pulse_us,
        .held_ms = type == RX433_EVENT_PRESS ? 0 : pdTICKS_TO_MS(seen->at - s_pressed_at),
    };
    s_cfg.callback(&ev, s_cfg.user_ctx);
}

// A clipped frame is the start or the end of a full code: interference
// usually eats the first bits after the sync gap, and missed short pulses
// can merge lows into a false gap that cuts the frame's end off.
static bool is_part_of(uint32_t part, uint8_t part_bits, const seen_t *full)
{
    if (!full->valid || part_bits < MIN_PART_BITS || part_bits >= full->bits) {
        return false;
    }
    uint32_t mask = (1u << part_bits) - 1;
    return (full->code & mask) == part ||                      // end of the code
           (full->code >> (full->bits - part_bits)) == part;   // start of the code
}

static void remember(const seen_t *seen)
{
    int i = 0;
    while (i < KNOWN_CODES - 1 && s_known[i].valid && (s_known[i].code != seen->code || s_known[i].bits != seen->bits)) {
        i++;
    }
    memmove(&s_known[1], &s_known[0], i * sizeof(s_known[0])); // move to front, dropping the oldest
    s_known[0] = *seen;
    s_known[0].valid = true;
}

// Resolves a clipped frame to the single known code it is part of.
static const seen_t *known_with_part(uint32_t part, uint8_t bits)
{
    const seen_t *match = NULL;
    for (int i = 0; i < KNOWN_CODES; i++) {
        if (is_part_of(part, bits, &s_known[i])) {
            if (match) {
                return NULL; // ambiguous
            }
            match = &s_known[i];
        }
    }
    return match;
}

static bool is_known(uint32_t code, uint8_t bits)
{
    for (int i = 0; i < KNOWN_CODES; i++) {
        if (s_known[i].valid && s_known[i].code == code && s_known[i].bits == bits) {
            return true;
        }
    }
    return false;
}

static void pressed(const seen_t *seen, uint16_t pulse_us, TickType_t now)
{
    if (s_held.valid && (s_held.code != seen->code || s_held.bits != seen->bits)) {
        emit(RX433_EVENT_RELEASE, &s_held, 0);
        s_held.valid = false;
    }
    if (!s_held.valid) {
        s_held = *seen;
        s_held.valid = true;
        s_pressed_at = now;
        s_long_sent = false;
        emit(RX433_EVENT_PRESS, &s_held, pulse_us);
    }
    s_held.at = now;
    // Long press: the remote is still sending this long after the press
    // started. Timed by its frames, not by when the release arrives, which is
    // RELEASE_MS after the button is actually let go.
    if (s_cfg.long_press_ms && !s_long_sent && now - s_pressed_at >= pdMS_TO_TICKS(s_cfg.long_press_ms)) {
        s_long_sent = true;
        emit(RX433_EVENT_LONG, &s_held, pulse_us);
    }
    remember(&s_held);
}

static void frame_seen(uint32_t code, uint8_t bits, uint16_t pulse_us)
{
    TickType_t now = xTaskGetTickCount();
    bool full = FULL_BITS ? bits == FULL_BITS : true;
    bool recent = s_candidate.valid && now - s_candidate.at <= pdMS_TO_TICKS(CONFIRM_MS);

    if (full) {
        seen_t seen = { .code = code, .bits = bits, .at = now, .valid = true };
        bool confirmed = is_known(code, bits) ||
                         (recent && s_candidate.code == code && s_candidate.bits == bits);
#if CONFIG_RX433_LOG_FRAMES
        ESP_LOGI(TAG, "full 0x%08lx%s", (unsigned long)code, confirmed ? "" : " (unconfirmed)");
#endif
        if (confirmed) {
            s_candidate.valid = false;
            pressed(&seen, pulse_us, now);
        } else {
            s_candidate = seen;
        }
        return;
    }

    if (FULL_BITS && bits > FULL_BITS) {
        return;
    }
    const seen_t *match = NULL;
    if (s_held.valid && is_part_of(code, bits, &s_held)) {
        match = &s_held;
    } else if (recent && is_part_of(code, bits, &s_candidate)) {
        match = &s_candidate;
    } else {
        match = known_with_part(code, bits);
    }
#if CONFIG_RX433_LOG_FRAMES
    ESP_LOGI(TAG, "clipped %u bits 0x%06lx -> %s", bits, (unsigned long)code,
             match ? "matched" : "no match");
#endif
    if (match) {
        seen_t seen = *match;
        s_candidate.valid = false;
        pressed(&seen, pulse_us, now);
    }
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
    rmt_symbol_word_t buf[32];

    while (true) {
        size_t received = xStreamBufferReceive(s_stream, buf, sizeof(buf), pdMS_TO_TICKS(50));
        for (size_t i = 0; i < received / sizeof(buf[0]); i++) {
            frame_feed(&buf[i]);
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

    BaseType_t ok = xTaskCreate(rx_task, "rx433", 4096, NULL, CONFIG_RX433_TASK_PRIORITY, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "task");

    ESP_RETURN_ON_ERROR(rmt_receive(s_chan, s_chunk, sizeof(s_chunk), &s_rx_cfg), TAG, "receive");
    ESP_LOGI(TAG, "listening on GPIO%d", config->gpio);
    return ESP_OK;
}
