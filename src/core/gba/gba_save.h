// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// GBA cartridge save chips: SRAM (32 KB), Flash (64 KB, or 128 KB in two
// banks) and EEPROM (512 B or 8 KB, bit-serial over DMA 3 at 0x0D000000).
// The type comes from the string Nintendo's SDK leaves in the ROM
// (SRAM_V, FLASH_V/FLASH512_V, FLASH1M_V, EEPROM_V); an EEPROM's size from
// the first DMA that addresses it (9/73 bits: 512 B, 17/81 bits: 8 KB).
//
// Save files are the chip's bytes, as mGBA (and so DSlot's earlier stand-in)
// writes them: EEPROM blocks of 8 bytes, the first bit out the top of byte 0.
#pragma once
#include "core/types.h"

#include <string>
#include <vector>

namespace ds::gba {

class Save {
public:
  enum class Type : u8 { None, Sram, Flash64, Flash128, Eeprom512, Eeprom8k, EepromUnsized };
  static const char* name(Type t);

  // From the ROM's strings; None when it has none (no save chip).
  static Type detect(const u8* rom, u32 size);
  void set_type(Type t);
  Type type() const { return type_; }
  bool eeprom() const { return type_ == Type::Eeprom512 || type_ == Type::Eeprom8k || type_ == Type::EepromUnsized; }

  void reset();   // command state only; the stored bytes stay

  // 0x0E000000-0x0FFFFFFF (SRAM, Flash): 8-bit bus.
  u8   read8(u32 addr);
  void write8(u32 addr, u8 v);
  // EEPROM, one bit per halfword access (bit 0).
  u16  eeprom_read();
  void eeprom_write(u16 v);
  // A DMA of `count` halfwords to the EEPROM sets its size if unknown.
  void eeprom_dma(u32 count);

  // The bytes a save file holds (empty for None), and loading one.
  std::vector<u8> data() const;
  bool load(const u8* p, size_t n);
  bool dirty = false;   // written since the frontend last saved (it clears this)
  u32 writes = 0;       // writes so far, for "has the game stopped saving"

  template <class S> void sync_state(S& s);

private:
  Type type_ = Type::None;
  std::vector<u8> mem_;
  // Flash command state.
  enum class Flash : u8 { Ready, Cmd1, Cmd2, Erase1, Erase2, Erase3, Write, Bank };
  Flash fstate_ = Flash::Ready;
  bool id_mode_ = false, erase_mode_ = false;
  u32 bank_ = 0;
  // EEPROM bit streams.
  u64 bits_ = 0;        // command bits in
  u32 nbits_ = 0;
  u32 read_addr_ = 0, read_pos_ = 0;   // read_pos_ < 68: a read is going out
  bool reading_ = false;
  u32 addr_bits() const { return type_ == Type::Eeprom8k ? 14 : 6; }
  u64 wdata_ = 0;
  void touch() { dirty = true; ++writes; }
};

} // namespace ds::gba
