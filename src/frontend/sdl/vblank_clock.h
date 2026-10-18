// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// The panels' vblanks as DRM timestamps them: DRM_IOCTL_WAIT_VBLANK with a
// relative sequence of 0 returns a CRTC's last vblank (count and
// CLOCK_MONOTONIC time) without waiting. Any process may ask, on its own
// file descriptor: no DRM master needed, nothing shown changes.
#pragma once
#include "core/types.h"

#include <cerrno>
#include <ctime>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace ds::sdl {

class VblankClock {
public:
  VblankClock() = default;
  VblankClock(const VblankClock&) = delete;
  VblankClock& operator=(const VblankClock&) = delete;
  ~VblankClock() { if (fd_ >= 0) ::close(fd_); }

  bool open(const char* node = "/dev/dri/card0") {
    if (fd_ < 0) fd_ = ::open(node, O_RDWR | O_CLOEXEC);
    return fd_ >= 0;
  }
  bool ok() const { return fd_ >= 0; }

  // CRTC `pipe`'s last vblank. False if DRM has none to give (CRTC off).
  bool last(int pipe, u32* seq, u64* ns) const { return ask(pipe, 0, false, seq, ns); }
  // Blocks until CRTC `pipe`'s vblank count reaches `target` (absolute).
  bool wait_for(int pipe, u32 target, u32* seq = nullptr, u64* ns = nullptr) const { return ask(pipe, target, true, seq, ns); }

  static u64 now_ns() {
    timespec t{};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return static_cast<u64>(t.tv_sec) * 1'000'000'000ull + static_cast<u64>(t.tv_nsec);
  }

private:
  // linux/include/uapi/drm/drm.h
  struct Request { unsigned type; unsigned sequence; unsigned long signal; };
  struct Reply { unsigned type; unsigned sequence; long tval_sec; long tval_usec; };
  union Wait { Request request; Reply reply; };
  static constexpr unsigned RELATIVE = 0x1, ABSOLUTE = 0x0, SECONDARY = 0x20000000, HIGH_CRTC_SHIFT = 1, HIGH_CRTC_MASK = 0x3e;
  static constexpr unsigned long IOCTL_WAIT_VBLANK = _IOWR('d', 0x3a, Wait);

  bool ask(int pipe, u32 seq_in, bool absolute, u32* seq, u64* ns) const {
    if (fd_ < 0) return false;
    Wait w{};
    const unsigned crtc = pipe == 1 ? SECONDARY : (static_cast<unsigned>(pipe) << HIGH_CRTC_SHIFT) & HIGH_CRTC_MASK;
    w.request.type = (absolute ? ABSOLUTE : RELATIVE) | crtc;
    w.request.sequence = seq_in;
    int r;
    do r = ioctl(fd_, IOCTL_WAIT_VBLANK, &w); while (r < 0 && errno == EINTR);
    if (r < 0) return false;
    if (seq) *seq = w.reply.sequence;
    if (ns) *ns = static_cast<u64>(w.reply.tval_sec) * 1'000'000'000ull + static_cast<u64>(w.reply.tval_usec) * 1000ull;
    return true;
  }

  int fd_ = -1;
};

} // namespace ds::sdl
