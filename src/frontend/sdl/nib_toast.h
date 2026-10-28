// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
// Toasts in DSlot's style: nibkit's (nibOS's sdk/nibkit-menu-c, nib_toast_render)
// face, panel and colours, the same as the shared pause menu, drawn once when a
// toast appears and kept as a picture with alpha, bottom right: laid over the
// canvas each frame (blit), or handed to the GLES presenter as an overlay
// (Display::set_overlay) where the screens are textures scaled on the GPU and
// there is no panel-resolution buffer to draw in. Built only with
// DSPERATE_NIBKIT_MENU; without it, or on a canvas too small for the face (a
// 256x192 scratch), menu.cpp's draw_toast.
#include "core/types.h"
#include "frontend/sdl/menu.h"

#include <vector>

struct NibToast;

namespace ds::sdl {

class NibToastPainter {
public:
  enum Kind : int { Info = 0, Unlock = 1, Problem = 2 };   // NIB_TOAST_*

  NibToastPainter();
  ~NibToastPainter();
  NibToastPainter(const NibToastPainter&) = delete;
  NibToastPainter& operator=(const NibToastPainter&) = delete;

  // Makes toast `serial`'s picture for a target `cw` x `ch` pixels, unless it
  // is already made for it. False when the target is too small for the face.
  bool prepare(u64 serial, int cw, int ch, Kind kind, const char* header, const char* title,
               const char* detail, u32 points, const ToastImage* badge);
  // Whether prepare() has been asked for toast `serial` on a canvas of `c`'s
  // size already (made or not): then it only answers.
  bool prepared(u64 serial, int cw, int ch) const { return serial == serial_ && cw == cw_ && ch == ch_; }
  // Where the prepared picture goes on its target: nibkit's place for a toast
  // (nib_toast_place), bottom right a margin in.
  Rect place() const;
  // Its pixels: RGBA bytes with straight alpha, rows top first, place()'s size.
  const u8* rgba() const { return ok_ ? rgba_.data() : nullptr; }
  // Lays the prepared picture over `c`, bottom right; what it covered.
  Rect blit(const Canvas& c) const;

private:
  NibToast* t_ = nullptr;
  u64 serial_ = ~u64{0};
  int cw_ = 0, ch_ = 0, w_ = 0, h_ = 0;
  float scale_ = 1.0f;
  bool ok_ = false;
  std::vector<u32> px_;   // 0xAARRGGBB, straight alpha
  std::vector<u8> rgba_;  // the same as nibkit drew it
  std::vector<u8> badge_rgba_;
};

} // namespace ds::sdl
