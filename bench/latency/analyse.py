#!/usr/bin/env python3
"""Button-to-screen latency from a latency.sh folder (runs 0 and 1: baseline; 2 on: one press each).

    analyse.py OUT [--pipe-of-screen 1,0] [--commit-ms 1.0] [--key-up]

For each press: the frame the core first got the button (N), the first frame from N on whose
picture differs from the baseline's at the same frame (M, the game's response), and the times
in between, from lat-press's stamps and DSperate's DS_DEBUG=latency log (latency_log.h):

  pad       written into gpio-keys-control -> pocket-input's uinput event
  wait      -> the emulation thread takes the input at the start of frame N
  frames    M - N: emulated frames before the picture changes (the game's own, plus where the
            emulator ends a frame)
  emulate   poll(N) -> frame M emulated
  present   -> the swap of the changed screen's window returned
  scanout   -> the page flip: the first vblank of that panel's CRTC at least COMMIT ms after both
            the swap and the GPU's draw (GLES present: a timestamp query) are done. The CRTCs' vblank grids come from
            the vblank stamps in the log.
  total     written -> flip

Screens: 0 top, 1 bottom. --pipe-of-screen says which DRM pipe shows each (default 1,0: the
bottom panel, DSI-1, is KMSDRM display 0).
"""
import glob, os, statistics, sys
from analyse_lib import load, grid, next_vblank, ready

out = sys.argv[1]
pipe_of = [1, 0]
commit_ns = 1.0e6
key_up = "--key-up" in sys.argv
for i, a in enumerate(sys.argv):
    if a == "--pipe-of-screen": pipe_of = [int(x) for x in sys.argv[i + 1].split(",")]
    if a == "--commit-ms": commit_ns = float(sys.argv[i + 1]) * 1e6


def pct(xs, p):
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(round(p / 100 * (len(xs) - 1))))]


base = [load(f"{out}/run{r}.log") for r in (0, 1)]
b0 = {r["frame"]: r["hash"] for r in base[0] if r["post"]}
b1 = {r["frame"]: r["hash"] for r in base[1] if r["post"]}
common = sorted(set(b0) & set(b1))
diff = [f for f in common if b0[f] != b1[f]]
if diff:
    print(f"baseline runs differ from frame {diff[0]} ({len(diff)} of {len(common)} frames): not deterministic", file=sys.stderr)
# Either baseline's picture of a frame (a frame one did not show, the other may have).
reference = {**b1, **b0} if not diff else {f: b0[f] for f in common if f < diff[0]}

stages = {k: [] for k in ("pad", "wait", "frames", "emulate", "present", "scanout", "total")}
for path in sorted(glob.glob(f"{out}/run*.press"), key=lambda p: int(p.split("run")[-1].split(".")[0])):
    r = int(path.split("run")[-1].split(".")[0])
    press = {l.split()[0]: (int(l.split()[1]), int(l.split()[2])) for l in open(path) if l.strip()}
    rows = load(f"{out}/run{r}.log")
    if not rows or "down" not in press: continue
    written, pad = press["up" if key_up else "down"]
    if key_up:
        held = [x for x in rows if x["buttons"]]
        if not held: continue
        n = next((x for x in rows if x["frame"] > held[0]["frame"] and not x["buttons"]), None)
    else:
        n = next((x for x in rows if x["buttons"] and x["poll"] >= written), None)
    if not n: print(f"run {r}: the press never reached the core", file=sys.stderr); continue
    m = next((x for x in rows if x["frame"] >= n["frame"] and x["post"] and x["frame"] in reference and x["hash"] != reference[x["frame"]]), None)
    if not m: print(f"run {r}: no response within the baseline", file=sys.stderr); continue
    scr = 0 if m["hash"][0] != reference[m["frame"]][0] else 1
    if m["hash"][1 - scr] != reference[m["frame"]][1 - scr] and m["swap"][1 - scr] and m["swap"][1 - scr] < m["swap"][scr]:
        scr = 1 - scr   # both changed: the one swapped first
    g = grid(rows, pipe_of[scr])
    swap = m["swap"][scr] or m["post"]
    flip = next_vblank(g, ready(m, scr) + commit_ns) if g else swap
    s = dict(pad=pad - written, wait=n["poll"] - pad, frames=m["frame"] - n["frame"], emulate=m["emu"] - n["poll"],
             present=swap - m["emu"], scanout=flip - swap, total=flip - written)
    for k, v in s.items(): stages[k].append(v)
    print(f"run {r:3}: frame {n['frame']} -> {m['frame']} (+{s['frames']}, screen {scr})  " +
          "  ".join(f"{k} {v / 1e6:.1f}" for k, v in s.items() if k != "frames") + " ms")

if stages["total"]:
    print(f"\n{len(stages['total'])} presses       median    p95   (ms; frames for 'frames')")
    for k, xs in stages.items():
        div = 1 if k == "frames" else 1e6
        print(f"  {k:10} {statistics.median(xs) / div:8.1f} {pct(xs, 95) / div:7.1f}")
