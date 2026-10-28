// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "frontend/sdl/cheevos_indicators.h"

#include "frontend/sdl/rt_budget.h"

#if DSPERATE_NIBKIT_MENU
#include <nibkit_menu.h>
#endif

namespace ds::sdl {

#if DSPERATE_NIBKIT_MENU

struct CheevosIndicators::Nib {
  NibIndicators* up = nib_indicators_new();
  NibToast* t = nib_toast_new();
  ~Nib() { nib_toast_free(t); nib_indicators_free(up); }
  // The badge `name` from `badges` on the toast being filled, or waited for.
  void badge(const std::string& name, const ToastQueue& badges) {
    if (name.empty()) return;
    const ToastImage* img = badges.badge_image(name);
    if (const u8* rgba = badges.badge_rgba(name); rgba && img) nib_toast_badge(t, rgba, img->w, img->h);
    else nib_toast_await_badge(t, name.c_str());
  }
};
CheevosIndicators::CheevosIndicators() : nib_(std::make_unique<Nib>()) {}
CheevosIndicators::~CheevosIndicators() = default;

void CheevosIndicators::progress(u32 id, const std::string& title, const std::string& measured, const std::string& badge,
                                 const ToastQueue& badges) {
  nib_toast_progress(nib_->t, title.c_str(), measured.c_str());
  nib_->badge(badge, badges);
  nib_indicators_progress(nib_->up, id, nib_->t);
}

void CheevosIndicators::hide_progress() { nib_indicators_hide_progress(nib_->up); }

void CheevosIndicators::challenge(u32 id, const std::string& title, const std::string& badge, const ToastQueue& badges) {
  nib_toast_challenge(nib_->t, title.c_str());
  nib_->badge(badge, badges);
  nib_indicators_challenge(nib_->up, id, nib_->t);
}

void CheevosIndicators::end_challenge(u32 id) { nib_indicators_end_challenge(nib_->up, id); }

void CheevosIndicators::clear() { nib_indicators_clear(nib_->up); }

void CheevosIndicators::badge_arrived(const std::string& name, const ToastQueue& badges) {
  const ToastImage* img = badges.badge_image(name);
  if (const u8* rgba = badges.badge_rgba(name); rgba && img) nib_indicators_badge(nib_->up, name.c_str(), rgba, img->w, img->h);
}

void CheevosIndicators::tick() {
  ++frames_;
  serial_ = nib_indicators_tick(nib_->up, frames_ * 1000 / 60);
}

bool CheevosIndicators::picture(int w, int h) {
  if (!on()) return false;
  if (pic_.serial == serial_ && pic_.cw == w && pic_.ch == h) return pic_.ok;
  pic_.serial = serial_;
  pic_.cw = w;
  pic_.ch = h;
  pic_.ok = false;
  if (nib_menu_scale_for(w, h) < 0.5f) return false;
  // Text is rasterised when what the panels say changed (a few milliseconds); a
  // step of the ring alone is a fraction of one. Either way as an ordinary task.
  const OffRealtime off_rt;
  int pw = 0, ph = 0, px = 0, py = 0;
  const u8* stack = nib_indicators_render(nib_->up, w, h, &pw, &ph, &px, &py);
  if (!stack || pw <= 0 || ph <= 0) return false;
  pic_.set(stack, pw, ph, px, py);
  return true;
}

#else

struct CheevosIndicators::Nib {};
CheevosIndicators::CheevosIndicators() = default;
CheevosIndicators::~CheevosIndicators() = default;
void CheevosIndicators::progress(u32, const std::string&, const std::string&, const std::string&, const ToastQueue&) {}
void CheevosIndicators::hide_progress() {}
void CheevosIndicators::challenge(u32, const std::string&, const std::string&, const ToastQueue&) {}
void CheevosIndicators::end_challenge(u32) {}
void CheevosIndicators::clear() {}
void CheevosIndicators::badge_arrived(const std::string&, const ToastQueue&) {}
void CheevosIndicators::tick() {}
bool CheevosIndicators::picture(int, int) { return false; }

#endif

} // namespace ds::sdl
