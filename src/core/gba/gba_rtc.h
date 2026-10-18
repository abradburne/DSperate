// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// The cartridge's GPIO port (0x080000C4 data, C6 direction, C8 readable)
// and the Seiko S-3511 real-time clock on it: SCK on pin 0, SIO on pin 1,
// CS on pin 2. Commands and data go LSB first, eight clocks a byte; the
// time is BCD (year, month, day, weekday, hour, minute, second), the host's
// local time plus whatever offset the game has set. Present when the ROM
// carries the SDK's "SIIRTC_V" string (Pokemon Ruby/Sapphire/Emerald, Boktai).
#pragma once
#include "core/types.h"

namespace ds::gba {

class Rtc {
public:
  bool present = false;
  void reset();

  // GPIO registers, by offset from 0x080000C4 (0, 2, 4).
  u16  read(u32 off) const;
  void write(u32 off, u16 v);
  bool readable() const { return present && (control_ & 1); }

  // Seconds added to the host clock (what the game set it to, minus then).
  s64 offset = 0;
  // Tests: a fixed time instead of the host's (seconds since the epoch, local), 0 = the host's.
  s64 fixed_now = 0;

  template <class S> void sync_state(S& s);

private:
  u8 data_ = 0, dir_ = 0, control_ = 0;
  u8 sio_out_ = 0;              // what the chip drives on SIO
  // The serial transfer.
  u8 step_ = 0;                 // 0 idle, 1 SCK high seen, 2 selected
  u8 bit_in_ = 0, nbits_ = 0, bits_ = 0;
  bool active_ = false, reading_ = false;
  u8 cmd_ = 0, nbytes_ = 0, byte_ = 0;
  u8 buf_[7] = {};
  u8 status_ = 0x40;            // S-3511 status: bit 6 = 24-hour mode
  void pins(u8 p);
  void command(u8 c);
  void finish_write();
  void latch_time();
  s64 now() const;
};

} // namespace ds::gba
