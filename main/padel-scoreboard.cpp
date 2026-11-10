// Padel scoreboard on one 32x16 HUB12 panel, controlled by two 433 MHz
// remote buttons. What the panel shows is specified in design/SPEC.md and
// drawn by anim.cpp; this file is the screen flow (SPEC §4) and the inputs
// (SPEC §3).
//
//   power on     splash (any remote press skips it), then BIND: "PRESS /
//                LEFT" takes the first remote pressed as the left team's,
//                "PRESS / RIGHT" a different one as the right team's. Other
//                remotes are ignored for the rest of the match. Then
//                "SELECT / SERVE": the side whose remote is pressed serves
//                first and the board wipes in.
//   short press  point to that team (on release): Point, or Golden at
//                40-40, or Game / Set / Match when the point wins one.
//                After a set (not the last), "SWAP / ENDS" waits for a
//                press (which doesn't score) and
//                the board emerges mirrored. Remotes stay with their team.
//   long press   undo the last point (either remote).
//   match over   MATCH and the final score stay on the panel until power
//                is cycled.
//
// Presses during an animation are queued and applied when it finishes.

#include <Arduino.h>
#include <DMD2.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "anim.h"
#include "hub12.h"
#include "rx433.h"
#include "score.h"
#include "sdkconfig.h"
#include "splash_frames.h"

static const char *TAG = "scoreboard";

// Held this long = long press, acted on as soon as it's reached (rx433
// times it from the remote's own frames).
static constexpr uint32_t LONG_PRESS_MS = 1000;

static constexpr int64_t FRAME_US = 1000000 / 30; // the design is authored at 30 fps
static const double NOT_PRESSED = INFINITY;                // "no press yet" for the prompts

static Hub12 panel;
static DMDFrame back_buffer(32, 16); // drawn here, then copied, so a scan never sees a half-drawn frame
static Match match;
static QueueHandle_t event_queue;
static uint32_t codes[2]; // remote code of team 0 (bound as left) and team 1

static void on_rx(const rx433_event_t *ev, void *)
{
    xQueueSend(event_queue, ev, 0);
}

static int team_of(uint32_t code)
{
    for (int i = 0; i < 2; i++) {
        if (codes[i] == code) {
            return i;
        }
    }
    return -1;
}

static void show_canvas(const Canvas &frame)
{
    for (int y = 0; y < Canvas::H; y++) {
        for (int x = 0; x < Canvas::W; x++) {
            back_buffer.setPixel(x, y, frame.get(x, y) ? GRAPHICS_ON : GRAPHICS_OFF);
        }
    }
    panel.copyFrame(back_buffer, 0, 0);
}

// Seconds since a screen started.
struct Clock {
    int64_t t0 = esp_timer_get_time();
    double now() const { return (esp_timer_get_time() - t0) / 1e6; }
};

// Renders render(canvas, t) at 30 fps on `clk` until done(t). With `events`,
// remote events are handed to on_event(t, ev) as they arrive; without, they
// stay queued for whoever runs next.
template <class Render, class OnEvent, class Done>
static void run(const Clock &clk, Render render, OnEvent on_event, bool events, Done done)
{
    Canvas canvas;
    int64_t next = esp_timer_get_time();
    while (true) {
        double t = clk.now();
        if (done(t)) {
            return;
        }
        canvas.clear();
        render(canvas, t);
        show_canvas(canvas);

        next += FRAME_US;
        while (true) {
            int64_t wait = next - esp_timer_get_time();
            if (wait <= 0) {
                if (wait < -FRAME_US) {
                    next = esp_timer_get_time(); // fell behind: don't try to catch up
                }
                break;
            }
            TickType_t ticks = pdMS_TO_TICKS((wait + 999) / 1000);
            if (!events) {
                vTaskDelay(ticks);
                break;
            }
            rx433_event_t ev;
            if (xQueueReceive(event_queue, &ev, ticks)) {
                on_event(clk.now(), ev);
            }
        }
    }
}

// Plays a fixed-length animation; remote events queue up meanwhile.
template <class Render>
static void play(Render render, double seconds)
{
    Clock clk;
    run(
        clk, render, [](double, const rx433_event_t &) {}, false, [seconds](double t) { return t >= seconds; });
}

// Service box (0 top, 1 bottom) for the team on panel side `serving_side` after
// `played` points of the game: the first serve of a game is from the server's
// right-hand box, then they alternate. Each team's right is on the opposite
// side of the court; by default the top of the panel is the right-hand box of
// the team on the right (menuconfig flips it for the other mounting).
static int serve_box(int serving_side, int played)
{
    int box = played % 2;
    if (serving_side == 0) {
        box ^= 1;
    }
#if CONFIG_SCOREBOARD_FLIP_SERVE_BOXES
    box ^= 1;
#endif
    return box;
}

// The score as laid out on the panel: Board side 0 is the left half. Team 0
// starts on the left and the sides mirror on every change of ends.
static Board board_of(const Score &score, bool swapped)
{
    int left = swapped ? 1 : 0, right = 1 - left;
    Board board;
    board.pts[0] = score.points[left];
    board.pts[1] = score.points[right];
    board.tiebreak = score.tiebreak;
    board.games[0] = score.games[left];
    board.games[1] = score.games[right];
    board.sets[0] = score.sets[left];
    board.sets[1] = score.sets[right];
    board.servingSide = score.server == left ? 0 : 1;
    board.serveBox = serve_box(board.servingSide, score.points[0] + score.points[1]);
    return board;
}

static void show_board()
{
    const Score &score = match.score();
    ESP_LOGI(TAG, "sets %u-%u games %u-%u points %u-%u%s serve %c%s", score.sets[0], score.sets[1], score.games[0],
             score.games[1], score.points[0], score.points[1], score.tiebreak ? " (tiebreak)" : "", 'A' + score.server,
             score.endsSwapped() ? " [A on the right]" : "");
    Canvas canvas;
    drawBoard(canvas, board_of(score, score.endsSwapped()));
    show_canvas(canvas);
}

// Boot splash (SPEC §5.1). Any remote press skips the rest; that press
// doesn't bind anything.
static void splash()
{
    static const int SPLASH_COUNT = sizeof(SPLASH) / sizeof(SPLASH[0]);
    double length = 0;
    for (const FixedFrame &frame : SPLASH) {
        length += frame.ms / 1000.0;
    }
    Clock clk;
    bool skipped = false;
    run(
        clk, [](Canvas &canvas, double t) { drawFixed(canvas, SPLASH, SPLASH_COUNT, t); },
        [&](double, const rx433_event_t &ev) {
            if (ev.type == RX433_EVENT_PRESS) {
                ESP_LOGI(TAG, "splash skipped");
                skipped = true;
            }
        },
        true, [&](double t) { return skipped || t >= length; });
}

// Bind controllers (SPEC §5.2) on the setup clock, which the splash just
// started: the screen wipes in around the dividing line, then waits.
static void bind(const Clock &clk)
{
    double left_pressed_at = NOT_PRESSED, right_pressed_at = NOT_PRESSED;
    ESP_LOGI(TAG, "bind: press the left remote");
    run(
        clk,
        [&](Canvas &canvas, double t) {
            animBind(canvas, t, left_pressed_at, right_pressed_at);
            bindWipeIn(canvas, t);
        },
        [&](double t, const rx433_event_t &ev) {
            if (ev.type != RX433_EVENT_PRESS) {
                return;
            }
            if (left_pressed_at == NOT_PRESSED) {
                left_pressed_at = t;
                codes[0] = ev.code;
                ESP_LOGI(TAG, "remote 0x%08lx is left (team A); press the right remote", (unsigned long)ev.code);
            } else if (right_pressed_at == NOT_PRESSED && t >= left_pressed_at + BIND_AFTER_PRESS_S) {
                if (ev.code == codes[0]) {
                    ESP_LOGI(TAG, "remote 0x%08lx is already left, press the other one", (unsigned long)ev.code);
                    return;
                }
                right_pressed_at = t;
                codes[1] = ev.code;
                ESP_LOGI(TAG, "remote 0x%08lx is right (team B)", (unsigned long)ev.code);
            }
        },
        true, [&](double t) { return t >= right_pressed_at + BIND_AFTER_PRESS_S; });
}

// Select serve (SPEC §5.3), on a clock starting as Bind's last wipe ends so
// the prompt reads SELECT first. Returns the side (= team, nothing is swapped
// yet) that serves first; ends with the fresh board on the panel.
static int select_serve(const Clock &clk)
{
    double pressed_at = NOT_PRESSED;
    int side = 0;
    ESP_LOGI(TAG, "select serve: press a remote");
    run(
        clk, [&](Canvas &canvas, double t) { animServe(canvas, t, pressed_at, side, serve_box(side, 0)); },
        [&](double t, const rx433_event_t &ev) {
            int team = team_of(ev.code);
            if (ev.type == RX433_EVENT_PRESS && pressed_at == NOT_PRESSED && team >= 0) {
                pressed_at = t;
                side = team;
                ESP_LOGI(TAG, "team %c serves first", 'A' + team);
            }
        },
        true, [&](double t) { return t >= pressed_at + SERVE_AFTER_PRESS_S; });
    return side;
}

// Swap ends (SPEC §5.7): the board folds away, the prompt waits for a press
// from either bound remote (the acknowledgement; it doesn't score), the
// board emerges mirrored.
static void swap_ends(const Board &from, const Board &to)
{
    ESP_LOGI(TAG, "swap ends");
    Clock clk;
    double pressed_at = NOT_PRESSED;
    run(
        clk, [&](Canvas &canvas, double t) { animSwap(canvas, t, from, to, pressed_at); },
        [&](double t, const rx433_event_t &ev) {
            if (ev.type == RX433_EVENT_PRESS && pressed_at == NOT_PRESSED && t >= 0.5 && team_of(ev.code) >= 0) {
                pressed_at = t;
            }
        },
        true, [&](double t) { return t >= pressed_at + SWAP_AFTER_PRESS_S; });
}

// Match won (SPEC §5.5): plays once and holds the final score until power
// is cycled. Never returns; remote presses are ignored.
[[noreturn]] static void match_over(const Board &from, int side, const Score &score)
{
    char final_score[16] = ""; // each set's games, winner's first
    int winner = score.winner;
    for (int i = 0; i < score.sets[0] + score.sets[1]; i++) {
        size_t used = strlen(final_score);
        snprintf(final_score + used, sizeof final_score - used, "%s%d-%d", i ? " " : "", score.set_games[i][winner],
                 score.set_games[i][1 - winner]);
    }
    ESP_LOGI(TAG, "match: team %c wins %s", 'A' + winner, final_score);
    Clock clk;
    run(
        clk, [&](Canvas &canvas, double t) { animMatch(canvas, t, from, side, final_score); },
        [](double, const rx433_event_t &) {}, true, [](double) { return false; });
    for (;;) {
        vTaskDelay(portMAX_DELAY);
    }
}

// A point to `team`: the score moves on and the matching animation plays,
// then Swap Ends after a set.
static void scored(int team)
{
    Score before = match.score();
    bool swapped = before.endsSwapped();
    int side = swapped ? 1 - team : team;
    match.point(team);
    const Score &after = match.score();
    Board from = board_of(before, swapped), to = board_of(after, swapped);

    if (after.winner >= 0) {
        match_over(from, side, after);
    }
    if (after.sets[team] != before.sets[team]) {
        play([&](Canvas &canvas, double t) { animSet(canvas, t, from, to, side); }, SET_S);
    } else if (after.games[team] != before.games[team]) {
        play([&](Canvas &canvas, double t) { animGame(canvas, t, from, to, side); }, GAME_S);
    } else if (after.golden()) {
        play([&](Canvas &canvas, double t) { animGolden(canvas, t, from, to, side); }, GOLDEN_S);
    } else {
        play([&](Canvas &canvas, double t) { animPoint(canvas, t, from, to, side); }, POINT_S);
    }
    if (after.swaps != before.swaps) {
        swap_ends(to, board_of(after, !swapped));
    }
    show_board();
}

// Undo the last point. Undoing back across a change of ends just redraws
// the board the other way round.
static void undo()
{
    Score before = match.score();
    bool swapped = before.endsSwapped();
    if (!match.undo()) {
        return;
    }
    ESP_LOGI(TAG, "undo");
    Board from = board_of(before, swapped), to = board_of(match.score(), swapped);
    play([&](Canvas &canvas, double t) { animUndo(canvas, t, from, to); }, UNDO_S);
    show_board();
}

extern "C" void app_main(void)
{
    initArduino();

    panel.start();

    event_queue = xQueueCreate(16, sizeof(rx433_event_t));
    rx433_config_t cfg = {};
    cfg.gpio = CONFIG_RX433_GPIO;
    cfg.callback = on_rx;
    cfg.long_press_ms = LONG_PRESS_MS;
    ESP_ERROR_CHECK(rx433_start(&cfg));

    splash();
    Clock setup_clock;
    bind(setup_clock);
    Clock serve_clock;
    match.setFirstServer(select_serve(serve_clock));
    show_board();

    int held = -1;          // team whose button is down, -1 = none
    bool long_done = false; // long-press action already taken for this hold

    while (true) {
        rx433_event_t ev;
        xQueueReceive(event_queue, &ev, portMAX_DELAY);
        int team = team_of(ev.code);

        switch (ev.type) {
        case RX433_EVENT_PRESS:
            if (team < 0) {
                ESP_LOGI(TAG, "remote 0x%08lx unknown, ignored", (unsigned long)ev.code);
            } else {
                held = team;
                long_done = false;
            }
            break;

        case RX433_EVENT_LONG:
            if (team >= 0 && team == held && !long_done) {
                long_done = true;
                undo();
            }
            break;

        case RX433_EVENT_RELEASE:
            // Releases of presses consumed by a prompt arrive with nothing held
            // and are ignored.
            if (team >= 0 && team == held) {
                if (!long_done) {
                    ESP_LOGI(TAG, "point to team %c", 'A' + team);
                    scored(team);
                }
                held = -1;
            }
            break;
        }
    }
}
