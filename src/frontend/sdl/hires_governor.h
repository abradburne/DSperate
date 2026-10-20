// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
// What hi-res 3D does while the GPU cannot keep up. The GLES present leaves a
// frame out when the GPU is still drawing the one before
// (gl_frame_presenter.h); a game whose 3D at S x takes longer than a refresh
// then shows 40-50 uneven pictures a second. Three modes, fed once a frame:
//
//   Full  every frame at S x. Frames left out here and there -- 12 runs of
//         them in the last two seconds, a GPU 10% short -- go to Half when
//         adaptive frameskip is on (emu.frameskip >= 1); without it, 42 runs
//         (35%) go to Low. Runs, not frames: a stall (loading a scene, a
//         shader's first use) leaves out a dozen frames in one run.
//   Half  S x, every other frame shown, evenly: 30 pictures a second while
//         emulation and sound run at full speed (the frames between are still
//         emulated, and a display capture among them still drawn at S x for
//         the next shown frame). Back to Full once the shown frames' GPU work
//         has fitted in one frame for two seconds, tried no sooner than 5 s
//         after Half began, a wait that doubles to 60 s each time Full falls
//         back within 3 s; 30 s held at Full starts it over. Half not holding
//         either (video.hires3d_fallback): a quarter of the last 120 shown
//         frames (four seconds) left out or their work running past the next
//         shown frame -- one stall of a second, loading a scene, is not enough:
//   Low   the 3D at 1x, until the next launch: no going back and forth.
//
// Which frames are shown is all that changes: the emulated frames and the 1x
// screens are the same in every mode.
#include "core/types.h"

namespace ds::sdl {

class HiresGovernor {
public:
  enum class Mode : u8 { Full, Half, Low };
  void set_fallback(bool on) { fallback_ = on; }   // video.hires3d_fallback: Low allowed
  // Adaptive frameskip on: Half allowed. Off while at Half: back to Full.
  void set_half(bool on) {
    half_ok_ = on;
    if (!on && mode_ == Mode::Half && !pinned_) to_full();
  }
  // Half from now on, whatever comes (DS_DEBUG=hires-half).
  void pin_half() { mode_ = Mode::Half; pinned_ = true; }
  Mode mode() const { return mode_; }
  bool hires() const { return mode_ != Mode::Low; }
  // One emulated frame. Full: `late`, it was left out with the GPU still
  // busy (a run of them counts once). Half: a shown frame (`late`: left out all the same, or the shown
  // frame before's work ran past it) or one between (`late`: the shown
  // frame's work still running a frame on). True when the mode changed (for
  // the log).
  bool frame(bool late, bool shown = true) {
    ++frame_;
    if (pinned_) return false;
    switch (mode_) {
    case Mode::Low: return false;
    case Mode::Full: {
      if (!half_ok_ && !fallback_) return false;
      const bool run = late && !prev_late_;
      prev_late_ = late;
      late_ = late_ - full_[frame_ % WINDOW] + run;
      full_[frame_ % WINDOW] = run;
      if (frame_ - since_ >= SETTLED) wait_ = FIRST_WAIT;
      if (frame_ - since_ < WINDOW) return false;   // two seconds at Full before judging
      if (half_ok_ && late_ >= HALF_RUNS) {
        if (retried_ && frame_ - since_ < PROBATION) wait_ = wait_ * 2 > MAX_WAIT ? MAX_WAIT : wait_ * 2;
        mode_ = Mode::Half; since_ = frame_; clear(); ++halves_;
        return true;
      }
      if (!half_ok_ && fallback_ && late_ >= LOW_RUNS) { mode_ = Mode::Low; return true; }
      return false;
    }
    case Mode::Half:
      if (shown) {
        miss_ = miss_ - shown_[shown_n_ % LOW_WINDOW] + late;
        shown_[shown_n_++ % LOW_WINDOW] = late;
        if (fallback_ && shown_n_ >= LOW_WINDOW && miss_ * 100 >= LOW_WINDOW * MISS_PCT) { mode_ = Mode::Low; return true; }
      } else {
        slow_ = slow_ - between_[between_n_ % HALF_WINDOW] + late;
        between_[between_n_++ % HALF_WINDOW] = late;
        if (frame_ - since_ >= wait_ && between_n_ >= HALF_WINDOW && slow_ * 100 <= HALF_WINDOW * FIT_SLOW_PCT) { to_full(); return true; }
      }
      return false;
    }
    return false;
  }
  u32 halves() const { return halves_; }       // times Full went to Half
  // The windows as they stand (hires-stats): Full's runs left out; Half's
  // shown frames not in time and frames between whose shown frame ran over.
  u32 window_late() const { return late_; }
  u32 window_miss() const { return miss_; }
  u32 window_slow() const { return slow_; }
  u32 wait_frames() const { return wait_; }    // at Half, before Full is tried again

private:
  static constexpr u32 WINDOW = 120, HALF_WINDOW = 60;         // two seconds of frames; of frames between at Half
  static constexpr u32 LOW_WINDOW = 120;                       // shown frames at Half: four seconds
  static constexpr u32 HALF_RUNS = 12, LOW_RUNS = 42;          // Full: runs left out, to Half; to Low without frameskip
  static constexpr u32 MISS_PCT = 25;                          // Half: shown frames not in time, to Low
  static constexpr u32 FIT_SLOW_PCT = 3;                       // Half: at most this many work-ran-over, back to Full
  static constexpr u32 FIRST_WAIT = 300, MAX_WAIT = 3600;      // 5 s, doubling to 60 s
  static constexpr u32 PROBATION = 180, SETTLED = 1800;        // 3 s; 30 s
  void to_full() { mode_ = Mode::Full; retried_ = true; since_ = frame_; clear(); }
  void clear() {
    for (bool& b : full_) b = false;
    for (bool& b : shown_) b = false;
    for (bool& b : between_) b = false;
    late_ = miss_ = slow_ = 0; shown_n_ = between_n_ = 0; prev_late_ = false;
  }
  Mode mode_ = Mode::Full;
  bool fallback_ = true, half_ok_ = false, retried_ = false, pinned_ = false, prev_late_ = false;   // retried_: Full again after a Half
  bool full_[WINDOW] = {}, shown_[LOW_WINDOW] = {}, between_[HALF_WINDOW] = {};
  u32 late_ = 0, miss_ = 0, slow_ = 0, shown_n_ = 0, between_n_ = 0;
  u64 frame_ = 0, since_ = 0;
  u32 wait_ = FIRST_WAIT, halves_ = 0;
};

} // namespace ds::sdl
