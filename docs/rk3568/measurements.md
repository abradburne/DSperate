# DSperate on the RG DS+: choice and measurements

The aim: a DS emulator started from DSlot's shelf, quick to come up on both panels. Home saves
state and returns to DSlot; Back brings up a menu (save states, reset, shaders).

## Emulator selection, October 2026 (history)

How the emulator was chosen, kept as a dated record. It is not an ongoing benchmark: builds are
measured against our own earlier builds and targets (the sections below, `bench/`).

**Choice (2026-10-16): DSperate** (github.com/beebono/DSperate, GPLv3, C++17 on SDL2), measured
against DraStic on the DS+. It is fast enough on everything tried, it is open source, and it
already has most of what DSlot needs:
- a dual-window mode that puts one DS screen on each panel, with its own KMS page flips and the
  RK3568's RGA doing the scaling;
- a pause menu drawn over the game (save/load, options, cheats, quit);
- 10 save-state slots, plus a hidden auto slot written on quit and resumed at the next start;
- filters (nearest, bilinear, LCD grid, sharp box seams, integer scaling);
- a built-in FreeBIOS, so no BIOS dumps are needed.

### Measured on the DS+ for the selection (dev system, kernel 7.3-rc7, CPU 1.99 GHz, schedutil)

**DSperate**, built at commit 7c1fb67 (`-mcpu=cortex-a55`, Release, no PGO, `DSPERATE_NET=OFF`).
Emulation cost per frame, unpaced, from `dsperate-headless --direct --frames 3900 --stats-from 300`:
each game's first 65 s from boot with no input (intros, attract demos), with the first 5 s left out.

| Game | median ms | p99 ms | frames over 16.7 ms |
|---|---|---|---|
| Castlevania: Dawn of Sorrow | 4.1 | 6.4 | 0 / 3600 |
| New Super Mario Bros. | 9.1 | 13.9 | 4 (0.11%) |
| Pokémon Platinum | 2.9 | 11.1 | 0 |
| Mario Kart DS | 10.1 | 13.8 | 7 (0.19%) |
| Super Mario 64 DS (title) | 3.5 | 3.8 | 0 |
| Super Mario 64 DS (gameplay replay, `scenes/sm64.dsin`) | 6.3 | 14.5 | 9 / 1500 (0.6%) |
| Zelda: Spirit Tracks | 10.2 | 14.6 | 2 (0.06%) |
| Golden Sun: Dark Dawn | 6.7 | 14.2 | 4 (0.11%) |

**DraStic** r2.5.2.2 (Linux aarch64, from ROCKNIX's packages repo, SHA-256 checked against its
recipe; redistribution allowed by its author). It has no headless mode, and its fast-forward is
slower than real time here, so it was only measured in real time (a version of `bench/render/run-bench.sh` that
ran both, presents timed by an `LD_PRELOAD` shim; both since made DSperate-only or removed). It holds the panels' rate in every game, but that doesn't show how much
headroom it has. It's closed source and abandoned, and its menu is its own.

**Not chosen** (at the time):
- **NNDDSS, the stock emulator:** Android DraStic (`com/dsemu/drastic` in `libnnddss.so`) inside
  bionic/Android shims, an opaque binary from the firmware.
- **melonDS:** reported at 30–40 fps in 3D games on this class of CPU.

## Found: the panels ran at 56 Hz, not 60 (fixed 2026-10-17)

**Fixed:** the DTS now sets VPLL to 1188 MHz, so the dclks are 1188/19 = 62.526 MHz and both
panels refresh at **59.937 Hz** (`bench/tools/vblank.c`; details in `kernel/README.md` in nibOS). DSperate,
re-measured in real time, holds full speed (below). What was found:

Both panels refreshed at **56.08 Hz** (`bench/tools/vblank.c`). The mode asks for 62.8 MHz
(1304×800 total, 60.2 Hz), but `dclk_vop0/1` run at **58.5 MHz**, the nearest the CRU gives. So
everything synced to the display ran at 56.1 fps, 94% of the DS's 59.83 Hz: DSperate (both
panels), DraStic and Hyprland alike. Fix this in the kernel/DTS (a VOP dclk parent that can make
the exact clock, e.g. VPLL, or panel timings whose clock the CRU can make at 59.8–60 Hz). Then
re-measure DSperate in real time (`DS_FPS=1` prints emu/present/wait per second).

## Tools here

- `bench/tools/vblank.c`: the panels' real refresh rate (300 vblanks per CRTC).
- `bench/render/run-bench.sh`: one DSperate build, seven games, on the panels without sound.
- `bench/render/bench-realtime.sh`: the packaged DSperate, seven games, on the panels with sound.
- `bench/launch/launch-timing.sh`, `bench/launch/press.py`: A on the shelf → game → Menu → shelf, timed from the journal.
- `bench/render/render-bench.sh`, `bench/render/render-same.sh`: DSperate's render and present cost per game, and whether
  two builds show exactly the same pictures (`render-review.md`: the review of that path).

The games are on the TF2 card, which is exFAT, mounted at `/mnt/ds` with the kernel's driver.

## Real time on the panels at 59.94 Hz (2026-10-17)

`bench/render/bench-realtime.sh`: DSperate as packaged (`dsperate/`), both panels, sound through PipeWire,
frame-skip off, no real-time priority; 65 s from boot, first 5 s left out. Every game runs at
59.8 fps (the DS's own 59.83 Hz; the panels' 59.94 Hz is closer to it than 60 would be).

| Game | median ms | p99 ms | frames over 16.7 ms |
|---|---|---|---|
| Castlevania: Dawn of Sorrow | 7.1 | 14.2 | 8 / 3589 (0.22%) |
| New Super Mario Bros. | 10.6 | 15.5 | 13 (0.36%) |
| Pokémon Platinum | 6.9 | 12.7 | 13 (0.36%) |
| Mario Kart DS | 9.1 | 14.6 | 12 (0.33%) |
| Super Mario 64 DS (title) | 5.7 | 10.2 | 1 (0.03%) |
| Zelda: Spirit Tracks | 9.6 | 14.6 | 8 (0.22%) |
| Golden Sun: Dark Dawn | 9.3 | 19.1 | 58 / 3527 (1.64%) |

That run sent frames to the panels with DSperate's own KMS page flips, scaled by the RGA (its
defaults). The installed settings use SDL's GL renderer instead (`video.sink = renderer`), with
nearest-neighbour scaling on the GPU: 256×192 → 1024×768 is exactly 4×, so every DS pixel is a
sharp 4×4 block. Why, from Assassin's Creed: Altair's Chronicles (a 3D-heavy scene, the same
save state each time, 40 s, fresh boot):

| Output | fps | emulation over budget | present | seconds under 59.5 fps |
|---|---|---|---|---|
| KMS flips, RGA scaling (bilinear: blurred) | 29 | — | 30 ms | all |
| KMS flips, CPU scaling (nearest) | 59.8 | 1.40% | 0.1 ms | 1 of 40 |
| SDL GL renderer (nearest) | 59.6 | 0.04% | 10 ms | 3 of 39 |

The CPU scaler competes with the 3D threads (emulation 9.4 ms a frame against 5.4 ms). The GL
renderer's present blocks for ~10 ms, since SDL waits for each panel's flip in turn; frames are
still on time almost always.

**Suspend:** with DSperate's own KMS flips on screen, suspending hangs the DS+ for good when the
CPUs go offline (`pm_test` = processors; lid close needs a forced power-off). The shell, DSlot's
shelf and DSperate on the GL renderer all suspend and resume. Not yet found in the kernel.

## DSlot integration (2026-10-17)

DSlot (its own repository) starts a game with `/usr/lib/dslot/launch ROM`
(`rootfs/dslot/launch` in nibOS). DSlot quits SDL first, which gives up both panels; DSperate takes
them in dual-window mode (KMS page flips straight to each panel: KMSDRM display 0, DSI-1, is the
bottom screen and gets the touch); DSlot starts SDL again when it exits. Installed by
`rootfs/dslot/install.sh` in nibOS with the settings in `rootfs/dslot/dsperate.ini` in nibOS.

- **Saves:** battery save, states and the auto slot in `/mnt/roms/Saves/NDS/<ROM name>/` on the
  ROMS partition. Menu writes the auto slot and quits; the next start resumes from it.
- **Keys:** Menu alone (BTN_MODE, SDL's guide, DSperate's pad modifier) = save and quit; Back =
  pause menu (save/load states, reset, OPTIONS with scaling, filters and the FPS counter, each
  kept for one game or for all); R2 held = fast forward (~6× here). Face buttons as labelled
  (DSperate's face_fix corrects SDL's x/y), one stick on the d-pad.
- **The TF2 library:** the kernel now has exFAT (`CONFIG_EXFAT_FS=m`, `kernel/build.sh` in nibOS);
  `/mnt/ds` is an automount. Under fuse-exfat DSperate took the card for a network share and
  read each whole ROM into memory before starting.

**Times** (`bench/launch/launch-timing.sh`, keys injected with `bench/launch/press.py`, CLOCK_BOOTTIME marks from DSlot and
from DSperate's `DS_MARK=1`; warm, resuming from the auto slot):

| | s |
|---|---|
| A on the shelf → DSlot's insert animation done, launcher started | 1.00 |
| DSperate started → SDL ready | 0.35 |
| → first frame on both panels | 0.27 |
| **A → game on screen** | **1.66** |
| Menu pressed → DSperate leaves (an 80 ms tap; 0.27 when SDL held guide down ≥ 250 ms) | 0.10 |
| auto slot written (5.6 MB) | 0.07 |
| DSperate closed | 0.12 |
| DSlot's shelf back on screen | 0.33 |
| **Menu → shelf** | **0.60** (was 0.82) |

Before tuning, A → game was 2.1–2.55 s and Menu → shelf 0.9 s.

- Menu → shelf: SDL keeps a gamepad's guide button down for at least 250 ms
  (`SDL_MINIMUM_GUIDE_BUTTON_DELAY_MS`), so a quick tap left the game late. DSperate now takes
  guide's release from the joystick button it is mapped to and drops SDL's late one; it leaves
  ~20 ms after Menu is let go.
- A → game: starting DSperate while the insert animation plays (DSlot `DSLOT_PRESTART=1`,
  DSperate `DS_WAIT_FD`: everything but video before the panels are free) was measured at
  1.73 → 1.45 s with no animation frame lost (longest 19–20 ms against 17–18). Left as
  prototypes (branches `prestart-proto` in both repositories): the launch time is fine as it is.

- `SDL_Init` was 0.45 s: SDL opens and closes every input device looking for controllers, and
  each close waits for an RCU grace period. Expedited RCU
  (`rootfs/dslot/pocketos-rcu.conf` in nibOS) makes it 0.24 s.
- SDL3's atomic KMSDRM path has no plane for the second panel and crashed DSperate (SIGSEGV
  in `KMSDRM_DestroySurfaces`) on every exit; the launcher sets `SDL_KMSDRM_ATOMIC=0`, as DSlot
  does.
- sdl2-compat 2.32.74 passes SDL3's keycode for Back to SDL2 programs untranslated; in SDL2's
  numbering it is "Sleep", which is what `dsperate.ini` binds.
- Real-time priority (`LimitRTPRIO=10` on dslot.service; DSperate runs SCHED_RR 5, presenters
  6). PipeWire's client library sets RLIMIT_RTTIME to 200 ms in the game's process, and the
  kernel resets a thread's count only when it sleeps *as a real-time thread*; DSperate's
  emulation thread takes its waits as SCHED_OTHER (the present's vsync wait, the pacer's spin),
  so it was killed (SIGKILL) about 0.65 s after the first frame, paced or not, not just in fast
  forward. DSperate now naps 20 µs a frame as a real-time thread when the limit is finite, and is
  SCHED_OTHER while fast-forwarding (or with the limiter off). With it, every game is faster
  (`bench/render/bench-realtime.sh`, KMS sink, CPU scaling; against the table above):

  | Game | median ms (RT) | p99 ms | over 16.7 ms (RT) | without RT |
  |---|---|---|---|---|
  | Castlevania: Dawn of Sorrow | 5.2 | 9.9 | 0.03% | 0.22% |
  | New Super Mario Bros. | 9.3 | 13.4 | 0.08% | 0.36% |
  | Pokémon Platinum | 4.1 | 10.9 | 0.19% | 0.36% |
  | Mario Kart DS | 7.4 | 10.9 | 0.25% | 0.33% |
  | Super Mario 64 DS (title) | 4.2 | 8.1 | 0% | 0.03% |
  | Zelda: Spirit Tracks | 8.6 | 13.8 | 0.03% | 0.22% |
  | Golden Sun: Dark Dawn | 7.3 | 16.1 | 0.96% | 1.64% |

**RAM** while playing: DSperate ~109 MB (PSS), DSlot staying resident underneath ~104 MB (its
artwork caches); ~420–500 MB of 984 MB still available.

**Source:** the fork [abradburne/DSperate](https://github.com/abradburne/DSperate), branch
`pocketos` (upstream 7c1fb67 plus our commits; the PKGBUILD pins one). Sent upstream:
`--saves DIR` / `--states DIR` ([#12](https://github.com/beebono/DSperate/pull/12)) and the
glib shim's missing byte-order macros, which kept Wi-Fi from building with GCC 14 and later
([#13](https://github.com/beebono/DSperate/pull/13)). Ready to send: guide's release from the
joystick, and real-time priority under PipeWire's RLIMIT_RTTIME. Ours only so far: `DS_MARK=1`
stage marks, hi-res 3D (below).

**Left to do:**
- Why DSperate's KMS path hangs suspend (its scanout buffers exported as dma-bufs, legacy page
  flips on both CRTCs); needs a console that survives the hang (pstore/ramoops, or UART).
- The GL renderer's present: flip both panels without waiting on each in turn (or a cheaper
  4× scaler for the KMS path once it suspends).
- A PGO profile trained on the DS+ (the committed one is for GCC 13.3; this is GCC 16). Not
  started: an instrumented build is easy (`DSPERATE_PGO=generate`), the training needs the
  ROMs of upstream's scenes or our own.
- Hi-res 3D: anti-aliasing at hi-res (`video.aa` acts on the 1x picture only); 4× (below);
  the two simplifications in fog and shadow volumes (below).
- Frame pacing and button-to-screen latency: measured and partly fixed (below); GBA mode and the
  two panels' 8.2 ms phase offset left.

## Hi-res 3D (`video.hires3d`, 2026-10-17)

`video.hires3d = 2` (or `--hires3d 2`, or `DS_HIRES3D=2`) on the renderer sink draws the 3D layer at 512×384
instead of 256×192; 4 is the panels' own 1024×768. Off by default. Edge marking (the outlines
in NSMB), fog and shadow volumes are drawn at hi-res too (2026-10-18, below).
It can be set in the pause menu (OPTIONS, 3D RESOLUTION; for one game or all), or for the
console or one game in DSlot's settings (from `dsperate.toml`), which DSlot passes as
`--hires3d N` and which wins over the menu.

How (DSperate fork, `pocketos` branch): the emulation is unchanged, the CPU raster still draws
every frame at 1x (display capture and the 1x picture stay exact). Its polygon list is kept
(`Renderer3D::set_display_list`), engine A keeps each line's composite inputs and a per-pixel
class (`Engine2D::HiresLine`), and a GLES 3.1 present of our own (instead of SDL_Renderer)
draws the 3D again at S× and composites it with engine A's lines on the GPU (`gl_hires.cpp`):
a cheap pass where the 1x pixel or the hi-res sample is the answer, engine A's colour effects
and master brightness only where they apply. A display capture of engine A is redrawn too,
and a panel whose 1x picture is exactly one of the last three captures shows that capture's
hi-res picture. Golden Sun moves engine A between the panels every frame and shows its 3D on
both through VRAM (display mode 2 and engine B), so guessing from the registers picked the
wrong panel. All GL work runs on the present thread, one frame in flight.

The frontend reads a finished frame after the next frame's `begin_frame()`. The hi-res path
first read the line set and 3D frame current at that point, which paired frame N−1's 2D with
frame N+1's 3D. That caused NSMB's map flicker and Golden Sun's panels swapping each frame.
`Gpu::hires_lines()` and `frame_3d_shown()` now give the frame that just finished. From boot,
Golden Sun's panels change resolution only at scene changes. They also drop to 1x during
fades, since master brightness on a capture shown through VRAM is not matched yet.

Golden Sun's terrain is translucent on the DS only because its textures carry alpha. The
DS draws those texels that come out at full alpha as opaque pixels: they write depth, so
farther polygons later in the list stay behind them. The 2× raster now draws them that way
too. Before, farther terrain painted over cliffs and characters (the user's states 1 and 2).

The 3D at 2× is GPU-bound on the DS+'s one-core Mali-G52. The DS depth comes from the shader,
so nothing is rejected early, and the opaque part of Golden Sun's title covers about 4× the
screen. Shading every one of those fragments in full took most of a 13 ms render, and the
intro's heavy stretch ran at 45–54 fps, so the music broke up. The opaque part is now a
visibility buffer. A cheap pre-pass runs the DS depth test and keeps each pixel's winner,
vertex colour and texel coordinate, then a full-screen resolve shades each pixel once. The
translucent tail is drawn as before. Measured with `DS_HIRES_BENCH` (the frame redrawn 60
times) and `DS_HIRES_STAGES` (GPU timer queries):

| Golden Sun title at 2× | GPU ms a render |
|---|---|
| one pass, full shader on every fragment | 13.0 |
| + no draw-buffer switch without edge marking | 11.1 |
| visibility buffer (pre-pass 5.0, resolve 2.5, tail 3.2) | 10.6 |

The intro stretch (seconds 8–28 from boot, real-time priority like `dslot.service`) went from
45–54 fps to 59.8 fps from cool, and to 58.2 fps hot (68 °C), with a few seconds at 52–55. What
is left is on the CPU side. At 2× the emulation thread takes 15.5–16 ms a frame against 13.2
at 1x, from engine A's select at 2× (+0.6 ms), the polygon list build (+0.4 ms), and contention
with the present thread and the GPU (+1.3 ms spread over the CPU and DMA).

Golden Sun: Dark Dawn from boot, 65 s, both panels, real-time priority (an earlier build):

| | 1x (SDL renderer) | 2× | 4× |
|---|---|---|---|
| average fps | 59.7 | **59.7** | 40 |
| seconds under 59 fps | 2 | 4 | 33 of 39 |
| emulation frames over 16.7 ms | 0.11% | 1.7% | – |

4× is GPU-bound here (~27 ms of GPU a frame in Golden Sun's busy stretch).

**Fog and shadow volumes at hi-res (2026-10-18, fork `9cca5e0`).** Loading Golden Sun's
slot 0 (its intro) at 2× made the lower screen flash pinky-orange. The intro is fogged orange.
The 2× renders had no fog, so they came out untinted, and every capture shown without its hi-res
copy (after a skipped present) fell back to the tinted 1× picture. Now:

- **Fog:** the 3D id plane carries each pixel's fog bit (opaque polygons and the rear plane set
  it); the edge pass does edge marking then fog, as the DS's final pass, with its density table,
  offset and shift on the pixel's DS depth (colour and alpha, or alpha only). Simplified: a
  translucent pixel keeps the fog bit of what is under it, where the DS also ANDs in its own.
- **Shadow volumes:** masks (shadow, id 0) set stencil bit 7 where their depth test fails,
  drawing nothing; the first mask after other polygons clears it. Shadows (id > 0) draw only
  there and not over their own polygon id, read from the opaque pre-pass's winner. Simplified:
  the clear covers the whole screen, where the DS clears only the lines a mask touches, and the
  own-id test is against the opaque polygon only.
- The id plane's writes switch with `glColorMaski` (Panfrost has `OES_draw_buffers_indexed`)
  instead of changing the draw buffers, which on a tiler ends the batch. With a draw-buffer
  change per run, Mario Kart's mask and shadow per kart cost 12 ms of GL calls a render.

Checked with `DS_HIRES_AB=<render>:<prefix>`, which writes one render as drawn, again without
shadow volumes, and the CPU raster's own 1× frame (the reference). Mario Kart's kart shadows
appear where the 1× frame has them. Golden Sun's lower screen from slot 0 now holds its tint
every frame. Cost at 2×: fog ~2 ms of GPU a render in Golden Sun's intro (1.1 ms the pass,
0.9 ms the tail keeping the id plane); shadows ~1.5–2 ms in Mario Kart's race demo, which stays
at 59.8 fps.

**Sound at 2× (2026-10-18).** In Golden Sun's field (the user's own save state, warm) the 2×
picture takes ~21 ms of GPU a frame. The emulation waited for the frame before at every
present, so the whole game, sound included, ran at 47 fps and the music broke up all the time;
the emulation itself was on time (0.04% of frames late). Three changes, measured with
`bench/render/audio-bench.sh` (the save state, both panels, real-time priority as under dslot.service; "dry"
frames are ones where the audio queue was found empty, a gap you hear):

- The GLES present no longer holds the emulation for a frame the GPU is still drawing (fork
  `3c877f0`): that display period isn't shown, and the game and its sound stay at full speed.
  About one picture in three is dropped in that scene (~40 shown a second). Decided per display
  period, so Golden Sun's screen pairs stay whole. `DS_PRESENT_WAIT=1` waits as before.
- `frameskip = 1` (adaptive) in `dsperate.ini`: when the emulation itself runs over (it does,
  warm), a display period isn't drawn instead of the sound running dry.
- The stock firmware's thermal limits (`hw/files/usr/lib/tmpfiles.d/pocketos-thermal.conf` in nibOS):
  limiting starts at 75 °C, not mainline's 70. From 70 °C the CPU fell to 1.42 GHz a few
  minutes in.

| Golden Sun field at 2× | fps | emulation frames late | dry frames | seconds with a gap |
|---|---|---|---|---|
| before, 65 s | 47.3 | 0.04% | 1395 | 51 of 51 |
| no wait on the GPU, 180 s warm | 59.7 | 4.51% | 65 | 19 |
| + frameskip 1, 180 s | 59.8 | 4.30% | 8 | 4 |
| + stock thermal limits, 180 s | 59.8 | 0.60% | 4 | 3 |
| as installed, 120 s | 59.8 | 0.10% | 2 | 1 (after loading the state) |

New Super Mario Bros. at 2× is unchanged by the present change (10 of 3903 frames not shown).
Warm and throttled at 70 °C it too had 4.8% late frames and 9 seconds with a gap, which the
thermal limits take care of.

Found on the way:
- **Vulkan on the DS+** (PanVK, `vulkan-panfrost-pocketos`, `rootfs/mesa` in nibOS): it runs on the
  G52 (`PAN_I_WANT_A_BROKEN_VULKAN_DRIVER=1`, Vulkan 1.0), but DSperate's own GPU raster
  (`video.gpu3d`) draws Golden Sun's title in 11.8 / 29.2 / 83.7 ms at 1x / 2× / 4×, where
  Panfrost's GL draws it in 2.5 / 4.9 / 13.5 (`tests/gl_probe.cpp` against `vk_probe`). Upstream
  had built and dropped a Vulkan hi-res path (`--internal-res`) for the same device class, on
  ARM's own driver.
- Where the 2× cost went, and what took it out: the composite as one big shader (a 2-pass
  split), re-uploading engine A's planes every frame (kept only for lines that need them), the
  bottom panel redrawn while unchanged (skipped), and all GL on the emulation thread, waiting
  on the GPU at every swap (moved to the present thread).

## Button-to-screen latency and frame pacing (2026-10-18)

Tools in `bench/latency/` (README in each script's header):
- `bench/latency/lat-press.c` writes a button into `gpio-keys-control` (pocket-input has it grabbed, but a
  write goes in through the grab's own handle, so pocket-input passes it on as a real press:
  only the GPIO interrupt and gpio-keys' debounce are left out) and stamps pocket-input's uinput
  event.
- DSperate's `DS_LATENCY=<file>` (fork, branch `pacing`): one line per emulated frame -- input
  taken, emulated, present begun, each window's swap, the GPU done (a `GL_TIMESTAMP_EXT` query
  before the GLES swap), a hash of each screen, each CRTC's last vblank (DRM_IOCTL_WAIT_VBLANK,
  relative 0, no wait). The kernel has no ftrace, so a flip is the first vblank of the panel's
  CRTC at least 1 ms after both the swap and the GPU are done. Checked with
  `SDL_VIDEO_DOUBLE_BUFFER=1` (the swap then waits for its own flip): swaps return 20 us after
  the vblank stamp. The top screen is DRM pipe 1, the bottom pipe 0, and **the panels refresh
  8.2 ms apart**.
- `bench/latency/latency.sh` starts DSperate from a save state ~22 times: two runs press nothing (the
  baseline; the emulation is deterministic from a state), the rest press once at a random
  moment. The game's response is the first shown frame whose picture differs from the
  baseline's. `bench/latency/matrix.sh` runs three games and several setting sets (`configs/`);
  `bench/latency/analyse.py`, `bench/latency/summary.py` turn them into the table below.
- `bench/latency/pacing-run.sh` + `bench/latency/pacing.py`: 65 s with no input, which vblank each frame went out on, per
  panel: repeated refreshes and frames never shown.

**Where the time goes**, DSperate as packaged (median of 30 presses; NSMB on the world map at
the installed 2x, d-pad right; Golden Sun: Dark Dawn at 1x in a dialogue, A; Advance Wars, GBA
mode, title screen, Start):

| game | frames to respond | wait for the frame | emulate | present | to the flip | **total** | p95 |
|---|---|---|---|---|---|---|---|
| NSMB (2x) | 2 | 4.9 | 43.5 | 9.0 | 16.3 | **79.8 ms** | 93.6 |
| Golden Sun (1x) | 3 | 5.8 | 72.5 | 3.4 | 10.5 | **102.1** | 113.7 |
| Advance Wars | 1 | 9.5 | 25.4 | 2.3 | 15.7 | **60.5** | 97.8 |

pocket-input adds 0.1-0.2 ms (gpio-keys to uinput). Most of each total is the game's own
frames (NSMB: 2, Golden Sun: 3-5 in a dialogue): emulated frames take 9-14 ms each, but the
response is shown a whole frame period apart. Ours on top: up to a frame of waiting for the
next frame start, and the present (2x: the GPU's ~18 ms a frame) plus up to a refresh to the
flip, where the GLES/renderer present waits for each panel's flip in turn.

**Pacing**, as packaged: a frame repeats on both panels every ~9 s (7-9 a minute), the panels'
59.937 Hz against the DS's 59.826. At 2x NSMB is GPU-bound: 52.9 frames shown a second, 470
repeated refreshes a minute; that is the hi-res cost, not pacing.

**What changed** (DSperate fork, branch `pacing`, ed38625; not yet in the package):
- `video.vsync_lock` (auto: on under KMSDRM): frame i flips on vblank base+i of
  `video.vsync_pipe` and starts a budget before it -- the longest emulation of the last 30 s
  plus the present's usual work, the commit and a margin, capped at a period + 2 ms. The DS
  runs at the panel's 59.94 Hz; the sound is resampled +0.19% (3 cents). A frame that flips late
  has the next one left out, so SDL's swap never leaves the pipeline a frame deeper.
- `emu.input_at = vblank`: the pad is read again at line 192, just before the game's VBlank
  handler, ~12 ms fresher than at frame start (a frame ends at line 0).
- `emu.frame_end = vblank` (off): frames end at line 192 -- a press shows a frame sooner (Golden
  Sun 107 -> 80 ms median in a 20-press set), but the 2D worker's last lines no longer overlap
  VBlank: +3.6 ms of emulation a frame in Golden Sun (10.6 -> 14.2), which the lock then
  cannot fit in a period.
- `rootfs/dslot/dsperate.ini`: `vsync_pipe = 1`, the top panel.

| 65 s, no input, 1x | repeated refreshes a minute (top / bottom) | frames shown a second |
|---|---|---|
| NSMB, packaged | 7 / 6 (every ~9 s) | 59.84 |
| NSMB, lock + input at VBlank | **0 / 0** (one run; 1 / 1 in another) | 59.95 |
| Golden Sun, packaged | 9 / 9 | 59.81 |
| Golden Sun, lock + input at VBlank | 4-6 / 4-6 | 59.86-59.89 |

Golden Sun's remaining repeats are frames whose emulation alone overruns a refresh (its p99 is
over 16.7 ms; README above) or whose present stalls ~10 ms; they also repeat as packaged.
Keeping the present real-time (`DS_PRESENT_RT=1`) made it worse (27).

| 30 presses, median (p95) | packaged | lock + input at VBlank |
|---|---|---|
| NSMB (2x) | 79.8 (93.6) | **69.6 (77.5)** |
| Golden Sun (1x) | 102.1 (113.7) | 99.7 (123.9) |
| Advance Wars (GBA) | 60.5 (97.8) | 71.1 (113.4): worse |

**Not done / why:**
- GBA mode was slower with the lock (its present waited ~8 ms for a flip). Not the panel: the
  lock follows the right one (pipe 1, the top). The lock's budget counts each frame's wait for
  the previous flip as present time, so it sits at its cap (a period + 2 ms) in every game:
  input is taken ~1.8 ms before a vblank and the frame flips a refresh later. DS games at 2x
  are GPU-bound enough not to show it; GBA frames (~8 ms) lose a refresh. Fixed for now by
  leaving the lock off in GBA mode (`vsync_lock = auto`), re-measured 2026-10-18 on Advance
  Wars, 20 presses, our part (wait + present + scanout): lock off 25.0 ms median, auto 26.9,
  forced on 35.0. The real fix is a budget from the present's own work (the GPU's done stamp,
  not the swap's return); then GBA mode can have the lock too.
- Golden Sun's latency is unchanged within noise. Each latency run lasts ~3.5 s, so the press
  lands while the adaptive budget is still at its start-up value (a period + 2 ms); the
  pacing runs show the settled lock. A longer run per press would measure it fairly.
- The two panels' vblanks are 8.2 ms apart (two VOP2 video ports started separately), so the
  panel the lock does not follow gets each frame ~8 ms later. Starting both ports together in
  the kernel would give both the shorter path.
- Not checked with a phone camera at 240 fps; the gpio-keys debounce and the panel's own
  response are outside these numbers.
