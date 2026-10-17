// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
// The renderer tier through a GLES 3.1 context of our own instead of an
// SDL_Renderer (video.hires3d): the same views, drawn as textured quads, plus
// the hi-res screen GlHires composites, which stays on the GPU. Every window
// of the process shares one context group, so a texture drawn in one can be
// shown by another.
#include "core/types.h"
#include "frontend/video/layout.h"

#include <SDL2/SDL.h>

#include <memory>

namespace ds::sdl {

class GlPresent {
public:
  GlPresent();
  ~GlPresent();
  bool open(SDL_Window* win, bool vsync, bool linear);
  void close();
  void make_current();
  // Shows `screen` from this texture (S x, row 0 = the DS's top line) at the next draw.
  void set_hires(int screen, unsigned tex) { hires_screen_ = screen; hires_tex_ = tex; }
  // The GL work runs on the present thread (present_thread.h) only, so a
  // context is never current on two threads: draw() posts it there and waits;
  // draw_now() is the work itself, for a job already on that thread.
  void draw(const frontend::View* views, int nviews, const u32* const fb[2]);
  void draw_now(const frontend::View* views, int nviews, const u32* const fb[2]);

  struct Impl;
private:
  std::unique_ptr<Impl> d_;
  int hires_screen_ = -1;
  unsigned hires_tex_ = 0;
};

} // namespace ds::sdl
