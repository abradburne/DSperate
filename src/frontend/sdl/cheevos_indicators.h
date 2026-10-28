// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
// RetroAchievements' indicators, kept on screen while they last rather than
// queued as toasts: the progress of the achievement being measured (one at a
// time, updated in place, until rcheevos takes it down) above the badges of the
// challenges under way (small, rounded, a little see-through; drawn once each badge
// has come). Bottom left, opposite the toasts. Drawn in DSlot's style
// where DSperate is built with nibkit; not drawn without it (the log has them).
#include "core/types.h"
#include "frontend/sdl/toast_queue.h"

#include <memory>
#include <string>
#include <vector>

namespace ds::sdl {

class CheevosIndicators {
public:
  CheevosIndicators();
  ~CheevosIndicators();
  CheevosIndicators(const CheevosIndicators&) = delete;
  CheevosIndicators& operator=(const CheevosIndicators&) = delete;

  void progress(u32 id, const std::string& title, const std::string& measured, const std::string& badge);
  void hide_progress();
  void challenge(u32 id, const std::string& badge);
  void end_challenge(u32 id);
  void clear();
  // A badge came (ToastQueue::badge): redrawn if one shown waits for it.
  void badge_arrived(const std::string& name);

  bool on() const { return progress_.id != 0 || !challenges_.empty(); }
  // Changes whenever what is shown does.
  u64 serial() const { return serial_; }

  // The indicators drawn for a `w` x `h` target with the badges `badges` has, once
  // per change and size, kept after. False when there are none, no nibkit, or the
  // target is under half the screen's scale.
  bool picture(int w, int h, const ToastQueue& badges);
  const ToastPicture& pic() const { return pic_; }
  Rect draw(const Canvas& c, const ToastQueue& badges) { return picture(c.w, c.h, badges) ? pic_.blit(c) : Rect{}; }

private:
  struct Progress { u32 id = 0; std::string title, measured, badge; };
  struct Challenge { u32 id = 0; std::string badge; };
  Progress progress_;
  std::vector<Challenge> challenges_;
  u64 serial_ = 1;
  ToastPicture pic_;
  struct Nib;
  std::unique_ptr<Nib> nib_;
};

} // namespace ds::sdl
