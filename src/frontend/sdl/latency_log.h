// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// DS_LATENCY=<path>: one line per emulated frame, for measuring button to
// screen (pocketOS's emu/latency). CLOCK_MONOTONIC ns, the clock DRM's vblank
// timestamps and evdev (EVIOCSCLOCKID) can be read in:
//
//   frame buttons poll emu post swap0 swap1 hash0 hash1 vbl0 vbl1 gpu0 gpu1
//
// frame: nds.frame_count before the frame ran; buttons: what the core was
// given (input::Frame::buttons); poll: when that input was taken; emu: the
// emulated frame done; post: the present begun (or handed to the present
// thread), 0 for a frame not shown (then the rest are 0 too); swapN: the swap of the window showing screen N returned (0 if
// none); hashN: the screen as presented, OSD included; vblN: CRTC N's last
// vblank at post (DRM pipe N), so its vblank grid can be rebuilt offline;
// gpuN: the GPU done with that window's frame (GLES present: a timestamp
// query before the swap; 0 elsewhere). Lines are written a few frames late,
// once the swaps and the GPU's stamps are in.
#pragma once
#include "core/types.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include "vblank_clock.h"

namespace ds::sdl::latlog {

struct Rec { u64 frame = 0; u32 buttons = 0; u64 poll = 0, emu = 0, post = 0, swap[2] = {0, 0}; u32 hash[2] = {0, 0}; u64 vbl[2] = {0, 0}, gpu[2] = {0, 0}; };

struct State {
  std::FILE* f = nullptr;
  static constexpr u64 RING = 16, LAG = 8;   // a frame's line is written LAG frames later
  Rec ring[RING];    // by frame number n
  u64 n = 0;         // frames begun (poll() calls); the current one is n - 1
  u64 shown = ~0ull; // the frame last posted
  VblankClock vbl;
  State() {
    if (const char* p = std::getenv("DS_LATENCY")) {
      f = std::fopen(p, "w");
      if (f) std::setvbuf(f, nullptr, _IOLBF, 0);
      vbl.open();
    }
  }
};
inline State& st() { static State s; return s; }
inline bool on() { return st().f != nullptr; }

inline u64 now_ns() {
  timespec t{};
  clock_gettime(CLOCK_MONOTONIC, &t);
  return static_cast<u64>(t.tv_sec) * 1'000'000'000ull + static_cast<u64>(t.tv_nsec);
}

inline u32 hash(const u32* px, u32 n) {
  u32 h = 2166136261u;
  for (u32 i = 0; i < n; ++i) h = (h ^ (px[i] & 0xFFFFFFu)) * 16777619u;
  return h;
}

// Emulation thread, once the frame's input is final. Writes the line of the
// frame LAG frames back: its present (one in flight at most) is long done.
inline void poll(u64 frame, u32 buttons) {
  if (!on()) return;
  State& s = st();
  if (s.n >= State::LAG) {
    const Rec& r = s.ring[(s.n - State::LAG) % State::RING];
    std::fprintf(s.f, "%llu %u %llu %llu %llu %llu %llu %08x %08x %llu %llu %llu %llu\n", static_cast<unsigned long long>(r.frame), r.buttons,
                 static_cast<unsigned long long>(r.poll), static_cast<unsigned long long>(r.emu), static_cast<unsigned long long>(r.post),
                 static_cast<unsigned long long>(r.swap[0]), static_cast<unsigned long long>(r.swap[1]), r.hash[0], r.hash[1],
                 static_cast<unsigned long long>(r.vbl[0]), static_cast<unsigned long long>(r.vbl[1]),
                 static_cast<unsigned long long>(r.gpu[0]), static_cast<unsigned long long>(r.gpu[1]));
  }
  Rec& r = s.ring[s.n % State::RING];
  r = Rec{};
  r.frame = frame; r.buttons = buttons; r.poll = now_ns();
  ++s.n;
}
inline Rec* cur() { State& s = st(); return s.n ? &s.ring[(s.n - 1) % State::RING] : nullptr; }
// The input given again mid-frame (emu.input_at = vblank): the frame's input is this one, taken now.
inline void repoll(u32 buttons) {
  if (!on()) return;
  if (Rec* r = cur()) if (buttons != r->buttons) { r->buttons = buttons; r->poll = now_ns(); }
}
inline void emu_done() { if (on()) if (Rec* r = cur()) r->emu = now_ns(); }

// Emulation thread, as the frame's present begins (or is handed to the present thread).
inline void post(const u32* const fb[2]) {
  if (!on()) return;
  State& s = st();
  Rec* r = cur();
  if (!r) return;
  r->post = now_ns();
  for (int i = 0; i < 2; ++i) { r->hash[i] = hash(fb[i], 256 * 192); s.vbl.last(i, nullptr, &r->vbl[i]); }
  s.shown = s.n - 1;
}

// Whichever thread swaps: the window showing `screen` (0 top, 1 bottom) has swapped the frame last posted.
inline void swapped(int screen) {
  if (!on()) return;
  State& s = st();
  if (s.shown == ~0ull || s.n - s.shown >= State::LAG) return;
  s.ring[s.shown % State::RING].swap[screen & 1] = now_ns();
}

// The frame the present thread is drawing now (the last posted), to hand to gpu_done() later.
inline u64 current() { return on() ? st().shown : ~0ull; }
// The GPU finished frame `id` on the window showing `screen`.
inline void gpu_done(u64 id, int screen, u64 ns) {
  if (!on()) return;
  State& s = st();
  if (id == ~0ull || s.n - id >= State::LAG) return;   // its line is written
  s.ring[id % State::RING].gpu[screen & 1] = ns;
}

} // namespace ds::sdl::latlog
