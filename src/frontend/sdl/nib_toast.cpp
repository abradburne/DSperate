// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "frontend/sdl/nib_toast.h"

#include <nibkit_menu.h>

#include <algorithm>

namespace ds::sdl {

namespace {
// The face is drawn for the screen's 1024 units; below half that it is too
// small to read, and the pixel toast reads better.
constexpr float kLeastScale = 0.5f;
} // namespace

NibToastPainter::NibToastPainter() : t_(nib_toast_new()) {}
NibToastPainter::~NibToastPainter() { nib_toast_free(t_); }

bool NibToastPainter::prepare(u64 serial, int cw, int ch, Kind kind, const char* header,
                              const char* title, const char* detail, u32 points,
                              const ToastImage* badge) {
  if (prepared(serial, cw, ch)) return ok_;
  serial_ = serial;
  cw_ = cw;
  ch_ = ch;
  ok_ = false;
  const float scale = std::min(cw / 1024.0f, ch / 768.0f);
  scale_ = scale;
  if (!t_ || scale < kLeastScale) return false;
  const u8* rgba = nullptr;
  if (badge && badge->px && badge->w > 0 && badge->h > 0) {
    badge_rgba_.resize(static_cast<size_t>(badge->w) * badge->h * 4);
    for (size_t i = 0; i < static_cast<size_t>(badge->w) * badge->h; ++i) {
      const u32 p = badge->px[i];
      badge_rgba_[i * 4 + 0] = static_cast<u8>(p >> 16);
      badge_rgba_[i * 4 + 1] = static_cast<u8>(p >> 8);
      badge_rgba_[i * 4 + 2] = static_cast<u8>(p);
      badge_rgba_[i * 4 + 3] = static_cast<u8>(p >> 24);
    }
    rgba = badge_rgba_.data();
  }
  // Two thirds of the screen at most: past that it starts hiding the game.
  const float most = 1024.0f * 2.0f / 3.0f;
  int w = 0, h = 0;
  const u8* out = nib_toast_render(t_, kind, header, title, detail, static_cast<int>(points), rgba,
                                   rgba ? badge->w : 0, rgba ? badge->h : 0, scale, most, &w, &h);
  if (!out || w <= 0 || h <= 0 || w > cw || h > ch) return false;
  rgba_.assign(out, out + static_cast<size_t>(w) * h * 4);
  w_ = w;
  h_ = h;
  px_.resize(static_cast<size_t>(w) * h);
  for (size_t i = 0; i < px_.size(); ++i) {
    const u8* p = out + i * 4;
    px_[i] = (u32{p[3]} << 24) | (u32{p[0]} << 16) | (u32{p[1]} << 8) | p[2];
  }
  ok_ = true;
  return true;
}

Rect NibToastPainter::place() const {
  if (!ok_) return {};
  int x = 0, y = 0;
  nib_toast_place(cw_, ch_, scale_, w_, h_, &x, &y);   // nibkit's corner, every app's
  return Rect{x, y, w_, h_};
}

Rect NibToastPainter::blit(const Canvas& c) const {
  if (!ok_ || c.w != cw_ || c.h != ch_) return {};
  const Rect at = place();
  const int x0 = at.x, y0 = at.y;
  for (int y = 0; y < h_ && y0 + y < c.h; ++y) {
    u32* d = c.px + static_cast<size_t>(y0 + y) * c.pitch + x0;
    const u32* s = px_.data() + static_cast<size_t>(y) * w_;
    for (int x = 0; x < w_ && x0 + x < c.w; ++x) {
      const u32 a = s[x] >> 24;
      if (a == 0) continue;
      if (a == 255) { d[x] = s[x]; continue; }
      const u32 sp = s[x], dp = d[x];
      u32 out = 0xFF000000u;
      for (int sh = 0; sh <= 16; sh += 8) {
        const u32 sc = (sp >> sh) & 0xFF, dc = (dp >> sh) & 0xFF;
        out |= ((dc * (255 - a) + sc * a + 127) / 255) << sh;
      }
      d[x] = out;
    }
  }
  return Rect{x0, y0, std::min(w_, c.w - x0), std::min(h_, c.h - y0)};
}

} // namespace ds::sdl
