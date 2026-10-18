// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// The GBA build of the interpreter's cycle charges (the names cpu_cycles.h
// has for the DS, which the GBA translation units leave out). The ARM7TDMI's
// own costs -- loads 1S+1N+1I, stores 2N, internal cycles -- with the
// cartridge's prefetch buffer (Gba::pf_*): the code fetch an instruction makes
// (at r15) costs its wait states, or a cycle when the buffer already holds
// it; internal cycles and accesses to other memories let the buffer fill,
// a data access to the cartridge stops it.
#pragma once
#include "core/cpu/cpu.h"
#include "core/gba/gba.h"

namespace ds::gba_interp {

inline gba::Gba& machine(CpuContext& cpu) { return *static_cast<gba::Gba*>(cpu.host); }
inline bool cart(u32 a) { return a >= 0x08000000 && a < 0x0E000000; }

// The code fetch made during this instruction: the instruction at r15.
inline u32 code_fetch(CpuContext& cpu, bool seq) {
  const bool t = cpu.thumb();
  const u32 a = cpu.hot.regs[15] & (t ? ~1u : ~3u);
  gba::Gba& g = machine(cpu);
  if (g.pf.on && cart(a)) return g.pf_fetch(a, t ? 1 : 2, seq);
  const u8* tm = cpu.timing7[a >> 15];
  return t ? tm[seq ? 1 : 0] : tm[seq ? 3 : 2];
}
// The current instruction's data accesses: other memories leave the cartridge
// bus to the prefetcher (data_cost has already stopped it for a cartridge access).
inline void data_idle(CpuContext& cpu) {
  if (!cart((cpu.data_region & 0xFF) << 24)) machine(cpu).pf_idle(cpu.data_cycles);
}

inline void charge_C(CpuContext& cpu) { cpu.hot.cycle_budget -= static_cast<s32>(code_fetch(cpu, true)); }

// Internal cycles break the code sequence: the fetch is non-sequential.
inline void charge_CI(CpuContext& cpu, u32 internal) {
  const u32 c = code_fetch(cpu, false);
  machine(cpu).pf_idle(internal);
  cpu.hot.cycle_budget -= static_cast<s32>(c + internal);
}

// Loads: the fetch, the data, an internal cycle.
inline void charge_CDI(CpuContext& cpu) {
  const u32 c = code_fetch(cpu, false);
  data_idle(cpu);
  machine(cpu).pf_idle(1);
  cpu.hot.cycle_budget -= static_cast<s32>(c + cpu.data_cycles + 1);
}
inline void charge_CDI_after_jump(CpuContext& cpu) { charge_CDI(cpu); }

// Stores: the fetch and the data.
inline void charge_CD(CpuContext& cpu) {
  const u32 c = code_fetch(cpu, false);
  data_idle(cpu);
  cpu.hot.cycle_budget -= static_cast<s32>(c + cpu.data_cycles);
}

} // namespace ds::gba_interp
