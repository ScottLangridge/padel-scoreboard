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
inline int jsround(double value)
{
    return (int)floor(value + 0.5);
}

struct Canvas {
    static constexpr int W = 32, H = 16;

    uint32_t rows[H] = {};
    int clipLeft = 0, clipRight = W - 1, clipTop = 0, clipBottom = H - 1; // clip, inclusive

    void clear()
    {
        for (auto &row : rows) {
            row = 0;
        }
        noClip();
    }

    void set(int x, int y, bool on = true)
    {
        if (x < clipLeft || x > clipRight || y < clipTop || y > clipBottom || x < 0 || x >= W || y < 0 || y >= H) {
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
    void setRounded(double x, double y, bool on = true) { set(jsround(x), jsround(y), on); }

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
        int left, right, top, bottom;
    };
    ClipState pushClip(int x0, int x1, int y0, int y1)
    {
        ClipState old = {clipLeft, clipRight, clipTop, clipBottom};
        clipLeft = x0 > clipLeft ? x0 : clipLeft;
        clipRight = x1 < clipRight ? x1 : clipRight;
        clipTop = y0 > clipTop ? y0 : clipTop;
        clipBottom = y1 < clipBottom ? y1 : clipBottom;
        return old;
    }
    void popClip(const ClipState &saved)
    {
        clipLeft = saved.left;
        clipRight = saved.right;
        clipTop = saved.top;
        clipBottom = saved.bottom;
    }
    void noClip() { popClip({0, W - 1, 0, H - 1}); }
};
