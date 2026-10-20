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
  // 35% of two seconds: 41 of 120 left out holds 2x, 42 drops it. A
  // quarter (Golden Sun: Dark Dawn's field at 2x) stays at 2x.
  HiresGovernor a;
  CHECK(feed(a, 6000, 41, 120) == 0 && a.hires());
  HiresGovernor q;
  CHECK(feed(q, 6000, 1, 4) == 0 && q.hires());
  HiresGovernor b;
  CHECK(feed(b, 240, 42, 120) == 120 && !b.hires() && b.drops() == 1);
  // Nothing is judged before two seconds have passed at 2x.
  HiresGovernor c;
  CHECK(feed(c, 119, 1) == 0 && c.hires());
}

void test_backoff() {
  HiresGovernor g;
  CHECK(feed(g, 120, 1) == 120 && !g.hires());
  CHECK(g.wait_frames() == 600);              // the first drop does not double the wait
  CHECK(feed(g, 600) == 600 && g.hires());    // 2x again after 10 s
  // Dropping again within 3 s of coming back doubles the wait.
  CHECK(feed(g, 120, 1) == 120 && !g.hires() && g.wait_frames() == 1200);
  CHECK(feed(g, 1200) == 1200 && g.hires());
  // 2x held for 30 s: the wait starts over at 10 s.
  CHECK(feed(g, 1800) == 0 && g.hires());
  CHECK(g.wait_frames() == 600);
  // A drop after more than 3 s at 2x keeps the wait as it is; with two
  // seconds at 2x behind it, 42 frames left out in a row are 35%.
  CHECK(feed(g, 120, 1) == 42 && g.wait_frames() == 600);
  CHECK(feed(g, 600) == 600);
  CHECK(feed(g, 300) == 0);                   // 5 s at 2x, then behind again
  CHECK(feed(g, 120, 1) == 42 && g.wait_frames() == 600);
}

void test_gives_up() {
  // Two quick drops in a row (each within 3 s of coming back): 1x for good,
  // not 2x every 20-80 s for a few seconds.
  HiresGovernor g;
  CHECK(feed(g, 120, 1) == 120 && !g.hires());
  CHECK(feed(g, 600) == 600 && g.hires());
  CHECK(feed(g, 120, 1) == 120 && g.wait_frames() == 1200);
  CHECK(feed(g, 1200) == 1200 && g.hires());
  CHECK(feed(g, 120, 1) == 120 && !g.hires() && g.drops() == 3);
  CHECK(feed(g, 100000) == 0 && !g.hires());
  // One quick drop, then 30 s held: the count starts over.
  HiresGovernor h;
  CHECK(feed(h, 120, 1) == 120 && feed(h, 600) == 600);
  CHECK(feed(h, 120, 1) == 120 && feed(h, 1200) == 1200);   // quick drop 1
  CHECK(feed(h, 1800) == 0 && h.hires());                    // held 30 s
  CHECK(feed(h, 120, 1) == 42 && feed(h, 600) == 600);       // a slow drop, back
  CHECK(feed(h, 120, 1) == 120 && feed(h, 1200) == 1200 && h.hires());   // quick drop 1 again: still retried
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
  test_gives_up();
  std::puts("hires_governor: ok");
  return 0;
}
