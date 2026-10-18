#!/usr/bin/env python3
"""One markdown table from a matrix.sh folder: each <game>-<set>/ through analyse.py.

    summary.py DATA
"""
import glob, os, re, subprocess, sys

data = sys.argv[1]
here = os.path.dirname(os.path.abspath(__file__))
print("| game | settings | presses | frames | wait | emulate | present | scanout | **total** median | p95 |")
print("|---|---|---|---|---|---|---|---|---|---|")
for d in sorted(glob.glob(f"{data}/*-*/")):
    name = os.path.basename(d.rstrip("/"))
    game, sets = name.split("-", 1)
    out = subprocess.run([sys.executable, f"{here}/analyse.py", d], capture_output=True, text=True).stdout
    m = re.search(r"^(\d+) presses", out, re.M)
    if not m: continue
    st = {k: (a, b) for k, a, b in re.findall(r"^\s+(\w+)\s+([\d.]+)\s+([\d.]+)$", out, re.M)}
    f = lambda k: f"{float(st[k][0]):.1f}"
    print(f"| {game} | {sets} | {m.group(1)} | {st['frames'][0]} | {f('wait')} | {f('emulate')} | {f('present')} | {f('scanout')} "
          f"| **{f('total')}** | {float(st['total'][1]):.1f} |")
