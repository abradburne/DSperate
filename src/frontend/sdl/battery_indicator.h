// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
// The low battery over a game: nibOS warns at 10% and 5% and powers off
// cleanly near empty (DSperate gets SIGTERM and writes its resume state).
// Each time the battery gets worse there is news for a toast; from 5% on, a
// small indicator stays at the top right (DSlot's battery icon in red, the
// percent left) till the charger goes in or the device powers off. It is
// nibkit's (nib_battery_*, the same over every nibOS app). Not drawn without
// nibkit, or where nibOS says nothing (off the device).
#include "core/types.h"
#include "frontend/sdl/toast_queue.h"

#include <memory>
#include <string>

namespace ds::sdl {

class BatteryIndicator {
public:
  BatteryIndicator();
  ~BatteryIndicator();
  BatteryIndicator(const BatteryIndicator&) = delete;
  BatteryIndicator& operator=(const BatteryIndicator&) = delete;

  // Once per frame shown: takes in what nibOS said (the file is read only when
  // it changes).
  void tick();
  bool on() const { return serial_ != 0; }
  // Changes whenever the picture does; 0 for nothing.
  u64 serial() const { return serial_; }
  // True once each time the battery gets worse, with a toast's words.
  bool news(std::string& title, std::string& detail);

  // The indicator drawn for a `w` x `h` target, once per change and size (as an
  // ordinary task). False when it isn't on, no nibkit, or the target is under
  // half the screen's scale.
  bool picture(int w, int h);
  const ToastPicture& pic() const { return pic_; }
  Rect draw(const Canvas& c) { return picture(c.w, c.h) ? pic_.blit(c) : Rect{}; }

private:
  u64 serial_ = 0;
  ToastPicture pic_;
  struct Nib;
  std::unique_ptr<Nib> nib_;
};

} // namespace ds::sdl
