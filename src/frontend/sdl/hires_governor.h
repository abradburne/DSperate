// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
// video.hires3d_fallback: hi-res 3D drops to 1x while the GPU cannot keep up.
// The GLES present leaves a frame out when the GPU is still drawing the one
// before (main.cpp); a game whose 3D at S x takes longer than a refresh then
// shows 40-50 pictures a second, which judders more than the 1x picture
// does. Fed once a frame with whether that frame was left out: when 15% of
// the last two seconds' frames were, the 3D goes to 1x; 2x is tried again
// after a wait that starts at 10 s and doubles (to 80 s) each time it drops
// again within 3 s of coming back, and resets once 2x has held for 30 s. So
// a scene that is too heavy settles at 1x, a light one comes back by itself,
// and neither flickers between the two.
#include "core/types.h"

namespace ds::sdl {

class HiresGovernor {
public:
  void set_enabled(bool on) { enabled_ = on; }
  bool hires() const { return hires_; }
  // One emulated frame: `late`, it was left out with the GPU still busy.
  // Returns true when this frame switched resolution (for the log).
  bool frame(bool late) {
    if (!enabled_) return false;
    ++frame_;
    if (hires_) {
      bool& slot = hist_[frame_ % WINDOW];   // the frame two seconds ago gives way to this one
      late_ = late_ - slot + late;
      slot = late;
      if (frame_ - since_ >= WINDOW && late_ * 100 >= WINDOW * DROP_PCT) {
        if (frame_ - since_ < PROBATION) backoff_ = backoff_ * 2 > MAX_WAIT ? MAX_WAIT : backoff_ * 2;   // dropped again soon after coming back
        hires_ = false; since_ = frame_; ++drops_;
        return true;
      }
      if (frame_ - since_ >= SETTLED) backoff_ = FIRST_WAIT;
      return false;
    }
    if (frame_ - since_ >= backoff_) { hires_ = true; since_ = frame_; clear(); return true; }
    return false;
  }
  u32 drops() const { return drops_; }
  u32 wait_frames() const { return backoff_; }

private:
  static constexpr u32 WINDOW = 120, DROP_PCT = 15;                      // two seconds, 15% left out
  static constexpr u32 FIRST_WAIT = 600, MAX_WAIT = 4800;                // 10 s, doubling to 80 s
  static constexpr u32 PROBATION = 180, SETTLED = 1800;                  // 3 s; 30 s
  void clear() { for (bool& b : hist_) b = false; late_ = 0; }
  bool enabled_ = true, hires_ = true;
  bool hist_[WINDOW] = {};
  u32 late_ = 0;
  u64 frame_ = 0, since_ = 0;
  u32 backoff_ = FIRST_WAIT, drops_ = 0;
};

} // namespace ds::sdl
