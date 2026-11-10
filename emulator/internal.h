#pragma once

// What the emulator's own parts share: the core (emulator.cpp), the
// terminal display (terminal.cpp) and the script runner (script.cpp).

#include <stdint.h>

#include <functional>
#include <istream>
#include <string>

constexpr int PANEL_W = 32, PANEL_H = 16;
constexpr int64_t NEVER = INT64_MAX;
constexpr int64_t TAP_US = 150000; // a quick press of a remote button

// ---------------------------------------------------------------------------
// Core (emulator.cpp)

// Time runs on a virtual clock for scripts, real time otherwise.
extern bool g_virtual_clock;

// Runs `action` when the clock reaches at_us. Safe from any thread.
void schedule(int64_t at_us, std::function<void()> action);

// Virtual clock only: called when the firmware waits for input with nothing
// else due (the board is idle). Returning true means the caller moved
// things on (pressed something or scheduled an action).
void on_idle(std::function<bool()> handler);

// Virtual clock only: play back at `speed` times real time instead of as
// fast as possible.
void pace_virtual_clock(double speed);

// Called with each log line, firmware's and the emulator's, as it happens.
void on_log(std::function<void(const std::string &line)> handler);

// Where the log goes: stdout, the terminal, or nowhere (it is still passed
// to on_log handlers).
enum class LogOutput { Stdout, Terminal, None };
void set_log_output(LogOutput output);

// Writes every frame shown on the panel to `path`: "<microseconds> <hex>"
// per line, the hex 16 rows of 8 digits, MSB = leftmost pixel.
bool record_frames(const char *path);

bool panel_pixel(int x, int y);
void print_line(const char *line);   // to the log output, bypassing on_log
void print_output(const char *line); // a script's own output: like print_line, but never hidden

// Presses remote `key` ('a', 'b' or 'x') at at_us for hold_us.
void press_remote(char key, int64_t at_us, int64_t hold_us);

// A remote is sending, or the receiver still holds a press it hasn't
// released.
bool radio_busy();

// ---------------------------------------------------------------------------
// Terminal (terminal.cpp)

void terminal_start(const char *help);
void terminal_stop();
void terminal_draw_panel();
void terminal_log(const char *line);
[[noreturn]] void read_keys(bool remotes); // remotes = false: only q quits

// ---------------------------------------------------------------------------
// Script runner (script.cpp)

// Reads the script and schedules its first step at time 0. Exits with a
// message on a syntax error. `watching`: it is being shown in the terminal.
void load_script(std::istream &in, bool watching);
