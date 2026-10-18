"""DS_DEBUG=latency log parsing and the CRTC vblank grid, shared by analyse.py and pacing.py."""


def load(path):
    rows = []
    for line in open(path):
        f = line.split()
        if len(f) < 11: continue
        rows.append(dict(frame=int(f[0]), buttons=int(f[1]), poll=int(f[2]), emu=int(f[3]), post=int(f[4]),
                         swap=[int(f[5]), int(f[6])], hash=(f[7], f[8]), vbl=[int(f[9]), int(f[10])],
                         gpu=[int(f[11]), int(f[12])] if len(f) >= 13 else [0, 0]))
    return rows


def grid(rows, pipe):
    """(reference vblank, period) for a pipe from its vblank stamps."""
    v = sorted({r["vbl"][pipe] for r in rows if r["vbl"][pipe]})
    if len(v) < 2: return None
    d = [b - a for a, b in zip(v, v[1:])]
    # Each gap is a whole number of periods (~16.68 ms on the DS+): refine over the whole run.
    n = sum(round(x / 16.68e6) for x in d)
    return v[0], (v[-1] - v[0]) / n


def next_vblank(g, t):
    ref, period = g
    k = -(-(t - ref) // period)
    return ref + k * period


def ready(r, scr):
    """When the frame on `scr`'s window could go out: its swap returned and the GPU finished it."""
    return max(r["swap"][scr] or r["post"], r["gpu"][scr])
