// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
// Hi-res 3D for the display (video.hires3d): the CPU raster still draws every
// frame at 1x for the emulation (display capture, the 1x picture), and the
// frame's polygon list (gpu::Renderer3D::DisplayList) is drawn again here through
// OpenGL ES 3.1 at S x, then composited with engine A's 2D planes
// (Engine2D::HiresLine) into an S x picture of that screen, all on the GPU:
// nothing is read back. The caller has the window's GL context current.
#include "core/types.h"
#include "core/gpu/engine2d.h"
#include "core/gpu/render3d.h"

#include <memory>
#include <string>
#include <vector>

namespace ds::sdl {

class GlHires {
public:
  GlHires();
  ~GlHires();
  // scale 2-4. False (and a reason) when the context cannot do it.
  bool init(u32 scale, std::string* why = nullptr);
  u32 scale() const { return scale_; }
  // Draws the frame's 3D at S x and composites engine A's screen. `fb` is
  // that screen's finished 1x picture (0xAARRGGBB), shown where a line has no
  // 3D. Returns the composited S x texture (RGBA8, row 0 = the DS's top line),
  // or 0 when the frame cannot be drawn at hi-res (show the 1x picture).
  unsigned render(const gpu::Renderer3D::DisplayList& dl, const std::vector<u32>& arena, u32 arena_top, u64 arena_epoch,
                  const gpu::Engine2D::HiresLine* lines, const u32* fb);

  struct Impl;
private:
  std::unique_ptr<Impl> d_;
  u32 scale_ = 1;
};

} // namespace ds::sdl
