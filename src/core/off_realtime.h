// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
// A thread off the real-time class for a while. Under the RLIMIT_RTTIME that
// PipeWire sets (frontend/sdl/rt_budget.h), the kernel kills a real-time thread
// that runs 200 ms without sleeping; work that may run that long on the
// emulation thread (a zipped ROM inflating, a state, RetroAchievements digesting
// a big set) runs under this guard. Linux only; elsewhere it does nothing.
#include <cerrno>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#include <time.h>
#endif

namespace ds {

// For as long as it lives, the calling thread is an ordinary task: for
// one-shot work that may run longer than RLIMIT_RTTIME allows without a
// sleep (a big zipped ROM inflates for longer than 200 ms on a Cortex-A55).
// Threads started meanwhile inherit SCHED_OTHER too. On the way out the real-time
// policy comes back and the thread naps once as a real-time thread, so the
// count starts from nothing. Nests: an inner guard finds nothing to drop.
class OffRealtime {
public:
  OffRealtime() {
#if defined(__linux__)
    if (pthread_getschedparam(pthread_self(), &policy_, &param_) != 0 || (policy_ != SCHED_RR && policy_ != SCHED_FIFO)) { policy_ = SCHED_OTHER; return; }
    sched_param z{};
    if (pthread_setschedparam(pthread_self(), SCHED_OTHER, &z) != 0) policy_ = SCHED_OTHER;
#endif
  }
  ~OffRealtime() {
#if defined(__linux__)
    if (policy_ == SCHED_OTHER) return;
    if (pthread_setschedparam(pthread_self(), policy_, &param_) != 0) return;
    timespec ts{0, 20000};
    while (clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, &ts) == EINTR) {}
#endif
  }
  OffRealtime(const OffRealtime&) = delete;
  OffRealtime& operator=(const OffRealtime&) = delete;

private:
#if defined(__linux__)
  int policy_ = SCHED_OTHER;
  sched_param param_{};
#endif
};

} // namespace ds
