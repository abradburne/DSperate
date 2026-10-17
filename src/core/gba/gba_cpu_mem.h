// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// The GBA build of the interpreter's memory accessors (see interp_internal.h):
// same names and signatures as cpu_mem.h's, declared in ds::gba_interp so they
// hide the DS ones there. Direct pages go through the CPU's page table as on
// the DS; everything else reaches the GBA's bus (gba.cpp). Costs come from the
// GBA's own per-32 KB table (Gba::rebuild_timing, from WAITCNT), in GBA cycles.
#pragma once
#include "core/cpu/cpu.h"
#include "core/cpu/cpu_mem.h"   // rotr32; its accessors are compiled out here

#include <cstring>

namespace ds::gba {
u8   bus_read8 (CpuContext& cpu, u32 addr);
u16  bus_read16(CpuContext& cpu, u32 addr);
u32  bus_read32(CpuContext& cpu, u32 addr);
void bus_write8 (CpuContext& cpu, u32 addr, u8 v);
void bus_write16(CpuContext& cpu, u32 addr, u16 v);
void bus_write32(CpuContext& cpu, u32 addr, u32 v);
u32  bus_fetch(CpuContext& cpu, u32 addr, u32 width);
} // namespace ds::gba

namespace ds::gba_interp {

// width: 0 = 8/16-bit, 1 = 32-bit. data_region never reads as 0x02: the DS's
// ARM7 main-RAM overlap rules (cpu_cycles.h) are not the GBA's EWRAM.
inline void data_cost(CpuContext& cpu, u32 addr, int width, bool seq, bool) {
  const u8* t = cpu.timing7[addr >> 15];
  const u32 c = seq ? t[width ? 3 : 1] : t[width ? 2 : 0];
  if (seq) cpu.data_cycles += c; else { cpu.data_cycles = c; cpu.data_region = 0xFF; }
}

inline u8 mem_read8(CpuContext& cpu, u32 addr, bool seq = false) {
  data_cost(cpu, addr, 0, seq, false);
  if (u8* p = cpu.page_table.read_ptr(addr)) return *p;
  return gba::bus_read8(cpu, addr);
}
inline u16 mem_read16(CpuContext& cpu, u32 addr, bool seq = false) {
  addr &= ~1u;
  data_cost(cpu, addr, 0, seq, false);
  if (u8* p = cpu.page_table.read_ptr(addr)) { u16 v; std::memcpy(&v, p, 2); return v; }
  return gba::bus_read16(cpu, addr);
}
inline u32 mem_read32(CpuContext& cpu, u32 addr, bool seq = false) {
  addr &= ~3u;
  data_cost(cpu, addr, 1, seq, false);
  if (u8* p = cpu.page_table.read_ptr(addr)) { u32 v; std::memcpy(&v, p, 4); return v; }
  return gba::bus_read32(cpu, addr);
}

inline void mem_write8(CpuContext& cpu, u32 addr, u8 v, bool seq = false) {
  data_cost(cpu, addr, 0, seq, true);
  bool code = false;
  if (u8* p = cpu.page_table.write_ptr(addr, &code)) { *p = v; return; }
  gba::bus_write8(cpu, addr, v);
}
inline void mem_write16(CpuContext& cpu, u32 addr, u16 v, bool seq = false) {
  addr &= ~1u;
  data_cost(cpu, addr, 0, seq, true);
  bool code = false;
  if (u8* p = cpu.page_table.write_ptr(addr, &code)) { std::memcpy(p, &v, 2); return; }
  gba::bus_write16(cpu, addr, v);
}
inline void mem_write32(CpuContext& cpu, u32 addr, u32 v, bool seq = false) {
  addr &= ~3u;
  data_cost(cpu, addr, 1, seq, true);
  bool code = false;
  if (u8* p = cpu.page_table.write_ptr(addr, &code)) { std::memcpy(p, &v, 4); return; }
  gba::bus_write32(cpu, addr, v);
}

inline u32 fetch32(CpuContext& cpu, u32 addr) {
  if (u8* p = cpu.page_table.read_ptr(addr)) { u32 v; std::memcpy(&v, p, 4); return v; }
  return gba::bus_fetch(cpu, addr, 32);
}
inline u16 fetch16(CpuContext& cpu, u32 addr) {
  if (u8* p = cpu.page_table.read_ptr(addr)) { u16 v; std::memcpy(&v, p, 2); return v; }
  return static_cast<u16>(gba::bus_fetch(cpu, addr, 16));
}

} // namespace ds::gba_interp
