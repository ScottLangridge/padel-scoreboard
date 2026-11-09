# Padel Scoreboard — Display Specification

Target: a single-colour (yellow) 32×16 P10 LED matrix, two wireless/wired button
controllers (one per team), driven by a microcontroller. This document is the
complete description of what the panel shows, when, and how screens chain. The
reference implementation of every animation is the JavaScript in `p10-sim.html`
(the `animate` section, before the `SIMULATOR` banner); when this document and
the code disagree, the code wins.

---

## 1. Conventions

- **Pixels**: `x` 0..31 left→right, `y` 0..15 top→bottom. Each LED is on or off
  (no brightness levels are used anywhere).
- **Frame rate**: everything is authored at 30 fps. Times below are seconds from
  the start of the animation in question.
- **Easing**: `ease(p) = 1 − (1 − p)^3` (ease-out cubic) is used for nearly every
  movement. `easeBack(p)` is an overshoot used only for text dropping in
  (constant `c = 1.7`). "Progress from a to b" means `ease(clamp((t−a)/(b−a)))`.
- **Fonts** (`fonts.json`, glyphs as rows of `#`/`.`):
  - `5x7` — digits, letters, `-`. Advance 6 px (5 + 1 gap).
  - `3x5` — digits and `-` only. Advance 4 px. Used for games.
  - `4x7_condensed` — G O L D E N S C T R V only. Advance 5 px. Used where a word
    is too wide in 5x7 (GOLDEN, SELECT / SERVE).
  - A word is centred with `x = round((32 − (width − 1)) / 2)`.
- **Sides**: "left" and "right" are the halves of the panel as the players see it.
  All score data is stored per side and mirrored on a swap of ends (see §5.7).

---

## 2. The resting board

This is what the panel shows between events. Nothing on it moves.

```
col: 0         1         2         3
     01234567890123456789012345678901
 0   .....#...###.........#...###...   points, 5x7
 1   ....##..#...#.......##..#...#..
 2   ...#.#..#..##......#.#..#..##..
 3   ..#..#..#.#.#.####.#.#..#.#.#..   ← dash, cols 14–17 row 3
 4   ..#####.##..#......#####.##..#..
 5   .....#..#...#.........#..#...#..
 6   .....#...###..........#...###...
 7   ................................   ← ball lane (empty at rest)
 8   ################################   ← dividing line, full width
 9   ..#..........................#..
10   ..#...###....####......###...#..   games 3x5 | serve triangle | games 3x5
11   ..#...#.#....###.......#.#...#..
12   ###...#.#....##........#.#...#..   ← set cells (cols 0–1 / 30–31)
13   ###...#.#....#.........#.#...#..
14   ###...###..............###...#..
15   ###..........................#..
```
(Example: 40–40 would be GOLDEN, see §5.6. Shown: 40–15, games 0–0, sets 1–0,
left team serving to the top corner.)

| Element | Where | Rule |
|---|---|---|
| Points, left | 5x7, row 0, **right-aligned so its last column is 13** | values `0 15 30 40`. `x = 13 − (width − 1)`, width = chars × 6 |
| Points, right | 5x7, row 0, **starts at column 19** | |
| Dash | row 3, cols 14–17 | always present with the points |
| Dividing line | row 8, cols 0–31 | always present |
| Games, left | 3x5 at (6, 10) | single digit 0–7 |
| Games, right | 3x5 at (23, 10) | |
| Set posts | col 2 and col 29, rows 9–15 | always present |
| Set cells | left cols 0–1, right cols 30–31. **First set won: rows 12–15. Second set won: rows 8–11** | 2-px wide blocks against the panel edge; best-of-three so max two |
| Serve triangle | 4×4 right-angled triangle at the bottom centre, see below | exactly one shown at rest |

### 2.1 Serve triangle

Encodes who serves (which half it sits in) and which service box (which way its
right angle points). `side` 0 = left team, 1 = right team. `box` 0 = top, 1 = bottom.

```
origin  = (side ? 18 : 13,  box ? 14 : 10)
for j in 0..3, for i in 0..(3−j):
    set( origin.x + (side ? −i : i),  origin.y + (box ? −j : j) )
```
So left/top fills cols 13–16 × rows 10–13 with the right angle at (13,10);
right/bottom fills cols 15–18 × rows 11–14 with the right angle at (18,14), etc.
The four variants each sit one column and one row toward their corner, which is
intentional.

Rules: at the start of a game the first serve is from the right-hand service
box of the serving team, the box toggles after every point, and serve passes
to the other team at the end of every game. Each team's right-hand box is on
the opposite side of the court, so `box = (points played) mod 2`, inverted for
the left team: by default the top of the panel is the right team's right-hand
box and the left team's first serve points down. Which side of the court is
the top depends on how the panel is mounted, so the firmware has a switch to
invert the mapping.

---

## 3. Inputs

Two controllers, each with one button. Events the firmware must distinguish:

| Event | Meaning (on the resting board) |
|---|---|
| Short press, left controller | Point to left |
| Short press, right controller | Point to right |
| Undo gesture (recommended: long press ≥ 1 s on either controller) | Undo the last point |
| Any press while a prompt is waiting (Bind, Select Serve, Swap Ends) | Acknowledge / select (see each prompt) |

Presses arriving while an animation is playing should be queued and applied when
it finishes, not dropped. Point and Undo are short (0.75 s) precisely because
they happen constantly; the long ones (Game, Set) only happen a few times a match.

---

## 4. Screen flow

```
POWER ON
  │
  ▼
SPLASH  (§5.1)  plays once, ends with the dividing line alone at row 8
  │
  ▼
BIND  (§5.2)    "PRESS / LEFT" … waits for a press → that controller = left
  │             "PRESS / RIGHT" … waits for a press from the OTHER controller
  ▼
SELECT SERVE (§5.3)  waits for a press from either side → that side serves first
  │
  ▼
BOARD (§2)  ◄──────────────────────────────────────────────┐
  │                                                        │
  ├─ press L/R ─► POINT (§5.4)  or GOLDEN (§5.6) if 40-40 ─┤
  │                └─ if that point wins the game:         │
  │                     GAME (§5.5)                        │
  │                       └─ if it also wins the set:      │
  │                            SET instead of GAME         │
  │                              └─ if it also wins match: │
  │                                   MATCH (holds the final score until power off)
  │                     after SET (not MATCH): SWAP ENDS (§5.7) ────────────────────────┤
  ├─ undo ─────► UNDO (§5.4) ───────────────────────────────┤
```

Precedence when a point ends a game: play **one** of Point → Game → Set → Match,
whichever is the highest that applies (each already contains the ball and the
score change, so they are not stacked). Swap Ends plays after the Set
animation has fully finished, and never after Match.

Deuce rule: **golden point**. At 40–40 the points row shows GOLDEN until the next
point; that point always wins the game (so it plays Game/Set/Match directly, never
a plain Point). No advantage display is needed.

Tiebreak: not designed yet. Suggested: show tiebreak points as plain numbers in
the points row using the same Point animation; everything else unchanged.

---

## 5. Animations

Each animation is defined by what it draws over time; all are deterministic
functions of time and the score before/after. `frames/*.json` and `gifs/*.gif`
are 30 fps renders of each one with the example scores used in the simulator,
for visual reference and for pixel-exact checking of a port. The ones marked
**parametric** must be re-implemented with live score values; the ones marked
**fixed** can be played back from the frame files as they are.

Shared building blocks (names as in the code):

- **`roll(old, new, dir)`** — odometer roll inside a window the height of the
  font: old value moves out by (height+1) rows and new value moves in behind it,
  `dir` +1 = new comes up from below, −1 = new comes down from above. Eased.
- **`winBall(dir)`** — the winning ball: 4 pixels in a row along row 7 (the lane
  above the line), head leading, travelling the whole width in 0.35 s
  (left→right: head from x = −1 to 34; right→left: 32 to −3). Where the head is,
  the line pixel directly beneath it and the one behind it are **off** (a shadow
  in the line). It flies toward the side that won the point.
- **`serveFlip(from, to, p)`** — the serve triangle shrinks (size 4→0) over the
  first half of `p` and regrows at its new position/orientation over the second
  half. Takes 0.25 s wherever it is used.
- **`altWords(A, B)`** — prompt text that alternates: hold A, roll up to B, hold
  B, roll back to A. Two paces exist: Bind/Select-Serve/Swap use hold 0.9 s /
  roll 0.2 s; (Swap uses hold 0.6 / roll 0.15 — see §5.7).
- **`tri(x, dir, nudge)`** — a 3-column arrowhead pointing in `dir` with its tip
  at column x, rows 10–14 (tip on row 12). "Beckoning" = `nudge` alternates 0/1
  every 0.5 s so the arrowhead shifts one pixel toward the edge and back.
- **`barWipe(dir, before, after)`** — a solid lit bar sweeps across the panel:
  its leading edge crosses the width over 0.3 s and its trailing edge over 0.35 s
  starting 0.25 s later, so the bar is at most ~1/3 panel wide. Ahead of the bar
  the `before` picture is still drawn; behind it the `after` picture is revealed.
  Total 0.6 s.

### 5.1 Splash — fixed — `frames/splash.json`

Plays once at power on. 11.4 s in the reference render; on the board stop at the
"line alone" frame (8.7 s) and hand over to Bind.

| t (s) | What happens |
|---|---|
| 0.00–1.45 | A single pixel falls in at x = 16 from above the panel, lands on row 15, bounces three times with diminishing height (peaks rows 10, 13, 14) and comes to rest on row 15. |
| 1.45–1.80 | Rest. Nothing moves. |
| 1.80–2.40 | The court grows from the ball: the floor (row 15) spreads from x = 16 to both edges (0–50 % of this window); the net zig-zags upward from the ball — (16,14) (15,13) (16,12) then the tape (15,11)+(16,11) — (10–50 %); once the floor reaches the edges the back glass rises at x = 0 and x = 31 from row 14 up to row 2 (45–100 %). |
| 2.40–2.50 | Hold on the finished court. |
| 2.50–8.28 | **Board export rally** (`frames/rally-board-export.json`, 5.78 s, 125 frames): players walk in from the net to the baselines; serve; return; volley; lob; smash out of court. This section is frame data from the original board firmware with the glass extended 3 rows taller; play it verbatim. |
| 8.28–8.73 | Everything above the floor dissolves away pixel by pixel in a fixed pseudo-random order (seeded LCG, see `perm` in code), leaving only the floor. |
| 8.73–9.13 | The floor rises from row 15 to row 8 (eased). This is the dividing line of every later screen. |
| 9.13→ | **Hand-over to Bind.** The Bind screen wipes in vertically from the line outward: rows within `h` of row 8 are shown where `h` goes 0→8 over 0.35 s. (The reference render continues for 2.2 s of the Bind screen so the wipe can be seen.) |

### 5.2 Bind controllers — fixed (two waiting loops + two transitions) — `frames/bind.json`

Purpose: learn which physical controller is left and which is right.

| State | Display | Exit |
|---|---|---|
| Wait LEFT | Dividing line. Points row alternates **PRESS** / **LEFT** (hold 0.9 s, roll 0.2 s). An arrowhead at the left edge (tip x = 4, pointing left) beckons every 0.5 s. | Any controller press. That controller becomes **left**. |
| Transition | `barWipe` left→right (0.6 s) reveals the next state. | |
| Wait RIGHT | Same, with **PRESS** / **RIGHT** and the arrowhead at the right edge (tip x = 27, pointing right). | A press from the **other** controller (ignore the already-bound one). It becomes right. |
| Transition | `barWipe` right→left (0.6 s) reveals Select Serve, whose prompt clock starts as the wipe ends so it reads SELECT before SERVE. | |

Reference render presses at 3.6 s and 7.8 s.

### 5.3 Select serve — fixed loop, then parametric exit — `frames/serve.json`

| t | What happens |
|---|---|
| wait | Dividing line. Points row alternates **SELECT** / **SERVE** in the 4-wide condensed font (hold 0.9 s, roll 0.2 s). Arrowheads at both edges beckon (tip x = 4 pointing left, tip x = 27 pointing right). |
| press (side S) | The other arrowhead disappears and the prompt rolls up and out (8 px over 0.3 s). The pressed side's arrowhead glides toward the centre over 0.5 s (left: tip x 4→13), then over 0.3 s folds down to a single column and regrows as the serve triangle for side S in its first-serve box (left: bottom, right: top). |
| +0.8–1.4 s | The rest of the fresh board emerges from the centre around the triangle: the points `0 - 0` and the games digits slide out to their places from behind the middle (12 px, decelerating, each half clipped to its own side as in §5.7) while the set posts grow down from the dividing line (rows 9→15). |

Reference render: left press at 3.0 s; total 4.4 s.

### 5.4 Point scored / Undo — parametric, 0.75 s — `frames/point-l.json`, `point-r.json`, `undo.json`

**Point to side S** (not making 40–40 and not winning the game):

| t | What happens |
|---|---|
| 0.00–0.35 | `winBall` flies toward the **winner** (point to left → ball goes right→left). |
| 0.30–0.70 | Side S's points value `roll`s up (+1) to its new value. The other side and the dash are static. |
| 0.50–0.75 | `serveFlip` to the new serve state (same server, box toggled). |

Games, sets and line are drawn normally throughout.

**Undo** (revert the last point; shown is 40–15 → 30–15):

| t | What happens |
|---|---|
| 0.00–0.75 | The dividing line "rewinds": pattern of 3 off / 3 on pixels marching **right to left** at 40 px/s (`(x + floor(t×40)) mod 6 < 3` → off). |
| 0.05–0.30 | The whole points row (both numbers and the dash) is drawn shifted **one pixel left**. |
| 0.30–0.70 | The undone side's points `roll` **down** (−1) to the previous value. |
| 0.50–0.75 | `serveFlip` back to the previous serve state. |

No ball. Undoing something larger than a point (a game or set): play the Undo
animation on the points row, then simply redraw the restored board; no special
treatment of games/sets.

### 5.5 Game won / Set won / Match won — parametric

**Game** (2.4 s, `frames/game.json`). Winner W; shown: left wins 40–30 → games 3→4.

| t | What happens |
|---|---|
| 0.00–0.35 | `winBall` toward the winner. |
| 0.35–0.70 | Both points numbers slide off horizontally to their own edges (16 px, eased). The dash disappears the moment they start moving. |
| 0.70–1.50 | **GAME** drops in from above, one letter at a time 0.15 s apart, each with an overshoot (`easeBack`, 0.35 s per letter). Letters at x = 4, 10, 16, 22, row 0. |
| 1.50–1.90 | W's games digit `roll`s up (+1). A 4-pixel sparkle sits at the digit's corners for the first 0.2 s: (x−1,y−1) (x+3,y−1) (x−1,y+5) (x+3,y+5) relative to the digit origin. |
| 2.00–2.40 | GAME lifts out the top (9 px, eased) while the new points (0–0 with dash) wipe in from the centre outward (half-width 0→14). |
| 2.05–2.30 | `serveFlip` to the other side, box top. |

**Set** (3.4 s, `frames/set.json`). Replaces Game when the game wins the set.
Shown: left wins 40–30 at 5–4 → set 1→2 for left (actually: sets 1–0 → 2–0 in the example).

| t | What happens |
|---|---|
| 0.00–0.35 | `winBall` toward the winner over the old board. |
| 0.35–0.85 | The panel floods solid from the **winner's** edge across to the other (columns lit 0→32, eased). |
| 0.85–1.30 | **SET** is punched out of the flood as dark 5x7 letters at x = 7, 13, 19, row 1, one letter per 0.15 s. |
| 1.40 | The set's games score (e.g. `6-4`) is punched out in dark 3x5 at (10, 9). |
| 2.10–2.60 | The flood drains **downward** (rows cleared from the top, 0→16, eased), revealing the new board beneath: points 0–0, games 0–0, the new set cell filled, serve to the other side/top. |
| 2.60–3.40 | The newly filled set block (the 2-px cell **and** the post beside it, 3 columns × 4 rows) blinks off/on every 0.13 s. |

**Match** (6.5 s, then holds, `frames/match.json`). Replaces Set when the set wins the match.

| t | What happens |
|---|---|
| 0.00–0.35 | `winBall` over the old board. |
| 0.35–1.00 | The whole board dissolves pixel by pixel in the fixed pseudo-random order. |
| 1.00–1.90 | **MATCH** drops in letter by letter (0.12 s apart, `easeBack` 0.4 s each) at x = 1, 7, 13, 19, 25, row 0, and stays. |
| 1.30–3.90 | Five fireworks launch from the bottom at x = 6, 25, 15, 10, 22 (starting 1.3, 1.7, 2.2, 2.7, 3.1): a single pixel rises from row 15 to row 10 over 0.45 s, then 8 sparks fly out radially over 0.7 s (spread 5.5 px, slight downward drift `+5b²`), twinkling off/on for the last 45 %. |
| 4.00–4.40 | The final score in 3x5 (`6-4 6-3`, x = 2, row 9) wipes in from the centre. |
| 4.40→ | MATCH and the final score stay on the panel until power is cycled; presses are ignored. A three-set score (e.g. `4-6 6-3 7-5`) is wider than the panel and scrolls through continuously at 25 px/s instead. |

### 5.6 Golden point — parametric, 2.3 s — `frames/golden.json`

Plays instead of Point when the point makes it 40–40.

| t | What happens |
|---|---|
| 0.00–0.75 | Identical to a normal Point: ball, roll to 40–40, serve flip. |
| 0.75–1.00 | The dash (cols 14–17, row 3) narrows to 2 columns and grows vertically into a bar spanning rows 0–6 (half-height 0→3, half-width 2→1, eased). |
| 1.00–1.45 | The bar splits into two single-column bars that sweep outward to the edges (offset d 0→16). Ahead of each bar the 40 is still visible; behind them **GOLDEN** (condensed 4-wide font, x = 2, 29 px wide) is revealed. |
| 1.60–2.20 | One glint: a 2-pixel-wide dark diagonal streak (`x = g − y`, g from −6 to 38) crosses the letters once. |
| 2.20→ | **GOLDEN stays in the points row** (instead of 40–40) until the next point, which ends the game. |

### 5.7 Swap ends — parametric with a waiting prompt, 4.4 s in reference — `frames/swap.json`

Triggered after every Set except the one that wins the match. The press is
the acknowledgement that the teams have physically changed ends.

| t | What happens |
|---|---|
| 0.00–0.50 | Each half of the board (its points number and games digit) slides toward the centre and disappears behind cols 14–17 / the serve area (12 px, **accelerating**, p²). Set cells shrink to nothing (rows 4→0) while the posts stay. The serve triangle disappears at 0.5 s. |
| 0.50→press | **SWAP** / **ENDS** alternates in the points row (hold 0.6 s, roll 0.15 s). In the bottom band two arrowheads start at the edges (tips x = 5 pointing right and x = 26 pointing left), walk inward, pass through each other and continue to the far edge over 1.1 s, pause 0.4 s, repeat. |
| press | The prompt rolls up and out (8 px over 0.3 s). |
| +0.3–0.9 s | The board emerges from the centre with **left and right data exchanged** (points, games, set cells), decelerating. The serve triangle reappears mirrored (other side, same box). |

Data model note: a swap mirrors *everything* side-related, including which
controller scores for which half of the panel. The left controller is "the
team currently on the left of the display".

---

## 6. Files in this package

| Path | Contents |
|---|---|
| `SPEC.md` | this document |
| `p10-sim.html` | the simulator: open in a browser; click a clip name. The `animate` section is the reference code for every animation. |
| `fonts.json` | the three bitmap fonts as `#`/`.` rows, with advance widths |
| `frames/<clip>.json` | 30 fps render of each clip: `{ms, hex}` per frame, consecutive identical frames merged. `hex` = 128 hex chars = 512 bits, row-major from (0,0), MSB first, 1 = on |
| `frames/rally-board-export.json` | the rally section of the splash (frame data from the board, glass raised 3 rows) |
| `gifs/<clip>.gif` | the same renders as 32×16 GIFs, one pixel per LED |

Example scores used in the reference renders (so you can match them exactly):

| Clip | Before | After |
|---|---|---|
| point-l | 30–15, games 3–2, sets 1–0, serve L/top | 40–15, serve L/bottom |
| point-r | 30–15 | 30–30 |
| undo | 40–15, serve L/bottom | 30–15, serve L/top |
| game | 40–30, games 3–2, serve L/bottom | 0–0, games 4–2, serve R/top |
| set | 40–30, games 5–4, sets 1–0 | 0–0, games 0–0, sets 2–0, serve R/top |
| match | 40–30, games 5–3, sets 1–0 | MATCH, final `6-4 6-3` |
| swap | 30–15, games 3–2, sets 1–0, serve L/top | 15–30, games 2–3, sets 0–1, serve R/top |
| golden | 40–30, serve R/top | GOLDEN, serve R/bottom |
