#!/usr/bin/env python3
"""Compares the host-rendered clips with the reference renders, pixel for pixel.

    tools/check_frames.py test/render_clips design/frames [clip ...]

For each clip, runs the renderer and expands the reference's merged frames
(each {ms, hex} covers round(ms * 30 / 1000) source frames). Prints PASS or
the first differing frame as ASCII: reference, port, and the differences.
"""

import json
import os
import subprocess
import sys

FPS = 30
CLIPS = ["splash", "bind", "serve", "point-l", "point-r", "undo", "game", "set", "match", "swap", "golden"]


def expand(frames):
    out = []
    for fr in frames:
        out.extend([fr["hex"]] * round(fr["ms"] * FPS / 1000))
    return out


def rows(hx):
    return [format(int(hx[y * 8:(y + 1) * 8], 16), "032b") for y in range(16)]


def ascii(hx, on="#", off="."):
    return [r.replace("1", on).replace("0", off) for r in rows(hx)]


def diff(a, b):
    ra, rb = rows(a), rows(b)
    return ["".join("-" if x == "1" and y == "0" else "+" if x == "0" and y == "1" else "." for x, y in zip(la, lb))
            for la, lb in zip(ra, rb)]


def check(renderer, frames_dir, clip):
    ref = json.load(open(os.path.join(frames_dir, clip + ".json")))
    want = expand(ref["frames"])
    got = subprocess.run([renderer, clip], check=True, capture_output=True, text=True).stdout.split()
    n = min(len(want), len(got))
    bad = next((i for i in range(n) if want[i] != got[i]), None)
    if bad is None and len(want) == len(got):
        print(f"PASS {clip:8s} {len(got)} frames")
        return True
    if bad is None:
        print(f"FAIL {clip:8s} frame count: reference {len(want)}, port {len(got)}")
        return False
    print(f"FAIL {clip:8s} first difference at frame {bad} (t = {bad / FPS:.3f} s), "
          f"{sum(want[i] != got[i] for i in range(n))} of {n} frames differ")
    print(f"       {'reference':32s}  {'port':32s}  {'diff (- missing, + extra)'}")
    for r, p, d in zip(ascii(want[bad]), ascii(got[bad]), diff(want[bad], got[bad])):
        print(f"       {r}  {p}  {d}")
    return False


def main():
    renderer, frames_dir = sys.argv[1], sys.argv[2]
    clips = sys.argv[3:] or CLIPS
    ok = all([check(renderer, frames_dir, c) for c in clips])
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
