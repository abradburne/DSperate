// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/gba/gba_rtc.h"
#include "core/state/state.h"

#include <ctime>

namespace ds::gba {

namespace {
// Data bytes per command (0 reset, 2 date and time, 4 status, 6 time).
constexpr u8 BYTES[8] = {0, 0, 7, 0, 1, 0, 3, 0};
u8 bcd(int v) { return static_cast<u8>(((v / 10) << 4) | (v % 10)); }
int unbcd(u8 v) { return (v >> 4) * 10 + (v & 15); }
} // namespace

void Rtc::reset() {
  data_ = dir_ = control_ = 0;
  sio_out_ = 0;
  step_ = bit_in_ = nbits_ = bits_ = 0;
  active_ = reading_ = false;
  cmd_ = nbytes_ = byte_ = 0;
  status_ = 0x40;
}

s64 Rtc::now() const { return (fixed_now ? fixed_now : static_cast<s64>(std::time(nullptr))) + offset; }

u16 Rtc::read(u32 off) const {
  switch (off) {
  case 0: return static_cast<u16>(((data_ & dir_) | (!(dir_ & 2) ? sio_out_ << 1 : 0)) & 15);
  case 2: return dir_;
  case 4: return control_;
  default: return 0;
  }
}

void Rtc::write(u32 off, u16 v) {
  switch (off) {
  case 0: data_ = v & 15; pins(data_); break;
  case 2: dir_ = v & 15; break;
  case 4: control_ = v & 1; break;
  default: break;
  }
}

void Rtc::latch_time() {
  const std::time_t t = static_cast<std::time_t>(now());
  std::tm tm{};
  localtime_r(&t, &tm);
  const int h = tm.tm_hour;
  const u8 hour = static_cast<u8>(((status_ & 0x40) ? bcd(h) : bcd(h % 12)) | (h >= 12 ? 0x40 : 0));
  const u8 date[7] = {bcd(tm.tm_year % 100), bcd(tm.tm_mon + 1), bcd(tm.tm_mday), bcd(tm.tm_wday), hour, bcd(tm.tm_min), bcd(tm.tm_sec)};
  if (cmd_ == 2) for (int i = 0; i < 7; ++i) buf_[i] = date[i];
  else for (int i = 0; i < 3; ++i) buf_[i] = date[4 + i];
}

void Rtc::command(u8 c) {
  // LSB first: 0110 in the low nibble, the command in bits 4-6, read in bit 7.
  if ((c & 15) != 6) return;
  cmd_ = (c >> 4) & 7;
  reading_ = c & 0x80;
  nbytes_ = BYTES[cmd_];
  byte_ = 0;
  if (cmd_ == 0) { status_ = 0; offset = 0; return; }   // reset
  if (!nbytes_) return;
  active_ = true;
  if (reading_) {
    if (cmd_ == 4) buf_[0] = status_;
    else latch_time();
  }
}

void Rtc::finish_write() {
  active_ = false;
  if (cmd_ == 4) { status_ = buf_[0] & 0x6A; return; }
  std::tm tm{};
  const std::time_t t0 = static_cast<std::time_t>(now());
  localtime_r(&t0, &tm);
  if (cmd_ == 2) {
    tm.tm_year = 100 + unbcd(buf_[0]); tm.tm_mon = unbcd(buf_[1] & 0x1F) - 1; tm.tm_mday = unbcd(buf_[2] & 0x3F);
    tm.tm_hour = unbcd(buf_[4] & 0x3F); tm.tm_min = unbcd(buf_[5] & 0x7F); tm.tm_sec = unbcd(buf_[6] & 0x7F);
  } else {
    tm.tm_hour = unbcd(buf_[0] & 0x3F); tm.tm_min = unbcd(buf_[1] & 0x7F); tm.tm_sec = unbcd(buf_[2] & 0x7F);
  }
  tm.tm_isdst = -1;
  const s64 base = (fixed_now ? fixed_now : static_cast<s64>(std::time(nullptr)));
  offset = static_cast<s64>(std::mktime(&tm)) - base;
}

// What the game drives on the pins, each write. Selection: SCK high with CS
// low, then CS high. A bit: SCK low (the game puts it on SIO, or the chip
// does when reading), then SCK high clocks it. CS low ends the transfer.
void Rtc::pins(u8 p) {
  const bool sck = p & 1, cs = p & 4;
  switch (step_) {
  case 0: if (sck && !cs) step_ = 1; break;
  case 1: if (sck && cs) step_ = 2; else if (!(sck && !cs)) step_ = 0; break;
  default:
    if (!cs) {   // deselected
      step_ = (sck && !cs) ? 1 : 0;
      active_ = false; nbits_ = 0; bits_ = 0;
      break;
    }
    if (!sck) {
      if (active_ && reading_) sio_out_ = (buf_[byte_] >> nbits_) & 1;
      else bit_in_ = (p >> 1) & 1;
      break;
    }
    // SCK rising.
    if (active_ && reading_) {
      if (++nbits_ == 8) { nbits_ = 0; if (++byte_ >= nbytes_) active_ = false; }
      break;
    }
    bits_ = static_cast<u8>(bits_ | (bit_in_ << nbits_));
    if (++nbits_ < 8) break;
    if (!active_) command(bits_);
    else { buf_[byte_] = bits_; if (++byte_ >= nbytes_) finish_write(); }
    nbits_ = 0; bits_ = 0;
    break;
  }
}

template <class S> void Rtc::sync_state(S& s) {
  s.begin("GRTC");
  s.fields(present, offset, data_, dir_, control_, sio_out_, step_, bit_in_, nbits_, bits_, active_, reading_, cmd_, nbytes_, byte_, buf_, status_);
  s.end();
}
template void Rtc::sync_state<state::Writer>(state::Writer&);
template void Rtc::sync_state<state::Reader>(state::Reader&);

} // namespace ds::gba
