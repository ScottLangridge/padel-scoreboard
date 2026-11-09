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
static const double INF = INFINITY;                // "no press yet" for the prompts

static Hub12 dmd;
static DMDFrame back(32, 16); // drawn here, then copied, so a scan never sees a half-drawn frame
static Canvas canvas;
static Match match;
static QueueHandle_t s_events;
static uint32_t codes[2]; // remote code of team 0 (bound as left) and team 1

static void on_rx(const rx433_event_t *ev, void *)
{
    xQueueSend(s_events, ev, 0);
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

static void push(const Canvas &c)
{
    for (int y = 0; y < Canvas::H; y++) {
        for (int x = 0; x < Canvas::W; x++) {
            back.setPixel(x, y, c.get(x, y) ? GRAPHICS_ON : GRAPHICS_OFF);
        }
    }
    dmd.copyFrame(back, 0, 0);
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
    int64_t next = esp_timer_get_time();
    while (true) {
        double t = clk.now();
        if (done(t)) {
            return;
        }
        canvas.clear();
        render(canvas, t);
        push(canvas);

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
            if (xQueueReceive(s_events, &ev, ticks)) {
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

// Service box (0 top, 1 bottom) for the team on panel side `sv` after
// `played` points of the game: the first serve of a game is from the server's
// right-hand box, then they alternate. Each team's right is on the opposite
// side of the court; by default the top of the panel is the right-hand box of
// the team on the right (menuconfig flips it for the other mounting).
static int serve_box(int sv, int played)
{
    int box = (played % 2) ^ (sv == 0 ? 1 : 0);
#if CONFIG_SCOREBOARD_FLIP_SERVE_BOXES
    box ^= 1;
#endif
    return box;
}

// The score as laid out on the panel: Board side 0 is the left half. Team 0
// starts on the left and the sides mirror on every change of ends.
static Board board_of(const Score &s, bool swapped)
{
    int l = swapped ? 1 : 0, r = 1 - l;
    Board b;
    b.pts[0] = s.points[l];
    b.pts[1] = s.points[r];
    b.tiebreak = s.tiebreak;
    b.games[0] = s.games[l];
    b.games[1] = s.games[r];
    b.sets[0] = s.sets[l];
    b.sets[1] = s.sets[r];
    b.sv = s.server == l ? 0 : 1;
    b.bx = serve_box(b.sv, s.points[0] + s.points[1]);
    return b;
}

static void show_board()
{
    const Score &s = match.score();
    ESP_LOGI(TAG, "sets %u-%u games %u-%u points %u-%u%s serve %c%s", s.sets[0], s.sets[1], s.games[0], s.games[1],
             s.points[0], s.points[1], s.tiebreak ? " (tiebreak)" : "", 'A' + s.server,
             s.endsSwapped() ? " [A on the right]" : "");
    canvas.clear();
    drawBoard(canvas, board_of(s, s.endsSwapped()));
    push(canvas);
}

// Boot splash (SPEC §5.1). Any remote press skips the rest; that press
// doesn't bind anything.
static void splash()
{
    static const int N = sizeof(SPLASH) / sizeof(SPLASH[0]);
    double length = 0;
    for (const FixedFrame &f : SPLASH) {
        length += f.ms / 1000.0;
    }
    Clock clk;
    bool skipped = false;
    run(
        clk, [](Canvas &c, double t) { drawFixed(c, SPLASH, N, t); },
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
    double pressL = INF, pressR = INF;
    ESP_LOGI(TAG, "bind: press the left remote");
    run(
        clk,
        [&](Canvas &c, double t) {
            animBind(c, t, pressL, pressR);
            bindWipeIn(c, t);
        },
        [&](double t, const rx433_event_t &ev) {
            if (ev.type != RX433_EVENT_PRESS) {
                return;
            }
            if (pressL == INF) {
                pressL = t;
                codes[0] = ev.code;
                ESP_LOGI(TAG, "remote 0x%08lx is left (team A); press the right remote", (unsigned long)ev.code);
            } else if (pressR == INF && t >= pressL + BIND_AFTER_PRESS_S) {
                if (ev.code == codes[0]) {
                    ESP_LOGI(TAG, "remote 0x%08lx is already left, press the other one", (unsigned long)ev.code);
                    return;
                }
                pressR = t;
                codes[1] = ev.code;
                ESP_LOGI(TAG, "remote 0x%08lx is right (team B)", (unsigned long)ev.code);
            }
        },
        true, [&](double t) { return t >= pressR + BIND_AFTER_PRESS_S; });
}

// Select serve (SPEC §5.3), on a clock starting as Bind's last wipe ends so
// the prompt reads SELECT first. Returns the side (= team, nothing is swapped
// yet) that serves first; ends with the fresh board on the panel.
static int select_serve(const Clock &clk)
{
    double pressT = INF;
    int side = 0;
    ESP_LOGI(TAG, "select serve: press a remote");
    run(
        clk, [&](Canvas &c, double t) { animServe(c, t, pressT, side, serve_box(side, 0)); },
        [&](double t, const rx433_event_t &ev) {
            int team = team_of(ev.code);
            if (ev.type == RX433_EVENT_PRESS && pressT == INF && team >= 0) {
                pressT = t;
                side = team;
                ESP_LOGI(TAG, "team %c serves first", 'A' + team);
            }
        },
        true, [&](double t) { return t >= pressT + SERVE_AFTER_PRESS_S; });
    return side;
}

// Swap ends (SPEC §5.7): the board folds away, the prompt waits for a press
// from either bound remote (the acknowledgement; it doesn't score), the
// board emerges mirrored.
static void swap_ends(const Board &a, const Board &b)
{
    ESP_LOGI(TAG, "swap ends");
    Clock clk;
    double pressT = INF;
    run(
        clk, [&](Canvas &c, double t) { animSwap(c, t, a, b, pressT); },
        [&](double t, const rx433_event_t &ev) {
            if (ev.type == RX433_EVENT_PRESS && pressT == INF && t >= 0.5 && team_of(ev.code) >= 0) {
                pressT = t;
            }
        },
        true, [&](double t) { return t >= pressT + SWAP_AFTER_PRESS_S; });
}

// Match won (SPEC §5.5): plays once and holds the final score until power
// is cycled. Never returns; remote presses are ignored.
[[noreturn]] static void match_over(const Board &a, int side, const Score &s)
{
    char final_score[16] = ""; // each set's games, winner's first
    int w = s.winner;
    for (int i = 0; i < s.sets[0] + s.sets[1]; i++) {
        snprintf(final_score + strlen(final_score), sizeof final_score - strlen(final_score), "%s%d-%d", i ? " " : "",
                 s.set_games[i][w], s.set_games[i][1 - w]);
    }
    ESP_LOGI(TAG, "match: team %c wins %s", 'A' + w, final_score);
    Clock clk;
    run(
        clk, [&](Canvas &c, double t) { animMatch(c, t, a, side, final_score); },
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
    bool sw = before.endsSwapped();
    int side = sw ? 1 - team : team;
    match.point(team);
    const Score &after = match.score();
    Board a = board_of(before, sw), b = board_of(after, sw);

    if (after.winner >= 0) {
        match_over(a, side, after);
    }
    if (after.sets[team] != before.sets[team]) {
        play([&](Canvas &c, double t) { animSet(c, t, a, b, side); }, SET_S);
    } else if (after.games[team] != before.games[team]) {
        play([&](Canvas &c, double t) { animGame(c, t, a, b, side); }, GAME_S);
    } else if (after.golden()) {
        play([&](Canvas &c, double t) { animGolden(c, t, a, b, side); }, GOLDEN_S);
    } else {
        play([&](Canvas &c, double t) { animPoint(c, t, a, b, side); }, POINT_S);
    }
    if (after.swaps != before.swaps) {
        swap_ends(b, board_of(after, !sw));
    }
    show_board();
}

// Undo the last point. Undoing back across a change of ends just redraws
// the board the other way round.
static void undo()
{
    Score before = match.score();
    bool sw = before.endsSwapped();
    if (!match.undo()) {
        return;
    }
    ESP_LOGI(TAG, "undo");
    Board a = board_of(before, sw), b = board_of(match.score(), sw);
    play([&](Canvas &c, double t) { animUndo(c, t, a, b); }, UNDO_S);
    show_board();
}

extern "C" void app_main(void)
{
    initArduino();

    dmd.start();

    s_events = xQueueCreate(16, sizeof(rx433_event_t));
    rx433_config_t cfg = {};
    cfg.gpio = CONFIG_RX433_GPIO;
    cfg.callback = on_rx;
    cfg.long_press_ms = LONG_PRESS_MS;
    ESP_ERROR_CHECK(rx433_start(&cfg));

    splash();
    Clock setup;
    bind(setup);
    Clock serve;
    match.setFirstServer(select_serve(serve));
    show_board();

    int held = -1;          // team whose button is down, -1 = none
    bool long_done = false; // long-press action already taken for this hold

    while (true) {
        rx433_event_t ev;
        xQueueReceive(s_events, &ev, portMAX_DELAY);
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
