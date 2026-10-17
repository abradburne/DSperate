// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/psg/gb_psg.h"
#include "core/state/state.h"

#include <algorithm>

namespace ds::psg {

namespace {
constexpr u8 DUTY[4][8] = {{0, 0, 0, 0, 0, 0, 0, 1}, {1, 0, 0, 0, 0, 0, 0, 1}, {1, 0, 0, 0, 0, 1, 1, 1}, {0, 1, 1, 1, 1, 1, 1, 0}};
constexpr u32 NOISE_DIV[8] = {8, 16, 32, 48, 64, 80, 96, 112};
// Bits that read back as 1 on a DMG/CGB (write-only or unused); 0 on the AGB.
constexpr u8 READ_OR[0x17] = {0x80, 0x3F, 0x00, 0xFF, 0xBF, 0xFF, 0x3F, 0x00, 0xFF, 0xBF, 0x7F, 0xFF, 0x9F, 0xFF, 0xBF,
                              0xFF, 0xFF, 0x00, 0x00, 0xBF, 0x00, 0x00, 0x70};
// Each channel's registers: length/duty, envelope, frequency low, frequency high/control.
constexpr u8 NRX1[4] = {0x11, 0x16, 0x1B, 0x20}, NRX2[4] = {0x12, 0x17, 0x1C, 0x21},
             NRX3[4] = {0x13, 0x18, 0x1D, 0x22}, NRX4[4] = {0x14, 0x19, 0x1E, 0x23};
} // namespace

void GbPsg::reset() {
  ch_ = {};
  regs_.fill(0);
  wave_.fill(0);
  nr52_ = 0;
  seq_timer_ = 8192; seq_step_ = 0;
  sweep_shadow_ = sweep_timer_ = 0; sweep_on_ = sweep_negated_ = false;
  wave_sample_ = 0;
  acc_cycles_ = 0;
}

void GbPsg::power_off() {
  for (u8 r = 0x10; r < 0x26; ++r) regs_[r - 0x10] = 0;
  for (Channel& c : ch_) { const s32 acc = c.acc; c = Channel{}; c.acc = acc; }
  sweep_on_ = false;
}

s32 GbPsg::period(int c) const {
  if (c == 3) {
    const u8 v = reg(0x22);
    const u32 s = v >> 4;
    if (s >= 14) return 0x7FFFFFFF;   // the LFSR stops
    return static_cast<s32>(NOISE_DIV[v & 7] << s);
  }
  const u32 f = reg(NRX3[c]) | ((reg(NRX4[c]) & 7u) << 8);
  return static_cast<s32>((2048 - f) * (c == 2 ? 2u : 4u));
}

s32 GbPsg::level(int c) const {
  const Channel& ch = ch_[c];
  if (!ch.on || !ch.dac) return 0;
  u32 d;
  switch (c) {
  case 0: case 1: d = DUTY[reg(NRX1[c]) >> 6][ch.pos] ? ch.volume : 0; break;
  case 2: {
    const u8 v = reg(0x1C);
    if (model_ == Model::Agb && (v & 0x80)) d = wave_sample_ * 3 / 4;
    else { const u32 code = (v >> 5) & 3; d = code ? wave_sample_ >> (code - 1) : 0; }
    break;
  }
  default: d = (ch.lfsr & 1) ? 0 : ch.volume; break;
  }
  return static_cast<s32>(d) * 2 - 15;
}

void GbPsg::advance(int c, u32 cycles) {
  Channel& ch = ch_[c];
  if (!ch.on) { ch.acc += level(c) * static_cast<s32>(cycles); return; }
  s32 rem = static_cast<s32>(cycles);
  while (rem > 0) {
    const s32 d = std::min(rem, ch.timer);
    ch.acc += level(c) * d;
    ch.timer -= d; rem -= d;
    if (ch.timer > 0) break;
    ch.timer = period(c);
    switch (c) {
    case 0: case 1: ch.pos = (ch.pos + 1) & 7; break;
    case 2: {
      const bool agb = model_ == Model::Agb;
      const u8 v = reg(0x1A);
      ch.pos = (ch.pos + 1) & ((agb && (v & 0x20)) ? 63 : 31);
      const u32 bank = agb ? (((v >> 6) & 1) ^ (ch.pos >> 5)) : 0;
      const u8 b = wave_[bank * 16 + ((ch.pos & 31) >> 1)];
      wave_sample_ = (ch.pos & 1) ? (b & 15u) : (b >> 4);
      break;
    }
    default: {
      const u32 x = (ch.lfsr ^ (ch.lfsr >> 1)) & 1;
      ch.lfsr = (ch.lfsr >> 1) | (x << 14);
      if (reg(0x22) & 8) ch.lfsr = (ch.lfsr & ~0x40u) | (x << 6);
      break;
    }
    }
  }
}

u32 GbPsg::sweep_next() {
  const u8 v = reg(0x10);
  const u32 delta = sweep_shadow_ >> (v & 7);
  u32 f;
  if (v & 8) { f = sweep_shadow_ - delta; sweep_negated_ = true; }
  else f = sweep_shadow_ + delta;
  if (f > 2047) ch_[0].on = false;
  return f;
}

void GbPsg::step_sequencer() {
  const u32 step = seq_step_;
  seq_step_ = (seq_step_ + 1) & 7;
  if (!(step & 1)) {   // length, 256 Hz
    for (Channel& c : ch_)
      if (c.length_on && c.length && --c.length == 0) c.on = false;
  }
  if (step == 2 || step == 6) {   // sweep, 128 Hz
    if (sweep_timer_ && --sweep_timer_ == 0) {
      const u8 v = reg(0x10);
      const u32 p = (v >> 4) & 7;
      sweep_timer_ = p ? p : 8;
      if (sweep_on_ && p) {
        const u32 f = sweep_next();
        if (f <= 2047 && (v & 7)) {
          sweep_shadow_ = f;
          regs_[0x13 - 0x10] = static_cast<u8>(f);
          regs_[0x14 - 0x10] = static_cast<u8>((regs_[0x14 - 0x10] & ~7u) | (f >> 8));
          sweep_next();
        }
      }
    }
  }
  if (step == 7) {   // envelopes, 64 Hz
    for (int c : {0, 1, 3}) {
      Channel& ch = ch_[c];
      if (!ch.env_period) continue;
      if (--ch.env_timer) continue;
      ch.env_timer = ch.env_period;
      if (ch.env_up && ch.volume < 15) ++ch.volume;
      else if (!ch.env_up && ch.volume > 0) --ch.volume;
    }
  }
}

void GbPsg::trigger(int c) {
  Channel& ch = ch_[c];
  ch.on = ch.dac;
  if (!ch.length) ch.length = c == 2 ? 256 : 64;
  ch.timer = period(c);
  if (c != 2) {
    const u8 e = reg(NRX2[c]);
    ch.volume = e >> 4; ch.env_up = e & 8; ch.env_period = e & 7; ch.env_timer = ch.env_period;
  }
  if (c == 2) ch.pos = 0;
  if (c == 3) ch.lfsr = 0x7FFF;
  if (c == 0) {
    const u8 v = reg(0x10);
    sweep_shadow_ = reg(0x13) | ((reg(0x14) & 7u) << 8);
    const u32 p = (v >> 4) & 7;
    sweep_timer_ = p ? p : 8;
    sweep_on_ = p || (v & 7);
    sweep_negated_ = false;
    if (v & 7) sweep_next();
  }
}

void GbPsg::write(u8 r, u8 v) {
  if (r < 0x10 || r > 0x26) return;
  if (r == 0x26) {
    const bool was = on();
    nr52_ = v & 0x80;
    if (was && !on()) power_off();
    if (!was && on()) { seq_step_ = 0; seq_timer_ = 8192; }
    return;
  }
  if (!on()) return;
  regs_[r - 0x10] = v;
  switch (r) {
  case 0x11: case 0x16: ch_[r == 0x11 ? 0 : 1].length = 64 - (v & 63u); break;
  case 0x1B: ch_[2].length = 256 - v; break;
  case 0x20: ch_[3].length = 64 - (v & 63u); break;
  case 0x12: case 0x17: case 0x21: {
    Channel& ch = ch_[r == 0x12 ? 0 : r == 0x17 ? 1 : 3];
    ch.dac = (v & 0xF8) != 0;
    if (!ch.dac) ch.on = false;
    break;
  }
  case 0x1A: ch_[2].dac = v & 0x80; if (!ch_[2].dac) ch_[2].on = false; break;
  case 0x14: case 0x19: case 0x1E: case 0x23: {
    const int c = r == 0x14 ? 0 : r == 0x19 ? 1 : r == 0x1E ? 2 : 3;
    ch_[c].length_on = v & 0x40;
    if (v & 0x80) trigger(c);
    break;
  }
  default: break;
  }
}

u8 GbPsg::read(u8 r) const {
  if (r < 0x10 || r > 0x26) return 0xFF;
  if (r == 0x26) {
    u8 v = nr52_ & 0x80;
    for (int c = 0; c < 4; ++c) if (ch_[c].on) v |= static_cast<u8>(1u << c);
    return model_ == Model::Agb ? v : static_cast<u8>(v | 0x70);
  }
  const u8 m = READ_OR[r - 0x10];
  return model_ == Model::Agb ? static_cast<u8>(reg(r) & ~m) : static_cast<u8>(reg(r) | m);
}

u8 GbPsg::wave_read(u32 i) const {
  const u32 bank = model_ == Model::Agb ? (((reg(0x1A) >> 6) & 1) ^ 1) : 0;
  return wave_[bank * 16 + (i & 15)];
}
void GbPsg::wave_write(u32 i, u8 v) {
  const u32 bank = model_ == Model::Agb ? (((reg(0x1A) >> 6) & 1) ^ 1) : 0;
  wave_[bank * 16 + (i & 15)] = v;
}

void GbPsg::run(u32 cycles) {
  while (cycles) {
    const u32 n = std::min(cycles, seq_timer_);
    for (int c = 0; c < 4; ++c) advance(c, n);
    acc_cycles_ += n;
    cycles -= n;
    if (!on()) continue;   // the sequencer stops with the chip
    seq_timer_ -= n;
    if (!seq_timer_) { seq_timer_ = 8192; step_sequencer(); }
  }
}

void GbPsg::sample(s32 out[4]) {
  for (int c = 0; c < 4; ++c) {
    out[c] = acc_cycles_ ? ch_[c].acc / static_cast<s32>(acc_cycles_) : level(c);
    ch_[c].acc = 0;
  }
  acc_cycles_ = 0;
}

void GbPsg::mix(const s32 lv[4], s32& left, s32& right) const {
  const u8 nr50 = reg(0x24), nr51 = reg(0x25);
  s32 l = 0, r = 0;
  for (int c = 0; c < 4; ++c) {
    if (nr51 & (0x10 << c)) l += lv[c];
    if (nr51 & (0x01 << c)) r += lv[c];
  }
  left = l * static_cast<s32>(((nr50 >> 4) & 7) + 1);
  right = r * static_cast<s32>((nr50 & 7) + 1);
}

template <class S> void GbPsg::sync_state(S& s) {
  s.begin("GPSG");
  s.fields(regs_, wave_, nr52_, seq_timer_, seq_step_, sweep_shadow_, sweep_timer_, sweep_on_, sweep_negated_, wave_sample_, acc_cycles_);
  for (Channel& c : ch_)
    s.fields(c.on, c.dac, c.length, c.length_on, c.timer, c.pos, c.volume, c.env_period, c.env_timer, c.env_up, c.lfsr, c.acc);
  s.end();
}
template void GbPsg::sync_state<state::Writer>(state::Writer&);
template void GbPsg::sync_state<state::Reader>(state::Reader&);

} // namespace ds::psg
