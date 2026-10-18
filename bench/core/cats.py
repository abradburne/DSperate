#!/usr/bin/env python3
# bench/core/cats.py FILE...: main-thread perf symbols (perf report --comm base --sort sym) by area,
# as a share of the main thread's samples.
import re, sys
CATS = [
  ("jit code", r"^jit[79]_"),
  ("jit stubs/helpers", r"jit_stub|jit_h_|ds::jit::|ds_slice_next"),
  ("interp (DS)", r"ds::interp::"),
  ("GBA CPU", r"gba_interp|run_gba_frame|Gba::pf_|Gba::rebuild_timing|CpuContext::jump|Gba::(read|write|fetch)|bus_(read|write|fetch)"),
  ("GBA PPU", r"gba::Ppu"),
  ("GBA audio/psg", r"psg::|gba::Audio"),
  ("GBA other", r"gba::Gba"),
  ("DMA", r"ds::dma::"),
  ("scheduler/events", r"Scheduler::|ev_scanline|ev_hblank|idle|wait_loop"),
  ("I/O + bus", r"ds::io::|ds::mem::Bus|Bus::|Io::"),
  ("SPU", r"ds::spu::"),
  ("3D geometry (Gpu3D)", r"Gpu3D|finalise_list|__merge|__move_merge|__insertion_sort"),
  ("3D raster (main)", r"Renderer3D|gatherN|resolve"),
  ("2D (main)", r"Engine2D|kern::|Gpu::|gpu::"),
  ("memcpy/memset", r"^mem(cpy|set|move)"),
  ("main/frontend", r"^main$|frame_report"),
]
for f in sys.argv[1:]:
    tot = {}; all_ = 0.0; other = {}
    for line in open(f):
        m = re.match(r"\s*([\d.]+)%\s+\[.\]\s+(.*)", line)
        if not m: continue
        p = float(m.group(1)); s = m.group(2).strip(); all_ += p
        for name, rx in CATS:
            if re.search(rx, s): tot[name] = tot.get(name, 0) + p; break
        else: other[s] = other.get(s, 0) + p
    print(f"== {f.split('/')[-1]} (main thread {all_:.1f}% of all samples)")
    for name, _ in CATS:
        if name in tot: print(f"  {name:22s} {100*tot[name]/all_:5.1f}%")
    o = sum(other.values()); print(f"  {'other':22s} {100*o/all_:5.1f}%  top: " + "; ".join(f"{k[:40]} {100*v/all_:.1f}" for k, v in sorted(other.items(), key=lambda x: -x[1])[:4]))
