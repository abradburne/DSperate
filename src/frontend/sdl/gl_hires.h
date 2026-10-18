// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
// Hi-res 3D for the display (video.hires3d). The CPU raster still draws every
// frame at 1x for the emulation (display capture, the 1x picture); the
// frame's polygon list (gpu::Renderer3D::DisplayList) is drawn again here
// through OpenGL ES 3.1 at S x and composited with engine A's 2D planes
// (Engine2D::HiresLine) into an S x picture of that screen, all on the GPU:
// nothing is read back. One render, in passes (docs/rk3568-port.md has the
// picture):
//
//   draw3d     arena upload   textures new since the last frame
//              fans           the polygons as triangle fans, in runs
//              prepass        the opaque prefix's DS depth test; each pixel
//                             keeps its winner (a visibility buffer)
//              resolve        the winners shaded, once a pixel
//              tail           the translucent polygons and shadow volumes,
//                             in list order, run by run
//   composite  lines upload   engine A's kept lines, through a mapped buffer
//              composite      pass 1: pixels the 3D plays no part in or simply
//                             covers; pass 2: colour effects and master
//                             brightness on the rest, with edge marking and
//                             fog where the frame has them
//
// The caller has the window's GL context current (the present thread).
#include "core/types.h"
#include "core/gpu/engine2d.h"
#include "core/gpu/render3d.h"

#include <memory>
#include <string>
#include <vector>

namespace ds::sdl {

class GlHires {
public:
  // What render() composites: the screen as shown (master brightness
  // applied), or as a display capture takes it (engine A's graphics, before
  // master brightness).
  enum Output : int { kShown = 0, kCaptured = 1 };
  // Stages a hires-bench run can leave out (debug_options.h), as mask bits.
  enum Stage : u32 { kPrepass = 1, kResolve = 2, kTail = 4, kEdgeFog = 8, kComposite = 16 };

  GlHires();
  ~GlHires();
  // scale 2-4. False (and a reason) when the context cannot do it.
  bool init(u32 scale, std::string* why = nullptr);
  u32 scale() const { return scale_; }
  // Draws the frame's 3D at S x and composites engine A's screen from its
  // kept lines (`lines`, 192 of them). Returns the composited S x texture
  // (RGBA8, row 0 = the DS's top line), or 0 when the frame cannot be drawn
  // at hi-res (show the 1x picture). Results go round a ring of six
  // textures, so the last frames' (shown, and captures shown a frame or two
  // later) stay valid through this one's.
  unsigned render(const gpu::Renderer3D::DisplayList& dl, const std::vector<u32>& arena, u32 arena_top, u64 arena_epoch,
                  const gpu::Engine2D::HiresLine* lines, Output output = kShown);

  struct Impl;
private:
  bool draw3d(const gpu::Renderer3D::DisplayList& dl, const std::vector<u32>& arena, u32 arena_top, u64 arena_epoch);
  unsigned composite(const gpu::Engine2D::HiresLine* lines, Output output);
  void bench(const gpu::Renderer3D::DisplayList& dl, const std::vector<u32>& arena, u32 arena_top, u64 arena_epoch,
             const gpu::Engine2D::HiresLine* lines, Output output);
  std::unique_ptr<Impl> d_;
  u32 scale_ = 1;
  u32 skip_ = 0;   // Stage bits left out (hires-bench)
};

} // namespace ds::sdl
