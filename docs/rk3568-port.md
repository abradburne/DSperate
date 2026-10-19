# The RG DS+ (RK3568) port

This fork of DSperate (branch `pocketos` of
[abradburne/DSperate](https://github.com/abradburne/DSperate), on top of
[beebono/DSperate](https://github.com/beebono/DSperate) `7c1fb67`) is the emulator
nibOS (formerly pocketOS) ships for the Anbernic RG DS+ (RK3568: four Cortex-A55 at 1.99 GHz, a Mali-G52
on Panfrost with GLES 3.1, two 1024x768 panels). It adds:

| | where | what |
|---|---|---|
| GBA mode | `src/core/gba`, `src/core/psg`, `frontend/sdl/gba_frontend.*` | a Game Boy Advance machine beside the DS one |
| Hi-res 3D | `frontend/sdl/gl_hires*`, `core/gpu` (display list, kept lines) | the 3D drawn again on the GPU at 2-4x, composited with the 2D there |
| GLES present | `frontend/sdl/gl_present.*`, `gl_frame_presenter.*` | the panels through GLES on the present thread, at 1x too |
| Fallback | `frontend/sdl/hires_governor.h` | hi-res 3D drops to 1x while the GPU cannot keep up |
| Real-time budget | `frontend/sdl/rt_budget.h` | the emulation thread survives RLIMIT_RTTIME |
| Debug switches | `frontend/sdl/debug_options.*` | `DS_DEBUG`, below |
| Tests | `tests/gba_test.cpp`, `gl_hires_test.cpp`, `hires_governor_test.cpp` | below |

Everything here keeps the emulation exact: frame and audio hashes are unchanged by every
speed-up, and the hi-res picture equals the 1x one wherever the 1x one has no edge
(tested; the known exceptions are listed under *Exactness*).

## Build, test, profile

The build is upstream's (`README.md`). The handheld's package builds it with the flags in
`packaging/arch/PKGBUILD`:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DDSPERATE_TUNE=-mcpu=cortex-a55 \
  -DDSPERATE_NET=OFF -DDSPERATE_CHEEVOS=OFF
cmake --build build
ctest --test-dir build
```

GLES 3.1 headers (`GLES3/gl31.h`, libGLESv2) turn on the hi-res path and its test;
without them it compiles as a stub that declines. The tests of ours:

| test | what |
|---|---|
| `gba` | jsmolka's gba-tests (arm, thumb, memory, bios, nes, save chips) end on mGBA's "all passed" screen; picture hashes of three demos against mGBA 0.10.5; the bus, timers, DMA, IRQs, a state round trip; the GB PSG's four channels; save-chip detection; every WAITCNT value against GBATEK's table; EEPROM over DMA; the RTC pin by pin |
| `gl_hires` | the hi-res path end to end on SDL's offscreen GLES context: scenes set up as a game would (registers and geometry commands through the bus), drawn at 2x and 4x, compared with the CPU's 1x picture away from edges and against golden hashes for the renderer they were recorded on (`tests/data/gl_hires/golden.inc`; `test_gl_hires --print` prints them, `--dump DIR` writes the pictures). Skipped without a GLES 3.1 context |
| `hires_governor` | when the fallback drops to 1x and comes back, frame by frame |

Exactness across a change is checked on whole games, outside the repository (the ROMs
are not ours to commit), with the headless frontend's hashes:

```bash
dsperate-headless --frames 3600 --hash-frames frames.txt --dump-audio audio.pcm game.nds
dsperate-headless --gba --frames 3600 --press 400:start:4 --press 475:a:4 --hash-frames f.txt game.gba
```

and, for what reaches the panels, `DS_DEBUG=present-wait,present-hash=FILE` (a hash per
present; under `--replay` the RTC is frozen, so a run repeats exactly). `bench/core/bench.sh`,
`bench/core/gba-exact.sh`, `bench/render/render-same.sh` and `bench/render/render-bench.sh` wrap
these for the DS+ (`bench/README.md`); their reports (`docs/rk3568/core-review.md`,
`docs/rk3568/render-review.md`) have the
measurements behind the design choices below.

The package is built with profile-guided optimisation (`DSPERATE_PGO=use`, the profile
trained on the DS+ by `bench/core/pgo-train.sh` into `pgo/aarch64-gcc<version>/`): 2-13% off the median frame time.
Retrain it for every package; an edited function silently loses its profile.

## The GBA machine

A DS runs GBA cartridges on its ARM7, switched into GBA mode. Here that is a machine of
its own, `gba::Gba` (`core/gba/gba.h`), which `NDS` holds while a GBA game runs
(`NDS::load_gba`, `NDS::gba`); nothing on the DS path changes for it.

- **CPU.** The DS's ARM7TDMI interpreter compiled a second time
  (`gba_interp_arm.cpp`, `gba_interp_thumb.cpp`) with `DS_INTERP_GBA` set: its memory
  accessors are the GBA bus's (`gba_cpu_mem.h`), its cycle charges the GBA's with the
  cartridge prefetch buffer (`gba_cpu_cycles.h`), and the two-instruction pipeline is kept
  for self-modifying code (`gba_interp.cpp`). No recompiler: every game measured runs
  inside its frame time interpreted.
- **Time.** GBA cycles (16.78 MHz), an event loop of its own (PPU lines, timers, sound,
  IRQ delay, serial). `run_frame()` runs to the start of VBlank. Loops that only poll
  (Advance Wars 2's VBlank wait) are proven side-effect-free and charged, not run
  (`Gba::idle_*`).
- **Picture.** `gba::Ppu` draws each line at HBlank from the live registers (modes 0-5,
  sprites, windows, mosaic, blending), composed with vector masks; `Gpu::show_gba`
  centres the 240x160 frame on the top screen, which at 4x is exactly 960x640 inside the
  panel.
- **Sound.** The Game Boy PSG (`core/psg/gb_psg.*`, a component any machine can carry,
  AGB model) and the two Direct Sound FIFOs, mixed as the GBA's DAC does
  (`gba_audio.cpp`), into NDS's audio ring at 32768 Hz.
- **Cartridge.** SRAM, Flash 64/128 KB and EEPROM 512 B/8 KB, the type from the SDK's
  string in the ROM (`gba_save.*`, files in mGBA's layout); the GPIO real-time clock
  (`gba_rtc.*`).
- **BIOS.** A dump if given (`paths.gba_bios`), else the built-in Cult-of-GBA BIOS
  (`gba/bios`, MIT).
- **Frontend.** `--gba` or a `.gba` (or a zip holding one) starts it; GBA mode defaults
  to the single-screen layout. The game code, the save file and the start line are in
  `frontend/sdl/gba_frontend.*`.

mGBA's test suite (built with devkitARM, run through mGBA's debug port in both):
DSperate is at or above mGBA 0.10.5 on I/O read, timer IRQ, BIOS math, DMA and the
misc. edge cases, level on shifter, carry and multiply, below on memory (1498/1552),
timing (1276/2020), timer count-up (504/936) and SIO register reads (25/90 against 26):
the prefetch buffer's exact cycle in some sequences, and DMA starting mid-instruction,
which an instruction-at-a-time interpreter does not model.

## Hi-res 3D

`video.hires3d = 2..4` (`--hires3d N`). The CPU raster still draws every frame at 1x:
the game can capture the screen, and the 1x picture is the reference. The hi-res path
draws the same frame again for the display.

```
emulation thread                                  present thread (GLES 3.1)
----------------                                  -------------------------
Renderer3D (CPU raster, 1x)                       GlFramePresenter::draw
  + DisplayList: the frame's polygons,              GlHires::render
    vertices, post tables, in the GPU               |
    raster's layout; textures decoded once          |  draw3d
    into an append-only arena (epoch)               |    arena upload    new textures only
Engine2D (engine A, 1x)                             |    build fans      opaque prefix | tail runs
  + HiresLine per line: the composited out,         |    prepass         DS depth test, list order;
    what the 3D covers (under), top/second          |                    winner id, colour, texel
    records, ids, kinds, window bits, a             |                    -> visibility buffer
    class per pixel (0: 3D plays no part,           |    resolve         winners shaded once a pixel
    1: 3D on top, nothing after it,                 |    tail            translucent + shadow volumes,
    2: needs the full composite)                    |                    run by run, stencil = DS rules
                                                    |  composite
GlFramePresenter::present                           |    lines upload    mapped buffer, ring of 3
  snapshot -> post job ------------------------->   |    pass 1          class 0/1 pixels
                                                    |    pass 2          effects + master brightness
                                                    |                    (+ edge marking, fog)
                                                    v
                                                  S x picture -> GlPresent draws it in the window
```

Why it is shaped like this (the measurements are in `docs/rk3568/render-review.md`):

- **Visibility buffer.** Golden Sun draws ~4x the screen in opaque overdraw; shading
  every fragment was most of the GPU's time on the one-core G52. The pre-pass writes
  the DS's own integer depth from the shader so ties resolve as on the DS (the earlier
  polygon keeps the pixel); the resolve shades each pixel once.
- **Runs.** The tail is drawn in list order, a draw per run of polygons with the same
  state (`RunKey`): translucency, depth write, polygon id (the stencil keeps "no blend
  over your own id"), shadow mask or shadow. A translucent polygon at alpha 31 is drawn
  twice: its alpha-31 texels as an opaque polygon's (they write depth), the rest blended;
  each part only where its texture can have such texels.
- **Classes.** Engine A decides per pixel, from priorities rather than 1x coverage, what
  the 3D is to that pixel, so most pixels need no colour-effect work and the 3D's edges
  are the hi-res ones.
- **Captures.** A game that shows the 3D through display capture (Golden Sun swaps
  engine A between the screens every frame) gets its capture redone at hi-res; a screen
  whose 1x picture is a recent capture's shows that capture's hi-res picture.
- **Rings.** Per-render inputs go round rings of three buffers and outputs round six
  textures: rewriting a buffer the GPU still reads made Panfrost shadow it, ~3 ms of
  kernel time a frame.

### Exactness

Compared with the CPU's 1x picture (`test_gl_hires`):

- Opaque polygons in depth order, Gouraud colour, direct-colour and A3I5 textures,
  perspective W-buffered polygons, edge marking, fog, the 2D layers' priorities around
  the 3D, windows, alpha blending with the 3D as either target, master brightness and
  display capture: equal (the scenes in `tests/gl_hires_test.cpp`).
- Translucent polygons blend in GL's fixed function on 8-bit targets, which rounds where
  the DS truncates: one step in 64 per translucent layer.
- A translucent polygon over a transparent rear plane (clear alpha 0): the DS writes it
  unblended; GL blends it with the clear colour (kept as the `translucent_over_clear`
  scene, hashed but not compared).
- Quads whose texture coordinates are not affine over the quad (a trapezoid mapped to a
  square) are drawn as two triangles and interpolate differently from the DS's spans.
- The DS interpolates perspective approximately; its texel steps can sit up to a pixel
  from the exact ones the GPU draws.

### Fallback and the present

The present thread draws a frame while the emulation runs the next (`gl_frame_presenter.h`
has the hand-off). When the GPU is still drawing the frame before, a hi-res frame is left
out rather than holding the emulation back (decided once a display period, so capture
pairs stay whole). `HiresGovernor` (`video.hires3d_fallback`, on by default) drops the 3D
to 1x when 15% of two seconds' frames were left out, and tries the higher scale again
after 10 s, doubling to 80 s when it falls back again within 3 s. At 1x the GLES present
is used for the windows too, and there a frame waits for the one before (vsync pacing).

The emulation thread runs real-time under DSlot. PipeWire sets RLIMIT_RTTIME (200 ms)
in every process it joins; `RtBudget` naps briefly once a paced frame as a real-time
thread, and drops the policy while unpaced (fast forward). One-shot work that can outlast
the limit runs under `OffRealtime`, as an ordinary task: startup from the ROM to the starting
state, the picker's launch, and every save and state load or save. Inflating a zipped 8 or
16 MB GBA ROM takes 155-200 ms on the A55s; before this the kernel killed Advance Wars 2,
DKC2 and King of Swing a quarter of a second after DSlot started them.
`bench/launch/rt-limits.sh` starts games under dslot.service's limits and fails on any that
is killed.

A game that cannot start (unreadable file, damaged zip, no game in it, a bad BIOS dump, a
`--load-state` that fails) shows its name and the reason on the panel until a button is
pressed, then exits; DSlot is back on the shelf. The log has the details.

SIGTERM puts a game away as Menu does: the auto state and the battery save are written and
DSperate exits 0 (DSlot sends it for nibOS's power menu, and systemd at shutdown). SIGINT and
SIGTERM are blocked in every thread and taken by one waiter, so they never interrupt another
thread's call. With two windows DSperate sets `SDL_KMSDRM_ATOMIC=0` unless the environment
sets it: SDL3's atomic KMSDRM path has no plane for the second panel and crashed in the swap,
mid-game or while leaving (the old launcher exported it; DSlot does not).

Frame pacing is locked to the panels' vblanks in DS mode (`video.vsync_lock = auto`,
`vsync_lock.h`): frame i flips on vblank base + i of one panel, the console runs at the panel's
59.94 Hz with the sound resampled (+0.19%), and the pad is read again at line 192, just before
the game's VBlank handler (`emu.input_at = vblank`). No frame is shown twice any more where
the panel's rate is off the DS's; NSMB's button-to-screen time fell from 79.8 to 69.6 ms
median. In GBA mode the lock stays off unless forced: there it cost a refresh (our part of
Advance Wars' latency 25 -> 35 ms, median of 20 presses). The cause is the lock's budget,
which counts the wait for the previous flip as present time and so sits at its cap (a period
+ 2 ms) in every game; DS games at 2x are GPU-bound enough not to show it. Measuring the
present's own work instead is the follow-up (`docs/rk3568/measurements.md`, latency).

## DS_DEBUG

The hi-res path's and the GLES present's switches, one environment variable read once
(`frontend/sdl/debug_options.h`); `DS_DEBUG=help` lists them:

| key | what |
|---|---|
| `marks` | launch stages on stderr, against CLOCK_BOOTTIME (DSlot logs its own on the same clock) |
| `present-wait` | every frame shown: the present waits for the GPU instead of leaving a frame out |
| `present-hash=FILE` | per present: window, count, FNV-1a of the picture, and for a window one 1x screen fills the hash it must have |
| `present-ppm=N[+COUNT]:PREFIX` | the windows' pictures at presents N.. as PPM |
| `hires-stats` | CPU ms per stage of a render, every 120; frames drawn at once / captured, every 300 |
| `hires-stages` | GPU ms per stage (timer queries; they change Panfrost's batching, so they rank) |
| `hires-bench=N[:MASK/MASK/...]` | render N drawn 60 times per mask of left-out stages (`GlHires::Stage`), glFinish'd, timed |

The rest of the emulator's switches are upstream's own `DS_*` variables (`DS_VERBOSE`,
`DS_FRAME_STATS`, ...).

## Keeping upstream merges cheap

Our code lives in files of its own wherever it can; in upstream's files it is hooks:
`main.cpp` (about 100 lines: the GBA flags and load, the hi-res and RT-budget calls),
`Gpu` / `Engine2D` / `Renderer3D` (the kept lines and display list, behind `set_hires` /
`set_display_list`), the interpreter's `DS_INTERP_NS` / `DS_INTERP_GBA` seams, and the
zip reader's GBA entry. Commit subjects use upstream's prefixes (`frontend:`, `gpu:`,
`gba:`, `tests:`, `docs:`, `config:`).
