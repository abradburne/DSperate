// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
// The toasts over a game, one at a time, in turn. Where DSperate is built with
// nibkit (DSPERATE_NIBKIT_MENU) it is nibkit's queue (nib_toasts_*, the one every
// nibOS app uses): its timing, its wait for a badge, its pictures in DSlot's style,
// bottom right. Without nibkit, or on a target under half the screen's scale (a
// 256x192 scratch), the words go to menu.cpp's pixel toast; without nibkit the
// queue is DSperate's own, the same rules counted in frames.
//
// The clock is the frames played (tick() once per emulated frame), so a toast
// waits while the game is paused.
#include "core/types.h"
#include "frontend/sdl/menu.h"

#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ds::sdl {

enum class ToastKind : int { Info = 0, Unlock = 1, Problem = 2 };   // NIB_TOAST_*

struct Toast {
  ToastKind kind = ToastKind::Info;
  const char* header = nullptr;   // an unlock's is nibkit's "Achievement unlocked"
  std::string title, detail;
  u32 points = 0;
  std::string badge;              // a badge name: shown once ToastQueue::badge() has it
  int frames = 0;                 // how long it shows; 0: its kind's time (4 s an unlock, else 2.5 s)
};

// A picture drawn for a target of a size: RGBA bytes with straight alpha as
// nibkit drew it, the same as 0xAARRGGBB for a canvas, and where it goes.
struct ToastPicture {
  bool ok = false;
  u64 serial = 0;                 // what it is a picture of
  int cw = 0, ch = 0;             // the target it was drawn for
  Rect at;
  std::vector<u8> rgba;
  std::vector<u32> px;
  void set(const u8* rgba_in, int w, int h, int x, int y);
  // Lays it over `c` (the target it was drawn for); what it covered.
  Rect blit(const Canvas& c) const;
};

class ToastQueue {
public:
  ToastQueue();
  ~ToastQueue();
  ToastQueue(const ToastQueue&) = delete;
  ToastQueue& operator=(const ToastQueue&) = delete;

  void push(Toast t);
  // A badge decoded (0xAARRGGBB, rows top first): to every toast waiting for it.
  void badge(const std::string& name, int w, int h, std::vector<u32> px);
  const ToastImage* badge_image(const std::string& name) const;
  // Badge bytes as nibkit takes them, RGBA; null for one not had.
  const u8* badge_rgba(const std::string& name) const;

  // Once per emulated frame.
  void tick();
  bool on() const { return serial_ != 0; }
  // Changes whenever what is shown does; 0 for nothing.
  u64 serial() const { return serial_; }

  // The toast on show drawn in DSlot's style for a `w` x `h` target, once per toast
  // and size (as an ordinary task: text rasterised, a few ms), kept after. False
  // when there is none, or no nibkit, or the target is too small: then draw().
  bool picture(int w, int h);
  const ToastPicture& pic() const { return pic_; }
  // Draws the toast on show on `c`, the picture or the pixel toast; what it covered.
  Rect draw(const Canvas& c);

private:
  const Toast* shown_words() const;
  struct Img { int w = 0, h = 0; std::vector<u32> px; std::vector<u8> rgba; };
  std::map<std::string, Img> badges_;
  std::map<std::string, ToastImage> badge_views_;
  u64 frames_ = 0;
  u64 serial_ = 0;
  ToastPicture pic_;
#if DSPERATE_NIBKIT_MENU
  struct Nib;
  std::unique_ptr<Nib> nib_;
  std::map<u64, Toast> words_;    // the queue's toasts by id, for the pixel toast
#else
  std::deque<Toast> queue_;
  Toast now_;
  int left_ = 0;
  int wait_ = 0;
#endif
};

} // namespace ds::sdl
