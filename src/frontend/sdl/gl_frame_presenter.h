// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
// The frame loop's present through the GLES path (Display::gl(): the
// renderer sink with video.hires3d, or a window a screen at 1x). Every GL
// call runs on the present thread (present_thread.h), so a frame's GPU work
// overlaps the next frame's emulation:
//
//   emulation thread                      present thread
//   ----------------                      --------------
//   run frame N
//   present(N): wait for job N-1,   --->  job N: hi-res 3D (GlHires) for the
//     snapshot what the GPU reads,          screen engine A shows, captures
//     post job N                            redone at hi-res, both windows'
//   run frame N+1                           draws and swaps
//
// With hi-res 3D the frame's polygon list is drawn again at S x and
// composited in the context of the window showing engine A: shown at once in
// display mode 1; a display capture of engine A is drawn too and shown the
// frame after, when the bank it went to is shown (the DS's own delay).
//
// When the GPU is still on job N-1, frame N is left out (the one before
// stays up) rather than holding the emulation, and its sound, back: at 2x
// Golden Sun's field takes ~21 ms of GPU a frame, and waiting ran the whole
// game at 47 fps. That is decided once a display period, so a game that
// moves engine A between the screens through captures keeps each pair whole;
// HiresGovernor drops the 3D to 1x while too many frames are left out. At 1x
// the GPU's share is small and what is left of a job is its swap waiting for
// a flip: there the next frame waits, which is vsync pacing.
#include "core/types.h"
#include "frontend/sdl/gl_hires.h"
#include "frontend/sdl/hires_governor.h"

#include <memory>
#include <vector>

namespace ds { struct NDS; }

namespace ds::sdl {

class Display;

class GlFramePresenter {
public:
  GlFramePresenter() = default;
  ~GlFramePresenter();
  GlFramePresenter(const GlFramePresenter&) = delete;
  GlFramePresenter& operator=(const GlFramePresenter&) = delete;

  // video.hires3d = `scale` (2-4) on `top`'s GLES present: the polygon lists
  // and engine A's lines kept by the core, the 3D drawn again at that scale.
  // `fallback`: video.hires3d_fallback. Prints what it got; false at 1x.
  bool open_hires(NDS& nds, Display& top, u32 scale, bool fallback);
  bool hires() const { return gl_ != nullptr; }
  void set_verbose(bool on) { verbose_ = on; }

  // The frame just run, on `top` (and `second`, a window for the other
  // panel, or null); `fb` the screens' 1x pictures.
  void present(NDS& nds, const u32* const fb[2], Display& top, Display* second);

  // Frames left out so far, the GPU still drawing the one before.
  u64 late() const { return late_; }

private:
  // What the present thread reads for a frame, copied on the emulation thread.
  struct Snapshot {
    std::vector<u32> fb[2];
    gpu::Renderer3D::DisplayList dl;
    std::vector<u32> arena;
    u32 arena_top = 0;
    u64 arena_epoch = ~0ull;
    const gpu::Engine2D::HiresLine* lines = nullptr;   // Gpu's own set for the frame (two alternate)
    int screen = -1;          // engine A's screen, -1: no hi-res this frame
    u32 mode = 0;             // engine A's display mode
    int capture = 0;          // Gpu::hires_capture() of the frame's capture
    bool has_list = false;
  };
  // A capture redone at hi-res, with its 1x picture (as VRAM holds it) to
  // recognise it when a screen shows it again. Present thread only.
  struct Capture { std::vector<u32> img; unsigned tex = 0; };

  void snapshot(NDS& nds, const u32* const fb[2]);
  void draw(Display* const wins[2]);   // the job

  std::unique_ptr<GlHires> gl_;
  HiresGovernor governor_;
  bool verbose_ = false;
  Snapshot snap_;
  Capture caps_[3];
  int cap_next_ = 0;
  u64 job_ = 0;               // the last job posted
  u64 late_ = 0;
  int period_left_ = 0;       // frames left in the display period the decision holds for
  bool skipping_ = false, hi_now_ = false;
  // hires-stats.
  u32 n_ = 0, n_now_ = 0, n_cap_ = 0, n_both_ = 0, n_from_cap_ = 0;
};

} // namespace ds::sdl
