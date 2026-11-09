# Padel scoreboard display package

Everything a coding agent needs to implement the display side of the padel
scoreboard on a 32×16 yellow P10 LED matrix.

Start with `SPEC.md`. It covers the coordinate system, the resting board layout,
the inputs, the screen flow (which animation plays when, and how they chain),
and a timed breakdown of every animation.

Then use:

- `p10-sim.html` — the browser simulator. The `animate` section of its script is
  the reference implementation of every animation; port from it.
- `fonts.json` — the bitmap fonts.
- `frames/` and `gifs/` — pixel-exact 30 fps renders of every clip for checking
  a port (or for playing the fixed clips back directly).
