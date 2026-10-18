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
// Sound is gba_audio.cpp (the Game Boy PSG of core/psg plus Direct Sound).
// Not yet (phase 3/4 of pocketos/emu/gba-plan.md): save chips beyond plain
// SRAM (Flash, EEPROM), the prefetch buffer and exact wait-state timing,
// RTC/GPIO, the recompiler.
#pragma once
#include "core/types.h"
#include "core/cpu/cpu.h"
#include "core/mem/page_table.h"
#include "core/gba/gba_ppu.h"
#include "core/gba/gba_audio.h"
#include "core/gba/gba_save.h"
#include "core/gba/gba_rtc.h"

#include <array>
#include <cstdio>
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
constexpr u32 ROM_MAX = 32 * 1024 * 1024;
constexpr u32 SAVE_MAX = 128 * 1024;          // the largest save chip (Flash 128 KB)

// IF/IE bits.
enum Irq : u16 {
  IRQ_VBLANK = 1 << 0, IRQ_HBLANK = 1 << 1, IRQ_VCOUNT = 1 << 2,
  IRQ_TIMER0 = 1 << 3, IRQ_SERIAL = 1 << 7, IRQ_DMA0 = 1 << 8, IRQ_KEYPAD = 1 << 12, IRQ_GAMEPAK = 1 << 13,
};

enum class Ev : u8 { Ppu, Timer0, Timer1, Timer2, Timer3, Audio, Irq, Sio, Count };
// Cycles from an interrupt being raised (IE & IF with IME) to the CPU seeing it.
constexpr u32 IRQ_DELAY = 7;

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
  // The cartridge's save chip (gba_save.h), its type found by load_rom().
  Save save;
  // The cartridge's GPIO port and real-time clock (gba_rtc.h), if it has one.
  Rtc rtc;

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
  // Headless/tests: mGBA's debug-print port (0x04FFF600 string, 0x04FFF700
  // flags, 0x04FFF780 enable), which homebrew and mGBA's test suite log
  // through. Lines go here; null leaves the port unanswered, as on hardware.
  FILE* debug_out = nullptr;

  // The CPU's prefetched instructions (gba_interp.cpp): addresses | Thumb bit.
  struct Pipeline { u32 addr[2] = {}, val[2] = {}, n = 0; } pipe;

  // The cartridge prefetch buffer (WAITCNT bit 14), charged by
  // gba_cpu_cycles.h: while the CPU leaves the cartridge bus alone (internal
  // cycles, other memories) it reads the next halfwords of code ahead, up to
  // eight; a code fetch it already holds takes one cycle. `next` is the
  // halfword being fetched, the `count` before it are held.
  struct Prefetch { bool on = false, active = false; u32 next = 0, count = 0, progress = 0; } pf;
  u32  pf_fetch(u32 addr, u32 halfwords, bool seq);   // a code fetch from the cartridge: its cycles
  void pf_idle(u32 cycles);
  void pf_flush() { pf.active = false; pf.count = 0; pf.progress = 0; }
  void pf_restart(u32 addr) {
    pf.count = 0; pf.progress = 0;
    pf.active = pf.on && addr >= 0x08000000 && addr < 0x0E000000;
    pf.next = addr;
  }

  // ---- bus (gba_cpu_mem.h's slow paths; DMA uses the *_raw forms) ----
  u8  read8(u32 addr);
  u16 read16(u32 addr);
  u32 read32(u32 addr);
  void write8(u32 addr, u8 v);
  void write16(u32 addr, u16 v);
  void write32(u32 addr, u32 v);
  u32 fetch(u32 addr, u32 width);

  // I/O registers, 16 bits at a time (8- and 32-bit accesses are split).
  // io_open: nothing answers there (write-only or unused), the read is open bus.
  static bool io_open(u32 off);
  u16  io_read16(u32 off);
  void io_write16(u32 off, u16 v, u16 mask = 0xFFFF);   // mask: the bytes written

  u64 now() const { return in_cpu_ ? now_ + static_cast<u64>(slice_budget_ - cpu->hot.cycle_budget) : now_; }

  std::unique_ptr<CpuContext> cpu;
  mem::PageBuf bios, ewram, iwram, palette, vram, oam, rom;
  std::array<u8, IO_SIZE> io{};     // register backing (what reads see, where nothing computes it)
  Ppu ppu;
  Audio audio;

  // Interrupts.
  u16 ie = 0, if_ = 0, ime = 0;
  void raise_irq(u16 bits) { if_ |= bits; update_irq(); }
  // A Direct Sound FIFO (0 = A, 1 = B) is half empty: the DMA channel aimed
  // at it in sound timing (1 or 2, mode 3) moves four words.
  void dma_fifo(int fifo, u64 at);
  void update_irq();
  u64 event_at(Ev e) const { return ev_at_[static_cast<size_t>(e)]; }   // diagnostics

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

  // ---- serial port, nothing plugged in ----
  // A transfer on the internal clock (normal mode) or as the multiplayer
  // parent finishes when its bits would have, receiving all ones from no one;
  // on an external clock it waits for a partner that never comes.
  void sio_start(u16 cnt);
  void sio_done();

  // ---- misc ----
  u16 keyinput_ = 0x3FF;
  void check_keypad_irq();
  u32 bios_latch_ = 0;              // last BIOS opcode fetched: what a BIOS read outside the BIOS sees
  bool rrr_io_ = false;             // the built-in BIOS is resetting the video registers (Gba::fetch)
  u32 open_bus();
  // mGBA's debug port (debug_out).
  bool debug_on_ = false;
  char debug_str_[0x101] = {};
  bool debug_at(u32 addr) const { return debug_out && addr >= 0x04FFF600 && addr < 0x04FFF800; }
  u16  debug_read16(u32 addr) const { return addr == 0x04FFF780 && debug_on_ ? 0x1DEA : 0; }
  void debug_write(u32 addr, u32 v, u32 width);
  std::vector<std::array<u8, 4>> timing_;   // per 32 KB: N16 S16 N32 S32 (CpuContext::timing7)
  void rebuild_timing();
  void map_memory();
  bool eeprom_at(u32 addr) const;
  bool gpio_at(u32 addr) const { return rtc.present && addr >= 0x080000C4 && addr < 0x080000CA; }
  void map_gpio();   // the ROM page holding the GPIO port off the direct path while it reads back   // the EEPROM answers here (0x0D, or its last 256 bytes behind a 32 MB ROM)
  void run_cpu_until(u64 until);
  friend class Ppu;
};

} // namespace ds::gba
