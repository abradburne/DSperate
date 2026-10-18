// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/gba/gba.h"
#include "core/gba/bios/cultbios.h"
#include "core/gba/gba_interp.h"
#include "core/cpu/timing_mode.h"
#include "core/state/state.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace ds::gba {

namespace {
u64 hash_bytes(const u8* p, size_t n) {
  u64 h = 1469598103934665603ull;
  size_t i = 0;
  for (; i + 8 <= n; i += 8) { u64 w; std::memcpy(&w, p + i, 8); h = (h ^ w) * 1099511628211ull; h ^= h >> 29; }
  for (; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
  return h ^ n;
}
inline u16 rd16(const u8* p) { return static_cast<u16>(p[0] | (p[1] << 8)); }
inline u32 rd32(const u8* p) { u32 v; std::memcpy(&v, p, 4); return v; }
inline void wr16(u8* p, u16 v) { p[0] = static_cast<u8>(v); p[1] = static_cast<u8>(v >> 8); }
inline u32 vram_off(u32 addr) { u32 o = addr & 0x1FFFF; return o >= 0x18000 ? o - 0x8000 : o; }
// The GBA's cycle model wants the exact cost tables (cpu_cycles.h), not the
// DS recompiler's constant "fast" model; process-wide, so held only while running.
struct ExactTiming {
  bool saved = g_fast_timing;
  ExactTiming() { g_fast_timing = false; }
  ~ExactTiming() { g_fast_timing = saved; }
};
} // namespace

// gba_cpu_mem.h's slow paths.
u8   bus_read8 (CpuContext& c, u32 a) { return static_cast<Gba*>(c.host)->read8(a); }
u16  bus_read16(CpuContext& c, u32 a) { return static_cast<Gba*>(c.host)->read16(a); }
u32  bus_read32(CpuContext& c, u32 a) { return static_cast<Gba*>(c.host)->read32(a); }
void bus_write8 (CpuContext& c, u32 a, u8 v)  { static_cast<Gba*>(c.host)->write8(a, v); }
void bus_write16(CpuContext& c, u32 a, u16 v) { static_cast<Gba*>(c.host)->write16(a, v); }
void bus_write32(CpuContext& c, u32 a, u32 v) { static_cast<Gba*>(c.host)->write32(a, v); }
u32  bus_fetch(CpuContext& c, u32 a, u32 w)   { return static_cast<Gba*>(c.host)->fetch(a, w); }

Gba::Gba()
    : cpu(new CpuContext),
      bios(mem::alloc_page_buf(BIOS_SIZE)), ewram(mem::alloc_page_buf(EWRAM_SIZE)), iwram(mem::alloc_page_buf(IWRAM_SIZE)),
      palette(mem::alloc_page_buf(2 * PALETTE_SIZE)), vram(mem::alloc_page_buf(VRAM_SIZE)), oam(mem::alloc_page_buf(2 * OAM_SIZE)),
      rom(mem::alloc_page_buf(ROM_MAX)),
      ppu(*this), audio(*this), timing_(size_t{1} << 17) {
  std::memcpy(bios.get(), kCultBios, BIOS_SIZE);
  bios_id = hash_bytes(bios.get(), BIOS_SIZE);
  for (u32 a = 0; a < ROM_MAX; a += 2) wr16(rom.get() + a, static_cast<u16>(a >> 1));   // open bus past the end of a cart
  map_memory();
  reset();
}

Gba::~Gba() = default;

bool Gba::load_bios(const std::string& path, std::string* err) {
  bios_native = false;
  if (!path.empty()) {
    std::ifstream f(path, std::ios::binary);
    if (f) {
      std::vector<u8> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
      if (d.size() != BIOS_SIZE) { if (err) *err = path + ": a GBA BIOS is 16384 bytes"; return false; }
      std::memcpy(bios.get(), d.data(), BIOS_SIZE);
      bios_native = true;
    }
  }
  if (!bios_native) std::memcpy(bios.get(), kCultBios, BIOS_SIZE);
  bios_id = hash_bytes(bios.get(), BIOS_SIZE);
  return true;
}

bool Gba::load_rom(std::vector<u8> image, std::string* err) {
  if (image.size() < 0xC0) { if (err) *err = "too small for a GBA cartridge"; return false; }
  if (image.size() > ROM_MAX) image.resize(ROM_MAX);
  rom_size = static_cast<u32>(image.size());
  for (u32 a = 0; a < ROM_MAX; a += 2) wr16(rom.get() + a, static_cast<u16>(a >> 1));
  std::memcpy(rom.get(), image.data(), image.size());
  rom_id = hash_bytes(image.data(), image.size());
  save.set_type(Save::detect(rom.get(), rom_size));
  rtc.present = false;
  for (u32 i = 0; i + 8 <= rom_size && !rtc.present; i += 4) rtc.present = std::memcmp(rom.get() + i, "SIIRTC_V", 8) == 0;
  map_memory();
  return true;
}

std::string Gba::title() const {
  std::string t(reinterpret_cast<const char*>(rom.get() + 0xA0), 12);
  while (!t.empty() && (t.back() == '\0' || t.back() == ' ')) t.pop_back();
  return t;
}
u32 Gba::game_code() const { return rd32(rom.get() + 0xAC); }

// A game with no save string that writes to 0x0E anyway gets SRAM.
static void save_write_any(Save& s, u32 addr, u8 v) {
  if (s.type() == Save::Type::None) s.set_type(Save::Type::Sram);
  s.write8(addr, v);
}

bool Gba::eeprom_at(u32 addr) const {
  return save.eeprom() && (addr >> 24) == 0x0D && (rom_size <= 16 * 1024 * 1024 || addr >= 0x0DFFFF00);
}

// ---- memory map --------------------------------------------------------------

void Gba::map_memory() {
  mem::PageTable& pt = cpu->page_table;
  using mem::PAGE_READABLE; using mem::PAGE_WRITABLE; using mem::PAGE_SIZE;
  // BIOS (read-protected), I/O and SRAM stay on the slow path.
  for (u32 a = 0x02000000; a < 0x03000000; a += EWRAM_SIZE) pt.map(a, EWRAM_SIZE, ewram.get(), PAGE_READABLE | PAGE_WRITABLE);
  for (u32 a = 0x03000000; a < 0x04000000; a += IWRAM_SIZE) pt.map(a, IWRAM_SIZE, iwram.get(), PAGE_READABLE | PAGE_WRITABLE);
  // Palette and OAM are 1 KB, half a page: their buffers hold two copies, kept
  // equal by the (slow-path) stores, so loads can be direct.
  for (u32 a = 0x05000000; a < 0x06000000; a += PAGE_SIZE) pt.map(a, PAGE_SIZE, palette.get(), PAGE_READABLE);
  for (u32 a = 0x07000000; a < 0x08000000; a += PAGE_SIZE) pt.map(a, PAGE_SIZE, oam.get(), PAGE_READABLE);
  // VRAM: 96 KB in each 128 KB, the last 32 KB mirroring the OBJ 32 KB. Stores
  // take the slow path (byte stores widen to halfwords or vanish).
  for (u32 a = 0x06000000; a < 0x07000000; a += PAGE_SIZE) pt.map(a, PAGE_SIZE, vram.get() + vram_off(a), PAGE_READABLE);
  for (u32 a = 0x08000000; a < 0x0E000000; a += ROM_MAX) pt.map(a, ROM_MAX, rom.get(), PAGE_READABLE);
  // An EEPROM takes the 0x0D region (all of it, or the last page behind a
  // 32 MB ROM) off the direct path.
  if (save.eeprom()) {
    if (rom_size <= 16 * 1024 * 1024) pt.unmap(0x0D000000, 0x01000000);
    else pt.unmap(0x0DFFF800, PAGE_SIZE);
  }
}

void Gba::map_gpio() {
  // While the port reads back, its 2 KB page (and the mirrors') takes the slow path, where
  // the ROM is read directly around the six bytes of the port.
  for (u32 base : {0x08000000u, 0x0A000000u, 0x0C000000u}) {
    if (rtc.readable()) cpu->page_table.unmap(base, mem::PAGE_SIZE);
    else cpu->page_table.map(base, mem::PAGE_SIZE, rom.get(), mem::PAGE_READABLE);
  }
}

void Gba::rebuild_timing() {
  const u16 w = rd16(&io[0x204]);
  static const u8 nws[4] = {4, 3, 2, 8};
  auto fill = [&](u32 region, u8 n16, u8 s16, u8 n32, u8 s32) {
    for (u32 i = region << 9; i < (region + 1) << 9; ++i) timing_[i] = {n16, s16, n32, s32};
  };
  for (u32 r = 0; r < 256; ++r) fill(r, 1, 1, 1, 1);
  fill(0x02, 3, 3, 6, 6);
  fill(0x05, 1, 1, 2, 2);
  fill(0x06, 1, 1, 2, 2);
  auto rom = [&](u32 region, u32 n, u32 s) {
    const u8 N = static_cast<u8>(1 + n), S = static_cast<u8>(1 + s);
    fill(region, N, S, static_cast<u8>(N + S), static_cast<u8>(2 * S));
    fill(region + 1, N, S, static_cast<u8>(N + S), static_cast<u8>(2 * S));
  };
  rom(0x08, nws[(w >> 2) & 3], (w & 0x10) ? 1 : 2);
  rom(0x0A, nws[(w >> 5) & 3], (w & 0x80) ? 1 : 4);
  rom(0x0C, nws[(w >> 8) & 3], (w & 0x400) ? 1 : 8);
  const u8 sr = static_cast<u8>(1 + nws[w & 3]);
  fill(0x0E, sr, sr, sr, sr);
  fill(0x0F, sr, sr, sr, sr);
  cpu->timing7 = reinterpret_cast<const u8 (*)[4]>(timing_.data());
  const bool on = w & 0x4000;
  if (on != pf.on) { pf.on = on; pf_flush(); }
}

void Gba::pf_idle(u32 n) {
  if (!pf.active) return;
  const u32 duty = timing_[pf.next >> 15][1];
  while (n && pf.count < 8) {
    const u32 need = duty - pf.progress;
    if (n < need) { pf.progress += n; return; }
    n -= need; pf.progress = 0; ++pf.count; pf.next += 2;
  }
}

u32 Gba::pf_fetch(u32 a, u32 halfwords, bool seq) {
  const u8* t = timing_[a >> 15].data();
  u32 cost = 0;
  for (u32 h = 0; h < halfwords; ++h, a += 2) {
    if (pf.count && pf.next - 2 * pf.count == a) {   // held: a cycle (both halves of a word in one)
      --pf.count;
      if (h == 0) { cost += 1; pf_idle(1); }
    } else if (pf.active && !pf.count && pf.next == a) {   // on its way: what is left of it
      cost += t[1] - pf.progress;
      pf.progress = 0; pf.next += 2;
    } else {   // a miss: an ordinary access, and the buffer starts again behind it
      cost += h ? t[1] : t[seq ? 1 : 0];
      pf.active = true; pf.count = 0; pf.progress = 0; pf.next = a + 2;
    }
  }
  return cost;
}

void Gba::debug_write(u32 addr, u32 v, u32 width) {
  if (addr >= 0x04FFF780) { if (width >= 16 && (v & 0xFFFF) == 0xC0DE) debug_on_ = true; return; }
  if (!debug_on_) return;
  if (addr < 0x04FFF700) {
    for (u32 i = 0; i < width / 8; ++i) debug_str_[(addr - 0x04FFF600 + i) & 0xFF] = static_cast<char>(v >> (8 * i));
    return;
  }
  if ((addr & ~1u) == 0x04FFF700 && (v & 0x100)) {   // flush, at level v & 7
    std::fprintf(debug_out, "%s\n", debug_str_);
    std::fflush(debug_out);
    std::fill(std::begin(debug_str_), std::end(debug_str_), '\0');
  }
}

u32 Gba::open_bus() {
  const u32 pc = cpu->hot.regs[15];
  auto raw32 = [&](u32 a) -> u32 { if (u8* p = cpu->page_table.read_ptr(a & ~3u)) return rd32(p); if (a < BIOS_SIZE) return rd32(bios.get() + (a & ~3u)); return 0; };
  if (cpu->thumb()) {
    const u32 w = raw32(pc);
    const u16 h = static_cast<u16>((pc & 2) ? w >> 16 : w);
    return h | (u32{h} << 16);
  }
  return raw32(pc);
}

u32 Gba::fetch(u32 addr, u32 width) {
  if (addr < BIOS_SIZE) {
    const u32 v = width == 32 ? rd32(bios.get() + (addr & ~3u)) : rd16(bios.get() + (addr & ~1u));
    // What the bus last carried from the BIOS: the CPU fetches two
    // instructions ahead (gba_interp.cpp), so this is the prefetched opcode.
    bios_latch_ = width == 32 ? v : (v | (v << 16));
    if (!bios_native) {
      // A game reading the BIOS sees what Nintendo's leaves on the bus, not
      // the built-in one's own opcodes: its SWI and IRQ returns (cultbios
      // 0x170, 0x124, prefetching 0x178 and 0x12C) stand for Nintendo's at
      // 0x188 and 0x13C. Its IRQ call (0x11C) prefetches the same opcode as
      // Nintendo's at 0x134.
      if (addr == 0x178) bios_latch_ = 0xE3A02004;
      else if (addr == 0x12C) bios_latch_ = 0xE55EC002;
      // RegisterRamReset's "all other registers" (cultbios 0xD78-0xDBC,
      // back at 0xCC4) leaves DISPCNT 0, Nintendo's leaves forced blank
      // (0x0080), which games read to decide whether to write video
      // registers now or queue them for VBlank (Drill Dozer).
      else if (addr == 0xD80) rrr_io_ = true;
      else if (addr == 0xCC4 && rrr_io_) { rrr_io_ = false; io_write16(0x000, 0x0080); }
    }
    return v;
  }
  return width == 32 ? read32(addr) : read16(addr);
}

u8 Gba::read8(u32 addr) {
  switch (addr >> 24) {
  case 0x00:
    if (addr < BIOS_SIZE) return static_cast<u8>((cpu->hot.regs[15] < BIOS_SIZE + 8 ? rd32(bios.get() + (addr & ~3u)) : bios_latch_) >> ((addr & 3) * 8));
    break;
  case 0x04:
    if ((addr & 0xFFFFFF) < IO_SIZE && !io_open(addr & 0x3FE)) return static_cast<u8>(io_read16(addr & 0x3FE) >> ((addr & 1) * 8));
    break;
  case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C:
    if (gpio_at(addr) && rtc.readable()) return static_cast<u8>(rtc.read((addr - 0x080000C4) & ~1u) >> ((addr & 1) * 8));
    return rom[addr & (ROM_MAX - 1)];
  case 0x0D: return rom[addr & (ROM_MAX - 1)];   // the EEPROM is 16-bit only
  case 0x0E: case 0x0F: return save.read8(addr);
  default:
    if (u8* p = cpu->page_table.read_ptr(addr)) return *p;
    break;
  }
  return static_cast<u8>(open_bus() >> ((addr & 3) * 8));
}

u16 Gba::read16(u32 addr) {
  const u32 raw = addr;   // SRAM/Flash: an 8-bit bus, the address as given
  addr &= ~1u;
  switch (addr >> 24) {
  case 0x00:
    if (addr < BIOS_SIZE) return static_cast<u16>((cpu->hot.regs[15] < BIOS_SIZE + 8 ? rd32(bios.get() + (addr & ~3u)) : bios_latch_) >> ((addr & 2) * 8));
    break;
  case 0x04:
    if ((addr & 0xFFFFFF) < IO_SIZE && !io_open(addr & 0x3FE)) return io_read16(addr & 0x3FE);
    if (debug_at(addr)) return debug_read16(addr);
    break;
  case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C:
    if (gpio_at(addr) && rtc.readable()) return rtc.read(addr - 0x080000C4);
    return rd16(rom.get() + (addr & (ROM_MAX - 1)));
  case 0x0D: if (eeprom_at(addr)) return save.eeprom_read(); return rd16(rom.get() + (addr & (ROM_MAX - 1)));
  case 0x0E: case 0x0F: return static_cast<u16>(save.read8(raw) * 0x0101u);
  default:
    if (u8* p = cpu->page_table.read_ptr(addr)) return rd16(p);
    break;
  }
  return static_cast<u16>(open_bus() >> ((addr & 2) * 8));
}

u32 Gba::read32(u32 addr) {
  const u32 raw = addr;
  addr &= ~3u;
  switch (addr >> 24) {
  case 0x00:
    if (addr < BIOS_SIZE) return cpu->hot.regs[15] < BIOS_SIZE + 8 ? rd32(bios.get() + addr) : bios_latch_;
    break;
  case 0x04:
    if ((addr & 0xFFFFFF) < IO_SIZE) {
      const u32 ob = (io_open(addr & 0x3FC) || io_open((addr & 0x3FC) + 2)) ? open_bus() : 0;
      const u32 lo = io_open(addr & 0x3FC) ? ob & 0xFFFF : io_read16(addr & 0x3FC);
      const u32 hi = io_open((addr & 0x3FC) + 2) ? ob >> 16 : io_read16((addr & 0x3FC) + 2);
      return lo | (hi << 16);
    }
    break;
  case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C:
    if (gpio_at(addr) && rtc.readable()) return rtc.read(addr - 0x080000C4) | (u32{rtc.read(addr - 0x080000C4 + 2)} << 16);
    return rd32(rom.get() + (addr & (ROM_MAX - 1)));
  case 0x0D: if (eeprom_at(addr)) return save.eeprom_read(); return rd32(rom.get() + (addr & (ROM_MAX - 1)));
  case 0x0E: case 0x0F: return save.read8(raw) * 0x01010101u;
  default:
    if (u8* p = cpu->page_table.read_ptr(addr)) return rd32(p);
    break;
  }
  return open_bus();
}

void Gba::write8(u32 addr, u8 v) {
  switch (addr >> 24) {
  case 0x02: ewram[addr & (EWRAM_SIZE - 1)] = v; return;
  case 0x03: iwram[addr & (IWRAM_SIZE - 1)] = v; return;
  case 0x04:
    if ((addr & 0xFFFFFF) < IO_SIZE) io_write16(addr & 0x3FE, static_cast<u16>(v * 0x0101u), (addr & 1) ? 0xFF00 : 0x00FF);
    else if (debug_at(addr)) debug_write(addr, v, 8);
    return;
  case 0x05: write16(addr, static_cast<u16>(v * 0x0101u)); return;   // a byte lands in both halves
  case 0x06: {
    // BG VRAM: both halves; OBJ VRAM: dropped.
    const u32 bg_end = (io[0] & 7) >= 3 ? 0x14000 : 0x10000;
    if (vram_off(addr) < bg_end) write16(addr, static_cast<u16>(v * 0x0101u));
    return;
  }
  case 0x07: return;                                                 // OAM ignores byte stores
  case 0x0E: case 0x0F: save_write_any(save, addr, v); return;
  default: return;
  }
}

void Gba::write16(u32 addr, u16 v) {
  const u32 raw = addr;
  addr &= ~1u;
  switch (addr >> 24) {
  case 0x02: wr16(ewram.get() + (addr & (EWRAM_SIZE - 1)), v); return;
  case 0x03: wr16(iwram.get() + (addr & (IWRAM_SIZE - 1)), v); return;
  case 0x04:
    if ((addr & 0xFFFFFF) < IO_SIZE) io_write16(addr & 0x3FE, v);
    else if (debug_at(addr)) debug_write(addr, v, 16);
    return;
  case 0x05: wr16(palette.get() + (addr & 0x3FF), v); wr16(palette.get() + 0x400 + (addr & 0x3FF), v); return;
  case 0x06: wr16(vram.get() + vram_off(addr), v); return;
  case 0x07: wr16(oam.get() + (addr & 0x3FF), v); wr16(oam.get() + 0x400 + (addr & 0x3FF), v); return;
  case 0x0D: if (eeprom_at(addr)) save.eeprom_write(v); return;
  case 0x0E: case 0x0F: save_write_any(save, raw, static_cast<u8>(v >> ((raw & 1) * 8))); return;   // the byte the rotated store puts there
  case 0x08:
    if (gpio_at(addr)) {
      const bool was = rtc.readable();
      rtc.write(addr - 0x080000C4, v);
      if (rtc.readable() != was) map_gpio();
    }
    return;
  default: return;   // ROM, unmapped
  }
}

void Gba::write32(u32 addr, u32 v) {
  if ((addr >> 24) == 0x0E || (addr >> 24) == 0x0F) { save_write_any(save, addr, static_cast<u8>(v >> ((addr & 3) * 8))); return; }
  addr &= ~3u;
  write16(addr, static_cast<u16>(v));
  write16(addr + 2, static_cast<u16>(v >> 16));
}

// ---- I/O ---------------------------------------------------------------------

bool Gba::io_open(u32 off) {
  if (off >= 0x10 && off < 0x48) return true;                    // BG scroll/affine, window rectangles
  if (off == 0x4C || off == 0x4E || (off >= 0x54 && off < 0x60)) return true;   // MOSAIC, BLDY, unused
  if (off == 0x8C || off == 0x8E || (off >= 0xA0 && off < 0xB0)) return true;  // the FIFOs, unused
  if (off >= 0xB0 && off < 0xE0) return (off - 0xB0) % 12 < 8;   // DMA addresses
  return off >= 0xE0 && off < 0x100;
}

u16 Gba::io_read16(u32 off) {
  switch (off) {
  case 0x000: case 0x002: case 0x00C: case 0x00E: return rd16(&io[off]);
  case 0x008: case 0x00A: return rd16(&io[off]) & 0xDFFF;
  case 0x048: case 0x04A: return rd16(&io[off]) & 0x3F3F;
  case 0x050: return rd16(&io[off]) & 0x3FFF;
  case 0x052: return rd16(&io[off]) & 0x1F1F;
  case 0x120: case 0x122: case 0x124: case 0x126: case 0x128: case 0x12A:   // serial: kept as written
  case 0x132: case 0x134: case 0x140: case 0x150: case 0x152: case 0x154: case 0x156: case 0x158:
  case 0x204: case 0x300:
    return rd16(&io[off]);
  case 0x004: return static_cast<u16>((rd16(&io[4]) & 0xFF38) | (vcount_ >= 160 && vcount_ < 227 ? 1 : 0) | (in_hblank_ ? 2 : 0) |
                                      (vcount_ == (rd16(&io[4]) >> 8) ? 4 : 0));
  case 0x006: return static_cast<u16>(vcount_);
  case 0x100: case 0x104: case 0x108: case 0x10C: return timer_read(static_cast<int>((off - 0x100) >> 2));
  case 0x102: case 0x106: case 0x10A: case 0x10E: return timers_[(off - 0x100) >> 2].control;
  case 0x130: return keyinput_;
  case 0x200: return ie;
  case 0x202: return if_;
  case 0x208: return ime;
  default: break;
  }
  if (off >= 0x60 && off < 0xB0) return audio.read16(off);
  if (off >= 0xB0 && off < 0xE0) {   // DMA: the count reads 0, the control back
    const u32 i = (off - 0xB0) / 12, r = (off - 0xB0) % 12;
    return r == 10 ? dma_[i].control : 0;
  }
  return 0;   // unused, readable as 0
}

void Gba::io_write16(u32 off, u16 v, u16 mask) {
  const u16 old = rd16(&io[off]);
  const u16 nv = static_cast<u16>((old & ~mask) | (v & mask));
  switch (off) {
  case 0x004: wr16(&io[off], static_cast<u16>((old & 0x0007) | (nv & 0xFF38))); return;
  case 0x006: return;
  case 0x100: case 0x104: case 0x108: case 0x10C: {
    Timer& t = timers_[(off - 0x100) >> 2];
    t.reload = static_cast<u16>((t.reload & ~mask) | (v & mask));
    return;
  }
  case 0x102: case 0x106: case 0x10A: case 0x10E: {
    const int i = static_cast<int>((off - 0x100) >> 2);
    if (mask & 0xFF) timer_write_control(i, static_cast<u16>((timers_[i].control & ~mask) | (v & mask)));
    return;
  }
  case 0x128: {   // SIOCNT; bit 7 is busy, which only a transfer's end clears
    const u16 c = static_cast<u16>((nv & ~0x0080) | ((old | nv) & 0x0080 & (nv | 0xFF7F)));
    wr16(&io[off], c);
    if (!(old & 0x80) && (nv & 0x80) && !(rd16(&io[0x134]) & 0x8000)) sio_start(c);
    return;
  }
  case 0x130: return;
  case 0x132: wr16(&io[off], nv & 0xC3FF); check_keypad_irq(); return;
  case 0x200: ie = nv & 0x3FFF; update_irq(); return;
  case 0x202: if_ &= static_cast<u16>(~(v & mask)); update_irq(); return;
  case 0x204: wr16(&io[off], static_cast<u16>((nv & 0x5FFF) | 0x8000)); rebuild_timing(); return;
  case 0x208: ime = nv & 1; update_irq(); return;
  case 0x300:
    if (mask & 0x00FF) io[0x300] = static_cast<u8>(nv & 1);   // POSTFLG
    if (mask & 0xFF00) {                                       // HALTCNT: halt (bit 7 clear) or stop; both wait for an IRQ here
      io[0x301] = static_cast<u8>(nv >> 8);
      cpu->halted = true;   // the run loop leaves after this instruction
    }
    return;
  default: break;
  }
  if (off >= 0x60 && off < 0xB0) { audio.write16(off, v, mask); return; }
  if (off >= 0xB0 && off < 0xE0) {
    const int i = static_cast<int>((off - 0xB0) / 12);
    const u32 base = 0xB0 + static_cast<u32>(i) * 12, r = off - base;
    Dma& d = dma_[i];
    if (r == 10) { dma_write_control(i, static_cast<u16>((d.control & ~mask) | (v & mask))); return; }
    wr16(&io[off], nv);
    d.sad = rd32(&io[base]); d.dad = rd32(&io[base + 4]); d.count = rd16(&io[base + 8]);
    return;
  }
  wr16(&io[off], nv);
  if ((off >= 0x28 && off < 0x30) || (off >= 0x38 && off < 0x40)) ppu.ref_written(off);
}

// The CPU sees an interrupt IRQ_DELAY cycles after it is raised (mGBA's
// suite: a timer IRQ lets three or four more instructions run); it goes away
// at once when IE, IF or IME drop it. A halted CPU wakes as soon as IE & IF.
void Gba::update_irq() {
  const bool any = (ie & if_ & 0x3FFF) != 0;
  if (any && (ime & 1)) {
    if (!cpu->hot.irq_pending && ev_at_[static_cast<size_t>(Ev::Irq)] == ~u64{0}) schedule(Ev::Irq, now() + IRQ_DELAY);
  } else {
    cpu->hot.irq_pending = 0;
    if (ev_at_[static_cast<size_t>(Ev::Irq)] != ~u64{0}) cancel(Ev::Irq);
  }
  if (any && !in_cpu_ && ev_at_[static_cast<size_t>(Ev::Irq)] == ~u64{0}) cpu->halted = false;
}

void Gba::set_keys(u16 keyinput) {
  keyinput_ = keyinput & 0x3FF;
  check_keypad_irq();
}

void Gba::check_keypad_irq() {
  const u16 cnt = rd16(&io[0x132]);
  if (!(cnt & 0x4000)) return;
  const u16 want = cnt & 0x3FF, held = static_cast<u16>(~keyinput_ & 0x3FF);
  if ((cnt & 0x8000) ? (held & want) == want : (held & want) != 0) raise_irq(IRQ_KEYPAD);
}

// ---- events -----------------------------------------------------------------

void Gba::rescan() {
  next_at_ = ~u64{0};
  for (u64 t : ev_at_) next_at_ = std::min(next_at_, t);
}

void Gba::schedule(Ev e, u64 at) {
  ev_at_[static_cast<size_t>(e)] = at;
  next_at_ = std::min(next_at_, at);
  if (in_cpu_ && at < slice_end_) {   // end the running slice there
    const u64 t = std::max(at, now());
    const s32 d = static_cast<s32>(slice_end_ - t);
    cpu->hot.cycle_budget -= d; slice_budget_ -= d; slice_end_ = t;
  }
}

void Gba::cancel(Ev e) { ev_at_[static_cast<size_t>(e)] = ~u64{0}; rescan(); }

void Gba::fire(Ev e, u64 at) {
  switch (e) {
  case Ev::Ppu: if (in_hblank_) line_start(at); else hblank_start(at); break;
  case Ev::Timer0: case Ev::Timer1: case Ev::Timer2: case Ev::Timer3:
    timer_overflow(static_cast<int>(e) - static_cast<int>(Ev::Timer0), at); break;
  case Ev::Audio: audio.sample(at); schedule(Ev::Audio, at + Audio::SAMPLE_CYCLES); break;
  case Ev::Sio: sio_done(); break;
  case Ev::Irq:
    cpu->hot.irq_pending = ((ie & if_ & 0x3FFF) && (ime & 1)) ? 1u : 0u;
    if (cpu->hot.irq_pending) cpu->halted = false;
    break;
  default: break;
  }
}

void Gba::line_start(u64 at) {
  vcount_ = (vcount_ + 1) % LINES;
  in_hblank_ = false;
  const u16 stat = rd16(&io[4]);
  if (vcount_ == 160) {
    ppu.vblank();
    if (stat & 0x08) raise_irq(IRQ_VBLANK);
    dma_trigger(1);
    frame_done_ = true;
  }
  if (vcount_ == (stat >> 8) && (stat & 0x20)) raise_irq(IRQ_VCOUNT);
  schedule(Ev::Ppu, at + HDRAW_CYCLES);
}

void Gba::hblank_start(u64 at) {
  in_hblank_ = true;
  if (vcount_ < GBA_H) { ppu.render_line(vcount_); ppu.line_done(); }
  if (rd16(&io[4]) & 0x10) raise_irq(IRQ_HBLANK);
  if (vcount_ < GBA_H) dma_trigger(2);
  schedule(Ev::Ppu, at + (CYCLES_PER_LINE - HDRAW_CYCLES));
}

// ---- timers -----------------------------------------------------------------

u16 Gba::timer_read(int i) {
  const Timer& t = timers_[i];
  const u64 n = now();
  if (!timer_counting(i) || n < t.start) return t.counter;
  return static_cast<u16>(t.counter + ((n - t.start) >> timer_shift(t.control)));
}

void Gba::timer_freeze(int i) { timers_[i].counter = timer_read(i); timers_[i].start = std::max(now(), timers_[i].start); }

void Gba::timer_schedule(int i) {
  const Ev e = static_cast<Ev>(static_cast<int>(Ev::Timer0) + i);
  if (!timer_counting(i)) { if (ev_at_[static_cast<size_t>(e)] != ~u64{0}) cancel(e); return; }
  const Timer& t = timers_[i];
  schedule(e, t.start + ((u64{0x10000} - t.counter) << timer_shift(t.control)));
}

void Gba::timer_write_control(int i, u16 v) {
  Timer& t = timers_[i];
  if (timer_counting(i)) timer_freeze(i);
  const u16 old = t.control;
  t.control = v & (i == 0 ? 0xC3 : 0xC7);
  // Enabling: the reload is taken and the count starts 2 cycles after the write.
  if (!(old & 0x80) && (v & 0x80)) { t.counter = t.reload; t.start = now() + 2; }
  else t.start = std::max(now(), t.start);
  timer_schedule(i);
}

void Gba::timer_overflow(int i, u64 at) {
  Timer& t = timers_[i];
  t.counter = t.reload;
  t.start = at;
  if (t.control & 0x40) raise_irq(static_cast<u16>(IRQ_TIMER0 << i));
  if (i < 2) audio.timer_overflow(i, at);   // Direct Sound FIFO A/B (SOUNDCNT_H bits 10/14)
  if (i < 3) timer_tick_cascade(i + 1, at);
  timer_schedule(i);
}

void Gba::timer_tick_cascade(int i, u64 at) {
  Timer& t = timers_[i];
  if (!(t.control & 0x80) || !(t.control & 4)) return;
  if (++t.counter == 0) timer_overflow(i, at);
}

// ---- DMA --------------------------------------------------------------------

void Gba::dma_write_control(int i, u16 v) {
  Dma& d = dma_[i];
  const u16 old = d.control;
  d.control = v & (i == 3 ? 0xFFE0 : 0xF7E0);
  if (!(old & 0x8000) && (v & 0x8000)) {
    d.src = d.sad & (i == 0 ? 0x07FFFFFFu : 0x0FFFFFFFu);
    d.dst = d.dad & (i == 3 ? 0x0FFFFFFFu : 0x07FFFFFFu);
    const u32 n = i == 3 ? d.count : (d.count & 0x3FFF);
    d.left = n ? n : (i == 3 ? 0x10000u : 0x4000u);
    if (((d.control >> 12) & 3) == 0) dma_run(i);
  }
}

void Gba::dma_trigger(u32 timing) {
  for (int i = 0; i < 4; ++i)
    if ((dma_[i].control & 0x8000) && ((dma_[i].control >> 12) & 3) == timing) dma_run(i);
}

void Gba::dma_run(int i) {
  Dma& d = dma_[i];
  const u16 ctl = d.control;
  const u32 timing = (ctl >> 12) & 3;
  if (timing == 3) return;   // sound FIFO (DMA1/2: dma_fifo) and video capture (DMA3, not yet)
  const bool word = ctl & 0x400;
  const s32 step = word ? 4 : 2;
  const u32 dmode = (ctl >> 5) & 3, smode = (ctl >> 7) & 3;
  s32 sinc = smode == 1 ? -step : smode == 2 ? 0 : step;
  const s32 dinc = dmode == 1 ? -step : dmode == 2 ? 0 : step;
  if (d.src >= 0x08000000 && d.src < 0x0E000000) sinc = step;   // cartridge reads always increment
  const u32 n = d.left;
  u32 src = d.src, dst = d.dst;
  if ((dst >> 24) == 0x0D) save.eeprom_dma(n);   // an EEPROM's size, from its first command
  const u8* ts = timing_[src >> 15].data();
  const u8* td = timing_[dst >> 15].data();
  u64 cycles = 2;
  // A DMA's addresses are aligned to its width, even on the 8-bit save bus.
  // Below 0x02000000 (the BIOS, nothing) it reads what it last moved.
  for (u32 k = 0; k < n; ++k) {
    if (word) {
      if (src >= 0x02000000) dma_latch_ = read32(src & ~3u);
      write32(dst & ~3u, dma_latch_);
    } else {
      if (src >= 0x02000000) { const u16 v = read16(src & ~1u); dma_latch_ = v * 0x10001u; }
      write16(dst & ~1u, static_cast<u16>(dma_latch_ >> ((dst & 2) * 8)));
    }
    src += static_cast<u32>(sinc); dst += static_cast<u32>(dinc);
  }
  cycles += word ? (ts[2] + td[2] + (n - 1) * (ts[3] + td[3])) : (ts[0] + td[0] + (n - 1) * (ts[1] + td[1]));
  d.src = src;
  d.dst = dmode == 3 ? (d.dad & (i == 3 ? 0x0FFFFFFFu : 0x07FFFFFFu)) : dst;
  if ((ctl & 0x200) && timing != 0) {
    const u32 c = i == 3 ? d.count : (d.count & 0x3FFF);
    d.left = c ? c : (i == 3 ? 0x10000u : 0x4000u);
  } else {
    d.control &= 0x7FFF;
  }
  if (ctl & 0x4000) raise_irq(static_cast<u16>(IRQ_DMA0 << i));
  if (in_cpu_) cpu->hot.cycle_budget -= static_cast<s32>(cycles);
  else now_ += cycles;
}

void Gba::dma_fifo(int fifo, u64 at) {
  const u32 target = 0x040000A0u + 4u * static_cast<u32>(fifo);
  for (int i = 1; i <= 2; ++i) {
    Dma& d = dma_[i];
    if (!(d.control & 0x8000) || ((d.control >> 12) & 3) != 3 || (d.dad & ~3u) != target) continue;
    // Four words, whatever the count and width say; the destination stays put.
    const u32 smode = (d.control >> 7) & 3;
    const s32 sinc = smode == 1 ? -4 : smode == 2 ? 0 : 4;
    const u8* ts = timing_[d.src >> 15].data();
    for (int k = 0; k < 4; ++k) {
      const u32 v = d.src >= 0x02000000 ? read32(d.src) : dma_latch_;
      write32(target, v);
      dma_latch_ = v;
      d.src += static_cast<u32>(sinc);
    }
    if (d.control & 0x4000) raise_irq(static_cast<u16>(IRQ_DMA0 << i));
    if (!(d.control & 0x200)) d.control &= 0x7FFF;
    const u64 cycles = 2 + ts[2] + 3u * ts[3] + 4;   // + four 1-cycle I/O writes
    if (in_cpu_) cpu->hot.cycle_budget -= static_cast<s32>(cycles);
    else now_ = std::max(now_, at) + cycles;
    return;
  }
}

// ---- serial -----------------------------------------------------------------

void Gba::sio_start(u16 cnt) {
  const u32 mode = (cnt >> 12) & 3;
  u64 cycles;
  if (mode <= 1) {                         // normal, 8 or 32 bits
    if (!(cnt & 1)) return;                // external clock: no partner to clock it
    cycles = (mode ? 32u : 8u) * ((cnt & 2) ? 8u : 64u);   // 2 MHz or 256 kHz
  } else if (mode == 2) {                  // multiplayer, as the parent
    static const u32 baud[4] = {9600, 38400, 57600, 115200};
    cycles = u64{CLOCK_HZ} * 18 * 4 / baud[cnt & 3];       // four units of start, 16 bits, stop
  } else return;                           // UART: nothing to talk to
  schedule(Ev::Sio, now() + cycles);
}

void Gba::sio_done() {
  const u16 cnt = rd16(&io[0x128]);
  const u32 mode = (cnt >> 12) & 3;
  if (mode == 1) { wr16(&io[0x120], 0xFFFF); wr16(&io[0x122], 0xFFFF); }
  else if (mode == 0) io[0x12A] = 0xFF;
  else if (mode == 2) {
    wr16(&io[0x120], rd16(&io[0x12A]));    // its own word, then nobody
    wr16(&io[0x122], 0xFFFF); wr16(&io[0x124], 0xFFFF); wr16(&io[0x126], 0xFFFF);
  }
  wr16(&io[0x128], static_cast<u16>(cnt & ~0x0080));
  if (cnt & 0x4000) raise_irq(IRQ_SERIAL);
}

// ---- running ----------------------------------------------------------------

void Gba::reset() {
  cpu->reset(Cpu::ARM7, nullptr);
  cpu->host = this;
  std::memset(ewram.get(), 0, EWRAM_SIZE);
  std::memset(iwram.get(), 0, IWRAM_SIZE);
  std::memset(palette.get(), 0, 2 * PALETTE_SIZE);
  std::memset(vram.get(), 0, VRAM_SIZE);
  std::memset(oam.get(), 0, 2 * OAM_SIZE);
  io.fill(0);
  wr16(&io[0x88], 0x0200);   // SOUNDBIAS
  for (u32 m : {0x20u, 0x26u, 0x30u, 0x36u}) wr16(&io[m], 0x0100);   // BG2/BG3 PA and PD: identity
  wr16(&io[0x204], 0x8000);
  ie = if_ = ime = 0;
  timers_ = {};
  dma_ = {};
  ev_at_.fill(~u64{0});
  next_at_ = ~u64{0};
  now_ = 0;
  in_cpu_ = false;
  vcount_ = 0;
  in_hblank_ = false;
  keyinput_ = 0x3FF;
  dma_latch_ = 0;
  bios_latch_ = 0;
  pipe = {};
  pf = {};
  frame_count = 0;
  rebuild_timing();
  ppu.reset();
  audio.reset();
  save.reset();
  rtc.reset();
  map_gpio();
  schedule(Ev::Ppu, HDRAW_CYCLES);
  schedule(Ev::Audio, Audio::SAMPLE_CYCLES);
  if (skip_bios) {
    // Where the BIOS leaves things when it jumps to the cartridge.
    cpu->hot.regs[13] = 0x03007FE0;           // SVC (the mode after reset)
    cpu->bank_r13[2] = 0x03007FA0;            // IRQ
    cpu->set_cpsr(0x1F);                      // SYS
    cpu->hot.regs[13] = 0x03007F00;
    cpu->hot.regs[15] = 0x08000000 + 8;
    cpu->code_cycles = 0x08000000 >> 15; cpu->code_region = 0x08;
    bios_latch_ = 0xE129F000;
    io[0x300] = 1;                            // POSTFLG
    // The I/O the BIOS leaves (as it and mGBA do): forced blank -- games that
    // queue register writes for their VBlank handler write directly while it
    // is set (Drill Dozer, Sonic Advance 2) -- the sound mixer's defaults, and
    // the serial port in general-purpose mode.
    io_write16(0x000, 0x0080);
    audio.write16(0x82, 0x880E, 0xFFFF);
    audio.write16(0x88, 0x0200, 0xFFFF);
    io_write16(0x134, 0x8000);
  }
}

void Gba::run_cpu_until(u64 until) {
  // Halt ends on IE & IF, IME or not -- unless an IRQ is on its way to the CPU:
  // then it sleeps until it arrives (the BIOS's IntrWait halts just after IME = 1).
  if (cpu->halted && (ie & if_ & 0x3FFF) && ev_at_[static_cast<size_t>(Ev::Irq)] == ~u64{0}) cpu->halted = false;
  if (cpu->halted) { now_ = until; return; }
  const s64 slice = std::min<s64>(static_cast<s64>(until - now_), s64{1} << 20);
  slice_budget_ = static_cast<s32>(slice);
  cpu->hot.cycle_budget = slice_budget_;
  slice_end_ = now_ + static_cast<u64>(slice);
  in_cpu_ = true;
  gba_interp::run(*cpu);
  in_cpu_ = false;
  now_ += static_cast<u64>(static_cast<s64>(slice_budget_) - cpu->hot.cycle_budget);
  if (cpu->halted && (ie & if_ & 0x3FFF)) cpu->halted = false;   // halted with an IRQ already pending
}

void Gba::run_frame() {
  ExactTiming exact;
  frame_done_ = false;
  while (!frame_done_) {
    if (now_ < next_at_) run_cpu_until(next_at_);
    while (next_at_ <= now_) {
      size_t e = 0;
      for (size_t k = 1; k < ev_at_.size(); ++k) if (ev_at_[k] < ev_at_[e]) e = k;
      const u64 at = ev_at_[e];
      ev_at_[e] = ~u64{0};
      rescan();
      fire(static_cast<Ev>(e), at);
    }
  }
  ++frame_count;
}

// ---- save states ------------------------------------------------------------

// "GBA2"; a "GBAM" chunk (dsperate .47/.49) also had 64 KB of SRAM in it
// and no save, RTC or IRQ-delay state: it still loads.
template <class S> void Gba::sync_state(S& s) {
  bool old = false;
  if constexpr (S::reading) old = s.next_is("GBAM");
  s.begin(old ? "GBAM" : "GBA2");
  s.blob(ewram.get(), EWRAM_SIZE);
  s.blob(iwram.get(), IWRAM_SIZE);
  s.blob(palette.get(), 2 * PALETTE_SIZE);
  s.blob(vram.get(), VRAM_SIZE);
  s.blob(oam.get(), 2 * OAM_SIZE);
  if constexpr (S::reading) {
    if (old) {
      std::vector<u8> sram(64 * 1024);
      s.blob(sram.data(), sram.size());
      if (save.type() == Save::Type::None) save.set_type(Save::Type::Sram);
      if (save.type() == Save::Type::Sram) save.load(sram.data(), 0x8000);
    }
  }
  s.blob(io.data(), IO_SIZE);
  s.fields(ie, if_, ime);
  for (size_t k = 0; k <= static_cast<size_t>(Ev::Audio); ++k) s.put(ev_at_[k]);
  s.fields(now_, vcount_, in_hblank_, keyinput_, bios_latch_, dma_latch_, frame_count);
  for (Timer& t : timers_) s.fields(t.reload, t.control, t.counter, t.start);
  for (Dma& d : dma_) s.fields(d.sad, d.dad, d.count, d.control, d.src, d.dst, d.left);
  if constexpr (S::reading) ev_at_[static_cast<size_t>(Ev::Irq)] = ev_at_[static_cast<size_t>(Ev::Sio)] = ~u64{0};
  s.put(ev_at_[static_cast<size_t>(Ev::Irq)]);
  s.put(ev_at_[static_cast<size_t>(Ev::Sio)]);
  s.end();
  cpu->sync_state(s);
  ppu.sync_state(s);
  audio.sync_state(s);
  if (s.next_is("GSAV")) save.sync_state(s);
  if (s.next_is("GRTC")) rtc.sync_state(s);
}
template void Gba::sync_state<state::Writer>(state::Writer&);
template void Gba::sync_state<state::Reader>(state::Reader&);

void Gba::after_load() {
  cpu->host = this;
  map_gpio();
  pipe = {};
  rebuild_timing();
  rescan();
  update_irq();
}

} // namespace ds::gba
