// Host emulator for the scoreboard firmware: runs the real screen flow
// (main/padel-scoreboard.cpp, built against the stand-in headers in shims/)
// with the panel drawn in the terminal and the 433 MHz remotes on the
// keyboard, or driven by a test script. See README.md.
//
// This file is the core: the clock, the FreeRTOS queue and delay the
// firmware waits in, the remotes and receiver, and the log and panel output.

#include "emulator.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include "DMD2.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "internal.h"
#include "rx433.h"
#include "sdkconfig.h"

extern "C" void app_main(void);

bool g_virtual_clock = false;

namespace {

constexpr int64_t TICK_US = 1000000 / CONFIG_FREERTOS_HZ;

// ---------------------------------------------------------------------------
// Time and scheduled actions
//
// Everything the emulator does by itself (remote frames, script steps) is an
// action scheduled for a time. Live, a thread runs each one when real time
// reaches it. On the virtual clock there is no second thread: whenever the
// firmware waits, the clock jumps straight to the next action due before its
// wait ends, so a whole match runs in moments and the same way every time.

std::mutex g_mutex; // guards g_actions, g_virtual_us and every EmuQueue
std::condition_variable g_wake;
std::multimap<int64_t, std::function<void()>> g_actions;
int64_t g_virtual_us = 0;
const auto g_start = std::chrono::steady_clock::now();

std::function<bool()> g_idle_handler;
double g_pace = 0; // virtual clock: 0 = as fast as possible, else times real time

void draw_panel_when_due(bool changed);

int64_t real_us()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - g_start).count();
}

std::chrono::steady_clock::time_point real_time_point(int64_t at_us)
{
    return g_start + std::chrono::microseconds(at_us);
}

// Live only: the thread that runs actions as they fall due.
void run_actions_live()
{
    std::unique_lock<std::mutex> lock(g_mutex);
    while (true) {
        if (g_actions.empty()) {
            g_wake.wait(lock);
            continue;
        }
        auto next = g_actions.begin();
        if (real_us() < next->first) {
            g_wake.wait_until(lock, real_time_point(next->first));
            continue;
        }
        std::function<void()> action = std::move(next->second);
        g_actions.erase(next);
        lock.unlock();
        action();
        lock.lock();
    }
}

// Virtual clock: moves it forward, in real time too when pacing.
void move_clock_to(int64_t at_us)
{
    int64_t from_us;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        from_us = g_virtual_us;
        g_virtual_us = std::max(g_virtual_us, at_us);
    }
    if (g_pace > 0 && at_us > from_us) {
        std::this_thread::sleep_for(std::chrono::microseconds((int64_t)((at_us - from_us) / g_pace)));
        draw_panel_when_due(false);
    }
}

// Virtual clock: runs the actions due up to `deadline`, moving the clock to
// each, and then to the deadline. Returns early, true, as soon as `ready`
// holds (checked with g_mutex held).
//
// A wait for ever, with nothing ready and the remotes quiet, is the board
// idling: the idle handler gets a turn first each time round.
template <class Ready>
bool advance_until(int64_t deadline, Ready ready)
{
    auto is_ready = [&ready]() {
        std::lock_guard<std::mutex> lock(g_mutex);
        return ready();
    };
    while (true) {
        if (deadline == NEVER && g_idle_handler && !radio_busy() && !is_ready() && g_idle_handler()) {
            continue;
        }
        std::function<void()> action;
        int64_t action_at;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (ready()) {
                return true;
            }
            if (g_actions.empty() || g_actions.begin()->first > deadline) {
                if (deadline == NEVER) {
                    fprintf(stderr, "emulator: the firmware is waiting for input and nothing is left to happen\n");
                    exit(1);
                }
                break;
            }
            auto next = g_actions.begin();
            action_at = next->first;
            action = std::move(next->second);
            g_actions.erase(next);
        }
        move_clock_to(action_at);
        action();
    }
    move_clock_to(deadline);
    return false;
}

int64_t deadline_after(TickType_t ticks)
{
    return ticks == portMAX_DELAY ? NEVER : emu_now_us() + (int64_t)ticks * TICK_US;
}

// ---------------------------------------------------------------------------
// Output: the log and the panel

std::mutex g_print_mutex;
LogOutput g_log_output = LogOutput::Stdout;
std::vector<std::function<void(const std::string &)>> g_log_handlers;
bool g_panel[PANEL_H][PANEL_W];
FILE *g_frames_file = nullptr;

void log_line(char level, const char *tag, const char *text)
{
    char line[320];
    snprintf(line, sizeof line, "%c (%lld) %s: %s", level, (long long)(emu_now_us() / 1000), tag, text);
    print_line(line);
    for (auto &handler : g_log_handlers) {
        handler(line);
    }
}

// The terminal can't take every frame when a script plays faster than real
// time, and a screen shows ~60 a second anyway: draw at most that often,
// always ending on the latest frame.
bool g_panel_drawn = true;
int64_t g_last_draw_us = -1000000;

void draw_panel_when_due(bool changed)
{
    if (changed) {
        g_panel_drawn = false;
    }
    int64_t now = real_us();
    if (g_panel_drawn || (g_pace > 0 && now - g_last_draw_us < 16000)) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_print_mutex);
    terminal_draw_panel();
    g_panel_drawn = true;
    g_last_draw_us = now;
}

void write_frame()
{
    fprintf(g_frames_file, "%lld ", (long long)emu_now_us());
    for (int y = 0; y < PANEL_H; y++) {
        uint32_t row = 0;
        for (int x = 0; x < PANEL_W; x++) {
            row = row << 1 | g_panel[y][x];
        }
        fprintf(g_frames_file, "%08x", row);
    }
    fputc('\n', g_frames_file);
}

// ---------------------------------------------------------------------------
// Remotes and the receiver

// The three remotes and what rx433 would report for them. It mirrors the
// press tracking in components/rx433/rx433.c:
//   - a held button sends a frame every FRAME_PERIOD_US;
//   - a code never seen before needs a second frame within CONFIRM_US before
//     it counts (after that one frame is enough);
//   - PRESS on the first counted frame, LONG once it has been sending for
//     long_press_ms, RELEASE CONFIG_RX433_RELEASE_MS after its last frame;
//   - a frame of a different code releases the held one first.
// Its state is only touched from actions, which never run two at a time.
class Radio {
public:
    esp_err_t start(const rx433_config_t &config)
    {
        if (!config.callback) {
            return ESP_ERR_INVALID_ARG;
        }
        if (started_) {
            return ESP_ERR_INVALID_STATE;
        }
        config_ = config;
        started_ = true;
        return ESP_OK;
    }

    void press(char key, int64_t at_us, int64_t hold_us)
    {
        presses_due_++;
        schedule(at_us, [this, key, hold_us]() {
            presses_due_--;
            start_sending(key, hold_us);
        });
    }

    // A press is due or under way, or one hasn't been released yet.
    bool busy() const { return presses_due_ > 0 || ticking_; }

private:
    static constexpr int64_t FRAME_PERIOD_US = 40000; // one 24-bit frame plus its sync gap
    static constexpr int64_t CONFIRM_US = 300000;     // rx433.c CONFIRM_MS
    static constexpr int64_t RELEASE_US = CONFIG_RX433_RELEASE_MS * 1000LL;
    static constexpr uint8_t BITS = 24;

    struct Remote {
        char key;
        uint32_t code;
        int64_t sending_until; // last moment its button is down, -1 = never pressed
    };

    void start_sending(char key, int64_t hold_us)
    {
        int64_t now = emu_now_us();
        for (Remote &remote : remotes_) {
            if (remote.key == key) {
                remote.sending_until = std::max(remote.sending_until, now + hold_us);
            }
        }
        if (!ticking_) {
            ticking_ = true;
            schedule(now, [this]() { tick(); });
        }
    }

    // One frame period: each remote that is sending gets a frame through,
    // then the held press is released if it has gone quiet.
    void tick()
    {
        int64_t now = emu_now_us();
        bool sending = false;
        for (const Remote &remote : remotes_) {
            if (now <= remote.sending_until) {
                frame(remote.code, now);
                sending = true;
            }
        }
        if (held_ && now - last_frame_at_ > RELEASE_US) {
            emit(RX433_EVENT_RELEASE, held_code_, last_frame_at_ - pressed_at_);
            held_ = false;
        }
        if (sending || held_) {
            schedule(now + FRAME_PERIOD_US, [this]() { tick(); });
        } else {
            ticking_ = false;
        }
    }

    void frame(uint32_t code, int64_t now)
    {
        bool known = std::find(known_.begin(), known_.end(), code) != known_.end();
        bool second = candidate_valid_ && candidate_code_ == code && now - candidate_at_ <= CONFIRM_US;
        if (!known && !second) {
            candidate_valid_ = true;
            candidate_code_ = code;
            candidate_at_ = now;
            return;
        }
        candidate_valid_ = false;
        if (!known) {
            known_.push_back(code);
        }
        if (held_ && held_code_ != code) {
            emit(RX433_EVENT_RELEASE, held_code_, last_frame_at_ - pressed_at_);
            held_ = false;
        }
        if (!held_) {
            held_ = true;
            held_code_ = code;
            pressed_at_ = now;
            long_sent_ = false;
            emit(RX433_EVENT_PRESS, code, 0);
        }
        last_frame_at_ = now;
        int64_t held_us = now - pressed_at_;
        if (config_.long_press_ms && !long_sent_ && held_us >= config_.long_press_ms * 1000LL) {
            long_sent_ = true;
            emit(RX433_EVENT_LONG, code, held_us);
        }
    }

    void emit(rx433_event_type_t type, uint32_t code, int64_t held_us)
    {
        static const char *const NAMES[] = {"PRESS", "LONG", "RELEASE"};
        char key = '?';
        for (const Remote &remote : remotes_) {
            if (remote.code == code) {
                key = remote.key;
            }
        }
        char text[64];
        snprintf(text, sizeof text, "%c %s (0x%06lx)", key, NAMES[type], (unsigned long)code);
        log_line('E', "remote", text); // E for emulator, so these stand out from the firmware's lines
        rx433_event_t event = {};
        event.type = type;
        event.code = code;
        event.bits = BITS;
        event.pulse_us = 350;
        event.held_ms = (uint32_t)(held_us / 1000);
        config_.callback(&event, config_.user_ctx);
    }

    Remote remotes_[3] = {{'a', 0x5a5a51, -1}, {'b', 0x5a5a52, -1}, {'x', 0x3c3c3c, -1}};
    rx433_config_t config_ = {};
    bool started_ = false;
    bool ticking_ = false;
    std::atomic<int> presses_due_{0}; // scheduled, not started (presses come from the key thread too)

    std::vector<uint32_t> known_; // codes confirmed before
    bool candidate_valid_ = false;
    uint32_t candidate_code_ = 0;
    int64_t candidate_at_ = 0;

    bool held_ = false;
    uint32_t held_code_ = 0;
    int64_t pressed_at_ = 0, last_frame_at_ = 0;
    bool long_sent_ = false;
};

Radio g_radio;

} // namespace

// ---------------------------------------------------------------------------
// internal.h

void schedule(int64_t at_us, std::function<void()> action)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_actions.emplace(at_us, std::move(action));
    g_wake.notify_all();
}

void on_idle(std::function<bool()> handler)
{
    g_idle_handler = std::move(handler);
}

void pace_virtual_clock(double speed)
{
    g_pace = speed;
}

void on_log(std::function<void(const std::string &)> handler)
{
    g_log_handlers.push_back(std::move(handler));
}

void set_log_output(LogOutput output)
{
    g_log_output = output;
}

bool record_frames(const char *path)
{
    g_frames_file = fopen(path, "w");
    return g_frames_file != nullptr;
}

bool panel_pixel(int x, int y)
{
    return x >= 0 && x < PANEL_W && y >= 0 && y < PANEL_H && g_panel[y][x];
}

void print_line(const char *line)
{
    std::lock_guard<std::mutex> lock(g_print_mutex);
    switch (g_log_output) {
    case LogOutput::Stdout:
        puts(line);
        break;
    case LogOutput::Terminal:
        terminal_log(line);
        break;
    case LogOutput::None:
        break;
    }
}

void print_output(const char *line)
{
    if (g_log_output == LogOutput::None) {
        std::lock_guard<std::mutex> lock(g_print_mutex);
        puts(line);
    } else {
        print_line(line);
    }
}

void press_remote(char key, int64_t at_us, int64_t hold_us)
{
    g_radio.press(key, at_us, hold_us);
}

bool radio_busy()
{
    return g_radio.busy();
}

// ---------------------------------------------------------------------------
// emulator.h: what the shims call

int64_t emu_now_us()
{
    if (g_virtual_clock) {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_virtual_us;
    }
    return real_us();
}

void emu_show_panel(const DMDFrame &panel)
{
    for (int y = 0; y < PANEL_H; y++) {
        for (int x = 0; x < PANEL_W; x++) {
            g_panel[y][x] = panel.getPixel(x, y);
        }
    }
    if (g_frames_file) {
        write_frame();
    }
    if (g_log_output == LogOutput::Terminal) {
        draw_panel_when_due(true);
    }
}

void emu_log(char level, const char *tag, const char *format, ...)
{
    char text[256];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof text, format, args);
    va_end(args);
    log_line(level, tag, text);
}

extern "C" esp_err_t rx433_start(const rx433_config_t *config)
{
    esp_err_t result = config ? g_radio.start(*config) : ESP_ERR_INVALID_ARG;
    if (result == ESP_OK) {
        log_line('I', "rx433", "emulated: remotes a, b and x");
    }
    return result;
}

struct EmuQueue {
    size_t item_size;
    size_t capacity;
    std::deque<std::vector<uint8_t>> items;
};

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size)
{
    return new EmuQueue{item_size, length, {}};
}

// Never blocks: the firmware only sends with a zero wait (from the rx433
// callback), dropping the event when the queue is full, as here.
BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (queue->items.size() >= queue->capacity) {
        return pdFALSE;
    }
    const uint8_t *bytes = static_cast<const uint8_t *>(item);
    queue->items.emplace_back(bytes, bytes + queue->item_size);
    g_wake.notify_all();
    return pdTRUE;
}

BaseType_t xQueueReceive(QueueHandle_t queue, void *buffer, TickType_t ticks_to_wait)
{
    int64_t deadline = deadline_after(ticks_to_wait);
    auto ready = [queue]() { return !queue->items.empty(); };
    if (g_virtual_clock) {
        if (!advance_until(deadline, ready)) {
            return pdFALSE;
        }
    } else {
        std::unique_lock<std::mutex> lock(g_mutex);
        if (deadline == NEVER) {
            g_wake.wait(lock, ready);
        } else if (!g_wake.wait_until(lock, real_time_point(deadline), ready)) {
            return pdFALSE;
        }
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    memcpy(buffer, queue->items.front().data(), queue->item_size);
    queue->items.pop_front();
    return pdTRUE;
}

void vTaskDelay(TickType_t ticks)
{
    int64_t deadline = deadline_after(ticks);
    if (g_virtual_clock) {
        advance_until(deadline, []() { return false; });
    } else if (deadline == NEVER) {
        while (true) {
            std::this_thread::sleep_for(std::chrono::hours(1));
        }
    } else {
        std::this_thread::sleep_until(real_time_point(deadline));
    }
}

// ---------------------------------------------------------------------------

namespace {

const char *const USAGE = "usage: emulator                          play live in the terminal\n"
                          "       emulator --script <file> [options]  run a test script (- = stdin)\n"
                          "options:\n"
                          "  --quiet          print only the script's own output, not the log\n"
                          "  --frames <file>  write every frame shown, with its time\n"
                          "  --watch          show it in the terminal as it plays\n"
                          "  --speed <n>      with --watch: n times real time (default 1)\n";

[[noreturn]] void usage_error()
{
    fputs(USAGE, stderr);
    exit(2);
}

void start_live()
{
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        fprintf(stderr, "emulator: live mode needs a terminal; use --script <file> otherwise\n");
        exit(2);
    }
    terminal_start("a/b: tap remote   A/B: hold remote 1.5 s   x: unknown remote   q: quit");
    set_log_output(LogOutput::Terminal);
    std::thread(run_actions_live).detach();
    std::thread([]() { read_keys(true); }).detach();
}

} // namespace

int main(int argc, char **argv)
{
    const char *script = nullptr, *frames = nullptr;
    bool quiet = false, watch = false;
    double speed = 1;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        bool has_value = i + 1 < argc;
        if (arg == "--script" && has_value) {
            script = argv[++i];
        } else if (arg == "--frames" && has_value) {
            frames = argv[++i];
        } else if (arg == "--speed" && has_value) {
            speed = atof(argv[++i]);
        } else if (arg == "--quiet") {
            quiet = true;
        } else if (arg == "--watch") {
            watch = true;
        } else {
            usage_error();
        }
    }

    if (!script) {
        if (argc > 1) {
            usage_error();
        }
        start_live();
        app_main(); // never returns: q exits
    }

    g_virtual_clock = true;
    if (frames && !record_frames(frames)) {
        fprintf(stderr, "emulator: can't write %s\n", frames);
        return 2;
    }
    if (watch) {
        if (speed <= 0) {
            usage_error();
        }
        terminal_start("watching a script   q: quit");
        set_log_output(LogOutput::Terminal);
        pace_virtual_clock(speed);
        std::thread([]() { read_keys(false); }).detach();
    } else if (quiet) {
        set_log_output(LogOutput::None);
    }
    if (strcmp(script, "-") == 0) {
        load_script(std::cin, watch);
    } else {
        std::ifstream file(script);
        if (!file) {
            fprintf(stderr, "emulator: can't open %s\n", script);
            return 2;
        }
        load_script(file, watch);
    }
    app_main(); // never returns: the script exits when it is done
    return 0;
}
