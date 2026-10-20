# bench: measuring DSperate on a handheld

The tools behind the measurements in `docs/rk3568/`: emulation cost and exactness, the render
and present path, button-to-screen latency and frame pacing, battery draw, launch time. They
were written for the Anbernic RG DS+ (RK3568, nibOS — formerly pocketOS — with the DSlot
launcher) and assume a Linux handheld reached over SSH; nothing here is part of the build.

Every script starts with a header saying where it runs (on the handheld, or on a computer
that drives it), how to call it, and what it prints. `-h` prints it.

## Settings

Scripts that run on a computer read `bench/config.sh` through `bench/lib.sh`: the handheld's
SSH target (`DEVICE`) and key, the build machine (`BUILD_HOST`, an aarch64 machine with the
package's compiler, or `local`), the PGO training directory, the GBA ROM folder. Put your
values in `bench/config.local.sh` (git-ignored; `config.local.sh.example` shows the format) or
in the environment for one run. Scripts that run on the handheld take `ROMS` (default
`/mnt/ds/Roms`, holding `NDS/` and `GBA/`) and `STATES` from the environment.

The game scenes name the commercial games they were measured with; the ROMs, BIOS and save
states are yours to provide and are never part of this repository.

## What is here

| path | runs on | what |
|---|---|---|
| `core/bench.sh` | handheld | emulation cost per frame (headless, unpaced) on fixed scenes, with frame and audio hashes: the exactness check for a speed-up |
| `core/prof.sh`, `core/cats.py` | handheld, computer | a perf profile of a scene (JIT blocks named through `DS_PERF_MAP`); the main thread's samples by area |
| `core/gba-exact.sh` | computer | two builds side by side on every GBA ROM: frames, audio, time |
| `core/pgo-train.sh` | computer | the package's PGO profile, trained on the handheld, into `pgo/aarch64-gcc<version>/` |
| `core/gba-lazy-audio.patch` | — | a measured and rejected idea, kept for reference (`docs/rk3568/core-review.md`) |
| `render/render-bench.sh` | handheld | render and present cost of one game on the panels: frame times, frames not shown, GPU and CPU per stage (`DS_DEBUG=hires-stats`, `hires-stages`) |
| `render/render-same.sh` | handheld | whether two builds put exactly the same pictures on the panels, every present (`DS_DEBUG=present-wait,present-hash=`), replayed |
| `render/power-run.sh` | handheld | battery charge, current and power over a run, CPU and GPU frequency residency |
| `render/audio-bench.sh`, `bench-realtime.sh`, `run-bench.sh` | handheld | sound break-ups; real time on the panels, the packaged build with sound and any build without |
| `latency/` | handheld + computer | button-to-screen latency (`latency.sh`, `matrix.sh`, `lat-press.c`, `analyse.py`, `summary.py`) and frame pacing (`pacing-run.sh`, `pacing.py`), from `DS_DEBUG=latency=` logs |
| `launch/` | handheld | launch timing from the launcher's shelf (`launch-timing.sh`, `press.py`); games started under the launcher's real-time limits, failing on any the kernel kills (`rt-limits.sh`) |
| `tools/device-run.sh` | handheld | runs a measurement with the launcher stopped, under the shared device lock (`/tmp/nibos-device.lock`; `LOCK_HELD=1` when the caller took it), refusing (or waiting, `WAIT=1`) while it is held, a game runs, or the battery is low |
| `tools/vblank.c` | handheld | the panels' real refresh rate |
| `gba-oracle/` | macOS | mGBA as a reference for GBA mode, frame by frame (not linked into DSperate) |

`lat-press` writes a button into the GPIO keys device that nibOS's input daemon (pocket-input)
reads, so the press goes through the same path as a real one; elsewhere, adapt its device names.
