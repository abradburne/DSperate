// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Emulation locked to the panel's refresh (video.vsync_lock). The DS runs at
// 59.8261 Hz, a panel at whatever its mode gives (59.937 Hz on the RG DS+), so
// a pacer on the console's clock drifts against the scanout: a frame shown
// twice every 1 / (difference) seconds (9 s there), and a present that waits
// longer and longer for its flip in between. Locked, frame i is flipped on the
// panel's vblank number base+i and starts a budget before it: one frame per
// refresh, the input taken a fixed time before the flip. The budget is the
// longest any frame lately took from its input to its swap, plus the commit, or the period less video.frame_delay. The console runs at the panel's rate (0.19% fast there); the sound is
// resampled to match (Audio::set_speed), so the pitch moves by 3 cents.
//
// A frame that flips late (emulation over its budget, or the present behind)
// would leave every later swap waiting one refresh for the flip before it:
// SDL's swap waits for the window's previous flip. So the next frame is
// emulated but not shown, the one after is back on its vblank, and the budget
// grows by a millisecond; the console's time keeps pace with the panel's
// either way.
#pragma once
#include "vblank_clock.h"

#include <algorithm>
#include <atomic>
#include <cmath>

namespace ds::sdl {

class VsyncLock {
public:
  // pipe: the DRM CRTC to follow. nominal_ns: a first guess at its period, until measured.
  bool open(int pipe, double nominal_ns) {
    if (!clock_.open()) return false;
    u32 s = 0; u64 t = 0;
    if (!clock_.last(pipe, &s, &t) || !t) return false;
    pipe_ = pipe; period_ = nominal_ns; budget_ = nominal_ns + CAP_OVER;   // until frames have been measured
    s0_ = ref_s_ = s; t0_ = ref_t_ = t;
    rebase_ = true;
    return true;
  }
  bool ok() const { return clock_.ok() && pipe_ >= 0; }
  int pipe() const { return pipe_; }

  // Start a new frame count at the next vblank it can make (unpause, load, after fast forward).
  void reset() { rebase_ = true; skip_ = 0; judged_ = true; }

  // video.frame_delay: a fixed wait after the vblank before a frame starts
  // (the input taken period - delay before its flip). Negative: auto, a
  // budget from what frames have cost lately.
  void set_delay_ns(double ns) { delay_ = ns; if (ns >= 0) budget_ = period_ - ns; }
  // GPU draw and kernel commit after the swap: a swap later than this before a
  // vblank misses it.
  void set_commit_ns(double ns) { commit_ = ns; }

  // Refresh period, measured over the run (nominal until ~0.5 s in).
  double period_ns() const { return period_; }
  bool measured() const { return ref_s_ - s0_ >= 30; }
  // How long before its flip a frame starts: input to flip, ns.
  double budget_ns() const { return budget_; }

  // Whether to present the frame about to be emulated. Not when it is one
  // emulated to catch up, or when the last shown frame flipped late.
  bool take_present() {
    if (catch_up_) return false;
    const u64 seen = swap_frame_.load(std::memory_order_acquire);
    if (!judged_ && seen == shown_ && shown_ >= frame0_) {
      judged_ = true;
      const u64 ns = swap_ns_.load(std::memory_order_relaxed);
      refresh();
      // The frame's cost in two parts: the emulation (start to present),
      // which is what varies, and the present's own work. SDL's swap also
      // waits for each window's flip before (the bottom panel's comes 8 ms
      // after the top's): counted, those waits would feed the budget.
      const double post = static_cast<double>(shown_post_ ? shown_post_ : ns);
      const double emu = post - static_cast<double>(shown_start_);
      cost(emu, static_cast<double>(ns) - post);
      const double unblocked = emu + present_;
      const u64 seq = flip_seq(ns);
      const s64 late = static_cast<s64>(seq) - static_cast<s64>(base_ + (shown_ - frame0_));
      if (late > 0) {
        skip_ = static_cast<u32>(late > 3 ? 3 : late); ++late_;
        last_late_cost_ = unblocked;
        extra_ = std::min(extra_ + 1e6, CAP_OVER);   // a heavier stretch: room at once, decaying back
      }
    }
    extra_ *= EXTRA_DECAY;
    if (skip_) { --skip_; ++skipped_; return false; }
    shown_ = frames_;
    shown_start_ = start_;
    shown_post_ = 0;
    judged_ = false;
    return true;
  }

  // The frame take_present() last let through, to hand to swapped().
  u64 shown() const { return shown_; }
  // Its present began (emulation thread): the emulation's part of its cost ends here.
  void posted(u64 ns) { shown_post_ = ns; }
  // That frame's swap returned (any thread).
  void swapped(u64 frame, u64 ns) {
    swap_ns_.store(ns, std::memory_order_relaxed);
    swap_frame_.store(frame, std::memory_order_release);
  }

  // After a frame: when the next one should start, CLOCK_MONOTONIC ns (0: at
  // once). Frame j is flipped on vblank base + j and starts the budget before
  // it. A whole period behind, it runs at once and is not shown (catching
  // up); two behind, the count starts over, as the pacer drops more than a
  // frame of debt.
  u64 next_start() {
    const u64 frame = ++frames_;
    refresh();
    const u64 now = VblankClock::now_ns();
    if (rebase_) { rebase(frame, now); rebase_ = false; }
    catch_up_ = false;
    if (delay_ < 0 && cost_n_ >= 60) budget_ = std::clamp(pct_ + commit_ + MARGIN + extra_, 3e6, period_ + CAP_OVER);
    u64 start = vblank_time(base_ + frame - frame0_) - static_cast<u64>(budget_);
    if (now > start + static_cast<u64>(2 * period_)) { rebase(frame, now); ++rebased_; start = vblank_time(base_) - static_cast<u64>(budget_); }
    else if (now > start + static_cast<u64>(period_)) { catch_up_ = true; ++caught_up_; start_ = now; return 0; }
    start_ = start > now ? start : now;
    return start;
  }

  // Counts since the last call: late flips, presents left out, frames run to catch up, restarts.
  struct Stats { u32 late, skipped, caught_up, rebased; };
  Stats take_stats() { Stats s{late_, skipped_, caught_up_, rebased_}; late_ = skipped_ = caught_up_ = rebased_ = 0; return s; }

private:
  void refresh() {
    u32 s = 0; u64 t = 0;
    if (!clock_.last(pipe_, &s, &t) || !t || s == ref_s_) return;
    ref_s_ = s; ref_t_ = t;
    if (ref_s_ - s0_ >= 30) period_ = static_cast<double>(ref_t_ - t0_) / static_cast<double>(ref_s_ - s0_);
    if (delay_ >= 0) budget_ = period_ - delay_;
  }
  // Frame `frame` becomes number 0, flipped on the first vblank it can still make from `now`.
  void rebase(u64 frame, u64 now) {
    frame0_ = frame;
    base_ = flip_seq(static_cast<u64>(static_cast<double>(now) + budget_ - commit_));
    judged_ = true;
  }
  // A shown frame's emulation (input to present) and present (to the swap's
  // return). Every 60, the budget becomes the longest emulation of the last
  // 1800 (30 s) plus the present's usual work (10th percentile of the last
  // 120: the rest is waiting), the commit and a margin. A percentile of the
  // emulation left a repeated frame every ten seconds or so in Golden Sun:
  // one late frame is a repeat, so the budget covers the worst seen.
  void cost(double emu, double present) {
    if (delay_ >= 0 || !(emu > 0) || emu > 4 * period_) return;
    costs_[cost_n_ % COSTS] = emu;
    if (present >= 0) presents_[cost_n_ % PRESENTS] = present;
    ++cost_n_;
    if (cost_n_ < 60 || cost_n_ % 60) return;
    const u32 n = cost_n_ < COSTS ? cost_n_ : COSTS, m = cost_n_ < PRESENTS ? cost_n_ : PRESENTS;
    pct_ = *std::max_element(costs_, costs_ + n);
    double v[PRESENTS];
    std::copy(presents_, presents_ + m, v);
    std::nth_element(v, v + m / 10, v + m);
    present_ = v[m / 10];
    pct_ += present_;
  }
  u64 vblank_time(u64 seq) const { return ref_t_ + static_cast<u64>(std::llround((static_cast<double>(seq) - static_cast<double>(ref_s_)) * period_)); }
  // The vblank a swap at `ns` is flipped on.
  u64 flip_seq(u64 ns) const {
    const double after = static_cast<double>(ns) + commit_ - static_cast<double>(ref_t_);
    return ref_s_ + static_cast<u64>(after <= 0 ? 0 : std::ceil(after / period_));
  }

  static constexpr u32 COSTS = 1800, PRESENTS = 120;
  static constexpr double MARGIN = 1.5e6;
  // The budget stops a little over a period: a frame whose emulation alone
  // takes longer is shown a refresh late (one repeat) rather than every frame
  // being held back a whole refresh for it.
  static constexpr double CAP_OVER = 2e6;
  static constexpr double EXTRA_DECAY = 0.995;   // a late flip's extra millisecond, halved in ~2 s
  VblankClock clock_;
  int pipe_ = -1;
  double period_ = 0, delay_ = -1, commit_ = 1.5e6, budget_ = 0;
  double costs_[COSTS] = {};
  double pct_ = 0, extra_ = 0;   // the longest recent cost; room added by late flips
  double presents_[PRESENTS] = {};
  double present_ = 0;           // the present's usual work
  u64 shown_post_ = 0;
public:
  double last_late_cost_ = 0;   // DS_FPS: what the last late frame cost
private:
  u32 cost_n_ = 0;
  // Vblank counts are 32-bit in DRM but kept 64-bit here: a run never wraps them.
  u64 s0_ = 0, t0_ = 0, ref_s_ = 0, ref_t_ = 0;
  u64 base_ = 0, frame0_ = 0;   // frame frame0_ + j is flipped on vblank base_ + j
  bool rebase_ = true, catch_up_ = false, judged_ = true;
  u64 frames_ = 0;              // frames run (next_start() calls)
  u64 start_ = 0;               // when the frame now running started
  u64 shown_ = 0, shown_start_ = 0;   // the frame last allowed to present, and its start
  u32 skip_ = 0;
  std::atomic<u64> swap_frame_{~0ull}, swap_ns_{0};
  u32 late_ = 0, skipped_ = 0, caught_up_ = 0, rebased_ = 0;
};

} // namespace ds::sdl
