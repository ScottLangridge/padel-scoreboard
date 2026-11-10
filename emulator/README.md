# Emulator

Runs the firmware's screen flow on your computer, so you can try a change
without flashing the board, and test whole matches in moments.

```sh
make -C emulator                     # build
emulator/emulator                    # play it live in the terminal
emulator/emulator --script <file>    # run a test script
make -C emulator check               # run every scenarios/*.txt
```

## Live

The panel is drawn in the terminal, and the log scrolls underneath it. The
keys stand in for the remotes:

| Key     | Does                                                        |
|---------|-------------------------------------------------------------|
| `a` `b` | tap remote a or b (a short press scores on release)         |
| `A` `B` | hold remote a or b for 1.5 s (a long press: undo)           |
| `x`     | tap a stranger's remote, which the scoreboard should ignore |
| `q`     | quit                                                        |

Holding a key down also holds the remote, as long as the terminal's key
repeat starts within about half a second.

## Test scripts

A script plays the firmware on a virtual clock. Time jumps straight to the
next thing that happens, so a whole match takes milliseconds and every run
is identical. Its steps wait for the firmware to react, not for fixed times,
so you can set up any score quickly. This gets to a tiebreak:

```
start                    # skip the splash, bind a (team A) and b (team B), A serves
game a
game b                   # ... alternating, up to 6-6
expect score "games 6-6 points 0-0 (tiebreak) serve A"
point a
expect score "points 1-0 (tiebreak) serve B"
snap                     # print the panel
```

The steps are listed at the top of `script.cpp`:
- **Playing:** `start`, `point`, `game`, `set`, `swap` (acknowledge SWAP /
  ENDS) and `undo`.
- **Low level:** `tap`, `hold`, `wait <seconds>`, `wait idle`, `wait log` and
  `at`.
- **Checking:** `snap`, `pixel`, `expect score`, `expect log`, `expect pixel`
  and `expect board`.

The output is the firmware's log, the emulated remote events
(`E (…) remote:` lines), each step as it runs (`> …`) and anything the
script prints. A failed check stops the run with `FAIL`, the panel and the
last log lines, and exits 1. Otherwise the run ends with `PASS` and exits 0.

Options:

| Option            | Does                                                         |
|-------------------|--------------------------------------------------------------|
| `--quiet`         | leave out the log: just the steps, snapshots and the result  |
| `--frames <file>` | write every frame shown: `<µs> <hex>` per line, 16 rows of 8 hex digits, MSB = leftmost pixel |
| `--watch`         | show it playing in the terminal                              |
| `--speed <n>`     | with `--watch`: at n times real time, e.g. 100               |

`scenarios/` has a full match and a tiebreak. `make -C emulator check` runs
them all.

### Notes for agents

- The script reads the firmware's state from its log: the screen, the score
  and which remote is which team. The score line is
  `sets A-B games A-B points A-B [(tiebreak)] serve <team> [[A on the right]]`,
  always in team order, A first.
- To look at a particular moment, put a `snap` or `pixel` there, after a
  `wait` or `wait log`. Or record `--frames` and look up the time
  afterwards.
- After the last point of a set, the scoreboard shows SWAP / ENDS until a
  remote acknowledges it. A script has to `swap` before scoring again.

## What is real and what isn't

- **Real:** `main/padel-scoreboard.cpp`, `anim.cpp`, `score.cpp` and
  `hub12.h` compile unchanged, with the menuconfig settings from `sdkconfig`.
- **Stand-ins:** `shims/` replaces DMD2, FreeRTOS, `esp_timer`, logging,
  LEDC and GPIO with just enough to run that code on the host.
- **The remotes:** these are emulated at the rx433 component's interface.
  Presses become the PRESS / LONG / RELEASE events that rx433 sends, modelled
  on its press tracking: confirming a new code, the long press, release
  timing, and one remote cutting in on another. The radio decoding in
  `components/rx433/rx433.c` doesn't run, so receiver noise and range need the
  real hardware.
- **The panel:** it is shown frame by frame as the firmware copies frames to
  it. Scan timing and brightness are not emulated.
