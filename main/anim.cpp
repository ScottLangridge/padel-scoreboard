#include "anim.h"

#include "fonts.h"

#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Easing and timing (SPEC §1)

static double mix(double a, double b, double s)
{
    return a + (b - a) * s;
}

static double ease(double s)
{
    return 1 - pow(1 - s, 3);
}

static double easeBack(double s) // overshoot, for text dropping in
{
    const double c = 1.7;
    return 1 + (c + 1) * pow(s - 1, 3) + c * pow(s - 1, 2);
}

// Progress 0..1 of t through [a, b]. A still-waiting press (a = INFINITY)
// counts as not started.
static double clampT(double t, double a, double b)
{
    if (!(t > a)) {
        return 0;
    }
    if (t >= b) {
        return 1;
    }
    return (t - a) / (b - a);
}

static double ez(double t, double a, double b)
{
    return ease(clampT(t, a, b));
}

// ---------------------------------------------------------------------------
// Fonts

static void drawText(Canvas &c, const Font &f, const char *s, int x, int y, bool on = true)
{
    for (; *s; s++, x += f.advance) {
        unsigned code = (unsigned char)*s;
        const uint8_t *g = f.glyphs[code >= 32 && code < 128 ? code - 32 : 0];
        for (int j = 0; j < f.height; j++) {
            for (int i = 0; i < f.width; i++) {
                if (g[j] & (1 << (f.width - 1 - i))) {
                    c.set(x + i, y + j, on);
                }
            }
        }
    }
}

enum Face { F5, F3, F4 }; // 5x7, 3x5, 4x7 condensed

static const Font &font(Face f)
{
    return f == F5 ? FONT_5X7 : f == F3 ? FONT_3X5 : FONT_4X7;
}

static void text(Canvas &c, const char *s, int x, int y, bool on = true)
{
    drawText(c, FONT_5X7, s, x, y, on);
}
static void text3(Canvas &c, const char *s, int x, int y, bool on = true)
{
    drawText(c, FONT_3X5, s, x, y, on);
}
static void text4(Canvas &c, const char *s, int x, int y, bool on = true)
{
    drawText(c, FONT_4X7, s, x, y, on);
}

static int textWidth(const char *s) // 5x7, including the trailing gap
{
    return (int)strlen(s) * 6;
}

// Centring: x = round((32 - (width - 1)) / 2)
static int wordX(const char *w)
{
    return jsround((32 - (textWidth(w) - 1)) / 2.0);
}
static int wordX4(const char *w)
{
    return jsround((32 - ((int)strlen(w) * 5 - 1)) / 2.0);
}

// ---------------------------------------------------------------------------
// Board elements (SPEC §2)

static constexpr int L_EDGE = 13, R_EDGE = 19; // points: left ends at 13, right starts at 19
static constexpr int GL = 6, GR = 23, GY = 10; // games digit origins
static constexpr int PY = 0;                   // points row
static constexpr int GOLDEN_X = 2;             // GOLDEN is 29 wide: cols 2-30

static const char *const PTS[] = {"0", "15", "30", "40"};

// The text shown for one side's points.
static const char *ptsText(const Board &s, int side, char buf[4])
{
    if (s.tiebreak) {
        snprintf(buf, 4, "%d", s.pts[side]);
        return buf;
    }
    return PTS[s.pts[side] & 3];
}

static int lx(const char *s) // right-align the left number against L_EDGE
{
    return L_EDGE - (textWidth(s) - 1);
}

static void netLine(Canvas &c)
{
    for (int x = 0; x < 32; x++) {
        c.set(x, 8);
    }
}

static void dash(Canvas &c, int dx = 0)
{
    for (int x = 14; x <= 17; x++) {
        c.set(x + dx, PY + 3);
    }
}

// Both points numbers, each shifted by its own offset; the dash only when
// neither has moved.
static void points(Canvas &c, const char *l, const char *r, int dl = 0, int dr = 0)
{
    text(c, l, lx(l) + dl, PY);
    text(c, r, R_EDGE + dr, PY);
    if (dl == 0 && dr == 0) {
        dash(c);
    }
}

// The whole points row of a board shifted by dx (and drawn at row y): GOLDEN,
// or the numbers and the dash.
static void pointsRow(Canvas &c, const Board &s, int dx = 0, int y = PY)
{
    if (s.golden()) {
        text4(c, "GOLDEN", GOLDEN_X + dx, y);
        return;
    }
    char bl[4], br[4];
    const char *l = ptsText(s, 0, bl), *r = ptsText(s, 1, br);
    text(c, l, lx(l) + dx, y);
    text(c, r, R_EDGE + dx, y);
    for (int x = 14; x <= 17; x++) {
        c.set(x + dx, y + 3);
    }
}

static void games(Canvas &c, const Board &s)
{
    char b[4];
    snprintf(b, sizeof b, "%d", s.games[0]);
    text3(c, b, GL, GY);
    snprintf(b, sizeof b, "%d", s.games[1]);
    text3(c, b, GR, GY);
}

static void setPosts(Canvas &c)
{
    for (int y = 9; y < 16; y++) {
        c.set(2, y);
        c.set(29, y);
    }
}

// Set cell i (0 = first set won, lower; 1 = second, upper) against edge x0,
// grown to `rows` of its 4 rows from the bottom.
static void setCell(Canvas &c, int x0, int i, int rows = 4)
{
    int bottom = i ? 12 : 16;
    for (int y = bottom - rows; y < bottom; y++) {
        c.set(x0, y);
        c.set(x0 + 1, y);
    }
}

static void setsTally(Canvas &c, int l, int r, int rows = 4)
{
    setPosts(c);
    for (int i = 0; i < l; i++) {
        setCell(c, 0, i, rows);
    }
    for (int i = 0; i < r; i++) {
        setCell(c, 30, i, rows);
    }
}

// Serve triangle (§2.1): right angle toward the corner the serve comes from.
static void serve(Canvas &c, int side, int box, int size = 4)
{
    int x0 = side ? 18 : 13, y0 = box ? 14 : 10;
    for (int j = 0; j < size; j++) {
        for (int i = 0; i + j < size; i++) {
            c.set(x0 + (side ? -i : i), y0 + (box ? -j : j));
        }
    }
}

// The triangle shrinks away over the first half of p and regrows at its new
// place over the second.
static void serveFlip(Canvas &c, const Board &a, const Board &b, double p)
{
    if (p <= 0) {
        serve(c, a.sv, a.bx);
    } else if (p >= 1) {
        serve(c, b.sv, b.bx);
    } else if (p < 0.5) {
        int n = jsround(4 * (1 - 2 * p));
        if (n) {
            serve(c, a.sv, a.bx, n);
        }
    } else {
        int n = jsround(4 * (2 * p - 1));
        if (n) {
            serve(c, b.sv, b.bx, n);
        }
    }
}

void drawBoard(Canvas &c, const Board &s)
{
    netLine(c);
    pointsRow(c, s);
    games(c, s);
    setsTally(c, s.sets[0], s.sets[1]);
    serve(c, s.sv, s.bx);
}

// ---------------------------------------------------------------------------
// Shared building blocks (SPEC §5)

// Odometer roll inside a window the height of the face: the old value moves
// out by height+1 rows and the new one follows it in. dir +1 = new comes up
// from below. drawOld/drawNew take the y to draw at.
template <class DrawOld, class DrawNew>
static void rollRow(Canvas &c, int y, int h, double p, int dir, DrawOld drawOld, DrawNew drawNew)
{
    int off = jsround(ease(p) * (h + 1)) * dir;
    auto clip = c.pushClip(0, 31, y, y + h - 1);
    drawOld(y - off);
    drawNew(y + (h + 1) * dir - off);
    c.popClip(clip);
}

// Roll one string to another; xOf gives the x for a string.
template <class XOf>
static void roll(Canvas &c, const char *oldS, const char *newS, XOf xOf, int y, double p, int dir, Face face = F5)
{
    const Font &f = font(face);
    rollRow(
        c, y, f.height, p, dir, [&](int yy) { drawText(c, f, oldS, xOf(oldS), yy); },
        [&](int yy) { drawText(c, f, newS, xOf(newS), yy); });
}

// The winning ball: 4 pixels along the lane above the line, head leading,
// with a shadow in the line beneath the head. dir +1 = left to right. It
// flies toward the side that won the point.
static void winBall(Canvas &c, double t, double t0, int dir)
{
    double p = clampT(t, t0, t0 + 0.35);
    if (p >= 1) {
        return;
    }
    double head = dir > 0 ? mix(-1, 34, p) : mix(32, -3, p);
    for (int k = 0; k < 4; k++) {
        int x = jsround(head - k * dir * 1.2);
        c.set(x, 7);
        if (k == 0) {
            c.set(x, 8, false);
            c.set(x - dir, 8, false);
        }
    }
}

// Arrowhead pointing in dir with its tip at x, rows 10-14; nudge shifts it
// one pixel in dir.
static void tri(Canvas &c, int x, int dir, int nudge)
{
    for (int i = 0; i < 3; i++) {
        for (int y = 12 - i; y <= 12 + i; y++) {
            c.set(x + (nudge - i) * dir, y);
        }
    }
}

// Prompt text alternating A, roll, B, roll back.
static void altWords(Canvas &c, const char *a, const char *b, double t, int y, double hold, double rl, Face face = F5)
{
    double ph = fmod(t, 2 * (hold + rl)); // negative before the prompt starts: shows A
    auto xo = [face](const char *w) { return face == F4 ? wordX4(w) : wordX(w); };
    const Font &f = font(face);
    if (ph < hold) {
        drawText(c, f, a, xo(a), y);
    } else if (ph < hold + rl) {
        roll(c, a, b, xo, y, (ph - hold) / rl, 1, face);
    } else if (ph < 2 * hold + rl) {
        drawText(c, f, b, xo(b), y);
    } else {
        roll(c, b, a, xo, y, (ph - 2 * hold - rl) / rl, 1, face);
    }
}

// A lit bar sweeps across in dir over 0.6 s from t0: `before` is still drawn
// ahead of it, `after` is revealed behind it.
template <class Before, class After>
static void barWipe(Canvas &c, double t, double t0, int dir, Before before, After after)
{
    int lead = jsround(ez(t, t0, t0 + 0.3) * 32), trail = jsround(ez(t, t0 + 0.25, t0 + 0.6) * 32);
    auto col = [dir](int k) { return dir > 0 ? k : 31 - k; };
    before();
    for (int k = trail; k < lead; k++) {
        for (int y = 0; y < 16; y++) {
            c.set(col(k), y);
        }
    }
    if (trail > 0) {
        auto clip = dir > 0 ? c.pushClip(0, trail - 1, 0, 15) : c.pushClip(32 - trail, 31, 0, 15);
        for (int k = 0; k < trail; k++) {
            for (int y = 0; y < 16; y++) {
                c.set(col(k), y, false);
            }
        }
        after();
        c.popClip(clip);
    }
}

// Fixed pseudo-random pixel order for the dissolves: a Fisher-Yates shuffle
// driven by the reference's LCG, reproducing its double-precision arithmetic
// (the product overflows 53 bits, so the rounding is part of the sequence).
static const uint16_t *perm()
{
    static uint16_t a[512];
    static bool made = false;
    if (!made) {
        made = true;
        uint32_t s = 7;
        auto r = [&s]() {
            volatile double prod = (double)s * 1103515245.0; // volatile: no fused multiply-add
            double v = prod + 12345.0;
            s = (uint32_t)(int64_t)v & 0x7fffffff;
            return s / 2147483647.0;
        };
        for (int i = 0; i < 512; i++) {
            a[i] = i;
        }
        for (int i = 511; i > 0; i--) {
            int j = (int)floor(r() * (i + 1));
            if (j > i) {
                j = i;
            }
            uint16_t tmp = a[i];
            a[i] = a[j];
            a[j] = tmp;
        }
    }
    return a;
}

static void dissolve(Canvas &c, int n)
{
    const uint16_t *p = perm();
    for (int i = 0; i < n; i++) {
        c.clearIndex(p[i]);
    }
}

// ---------------------------------------------------------------------------
// Point / Golden / Undo (SPEC §5.4, §5.6)

// The common first 0.75 s: ball, the scorer's points roll up, serve flip.
// Draws the dash as well unless told not to (Golden grows its own).
static void pointBody(Canvas &c, double t, const Board &a, const Board &b, int side, bool withDash)
{
    netLine(c);
    games(c, a);
    setsTally(c, a.sets[0], a.sets[1]);
    serveFlip(c, a, b, clampT(t, 0.5, 0.75));
    double p = clampT(t, 0.3, 0.7);
    if (withDash) {
        dash(c);
    }
    char ba[4], bo[4], bn[4];
    const char *still = ptsText(a, 1 - side, ba);
    const char *oldS = ptsText(a, side, bo), *newS = ptsText(b, side, bn);
    if (side) {
        text(c, still, lx(still), PY);
        roll(c, oldS, newS, [](const char *) { return R_EDGE; }, PY, p, 1);
    } else {
        text(c, still, R_EDGE, PY);
        roll(c, oldS, newS, lx, PY, p, 1);
    }
    winBall(c, t, 0, side ? 1 : -1);
}

void animPoint(Canvas &c, double t, const Board &a, const Board &b, int side)
{
    pointBody(c, t, a, b, side, true);
}

void animGolden(Canvas &c, double t, const Board &a, const Board &b, int side)
{
    const double T1 = 0.75, T2 = 1.0, T3 = 1.45; // dash becomes a bar; bars sweep out; done
    if (t < T2) {
        // The point lands; the dash stands up into a vertical bar.
        pointBody(c, t, a, b, side, false);
        double p = ez(t, T1, T2);
        int hw = jsround(mix(2, 1, p)), hh = jsround(mix(0, 3, p));
        for (int y = PY + 3 - hh; y <= PY + 3 + hh; y++) {
            for (int x = 16 - hw; x <= 15 + hw; x++) {
                c.set(x, y);
            }
        }
        return;
    }
    netLine(c);
    games(c, a);
    setsTally(c, a.sets[0], a.sets[1]);
    serve(c, b.sv, b.bx);
    if (t < T3) {
        // Twin bars sweep to the edges: 40-40 ahead of them, GOLDEN behind.
        int d = jsround(ez(t, T2, T3) * 16);
        char bl[4], br[4];
        const char *l = ptsText(b, 0, bl), *r = ptsText(b, 1, br);
        auto clip = c.pushClip(0, 13 - d, PY, PY + 6);
        text(c, l, lx(l), PY);
        c.popClip(clip);
        clip = c.pushClip(18 + d, 31, PY, PY + 6);
        text(c, r, R_EDGE, PY);
        c.popClip(clip);
        clip = c.pushClip(15 - d, 16 + d, PY, PY + 6);
        text4(c, "GOLDEN", GOLDEN_X, PY);
        c.popClip(clip);
        for (int y = PY; y <= PY + 6; y++) {
            c.set(15 - d, y);
            c.set(16 + d, y);
        }
        return;
    }
    text4(c, "GOLDEN", GOLDEN_X, PY);
    if (t >= 1.6 && t < 2.2) { // one glint: a dark diagonal streak across the letters
        double g = ez(t, 1.6, 2.2) * 44 - 6;
        for (int y = PY; y <= PY + 6; y++) {
            c.set(jsround(g - y), y, false);
            c.set(jsround(g - y) + 1, y, false);
        }
    }
}

void animUndo(Canvas &c, double t, const Board &a, const Board &b)
{
    netLine(c);
    games(c, a);
    setsTally(c, a.sets[0], a.sets[1]);
    serveFlip(c, a, b, clampT(t, 0.5, 0.75));
    if (t < 0.75) { // the line runs backwards: dark gaps marching right to left
        int ph = (int)floor(t * 40);
        for (int x = 0; x < 32; x++) {
            if ((x + ph) % 6 < 3) {
                c.set(x, 8, false);
            }
        }
    }
    int tug = t > 0.05 && t < 0.3 ? -1 : 0; // the whole row is pulled back for a moment
    double p = clampT(t, 0.3, 0.7);
    if (a.golden() || b.golden()) {
        // GOLDEN can't roll one side at a time: the whole row rolls down.
        rollRow(
            c, PY, 7, p, -1, [&](int y) { pointsRow(c, a, tug, y); }, [&](int y) { pointsRow(c, b, tug, y); });
        return;
    }
    dash(c, tug);
    char ba[4], bb[4];
    for (int side = 0; side < 2; side++) {
        const char *oldS = ptsText(a, side, ba), *newS = ptsText(b, side, bb);
        auto xOf = [side, tug](const char *s) { return (side ? R_EDGE : lx(s)) + tug; };
        if (strcmp(oldS, newS) == 0) {
            text(c, oldS, xOf(oldS), PY);
        } else {
            roll(c, oldS, newS, xOf, PY, p, -1);
        }
    }
}

// ---------------------------------------------------------------------------
// Game / Set / Match (SPEC §5.5)

void animGame(Canvas &c, double t, const Board &a, const Board &b, int winner)
{
    int loser = 1 - winner;
    netLine(c);
    setsTally(c, a.sets[0], a.sets[1]);
    serveFlip(c, a, b, clampT(t, 2.05, 2.3));

    // Games row: the winner's digit rolls up with a sparkle at its corners.
    int gxw = winner ? GR : GL, gxl = loser ? GR : GL;
    if (t < 1.5) {
        games(c, a);
    } else {
        char bl[4], bo[4], bn[4];
        snprintf(bl, sizeof bl, "%d", a.games[loser]);
        snprintf(bo, sizeof bo, "%d", a.games[winner]);
        snprintf(bn, sizeof bn, "%d", b.games[winner]);
        text3(c, bl, gxl, GY);
        roll(c, bo, bn, [gxw](const char *) { return gxw; }, GY, clampT(t, 1.5, 1.9), 1, F3);
    }
    if (t >= 1.5 && t < 1.7) {
        c.set(gxw - 1, GY - 1);
        c.set(gxw + 3, GY - 1);
        c.set(gxw - 1, GY + 5);
        c.set(gxw + 3, GY + 5);
    }

    // Points row: the numbers slide off to their own edges...
    if (t < 0.7) {
        int s = jsround(ez(t, 0.35, 0.7) * 16);
        if (a.golden()) { // GOL slides left, DEN slides right
            auto clip = c.pushClip(0, 15, PY, PY + 6);
            text4(c, "GOL", GOLDEN_X - s, PY);
            c.popClip(clip);
            clip = c.pushClip(16, 31, PY, PY + 6);
            text4(c, "DEN", GOLDEN_X + 15 + s, PY);
            c.popClip(clip);
        } else {
            char bl[4], br[4];
            points(c, ptsText(a, 0, bl), ptsText(a, 1, br), -s, s);
        }
    }
    // ...GAME drops in letter by letter, then lifts out the top...
    if (t >= 0.7) {
        int up = jsround(ez(t, 2.0, 2.4) * 9);
        const char *word = "GAME";
        for (int i = 0; i < 4; i++) {
            int y = jsround(mix(-8, PY, easeBack(clampT(t, 0.7 + i * 0.15, 1.05 + i * 0.15))));
            char ch[2] = {word[i], 0};
            text(c, ch, 4 + i * 6, y - up);
        }
    }
    // ...while the new points wipe in from the centre outward.
    if (t >= 2.0) {
        int w = jsround(ez(t, 2.05, 2.4) * 14);
        auto clip = c.pushClip(15 - w, 16 + w, PY, PY + 6);
        pointsRow(c, b);
        c.popClip(clip);
    }
    winBall(c, t, 0, winner ? 1 : -1);
}

void animSet(Canvas &c, double t, const Board &a, const Board &b, int winner)
{
    drawBoard(c, t < 2.1 ? a : b);
    int fillTo = jsround(ez(t, 0.35, 0.85) * 32); // columns lit from the winner's edge
    int cut = jsround(ez(t, 2.1, 2.6) * 16);      // rows drained from the top
    if (t >= 0.35) {
        for (int y = cut; y < 16; y++) {
            for (int k = 0; k < fillTo; k++) {
                c.set(winner ? 31 - k : k, y);
            }
        }
        auto clip = c.pushClip(0, 31, cut, 15);
        const char *word = "SET";
        for (int i = 0; i < 3; i++) {
            if (t >= 0.85 + i * 0.15) {
                char ch[2] = {word[i], 0};
                text(c, ch, 7 + i * 6, 1, false);
            }
        }
        if (t >= 1.4) { // the set's games, left-right
            char sc[8];
            snprintf(sc, sizeof sc, "%d-%d", a.games[0] + (winner == 0), a.games[1] + (winner == 1));
            text3(c, sc, 10, 9, false);
        }
        c.popClip(clip);
    }
    if (t >= 2.6 && t < 3.4 && ((int)floor((t - 2.6) / 0.13)) % 2) { // the new block blinks
        int i = b.sets[winner] - 1;
        int x0 = winner ? 29 : 0;
        for (int y = i ? 8 : 12; y < (i ? 12 : 16); y++) {
            for (int x = x0; x < x0 + 3; x++) {
                c.set(x, y, false);
            }
        }
    }
    winBall(c, t, 0, winner ? 1 : -1);
}

static void firework(Canvas &c, double t, double t0, int x0)
{
    const double rise = 0.45, life = 0.7, ay = 10;
    double d = t - t0;
    if (d < 0 || d > rise + life) {
        return;
    }
    if (d < rise) {
        c.set(x0, jsround(mix(15, ay, ease(d / rise))));
        return;
    }
    double b = (d - rise) / life;
    if (b > 0.55 && ((int)floor(b * 24)) % 2) { // twinkle out
        return;
    }
    for (int i = 0; i < 8; i++) {
        double an = i * M_PI / 4 + 0.3, sp = 5.5;
        c.setf(x0 + cos(an) * sp * b, ay + sin(an) * sp * b * 0.8 + 5 * b * b);
    }
}

static const double ROCKETS[][2] = {{1.3, 6}, {1.7, 25}, {2.2, 15}, {2.7, 10}, {3.1, 22}};

void animMatch(Canvas &c, double t, const Board &a, int winner, const char *finalScore)
{
    if (t < 1.0) { // the old board dissolves away
        drawBoard(c, a);
        winBall(c, t, 0, winner ? 1 : -1);
        dissolve(c, jsround(ez(t, 0.35, 1.0) * 512));
        return;
    }
    const char *word = "MATCH";
    for (int i = 0; i < 5; i++) {
        int y = jsround(mix(-8, 0, easeBack(clampT(t, 1.0 + i * 0.12, 1.4 + i * 0.12))));
        char ch[2] = {word[i], 0};
        text(c, ch, 1 + i * 6, y);
    }
    for (const auto &r : ROCKETS) {
        firework(c, t, r[0], (int)r[1]);
    }
    if (t >= 4.0) {
        int width = (int)strlen(finalScore) * 4 - 1;
        if (width <= 32) { // wipes in from the centre and stays
            int w = jsround(ez(t, 4.0, 4.4) * 16);
            auto clip = c.pushClip(16 - w, 15 + w, 9, 13);
            text3(c, finalScore, (32 - width) / 2, 9);
            c.popClip(clip);
        } else { // three sets don't fit: keep scrolling it through
            double x = 32 - fmod((t - 4.0) * 25, 32.0 + width);
            text3(c, finalScore, jsround(x), 9);
        }
    }
}

// ---------------------------------------------------------------------------
// Swap ends (SPEC §5.7)

void animSwap(Canvas &c, double t, const Board &a, const Board &b, double pressT)
{
    const double EMERGE = pressT + 0.3;
    netLine(c);
    double in = pow(clampT(t, 0, 0.5), 2), outp = ez(t, EMERGE, EMERGE + 0.6); // accelerate in, decelerate out
    const Board &s = t < EMERGE ? a : b;
    int k = t < EMERGE ? jsround(in * 12) : jsround((1 - outp) * 12);
    if (t < 0.5 || t >= EMERGE) { // each half slides into / out of the centre
        char bl[4], br[4], gl[4], gr[4];
        const char *l = ptsText(s, 0, bl), *r = ptsText(s, 1, br);
        snprintf(gl, sizeof gl, "%d", s.games[0]);
        snprintf(gr, sizeof gr, "%d", s.games[1]);
        dash(c);
        auto clip = c.pushClip(0, 13, 0, 15);
        text(c, l, lx(l) + k, PY);
        text3(c, gl, GL + k, GY);
        c.popClip(clip);
        clip = c.pushClip(18, 31, 0, 15);
        text(c, r, R_EDGE - k, PY);
        text3(c, gr, GR - k, GY);
        c.popClip(clip);
    }
    double h = t < EMERGE ? 1 - in : outp; // set cells shrink, then regrow
    setsTally(c, s.sets[0], s.sets[1], jsround(4 * h));
    if (t >= 0.5 && t < EMERGE) { // the prompt, rolling away once acknowledged
        int up = jsround(ez(t, pressT, EMERGE) * 8);
        auto clip = c.pushClip(0, 31, PY, PY + 6);
        altWords(c, "SWAP", "ENDS", t - 0.5, PY - up, 0.6, 0.15);
        c.popClip(clip);
        double ph = fmod(t - 0.5, 1.5); // arrowheads walk in from each edge and cross
        if (t < pressT && ph < 1.1) {
            int d = jsround(ease(ph / 1.1) * 23);
            tri(c, 5 + d, 1, 0);
            tri(c, 26 - d, -1, 0);
        }
    }
    if (t < 0.5) {
        serve(c, a.sv, a.bx);
    } else if (t >= EMERGE) {
        serve(c, b.sv, b.bx);
    }
}

// ---------------------------------------------------------------------------
// Bind / Select serve (SPEC §5.2, §5.3)

// The Select Serve waiting state, also revealed at the end of Bind.
static void servePrompt(Canvas &c, double t, bool arrows)
{
    netLine(c);
    altWords(c, "SELECT", "SERVE", t, 0, 0.9, 0.2, F4);
    if (arrows) {
        int n = ((int)floor(t / 0.5)) % 2;
        tri(c, 4, -1, n);
        tri(c, 27, 1, n);
    }
}

void animBind(Canvas &c, double t, double pressL, double pressR)
{
    int n = ((int)floor(t / 0.5)) % 2;
    auto left = [&]() {
        netLine(c);
        altWords(c, "PRESS", "LEFT", t, 0, 0.9, 0.2);
        tri(c, 4, -1, n);
    };
    auto right = [&]() {
        netLine(c);
        altWords(c, "PRESS", "RIGHT", t - pressL - 0.6, 0, 0.9, 0.2);
        tri(c, 27, 1, n);
    };
    auto done = [&]() { servePrompt(c, t - pressR - 0.6, true); }; // its clock starts when the wipe ends
    if (t < pressL) {
        left();
    } else if (t < pressL + 0.6) {
        barWipe(c, t, pressL, 1, left, right);
    } else if (t < pressR) {
        right();
    } else {
        barWipe(c, t, pressR, -1, right, done);
    }
}

void animServe(Canvas &c, double t, double pressT, int side, int box)
{
    const double GLIDE = pressT + 0.5, MORPH = GLIDE + 0.3, DONE = MORPH + 0.6;
    int dir = side ? 1 : -1;   // the arrowhead points outward
    int tip0 = side ? 27 : 4;  // at the edge...
    int tip1 = side ? 18 : 13; // ...glides to the serve triangle's corner
    if (t < pressT) {
        servePrompt(c, t, true);
        return;
    }
    netLine(c);
    int up = jsround(ez(t, pressT, pressT + 0.3) * 8); // the prompt rolls up and out
    if (up < 8) {
        auto clip = c.pushClip(0, 31, PY, PY + 6);
        altWords(c, "SELECT", "SERVE", t, PY - up, 0.9, 0.2, F4);
        c.popClip(clip);
    }
    if (t < GLIDE) {
        tri(c, jsround(mix(tip0, tip1, ez(t, pressT, GLIDE))), dir, 0);
    } else if (t < MORPH) { // folds to one column, regrows as the triangle
        double p = clampT(t, GLIDE, MORPH);
        if (p < 0.5) {
            int k = jsround(2 * (1 - 2 * p));
            for (int i = 0; i <= k; i++) {
                for (int y = 12 - i; y <= 12 + i; y++) {
                    c.set(tip1 - i * dir, y);
                }
            }
        } else {
            int n = jsround(4 * (2 * p - 1));
            serve(c, side, box, n < 1 ? 1 : n);
        }
    } else { // the rest of the board emerges from the centre around the triangle
        serve(c, side, box);
        double o = ez(t, MORPH, DONE);
        int k = jsround((1 - o) * 12);
        dash(c);
        auto clip = c.pushClip(0, 13, 0, 15);
        text(c, "0", lx("0") + k, PY);
        text3(c, "0", GL + k, GY);
        c.popClip(clip);
        clip = c.pushClip(18, 31, 0, 15);
        text(c, "0", R_EDGE - k, PY);
        text3(c, "0", GR - k, GY);
        c.popClip(clip);
        int rows = jsround(o * 7); // the set posts grow down from the line
        for (int y = 9; y < 9 + rows; y++) {
            c.set(2, y);
            c.set(29, y);
        }
    }
}

void bindWipeIn(Canvas &c, double t)
{
    int h = jsround(ease(t < BIND_WIPE_IN_S ? t / BIND_WIPE_IN_S : 1) * 8);
    for (int y = 0; y < 16; y++) {
        if (y != 8 && (y > 8 ? y - 8 : 8 - y) > h) {
            c.rows[y] = 0;
        }
    }
}

// ---------------------------------------------------------------------------
// Fixed clips

bool drawFixed(Canvas &c, const FixedFrame *frames, int count, double t)
{
    // Walk by source frames rather than ms so frame boundaries fall exactly
    // on 30 fps ticks.
    int target = (int)floor(t * 30 + 1e-9);
    int i = 0, start = 0;
    while (i < count - 1) {
        int n = jsround(frames[i].ms * 30 / 1000.0);
        if (target < start + n) {
            break;
        }
        start += n;
        i++;
    }
    for (int y = 0; y < 16; y++) {
        c.rows[y] |= frames[i].rows[y];
    }
    int total = start;
    for (int k = i; k < count; k++) {
        total += jsround(frames[k].ms * 30 / 1000.0);
    }
    return target < total;
}
