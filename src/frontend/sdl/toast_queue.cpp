// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "frontend/sdl/toast_queue.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>

#include "frontend/sdl/rt_budget.h"

#if DSPERATE_NIBKIT_MENU
#include <nibkit_menu.h>
#endif

namespace ds::sdl {
namespace {

bool verbose() {
  static const bool on = std::getenv("DS_VERBOSE") != nullptr;   // as main.cpp's VLOG
  return on;
}

// The face is drawn for the screen's 1024 units; below half that it is too
// small to read, and the pixel toast reads better.
constexpr float kLeastScale = 0.5f;

#if !DSPERATE_NIBKIT_MENU
constexpr int kUnlockFrames = 240;   // four seconds: long enough to read two lines
constexpr int kInfoFrames = 150;
constexpr int kBadgeWait = 90;       // an unlock waits this long for its badge, then shows without
#endif

} // namespace

void ToastPicture::set(const u8* in, int w, int h, int x, int y) {
  rgba.assign(in, in + static_cast<size_t>(w) * h * 4);
  px.resize(static_cast<size_t>(w) * h);
  for (size_t i = 0; i < px.size(); ++i) {
    const u8* p = in + i * 4;
    px[i] = (u32{p[3]} << 24) | (u32{p[0]} << 16) | (u32{p[1]} << 8) | p[2];
  }
  at = Rect{x, y, w, h};
  ok = true;
}

Rect ToastPicture::blit(const Canvas& c) const {
  if (!ok || c.w != cw || c.h != ch) return {};
  const int x0 = at.x, y0 = at.y;
  for (int y = 0; y < at.h && y0 + y < c.h; ++y) {
    if (y0 + y < 0) continue;
    u32* d = c.px + static_cast<size_t>(y0 + y) * c.pitch + x0;
    const u32* s = px.data() + static_cast<size_t>(y) * at.w;
    for (int x = 0; x < at.w && x0 + x < c.w; ++x) {
      if (x0 + x < 0) continue;
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
  return Rect{std::max(x0, 0), std::max(y0, 0), std::min(at.w, c.w - x0), std::min(at.h, c.h - y0)};
}

#if DSPERATE_NIBKIT_MENU
struct ToastQueue::Nib {
  NibToasts* q = nib_toasts_new();
  NibToast* t = nib_toast_new();
  ~Nib() { nib_toast_free(t); nib_toasts_free(q); }
};
ToastQueue::ToastQueue() : nib_(std::make_unique<Nib>()) {}
#else
ToastQueue::ToastQueue() = default;
#endif
ToastQueue::~ToastQueue() = default;

const ToastImage* ToastQueue::badge_image(const std::string& name) const {
  const auto it = badge_views_.find(name);
  return it == badge_views_.end() ? nullptr : &it->second;
}

const u8* ToastQueue::badge_rgba(const std::string& name) const {
  const auto it = badges_.find(name);
  return it == badges_.end() ? nullptr : it->second.rgba.data();
}

void ToastQueue::badge(const std::string& name, int w, int h, std::vector<u32> px) {
  if (w <= 0 || h <= 0 || px.size() != static_cast<size_t>(w) * h) return;
  Img& img = badges_[name];
  img.w = w;
  img.h = h;
  img.px = std::move(px);
  img.rgba.resize(img.px.size() * 4);
  for (size_t i = 0; i < img.px.size(); ++i) {
    const u32 p = img.px[i];
    img.rgba[i * 4 + 0] = static_cast<u8>(p >> 16);
    img.rgba[i * 4 + 1] = static_cast<u8>(p >> 8);
    img.rgba[i * 4 + 2] = static_cast<u8>(p);
    img.rgba[i * 4 + 3] = static_cast<u8>(p >> 24);
  }
  badge_views_[name] = ToastImage{w, h, img.px.data()};
#if DSPERATE_NIBKIT_MENU
  nib_toasts_badge(nib_->q, name.c_str(), img.rgba.data(), w, h);
#endif
}

#if DSPERATE_NIBKIT_MENU

void ToastQueue::push(Toast t) {
  NibToast* n = nib_->t;
  if (t.kind == ToastKind::Unlock) {
    nib_toast_unlock(n, t.title.c_str(), t.detail.empty() ? nullptr : t.detail.c_str(), static_cast<int>(t.points));
  } else {
    nib_toast_begin(n, static_cast<int>(t.kind), t.title.c_str());
    if (!t.detail.empty()) nib_toast_detail(n, t.detail.c_str());
    if (t.points) nib_toast_points(n, static_cast<int>(t.points));
  }
  if (t.header) nib_toast_header(n, t.header);
  if (!t.badge.empty()) {
    const auto it = badges_.find(t.badge);
    if (it != badges_.end()) nib_toast_badge(n, it->second.rgba.data(), it->second.w, it->second.h);
    else nib_toast_await_badge(n, t.badge.c_str());
  }
  if (t.frames > 0) nib_toast_shows(n, t.frames * 1000 / 60);
  const u64 id = nib_toasts_push(nib_->q, n);
  words_[id] = std::move(t);
}

void ToastQueue::tick() {
  ++frames_;
  serial_ = nib_toasts_tick(nib_->q, frames_ * 1000 / 60);
  // The words of toasts gone by are not needed again.
  const u64 shown = nib_toasts_shown_id(nib_->q);
  if (shown) words_.erase(words_.begin(), words_.lower_bound(shown));
}

const Toast* ToastQueue::shown_words() const {
  const auto it = words_.find(nib_toasts_shown_id(nib_->q));
  return it == words_.end() ? nullptr : &it->second;
}

bool ToastQueue::picture(int w, int h) {
  if (!serial_) return false;
  if (pic_.serial == serial_ && pic_.cw == w && pic_.ch == h) return pic_.ok;
  pic_.serial = serial_;
  pic_.cw = w;
  pic_.ch = h;
  pic_.ok = false;
  if (nib_menu_scale_for(w, h) < kLeastScale) return false;
  const auto t0 = std::chrono::steady_clock::now();
  const OffRealtime off_rt;
  int pw = 0, ph = 0, x = 0, y = 0;
  const u8* px = nib_toasts_render(nib_->q, w, h, &pw, &ph, &x, &y);
  if (px && pw > 0 && ph > 0 && pw <= w && ph <= h) pic_.set(px, pw, ph, x, y);
  if (verbose())
    std::fprintf(stderr, "toast %llu drawn for %dx%d in %.1f ms%s\n", static_cast<unsigned long long>(serial_), w, h,
                 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(),
                 pic_.ok ? "" : " (not drawn)");
  return pic_.ok;
}

#else

void ToastQueue::push(Toast t) { queue_.push_back(std::move(t)); }

void ToastQueue::tick() {
  ++frames_;
  if (left_ > 0 && --left_ > 0) return;
  if (queue_.empty()) { serial_ = 0; return; }
  const Toast& next = queue_.front();
  if (next.kind == ToastKind::Unlock && !next.badge.empty() && !badges_.count(next.badge) && wait_ < kBadgeWait) {
    ++wait_;   // the badge is on its way: a moment more, then without it
    if (left_ == 0) serial_ = 0;
    return;
  }
  wait_ = 0;
  now_ = std::move(queue_.front());
  queue_.pop_front();
  left_ = now_.frames > 0 ? now_.frames : now_.kind == ToastKind::Unlock ? kUnlockFrames : kInfoFrames;
  serial_ = frames_;
}

const Toast* ToastQueue::shown_words() const { return serial_ ? &now_ : nullptr; }

bool ToastQueue::picture(int, int) { return false; }

#endif

Rect ToastQueue::draw(const Canvas& c) {
  if (picture(c.w, c.h)) return pic_.blit(c);
  const Toast* t = shown_words();
  if (!t) return {};
  const char* header = t->header ? t->header : t->kind == ToastKind::Unlock ? "Achievement unlocked" : nullptr;
  const char* detail = t->detail.empty() ? nullptr : t->detail.c_str();
  const ToastImage* badge = t->badge.empty() ? nullptr : badge_image(t->badge);
  draw_toast(c, header, t->title.c_str(), detail, t->points, badge);
  return toast_rect(c, header, t->title.c_str(), detail, t->points, badge);
}

} // namespace ds::sdl
