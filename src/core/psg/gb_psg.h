// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// The Game Boy's sound chip: two square channels (the first with a frequency
// sweep), a 4-bit wave channel and a noise channel, with length counters,
// volume envelopes and the 512 Hz frame sequencer. A component of its own,
// addressed by the Game Boy's register numbers (NR10 = 0x10 ... NR52 = 0x26)
// and clocked in Game Boy master cycles (4194304 Hz), so any machine can carry
// it: the GBA maps its 0x04000060-0x0400009F onto it (core/gba/gba_audio.cpp);
// a Game Boy / Color machine would map 0xFF10-0xFF3F.
//
// The AGB model adds what the GBA changed: two 16-byte wave banks (NR30 bit 5
// plays both as one 64-sample wave, bit 6 picks the bank playing; the CPU
// sees the other one), a forced-75% wave volume (NR32 bit 7), and reads of
// write-only bits as 0 rather than 1.
//
// Output: each channel's DAC level, signed (-15..15 for a 0..15 digital level,
// 0 while its DAC is off), averaged over the span since the last sample()
// so tones above half the sample rate don't alias as hard.
#pragma once
#include "core/types.h"

#include <array>

namespace ds::psg {

class GbPsg {
public:
  enum class Model : u8 { Dmg, Cgb, Agb };
  static constexpr u32 CLOCK_HZ = 4'194'304;

  explicit GbPsg(Model m = Model::Dmg) : model_(m) { reset(); }
  void reset();
  Model model() const { return model_; }

  // Registers by Game Boy number, 0x10-0x26. Writes are dropped while the
  // chip is off (NR52 bit 7 clear), except NR52 itself (and, on a DMG, the
  // length counters).
  u8   read(u8 reg) const;
  void write(u8 reg, u8 value);
  // Wave RAM as the CPU sees it, 16 bytes (on the AGB: the bank not playing).
  u8   wave_read(u32 i) const;
  void wave_write(u32 i, u8 v);

  // Advance by Game Boy master cycles (4.19 MHz).
  void run(u32 cycles);
  bool on() const { return nr52_ & 0x80; }

  // Each channel's level averaged since the previous call (-15..15).
  void sample(s32 out[4]);
  // NR50/NR51 applied to `levels` as the Game Boy's own mixer does: sums of
  // the channels sent each way, times the master volume + 1 (-480..480).
  void mix(const s32 levels[4], s32& left, s32& right) const;

  template <class S> void sync_state(S& s);

private:
  struct Channel {
    bool on = false, dac = false;
    u32  length = 0;          // length steps left (of 64, 256 for the wave channel)
    bool length_on = false;
    s32  timer = 0;           // master cycles to the next step
    u32  pos = 0;             // duty step / wave sample
    u32  volume = 0, env_period = 0, env_timer = 0;
    bool env_up = false;
    u32  lfsr = 0;            // noise
    s32  acc = 0;             // level x cycles since sample()
  };
  std::array<Channel, 4> ch_{};
  std::array<u8, 0x17> regs_{};   // 0x10-0x26 as written
  std::array<u8, 32> wave_{};     // two 16-byte banks (one on a DMG/CGB)
  u8 nr52_ = 0;
  u32 seq_timer_ = 8192, seq_step_ = 0;
  // Channel 1's sweep.
  u32 sweep_shadow_ = 0, sweep_timer_ = 0;
  bool sweep_on_ = false, sweep_negated_ = false;
  u32 wave_sample_ = 0;           // the 4-bit sample the wave channel outputs
  u32 acc_cycles_ = 0;
  Model model_;

  u8 reg(u8 r) const { return regs_[r - 0x10]; }
  s32 level(int c) const;
  void step_sequencer();
  void trigger(int c);
  u32  sweep_next();
  void power_off();
  s32  period(int c) const;
  void advance(int c, u32 cycles);
};

} // namespace ds::psg
