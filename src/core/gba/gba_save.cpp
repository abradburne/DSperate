// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/gba/gba_save.h"
#include "core/state/state.h"

#include <algorithm>
#include <cstring>

namespace ds::gba {

namespace {
// Flash IDs (manufacturer, device): Panasonic MN63F805MNP for 64 KB,
// Macronix MX29L010 for 128 KB (what mGBA answers; Pokemon accepts it).
constexpr u8 ID64[2] = {0x32, 0x1B}, ID128[2] = {0xC2, 0x09};

bool has(const u8* rom, u32 size, const char* s) {
  const size_t n = std::strlen(s);
  for (u32 i = 0; i + n <= size; i += 4)   // the SDK's strings are word-aligned
    if (rom[i] == static_cast<u8>(s[0]) && std::memcmp(rom + i, s, n) == 0) return true;
  return false;
}
u32 type_size(Save::Type t) {
  switch (t) {
  case Save::Type::Sram: return 0x8000;
  case Save::Type::Flash64: return 0x10000;
  case Save::Type::Flash128: return 0x20000;
  case Save::Type::Eeprom512: return 512;
  case Save::Type::Eeprom8k: case Save::Type::EepromUnsized: return 8192;
  default: return 0;
  }
}
} // namespace

const char* Save::name(Type t) {
  switch (t) {
  case Type::None: return "none";
  case Type::Sram: return "SRAM 32 KB";
  case Type::Flash64: return "Flash 64 KB";
  case Type::Flash128: return "Flash 128 KB";
  case Type::Eeprom512: return "EEPROM 512 B";
  case Type::Eeprom8k: return "EEPROM 8 KB";
  case Type::EepromUnsized: return "EEPROM (size set by the game's first access)";
  }
  return "?";
}

Save::Type Save::detect(const u8* rom, u32 size) {
  if (has(rom, size, "FLASH1M_V")) return Type::Flash128;
  if (has(rom, size, "FLASH512_V") || has(rom, size, "FLASH_V")) return Type::Flash64;
  if (has(rom, size, "EEPROM_V")) return Type::EepromUnsized;
  if (has(rom, size, "SRAM_V") || has(rom, size, "SRAM_F_V")) return Type::Sram;
  return Type::None;
}

void Save::set_type(Type t) {
  type_ = t;
  mem_.assign(type_size(t), 0xFF);
  reset();
}

void Save::reset() {
  fstate_ = Flash::Ready; id_mode_ = erase_mode_ = false; bank_ = 0;
  bits_ = 0; nbits_ = 0; read_addr_ = read_pos_ = 0; reading_ = false; wdata_ = 0;
}

u8 Save::read8(u32 addr) {
  const u32 a = addr & 0xFFFF;
  switch (type_) {
  case Type::Sram: return mem_[addr & 0x7FFF];
  case Type::Flash64: case Type::Flash128: {
    if (id_mode_ && a < 2) return (type_ == Type::Flash128 ? ID128 : ID64)[a];
    return mem_[bank_ * 0x10000 + a];
  }
  default: return 0xFF;
  }
}

void Save::write8(u32 addr, u8 v) {
  const u32 a = addr & 0xFFFF;
  if (type_ == Type::Sram) { mem_[addr & 0x7FFF] = v; touch(); return; }
  if (type_ != Type::Flash64 && type_ != Type::Flash128) return;
  switch (fstate_) {
  case Flash::Write:
    mem_[bank_ * 0x10000 + a] = v;
    touch();
    fstate_ = Flash::Ready;
    return;
  case Flash::Bank:
    if (a == 0 && type_ == Type::Flash128) bank_ = v & 1;
    fstate_ = Flash::Ready;
    return;
  case Flash::Cmd1:
    fstate_ = (a == 0x2AAA && v == 0x55) ? Flash::Cmd2 : Flash::Ready;
    return;
  case Flash::Cmd2:
    fstate_ = Flash::Ready;
    if (a == 0x5555) {
      switch (v) {
      case 0x90: id_mode_ = true; break;
      case 0xF0: id_mode_ = false; break;
      case 0x80: erase_mode_ = true; break;
      case 0x10:
        if (erase_mode_) { std::fill(mem_.begin(), mem_.end(), 0xFF); touch(); }
        erase_mode_ = false;
        break;
      case 0xA0: fstate_ = Flash::Write; break;
      case 0xB0: fstate_ = Flash::Bank; break;
      default: break;
      }
    } else if (v == 0x30 && erase_mode_) {   // a 4 KB sector
      std::fill_n(mem_.begin() + bank_ * 0x10000 + (a & 0xF000), 0x1000, 0xFF);
      erase_mode_ = false;
      touch();
    }
    return;
  default:
    if (a == 0x5555 && v == 0xAA) fstate_ = Flash::Cmd1;
    else if (v == 0xF0) id_mode_ = false;
    return;
  }
}

void Save::eeprom_dma(u32 count) {
  if (type_ != Type::EepromUnsized) return;
  // Read request: 2 + address + 1 bits; write: 2 + address + 64 + 1.
  if (count == 9 || count == 73) type_ = Type::Eeprom512;
  else if (count == 17 || count == 81) type_ = Type::Eeprom8k;
  else return;
  if (type_ == Type::Eeprom512) mem_.resize(512);
}

void Save::eeprom_write(u16 v) {
  if (!eeprom()) return;
  if (type_ == Type::EepromUnsized) eeprom_dma(17);   // a CPU-driven access before any DMA: assume 8 KB
  const u32 b = v & 1, ab = addr_bits();
  reading_ = false;
  ++nbits_;
  if (nbits_ <= 2) { bits_ = (bits_ << 1) | b; if (nbits_ == 1) read_addr_ = 0; return; }
  if (nbits_ <= 2 + ab) { read_addr_ = (read_addr_ << 1) | b; return; }
  const u32 blocks = static_cast<u32>(mem_.size() / 8);
  if ((bits_ & 3) == 3) {   // read request: its last bit, then the data goes out
    read_addr_ %= blocks;
    reading_ = true; read_pos_ = 0;
    nbits_ = 0; bits_ = 0;
    return;
  }
  if ((bits_ & 3) == 2 && nbits_ <= 2 + ab + 64) { wdata_ = (wdata_ << 1) | b; return; }
  if ((bits_ & 3) == 2) {   // the write's closing bit
    u8* p = &mem_[(read_addr_ % blocks) * 8];
    for (int i = 0; i < 8; ++i) p[i] = static_cast<u8>(wdata_ >> (56 - 8 * i));
    touch();
  }
  nbits_ = 0; bits_ = 0; wdata_ = 0;
}

u16 Save::eeprom_read() {
  if (!eeprom() || !reading_) return 1;   // idle / a write done: ready
  const u32 pos = read_pos_++;
  if (read_pos_ >= 68) reading_ = false;
  if (pos < 4) return 0;
  const u32 bit = pos - 4;
  return (mem_[read_addr_ * 8 + bit / 8] >> (7 - bit % 8)) & 1;
}

std::vector<u8> Save::data() const {
  if (type_ == Type::None || type_ == Type::EepromUnsized) return {};
  return mem_;
}

bool Save::load(const u8* p, size_t n) {
  if (!n) return false;
  if (type_ == Type::EepromUnsized) {
    if (n == 512) set_type(Type::Eeprom512);
    else if (n == 8192) set_type(Type::Eeprom8k);
  }
  if (type_ == Type::None) {   // no string in the ROM: take the file's word for it
    set_type(n <= 512 ? Type::Eeprom512 : n <= 8192 ? Type::Eeprom8k : n <= 0x8000 ? Type::Sram : n <= 0x10000 ? Type::Flash64 : Type::Flash128);
  }
  std::memcpy(mem_.data(), p, std::min(n, mem_.size()));
  return true;
}

template <class S> void Save::sync_state(S& s) {
  s.begin("GSAV");
  s.fields(type_);
  s.vec(mem_);
  s.fields(fstate_, id_mode_, erase_mode_, bank_, bits_, nbits_, read_addr_, read_pos_, reading_, wdata_);
  s.end();
}
template void Save::sync_state<state::Writer>(state::Writer&);
template void Save::sync_state<state::Reader>(state::Reader&);

} // namespace ds::gba
