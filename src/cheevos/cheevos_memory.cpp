// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "cheevos/cheevos_memory.h"

#include <cstring>

#include "core/gba/gba.h"
#include "core/mem/bus.h"
#include "core/nds.h"
#include "rc_consoles.h"

namespace ds::cheevos {
namespace {

// What one of rcheevos' regions is expected to be here, and what answers for it.
struct Expect {
  u32 size;
  const char* what;
  const u8* host;
  u32 (*read)(const void*, u32, u8*, u32);
  const void* ctx;
};

// Refuse rather than mis-map achievement addresses against an unexpected map.
bool build(u32 console_id, const char* console, const Expect* expect, u32 count, MemoryMap& out, std::string& err) {
  err.clear();
  out.regions.clear();
  const rc_memory_regions_t* map = rc_console_memory_regions(console_id);
  if (!map) { err = std::string("rcheevos has no memory map for the ") + console; return false; }
  if (map->num_regions != count) {
    err = std::string("rcheevos' ") + console + " map has " + std::to_string(map->num_regions) +
          " regions, not the " + std::to_string(count) + " this was written against";
    return false;
  }
  for (u32 i = 0; i < count; ++i) {
    const rc_memory_region_t& rr = map->region[i];
    const u32 size = rr.end_address - rr.start_address + 1;
    if (size != expect[i].size) {
      err = std::string("rcheevos' ") + console + " region " + std::to_string(i) + " (" + expect[i].what +
            ") is " + std::to_string(size) + " bytes, not " + std::to_string(expect[i].size);
      return false;
    }
    MemoryRegion r;
    r.start = rr.start_address;
    r.end = rr.end_address;
    r.what = expect[i].what;
    // A hole (RC_MEMORY_TYPE_UNUSED) stays unbacked despite being a region.
    if (rr.type != RC_MEMORY_TYPE_UNUSED) {
      r.host = expect[i].host;
      r.read = expect[i].read;
      r.ctx = expect[i].ctx;
      if (!r.host && !r.read) { err = std::string("no buffer for ") + expect[i].what; return false; }
    }
    out.regions.push_back(r);
  }
  return true;
}

u32 gba_save_read(const void* ctx, u32 offset, u8* dst, u32 n) {
  return static_cast<const gba::Save*>(ctx)->peek(offset, dst, n);
}

} // namespace

bool ds_map(NDS& nds, MemoryMap& out, std::string& err) {
  // DSi map = DS map with its hole filled: 16 MB main RAM instead of 4 MB, then data TCM.
  mem::Bus& bus = nds.bus;
  if (nds.dsi) {
    const Expect dsi[2] = {
      {mem::Bus::MAIN_RAM_SIZE_DSI, "main RAM", bus.main_ram.get(), nullptr, nullptr},
      {mem::Bus::DTCM_SIZE,         "data TCM", bus.dtcm.get(),     nullptr, nullptr},
    };
    return build(RC_CONSOLE_NINTENDO_DSI, "DSi", dsi, 2, out, err);
  }
  const Expect ds[3] = {
    {mem::Bus::MAIN_RAM_SIZE, "main RAM",          bus.main_ram.get(), nullptr, nullptr},
    {0xC00000,                "the DSi-only hole", nullptr,            nullptr, nullptr},
    {mem::Bus::DTCM_SIZE,     "data TCM",          bus.dtcm.get(),     nullptr, nullptr},
  };
  return build(RC_CONSOLE_NINTENDO_DS, "DS", ds, 3, out, err);
}

bool gba_map(gba::Gba& g, MemoryMap& out, std::string& err) {
  // The save memory is read through the chip: its bytes move when a game with
  // no save string first writes there (it gets SRAM), and Flash banks.
  const Expect expect[3] = {
    {gba::IWRAM_SIZE, "internal work RAM",  g.iwram.get(), nullptr,       nullptr},
    {gba::EWRAM_SIZE, "on-board work RAM",  g.ewram.get(), nullptr,       nullptr},
    {0x10000,         "cartridge save RAM", nullptr,       gba_save_read, &g.save},
  };
  return build(RC_CONSOLE_GAMEBOY_ADVANCE, "GBA", expect, 3, out, err);
}

void Memory::attach(MemoryMap map) {
  map_ = std::move(map);
  max_address_ = 0;
  for (const MemoryRegion& r : map_.regions)
    if (r.end > max_address_) max_address_ = r.end;
}

bool Memory::attach(NDS& nds, std::string& err) {
  MemoryMap map;
  const bool ok = nds.gba ? gba_map(*nds.gba, map, err) : ds_map(nds, map, err);
  attach(ok ? std::move(map) : MemoryMap{});
  return ok;
}

u32 Memory::read(u32 address, u8* dst, u32 n) const {
  u32 backed = 0;
  while (n) {
    const MemoryRegion* hit = nullptr;
    for (const MemoryRegion& r : map_.regions) {
      if (address >= r.start && address <= r.end) { hit = &r; break; }
    }
    if (!hit) {
      std::memset(dst, 0, n);
      return backed;
    }
    const u32 off = address - hit->start;
    const u32 take = n < hit->size() - off ? n : hit->size() - off;
    if (hit->host) {
      std::memcpy(dst, hit->host + off, take);
      backed += take;
    } else if (hit->read) {
      backed += hit->read(hit->ctx, off, dst, take);
    } else {
      std::memset(dst, 0, take);
    }
    dst += take; address += take; n -= take;
  }
  return backed;
}

bool Memory::supported(u32 address, u32 n) const {
  if (n == 0) return true;
  u8 scratch[64];
  while (n) {
    const u32 take = n < sizeof scratch ? n : static_cast<u32>(sizeof scratch);
    if (read(address, scratch, take) != take) return false;
    address += take; n -= take;
  }
  return true;
}

u32 Memory::peek(u32 address, u32 num_bytes, void* ud) {
  const Memory* m = static_cast<const Memory*>(ud);
  u8 b[4] = {};
  if (num_bytes > sizeof b) num_bytes = sizeof b;
  if (m) m->read(address, b, num_bytes);
  u32 v = 0;   // little-endian, matching rcheevos' convention
  for (u32 i = 0; i < num_bytes; ++i) v |= static_cast<u32>(b[i]) << (8 * i);
  return v;
}

u32 Memory::read_memory(u32 address, u8* buffer, u32 num_bytes, void* ud) {
  const Memory* m = static_cast<const Memory*>(ud);
  if (!m) { std::memset(buffer, 0, num_bytes); return 0; }
  return m->read(address, buffer, num_bytes);
}

} // namespace ds::cheevos
