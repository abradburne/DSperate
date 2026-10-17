// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// The Game Boy Advance machine: what a DS turns into when its ARM7 switches
// to GBA mode, as a machine of its own beside NDS. It reuses the ARM7TDMI
// (CpuContext and the interpreter, built a second time against this bus:
// gba_interp_*.cpp) and DSperate's state format; memory map, I/O, DMA,
// timers, the event loop and the PPU (gba_ppu.cpp) are the GBA's own, so
// nothing on the DS path changes for it.
//
// Time is counted in GBA cycles (16.78 MHz). A frame is 228 lines of 1232
// cycles; run_frame() runs to the start of VBlank (line 160), so the picture
// it leaves is the one just drawn.
//
// Not yet (phase 3/4 of pocketos/emu/gba-plan.md): sound (PSG + Direct Sound
// FIFOs; the registers are stored, timers already report overflows to
// timer_overflow()), save chips beyond plain SRAM (Flash, EEPROM), the
// prefetch buffer and exact wait-state timing, RTC/GPIO, the recompiler.
#pragma once
#include "core/types.h"
#include "core/cpu/cpu.h"
#include "core/mem/page_table.h"
#include "core/gba/gba_ppu.h"

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace ds::gba {

constexpr u32 GBA_W = 240, GBA_H = 160;
constexpr u32 CLOCK_HZ = 16'777'216;
constexpr u32 CYCLES_PER_LINE = 1232;
constexpr u32 HDRAW_CYCLES = 1006;          // HBlank flag, IRQ and DMA from here to the line's end
constexpr u32 LINES = 228;
constexpr u32 CYCLES_PER_FRAME = CYCLES_PER_LINE * LINES;   // 280896: 59.73 Hz

constexpr u32 BIOS_SIZE = 16 * 1024, EWRAM_SIZE = 256 * 1024, IWRAM_SIZE = 32 * 1024;
constexpr u32 IO_SIZE = 0x400, PALETTE_SIZE = 1024, VRAM_SIZE = 96 * 1024, OAM_SIZE = 1024;
constexpr u32 ROM_MAX = 32 * 1024 * 1024, SRAM_SIZE = 64 * 1024;

// IF/IE bits.
enum Irq : u16 {
  IRQ_VBLANK = 1 << 0, IRQ_HBLANK = 1 << 1, IRQ_VCOUNT = 1 << 2,
  IRQ_TIMER0 = 1 << 3, IRQ_SERIAL = 1 << 7, IRQ_DMA0 = 1 << 8, IRQ_KEYPAD = 1 << 12, IRQ_GAMEPAK = 1 << 13,
};

enum class Ev : u8 { Ppu, Timer0, Timer1, Timer2, Timer3, Count };

using TraceFn = void (*)(CpuContext& cpu, u32 instr, void* user);

struct Gba {
  Gba();
  ~Gba();

  // An empty path, or one that does not exist, takes the built-in
  // Cult-of-GBA BIOS (bios_native false). A file that exists must be 16 KB.
  bool load_bios(const std::string& path, std::string* err = nullptr);
  bool bios_native = false;
  u64  bios_id = 0;
  // The cartridge image; padded/mirrored to 32 MB as the bus sees it.
  bool load_rom(std::vector<u8> image, std::string* err = nullptr);
  u64  rom_id = 0;
  u32  rom_size = 0;
  std::string title() const;       // header 0xA0, 12 characters, trailing NULs cut
  u32 game_code() const;           // header 0xAC, as a little-endian u32
  // Plain SRAM (32 KB, mirrored to 64 KB). Flash/EEPROM are phase 3.
  std::vector<u8> save_data() const;
  bool load_save(const u8* data, size_t n);
  bool save_dirty = false;         // SRAM written since the frontend last wrote it out (it clears this)
  u32 save_writes = 0;             // SRAM stores so far, for "has the game stopped saving"

  // Before reset(): start at the cartridge's entry with the registers the
  // BIOS leaves (the default), or run the BIOS's boot screen first.
  bool skip_bios = true;
  void reset();

  // Runs to the start of the next VBlank.
  void run_frame();
  u64 frame_count = 0;
  // KEYINPUT, 0 = held (bits 0-9: A B Select Start Right Left Up Down R L).
  void set_keys(u16 keyinput);
  // 240x160 0xAARRGGBB, the frame run_frame() just finished.
  const u32* framebuffer() const { return ppu.framebuffer(); }

  // Save states: the whole machine, between frames. Header and identity
  // checks are the caller's (NDS::save_state).
  template <class S> void sync_state(S& s);
  void after_load();

  TraceFn trace = nullptr;
  void* trace_user = nullptr;

  // The CPU's prefetched instructions (gba_interp.cpp): addresses | Thumb bit.
  struct Pipeline { u32 addr[2] = {}, val[2] = {}, n = 0; } pipe;

  // ---- bus (gba_cpu_mem.h's slow paths; DMA uses the *_raw forms) ----
  u8  read8(u32 addr);
  u16 read16(u32 addr);
  u32 read32(u32 addr);
  void write8(u32 addr, u8 v);
  void write16(u32 addr, u16 v);
  void write32(u32 addr, u32 v);
  u32 fetch(u32 addr, u32 width);

  // I/O registers, 16 bits at a time (8- and 32-bit accesses are split).
  u16  io_read16(u32 off);
  void io_write16(u32 off, u16 v, u16 mask = 0xFFFF);   // mask: the bytes written

  u64 now() const { return in_cpu_ ? now_ + static_cast<u64>(slice_budget_ - cpu->hot.cycle_budget) : now_; }

  std::unique_ptr<CpuContext> cpu;
  mem::PageBuf bios, ewram, iwram, palette, vram, oam, rom, sram;
  std::array<u8, IO_SIZE> io{};     // register backing (what reads see, where nothing computes it)
  Ppu ppu;

  // Interrupts.
  u16 ie = 0, if_ = 0, ime = 0;
  void raise_irq(u16 bits) { if_ |= bits; update_irq(); }
  void update_irq();

private:
  // ---- events ----
  std::array<u64, static_cast<size_t>(Ev::Count)> ev_at_{};
  u64 next_at_ = ~u64{0};
  void schedule(Ev e, u64 at);
  void cancel(Ev e);
  void rescan();
  void fire(Ev e, u64 at);
  u64 now_ = 0;
  bool in_cpu_ = false;
  s32 slice_budget_ = 0;
  u64 slice_end_ = 0;
  bool frame_done_ = false;

  // ---- PPU timing ----
  u32 vcount_ = 0;
  bool in_hblank_ = false;
  void line_start(u64 at);
  void hblank_start(u64 at);

  // ---- timers ----
  struct Timer {
    u16 reload = 0, control = 0, counter = 0;
    u64 start = 0;       // time `counter` was current (running, not cascaded)
  };
  std::array<Timer, 4> timers_{};
  static u32 timer_shift(u16 control) { static const u8 s[4] = {0, 6, 8, 10}; return s[control & 3]; }
  bool timer_counting(int i) const { return (timers_[i].control & 0x80) && !(i > 0 && (timers_[i].control & 4)); }
  u16  timer_read(int i);
  void timer_write_control(int i, u16 v);
  void timer_freeze(int i);
  void timer_schedule(int i);
  void timer_overflow(int i, u64 at);
  void timer_tick_cascade(int i, u64 at);

  // ---- DMA ----
  struct Dma {
    u32 sad = 0, dad = 0;   // as written
    u16 count = 0, control = 0;
    u32 src = 0, dst = 0, left = 0;   // internal, latched at enable
  };
  std::array<Dma, 4> dma_{};
  void dma_write_control(int i, u16 v);
  void dma_trigger(u32 timing);     // 1 VBlank, 2 HBlank (3 special: sound FIFO / video capture, phase 3)
  void dma_run(int i);
  u32 dma_latch_ = 0;               // last value a DMA moved (open bus for its reads)

  // ---- misc ----
  u16 keyinput_ = 0x3FF;
  void check_keypad_irq();
  u32 bios_latch_ = 0;              // last BIOS opcode fetched: what a BIOS read outside the BIOS sees
  u32 open_bus();
  std::vector<std::array<u8, 4>> timing_;   // per 32 KB: N16 S16 N32 S32 (CpuContext::timing7)
  void rebuild_timing();
  void map_memory();
  void run_cpu_until(u64 until);
  friend class Ppu;
};

} // namespace ds::gba
