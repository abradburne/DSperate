#!/usr/bin/env python3
"""Frame pacing from a DS_DEBUG=latency log (latency_log.h): which vblank each presented frame went out
on, per panel, and so how many refreshes showed a frame again (repeats) and how many frames were
never shown (two swaps before the same vblank: the first is replaced).

    pacing.py LOG [--pipe-of-screen 1,0] [--commit-ms 1.0] [--skip-s 2]

A frame goes out on the first vblank of its panel's CRTC at least COMMIT ms after its swap
returned (analyse.py does the same). Repeats count refreshes between the first and last flip.
"""
import sys
from analyse_lib import load, grid, next_vblank, ready

path = sys.argv[1]
pipe_of = [1, 0]
commit_ns = 1.0e6
skip_s = 2.0
for i, a in enumerate(sys.argv):
    if a == "--pipe-of-screen": pipe_of = [int(x) for x in sys.argv[i + 1].split(",")]
    if a == "--commit-ms": commit_ns = float(sys.argv[i + 1]) * 1e6
    if a == "--skip-s": skip_s = float(sys.argv[i + 1])

rows = [r for r in load(path) if r["post"]]   # the frames shown
t0 = rows[0]["poll"] + skip_s * 1e9
rows = [r for r in rows if r["poll"] >= t0]
secs = (rows[-1]["poll"] - rows[0]["poll"]) / 1e9
print(f"{len(rows)} frames presented over {secs:.1f} s ({len(rows) / secs:.3f} fps)")
for scr in (0, 1):
    g = grid(rows, pipe_of[scr])
    if not g: continue
    ref, period = g
    seqs = [round((next_vblank(g, ready(r, scr) + commit_ns) - ref) / period) for r in rows if r["swap"][scr]]
    if not seqs: continue
    shown = len(set(seqs))
    span = seqs[-1] - seqs[0] + 1
    repeats = span - shown
    dropped = len(seqs) - shown
    gaps = [b - a for a, b in zip(seqs, seqs[1:])]
    worst = max(gaps) if gaps else 0
    print(f"screen {scr} (CRTC {pipe_of[scr]}, {1e9 / period:.3f} Hz): {repeats} repeated refreshes, "
          f"{dropped} frames never shown, longest gap {worst} refreshes")
    # When the repeats came, as seconds into the run.
    when = [(rows[i + 1]["poll"] - rows[0]["poll"]) / 1e9 for i, d in enumerate(gaps) if d > 1]
    if when: print("  repeats at " + " ".join(f"{t:.1f}" for t in when[:40]) + (" ..." if len(when) > 40 else "") + " s")
