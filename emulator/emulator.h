#pragma once

// What the stand-in headers in shims/ need from the emulator (emulator.cpp).

#include <stdint.h>

class DMDFrame;

// Microseconds since boot: real time when live, the virtual clock when
// running a script.
int64_t emu_now_us();

// A frame has been copied onto the panel.
void emu_show_panel(const DMDFrame &panel);

// An ESP_LOGx line from the firmware.
void emu_log(char level, const char *tag, const char *format, ...) __attribute__((format(printf, 3, 4)));
