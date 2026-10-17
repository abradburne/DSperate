// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/gba/gba_audio.h"
#include "core/gba/gba.h"
#include "core/state/state.h"

#include <algorithm>

namespace ds::gba {

namespace {
// GBA I/O byte (0x060-0x087) -> Game Boy register number, 0 where none.
constexpr u8 NR_OF[0x28] = {
  0x10, 0, 0x11, 0x12, 0x13, 0x14, 0, 0,   // 060 SOUND1CNT_L, _H, _X
  0x16, 0x17, 0, 0, 0x18, 0x19, 0, 0,      // 068 SOUND2CNT_L, _H
  0x1A, 0, 0x1B, 0x1C, 0x1D, 0x1E, 0, 0,   // 070 SOUND3CNT_L, _H, _X
  0x20, 0x21, 0, 0, 0x22, 0x23, 0, 0,      // 078 SOUND4CNT_L, _H
  0x24, 0x25, 0, 0, 0x26, 0, 0, 0,         // 080 SOUNDCNT_L, (_H), _X
};
} // namespace

void Audio::reset() {
  psg_.reset();
  psg_at_ = 0;
  fifo_[0] = Fifo{}; fifo_[1] = Fifo{};
  cnt_h_ = 0; bias_ = 0x200;
  hp_x_[0] = hp_x_[1] = 0x200 << 5; hp_y_[0] = hp_y_[1] = 0;
  out.clear();
}

void Audio::catch_up(u64 now) {
  if (now <= psg_at_) return;
  const u64 n = (now - psg_at_) >> 2;   // GBA cycles -> Game Boy cycles
  psg_.run(static_cast<u32>(n));
  psg_at_ += n << 2;
}

u16 Audio::read16(u32 off) const {
  if (off < 0x88) {
    if (off == 0x82) return cnt_h_ & 0x770F;
    const u32 i = off - 0x60;
    u16 v = 0;
    if (NR_OF[i]) v |= psg_.read(NR_OF[i]);
    if (NR_OF[i + 1]) v |= static_cast<u16>(psg_.read(NR_OF[i + 1]) << 8);
    return v;
  }
  if (off == 0x88) return bias_;
  if (off >= 0x90 && off < 0xA0) return static_cast<u16>(psg_.wave_read(off - 0x90) | (psg_.wave_read(off - 0x8F) << 8));
  return 0;
}

void Audio::fifo_push(int f, u8 b) {
  Fifo& q = fifo_[f];
  if (q.count == 32) return;   // full: dropped
  q.buf[(q.rd + q.count) & 31] = b;
  ++q.count;
}

void Audio::write16(u32 off, u16 v, u16 mask) {
  if (off >= 0xA0 && off < 0xA8) {   // FIFO A / B, little-endian bytes in order
    const int f = off >= 0xA4;
    if (mask & 0x00FF) fifo_push(f, static_cast<u8>(v));
    if (mask & 0xFF00) fifo_push(f, static_cast<u8>(v >> 8));
    return;
  }
  if (off == 0x82) {
    cnt_h_ = static_cast<u16>((cnt_h_ & ~mask) | (v & mask));
    if (cnt_h_ & 0x0800) fifo_[0] = Fifo{};
    if (cnt_h_ & 0x8000) fifo_[1] = Fifo{};
    cnt_h_ &= 0x770F;
    return;
  }
  if (off == 0x88) { bias_ = static_cast<u16>(((bias_ & ~mask) | (v & mask)) & 0xC3FE); return; }
  catch_up(g_.now());
  if (off >= 0x90 && off < 0xA0) {
    if (mask & 0x00FF) psg_.wave_write(off - 0x90, static_cast<u8>(v));
    if (mask & 0xFF00) psg_.wave_write(off - 0x8F, static_cast<u8>(v >> 8));
    return;
  }
  if (off >= 0x88) return;
  const u32 i = off - 0x60;
  if ((mask & 0x00FF) && NR_OF[i]) psg_.write(NR_OF[i], static_cast<u8>(v));
  if ((mask & 0xFF00) && NR_OF[i + 1]) psg_.write(NR_OF[i + 1], static_cast<u8>(v >> 8));
}

void Audio::timer_overflow(int timer, u64 at) {
  for (int f = 0; f < 2; ++f) {
    if (((cnt_h_ >> (10 + 4 * f)) & 1) != static_cast<u32>(timer)) continue;
    if (!(cnt_h_ & (0x300 << (4 * f)))) continue;   // sent nowhere: not running
    Fifo& q = fifo_[f];
    if (q.count) { q.cur = static_cast<s8>(q.buf[q.rd]); q.rd = (q.rd + 1) & 31; --q.count; }
    if (q.count <= 16) g_.dma_fifo(f, at);
  }
}

void Audio::sample(u64 at) {
  catch_up(at);
  s32 lv[4], l = 0, r = 0;
  psg_.sample(lv);
  if (!psg_.on()) { out.push_back(0); out.push_back(0); return; }   // SOUNDCNT_X bit 7: everything off
  psg_.mix(lv, l, r);                                // -480..480
  static const int PSG_SHIFT[4] = {2, 1, 0, 0};      // 25%, 50%, 100%
  l >>= PSG_SHIFT[cnt_h_ & 3]; r >>= PSG_SHIFT[cnt_h_ & 3];
  for (int f = 0; f < 2; ++f) {
    const s32 d = fifo_[f].cur * ((cnt_h_ & (4 << f)) ? 4 : 2);   // 100% / 50%
    if (cnt_h_ & (0x200 << (4 * f))) l += d;
    if (cnt_h_ & (0x100 << (4 * f))) r += d;
  }
  // The 10-bit DAC: biased and clamped. The output's AC coupling then takes
  // the DC away (the bias, and the PSG's off-centre levels): a one-pole
  // high-pass at about 20 Hz.
  const s32 bias = bias_ & 0x3FE;
  const s32 in[2] = {std::clamp(l + bias, 0, 0x3FF) << 5, std::clamp(r + bias, 0, 0x3FF) << 5};
  for (int c = 0; c < 2; ++c) {
    hp_y_[c] = in[c] - hp_x_[c] + static_cast<s32>((static_cast<s64>(hp_y_[c]) * 32748 + 16384) >> 15);
    hp_x_[c] = in[c];
    out.push_back(static_cast<s16>(std::clamp(hp_y_[c], -32768, 32767)));
  }
}

template <class S> void Audio::sync_state(S& s) {
  s.begin("GAUD");
  s.fields(psg_at_, cnt_h_, bias_, hp_x_, hp_y_);
  for (Fifo& q : fifo_) s.fields(q.buf, q.rd, q.count, q.cur);
  s.end();
  psg_.sync_state(s);
}
template void Audio::sync_state<state::Writer>(state::Writer&);
template void Audio::sync_state<state::Reader>(state::Reader&);

} // namespace ds::gba
