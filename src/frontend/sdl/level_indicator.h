// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
// The volume and brightness indicator: nibOS changes the volume (the volume
// keys) and both screens' brightness (Menu + volume) from anywhere and
// announces each step; this shows the new level for a moment over the game, at
// the bottom right, then fades it away. It is nibkit's (nib_levels_*, the same
// over every nibOS app). Not drawn without nibkit, or where nibOS makes no
// announcements.
//
// The clock is the wall clock, not the frames played: a press shows and fades
// at its own pace whatever the game does. The picture changes when the level
// does and at each step of the fade, and serial() with it.
#include "core/types.h"
#include "frontend/sdl/toast_queue.h"

#include <memory>

namespace ds::sdl {

class LevelIndicator {
public:
  LevelIndicator();
  ~LevelIndicator();
  LevelIndicator(const LevelIndicator&) = delete;
  LevelIndicator& operator=(const LevelIndicator&) = delete;

  // Once per frame shown: takes in what nibOS announced.
  void tick();
  bool on() const { return serial_ != 0; }
  // Changes whenever the picture does; 0 for nothing.
  u64 serial() const { return serial_; }
  // Gone at once (a menu opens over the game).
  void hide();

  // The indicator drawn for a `w` x `h` target, its opacity in its alpha, once
  // per change and size (as an ordinary task). False when it isn't on, no
  // nibkit, or the target is under half the screen's scale.
  bool picture(int w, int h);
  const ToastPicture& pic() const { return pic_; }
  Rect draw(const Canvas& c) { return picture(c.w, c.h) ? pic_.blit(c) : Rect{}; }
  // How far up a toast in the same corner goes while it shows, in a `w` x `h`
  // target's pixels; 0 when it doesn't.
  int lift(int w, int h) const;

private:
  u64 serial_ = 0;
  ToastPicture pic_;
  struct Nib;
  std::unique_ptr<Nib> nib_;
};

} // namespace ds::sdl
