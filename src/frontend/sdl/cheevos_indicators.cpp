// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "frontend/sdl/cheevos_indicators.h"

#include <algorithm>
#include <cmath>

#include "frontend/sdl/rt_budget.h"

#if DSPERATE_NIBKIT_MENU
#include <nibkit_menu.h>
#endif

namespace ds::sdl {

#if DSPERATE_NIBKIT_MENU
struct CheevosIndicators::Nib {
  NibToast* t = nib_toast_new();
  ~Nib() { nib_toast_free(t); }
};
CheevosIndicators::CheevosIndicators() : nib_(std::make_unique<Nib>()) {}
#else
struct CheevosIndicators::Nib {};
CheevosIndicators::CheevosIndicators() = default;
#endif
CheevosIndicators::~CheevosIndicators() = default;

void CheevosIndicators::progress(u32 id, const std::string& title, const std::string& measured, const std::string& badge) {
  progress_ = Progress{id, title, measured, badge};
  ++serial_;
}

void CheevosIndicators::hide_progress() {
  if (!progress_.id) return;
  progress_ = {};
  ++serial_;
}

void CheevosIndicators::challenge(u32 id, const std::string& badge) {
  // One per achievement, newest last.
  end_challenge(id);
  challenges_.push_back(Challenge{id, badge});
  ++serial_;
}

void CheevosIndicators::end_challenge(u32 id) {
  const size_t before = challenges_.size();
  challenges_.erase(std::remove_if(challenges_.begin(), challenges_.end(), [&](const Challenge& c) { return c.id == id; }),
                    challenges_.end());
  if (challenges_.size() != before) ++serial_;
}

void CheevosIndicators::clear() {
  if (!on()) return;
  progress_ = {};
  challenges_.clear();
  ++serial_;
}

void CheevosIndicators::badge_arrived(const std::string& name) {
  bool used = progress_.id && progress_.badge == name;
  for (const Challenge& c : challenges_) used = used || c.badge == name;
  if (used) ++serial_;
}

#if DSPERATE_NIBKIT_MENU

bool CheevosIndicators::picture(int w, int h, const ToastQueue& badges) {
  if (!on()) return false;
  if (pic_.serial == serial_ && pic_.cw == w && pic_.ch == h) return pic_.ok;
  pic_.serial = serial_;
  pic_.cw = w;
  pic_.ch = h;
  pic_.ok = false;
  const float scale = nib_menu_scale_for(w, h);
  if (scale < 0.5f) return false;
  const OffRealtime off_rt;   // text rasterised: a few milliseconds, as an ordinary task

  // The progress panel: nibkit's toast, as every app draws one.
  int pw = 0, ph = 0, px = 0, py = 0;
  const u8* panel = nullptr;
  if (progress_.id) {
    nib_toast_begin(nib_->t, NIB_TOAST_INFO, progress_.title.c_str());
    nib_toast_header(nib_->t, "Progress");
    if (!progress_.measured.empty()) nib_toast_detail(nib_->t, progress_.measured.c_str());
    if (const u8* b = badges.badge_rgba(progress_.badge)) {
      const ToastImage* img = badges.badge_image(progress_.badge);
      nib_toast_badge(nib_->t, b, img->w, img->h);
    }
    panel = nib_toast_render(nib_->t, w, h, &pw, &ph, &px, &py);
    if (!panel || pw <= 0 || ph <= 0) { panel = nullptr; pw = ph = 0; }
  }
  // nibkit's corner margin, mirrored to the left; 16 units without a panel to measure.
  const int margin = panel ? std::max(0, w - px - pw) : static_cast<int>(std::lround(16.0f * scale));

  // The challenges: their badges in a row, the newest at the right.
  const int tile = std::max(8, static_cast<int>(std::lround(56.0f * scale)));
  const int gap = std::max(2, static_cast<int>(std::lround(8.0f * scale)));
  const int n = static_cast<int>(challenges_.size());
  const int row_w = n ? n * tile + (n - 1) * gap : 0;
  const int pic_w = std::max(pw, row_w);
  const int pic_h = ph + (panel && n ? gap : 0) + (n ? tile : 0);
  if (pic_w <= 0 || pic_h <= 0 || pic_w + margin > w || pic_h + margin > h) return false;

  std::vector<u8> out(static_cast<size_t>(pic_w) * pic_h * 4, 0);
  for (int y = 0; y < ph; ++y)
    std::copy_n(panel + static_cast<size_t>(y) * pw * 4, static_cast<size_t>(pw) * 4, out.data() + static_cast<size_t>(y) * pic_w * 4);
  for (int i = 0; i < n; ++i) {
    const int x0 = i * (tile + gap), y0 = pic_h - tile;
    const ToastImage* img = badges.badge_image(challenges_[static_cast<size_t>(i)].badge);
    for (int y = 0; y < tile; ++y) {
      u8* d = out.data() + (static_cast<size_t>(y0 + y) * pic_w + x0) * 4;
      for (int x = 0; x < tile; ++x, d += 4) {
        u32 p = 0xB0202020u;   // the badge still on its way: a dim square in its place
        if (img && img->px) p = img->px[(y * img->h / tile) * img->w + x * img->w / tile] | 0xFF000000u;
        d[0] = static_cast<u8>(p >> 16);
        d[1] = static_cast<u8>(p >> 8);
        d[2] = static_cast<u8>(p);
        d[3] = static_cast<u8>(p >> 24);
      }
    }
  }
  pic_.set(out.data(), pic_w, pic_h, margin, h - margin - pic_h);
  return true;
}

#else

bool CheevosIndicators::picture(int, int, const ToastQueue&) { return false; }

#endif

} // namespace ds::sdl
