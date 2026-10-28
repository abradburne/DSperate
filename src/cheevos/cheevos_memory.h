// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Translates RetroAchievements' flat per-console address space to ours: bounds
// check + memcpy from a host pointer, no page tables/traps/timing. A map per
// core, one reader for both:
//   DS:  4 MB main RAM, a 12 MB hole, then data TCM; DSi has the hole filled with RAM.
//   GBA: 32 KB internal work RAM, 256 KB on-board work RAM, then 64 KB of the
//        cartridge's save memory as the bus shows it at 0x0E000000.
// An unbacked address reads as zero and reports itself unbacked, so rcheevos
// marks the achievement unsupported. Each map is checked against rcheevos' at
// run time so an upstream map change fails loudly.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/types.h"

namespace ds { class NDS; }
namespace ds::gba { struct Gba; }

namespace ds::cheevos {

// One stretch of rcheevos' address space and what answers for it.
struct MemoryRegion {
  u32 start = 0, end = 0;     // inclusive RetroAchievements addresses
  const char* what = "";
  const u8* host = nullptr;   // fixed bytes, the whole region of them
  // Or bytes that come and go (a GBA save chip, whose type a game can still
  // change): fills `n` bytes from `offset` into the region and returns how many
  // were backed, the rest zero. Neither: unbacked (the DSi hole).
  u32 (*read)(const void* ctx, u32 offset, u8* dst, u32 n) = nullptr;
  const void* ctx = nullptr;
  u32 size() const { return end - start + 1; }
};

struct MemoryMap {
  std::vector<MemoryRegion> regions;   // in address order
};

// The machine's map, false with a reason when rcheevos' differs from the one
// this was written against.
bool ds_map(NDS& nds, MemoryMap& out, std::string& err);
bool gba_map(gba::Gba& gba, MemoryMap& out, std::string& err);

class Memory {
public:
  void attach(MemoryMap map);
  // The running machine's map: the GBA's in GBA mode, else the DS's (or DSi's).
  bool attach(NDS& nds, std::string& err);
  bool attached() const { return !map_.regions.empty(); }

  // Returns how many of `n` bytes were actually backed; unbacked bytes read
  // as zero. rcheevos treats a short count as "address not supported".
  u32 read(u32 address, u8* dst, u32 n) const;
  bool supported(u32 address, u32 n = 1) const;

  u32 max_address() const { return max_address_; }

  // `ud` is a Memory*. Safe to call unattached (reads zero).
  static u32 peek(u32 address, u32 num_bytes, void* ud);          // rc_runtime_peek_t
  static u32 read_memory(u32 address, u8* buffer, u32 num_bytes, void* ud);

private:
  MemoryMap map_;
  u32 max_address_ = 0;
};

} // namespace ds::cheevos
