# Emulator

Runs the firmware's screen flow on your computer, so you can try a change
without flashing the board.

```sh
make -C emulator            # build
emulator/emulator           # play it live in the terminal
make -C emulator check      # run the scripted scenarios
```

## Live

The panel is drawn in the terminal, and the log scrolls underneath it. The
keys stand in for the remotes:

| Key   | Does                                                       |
|-------|------------------------------------------------------------|
| `a` `b` | tap remote a or b (a short press scores on release)      |
| `A` `B` | hold remote a or b for 1.5 s (a long press: undo)        |
| `x`   | tap a stranger's remote, which the scoreboard should ignore |
| `q`   | quit                                                       |

Holding a key down also holds the remote, as long as the terminal's key
repeat starts within about half a second.

## Scripted

`emulator/emulator --script <file>` plays a script of presses on a virtual
clock. A whole match takes milliseconds, and the output is the same on every
run. It prints the firmware's log, the emulated remote events (`E ... remote:`
lines) and ASCII snapshots of the panel. The format is described at the top
of `emulator.cpp`, and `scenarios/match.txt` is a worked example.

`make -C emulator check` runs every `scenarios/*.txt` and compares the output
with its `.expected` file. After an intended change to the screen flow, run
`make -C emulator bless` to regenerate the expected output, and check the
diff before committing it.

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
