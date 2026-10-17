// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// GBA sound: the Game Boy's four PSG channels (core/psg, the AGB model) and
// the two Direct Sound channels, 8-bit signed samples fed through 32-byte
// FIFOs that timer 0 or 1 overflows pop and DMA 1/2 refill, mixed as the
// GBA's 10-bit DAC does around SOUNDBIAS. One stereo frame every 512 cycles
// (32768 Hz), handed to the frontend through NDS's output ring.
#pragma once
#include "core/types.h"
#include "core/psg/gb_psg.h"

#include <vector>

namespace ds::gba {

struct Gba;

class Audio {
public:
  explicit Audio(Gba& g) : g_(g), psg_(psg::GbPsg::Model::Agb) {}
  void reset();
  static constexpr u32 SAMPLE_CYCLES = 512;            // 16777216 / 512 = 32768 Hz
  static constexpr u32 SAMPLE_RATE = 32768;

  // I/O 0x060-0x0AF, a halfword at a time; `mask` says which bytes are written.
  u16  read16(u32 off) const;
  void write16(u32 off, u16 v, u16 mask);
  // Timer 0/1 overflowed at `at`: the FIFOs that timer clocks take their next sample.
  void timer_overflow(int timer, u64 at);
  // The sample event (every SAMPLE_CYCLES).
  void sample(u64 at);

  std::vector<s16> out;   // stereo frames since the frontend last took them

  template <class S> void sync_state(S& s);

private:
  Gba& g_;
  psg::GbPsg psg_;
  u64 psg_at_ = 0;        // GBA time the PSG has run to
  struct Fifo { u8 buf[32] = {}; u32 rd = 0, count = 0; s8 cur = 0; };
  Fifo fifo_[2];
  u16 cnt_h_ = 0, bias_ = 0x200;
  s32 hp_x_[2] = {}, hp_y_[2] = {};   // the output high-pass (sample())
  void catch_up(u64 now);
  void fifo_push(int f, u8 b);
};

} // namespace ds::gba
