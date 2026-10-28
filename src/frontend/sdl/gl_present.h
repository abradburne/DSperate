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
  // A picture filling the window instead of the screens (RGBA bytes, rows
  // top first): a menu drawn at the panel's own resolution. Shown and swapped
  // on the present thread, like draw().
  void draw_picture(const u8* rgba, int w, int h);
  // A picture laid over the screens at every present until replaced or
  // cleared (rgba NULL): RGBA bytes with straight alpha, rows top first,
  // `w` x `h` window pixels at (x, y) from the window's top left, unscaled
  // and blended by its alpha. A toast drawn at the panel's resolution. Copied
  // here; uploaded once, on the present thread, at the next present.
  void set_overlay(const u8* rgba, int w, int h, int x, int y);
  // The window's size in pixels, what set_overlay's places are in.
  bool drawable_size(int& w, int& h) const;
  // A window that may skip a frame identical to the one it shows (no upload,
  // draw or swap): never the one whose swap paces the frame loop.
  void set_may_skip(bool on) { may_skip_ = on; }
  // DS_LATENCY: the screen this window shows, for the GPU-done stamps (latency_log.h).
  void set_lat_screen(int screen) { lat_screen_ = screen; }

  struct Impl;
private:
  int lat_screen_ = 0;
  void gpu_stamp(Impl& d);
  std::unique_ptr<Impl> d_;
  int hires_screen_ = -1;
  bool may_skip_ = false;
  unsigned hires_tex_ = 0;
};

} // namespace ds::sdl
