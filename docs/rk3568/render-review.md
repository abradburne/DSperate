# DSperate render and present path on the DS+: review (2026-10-18)

What it costs to draw and show a frame on the RG DS+ (RK3568, 4x A55 at 1.99 GHz, Mali-G52 on
Panfrost), where the time goes, and what was changed. Scope: the 1x CPU 3D raster and 2D
engines (`src/core/gpu`), the hi-res GLES path (`gl_hires.cpp`, `gl_present.*`), the present
thread and the frame hand-off, and how 1x and 2x duplicate work. The emulation core (CPU/JIT,
scheduler, DMA, sound) is reviewed in a parallel session. DSperate fork, branch `render`
(from `pocketos` 9cca5e0).

## How it was measured

- `render-bench.sh ROM SECONDS [ini lines]` (here): one game from boot, no input, both panels,
  real-time priority as under dslot.service, the installed settings plus `hires3d` and
  frame-skip off. Prints the emulation's frame times (`DS_FRAME_STATS`, first 300 frames left
  out), the present thread's job time, frames not shown, fps, and at hi-res the GPU time per
  stage (`DS_HIRES_STAGES`, timer queries) and the present thread's CPU time per render
  (`DS_HIRES_DEBUG`). `PERF=<s>` adds a perf profile of the whole process (by thread).
- `render-same.sh BIN_A BIN_B ROM FRAMES`: whether two builds put the same pictures on the
  panels, frame by frame from boot. Every frame is shown (`DS_PRESENT_WAIT=1`) and each
  window's picture is hashed before its swap (`DS_PRESENT_HASH`, new).
- Games, 65 s from boot: Golden Sun: Dark Dawn (intro) at 2x, New Super Mario Bros. at 2x,
  Mario Kart DS (title and race demo) at 2x, Zelda: Spirit Tracks at 1x.

## Baseline (fork 9cca5e0, as packaged)

| 65 s from boot | fps | emulation ms median / p99 | frames not shown | present thread ms a job | GPU ms a render | present CPU ms a render |
|---|---|---|---|---|---|---|
| Golden Sun 2x | 59.2 | 8.4 / 18.7 | 549 of 3856 | 12.3 | ~11.2 | ~6.1 |
| NSMB 2x | 59.2 | 10.8 / 17.9 | 8 of 3870 | 8.6 | ~6.5 | ~6.1 |
| Mario Kart 2x | 59.8 | 8.3 / 12.6 | 97 of 3910 | 9.0 | ~5.5 | ~5.5 |
| Spirit Tracks 1x | 59.8 | 9.1 / 13.7 | - | (SDL_Renderer, on the emulation thread: 5.0 ms a frame) | - | - |

Where the CPU goes in Golden Sun at 2x (perf, 25 s, 999 Hz; ~2 of the 4 cores busy):

| thread | share of samples | per frame |
|---|---|---|
| emulation (JIT, DMA, geometry, the 2D/3D hand-off) | 39% | ~13 ms |
| present (GLES: hi-res render, composite, swaps) | 22% | ~7.5 ms, a third of it kernel (Panfrost BO map/unmap), a quarter in the driver |
| 3D raster bands (2 threads, the 1x CPU raster) | 28% | ~4.7 ms each |
| 2D engine A worker | 11% | ~3.7 ms |

GPU per hi-res render, Golden Sun's intro at frame 600, `DS_HIRES_BENCH` (the frame drawn 60
times, glFinish'd; each stage left out in turn): **4.9 ms** a render, of which the composite
**2.4**, the resolve 1.6, the pre-pass 1.4, the translucent tail 0.1, edge and fog 0.2, the rest
(clears, uploads) ~1.9. The timer queries (`DS_HIRES_STAGES`) put ~4.5 ms on the composite and
~5 on the tail: on a tiler a section's time includes work flushed into it, so they only rank.

The present thread's syscalls, Golden Sun at 2x (`strace -c` / `-e ioctl`, per present): ~9
job submits (about five render passes: pre-pass, 3D, edge, composite, the window), ~16
WAIT_BO, and ~7 buffer-object round trips through Mesa's BO cache for the passes' transient
memory, each MMAP_BO + mmap + munmap + MADVISE x2 + SET_LABEL x2 (64 KB each). Every render
pass costs a fixed toll in the kernel and the driver.

Run-to-run: Golden Sun's intro is deterministic from boot; New Super Mario Bros. (from frame
~889) and Mario Kart (~1447) are not, because the RTC runs on the host clock. `bench/render/render-same.sh`
therefore replays a recorded no-input log (`--replay` freezes the RTC).

## Ranked findings

Speed-ups first (cost measured; gain expected or, where done, measured), then simplifications.

| # | item | measured cost | expected gain | risk | size | status |
|---|---|---|---|---|---|---|
| S1 | Composite shaders divide every pixel's coordinate by a uniform scale (`p / u_scale`): integer division per fragment on the G52 | composite 2.4 of 4.9 GPU ms a render (GS) | ~1-2 GPU ms a render at 2x | low | small | **done**: NSMB 4.92 -> 4.19 GPU ms a render |
| S2 | Engine A's planes rebuilt on the CPU every render (classes, brightness, repacking, 9 textures) | 2.4 CPU ms a render on the present thread (GS), 2.6 (NSMB) | -1.2 ms: uploaded straight from the kept lines | low | medium | **done** (with S4: 0.8 ms) |
| S3 | 1x through SDL_Renderer on the emulation thread, both panels' flips in turn | 5.0 ms a frame on the emulation thread (Spirit Tracks) | the present off the emulation thread | low (pacing: see S3b) | small | **done**: 0.5 ms |
| S3b | At 1x on the present thread, the "GPU still drawing" skip fires on a swap wait: 45 of 3913 frames not shown in Spirit Tracks | 45 repeats a minute | wait for the frame before at 1x instead | low | tiny | **done**: none skipped; the emulation thread waits 1.2 ms a frame on average |
| S4 | The planes still go up as textures (Panfrost tiles them on the CPU) | ~1.1 CPU ms a render left in "planes" | ~0.8 ms: one mapped buffer the shaders read | medium | medium | **done**: planes 0.8-0.9 ms (from 2.4-2.7) |
| S5 | Five render passes a render, each with Mesa's transient-BO toll (above); edge marking and fog are a pass of their own | ~7 BO round trips + 9 submits a present | one pass less where edge marking or fog is on: fold them into the composite | medium | medium | **done**, as a composite build of its own: compiled into the one composite it cost NSMB (neither on) 0.6 GPU ms |
| S6 | Translucent tail: ~1 draw (plus state) per run; "raster calls" CPU | 2.1-2.5 CPU ms a render | fewer runs or cheaper state; needs the run counts (now in `DS_HIRES_DEBUG`) | medium | medium | to measure |
| S7 | A frame both shown and captured drew its 3D twice | never happens here: GS intro and field, NSMB, Mario Kart all count 0 such frames | none | - | small | done, then **reverted** (the draw3d/composite split stays) |
| S8 | Mesa: per-pass transient BOs cycle through the BO cache with mmap/munmap and SET_LABEL every time | ~7 x 8 syscalls a present | in Mesa (nibOS (formerly pocketOS) builds its own 26.2.4): keep cached BOs mapped, label once | medium | small (Mesa patch) | recommendation |
| S9 | The 1x CPU 3D raster: two band threads | ~4.7 ms a frame each in GS 2x, ~7.5 each in Spirit Tracks 1x | it is needed (capture, the 1x picture, class-2 pixels); a big win would need a lazy raster | high | large | not attempted |

Simplifications, dead code, duplication:

| # | item | status |
|---|---|---|
| C1 | One present path whatever the 3D resolution on the DS+ (GLES on the present thread at 1x too) | **done** (S3) |
| C2 | Composite inputs: 9 textures and ~70 lines of CPU repacking to 4 textures uploaded as they lie; pass 2's unreachable class 0/1 branches and the unused 3D-capture mode (2) gone | **done** (S2) |
| C3 | Dead uniforms/variables, a stale comment, two shadow debug switches | **done** |
| C4 | `GlHires::render` split into `draw3d` + `composite`; its timers and stage queries moved from function statics into the object | done (with S7) |
| C5 | The Vulkan GPU raster (`src/core/gpu/vk`, `video.gpu3d`) and its present are unused on the DS+ (5x slower than GL here) | upstream's; left |

## Stage 2: what changed, measured (fork branch `render`, bf54ef7)

Commits on top of `pocketos` 9cca5e0, in order: `DS_PRESENT_HASH` (the identity check);
3D drawn once per shown-and-captured frame (S7, later reverted, keeping the
`draw3d()`/`composite()` split); planes straight from the kept lines (S2); 1x through the GLES
present (S3); dead code (C3); the hash's expected-1x column and word-wise hashing; the
composite compiled for its scale (S1); 1x waits instead of skipping (S3b); planes through one
mapped buffer (S4); edge marking and fog in the composite, as a build of its own (S5); S7
reverted. Merges cleanly with `pacing` (ed38625) and with the core session's `core-speed`.

**Pixel output:** identical to 9cca5e0 frame by frame (`bench/render/render-same.sh`, every frame shown,
under `--replay`): Golden Sun intro 2917 presents, NSMB 1259, Mario Kart 2323 (title, menus and
the race demo with kart shadows), at 2x. The 1x GLES path: all 1003 presents of Spirit Tracks
equal the nearest 4x scaling of the 1x screen, i.e. what SDL_Renderer showed.

65 s from boot (Golden Sun field: 65 s from the user's slot 1), both panels, real-time priority,
frame-skip off, no timer queries (they change Panfrost's batching):

| | fps | emulation ms median / p99 | frames not shown | present thread ms a job | present CPU ms a render |
|---|---|---|---|---|---|
| Golden Sun intro 2x, before | 59.7 | 8.4 / 18.3 | 519 of 3903 | 11.8 | 5.1 |
| after | 59.7 | 8.7 / 18.5 | 486 of 3891 | 11.8 | 3.3 |
| Golden Sun field 2x, before | 59.7 | 12.9 / 19.1 | 1037 of 3901 | 14.3 | 5.4 |
| after | 59.5 | 13.3 / 20.2 | 1042 of 3889 | 14.1 | 3.8 |
| NSMB 2x, before | 59.2 | 10.8 / 17.7 | 9 of 3873 | 7.8 | 5.1 |
| after | 59.5 | 10.6 / 17.0 | 11 of 3890 | 5.8 | 3.6 |
| Mario Kart 2x, before | 59.5 | 9.1 / 19.5 | 221 of 3889 | 9.5 | 4.4 |
| after | 59.8 | 8.9 / 13.3 | 40 of 3909 | 8.0 | 3.9 |
| Spirit Tracks 1x, before | 59.8 | 9.2 / 13.8 | - | (SDL_Renderer: 5.3 ms a frame on the emulation thread) | - |
| after | 59.8 | 9.3 / 14.4 | 0 | 9.8 (present thread; the emulation thread waits 1.2 ms a frame) | - |

Single runs each. The emulation's p99 moves by several ms between runs of the same build (Mario
Kart's baseline: 12.6 in one, 19.5 in another), so only the present columns and the frames not
shown are read from this table; the emulation thread's own cost is the core session's.

GPU per render (`DS_HIRES_BENCH`, the frame drawn 60 times back to back):

| | before | after |
|---|---|---|
| NSMB, frame 600 | 4.92 ms | 4.19 (S1) / 4.23 (S4) / 4.8 with S5 compiled into the one composite, so S5 got a build of its own |
| Golden Sun field, render 300 | 13.63 ms | 12.28 |

Golden Sun's field (render 300), what each stage costs, leaving it out: the opaque pre-pass
~5-6.6 ms, the resolve ~3.9, the translucent tail ~3.8 (6 runs, 6.5k vertices), the composite
with fog ~3.0, fixed ~2.9. Not changed, and why:

- **The pre-pass** writes the DS's own integer depth from the shader (`gl_FragDepth`): that is
  what makes ties resolve as on the DS. Panfrost then tests and updates depth late for every
  fragment (`src/panfrost/lib/pan_earlyzs.c`; it does not use `EXT_conservative_depth`), so at
  ~4x overdraw nothing is rejected early. Hardware-interpolated depth would change tie results.
- **The tail's terrain** (alpha-31 translucent polygons) is drawn twice, opaque pixels then the
  rest: they differ in depth write, stencil and the id plane, which a draw cannot vary per pixel.
- **Mesa's per-pass toll** (S8): the present thread at 1x spends ~45% of its time in the kernel
  and ~28% in Panfrost for two plain window draws (~3.3 ms a frame, perf with Mesa's symbols):
  submits, and every pass's transient memory through the BO cache with MMAP_BO + mmap + munmap
  + 2 MADVISE + 2 SET_LABEL. Keeping cached BOs mapped and labelling once is a Mesa patch
  (nibOS builds Mesa 26.2.4 itself), not DSperate.
- **The 1x CPU raster** (two band threads, ~5-7.5 ms a frame each) is still needed at 2x
  (capture, the 1x picture, class-2 pixels).

## Installed: `pocketos` 71625ce (core-speed + render, PGO), against 9cca5e0

Package `dsperate 0.r7c1fb67.78.71625ce-1` (built and installed by the core session). Golden
Sun's intro: the same pictures as 9cca5e0, all 2917 presents (`DS_PRESENT_HASH`, replayed).
65 s from boot, single runs (the core session's speed-ups and PGO are in the emulation column):

| | fps | emulation ms median / p99 | s under 59 fps | frames not shown | present CPU ms a render |
|---|---|---|---|---|---|
| Golden Sun intro 2x, 9cca5e0 | 59.7 | 7.9 / 18.3 | 4 | 466 of 3903 | 4.6 |
| installed | 59.8 | 8.0 / 15.7 | 0 | 434 of 3909 | 3.3 |
| NSMB 2x, 9cca5e0 | 59.2 | 10.6 / 17.6 | 9 | 15 of 3875 | 5.0 |
| installed | 59.8 | 10.2 / 16.2 | 1 | 9 of 3909 | 3.6 |
| Mario Kart 2x, 9cca5e0 | 59.4 | 8.9 / 20.3 | 4 | 236 of 3883 | 4.4 |
| installed | 59.8 | 8.2 / 12.2 | 1 | 47 of 3908 | 3.8 |
| Spirit Tracks 1x, 9cca5e0 | 59.8 | 9.2 / 13.8 | 1 | - (present on the emulation thread: 5.0 ms a frame) | - |
| installed | 59.8 | 8.8 / 13.7 | 0 | 0 (the emulation thread waits 0.7 ms a frame) | - |

## GBA rendering (added to this review's scope, 2026-10-18)

The GBA picture: `gba::Ppu` draws each line at HBlank on the emulation thread (one thread: the
CPU interpreter, the PPU, the sound); `Gpu::show_gba` centres the 240x160 frame in the DS's top
screen and the present shows it like a DS frame, 4x on the top panel (960x640 inside 1024x768),
the black bottom screen skipped as unchanged. So the GBA's rendering cost is the PPU.

Profile on the DS+ (headless, perf, from boot with Start/A presses; after the core session's
76da654 PPU change): `compose` 13-26% of a frame, the line drawing (`render_line` with text BGs,
sprites, windows inlined) 10-19%, the interpreter and frame loop the rest.

Fork branch `gba-render` (from `core-speed`), 387b5c9: `compose` vectorised. Measured on the DS+,
headless, 3600 frames, two runs each, medians:

| ms a frame | 03cd984 | branch-free, sprite loop scalar | + branch-free effects | **387b5c9** (all vectorised) |
|---|---|---|---|---|
| Castlevania: Aria of Sorrow | 6.92 | 7.45 | 7.30 | **5.95** |
| Donkey Kong Country 2 | 10.12 | 10.58 | 11.08 | **9.18** |
| Advance Wars 2 | 10.09 | 10.72 | 10.61 | **9.32** |

Frame hashes the same on all three; frames and audio the same on six more games (`gba-exact`);
ctest 27/27. The middle columns are why it is all or nothing on the A55: masks for every pixel
cost more than the original's predictable branches unless every loop vectorises (the build VM's
big cores showed the opposite, so these were only decided on the DS+).

Not done: `draw_text` (~5-10%) could take a 4bpp tile row through NEON table lookups (a
16-colour bank is 32 bytes); `draw_sprites` scans all 128 OAM entries every line (a list kept
until OAM is written would need a hook in the GBA bus).

## Golden Sun at 2x (2026-10-18, evening)

Golden Sun: Dark Dawn left ~12% of frames unshown in its intro and ~23-27% in its field at 2x.

**Phase or GPU?** A scratch branch `combo` (`pocketos` 71625ce + `pacing` ed38625, the vsync lock
on the top panel), against `render` alone and render + core-speed, 65 s each, frames not shown:

| | render | + core-speed | + pacing |
|---|---|---|---|
| GS intro | 473 of 3906 | 474 | 462 (lock: 10 late flips) |
| GS field (slot 1) | 1009 of 3904 | 1009 | 907 (3 late flips) |

Timing accounts for ~10% of the field's unshown frames; the rest is the GPU over budget. It
already runs at 800 MHz 96% of the time there (devfreq `trans_stat`; `performance` is not an
available governor on this kernel).

**Where the GPU goes**, ms a render (`DS_HIRES_BENCH`, stages added one at a time: the
previous "leave one out" reading wrongly blamed the pre-pass):

| | fixed | pre-pass | resolve | translucent tail | composite | fog | total |
|---|---|---|---|---|---|---|---|
| intro, render 900 | 2.5 | +0.7 | +2.1 | **+6.8** | +1.1 | +0.3 | 13.5 |
| field, render 300 | 2.6 | +0.6 | +2.3 | **+6.2** | +1.5 | +0.1 | 13.3 |
| field, render 600 | 2.4 | +0.7 | +2.2 | **+6.4** | +1.0 | +0.5 | 13.3 |

The tail is half of it: Golden Sun's terrain is "translucent" only because its textures carry
alpha, so it is drawn in the tail, without the visibility buffer, twice (its alpha-31 pixels as
opaque ones, then the rest blended), every fragment shaded in full. These numbers vary by ~1 ms
run to run, too much to see the changes below; frames not shown are the measure there.

| change (branch `render`) | GS field, frames not shown in 45 s, three alternating runs | kept |
|---|---|---|
| before | 674, 606, 678 (mean 653) | |
| 7b64f51: each alpha-31 polygon drawn only in the part(s) its texture's alpha classes can reach (the texture cache notes alpha 0 / 1-30 / 31 per decoded texture) | 561, 556, 579 (565, **-13%**) | yes |
| bfd458a: tail fragments behind the opaque prefix discarded before shading (the pre-pass writes the prefix depth as a third target) | 592, 605, 566 (588) | reverted (300f7d6): the extra target costs what it saves |

Both were pixel-identical to the build before (Golden Sun's intro, 2917 presents under replay;
its field from slot 1, 1199 presents, after checking the field reproduces itself).

**Automatic fallback** (667273b, 40c2ae9; `video.hires3d_fallback`, on by default): when 15%
of the last two seconds' frames were left unshown with the GPU busy, the 3D goes to 1x; 2x is
tried again after 10 s, the wait doubling to 80 s each time it drops again within 3 s of coming
back, and back to 10 s after 30 s held at 2x. The resolution changes only between display
periods, so Golden Sun's captured screen pairs stay whole. 120 s each, with it on:

| | frames not shown | fps | seconds under 59 fps |
|---|---|---|---|
| GS field, fallback off (65 s) | 1009 of 3904 (26%) | 59.8 | 0 |
| GS field, fallback on | 88 of 7194 (1.2%) | 59.8 | 0 |
| GS intro, fallback off (65 s) | 473 of 3906 (12%) | 59.8 | 0 |
| GS intro, fallback on | 166 of 7199 (2.3%) | 59.8 | 0 |

In the field it settles at 1x and tries 2x again after 10, 20, 40, 80 s (a ~2 s burst of 2x
each time). The benches here turn it off (`bench/render/render-bench.sh` sets `hires3d_fallback = false`
unless `FALLBACK=1`).

**S9 (skip the 1x CPU raster when nothing can see it).** Frames at 2x where it could be skipped
with the output exact (no capture, engine A in display mode 1, no engine-A line taking the full
select, whose classes depend on where the 1x 3D is on top), 65 s each: NSMB 8 of 3600 (its blend
registers put every 3D line through the full select), Mario Kart 2423 of 3900 (62%), Golden Sun
461 of 3600 (13%). Not built: waiting on the user's decision, given it pays in Mario Kart-like
games only and is the largest change on the list.

## Battery (2026-10-18, on battery, the current `render` build)

`bench/render/power-run.sh` around `bench/render/render-bench.sh`: 150 s from boot each, both panels, real-time priority,
in this order (so battery level and temperature drift spread), on battery from 100% (93% at the
end). Power is the mean of the rk817 gauge's 1 s averages (current x voltage); "mA" is the
coulomb counter's charge over the run, which also reflects the falling voltage.

| | W | mA | frames not shown | CPU at 1992 MHz | GPU |
|---|---|---|---|---|---|
| idle, screens on, nothing running (60 s) | 2.39 | 609 | - | 3% (90% at 408) | idle |
| NSMB 2x, fallback off | 4.22 | 998 | 9 | 76% | 600 MHz 67%, 800 14% |
| Mario Kart 2x, fallback off | 4.48 | 1080 | 141 | 76% | 800 MHz 72% |
| NSMB 1x | 3.81 | 912 | - | 70% | 200 MHz 100% |
| Mario Kart 1x | 3.87 | 970 | - | 73% | 200-300 MHz |
| NSMB 2x, fallback on | 4.28 | 1068 | 8 | 92% | as off |
| Mario Kart 2x, fallback on | 4.49 | 1129 | 164 | 62% | as off |

- 2x costs **+0.4 W in NSMB (+11%)** and **+0.6 W in Mario Kart (+16%)** over 1x: the GPU at
  600-800 MHz instead of 200-300, the present thread's extra work.
- The fallback changes nothing here: neither game leaves 15% of frames unshown, so it never
  switches (it acts in Golden Sun's field and intro). Its runs came last, at a lower battery
  voltage, hence their higher mA at the same power.
- The CPU is at its top clock 62-92% of the time in every game, at 1x too: the emulation
  thread under real-time priority keeps schedutil there. A game costs ~1.4-2.1 W over the idle
  2.4 W, most of which is the two panels and their backlights. As a rough guide, the 4247 mAh
  cell (~16 Wh) lasts ~3.8 h at 2x and ~4.2 h at 1x in these games.
