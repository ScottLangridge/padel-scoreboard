# Agent guide

An ESP32-S3 padel scoreboard: one 32×16 HUB12 LED panel, scored with two
433 MHz remote buttons.

## Where things are

- `main/`: the firmware. `padel-scoreboard.cpp` is the screen flow and the
  inputs, `anim.cpp` draws the board and every animation, and `score.cpp`
  holds the scoring rules. `hub12.h` drives the panel.
- `components/rx433/`: the 433 MHz receiver and decoder.
- `design/`: what the panel should show. `SPEC.md` is the spec, and
  `p10-sim.html` is the reference implementation that `anim.cpp` is ported
  from (and checked against, pixel for pixel).
- `test/`: host checks of the scoring model and of the animations against
  the reference frames.
- `emulator/`: runs the firmware on the host, live in the terminal or driven
  by test scripts. See `emulator/README.md`.
- `tools/`: generators for the font and frame headers, and the frame checker.

## Testing a change

Don't make the owner flash the board to find out whether something works.
Use the emulator: a script can play whole matches, reach any score (a
tiebreak, a golden point, the end of a match), look at the panel at any
moment down to single pixels, and check the result. It runs on a virtual
clock, so a match takes milliseconds.

```sh
make -C test check        # scoring model + animations, pixel for pixel
make -C emulator check    # the emulator scenarios (emulator/scenarios/*.txt)
emulator/emulator --script <file>   # your own script; steps are listed in emulator/script.cpp
```

To test some behaviour, write a script with `expect` checks and run it. If
the behaviour is worth keeping covered, add the script to
`emulator/scenarios/`.

The firmware build needs ESP-IDF 5.5. On the owner's machine:

```sh
bash -c '. ~/.espressif/tools/activate_idf_v5.5.6.sh && idf.py build'
```

The emulator covers the screen flow, the scoring and the button handling,
but not the radio decoding in `rx433.c` or the panel's scan timing. Those
need the real board.

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
- `anim.cpp` must stay pixel-exact with the reference, so don't reorder or
  regroup its floating-point arithmetic.
- Don't edit `design/p10-sim.html`, or the generated `main/fonts.h` and
  `main/splash_frames.h`.
