# Agent guide

An ESP32-S3 padel scoreboard. It shows the score of one match on a single
32×16 HUB12 (P10) LED panel, and two 433 MHz remote buttons control it, one
for each team.

- **Short press:** a point to that remote's team, counted when the button is
  released.
- **Long press** (1 s, either remote): undo the last point.

At power on the panel plays a splash. Then it asks for the left team's
remote, then the right team's, and then for the team that serves first.
Remotes that weren't bound are ignored. After each set (except the last) it
shows SWAP / ENDS until a remote is pressed, then mirrors the board. MATCH
and the final score stay on the panel until the power is cycled. A press
that arrives during an animation is queued, not dropped.

The firmware is ESP-IDF 5.5 with arduino-esp32 as a component. Arduino is
there only because the DMD2 panel library needs it.

## Where things are

### Firmware (`main/`)

| File | What it is |
|---|---|
| `padel-scoreboard.cpp` | `app_main`: the screen flow (splash → bind → select serve → board → …), the remote events, the frame loop at 30 fps, and the log lines the emulator reads |
| `score.h` / `score.cpp` | `Match`: the scoring rules (best of 3, golden point, tiebreak at 6-6, serve rotation, changes of ends, undo history). No hardware dependencies |
| `anim.h` / `anim.cpp` | `drawBoard` and every animation, each a pure function of time and the board before and after. Ported from the reference |
| `canvas.h` | `Canvas`, the 32×16 one-bit frame the animations draw on, with the reference's drawing rules (`jsround`, clipping), and `FixedFrame` |
| `hub12.h` | `Hub12`: drives the panel through DMD2's `SoftDMD`, with OE on LEDC PWM and the scan run from an `esp_timer` |
| `fonts.h`, `splash_frames.h` | generated (see [Generated files](#generated-files)) |
| `Kconfig.projbuild` | the "LED Matrix" and "Scoreboard" menuconfig menus |
| `idf_component.yml` | pulls in `espressif/arduino-esp32` |

Teams and sides are different things. `Score` counts by **team** (0 is the
team bound as left at the start). `Board` (in `anim.h`) draws by **side** of
the panel (0 is the left half). They differ once the teams have changed ends,
and `padel-scoreboard.cpp` converts between them.

### Components (`components/`)

- `rx433/`: the 433 MHz receiver for fixed-code OOK remotes (EV1527 /
  PT2262). It captures the DATA pin with RMT, decodes the frames, and turns a
  held button's repeated frames into one PRESS, one LONG (after
  `long_press_ms`) and one RELEASE. It only decodes; the app decides what
  each code means. `include/rx433.h` documents the API, and `Kconfig` has its
  settings.
- `dmd2/`: an IDF wrapper around the upstream
  [DMD2](https://github.com/freetronics/DMD2) library, which is a git
  submodule in `dmd2/DMD2`. Don't modify the submodule; work around it in
  `hub12.h` or the wrapper's `CMakeLists.txt`. A fresh clone needs
  `git submodule update --init`.

### Design (`design/`)

What the panel should show. `design/README.md` introduces the package.

- `SPEC.md`: the spec. It covers the coordinates, the resting board, the
  inputs, the screen flow and a timed breakdown of every animation. The
  firmware cites it by section (`SPEC §5.4`).
- `p10-sim.html`: a browser simulator. The `animate` section of its script
  is the reference implementation that `anim.cpp` is ported from.
- `frames/*.json`: the reference renders of every clip at 30 fps, rendered
  from `p10-sim.html`. `test/` compares the port against them pixel for
  pixel. SPEC §6 gives the frame format and the example scores they use.
- `fonts.json`: the three bitmap fonts.

The `gifs/` folder that `design/README.md` and SPEC §6 mention isn't in this
repo.

### Host builds

- `test/`: host checks. `score_test.cpp` tests `Match`. `render_clips.cpp`
  renders each clip with `anim.cpp`, and `tools/check_frames.py` compares
  the result with `design/frames/`.
- `emulator/`: runs the real `padel-scoreboard.cpp`, `anim.cpp` and
  `score.cpp` on the host, against stand-in headers in `emulator/shims/`
  (Arduino, DMD2, FreeRTOS, esp_timer, the drivers, and rx433 replaced by
  emulated remotes). You can play it live in the terminal or drive it with a
  script. See `emulator/README.md`.

### Tools (`tools/`)

| Tool | Does |
|---|---|
| `render_reference.mjs` | renders `design/frames/*.json` from `p10-sim.html` (Node) |
| `check_frames.py` | compares `test/render_clips` output with `design/frames/` |
| `fonts_to_header.py` | `design/fonts.json` → `main/fonts.h` |
| `frames_to_header.py` | a `design/frames/*.json` clip → a C header of fixed frames |

## Configuration

The settings are in menuconfig (`idf.py menuconfig`) and are stored in
`sdkconfig`, which is committed. `sdkconfig.defaults` only sets
`CONFIG_FREERTOS_HZ=1000`.

- **LED Matrix** (`main/Kconfig.projbuild`): brightness (OE PWM duty; keep
  it low on USB power), the row scan period, and the GPIOs for OE, A, B, CLK,
  LAT and R.
- **Scoreboard**: `SCOREBOARD_FLIP_SERVE_BOXES`, for a panel mounted on the
  other side of the court.
- **433MHz Receiver** (`components/rx433/Kconfig`): the DATA GPIO, the
  expected frame length, the release timeout, frame logging and the task
  priority.

The emulator reads the same `sdkconfig` (through `emulator/gen_sdkconfig.py`),
so a changed setting changes the emulator too.

## Building and flashing

The firmware needs ESP-IDF 5.5. On the owner's machine:

```sh
bash -c '. ~/.espressif/tools/activate_idf_v5.5.6.sh && idf.py build'
```

`idf.py build` is enough to check that a change compiles. Flashing
(`idf.py flash monitor`) is the owner's job; don't make them flash the board
to find out whether something works (see below).

## Testing a change

The host checks and the emulator need only `g++`, `make` and Python 3. Run
both after any change to `main/`:

```sh
make -C test check        # scoring model + every animation, pixel for pixel
make -C emulator check    # builds the emulator, runs emulator/scenarios/*.txt
```

To test a particular behaviour, write an emulator script with `expect`
checks and run it:

```sh
make -C emulator                       # build (check does this too)
emulator/emulator --script <file>      # run it; exits 0 on PASS, 1 on FAIL
```

A script can play whole matches, reach any score (a tiebreak, a golden
point, the end of a match), look at the panel at any moment down to single
pixels, and check the result. It runs on a virtual clock, so a whole match
takes milliseconds. The steps are listed at the top of `emulator/script.cpp`,
and `emulator/README.md` explains them, with notes for agents. If the
behaviour is worth keeping covered, add the script to `emulator/scenarios/`.

The host builds don't cover everything:

- the radio decoding in `rx433.c` (the emulator replaces it with ideal
  remote events);
- the panel's scan timing, brightness and pin handling in `hub12.h`, and
  DMD2 itself.

Changes there need the real board. Say so, and say what the owner should
look for.

## Changing an animation

`p10-sim.html` is the source of truth, and `anim.cpp` must match it pixel
for pixel.

- Don't edit `design/p10-sim.html`. If the design itself has to change, ask
  the owner.
- Don't reorder or regroup the floating-point arithmetic in `anim.cpp`; it
  has to give the same results as the JavaScript. For the same reason,
  everything is built with `-ffp-contract=off` (`main/CMakeLists.txt`,
  `test/Makefile`, `emulator/Makefile`). Keep it.
- `make -C test check` is the proof. When a clip fails, `check_frames.py`
  prints the first differing frame. `test/render_clips <clip> <frame>` prints
  any frame as ASCII.

## Generated files

Don't edit these by hand. Regenerate them from the repo root:

```sh
python3 -I tools/fonts_to_header.py design/fonts.json main/fonts.h
python3 -I tools/frames_to_header.py design/frames/splash.json main/splash_frames.h SPLASH 9.233
node tools/render_reference.mjs design/p10-sim.html design/frames
```

The cutoff of 9.233 s ends the splash just before the hand-over to Bind
(SPEC §5.1), which the firmware draws itself. With these arguments the
committed headers are reproduced exactly.

`emulator/build/sdkconfig.h` is generated by the emulator's Makefile.

## Code conventions

The owner reads this code closely, so readability matters.

- **Names say what things are.** One-letter names only for `w`/`h`
  (including `Canvas::W`/`H`), `x`, `y`, `i` (a loop index) and `t`
  (animation time, in seconds). Nested loop counters are `i_<thing>`
  (`i_row`, `i_col`).
- **Accepted abbreviations:** `pts`, `ch`, `dx`/`dy`/`dt`/`d_left`/`d_right`,
  `clk`, `cfg`, `rx`, `buf`, `tmp`, `len`, `ms`, `TAG`, `arg`, `ctx`,
  `x0`/`y0`/`t0`, `mix` and `jsround`. Spell out anything else.
- **Time constants** take a `T_` prefix (`T_SWEEP_START`).
- **Case style follows the file:** camelCase in `main/anim.*` and
  `test/render_clips.cpp`, snake_case in `main/padel-scoreboard.cpp`,
  `main/score.*`, `components/rx433/`, the emulator and the Python tools.
- **Comments** explain why, and they cite the spec by section where it
  applies. A new file opens with a comment that says what it is (in a
  header, after `#pragma once`; a `.cpp` with its own header can rely on
  that one).
- **One meaning per name.** Don't reuse a name for a different thing, and
  don't shadow a name from an outer scope.
- **Side effects are visible.** A function that changes state is named as an
  action, not a query.
- The firmware logs the screen it is on, the score line and the remote
  bindings. The emulator's scripts read those log lines, so if you change
  their wording, update `emulator/script.cpp` and run
  `make -C emulator check`.
