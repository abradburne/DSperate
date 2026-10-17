// SPDX-License-Identifier: GPL-3.0-or-later
// The GBA machine (core/gba): jsmolka's gba-tests (data/gba, MIT) end on
// the screen mGBA draws for them, plus the bus quirks, timers, DMA, IRQs and
// a save-state round trip on a cartridge that only loops.
#include "check.h"
#include "core/gba/gba.h"
#include "core/state/state.h"
#include "core/psg/gb_psg.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace ds;
using namespace ds::gba;

namespace {

std::vector<u8> slurp(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<u8>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// FNV-1a over the picture at the GBA's 5 bits a channel (r, g, b): the
// same hash of mGBA 0.10.5's frame, whatever either expands colours to.
u64 picture_hash(const Gba& g) {
  u64 h = 1469598103934665603ull;
  const u32* px = g.framebuffer();
  for (u32 i = 0; i < GBA_W * GBA_H; ++i)
    for (u32 v : {(px[i] >> 19) & 31, (px[i] >> 11) & 31, (px[i] >> 3) & 31}) h = (h ^ v) * 1099511628211ull;
  return h;
}

// Frame 60 of each, as mGBA 0.10.5 shows it (with the same BIOS, booted past).
constexpr u64 kAllTestsPassed = 0x72eda4134bfd9363ull;
struct Demo { const char* rom; u64 hash; };
constexpr Demo kDemos[] = {{"hello.gba", 0xd3cb40cd035fdfb6ull}, {"shades.gba", 0x72d82bb4bcaaeb83ull}, {"stripes.gba", 0x5782997931352783ull}};

void run_rom(Gba& g, const char* name, int frames) {
  std::vector<u8> rom = slurp(std::string(GBA_TEST_DATA) + "/" + name);
  CHECK(!rom.empty());
  CHECK(g.load_rom(std::move(rom)));
  g.reset();
  for (int i = 0; i < frames; ++i) g.run_frame();
}

void test_suites() {
  // r12 holds the first failing test's number (0: all passed).
  for (const char* t : {"arm.gba", "thumb.gba", "memory.gba", "bios.gba", "nes.gba"}) {
    Gba g;
    run_rom(g, t, 60);
    if (g.cpu->hot.regs[12] != 0) std::fprintf(stderr, "%s: failed test %u\n", t, g.cpu->hot.regs[12]);
    CHECK(g.cpu->hot.regs[12] == 0);
    CHECK(picture_hash(g) == kAllTestsPassed);
  }
  for (const Demo& d : kDemos) {
    Gba g;
    run_rom(g, d.rom, 60);
    if (picture_hash(g) != d.hash) std::fprintf(stderr, "%s: picture %016llx, mGBA's %016llx\n", d.rom, (unsigned long long)picture_hash(g), (unsigned long long)d.hash);
    CHECK(picture_hash(g) == d.hash);
  }
}

// A cartridge whose entry point branches to itself.
std::vector<u8> loop_cart() {
  std::vector<u8> rom(0x200, 0);
  const u32 b_self = 0xEAFFFFFE;
  std::memcpy(rom.data(), &b_self, 4);
  return rom;
}

void test_bus() {
  Gba g;
  CHECK(g.load_rom(loop_cart()));
  g.reset();
  // Mirrors.
  g.write32(0x02000010, 0x11223344);
  CHECK(g.read32(0x02040010) == 0x11223344 && g.read32(0x02FC0010) == 0x11223344);
  g.write32(0x03000020, 0x55667788);
  CHECK(g.read32(0x03008020) == 0x55667788 && g.read32(0x03FF8020) == 0x55667788);
  CHECK(g.read32(0x0A000000) == 0xEAFFFFFE && g.read32(0x0C000000) == 0xEAFFFFFE);   // wait-state mirrors
  // Past the end of the cartridge: the address bus, halfword by halfword.
  CHECK(g.read16(0x08100000) == static_cast<u16>(0x08100000 >> 1));
  // Byte stores: palette and BG VRAM widen to both halves, OBJ VRAM and OAM drop them.
  g.write8(0x05000003, 0x12);
  CHECK(g.read16(0x05000002) == 0x1212 && g.read16(0x05000402) == 0x1212);
  g.write8(0x06000001, 0x34);
  CHECK(g.read16(0x06000000) == 0x3434);
  g.write8(0x06010000, 0x56);
  CHECK(g.read16(0x06010000) == 0);
  g.write16(0x06010000, 0x789A);
  CHECK(g.read16(0x06018000) == 0x789A);   // the last 32 KB mirror the OBJ 32 KB
  g.write8(0x07000000, 0x77);
  CHECK(g.read16(0x07000000) == 0);
  // SRAM: an 8-bit bus.
  g.write8(0x0E000004, 0xAB);
  CHECK(g.read8(0x0E008004) == 0xAB && g.read16(0x0E000004) == 0xABAB && g.read32(0x0E000004) == 0xABABABAB && g.save_dirty);
  // I/O: IF clears by writing ones, VCOUNT is read-only, KEYINPUT follows set_keys.
  g.if_ = 0x0005;
  g.write16(0x04000202, 0x0001);
  CHECK(g.if_ == 0x0004);
  g.set_keys(0x3FE);
  CHECK(g.read16(0x04000130) == 0x3FE);
}

void test_timers_dma_irq() {
  Gba g;
  CHECK(g.load_rom(loop_cart()));
  g.reset();
  // Timer 0 overflowing every 256 cycles, cascading into timer 1.
  g.write16(0x04000100, 0xFF00);
  g.write16(0x04000102, 0x00C0);   // enable, IRQ, prescaler 1
  g.write16(0x04000106, 0x0084);   // timer 1: enable, count-up
  g.run_frame();                    // to line 160: 197120 cycles
  CHECK(g.if_ & IRQ_TIMER0);
  const u16 t1 = g.read16(0x04000104);
  CHECK(t1 >= 197120 / 256 - 2 && t1 <= 197120 / 256 + 1);
  CHECK(g.read16(0x04000006) == 160);
  CHECK(g.read16(0x04000004) & 1);  // in VBlank
  // Immediate DMA3, 32-bit, 4 words EWRAM -> IWRAM, with its end IRQ.
  for (u32 i = 0; i < 4; ++i) g.write32(0x02000100 + i * 4, 0xA0 + i);
  g.write32(0x040000D4, 0x02000100);
  g.write32(0x040000D8, 0x03000200);
  g.write32(0x040000DC, 0xC4000004);   // enable, IRQ, 32-bit, immediate, count 4
  for (u32 i = 0; i < 4; ++i) CHECK(g.read32(0x03000200 + i * 4) == 0xA0 + i);
  CHECK(!(g.read16(0x040000DE) & 0x8000));   // done
  CHECK(g.if_ & (IRQ_DMA0 << 3));
  // IRQ line: IME and IE gate it.
  g.write16(0x04000200, IRQ_VBLANK);
  CHECK(!g.cpu->hot.irq_pending);
  g.write16(0x04000208, 1);
  g.if_ |= IRQ_VBLANK; g.update_irq();
  CHECK(g.cpu->hot.irq_pending);
}

void test_state_round_trip() {
  Gba g;
  run_rom(g, "hello.gba", 30);
  state::Writer w;
  g.sync_state(w);
  const u64 before = picture_hash(g);
  const u64 t = g.now();
  for (int i = 0; i < 10; ++i) g.run_frame();
  const u64 later = picture_hash(g);
  g.write32(0x02000000, 0xDEADBEEF);
  state::Reader r(w.data().data(), w.data().size());
  g.sync_state(r);
  CHECK(r.ok());
  g.after_load();
  CHECK(g.now() == t && picture_hash(g) == before && g.read32(0x02000000) != 0xDEADBEEF);
  for (int i = 0; i < 10; ++i) g.run_frame();
  CHECK(picture_hash(g) == later);
}

u32 GbPsg_clock_quarter() { return psg::GbPsg::CLOCK_HZ / 4; }

// The Game Boy PSG on its own: a square's pitch, the length counter, an envelope.
void test_psg() {
  psg::GbPsg p(psg::GbPsg::Model::Agb);
  p.write(0x26, 0x80);                 // on
  p.write(0x24, 0x77); p.write(0x25, 0xFF);
  p.write(0x16, 0x80);                 // channel 2: 50% duty, length 64
  p.write(0x17, 0xF0);                 // volume 15, no envelope
  p.write(0x18, 0x00); p.write(0x19, 0x87);   // frequency 1792: (2048 - 1792) * 4 * 8 cycles = 512 Hz; trigger
  int changes = 0, prev = 0;
  for (u32 i = 0; i < 32768; ++i) {    // one second at 32768 samples
    p.run(128);
    s32 lv[4], l, r;
    p.sample(lv);
    p.mix(lv, l, r);
    const int sign = l > 0 ? 1 : l < 0 ? -1 : 0;
    if (sign && prev && sign != prev) ++changes;
    if (sign) prev = sign;
  }
  CHECK(changes >= 1020 && changes <= 1026);   // two per cycle
  CHECK(p.read(0x26) & 2);
  // Length: 1 step left, counted at 256 Hz once enabled.
  p.write(0x16, 0xBF); p.write(0x19, 0xC7);
  p.run(8192 * 2 + 1);
  CHECK(!(p.read(0x26) & 2));
  // Envelope: from 15 down one step every 1/64 s; silent within a quarter second.
  p.write(0x17, 0xF1); p.write(0x19, 0x87);
  p.run(GbPsg_clock_quarter());
  s32 lv[4]; p.sample(lv);
  p.run(128); p.sample(lv);
  CHECK(lv[1] == -15);                 // volume 0: the DAC's floor
  CHECK(p.read(0x11) == 0);            // AGB: write-only bits read 0
}

// Direct Sound: timer 0 at 32768 Hz pops FIFO A, DMA1 refills it from EWRAM.
void test_direct_sound() {
  Gba g;
  CHECK(g.load_rom(loop_cart()));
  g.reset();
  for (u32 i = 0; i < 4096; ++i) g.write8(0x02000000 + i, (i & 1) ? 0x81 : 0x7F);   // +127, -127 (DMA walks on through it)
  g.write16(0x04000084, 0x0080);       // master on
  g.write16(0x04000082, 0x0B04);       // FIFO A: 100%, left and right, timer 0, reset
  g.write32(0x040000BC, 0x02000000);   // DMA1 from EWRAM ...
  g.write32(0x040000C0, 0x040000A0);   // ... to FIFO A
  g.write16(0x040000C6, 0xB640);       // enable, repeat, 32-bit, sound timing, destination fixed
  g.write16(0x04000100, static_cast<u16>(0x10000 - 512));
  g.write16(0x04000102, 0x0080);
  for (int i = 0; i < 3; ++i) g.run_frame();   // the first runs from reset to VBlank only
  const std::vector<s16>& out = g.audio.out;
  CHECK(out.size() == 2 * ((197120 + 2 * 280896) / 512));   // one frame per 512 cycles
  s16 lo = 0, hi = 0;
  for (size_t i = out.size() / 2; i < out.size(); i += 2) { lo = std::min(lo, out[i]); hi = std::max(hi, out[i]); }
  CHECK(hi > 12000 && lo < -12000);    // a full-scale square at 16 kHz
}

} // namespace

int main() {
  test_psg();
  test_direct_sound();
  test_bus();
  test_timers_dma_irq();
  test_suites();
  test_state_round_trip();
  std::puts("gba: ok");
  return 0;
}
