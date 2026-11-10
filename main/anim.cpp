#include "anim.h"

#include "fonts.h"

#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Easing and timing (SPEC §1)

static double mix(double from, double to, double amount)
{
    return from + (to - from) * amount;
}

static double ease(double progress)
{
    return 1 - pow(1 - progress, 3);
}

static double easeBack(double progress) // overshoot, for text dropping in
{
    const double overshoot = 1.7;
    return 1 + (overshoot + 1) * pow(progress - 1, 3) + overshoot * pow(progress - 1, 2);
}

// Progress 0..1 of t through [start, end]. A still-waiting press
// (start = INFINITY) counts as not started.
static double clampT(double t, double start, double end)
{
    if (!(t > start)) {
        return 0;
    }
    if (t >= end) {
        return 1;
    }
    return (t - start) / (end - start);
}

static double ez(double t, double start, double end)
{
    return ease(clampT(t, start, end));
}

// ---------------------------------------------------------------------------
// Fonts

static void drawText(Canvas &canvas, const Font &font, const char *str, int x, int y, bool on = true)
{
    for (; *str; str++, x += font.advance) {
        unsigned code = (unsigned char)*str;
        const uint8_t *glyph = font.glyphs[code >= 32 && code < 128 ? code - 32 : 0];
        for (int i_row = 0; i_row < font.height; i_row++) {
            for (int i_col = 0; i_col < font.width; i_col++) {
                if (glyph[i_row] & (1 << (font.width - 1 - i_col))) {
                    canvas.set(x + i_col, y + i_row, on);
                }
            }
        }
    }
}

enum Face { F5, F3, F4 }; // 5x7, 3x5, 4x7 condensed

static const Font &fontFor(Face face)
{
    return face == F5 ? FONT_5X7 : face == F3 ? FONT_3X5 : FONT_4X7;
}

static void text(Canvas &canvas, const char *str, int x, int y, bool on = true)
{
    drawText(canvas, FONT_5X7, str, x, y, on);
}
static void text3(Canvas &canvas, const char *str, int x, int y, bool on = true)
{
    drawText(canvas, FONT_3X5, str, x, y, on);
}
static void text4(Canvas &canvas, const char *str, int x, int y, bool on = true)
{
    drawText(canvas, FONT_4X7, str, x, y, on);
}

static int textWidth(const char *str) // 5x7, including the trailing gap
{
    return (int)strlen(str) * 6;
}

// Centring: x = round((32 - (width - 1)) / 2)
static int wordX(const char *word)
{
    return jsround((32 - (textWidth(word) - 1)) / 2.0);
}
static int wordX4(const char *word)
{
    return jsround((32 - ((int)strlen(word) * 5 - 1)) / 2.0);
}

// ---------------------------------------------------------------------------
// Board elements (SPEC §2)

static constexpr int L_EDGE = 13, R_EDGE = 19; // points: left ends at 13, right starts at 19
static constexpr int GL = 6, GR = 23, GY = 10; // games digit origins
static constexpr int PY = 0;                   // points row
static constexpr int GOLDEN_X = 2;             // GOLDEN is 29 wide: cols 2-30

static const char *const PTS[] = {"0", "15", "30", "40"};

// The text shown for one side's points.
static const char *ptsText(const Board &board, int side, char buf[4])
{
    if (board.tiebreak) {
        snprintf(buf, 4, "%d", board.pts[side]);
        return buf;
    }
    return PTS[board.pts[side] & 3];
}

static int lx(const char *str) // right-align the left number against L_EDGE
{
    return L_EDGE - (textWidth(str) - 1);
}

static void netLine(Canvas &canvas)
{
    for (int x = 0; x < 32; x++) {
        canvas.set(x, 8);
    }
}

static void dash(Canvas &canvas, int dx = 0)
{
    for (int x = 14; x <= 17; x++) {
        canvas.set(x + dx, PY + 3);
    }
}

// Both points numbers, each shifted by its own offset; the dash only when
// neither has moved.
static void points(Canvas &canvas, const char *left, const char *right, int dl = 0, int dr = 0)
{
    text(canvas, left, lx(left) + dl, PY);
    text(canvas, right, R_EDGE + dr, PY);
    if (dl == 0 && dr == 0) {
        dash(canvas);
    }
}

// The whole points row of a board shifted by dx (and drawn at row y): GOLDEN,
// or the numbers and the dash.
static void pointsRow(Canvas &canvas, const Board &board, int dx = 0, int y = PY)
{
    if (board.golden()) {
        text4(canvas, "GOLDEN", GOLDEN_X + dx, y);
        return;
    }
    char bl[4], br[4];
    const char *left = ptsText(board, 0, bl), *right = ptsText(board, 1, br);
    text(canvas, left, lx(left) + dx, y);
    text(canvas, right, R_EDGE + dx, y);
    for (int x = 14; x <= 17; x++) {
        canvas.set(x + dx, y + 3);
    }
}

static void games(Canvas &canvas, const Board &board)
{
    char buf[4];
    snprintf(buf, sizeof buf, "%d", board.games[0]);
    text3(canvas, buf, GL, GY);
    snprintf(buf, sizeof buf, "%d", board.games[1]);
    text3(canvas, buf, GR, GY);
}

static void setPosts(Canvas &canvas)
{
    for (int y = 9; y < 16; y++) {
        canvas.set(2, y);
        canvas.set(29, y);
    }
}

// Set cell setIndex (0 = first set won, lower; 1 = second, upper) against
// edge x0, grown to `rows` of its 4 rows from the bottom.
static void setCell(Canvas &canvas, int x0, int setIndex, int rows = 4)
{
    int bottom = setIndex ? 12 : 16;
    for (int y = bottom - rows; y < bottom; y++) {
        canvas.set(x0, y);
        canvas.set(x0 + 1, y);
    }
}

static void setsTally(Canvas &canvas, int leftSets, int rightSets, int rows = 4)
{
    setPosts(canvas);
    for (int i = 0; i < leftSets; i++) {
        setCell(canvas, 0, i, rows);
    }
    for (int i = 0; i < rightSets; i++) {
        setCell(canvas, 30, i, rows);
    }
}

// Serve triangle (§2.1): right angle toward the corner the serve comes from.
static void serve(Canvas &canvas, int side, int box, int size = 4)
{
    int x0 = side ? 18 : 13, y0 = box ? 14 : 10;
    for (int i_row = 0; i_row < size; i_row++) {
        for (int i_col = 0; i_col + i_row < size; i_col++) {
            canvas.set(x0 + (side ? -i_col : i_col), y0 + (box ? -i_row : i_row));
        }
    }
}

// The triangle shrinks away over the first half of progress and regrows at
// its new place over the second.
static void serveFlip(Canvas &canvas, const Board &from, const Board &to, double progress)
{
    if (progress <= 0) {
        serve(canvas, from.sv, from.bx);
    } else if (progress >= 1) {
        serve(canvas, to.sv, to.bx);
    } else if (progress < 0.5) {
        int size = jsround(4 * (1 - 2 * progress));
        if (size) {
            serve(canvas, from.sv, from.bx, size);
        }
    } else {
        int size = jsround(4 * (2 * progress - 1));
        if (size) {
            serve(canvas, to.sv, to.bx, size);
        }
    }
}

void drawBoard(Canvas &canvas, const Board &board)
{
    netLine(canvas);
    pointsRow(canvas, board);
    games(canvas, board);
    setsTally(canvas, board.sets[0], board.sets[1]);
    serve(canvas, board.sv, board.bx);
}

// ---------------------------------------------------------------------------
// Shared building blocks (SPEC §5)

// Odometer roll inside a window the height of the face: the old value moves
// out by height+1 rows and the new one follows it in. dir +1 = new comes up
// from below. drawOld/drawNew take the y to draw at.
template <class DrawOld, class DrawNew>
static void rollRow(Canvas &canvas, int y, int h, double progress, int dir, DrawOld drawOld, DrawNew drawNew)
{
    int off = jsround(ease(progress) * (h + 1)) * dir;
    auto clip = canvas.pushClip(0, 31, y, y + h - 1);
    drawOld(y - off);
    drawNew(y + (h + 1) * dir - off);
    canvas.popClip(clip);
}

// Roll one string to another; xOf gives the x for a string.
template <class XOf>
static void roll(Canvas &canvas, const char *oldS, const char *newS, XOf xOf, int y, double progress, int dir,
                 Face face = F5)
{
    const Font &font = fontFor(face);
    rollRow(
        canvas, y, font.height, progress, dir, [&](int yy) { drawText(canvas, font, oldS, xOf(oldS), yy); },
        [&](int yy) { drawText(canvas, font, newS, xOf(newS), yy); });
}

// The winning ball: 4 pixels along the lane above the line, head leading,
// with a shadow in the line beneath the head. dir +1 = left to right. It
// flies toward the side that won the point.
static void winBall(Canvas &canvas, double t, double t0, int dir)
{
    double progress = clampT(t, t0, t0 + 0.35);
    if (progress >= 1) {
        return;
    }
    double head = dir > 0 ? mix(-1, 34, progress) : mix(32, -3, progress);
    for (int i = 0; i < 4; i++) {
        int x = jsround(head - i * dir * 1.2);
        canvas.set(x, 7);
        if (i == 0) {
            canvas.set(x, 8, false);
            canvas.set(x - dir, 8, false);
        }
    }
}

// Arrowhead pointing in dir with its tip at x, rows 10-14; nudge shifts it
// one pixel in dir.
static void tri(Canvas &canvas, int x, int dir, int nudge)
{
    for (int i = 0; i < 3; i++) {
        for (int y = 12 - i; y <= 12 + i; y++) {
            canvas.set(x + (nudge - i) * dir, y);
        }
    }
}

// Prompt text alternating first, roll, second, roll back.
static void altWords(Canvas &canvas, const char *first, const char *second, double t, int y, double hold, double rl,
                     Face face = F5)
{
    double ph = fmod(t, 2 * (hold + rl)); // negative before the prompt starts: shows first
    auto xo = [face](const char *word) { return face == F4 ? wordX4(word) : wordX(word); };
    const Font &font = fontFor(face);
    if (ph < hold) {
        drawText(canvas, font, first, xo(first), y);
    } else if (ph < hold + rl) {
        roll(canvas, first, second, xo, y, (ph - hold) / rl, 1, face);
    } else if (ph < 2 * hold + rl) {
        drawText(canvas, font, second, xo(second), y);
    } else {
        roll(canvas, second, first, xo, y, (ph - 2 * hold - rl) / rl, 1, face);
    }
}

// A lit bar sweeps across in dir over 0.6 s from t0: `before` is still drawn
// ahead of it, `after` is revealed behind it.
template <class Before, class After>
static void barWipe(Canvas &canvas, double t, double t0, int dir, Before before, After after)
{
    int lead = jsround(ez(t, t0, t0 + 0.3) * 32), trail = jsround(ez(t, t0 + 0.25, t0 + 0.6) * 32);
    auto col = [dir](int swept) { return dir > 0 ? swept : 31 - swept; };
    before();
    for (int i_col = trail; i_col < lead; i_col++) {
        for (int y = 0; y < 16; y++) {
            canvas.set(col(i_col), y);
        }
    }
    if (trail > 0) {
        auto clip = dir > 0 ? canvas.pushClip(0, trail - 1, 0, 15) : canvas.pushClip(32 - trail, 31, 0, 15);
        for (int i_col = 0; i_col < trail; i_col++) {
            for (int y = 0; y < 16; y++) {
                canvas.set(col(i_col), y, false);
            }
        }
        after();
        canvas.popClip(clip);
    }
}

// Fixed pseudo-random pixel order for the dissolves: a Fisher-Yates shuffle
// driven by the reference's LCG, reproducing its double-precision arithmetic
// (the product overflows 53 bits, so the rounding is part of the sequence).
static const uint16_t *perm()
{
    static uint16_t order[512];
    static bool made = false;
    if (!made) {
        made = true;
        uint32_t seed = 7;
        auto nextRandom = [&seed]() {
            volatile double prod = (double)seed * 1103515245.0; // volatile: no fused multiply-add
            double sum = prod + 12345.0;
            seed = (uint32_t)(int64_t)sum & 0x7fffffff;
            return seed / 2147483647.0;
        };
        for (int i = 0; i < 512; i++) {
            order[i] = i;
        }
        for (int i = 511; i > 0; i--) {
            int pick = (int)floor(nextRandom() * (i + 1));
            if (pick > i) {
                pick = i;
            }
            uint16_t tmp = order[i];
            order[i] = order[pick];
            order[pick] = tmp;
        }
    }
    return order;
}

static void dissolve(Canvas &canvas, int count)
{
    const uint16_t *order = perm();
    for (int i = 0; i < count; i++) {
        canvas.clearIndex(order[i]);
    }
}

// ---------------------------------------------------------------------------
// Point / Golden / Undo (SPEC §5.4, §5.6)

// The common first 0.75 s: ball, the scorer's points roll up, serve flip.
// Draws the dash as well unless told not to (Golden grows its own).
static void pointBody(Canvas &canvas, double t, const Board &from, const Board &to, int side, bool withDash)
{
    netLine(canvas);
    games(canvas, from);
    setsTally(canvas, from.sets[0], from.sets[1]);
    serveFlip(canvas, from, to, clampT(t, 0.5, 0.75));
    double progress = clampT(t, 0.3, 0.7);
    if (withDash) {
        dash(canvas);
    }
    char ba[4], bo[4], bn[4];
    const char *still = ptsText(from, 1 - side, ba);
    const char *oldS = ptsText(from, side, bo), *newS = ptsText(to, side, bn);
    if (side) {
        text(canvas, still, lx(still), PY);
        roll(canvas, oldS, newS, [](const char *) { return R_EDGE; }, PY, progress, 1);
    } else {
        text(canvas, still, R_EDGE, PY);
        roll(canvas, oldS, newS, lx, PY, progress, 1);
    }
    winBall(canvas, t, 0, side ? 1 : -1);
}

void animPoint(Canvas &canvas, double t, const Board &from, const Board &to, int side)
{
    pointBody(canvas, t, from, to, side, true);
}

void animGolden(Canvas &canvas, double t, const Board &from, const Board &to, int side)
{
    const double T1 = 0.75, T2 = 1.0, T3 = 1.45; // dash becomes a bar; bars sweep out; done
    if (t < T2) {
        // The point lands; the dash stands up into a vertical bar.
        pointBody(canvas, t, from, to, side, false);
        double progress = ez(t, T1, T2);
        int hw = jsround(mix(2, 1, progress)), hh = jsround(mix(0, 3, progress));
        for (int y = PY + 3 - hh; y <= PY + 3 + hh; y++) {
            for (int x = 16 - hw; x <= 15 + hw; x++) {
                canvas.set(x, y);
            }
        }
        return;
    }
    netLine(canvas);
    games(canvas, from);
    setsTally(canvas, from.sets[0], from.sets[1]);
    serve(canvas, to.sv, to.bx);
    if (t < T3) {
        // Twin bars sweep to the edges: 40-40 ahead of them, GOLDEN behind.
        int spread = jsround(ez(t, T2, T3) * 16);
        char bl[4], br[4];
        const char *left = ptsText(to, 0, bl), *right = ptsText(to, 1, br);
        auto clip = canvas.pushClip(0, 13 - spread, PY, PY + 6);
        text(canvas, left, lx(left), PY);
        canvas.popClip(clip);
        clip = canvas.pushClip(18 + spread, 31, PY, PY + 6);
        text(canvas, right, R_EDGE, PY);
        canvas.popClip(clip);
        clip = canvas.pushClip(15 - spread, 16 + spread, PY, PY + 6);
        text4(canvas, "GOLDEN", GOLDEN_X, PY);
        canvas.popClip(clip);
        for (int y = PY; y <= PY + 6; y++) {
            canvas.set(15 - spread, y);
            canvas.set(16 + spread, y);
        }
        return;
    }
    text4(canvas, "GOLDEN", GOLDEN_X, PY);
    if (t >= 1.6 && t < 2.2) { // one glint: a dark diagonal streak across the letters
        double glint = ez(t, 1.6, 2.2) * 44 - 6;
        for (int y = PY; y <= PY + 6; y++) {
            canvas.set(jsround(glint - y), y, false);
            canvas.set(jsround(glint - y) + 1, y, false);
        }
    }
}

void animUndo(Canvas &canvas, double t, const Board &from, const Board &to)
{
    netLine(canvas);
    games(canvas, from);
    setsTally(canvas, from.sets[0], from.sets[1]);
    serveFlip(canvas, from, to, clampT(t, 0.5, 0.75));
    if (t < 0.75) { // the line runs backwards: dark gaps marching right to left
        int ph = (int)floor(t * 40);
        for (int x = 0; x < 32; x++) {
            if ((x + ph) % 6 < 3) {
                canvas.set(x, 8, false);
            }
        }
    }
    int tug = t > 0.05 && t < 0.3 ? -1 : 0; // the whole row is pulled back for a moment
    double progress = clampT(t, 0.3, 0.7);
    if (from.golden() || to.golden()) {
        // GOLDEN can't roll one side at a time: the whole row rolls down.
        rollRow(
            canvas, PY, 7, progress, -1, [&](int y) { pointsRow(canvas, from, tug, y); },
            [&](int y) { pointsRow(canvas, to, tug, y); });
        return;
    }
    dash(canvas, tug);
    char ba[4], bb[4];
    for (int side = 0; side < 2; side++) {
        const char *oldS = ptsText(from, side, ba), *newS = ptsText(to, side, bb);
        auto xOf = [side, tug](const char *str) { return (side ? R_EDGE : lx(str)) + tug; };
        if (strcmp(oldS, newS) == 0) {
            text(canvas, oldS, xOf(oldS), PY);
        } else {
            roll(canvas, oldS, newS, xOf, PY, progress, -1);
        }
    }
}

// ---------------------------------------------------------------------------
// Game / Set / Match (SPEC §5.5)

void animGame(Canvas &canvas, double t, const Board &from, const Board &to, int winner)
{
    int loser = 1 - winner;
    netLine(canvas);
    setsTally(canvas, from.sets[0], from.sets[1]);
    serveFlip(canvas, from, to, clampT(t, 2.05, 2.3));

    // Games row: the winner's digit rolls up with a sparkle at its corners.
    int gxw = winner ? GR : GL, gxl = loser ? GR : GL;
    if (t < 1.5) {
        games(canvas, from);
    } else {
        char bl[4], bo[4], bn[4];
        snprintf(bl, sizeof bl, "%d", from.games[loser]);
        snprintf(bo, sizeof bo, "%d", from.games[winner]);
        snprintf(bn, sizeof bn, "%d", to.games[winner]);
        text3(canvas, bl, gxl, GY);
        roll(canvas, bo, bn, [gxw](const char *) { return gxw; }, GY, clampT(t, 1.5, 1.9), 1, F3);
    }
    if (t >= 1.5 && t < 1.7) {
        canvas.set(gxw - 1, GY - 1);
        canvas.set(gxw + 3, GY - 1);
        canvas.set(gxw - 1, GY + 5);
        canvas.set(gxw + 3, GY + 5);
    }

    // Points row: the numbers slide off to their own edges...
    if (t < 0.7) {
        int slide = jsround(ez(t, 0.35, 0.7) * 16);
        if (from.golden()) { // GOL slides left, DEN slides right
            auto clip = canvas.pushClip(0, 15, PY, PY + 6);
            text4(canvas, "GOL", GOLDEN_X - slide, PY);
            canvas.popClip(clip);
            clip = canvas.pushClip(16, 31, PY, PY + 6);
            text4(canvas, "DEN", GOLDEN_X + 15 + slide, PY);
            canvas.popClip(clip);
        } else {
            char bl[4], br[4];
            points(canvas, ptsText(from, 0, bl), ptsText(from, 1, br), -slide, slide);
        }
    }
    // ...GAME drops in letter by letter, then lifts out the top...
    if (t >= 0.7) {
        int up = jsround(ez(t, 2.0, 2.4) * 9);
        const char *word = "GAME";
        for (int i = 0; i < 4; i++) {
            int y = jsround(mix(-8, PY, easeBack(clampT(t, 0.7 + i * 0.15, 1.05 + i * 0.15))));
            char ch[2] = {word[i], 0};
            text(canvas, ch, 4 + i * 6, y - up);
        }
    }
    // ...while the new points wipe in from the centre outward.
    if (t >= 2.0) {
        int w = jsround(ez(t, 2.05, 2.4) * 14);
        auto clip = canvas.pushClip(15 - w, 16 + w, PY, PY + 6);
        pointsRow(canvas, to);
        canvas.popClip(clip);
    }
    winBall(canvas, t, 0, winner ? 1 : -1);
}

void animSet(Canvas &canvas, double t, const Board &from, const Board &to, int winner)
{
    drawBoard(canvas, t < 2.1 ? from : to);
    int fillTo = jsround(ez(t, 0.35, 0.85) * 32); // columns lit from the winner's edge
    int cut = jsround(ez(t, 2.1, 2.6) * 16);      // rows drained from the top
    if (t >= 0.35) {
        for (int y = cut; y < 16; y++) {
            for (int i_col = 0; i_col < fillTo; i_col++) {
                canvas.set(winner ? 31 - i_col : i_col, y);
            }
        }
        auto clip = canvas.pushClip(0, 31, cut, 15);
        const char *word = "SET";
        for (int i = 0; i < 3; i++) {
            if (t >= 0.85 + i * 0.15) {
                char ch[2] = {word[i], 0};
                text(canvas, ch, 7 + i * 6, 1, false);
            }
        }
        if (t >= 1.4) { // the set's games, left-right
            char sc[8];
            snprintf(sc, sizeof sc, "%d-%d", from.games[0] + (winner == 0), from.games[1] + (winner == 1));
            text3(canvas, sc, 10, 9, false);
        }
        canvas.popClip(clip);
    }
    if (t >= 2.6 && t < 3.4 && ((int)floor((t - 2.6) / 0.13)) % 2) { // the new block blinks
        int setIndex = to.sets[winner] - 1;
        int x0 = winner ? 29 : 0;
        for (int y = setIndex ? 8 : 12; y < (setIndex ? 12 : 16); y++) {
            for (int x = x0; x < x0 + 3; x++) {
                canvas.set(x, y, false);
            }
        }
    }
    winBall(canvas, t, 0, winner ? 1 : -1);
}

static void firework(Canvas &canvas, double t, double t0, int x0)
{
    const double rise = 0.45, life = 0.7, ay = 10;
    double age = t - t0;
    if (age < 0 || age > rise + life) {
        return;
    }
    if (age < rise) {
        canvas.set(x0, jsround(mix(15, ay, ease(age / rise))));
        return;
    }
    double burst = (age - rise) / life;
    if (burst > 0.55 && ((int)floor(burst * 24)) % 2) { // twinkle out
        return;
    }
    for (int i = 0; i < 8; i++) {
        double an = i * M_PI / 4 + 0.3, sp = 5.5;
        canvas.setf(x0 + cos(an) * sp * burst, ay + sin(an) * sp * burst * 0.8 + 5 * burst * burst);
    }
}

static const double ROCKETS[][2] = {{1.3, 6}, {1.7, 25}, {2.2, 15}, {2.7, 10}, {3.1, 22}};

void animMatch(Canvas &canvas, double t, const Board &from, int winner, const char *finalScore)
{
    if (t < 1.0) { // the old board dissolves away
        drawBoard(canvas, from);
        winBall(canvas, t, 0, winner ? 1 : -1);
        dissolve(canvas, jsround(ez(t, 0.35, 1.0) * 512));
        return;
    }
    const char *word = "MATCH";
    for (int i = 0; i < 5; i++) {
        int y = jsround(mix(-8, 0, easeBack(clampT(t, 1.0 + i * 0.12, 1.4 + i * 0.12))));
        char ch[2] = {word[i], 0};
        text(canvas, ch, 1 + i * 6, y);
    }
    for (const auto &rocket : ROCKETS) {
        firework(canvas, t, rocket[0], (int)rocket[1]);
    }
    if (t >= 4.0) {
        int width = (int)strlen(finalScore) * 4 - 1;
        if (width <= 32) { // wipes in from the centre and stays
            int w = jsround(ez(t, 4.0, 4.4) * 16);
            auto clip = canvas.pushClip(16 - w, 15 + w, 9, 13);
            text3(canvas, finalScore, (32 - width) / 2, 9);
            canvas.popClip(clip);
        } else { // three sets don't fit: keep scrolling it through
            double x = 32 - fmod((t - 4.0) * 25, 32.0 + width);
            text3(canvas, finalScore, jsround(x), 9);
        }
    }
}

// ---------------------------------------------------------------------------
// Swap ends (SPEC §5.7)

void animSwap(Canvas &canvas, double t, const Board &from, const Board &to, double pressT)
{
    const double EMERGE = pressT + 0.3;
    netLine(canvas);
    double in = pow(clampT(t, 0, 0.5), 2), outp = ez(t, EMERGE, EMERGE + 0.6); // accelerate in, decelerate out
    const Board &shown = t < EMERGE ? from : to;
    int offset = t < EMERGE ? jsround(in * 12) : jsround((1 - outp) * 12);
    if (t < 0.5 || t >= EMERGE) { // each half slides into / out of the centre
        char bl[4], br[4], gl[4], gr[4];
        const char *left = ptsText(shown, 0, bl), *right = ptsText(shown, 1, br);
        snprintf(gl, sizeof gl, "%d", shown.games[0]);
        snprintf(gr, sizeof gr, "%d", shown.games[1]);
        dash(canvas);
        auto clip = canvas.pushClip(0, 13, 0, 15);
        text(canvas, left, lx(left) + offset, PY);
        text3(canvas, gl, GL + offset, GY);
        canvas.popClip(clip);
        clip = canvas.pushClip(18, 31, 0, 15);
        text(canvas, right, R_EDGE - offset, PY);
        text3(canvas, gr, GR - offset, GY);
        canvas.popClip(clip);
    }
    double cellGrow = t < EMERGE ? 1 - in : outp; // set cells shrink, then regrow
    setsTally(canvas, shown.sets[0], shown.sets[1], jsround(4 * cellGrow));
    if (t >= 0.5 && t < EMERGE) { // the prompt, rolling away once acknowledged
        int up = jsround(ez(t, pressT, EMERGE) * 8);
        auto clip = canvas.pushClip(0, 31, PY, PY + 6);
        altWords(canvas, "SWAP", "ENDS", t - 0.5, PY - up, 0.6, 0.15);
        canvas.popClip(clip);
        double ph = fmod(t - 0.5, 1.5); // arrowheads walk in from each edge and cross
        if (t < pressT && ph < 1.1) {
            int travel = jsround(ease(ph / 1.1) * 23);
            tri(canvas, 5 + travel, 1, 0);
            tri(canvas, 26 - travel, -1, 0);
        }
    }
    if (t < 0.5) {
        serve(canvas, from.sv, from.bx);
    } else if (t >= EMERGE) {
        serve(canvas, to.sv, to.bx);
    }
}

// ---------------------------------------------------------------------------
// Bind / Select serve (SPEC §5.2, §5.3)

// The Select Serve waiting state, also revealed at the end of Bind.
static void servePrompt(Canvas &canvas, double t, bool arrows)
{
    netLine(canvas);
    altWords(canvas, "SELECT", "SERVE", t, 0, 0.9, 0.2, F4);
    if (arrows) {
        int blink = ((int)floor(t / 0.5)) % 2;
        tri(canvas, 4, -1, blink);
        tri(canvas, 27, 1, blink);
    }
}

void animBind(Canvas &canvas, double t, double pressL, double pressR)
{
    int blink = ((int)floor(t / 0.5)) % 2;
    auto left = [&]() {
        netLine(canvas);
        altWords(canvas, "PRESS", "LEFT", t, 0, 0.9, 0.2);
        tri(canvas, 4, -1, blink);
    };
    auto right = [&]() {
        netLine(canvas);
        altWords(canvas, "PRESS", "RIGHT", t - pressL - 0.6, 0, 0.9, 0.2);
        tri(canvas, 27, 1, blink);
    };
    auto done = [&]() { servePrompt(canvas, t - pressR - 0.6, true); }; // its clock starts when the wipe ends
    if (t < pressL) {
        left();
    } else if (t < pressL + 0.6) {
        barWipe(canvas, t, pressL, 1, left, right);
    } else if (t < pressR) {
        right();
    } else {
        barWipe(canvas, t, pressR, -1, right, done);
    }
}

void animServe(Canvas &canvas, double t, double pressT, int side, int box)
{
    const double GLIDE = pressT + 0.5, MORPH = GLIDE + 0.3, DONE = MORPH + 0.6;
    int dir = side ? 1 : -1;   // the arrowhead points outward
    int tip0 = side ? 27 : 4;  // at the edge...
    int tip1 = side ? 18 : 13; // ...glides to the serve triangle's corner
    if (t < pressT) {
        servePrompt(canvas, t, true);
        return;
    }
    netLine(canvas);
    int up = jsround(ez(t, pressT, pressT + 0.3) * 8); // the prompt rolls up and out
    if (up < 8) {
        auto clip = canvas.pushClip(0, 31, PY, PY + 6);
        altWords(canvas, "SELECT", "SERVE", t, PY - up, 0.9, 0.2, F4);
        canvas.popClip(clip);
    }
    if (t < GLIDE) {
        tri(canvas, jsround(mix(tip0, tip1, ez(t, pressT, GLIDE))), dir, 0);
    } else if (t < MORPH) { // folds to one column, regrows as the triangle
        double progress = clampT(t, GLIDE, MORPH);
        if (progress < 0.5) {
            int halfWidth = jsround(2 * (1 - 2 * progress));
            for (int i = 0; i <= halfWidth; i++) {
                for (int y = 12 - i; y <= 12 + i; y++) {
                    canvas.set(tip1 - i * dir, y);
                }
            }
        } else {
            int size = jsround(4 * (2 * progress - 1));
            serve(canvas, side, box, size < 1 ? 1 : size);
        }
    } else { // the rest of the board emerges from the centre around the triangle
        serve(canvas, side, box);
        double emerge = ez(t, MORPH, DONE);
        int offset = jsround((1 - emerge) * 12);
        dash(canvas);
        auto clip = canvas.pushClip(0, 13, 0, 15);
        text(canvas, "0", lx("0") + offset, PY);
        text3(canvas, "0", GL + offset, GY);
        canvas.popClip(clip);
        clip = canvas.pushClip(18, 31, 0, 15);
        text(canvas, "0", R_EDGE - offset, PY);
        text3(canvas, "0", GR - offset, GY);
        canvas.popClip(clip);
        int rows = jsround(emerge * 7); // the set posts grow down from the line
        for (int y = 9; y < 9 + rows; y++) {
            canvas.set(2, y);
            canvas.set(29, y);
        }
    }
}

void bindWipeIn(Canvas &canvas, double t)
{
    int h = jsround(ease(t < BIND_WIPE_IN_S ? t / BIND_WIPE_IN_S : 1) * 8);
    for (int y = 0; y < 16; y++) {
        if (y != 8 && (y > 8 ? y - 8 : 8 - y) > h) {
            canvas.rows[y] = 0;
        }
    }
}

// ---------------------------------------------------------------------------
// Fixed clips

bool drawFixed(Canvas &canvas, const FixedFrame *frames, int count, double t)
{
    // Walk by source frames rather than ms so frame boundaries fall exactly
    // on 30 fps ticks.
    int target = (int)floor(t * 30 + 1e-9);
    int frame = 0, start = 0;
    while (frame < count - 1) {
        int ticks = jsround(frames[frame].ms * 30 / 1000.0);
        if (target < start + ticks) {
            break;
        }
        start += ticks;
        frame++;
    }
    for (int y = 0; y < 16; y++) {
        canvas.rows[y] |= frames[frame].rows[y];
    }
    int total = start;
    for (int i_frame = frame; i_frame < count; i_frame++) {
        total += jsround(frames[i_frame].ms * 30 / 1000.0);
    }
    return target < total;
}
