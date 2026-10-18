// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// video.hires3d_fallback (frontend/sdl/hires_governor.h): when hi-res 3D
// drops to 1x and when it comes back, frame by frame.
#include "check.h"
#include "frontend/sdl/hires_governor.h"

#include <cstdio>
#include <initializer_list>

using ds::sdl::HiresGovernor;

namespace {

// Feeds `n` frames, `late` of every `of` left out; the frame count at the
// first switch (0: none).
int feed(HiresGovernor& g, int n, int late = 0, int of = 1) {
  for (int i = 1; i <= n; ++i)
    if (g.frame(i % of < late)) return i;
  return 0;
}

void test_off() {
  HiresGovernor g;
  g.set_enabled(false);
  CHECK(feed(g, 1000, 1) == 0);
  CHECK(g.hires());
}

void test_threshold() {
  // 15% of two seconds: 17 of 120 left out holds 2x, 18 drops it.
  HiresGovernor a;
  CHECK(feed(a, 6000, 17, 120) == 0 && a.hires());
  HiresGovernor b;
  CHECK(feed(b, 240, 18, 120) == 120 && !b.hires() && b.drops() == 1);
  // Nothing is judged before two seconds have passed at 2x.
  HiresGovernor c;
  CHECK(feed(c, 119, 1) == 0 && c.hires());
}

void test_backoff() {
  HiresGovernor g;
  CHECK(feed(g, 120, 1) == 120 && !g.hires());
  CHECK(g.wait_frames() == 600);              // the first drop does not double the wait
  CHECK(feed(g, 600) == 600 && g.hires());    // 2x again after 10 s
  // Dropping again within 3 s of coming back doubles the wait, up to 80 s.
  for (unsigned want : {1200u, 2400u, 4800u, 4800u}) {
    CHECK(feed(g, 120, 1) == 120 && !g.hires());
    CHECK(g.wait_frames() == want);
    CHECK(feed(g, static_cast<int>(want)) == static_cast<int>(want) && g.hires());
  }
  // 2x held for 30 s: the wait starts over at 10 s.
  CHECK(feed(g, 1800) == 0 && g.hires());
  CHECK(g.wait_frames() == 600);
  // A drop after more than 3 s at 2x keeps the wait as it is; with two
  // seconds at 2x behind it, 18 frames left out in a row are 15%.
  CHECK(feed(g, 120, 1) == 18 && g.wait_frames() == 600);
  CHECK(feed(g, 600) == 600);
  CHECK(feed(g, 300) == 0);                   // 5 s at 2x, then behind again
  CHECK(feed(g, 120, 1) == 18 && g.wait_frames() == 600);
}

void test_window_clears() {
  // Frames left out before a return to 2x do not count against it.
  HiresGovernor g;
  CHECK(feed(g, 120, 1) == 120);
  CHECK(feed(g, 600) == 600 && g.hires());
  CHECK(feed(g, 119, 1) == 0 && g.hires());   // a new window of two seconds
}

} // namespace

int main() {
  test_off();
  test_threshold();
  test_backoff();
  test_window_clears();
  std::puts("hires_governor: ok");
  return 0;
}
