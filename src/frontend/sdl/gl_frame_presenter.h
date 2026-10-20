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
// moves engine A between the screens through captures keeps each pair whole.
// While too many are left out HiresGovernor shows every other frame at S x,
// evenly, with adaptive frameskip on (between them only a display capture is
// drawn, at S x, for the next shown frame), and the 3D at 1x when even that
// cannot keep up. Only which frames are shown changes. Every frame is tried
// again once a fence after each shown frame's work has passed a frame later:
// at half rate the job itself always ends in time, its GPU work need not. At 1x
// the GPU's share is small and what is left of a job is its swap waiting for
// a flip: there the next frame waits, which is vsync pacing.
#include "core/types.h"
#include "frontend/sdl/gl_hires.h"
#include "frontend/sdl/hires_governor.h"

#include <atomic>
#include <functional>
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
  // Each frame, before present(): `half`, adaptive frameskip is on (the
  // governor may show every other frame); `fast`, fast forward (which shows
  // what it shows: nothing is judged by it).
  void set_frameskip(bool half, bool fast) { governor_.set_half(half); fast_ = fast; }

  // Optional callbacks: `handed_over` on the emulation thread once the frame
  // is the present thread's (after the wait for the one before), `swapped`
  // on the present thread once its windows have swapped (the vsync lock's).
  struct Hooks { std::function<void()> handed_over, swapped; };
  // The frame just run, on `top` (and `second`, a window for the other
  // panel, or null); `fb` the screens' 1x pictures.
  void present(NDS& nds, const u32* const fb[2], Display& top, Display* second, const Hooks& hooks = {});

  // Frames left out so far, the GPU still drawing the one before.
  u64 late() const { return late_; }
  // Frames not shown on purpose, between the shown ones at half rate.
  u64 between() const { return between_; }

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
  // The job; !show: a capture only. `mark`: a fence after it (half rate's measure).
  void draw(Display* const wins[2], bool show = true, bool mark = false);
  void say_mode() const;

  std::unique_ptr<GlHires> gl_;
  HiresGovernor governor_;
  bool verbose_ = false;
  Snapshot snap_;
  Capture caps_[3];
  int cap_next_ = 0;
  u64 job_ = 0;               // the last job posted
  u64 late_ = 0, between_ = 0;
  bool fast_ = false;
  // Half rate: whether the GPU had finished the last shown frame one frame
  // on, and two (the next shown frame) (GlHires::frame_end_passed); set on
  // the present thread, read a pair later.
  std::atomic<int> fit1_{-1}, fit2_{-1};
  int period_left_ = 0;       // frames left in the display period the decision holds for
  bool skipping_ = false, hi_now_ = false;
  // hires-stats.
  u32 n_ = 0, n_now_ = 0, n_cap_ = 0, n_both_ = 0, n_from_cap_ = 0;
};

} // namespace ds::sdl
