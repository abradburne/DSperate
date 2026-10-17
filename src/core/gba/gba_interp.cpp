// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// The run loop of the GBA build of the interpreter: interp.cpp's, less the
// DS's census, DSi deferrals and the ARM7 BIOS wait-loop skip.
#define DS_INTERP_NS gba_interp
#define DS_INTERP_GBA 1
#include "core/cpu/interp/interp_internal.h"
#include "core/gba/gba_interp.h"
#include "core/gba/gba.h"

namespace ds::gba_interp {

void run(CpuContext& cpu) {
  cpu.check_irq();
  if (cpu.halted) return;
  const gba::Gba& g = *static_cast<const gba::Gba*>(cpu.host);
  while (cpu.hot.cycle_budget > 0) {
    cpu.data_cycles = 0;
    cpu.jumped = false;
    if (cpu.thumb()) {
      const u16 instr = fetch16(cpu, cpu.hot.regs[15] - 4);
      if (g.trace) g.trace(cpu, instr, g.trace_user);
      exec_thumb(cpu, instr);
      if (!cpu.jumped) cpu.hot.regs[15] += 2;
    } else {
      const u32 instr = fetch32(cpu, cpu.hot.regs[15] - 8);
      if (g.trace) g.trace(cpu, instr, g.trace_user);
      exec_arm(cpu, instr);
      if (!cpu.jumped) cpu.hot.regs[15] += 4;
    }
    if (cpu.halted) return;
    if (cpu.hot.irq_pending) cpu.check_irq();
  }
}

} // namespace ds::gba_interp
