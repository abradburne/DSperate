// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// The run loop of the GBA build of the interpreter: interp.cpp's with the
// prefetch pipeline, less the DS's census, DSi deferrals and the ARM7 BIOS
// wait-loop skip.
#define DS_INTERP_NS gba_interp
#define DS_INTERP_GBA 1
#include "core/cpu/interp/interp_internal.h"
#include "core/gba/gba_interp.h"
#include "core/gba/gba.h"

namespace ds::gba_interp {

void cart_data_access(CpuContext& cpu) { machine(cpu).pf_flush(); }

// The ARM7TDMI's pipeline: the two instructions after the one executing were
// fetched before it ran, so a store over them (self-modifying code) is not
// seen until they are refetched. Kept by address; a jump empties it.
void run(CpuContext& cpu) {
  cpu.check_irq();
  if (cpu.halted) return;
  gba::Gba& g = *static_cast<gba::Gba*>(cpu.host);
  gba::Gba::Pipeline& p = g.pipe;
  while (cpu.hot.cycle_budget > 0) {
    cpu.data_cycles = 0;
    cpu.jumped = false;
    const bool t = cpu.thumb();
    const u32 step = t ? 2 : 4, pc = cpu.hot.regs[15] - 2 * step, key = pc | t;
    u32 instr;
    if (p.n && p.addr[0] == key) {
      instr = p.val[0];
      p.addr[0] = p.addr[1]; p.val[0] = p.val[1]; p.n = 1;
    } else {
      instr = t ? fetch16(cpu, pc) : fetch32(cpu, pc);
      p.addr[0] = (pc + step) | t; p.val[0] = t ? fetch16(cpu, pc + step) : fetch32(cpu, pc + step); p.n = 1;
    }
    p.addr[1] = (pc + 2 * step) | t;
    p.val[1] = t ? fetch16(cpu, pc + 2 * step) : fetch32(cpu, pc + 2 * step);
    p.n = 2;
    if (g.trace) g.trace(cpu, instr, g.trace_user);
    if (t) {
      exec_thumb(cpu, static_cast<u16>(instr));
      if (!cpu.jumped) cpu.hot.regs[15] += 2;
    } else {
      exec_arm(cpu, instr);
      if (!cpu.jumped) cpu.hot.regs[15] += 4;
    }
    if (cpu.jumped) g.pf_restart(cpu.hot.regs[15] & ~1u);   // the refill fetched the target and the next; it reads on from there
    if (cpu.halted) return;
    if (cpu.hot.irq_pending) { cpu.jumped = false; cpu.check_irq(); if (cpu.jumped) g.pf_restart(cpu.hot.regs[15] & ~1u); }
  }
}

} // namespace ds::gba_interp
