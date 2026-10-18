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
// path and the tools around it (pocketOS's emu/render-*.sh) use.
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
  // hires-stats: every 120 hi-res renders, the present thread's CPU time per
  // stage; every 300 frames, how many were drawn at once, captured, or
  // shown from a capture.
  bool hires_stats = false;
  // hires-stages: GPU time per stage (timer queries), every 120 renders.
  // They change Panfrost's batching, so they rank the stages only.
  bool hires_stages = false;
  // hires-bench=N[:MASK/MASK/...]: at render N, the frame drawn 60 times per
  // mask, glFinish'd, the time a render printed. A mask leaves stages out
  // (GlHires::Stage bits); none given, nothing is left out.
  u32 hires_bench_at = 0;
  std::vector<u32> hires_bench_masks;
};

// The options, parsed from DS_DEBUG on the first call.
const Options& options();

// With `marks`: "dsperate: <seconds since boot> s <stage>" on stderr.
void mark(const char* stage);

} // namespace ds::sdl::debug
