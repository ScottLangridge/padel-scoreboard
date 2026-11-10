// Host-side renderer for the design clips: prints one 128-hex-char frame per
// line at 30 fps for the named clip, using the example scores from
// design/SPEC.md §6, so tools/check_frames.py can compare the port against
// design/frames/*.json pixel for pixel.
//
//   render_clips <clip>            frames as hex, one per line
//   render_clips <clip> <frame>    that frame as ASCII art

#include "anim.h"
#include "splash_frames.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static Board st(int lp, int rp, int lg, int rg, int ls, int rs, int sv = 0, int bx = 0)
{
    Board board;
    board.pts[0] = lp;
    board.pts[1] = rp;
    board.games[0] = lg;
    board.games[1] = rg;
    board.sets[0] = ls;
    board.sets[1] = rs;
    board.sv = sv;
    board.bx = bx;
    return board;
}

static const double INF = INFINITY;
static const double SPLASH_END = 9.23; // the Bind clock starts here (sim: SP.ON)

struct Clip {
    const char *name;
    double dur;
    void (*fn)(Canvas &, double);
};

static const Clip CLIPS[] = {
    {"splash", 11.43,
     [](Canvas &canvas, double t) {
         if (t < SPLASH_END) {
             drawFixed(canvas, SPLASH, sizeof(SPLASH) / sizeof(SPLASH[0]), t);
         } else {
             animBind(canvas, t - SPLASH_END, INF, INF);
             bindWipeIn(canvas, t - SPLASH_END);
         }
     }},
    {"bind", 8.4, [](Canvas &canvas, double t) { animBind(canvas, t, 3.6, 7.8); }},
    {"serve", 4.4, [](Canvas &canvas, double t) { animServe(canvas, t, 3.0, 0, 1); }},
    {"point-l", 0.75,
     [](Canvas &canvas, double t) { animPoint(canvas, t, st(2, 1, 3, 2, 1, 0, 0, 0), st(3, 1, 3, 2, 1, 0, 0, 1), 0); }},
    {"point-r", 0.75,
     [](Canvas &canvas, double t) { animPoint(canvas, t, st(2, 1, 3, 2, 1, 0, 0, 0), st(2, 2, 3, 2, 1, 0, 0, 1), 1); }},
    {"undo", 0.75, [](Canvas &canvas, double t) { animUndo(canvas, t, st(3, 1, 3, 2, 1, 0, 0, 1), st(2, 1, 3, 2, 1, 0, 0, 0)); }},
    {"game", 2.4,
     [](Canvas &canvas, double t) { animGame(canvas, t, st(3, 2, 3, 2, 1, 0, 0, 1), st(0, 0, 4, 2, 1, 0, 1, 0), 0); }},
    {"set", 3.4,
     [](Canvas &canvas, double t) { animSet(canvas, t, st(3, 2, 5, 4, 1, 0, 0, 1), st(0, 0, 0, 0, 2, 0, 1, 0), 0); }},
    {"match", 6.5, [](Canvas &canvas, double t) { animMatch(canvas, t, st(3, 2, 5, 3, 1, 0, 0, 1), 0, "6-4 6-3"); }},
    {"swap", 4.4,
     [](Canvas &canvas, double t) { animSwap(canvas, t, st(2, 1, 3, 2, 1, 0, 0, 0), st(1, 2, 2, 3, 0, 1, 1, 0), 3.5); }},
    {"golden", 2.3,
     [](Canvas &canvas, double t) { animGolden(canvas, t, st(3, 2, 3, 2, 1, 0, 1, 0), st(3, 3, 3, 2, 1, 0, 1, 1), 1); }},

    // Cases with no reference render, for eyeballing with the ASCII mode.
    {"undo-golden", 0.75, // 40-40 (GOLDEN) back to 40-30
     [](Canvas &canvas, double t) { animUndo(canvas, t, st(3, 3, 3, 2, 1, 0, 1, 1), st(3, 2, 3, 2, 1, 0, 1, 0)); }},
    {"game-golden", 2.4, // the golden point wins the game for the right
     [](Canvas &canvas, double t) { animGame(canvas, t, st(3, 3, 3, 2, 1, 0, 1, 1), st(0, 0, 3, 3, 1, 0, 0, 0), 1); }},
    {"set-r", 3.4, // right wins the set 7-5, its second
     [](Canvas &canvas, double t) { animSet(canvas, t, st(1, 3, 5, 6, 1, 1, 1, 1), st(0, 0, 0, 0, 1, 2, 0, 0), 1); }},
    {"match-3", 6.5, [](Canvas &canvas, double t) { animMatch(canvas, t, st(2, 3, 4, 5, 1, 1, 0, 1), 1, "4-6 6-3 7-5"); }},
    {"tiebreak", 0.75,
     [](Canvas &canvas, double t) {
         Board from = st(9, 10, 6, 6, 1, 0, 0, 1), to = st(10, 10, 6, 6, 1, 0, 1, 0);
         from.tiebreak = to.tiebreak = true;
         animPoint(canvas, t, from, to, 0);
     }},
};

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: render_clips <clip> [frame]\nclips:");
        for (const Clip &entry : CLIPS) {
            fprintf(stderr, " %s", entry.name);
        }
        fprintf(stderr, "\n");
        return 2;
    }
    const Clip *clip = nullptr;
    for (const Clip &entry : CLIPS) {
        if (strcmp(entry.name, argv[1]) == 0) {
            clip = &entry;
        }
    }
    if (!clip) {
        fprintf(stderr, "unknown clip %s\n", argv[1]);
        return 2;
    }

    Canvas canvas;
    if (argc > 2) {
        int frame = atoi(argv[2]);
        canvas.clear();
        clip->fn(canvas, frame / 30.0);
        printf("%s frame %d (t = %.3f s)\n", clip->name, frame, frame / 30.0);
        for (int y = 0; y < 16; y++) {
            for (int x = 0; x < 32; x++) {
                putchar(canvas.get(x, y) ? '#' : '.');
            }
            putchar('\n');
        }
        return 0;
    }

    // Like the simulator, the reference render includes one frame past the
    // end, clamped to the clip's duration (the looping clips wrap instead).
    int frames = (int)ceil(clip->dur * 30 - 1e-9) + 1;
    for (int frame = 0; frame < frames; frame++) {
        double t = frame / 30.0;
        if (t > clip->dur) {
            t = strcmp(clip->name, "splash") == 0 ? t - clip->dur : clip->dur;
        }
        canvas.clear();
        clip->fn(canvas, t);
        for (int y = 0; y < 16; y++) {
            printf("%08x", canvas.rows[y]);
        }
        putchar('\n');
    }
    return 0;
}
