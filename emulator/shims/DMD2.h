#pragma once

// Emulator stand-in for DMD2 (components/dmd2/DMD2): just the parts
// main/hub12.h and main/padel-scoreboard.cpp use. A frame is a plain pixel
// array, and copying a frame onto the panel shows it in the emulator. The
// real library's scan code never runs (see shims/esp_timer.h).

#include <stddef.h>
#include <vector>

#include "Arduino.h"
#include "emulator.h"

enum DMDGraphicsMode {
    GRAPHICS_OFF,
    GRAPHICS_ON,
};

class DMDFrame {
public:
    DMDFrame(byte pixelsWide, byte pixelsHigh)
        : width(pixelsWide), height(pixelsHigh), pixels_(pixelsWide * pixelsHigh, false),
          bitmap_bytes_((pixelsWide + 7) / 8 * pixelsHigh)
    {
        bitmap = bitmap_bytes_.data();
    }
    virtual ~DMDFrame() = default;

    void setPixel(unsigned int x, unsigned int y, DMDGraphicsMode mode = GRAPHICS_ON)
    {
        if (x < width && y < height) {
            pixels_[y * width + x] = mode == GRAPHICS_ON;
        }
    }

    bool getPixel(unsigned int x, unsigned int y) const { return x < width && y < height && pixels_[y * width + x]; }

    void copyFrame(DMDFrame &from, unsigned int left, unsigned int top)
    {
        for (unsigned int y = 0; y < from.height; y++) {
            for (unsigned int x = 0; x < from.width; x++) {
                setPixel(left + x, top + y, from.getPixel(x, y) ? GRAPHICS_ON : GRAPHICS_OFF);
            }
        }
    }

    void clearScreen() { pixels_.assign(pixels_.size(), false); }

    const byte width, height;

protected:
    // Hub12::scanDisplay() reads rows out of this; here it is never called.
    volatile uint8_t *bitmap;
    size_t unified_width_bytes() const { return (width + 7) / 8; }

private:
    std::vector<bool> pixels_;
    std::vector<uint8_t> bitmap_bytes_;
};

class BaseDMD : public DMDFrame {
public:
    virtual void scanDisplay() {}

protected:
    BaseDMD(byte panelsWide, byte panelsHigh) : DMDFrame(panelsWide * 32, panelsHigh * 16) {}
    virtual void writeSPIData(volatile uint8_t *rows[4], const int rowsize) = 0;

    volatile byte scan_row = 0;
};

class SoftDMD : public BaseDMD {
public:
    SoftDMD(byte panelsWide, byte panelsHigh, byte /*pin_noe*/, byte /*pin_a*/, byte /*pin_b*/, byte /*pin_sck*/,
            byte /*pin_clk*/, byte /*pin_r_data*/)
        : BaseDMD(panelsWide, panelsHigh)
    {
    }

    // On the board this lands in the bitmap the scan timer shifts out; here
    // the panel shows it straight away.
    void copyFrame(DMDFrame &from, unsigned int left, unsigned int top)
    {
        DMDFrame::copyFrame(from, left, top);
        emu_show_panel(*this);
    }

protected:
    void writeSPIData(volatile uint8_t *[4], const int) override {}
};
