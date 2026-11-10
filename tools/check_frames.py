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
    for frame in frames:
        out.extend([frame["hex"]] * round(frame["ms"] * FPS / 1000))
    return out


def rows(frame_hex):
    return [format(int(frame_hex[y * 8:(y + 1) * 8], 16), "032b") for y in range(16)]


def ascii(frame_hex, on="#", off="."):
    return [row.replace("1", on).replace("0", off) for row in rows(frame_hex)]


def diff(want_hex, got_hex):
    return ["".join("-" if want_bit == "1" and got_bit == "0" else "+" if want_bit == "0" and got_bit == "1" else "."
                    for want_bit, got_bit in zip(want_row, got_row))
            for want_row, got_row in zip(rows(want_hex), rows(got_hex))]


def check(renderer, frames_dir, clip):
    reference = json.load(open(os.path.join(frames_dir, clip + ".json")))
    want = expand(reference["frames"])
    got = subprocess.run([renderer, clip], check=True, capture_output=True, text=True).stdout.split()
    compared = min(len(want), len(got))
    first_bad = next((i for i in range(compared) if want[i] != got[i]), None)
    if first_bad is None and len(want) == len(got):
        print(f"PASS {clip:8s} {len(got)} frames")
        return True
    if first_bad is None:
        print(f"FAIL {clip:8s} frame count: reference {len(want)}, port {len(got)}")
        return False
    print(f"FAIL {clip:8s} first difference at frame {first_bad} (t = {first_bad / FPS:.3f} s), "
          f"{sum(want[i] != got[i] for i in range(compared))} of {compared} frames differ")
    print(f"       {'reference':32s}  {'port':32s}  {'diff (- missing, + extra)'}")
    for want_row, got_row, diff_row in zip(ascii(want[first_bad]), ascii(got[first_bad]), diff(want[first_bad], got[first_bad])):
        print(f"       {want_row}  {got_row}  {diff_row}")
    return False


def main():
    renderer, frames_dir = sys.argv[1], sys.argv[2]
    clips = sys.argv[3:] or CLIPS
    ok = all([check(renderer, frames_dir, clip) for clip in clips])
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
