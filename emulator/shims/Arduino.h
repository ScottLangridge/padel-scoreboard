#pragma once

// Emulator stand-in for arduino-esp32: the pin calls main/hub12.h makes, as
// no-ops.

#include <stdint.h>

typedef uint8_t byte;

#define LOW 0
#define HIGH 1
#define OUTPUT 0x03

inline void initArduino() {}
inline void pinMode(int, int) {}
inline void digitalWrite(int, int) {}
