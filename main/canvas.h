#pragma once

// A 32x16 one-bit frame with the simulator's drawing semantics (design/
// p10-sim.html): x 0..31 left to right, y 0..15 top to bottom, an optional
// clip rectangle, and off-panel writes ignored. No hardware dependencies, so
// the animation code builds on the host (see test/).

#include <math.h>
#include <stdint.h>

struct FixedFrame {
    uint16_t ms;       // how long this frame shows
    uint32_t rows[16]; // bit 31 = column 0, set = lit
};

// JavaScript's Math.round (halves go up), which the reference code uses for
// every pixel position.
inline int jsround(double v)
{
    return (int)floor(v + 0.5);
}

struct Canvas {
    static constexpr int W = 32, H = 16;

    uint32_t rows[H] = {};
    int cx0 = 0, cx1 = W - 1, cy0 = 0, cy1 = H - 1; // clip, inclusive

    void clear()
    {
        for (auto &r : rows) {
            r = 0;
        }
        noClip();
    }

    void set(int x, int y, bool on = true)
    {
        if (x < cx0 || x > cx1 || y < cy0 || y > cy1 || x < 0 || x >= W || y < 0 || y >= H) {
            return;
        }
        uint32_t bit = 0x80000000u >> x;
        if (on) {
            rows[y] |= bit;
        } else {
            rows[y] &= ~bit;
        }
    }

    // For positions computed as floats (fireworks), rounded like the reference.
    void setf(double x, double y, bool on = true) { set(jsround(x), jsround(y), on); }

    bool get(int x, int y) const
    {
        return x >= 0 && x < W && y >= 0 && y < H && (rows[y] & (0x80000000u >> x));
    }

    // Clears pixel i of the row-major buffer (y * 32 + x), ignoring the clip,
    // like the reference's direct buf[] writes.
    void clearIndex(int i) { rows[i / W] &= ~(0x80000000u >> (i % W)); }

    // Nested clips intersect; the reference replaces the clip, but no clip in
    // the design ever has to widen an outer one.
    struct ClipState {
        int x0, x1, y0, y1;
    };
    ClipState pushClip(int x0, int x1, int y0, int y1)
    {
        ClipState old = {cx0, cx1, cy0, cy1};
        cx0 = x0 > cx0 ? x0 : cx0;
        cx1 = x1 < cx1 ? x1 : cx1;
        cy0 = y0 > cy0 ? y0 : cy0;
        cy1 = y1 < cy1 ? y1 : cy1;
        return old;
    }
    void popClip(const ClipState &s)
    {
        cx0 = s.x0;
        cx1 = s.x1;
        cy0 = s.y0;
        cy1 = s.y1;
    }
    void noClip() { popClip({0, W - 1, 0, H - 1}); }
};
