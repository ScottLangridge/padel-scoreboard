// Host emulator for the scoreboard firmware: runs the real screen flow
// (main/padel-scoreboard.cpp, built against the stand-in headers in shims/)
// with the panel drawn in the terminal and the 433 MHz remotes on the
// keyboard, so changes can be tried without flashing.
//
//   emulator                    live, in the terminal (keys listed on screen)
//   emulator --script <file>    runs a script of presses on a virtual clock
//                               and prints the log and snapshots (- = stdin)
//
// The remotes are emulated at the rx433 component's interface (rx433.h):
// Radio below turns button presses into the PRESS / LONG / RELEASE events
// that rx433 sends, mirroring its press tracking. The radio decoding itself
// (components/rx433/rx433.c) is not emulated.
//
// Script format: one step per line, `#` starts a comment.
//
//   <time> tap <remote>             a short press (0.15 s)
//   <time> hold <remote> <seconds>  a press held that long (>= 1 s: long press)
//   <time> snap                     print what the panel shows
//   <time> end                      stop (default: 3 s after the last step)
//
// <time> is seconds since power on, or +<seconds> after the previous step.
// Remotes are a, b (the two to bind) and x (a stranger's). A trailing xN
// repeats a relative step N times, e.g. `+1.2 tap a x4`.

#include "emulator.h"

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "DMD2.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "rx433.h"
#include "sdkconfig.h"

extern "C" void app_main(void);

namespace {

constexpr int64_t NEVER = INT64_MAX;
constexpr int64_t TICK_US = 1000000 / CONFIG_FREERTOS_HZ;
constexpr int PANEL_W = 32, PANEL_H = 16;

// ---------------------------------------------------------------------------
// Time and scheduled actions
//
// Everything the emulator does on its own (remote frames, script steps) is an
// action scheduled for a time. Live, a thread runs each one when real time
// reaches it. Scripted, there is no second thread: whenever the firmware
// waits, the clock jumps straight to the next action due before its wait
// ends, so a whole match runs in moments and always the same way.

bool g_scripted = false;
std::mutex g_mutex; // guards g_actions, g_virtual_us and every EmuQueue
std::condition_variable g_wake;
std::multimap<int64_t, std::function<void()>> g_actions;
int64_t g_virtual_us = 0;
const auto g_start = std::chrono::steady_clock::now();

int64_t real_us()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - g_start).count();
}

std::chrono::steady_clock::time_point real_time_point(int64_t at_us)
{
    return g_start + std::chrono::microseconds(at_us);
}

void schedule(int64_t at_us, std::function<void()> action)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_actions.emplace(at_us, std::move(action));
    g_wake.notify_all();
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

[[noreturn]] void stop_script(const char *why)
{
    printf("--- %s at %.3f s ---\n", why, g_virtual_us / 1e6);
    fflush(stdout);
    exit(0);
}

// Scripted only: runs the actions due up to `deadline`, moving the virtual
// clock to each, and then to the deadline. Returns early, true, as soon as
// `ready` holds (it is checked with g_mutex held).
template <class Ready>
bool advance_until(int64_t deadline, Ready ready)
{
    while (true) {
        std::function<void()> action;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (ready()) {
                return true;
            }
            if (g_actions.empty() || g_actions.begin()->first > deadline) {
                if (deadline == NEVER) {
                    break;
                }
                g_virtual_us = std::max(g_virtual_us, deadline);
                return false;
            }
            auto next = g_actions.begin();
            g_virtual_us = std::max(g_virtual_us, next->first);
            action = std::move(next->second);
            g_actions.erase(next);
        }
        action();
    }
    stop_script("nothing left to happen");
}

int64_t deadline_after(TickType_t ticks)
{
    return ticks == portMAX_DELAY ? NEVER : emu_now_us() + (int64_t)ticks * TICK_US;
}

// ---------------------------------------------------------------------------
// Output: the log and the panel

std::mutex g_print_mutex;
bool g_panel[PANEL_H][PANEL_W];

class Terminal {
public:
    void start()
    {
        tcgetattr(STDIN_FILENO, &saved_);
        termios raw = saved_;
        raw.c_lflag &= ~(ICANON | ECHO); // keys arrive one at a time, unechoed; Ctrl-C still works
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        winsize size = {};
        ioctl(STDOUT_FILENO, TIOCGWINSZ, &size);
        rows_ = size.ws_row > LOG_TOP ? size.ws_row : 40;
        // Alternate screen, cursor hidden, the log scrolling under the panel.
        printf("\x1b[?1049h\x1b[?25l\x1b[2J\x1b[1;1H");
        printf("padel-scoreboard emulator   a/b: tap remote   A/B: hold remote 1.5 s   x: unknown remote   q: quit");
        printf("\x1b[%d;1H\x1b[2m--- log ---\x1b[0m", LOG_TOP - 1);
        printf("\x1b[%d;%dr", LOG_TOP, rows_);
        fflush(stdout);
        active_ = true;
    }

    void stop()
    {
        if (!active_) {
            return;
        }
        active_ = false;
        printf("\x1b[r\x1b[?25h\x1b[?1049l");
        fflush(stdout);
        tcsetattr(STDIN_FILENO, TCSANOW, &saved_);
    }

    void draw_panel()
    {
        std::string out = "\x1b[3;1H";
        for (int y = 0; y < PANEL_H; y++) {
            out += "  ";
            for (int x = 0; x < PANEL_W; x++) {
                out += g_panel[y][x] ? "\x1b[38;5;196m\xe2\x97\x8f " : "\x1b[38;5;237m\xc2\xb7 "; // lit ●, dark ·
            }
            out += "\x1b[0m\x1b[K\r\n";
        }
        char clock[48];
        snprintf(clock, sizeof clock, "\x1b[2m  t = %.1f s\x1b[0m\x1b[K", emu_now_us() / 1e6);
        out += clock;
        fputs(out.c_str(), stdout);
        fflush(stdout);
    }

    void log_line(const char *line)
    {
        printf("\x1b[%d;1H\r\n%s", rows_, line); // at the bottom of the scroll region, so the log scrolls up
        fflush(stdout);
    }

private:
    static constexpr int LOG_TOP = 22;
    termios saved_ = {};
    int rows_ = 40;
    bool active_ = false;
};

Terminal g_terminal;

void print_line(const char *line)
{
    std::lock_guard<std::mutex> lock(g_print_mutex);
    if (g_scripted) {
        puts(line);
    } else {
        g_terminal.log_line(line);
    }
}

void log_line(char level, const char *tag, const char *text)
{
    char line[320];
    snprintf(line, sizeof line, "%c (%lld) %s: %s", level, (long long)(emu_now_us() / 1000), tag, text);
    print_line(line);
}

void print_snapshot()
{
    std::lock_guard<std::mutex> lock(g_print_mutex);
    printf("snap at %.3f s\n", emu_now_us() / 1e6);
    for (int y = 0; y < PANEL_H; y++) {
        char row[PANEL_W + 1] = {};
        for (int x = 0; x < PANEL_W; x++) {
            row[x] = g_panel[y][x] ? '#' : '.';
        }
        printf("  %s\n", row);
    }
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

    // Schedules a press of remote `key` ('a', 'b' or 'x') held for hold_us.
    // Safe from any thread.
    void press(char key, int64_t at_us, int64_t hold_us)
    {
        schedule(at_us, [this, key, hold_us]() { start_sending(key, hold_us); });
    }

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

constexpr int64_t TAP_US = 150000;
constexpr int64_t LIVE_HOLD_US = 1500000;

// ---------------------------------------------------------------------------
// Live mode

void restore_terminal()
{
    g_terminal.stop();
}

void on_signal(int)
{
    restore_terminal();
    _exit(130);
}

// Keys to presses. Holding a key down also works (its auto-repeat keeps the
// remote sending), if the terminal's repeat delay is under about half a
// second.
void read_keys()
{
    while (true) {
        char key;
        if (read(STDIN_FILENO, &key, 1) != 1 || key == 'q') {
            restore_terminal();
            _exit(0);
        }
        switch (key) {
        case 'a':
        case 'b':
        case 'x':
            g_radio.press(key, emu_now_us(), TAP_US);
            break;
        case 'A':
        case 'B':
            g_radio.press(key - 'A' + 'a', emu_now_us(), LIVE_HOLD_US);
            break;
        }
    }
}

void start_live()
{
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        fprintf(stderr, "emulator: live mode needs a terminal; use --script <file> otherwise\n");
        exit(2);
    }
    g_terminal.start();
    atexit(restore_terminal);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    g_terminal.draw_panel();
    std::thread(run_actions_live).detach();
    std::thread(read_keys).detach();
}

// ---------------------------------------------------------------------------
// Scripted mode

[[noreturn]] void script_error(int line_number, const std::string &line, const char *problem)
{
    fprintf(stderr, "emulator: script line %d: %s\n    %s\n", line_number, problem, line.c_str());
    exit(2);
}

int64_t seconds_to_us(double seconds)
{
    return (int64_t)(seconds * 1e6 + 0.5);
}

void load_script(std::istream &in)
{
    int64_t previous_us = 0, last_us = 0;
    bool ends = false;
    std::string line;
    for (int line_number = 1; std::getline(in, line); line_number++) {
        std::string text = line.substr(0, line.find('#'));
        std::istringstream words(text);
        std::string time_word, command;
        if (!(words >> time_word)) {
            continue; // blank or comment
        }
        if (!(words >> command)) {
            script_error(line_number, line, "expected a command after the time");
        }
        bool relative = time_word[0] == '+';
        char *rest = nullptr;
        double seconds = strtod(time_word.c_str() + (relative ? 1 : 0), &rest);
        if (*rest || seconds < 0) {
            script_error(line_number, line, "bad time");
        }

        std::vector<std::string> args;
        int repeat = 1;
        for (std::string word; words >> word;) {
            if (word.size() > 1 && word[0] == 'x' && isdigit((unsigned char)word[1])) {
                repeat = atoi(word.c_str() + 1);
            } else {
                args.push_back(word);
            }
        }
        if (repeat > 1 && !relative) {
            script_error(line_number, line, "xN needs a relative (+) time");
        }

        for (int i = 0; i < repeat; i++) {
            int64_t at_us = relative ? previous_us + seconds_to_us(seconds) : seconds_to_us(seconds);
            if (at_us < previous_us) {
                script_error(line_number, line, "goes back in time");
            }
            previous_us = last_us = at_us;
            if (command == "tap" || command == "hold") {
                bool hold = command == "hold";
                if (args.size() != (hold ? 2u : 1u) || args[0].size() != 1 || !strchr("abx", args[0][0])) {
                    script_error(line_number, line, hold ? "usage: hold <a|b|x> <seconds>" : "usage: tap <a|b|x>");
                }
                int64_t hold_us = hold ? seconds_to_us(atof(args[1].c_str())) : TAP_US;
                g_radio.press(args[0][0], at_us, hold_us);
                last_us = at_us + hold_us;
            } else if (command == "snap") {
                schedule(at_us, print_snapshot);
            } else if (command == "end") {
                schedule(at_us, []() { stop_script("end"); });
                ends = true;
            } else {
                script_error(line_number, line, "unknown command (tap, hold, snap, end)");
            }
        }
    }
    if (!ends) {
        schedule(last_us + 3000000, []() { stop_script("end"); });
    }
}

} // namespace

// ---------------------------------------------------------------------------
// What the shims call

int64_t emu_now_us()
{
    if (g_scripted) {
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
    if (!g_scripted) {
        std::lock_guard<std::mutex> lock(g_print_mutex);
        g_terminal.draw_panel();
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
    if (g_scripted) {
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
    if (g_scripted) {
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

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "--script") == 0) {
        g_scripted = true;
        if (strcmp(argv[2], "-") == 0) {
            load_script(std::cin);
        } else {
            std::ifstream file(argv[2]);
            if (!file) {
                fprintf(stderr, "emulator: can't open %s\n", argv[2]);
                return 2;
            }
            load_script(file);
        }
    } else if (argc == 1) {
        start_live();
    } else {
        fprintf(stderr, "usage: emulator [--script <file>]\n");
        return 2;
    }
    app_main(); // never returns: the script's end, or q, exits
    return 0;
}
