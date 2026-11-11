// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "frontend/sdl/battery_indicator.h"

#include "frontend/sdl/rt_budget.h"

#if DSPERATE_NIBKIT_MENU
#include <nibkit_menu.h>
#endif

namespace ds::sdl {

#if DSPERATE_NIBKIT_MENU && defined(NIBKIT_MENU_VERSION) && NIBKIT_MENU_VERSION >= 400

struct BatteryIndicator::Nib {
  // A library older than the header has no nib_battery_*: never made, never asked.
  NibBattery* b = nib_menu_version() >= 400 ? nib_battery_new() : nullptr;
  ~Nib() {
    if (b) nib_battery_free(b);
  }
};

BatteryIndicator::BatteryIndicator() : nib_(std::make_unique<Nib>()) {}
BatteryIndicator::~BatteryIndicator() = default;

void BatteryIndicator::tick() {
  if (!nib_->b) return;
  serial_ = nib_battery_tick(nib_->b);
}

bool BatteryIndicator::news(std::string& title, std::string& detail) {
  const char* t = nullptr;
  const char* d = nullptr;
  if (!nib_->b || !nib_battery_news(nib_->b, &t, &d)) return false;
  title = t ? t : "";
  detail = d ? d : "";
  return true;
}

bool BatteryIndicator::picture(int w, int h) {
  if (!on()) return false;
  if (pic_.serial == serial_ && pic_.cw == w && pic_.ch == h) return pic_.ok;
  pic_.serial = serial_;
  pic_.cw = w;
  pic_.ch = h;
  pic_.ok = false;
  if (nib_menu_scale_for(w, h) < 0.5f) return false;
  // Drawn when the percent changes (a few hundred pixels of text), as an ordinary task.
  const OffRealtime off_rt;
  int pw = 0, ph = 0, px = 0, py = 0;
  const u8* rgba = nib_battery_render(nib_->b, w, h, &pw, &ph, &px, &py);
  if (!rgba || pw <= 0 || ph <= 0 || pw > w || ph > h) return false;
  pic_.set(rgba, pw, ph, px, py);
  return true;
}

#else

struct BatteryIndicator::Nib {};
BatteryIndicator::BatteryIndicator() = default;
BatteryIndicator::~BatteryIndicator() = default;
void BatteryIndicator::tick() {}
bool BatteryIndicator::news(std::string&, std::string&) { return false; }
bool BatteryIndicator::picture(int, int) { return false; }

#endif

} // namespace ds::sdl
