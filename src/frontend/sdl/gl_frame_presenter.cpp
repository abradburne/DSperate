// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "frontend/sdl/gl_frame_presenter.h"
#include "core/gpu/kernels.h"
#include "core/nds.h"
#include "frontend/sdl/debug_options.h"
#include "frontend/sdl/display.h"
#include "frontend/sdl/present_thread.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace ds::sdl {

GlFramePresenter::~GlFramePresenter() {
  if (job_) PresentThread::get().wait(job_);
}

bool GlFramePresenter::open_hires(NDS& nds, Display& top, u32 scale, bool fallback) {
  if (!top.gl()) { std::fprintf(stderr, "hires3d: needs video.sink = renderer and GLES; showing 1x\n"); return false; }
  auto gl = std::make_unique<GlHires>();
  std::string why;
  bool ok = false;
  PresentThread& pt = PresentThread::get();
  pt.wait(pt.post([&] { top.gl()->make_current(); ok = gl->init(scale, &why); }));
  if (!ok) { std::fprintf(stderr, "hires3d: unavailable (%s)\n", why.c_str()); return false; }
  nds.gpu3d.renderer().set_display_list(true);
  nds.gpu.set_hires(true);
  gl_ = std::move(gl);
  governor_.set_enabled(fallback);
  std::fprintf(stderr, "hires3d: %ux on the GPU (%ux%u)\n", scale, 256 * scale, 192 * scale);
  return true;
}

void GlFramePresenter::present(NDS& nds, const u32* const fb[2], Display& top, Display* second, const Hooks& hooks) {
  PresentThread& pt = PresentThread::get();
  if (period_left_ <= 0) {   // the resolution too changes only between display periods
    hi_now_ = gl_ && governor_.hires();
    skipping_ = !debug::options().present_wait && hi_now_ && !pt.done(job_);
    period_left_ = std::max<int>(1, nds.gpu.display_phase_period());
  }
  --period_left_;
  if (gl_ && governor_.frame(skipping_) && verbose_)
    std::fprintf(stderr, "hires3d: %s (%u drops to 1x so far; 2x tried again after %u frames)\n", governor_.hires() ? "back to 2x" : "GPU behind, 3D at 1x",
                 governor_.drops(), governor_.wait_frames());
  if (skipping_) { ++late_; return; }
  pt.wait(job_);
  if (hooks.handed_over) hooks.handed_over();
  snapshot(nds, fb);
  Display* const wins[2] = {&top, second};
  job_ = pt.post([this, wins, swapped = hooks.swapped] { draw(wins); if (swapped) swapped(); });
}

// What the job reads, copied while the emulation waits: the screens, and
// with hi-res 3D the frame's polygon list, the texture arena's new part and
// which of Gpu's two line sets is the frame's.
void GlFramePresenter::snapshot(NDS& nds, const u32* const fb[2]) {
  Snapshot& s = snap_;
  s.screen = -1; s.mode = 0; s.capture = 0; s.has_list = false;
  const int scr = nds.gpu.screen_a();
  const gpu::Renderer3D::FrameRef& ref = nds.gpu.frame_3d_shown();
  if (hi_now_ && scr >= 0 && ref.dlist && nds.gpu.hires_lines()) {
    s.screen = scr;
    s.mode = nds.gpu.display_mode_a();
    s.capture = gpu::Gpu::hires_capture(nds.gpu.captured_last());
    if (s.mode == 1 || s.capture == 1) {
      const gpu::Renderer3D::DisplayList& src = *ref.dlist;
      s.dl.polys.assign(src.polys.begin(), src.polys.begin() + src.npoly);
      s.dl.verts.assign(src.verts.begin(), src.verts.begin() + src.nvert);
      s.dl.post = src.post; s.dl.frame = src.frame; s.dl.npoly = src.npoly; s.dl.nvert = src.nvert; s.dl.epoch = src.epoch; s.dl.valid = src.valid;
      u32 top = 0;
      u64 epoch = 0;
      const std::vector<u32>& arena = nds.gpu3d.renderer().display_arena(&top, &epoch);
      if (epoch != s.arena_epoch) { s.arena_epoch = epoch; s.arena_top = 0; }
      if (s.arena.size() < arena.size()) s.arena.resize(arena.size());
      if (top > s.arena_top) std::memcpy(s.arena.data() + s.arena_top, arena.data() + s.arena_top, (top - s.arena_top) * sizeof(u32));
      s.arena_top = top;
      s.lines = nds.gpu.hires_lines();
      s.has_list = true;
    }
  }
  for (int sc = 0; sc < 2; ++sc) s.fb[sc].assign(fb[sc], fb[sc] + 256 * 192);
}

// On the present thread. A display capture of engine A is drawn at hi-res
// too, and its 1x picture kept (as the DS shows a capture again: 15-bit,
// through VRAM). A screen whose 1x picture is exactly a recent capture's --
// engine A's mode 2, or engine B showing the bank -- shows that capture's
// hi-res picture, whatever order the game moves them in (Golden Sun swaps
// engine A between the screens every frame).
void GlFramePresenter::draw(Display* const wins[2]) {
  const Snapshot& s = snap_;
  const u32* const sfb[2] = {s.fb[0].data(), s.fb[1].data()};
  auto window_of = [&](int screen) -> Display* {
    for (int i = 0; i < 2; ++i) if (wins[i] && wins[i]->gl() && wins[i]->shows(screen)) return wins[i];
    return nullptr;
  };
  if (gl_ && wins[0]->gl() && s.screen >= 0) {
    wins[0]->gl()->make_current();   // one context group: textures are shared
    auto same5 = [](const u32* a, const u32* b) {   // 5 bits a channel: VRAM's precision
      for (u32 k = 0; k < 256 * 192; ++k) if ((a[k] ^ b[k]) & 0xF8F8F8u) return false;
      return true;
    };
    int shown_from[2] = {-1, -1};
    for (int sc = 0; sc < 2; ++sc)
      for (int k = 0; k < 3; ++k) if (caps_[k].tex && same5(sfb[sc], caps_[k].img.data())) { shown_from[sc] = k; break; }
    for (int sc = 0; sc < 2; ++sc)
      if (shown_from[sc] >= 0) if (Display* w = window_of(sc)) w->gl()->set_hires(sc, caps_[shown_from[sc]].tex);
    const bool now = s.mode == 1 && s.has_list && shown_from[s.screen] < 0;
    if (now)
      if (const unsigned tex = gl_->render(s.dl, s.arena, s.arena_top, s.arena_epoch, s.lines))
        if (Display* w = window_of(s.screen)) w->gl()->set_hires(s.screen, tex);
    if (s.capture == 1 && s.has_list) {   // into the oldest slot
      Capture& c = caps_[cap_next_];
      cap_next_ = (cap_next_ + 1) % 3;
      c.img.resize(256 * 192);
      u16 v15[256];
      for (u32 y = 0; y < 192; ++y) {
        gpu::kern::active::capture_a15(s.lines[y].out, 256, v15);
        gpu::kern::active::output_vram_line(v15, 0, &c.img[y * 256]);
      }
      c.tex = gl_->render(s.dl, s.arena, s.arena_top, s.arena_epoch, s.lines, GlHires::kCaptured);
    }
    if (debug::options().hires_stats) {
      ++n_; n_now_ += now; n_cap_ += s.capture == 1; n_both_ += now && s.capture == 1; n_from_cap_ += (shown_from[0] >= 0) + (shown_from[1] >= 0);
      if (n_ % 300 == 0)
        std::fprintf(stderr, "hires3d: %u frames: %u shown at once, %u captured, %u both, %u screens shown from a capture\n", n_, n_now_, n_cap_, n_both_, n_from_cap_);
    }
  }
  for (int i = 0; i < 2; ++i) if (wins[i]) wins[i]->draw_gl_now(sfb);
}

} // namespace ds::sdl
