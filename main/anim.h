#pragma once

// The display design from design/SPEC.md: the resting board and every
// animation, ported from the reference implementation in design/p10-sim.html.
// Everything is a pure function of time and the board before/after, so it
// renders identically on the host (test/) and on the panel.
//
// Times are seconds from the start of the animation; frames are authored at
// 30 fps. Prompts that wait for a button take the press time as a parameter
// (INFINITY while still waiting) and run to a fixed time after it.

#include "canvas.h"

// What the panel shows, in panel sides: index 0 is the left half, 1 the right.
struct Board {
    int pts[2] = {0, 0};   // 0..3 = 0/15/30/40, or tiebreak points
    bool tiebreak = false; // points are plain numbers
    int games[2] = {0, 0}; // in the current set, 0..7
    int sets[2] = {0, 0};  // 0..2
    int servingSide = 0;            // side serving
    int serveBox = 0;            // service box: 0 top, 1 bottom

    bool golden() const { return !tiebreak && pts[0] == 3 && pts[1] == 3; } // row shows GOLDEN
};

constexpr double POINT_S = 0.75, GOLDEN_S = 2.3, UNDO_S = 0.75, GAME_S = 2.4, SET_S = 3.4;
constexpr double SWAP_AFTER_PRESS_S = 0.9, BIND_AFTER_PRESS_S = 0.6, SERVE_AFTER_PRESS_S = 1.4;
constexpr double BIND_WIPE_IN_S = 0.35;

// The resting board (SPEC §2). Nothing on it moves.
void drawBoard(Canvas &canvas, const Board &board);

// Point to `side` (§5.4): ball toward the winner, that side's points roll up,
// serve flips. from = before, to = after.
void animPoint(Canvas &canvas, double t, const Board &from, const Board &to, int side);

// The point that makes it 40-40 (§5.6): a Point, then the dash stands up,
// splits and sweeps GOLDEN in. GOLDEN then stays on the resting board.
void animGolden(Canvas &canvas, double t, const Board &from, const Board &to, int side);

// Undo (§5.4): the line rewinds, the points row is tugged back and rolls
// down to the restored values. from = before the undo, to = restored.
void animUndo(Canvas &canvas, double t, const Board &from, const Board &to);

// Game won by `winner` (§5.5): points slide off, GAME drops in, the games
// digit rolls, the new points wipe in.
void animGame(Canvas &canvas, double t, const Board &from, const Board &to, int winner);

// Set won (§5.5): the panel floods from the winner's edge, SET and the set
// score are punched out, the flood drains to reveal the new board.
void animSet(Canvas &canvas, double t, const Board &from, const Board &to, int winner);

// Match won (§5.5): the board dissolves, MATCH drops in, fireworks, then the
// final score (3x5, e.g. "6-4 6-3") wipes in and stays. Holds for ever; pass
// elapsed time.
void animMatch(Canvas &canvas, double t, const Board &from, int winner, const char *finalScore);

// Swap ends (§5.7): the board folds into the centre, SWAP / ENDS waits for
// the press at pressedAt, then the board emerges mirrored (to). Ends at
// pressedAt + SWAP_AFTER_PRESS_S.
void animSwap(Canvas &canvas, double t, const Board &from, const Board &to, double pressedAt);

// Bind controllers (§5.2): PRESS / LEFT until leftPressedAt, wipe, PRESS / RIGHT
// until rightPressedAt, wipe into the Select Serve prompt. Ends at
// rightPressedAt + BIND_AFTER_PRESS_S; continue with animServe on a clock starting
// there, so the prompt reads SELECT before SERVE.
void animBind(Canvas &canvas, double t, double leftPressedAt, double rightPressedAt);

// Select serve (§5.3): SELECT / SERVE until pressedAt from `side`. The prompt
// rolls away, that side's arrowhead glides to the centre and becomes the serve
// triangle (side's first-serve box `box`), and the rest of the fresh board
// emerges from the centre around it. Ends at pressedAt + SERVE_AFTER_PRESS_S.
void animServe(Canvas &canvas, double t, double pressedAt, int side, int box);

// The hand-over from the splash (§5.1): masks the Bind screen so it wipes in
// from the dividing line outward over the first BIND_WIPE_IN_S seconds.
void bindWipeIn(Canvas &canvas, double t);

// Draws the fixed frame showing at time t of a 30 fps clip (held on the last
// frame past the end). Returns false once t is past the end.
bool drawFixed(Canvas &canvas, const FixedFrame *frames, int count, double t);
