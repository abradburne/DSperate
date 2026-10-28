// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
// RetroAchievements' indicators, kept on screen while they last rather than
// queued as toasts. They are nibkit's (nib_indicators_*, the same in every
// nibOS emulator): the progress of the achievement being measured (one at a
// time, updated in place, until rcheevos takes it down) as a panel headed
// "Progress", and each challenge under way as an icon in a row under it: its
// badge alone, 60% opaque, in a gold ring that pulses slowly; a challenge for
// the achievement whose progress is shown is shown in that panel. Bottom left,
// opposite the toasts. Not drawn without nibkit (the log has them).
//
// The panels are drawn once each time what they say changes; while a ring
// pulses, only the ring is painted again, 16 times a second, and serial()
// changes with it. The clock is the frames played, as the toasts' is.
#include "core/types.h"
#include "frontend/sdl/toast_queue.h"

#include <memory>
#include <string>

namespace ds::sdl {

class CheevosIndicators {
public:
  CheevosIndicators();
  ~CheevosIndicators();
  CheevosIndicators(const CheevosIndicators&) = delete;
  CheevosIndicators& operator=(const CheevosIndicators&) = delete;

  // What rcheevos says. A badge `badges` already has goes with it; one it hasn't
  // is waited for (badge_arrived) a moment before the panel shows without it.
  void progress(u32 id, const std::string& title, const std::string& measured, const std::string& badge,
                const ToastQueue& badges);
  void hide_progress();
  void challenge(u32 id, const std::string& title, const std::string& badge, const ToastQueue& badges);
  void end_challenge(u32 id);
  void clear();
  // A badge came (ToastQueue::badge, which keeps it): to the panels waiting for it.
  void badge_arrived(const std::string& name, const ToastQueue& badges);

  // Once per emulated frame.
  void tick();
  bool on() const { return serial_ != 0; }
  // Changes whenever the picture does (the ring's steps too); 0 for nothing.
  u64 serial() const { return serial_; }

  // The indicators drawn for a `w` x `h` target, once per change and size, kept
  // after (as an ordinary task: text may be rasterised). False when there are
  // none, no nibkit, or the target is under half the screen's scale.
  bool picture(int w, int h);
  const ToastPicture& pic() const { return pic_; }
  Rect draw(const Canvas& c) { return picture(c.w, c.h) ? pic_.blit(c) : Rect{}; }

private:
  [[maybe_unused]] u64 frames_ = 0;   // the clock: frames played (unused without nibkit)
  u64 serial_ = 0;
  ToastPicture pic_;
  struct Nib;
  std::unique_ptr<Nib> nib_;
};

} // namespace ds::sdl
