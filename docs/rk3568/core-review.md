# DSperate core review: CPU, scheduler, memory, DMA, sound, GBA (2026-10-18)

Scope: the emulation core on the RG DS+ for DS and GBA — CPUs (interpreter, JIT), scheduler and
events, memory and bus timing, DMA, timers/IRQs, SPU and GBA sound, states, threads. Not the DS
2D/3D rasterisers or the GL present (the render-review session has those; the GBA PPU was handed
to this review). Work is on DSperate branch `core-speed` (from `pocketos` 9cca5e0).

## How it was measured

`bench/core/bench.sh` on the DS+ (schedutil, DSlot stopped, headless, unpaced): fixed scenes from
copies of the user's save states (`~/core-bench/states`) or from boot, the frame-time series
(`frame ms` median/mean/p95/p99, frames over 16.7 ms) and, per run, an md5 of every frame's hash
(`--hash-frames`) and of the whole audio output (`--dump-audio`). The same binary gives the same
hashes run after run, and timings repeat within ~1-2%. The frame times include the frame hashing
(~0.2 ms a frame, more in light scenes; measured inside the headless frame timer), the same for every
build, so they read slightly above the README's figures. Any change below that keeps both hashes is
exact on that scene. `bench/core/gba-exact.sh` does the same on rgbuild for six GBA games (two
builds side by side). `bench/core/prof.sh` records `perf` per scene (JIT blocks named through
`DS_PERF_MAP`); `bench/core/cats.py` sums the emulation thread's symbols by area.

| scene | what |
|---|---|
| gs1 | Golden Sun: Dark Dawn, user's state 1 (field), 1800 frames |
| gsboot | Golden Sun: Dark Dawn from boot (intro), 3900 frames |
| nsmb0 | New Super Mario Bros., user's state 0, 1800 frames |
| mkds | Mario Kart DS from boot (attract race), 3900 frames |
| ac | Assassin's Creed: Altair's Chronicles, user's auto state, 1800 frames |
| aos, dkc2 | GBA Castlevania: Aria of Sorrow, Donkey Kong Country 2, 3600 frames, Start/A pressed through menus |
| st, plat, dos, gsgba, mmz | held out of PGO training: Spirit Tracks, Pokémon Platinum, Dawn of Sorrow (boot), GBA Golden Sun, Mega Man Zero |

## Where the time goes (baseline 9cca5e0)

DS frame time is set by the emulation thread; the three 3D band threads and the 2D line worker run
beside it. Share of the emulation thread's samples:

| area | gs1 | gsboot | nsmb0 | mkds | ac |
|---|---|---|---|---|---|
| JIT-translated code | 12% | 16% | 19% | 13% | 20% |
| JIT stubs, lookup, slice loop | 17% | 18% | 17% | 16% | 17% |
| DMA | 17% | 13% | 8% | 7% | 3% |
| 3D geometry (Gpu3D) | 21% | 12% | 17% | 14% | 10% |
| I/O and bus dispatch | 6% | 8% | 7% | 7% | 14% |
| SPU | 7% | 8% | 5% | 7% | 2% |
| scheduler/events | 3% | 4% | 4% | 4% | 3% |
| 2D on the emulation thread | 8% | 12% | 15% | 23% | 10% |
| 3D raster on the emulation thread | 0% | 0% | 0% | 2% | 9% |

GBA runs on one thread: the CPU interpreter was 34% (AoS) / 59% (DKC 2), the PPU 57% / 36%.

## Ranked items

Measured = on the DS+, medians in ms (frames over 16.7 ms); "exact" = frame and audio hashes unchanged.

### Speed-ups

| # | item | measured cost | gain (measured or expected) | risk | size | state |
|---|---|---|---|---|---|---|
| 1 | **GBA PPU**: text BGs a tile at a time, compose back to front, sprite rows hoisted | PPU 36-57% of the GBA frame | AoS 9.92 → 7.01 (3.9% → 0%), DKC2 12.5 → 10.0 (8.1% → 0%), exact | low (hash-checked on 8 games) | 1 file, ~+40 lines | **done** 76da654 |
| 2 | **GBA WAITCNT**: rebuild only the cartridge's timing regions (was a 512 KB refill per write) | 2.2% of AoS, all of its tail | AoS p95 15.6 → 12.1, over budget 3.9% → 0.03%; exact | none | 8 lines | **done** f4eec1e |
| 3 | **PGO trained on the DS+** (`DSPERATE_PGO=use`, profile from the bench scenes) | — | trained scenes −2..−13% (gs1 12.4 → 11.1, gsboot 7.3 → 6.4, nsmb0 10.2 → 9.5, dkc2 10.0 → 9.3); held-out −1..−7% (plat −7%, dos −6%, mmz −6%, gsgba −2%, st −1%); exact | low: the profile is pinned to compiler + flags (configure refuses a mismatch); edited functions silently lose theirs, so retrain per package | a training script + the profile next to the PKGBUILD (which already picks it up) | **shipped** in package .78 (`dsperate/pgo/`, trained on 71625ce) |
| 4 | Gpu3D geometry (`submit_vertex`, `drain_all`, `exec_single`, polygon emit) | 10-21% of the DS emulation thread | expected 2-4% of DS frame time | medium (melonDS-exact maths) | medium | open: by line it is spread over the command walk and the vertex/polygon maths, already NEON; no single hot spot |
| 5 | DMA in Golden Sun | 17% of gs1's emulation thread | small | — | — | by line, this is the GXFIFO burst into the geometry log (`gxfifo_dma_burst`, inlined into `run_channel_impl`), i.e. geometry work (item 4), not DMA overhead |
| 6 | JIT block transitions (dispatch, lookup, `find_native`, indirect branches, `call_pure`) | 15-18% of every DS scene; the two dispatch stubs' LUT jump alone ~2-3% | expected 3-6% (a return-address prediction or inline indirect-branch cache in translated code) | high (JIT core, verified by the fuzzer and scene hashes) | large | open: the best remaining DS item, a project of its own |
| 6b | Assassin's Creed's direct geometry-port stores (`jit_h_st32` → `Bus::io_write` → `gx_port_write` per word) | ~10% of ac's emulation thread | expected 2-4% in ac | medium | medium | open: the C path already skips Io; what is left is the store helper's call chain per word |
| 7 | SPU mixing (`run_channel`, `mix`, `fifo_fill`) | 5-8% | channel-major batches measured 0 (exact) | low-medium | medium | tried, not kept |
| 8 | GBA interpreter (run loop, prefetch-buffer model `pf_fetch` 5%, `jump`) | 34-59% of GBA; by line the instruction pipeline's fetches (page-table read per fetch ~5%, `code_fetch` ~4%) | small from micro-work: inlining `pf_fetch` measured 0 (LTO already did) | medium (cycle-exact against mGBA's suite) | medium | tried inline: no gain, not kept |
| 9 | **GBA JIT** (gba-plan phase 4) | the interpreter, above | DKC2 maybe 10 → 5-6 ms | **high**: the prefetch buffer, pipeline (SMC) and IRQ-delay rules are per instruction and the mGBA-suite numbers depend on them; a JIT either calls back per instruction (little gain) or approximates (inexact) | very large | **not recommended now**: after 1-3 every GBA game measured is well inside budget; worth it later only for battery |
| 10 | GBA lazy sound mixing (no sample event every 512 cycles; frames made on demand) | 548 events a frame | **none measurable** on the DS+ (AoS 9.92 → 9.90); exact | low | +35 lines | tried, not kept (`bench/core/gba-lazy-audio.patch`) |

### Simplifications and dead code

| item | where | note |
|---|---|---|
| Word and halfword DMA runs were two copies of the same 70 lines | `dma.cpp` `run_channel_impl` | **done** 03cd984: one template (`direct_run<4/2>`); hashes unchanged on the DS scenes, speed unchanged |
| Stale header notes in GBA mode ("not yet: Flash/EEPROM, prefetch, RTC, recompiler") | `gba.h` | **done** 7a78b9c |
| The GBA interpreter sets the DS's fast-timing data cost per instruction | `interp_*.cpp` | already off at run time in GBA mode (`ExactTiming`): no cost, no change made |
| The interpreter slice loop and the native one are two copies of the scheduler's slice logic | `scheduler.cpp` `run_until_impl` / `slice_next` | duplication by design (the native one is flattened for the JIT); merging is not worth the risk |

## Log of changes (same baselines after each)

| commit | aos | dkc2 | gs1 | gsboot | nsmb0 | mkds | ac | hashes |
|---|---|---|---|---|---|---|---|---|
| base 9cca5e0 | 9.92 (3.9%) | 12.52 (8.1%) | 12.49 (0.4%) | 7.33 (0.2%) | 10.19 (0%) | 10.44 (0.2%) | 5.70 (0.7%) | reference |
| f4eec1e WAITCNT | 9.93 (0.03%) | 12.50 (8.4%) | — | — | — | — | — | same |
| 76da654 GBA PPU | 7.01 (0%) | 9.99 (0%) | 12.42 | 7.34 | 10.23 | 10.38 | 5.74 | same |
| blend fast path (dropped) | 7.03 | 10.13 | | | | | | same, no gain |
| + PGO (measurement build) | 6.78 | 9.28 | 11.14 | 6.40 | 9.48 | 10.14 | 5.41 | same |
| 03cd984 DMA template | 7.00 | 10.00 | 12.33 | 7.44 | 10.22 | 10.58 | 5.75 | same |
| SPU channel-major batch (dropped) | | | 12.43 | 7.40 | 10.21 | 10.45 | 5.74 | same, no gain |
| **package .78 (71625ce = core-speed + render, PGO), installed** | **6.70 (0%)** | **9.56 (0%)** | **11.13 (0.2%)** | **6.61 (0.1%)** | **9.48 (0%)** | **9.78 (0.2%)** | **5.45 (0.7%)** | same as base |

Held-out games, base build → package .78: Spirit Tracks 12.48 → 11.96, Platinum 3.01 → 2.84,
Dawn of Sorrow 2.99 → 2.83, GBA Golden Sun 6.00 → 5.76, Mega Man Zero 10.32 → 9.52 (the first column
is 76da654, which already had the GBA changes; all hashes the same).

### GBA idle iterations (core-speed 9b2bbc0, 2026-10-18 evening)

Advance Wars 2 (the user's game in progress, `aw2s`) was CPU-bound: its main loop does not halt
but polls a RAM flag set by the VBlank IRQ and KEYINPUT, ~1,700 times a frame, through two calls that
push to the stack, so no "no stores" idle rule could take it. `Gba::Idle`: at a loop's backward
B/B<cond>, an iteration that leaves the machine exactly as it found it (registers, pipeline,
prefetch buffer, every stored byte back to its old value, reads only from memory and from I/O that
only an event changes) is followed by identical ones until something outside changes (DMA, IRQ
lines, serial, keys; the line for a loop reading DISPSTAT/VCOUNT), so whole iterations up to the
slice end are charged without being run: the same state at the same cycle. Exact: hashes on eight
GBA games, ctest, mGBA's suite unchanged.

PGO against PGO (each trained on aos dkc2 aw2 aw2s for its own commit), median ms (p95):

| scene | 03cd984 | 9b2bbc0 |
|---|---|---|
| aw2s (user's game) | 9.66 (10.12) | **8.53** (9.13) |
| aw2 (menus) | 9.18 (10.42) | **8.20** (10.52) |
| dkc2 | 9.25 (13.84) | **6.84** (14.51) |
| mmz (held out) | 9.53 (10.41) | **6.49** (8.27) |
| aos | 6.63 (8.81) | 6.72 (8.95) |
| gsgba (held out) | 5.69 (7.31) | 5.74 (7.45) |

The checks cost ~1% where nothing idles and DKC 2's worst frames +5% (0.03% of frames over budget);
without PGO, LTO's inlining moved against it (render_line folded into the run loop) and the cost
looked larger. Kept.

## Exactness

- Every change above: frame and audio hashes identical on every bench scene it touches (the DS ones
  for the DMA change, the GBA ones and six more GBA games on rgbuild for the GBA changes).
- `ctest` 27/27 (jsmolka's arm/thumb/memory/bios/nes/save tests, the PPU demo hashes, DS tests).
- mGBA's suite with 03cd984: Memory 1498/1552, I/O read 130/130, Timing 1276/2020, Timer count-up
  504/936, Timer IRQ 71/90, Shifter 140, Carry 93, Multiply long 52/72, BIOS math 603/615, DMA
  1244/1244, SIO R/W 25/90, Misc 4/12: the same as gba-plan.md's table.
- The PGO build: same hashes as the build without it on all 12 scenes.

## PGO

`bench/core/pgo-train.sh COMMIT` builds the commit instrumented on rgbuild (the PKGBUILD's flags),
runs the bench scenes on the DS+ and writes `dsperate/pgo/` (.gcda files, MANIFEST with the
fingerprint), which the PKGBUILD already turns into `DSPERATE_PGO=use`. 784 KB. Retrain with every
package: a new compiler or flags makes the configure refuse the profile (the package build fails
loudly rather than going slower), edited functions silently lose theirs.

## Next package

Fork `pocketos` 9ec9871 = 71625ce + render 300f7d6 + gba-render 387b5c9 (vectorised GBA compose) +
core-speed 9b2bbc0 (idle iterations). ctest 27/27, GBA hashes identical (6 games on rgbuild), DS
hashes identical on the 6 DS scenes; one run in five of `gsboot` had a different bottom screen at
frame 3286 only (a one-frame picture race, emulation and audio identical; handed to the render
session). Packaging is left to the tidy session, which puts its frontend work on top and retrains
PGO once. Not pushed to GitHub yet (needs the user).

## Still open (ranked)

1. JIT block transitions (item 6): the largest remaining DS cost that is ours; a project of its own.
2. Assassin's Creed's per-word geometry-port stores from translated code (6b).
3. Gpu3D command walk and vertex maths (4): spread thin, 2-4% at best.
4. GBA JIT (9): only if battery life asks for it; everything measured runs inside budget.
