// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/cpu/cpu.h"

namespace ds::gba_interp {

// The ARM7TDMI interpreter on the GBA's bus (gba_cpu_mem.h): runs until the
// budget goes negative, the CPU halts, or an IRQ is taken and returns.
void run(CpuContext& cpu);

} // namespace ds::gba_interp
