// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// video.hires3d_fallback and adaptive frameskip at hi-res
// (frontend/sdl/hires_governor.h): every frame, every other frame, or 1x,
// frame by frame.
#include "check.h"
#include "frontend/sdl/hires_governor.h"

#include <cstdio>
#include <initializer_list>

using ds::sdl::HiresGovernor;
using Mode = HiresGovernor::Mode;

namespace {

// Feeds `n` frames at Full, `late` of every `of` left out; the frame count at
// the first change of mode (0: none).
int full(HiresGovernor& g, int n, int late = 0, int of = 1) {
  for (int i = 1; i <= n; ++i)
    if (g.frame(i % of < late)) return i;
  return 0;
}

// Feeds `n` frames at Half, shown and between in turn: every `miss`th shown
// frame late outside a probe, every `probe_miss`th frame late inside one
// (0: none). The frame count at the first change of mode (0: none).
int half(HiresGovernor& g, int n, int miss = 0, int probe_miss = 0) {
  int pn = 0;
  for (int i = 1; i <= n; ++i) {
    const bool shown = i % 2 == 1;
    bool late;
    if (g.probing()) late = probe_miss > 0 && ++pn % probe_miss == 0;
    else late = shown && miss > 0 && ((i + 1) / 2) % miss == 0;
    if (g.frame(late, shown)) return i;
  }
  return 0;
}

// Feeds Half until a probe begins; the frame count (0: none in `n`).
int until_probe(HiresGovernor& g, int n) {
  for (int i = 1; i <= n; ++i) {
    g.frame(false, i % 2 == 1);
    if (g.probing()) return i;
  }
  return 0;
}

HiresGovernor make(bool half_ok, bool fallback = true) {
  HiresGovernor g;
  g.set_half(half_ok);
  g.set_fallback(fallback);
  return g;
}

void test_off() {
  HiresGovernor g = make(false, false);
  CHECK(full(g, 1000, 1) == 0 && g.mode() == Mode::Full);
}

void test_to_half() {
  // Frameskip on: runs of frames left out, 11 in two seconds hold every frame, 12 halve it.
  HiresGovernor a = make(true);
  CHECK(full(a, 6000, 1, 11) == 0 && a.mode() == Mode::Full);
  HiresGovernor b = make(true);
  CHECK(full(b, 240, 1, 10) == 120 && b.mode() == Mode::Half && b.hires());
  // Nothing is judged before two seconds at Full.
  HiresGovernor c = make(true);
  CHECK(full(c, 119, 1, 2) == 0);
  // A stall of a second every ten (60 frames in one run) is not the GPU short.
  HiresGovernor e = make(true);
  CHECK(full(e, 6000, 60, 600) == 0 && e.mode() == Mode::Full);
  // A quarter left out (Golden Sun: Dark Dawn's field at 2x): Half, not 1x.
  HiresGovernor d = make(true);
  CHECK(full(d, 240, 1, 4) == 120 && d.mode() == Mode::Half);
}

void test_low_without_frameskip() {
  // Frameskip off: 42 runs in two seconds to 1x (every third frame, 40, holds); and 1x stays.
  HiresGovernor a = make(false);
  CHECK(full(a, 6000, 1, 3) == 0 && a.mode() == Mode::Full);
  HiresGovernor b = make(false);
  CHECK(full(b, 240, 1, 2) == 120 && b.mode() == Mode::Low && !b.hires());
  CHECK(full(b, 100000) == 0 && b.mode() == Mode::Low);
  // A quarter left out without frameskip stays at every frame (as before).
  HiresGovernor c = make(false);
  CHECK(full(c, 6000, 1, 4) == 0);
  HiresGovernor e = make(false);
  CHECK(full(e, 6000, 60, 600) == 0);
}

void test_half_to_low() {
  // At Half, a quarter of the last 120 shown frames not in time: 1x, until
  // the next launch.
  HiresGovernor g = make(true);
  CHECK(full(g, 120, 1, 2) == 120);
  CHECK(half(g, 239, 4) == 239 && g.mode() == Mode::Low);   // the 120th shown frame is the 30th late
  CHECK(full(g, 100000) == 0 && half(g, 100000) == 0 && g.mode() == Mode::Low);
  // 24 of 120 holds at Half; so does one stall of a second (29 shown frames in a row).
  HiresGovernor h = make(true);
  CHECK(full(h, 120, 1, 2) == 120 && half(h, 6000, 5, 1) == 0 && h.mode() == Mode::Half);
  HiresGovernor s = make(true);
  CHECK(full(s, 120, 1, 2) == 120);
  for (int i = 0; i < 58; ++i) CHECK(!s.frame(i % 2 == 0, i % 2 == 0));
  CHECK(half(s, 6000, 0, 1) == 0 && s.mode() == Mode::Half);
  // Without video.hires3d_fallback Half is as low as it goes.
  HiresGovernor n = make(true, false);
  CHECK(full(n, 120, 1, 2) == 120 && half(n, 6000, 1, 1) == 0 && n.mode() == Mode::Half);
}

void test_half_to_full() {
  // A probe 5 s after Half began; two seconds of it with every frame's work
  // in time: every frame again.
  HiresGovernor g = make(true);
  CHECK(full(g, 120, 1, 2) == 120 && g.mode() == Mode::Half && !g.probing());
  const int p = until_probe(g, 1000);
  CHECK(p >= 300 && p <= 302);
  CHECK(half(g, 1000) == 120 && g.mode() == Mode::Full);
  // One miss in a probe is allowed.
  HiresGovernor h = make(true);
  CHECK(full(h, 120, 1, 2) == 120 && until_probe(h, 1000) > 0 && half(h, 1000, 0, 100) == 120 && h.mode() == Mode::Full);
  // Frameskip turned off at Half: every frame again.
  HiresGovernor o = make(true);
  CHECK(full(o, 120, 1, 2) == 120);
  o.set_half(false);
  CHECK(o.mode() == Mode::Full);
}

void test_probe_fails() {
  // The second miss ends a probe, at Half: nothing shown changes, the next
  // probe waits twice as long, up to 60 s.
  HiresGovernor g = make(true);
  CHECK(full(g, 120, 1, 2) == 120);
  for (unsigned want : {600u, 1200u, 2400u, 3600u, 3600u}) {
    CHECK(until_probe(g, 10000) > 0);
    CHECK(half(g, 2, 0, 1) == 0 && !g.probing() && g.mode() == Mode::Half && g.wait_frames() == want);
  }
  CHECK(g.probes_failed() == 5);
  const int p = until_probe(g, 10000);
  CHECK(p >= 3600 && p <= 3602);
}

void test_backoff() {
  // Full falling back within 3 s of a probe's success doubles the wait too;
  // 30 s held at every frame starts it over.
  HiresGovernor g = make(true);
  CHECK(full(g, 120, 1, 2) == 120);
  CHECK(until_probe(g, 1000) > 0 && half(g, 1000) == 120 && g.mode() == Mode::Full);
  CHECK(full(g, 120, 1, 2) == 120 && g.mode() == Mode::Half && g.wait_frames() == 600);
  CHECK(until_probe(g, 1000) > 0 && half(g, 1000) == 120);
  CHECK(full(g, 1800) == 0 && full(g, 120, 1, 2) == 24 && g.wait_frames() == 300);   // the 12th run
  CHECK(g.halves() == 3);
}

} // namespace

int main() {
  test_off();
  test_to_half();
  test_low_without_frameskip();
  test_half_to_low();
  test_half_to_full();
  test_probe_fails();
  test_backoff();
  std::puts("hires_governor: ok");
  return 0;
}
