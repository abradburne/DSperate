// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
// DS_DEBUG: the switches for measuring and checking the GLES present and the
// hi-res 3D, in one environment variable read once, at first use:
//
//   DS_DEBUG=present-wait,present-hash=/tmp/a.txt dsperate game.nds
//
// a comma-separated list of key or key=value. DS_DEBUG=help prints the keys
// (kKeys in debug_options.cpp) and exits. Unknown keys are reported and
// ignored. The rest of the emulator's debug switches are DS_* variables of
// their own (DS_VERBOSE, DS_FRAME_STATS, ...); these are the ones the hi-res
// path and the tools around it (bench/render/*.sh) use.
#include "core/types.h"

#include <string>
#include <vector>

namespace ds::sdl::debug {

struct Options {
  // marks: launch stages on stderr, stamped with CLOCK_BOOTTIME (DSlot logs
  // its own against the same clock).
  bool marks = false;
  // present-wait: every frame shown; the GLES present waits for the GPU
  // rather than leaving a frame out while it still draws the one before.
  bool present_wait = false;
  // present-hash=FILE: a line per window present -- window, present count,
  // an FNV-1a hash of its picture and, for a window filled by one 1x screen,
  // the hash that picture must have (its nearest scaling).
  std::string present_hash;
  // present-ppm=N[+COUNT]:PREFIX: what each window shows at presents
  // N..N+COUNT-1, as PREFIX.<window>.<present>.ppm.
  u32 present_ppm_from = 0, present_ppm_count = 0;
  std::string present_ppm_prefix;
  // menu-ppm=PREFIX: the shared pause menu's two pictures each time they are
  // drawn, as PREFIX.<n>.top.ppm and PREFIX.<n>.bottom.ppm.
  std::string menu_ppm;
  // hires-stats: every 120 hi-res renders, the present thread's CPU time per
  // stage; every 300 frames, how many were drawn at once, captured, or
  // shown from a capture.
  bool hires_stats = false;
  // hires-half: hi-res 3D at half rate from the start and throughout (every
  // other frame shown; HiresGovernor), to check it against every frame.
  bool hires_half = false;
  // hires-switch=FRAME:SCALE[+FRAME:SCALE...]: the 3D resolution changed at
  // those frames as the pause menu changes it (no menu shown).
  std::vector<std::pair<u64, std::string>> hires_switch;
  // hires-stages: GPU time per stage (timer queries), every 120 renders.
  // They change Panfrost's batching, so they rank the stages only.
  bool hires_stages = false;
  // hires-bench=N[:MASK/MASK/...]: at render N, the frame drawn 60 times per
  // mask, glFinish'd, the time a render printed. A mask leaves stages out
  // (GlHires::Stage bits); none given, nothing is left out.
  u32 hires_bench_at = 0;
  std::vector<u32> hires_bench_masks;
  // latency=FILE: a line per frame from a button's press to the flip that
  // shows it (latency_log.h; pocketOS's emu/latency analyses it).
  std::string latency;
  // vsync-commit-ms=MS: the vsync lock's commit time instead of its own
  // estimate (vsync_lock.h).
  double vsync_commit_ms = 0;
};

// The options, parsed from DS_DEBUG on the first call.
const Options& options();

// With `marks`: "dsperate: <seconds since boot> s <stage>" on stderr. The
// stage is kept (with or without `marks`) for the crash line.
void mark(const char* stage);
// The last stage marked, for a signal handler: a static buffer, never freed.
const char* last_mark();

} // namespace ds::sdl::debug
