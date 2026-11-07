// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "frontend/sdl/level_indicator.h"

#include <SDL2/SDL.h>

#include "frontend/sdl/rt_budget.h"

#if DSPERATE_NIBKIT_MENU
#include <nibkit_menu.h>
#endif

namespace ds::sdl {

#if DSPERATE_NIBKIT_MENU && defined(NIBKIT_MENU_VERSION) && NIBKIT_MENU_VERSION >= 203

struct LevelIndicator::Nib {
  NibLevels* lv = nib_levels_new();
  ~Nib() { nib_levels_free(lv); }
};

LevelIndicator::LevelIndicator() : nib_(std::make_unique<Nib>()) {}
LevelIndicator::~LevelIndicator() = default;

void LevelIndicator::tick() {
  // A library older than the header has no indicator: nothing to show.
  if (nib_menu_version() < 203) return;
  serial_ = nib_levels_tick(nib_->lv, SDL_GetTicks64());
}

void LevelIndicator::hide() {
  if (nib_menu_version() < 203) return;
  nib_levels_hide(nib_->lv);
  serial_ = 0;
}

bool LevelIndicator::picture(int w, int h) {
  if (!on()) return false;
  if (pic_.serial == serial_ && pic_.cw == w && pic_.ch == h) return pic_.ok;
  pic_.serial = serial_;
  pic_.cw = w;
  pic_.ch = h;
  pic_.ok = false;
  if (nib_menu_scale_for(w, h) < 0.5f) return false;
  // The number is rasterised when the level changes (under a millisecond); a step
  // of the fade is a copy. Either way as an ordinary task.
  const OffRealtime off_rt;
  int pw = 0, ph = 0, px = 0, py = 0;
  const u8* rgba = nib_levels_render(nib_->lv, w, h, &pw, &ph, &px, &py);
  if (!rgba || pw <= 0 || ph <= 0 || pw > w || ph > h) return false;
  pic_.set(rgba, pw, ph, px, py);
  return true;
}

int LevelIndicator::lift(int w, int h) const {
  return on() ? nib_levels_lift(nib_->lv, w, h) : 0;
}

#else

struct LevelIndicator::Nib {};
LevelIndicator::LevelIndicator() = default;
LevelIndicator::~LevelIndicator() = default;
void LevelIndicator::tick() {}
void LevelIndicator::hide() {}
bool LevelIndicator::picture(int, int) { return false; }
int LevelIndicator::lift(int, int) const { return 0; }

#endif

} // namespace ds::sdl
