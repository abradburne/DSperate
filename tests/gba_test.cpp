// SPDX-License-Identifier: GPL-3.0-or-later
// The GBA machine (core/gba): jsmolka's gba-tests (data/gba, MIT) end on
// the screen mGBA draws for them, plus the bus quirks, timers, DMA, IRQs and
// a save-state round trip on a cartridge that only loops.
#include "check.h"
#include "core/gba/gba.h"
#include "core/nds.h"
#include "core/state/state.h"
#include "core/psg/gb_psg.h"

#include <algorithm>
#include <ctime>
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
  for (const char* t : {"arm.gba", "thumb.gba", "memory.gba", "bios.gba", "nes.gba",
                        "save_sram.gba", "save_flash64.gba", "save_flash128.gba", "save_none.gba"}) {
    Gba g;
    run_rom(g, t, 120);   // the Flash tests wait out erase times
    if (g.cpu->hot.regs[12] != 0) std::fprintf(stderr, "%s: failed test %u\n", t, g.cpu->hot.regs[12]);
    CHECK(g.cpu->hot.regs[12] == 0);
    if (picture_hash(g) != kAllTestsPassed) std::fprintf(stderr, "%s: picture %016llx\n", t, (unsigned long long)picture_hash(g));
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
  CHECK(g.read8(0x0E008004) == 0xAB && g.read16(0x0E000004) == 0xABAB && g.read32(0x0E000004) == 0xABABABAB && g.save.dirty && g.save.type() == Save::Type::Sram);
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
  CHECK(!g.cpu->hot.irq_pending);     // the CPU sees it IRQ_DELAY cycles later
  g.write16(0x04000208, 0);
  CHECK(!g.cpu->hot.irq_pending);
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

// A state that fails part way through (here: cut short) is refused and
// leaves the machine exactly as it was, running on as if never tried.
void test_state_refused_intact() {
  NDS a, b;
  std::string err;
  const std::string rom = std::string(GBA_TEST_DATA) + "/hello.gba";
  CHECK(a.load_gba(rom, "", &err) && b.load_gba(rom, "", &err));
  for (int i = 0; i < 30; ++i) { a.run_frame(); b.run_frame(); }
  state::Writer w;
  CHECK(a.save_state(w, err));
  std::vector<u8> cut = w.data();
  cut.resize(cut.size() - 8);   // the last chunk cut short: everything before it read in
  for (int i = 0; i < 10; ++i) { a.run_frame(); b.run_frame(); }
  state::Reader r(cut.data(), cut.size());
  CHECK(!a.load_state(r, err));
  for (int i = 0; i < 10; ++i) { a.run_frame(); b.run_frame(); }
  CHECK(picture_hash(*a.gba) == picture_hash(*b.gba) && a.gba->now() == b.gba->now());
  CHECK(std::memcmp(a.gba->ewram.get(), b.gba->ewram.get(), EWRAM_SIZE) == 0);
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

// The other three: channel 1's sweep running the pitch up past the top,
// the wave channel from the AGB's second bank, the noise channel's LFSR.
void test_psg_channels() {
  auto levels = [](psg::GbPsg& p, u32 samples, int ch, s32& lo, s32& hi) {
    lo = 99; hi = -99;
    for (u32 i = 0; i < samples; ++i) { p.run(128); s32 lv[4]; p.sample(lv); lo = std::min(lo, lv[ch]); hi = std::max(hi, lv[ch]); }
  };
  psg::GbPsg p(psg::GbPsg::Model::Agb);
  p.write(0x26, 0x80);
  // Sweep: period 1, shift 1, up. 1024 -> 1536 at the first sweep clock,
  // 2304 (past 2047) at the second, which turns the channel off. The sweep
  // clocks at 128 Hz, so it is gone within two clocks: 1/64 s.
  p.write(0x10, 0x11); p.write(0x11, 0x80); p.write(0x12, 0xF0);
  p.write(0x13, 0x00); p.write(0x14, 0x84);
  CHECK(p.read(0x26) & 1);
  p.run(psg::GbPsg::CLOCK_HZ / 256);
  CHECK(p.read(0x26) & 1);
  p.run(psg::GbPsg::CLOCK_HZ / 64);
  CHECK(!(p.read(0x26) & 1));
  // Wave: with bank 1 playing the CPU writes bank 0; then bank 0 plays it.
  p.write(0x1A, 0xC0);
  for (u32 i = 0; i < 16; ++i) p.wave_write(i, static_cast<u8>(((2 * i) << 4) | (2 * i + 1)));   // a 0..15 ramp, twice
  p.write(0x1A, 0x80);
  CHECK(p.wave_read(0) == 0);             // the CPU now sees bank 1, untouched
  p.write(0x1C, 0x20);                    // 100%
  p.write(0x1D, 0x00); p.write(0x1E, 0x80);   // the slowest rate: 32 Hz a pass
  s32 lo, hi;
  levels(p, 8192, 2, lo, hi);
  CHECK(lo <= -13 && hi >= 13);
  p.write(0x1C, 0x40);                    // 50%: the samples shifted right, half the swing
  levels(p, 8192, 2, lo, hi);
  CHECK(hi - lo >= 12 && hi - lo <= 14);
  // Noise: 7-bit LFSR, volume 15, a length of 32 steps (1/8 s at 256 Hz).
  p.write(0x21, 0xF0); p.write(0x20, 0x20); p.write(0x22, 0x08); p.write(0x23, 0xC0);
  CHECK(p.read(0x26) & 8);
  levels(p, 1024, 3, lo, hi);
  CHECK(lo < 0 && hi > 0);
  p.run(psg::GbPsg::CLOCK_HZ / 8);
  CHECK(!(p.read(0x26) & 8));
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

// EEPROM through DMA 3, as games drive it: an 8 KB chip (17/81-bit commands),
// a block written and read back, and the bytes in mGBA's file order.
void test_eeprom() {
  Gba g;
  std::vector<u8> rom = loop_cart();
  const char tag[] = "EEPROM_V124";
  rom.resize(0x400);
  std::memcpy(rom.data() + 0x200, tag, sizeof tag);
  CHECK(g.load_rom(std::move(rom)));
  g.reset();
  CHECK(g.save.type() == Save::Type::EepromUnsized);
  auto dma3 = [&](u32 src, u32 dst, u32 n) {
    g.write32(0x040000D4, src); g.write32(0x040000D8, dst);
    g.write32(0x040000DC, 0x80000000u | n);   // enable, 16-bit, immediate, both increment
  };
  // Write: 1 0, block 5 (14 bits), 64 data bits, 0.
  const u64 data = 0x0123456789ABCDEFull;
  std::vector<u16> bits = {1, 0};
  for (int i = 13; i >= 0; --i) bits.push_back((5 >> i) & 1);
  for (int i = 63; i >= 0; --i) bits.push_back(static_cast<u16>((data >> i) & 1));
  bits.push_back(0);
  CHECK(bits.size() == 81);
  for (size_t i = 0; i < bits.size(); ++i) g.write16(0x02000000 + static_cast<u32>(i) * 2, bits[i]);
  dma3(0x02000000, 0x0D000000, 81);
  CHECK(g.save.type() == Save::Type::Eeprom8k && g.save.dirty);
  CHECK(g.read16(0x0D000000) == 1);   // ready
  // Read: 1 1, block 5, 0; then 68 bits back.
  bits = {1, 1};
  for (int i = 13; i >= 0; --i) bits.push_back((5 >> i) & 1);
  bits.push_back(0);
  for (size_t i = 0; i < bits.size(); ++i) g.write16(0x02000000 + static_cast<u32>(i) * 2, bits[i]);
  dma3(0x02000000, 0x0D000000, 17);
  dma3(0x0D000000, 0x02001000, 68);
  u64 got = 0;
  for (u32 i = 0; i < 4; ++i) CHECK((g.read16(0x02001000 + i * 2) & 1) == 0);
  for (u32 i = 4; i < 68; ++i) got = (got << 1) | (g.read16(0x02001000 + i * 2) & 1);
  CHECK(got == data);
  const std::vector<u8> file = g.save.data();
  CHECK(file.size() == 8192 && file[5 * 8] == 0x01 && file[5 * 8 + 7] == 0xEF);
}

// The save chip from the SDK's string in the ROM, as Save::detect and
// Gba::load_rom find it: anywhere in the image, at a word boundary (where
// the SDK puts it; a match elsewhere is some other data).
void test_save_detection() {
  struct Case { const char* tag; Save::Type type; };
  const Case cases[] = {
      {"SRAM_V113", Save::Type::Sram},       {"SRAM_F_V100", Save::Type::Sram},
      {"FLASH_V126", Save::Type::Flash64},   {"FLASH512_V131", Save::Type::Flash64},
      {"FLASH1M_V103", Save::Type::Flash128}, {"EEPROM_V124", Save::Type::EepromUnsized},
      {"NO_SAVE_HERE", Save::Type::None},
  };
  for (const Case& c : cases)
    for (u32 at : {0x400u, 0x7FF0u}) {
      std::vector<u8> rom = loop_cart();
      rom.resize(0x8000);
      std::memcpy(rom.data() + at, c.tag, std::strlen(c.tag));
      CHECK(Save::detect(rom.data(), static_cast<u32>(rom.size())) == c.type);
      Gba g;
      CHECK(g.load_rom(std::move(rom)));
      CHECK(g.save.type() == c.type);
    }
  // The 128 KB chip's tag also contains the 64 KB one's prefix: the larger wins.
  std::vector<u8> rom = loop_cart();
  rom.resize(0x1000);
  std::memcpy(rom.data() + 0x200, "FLASH_V126", 10);
  std::memcpy(rom.data() + 0x300, "FLASH1M_V103", 12);
  CHECK(Save::detect(rom.data(), static_cast<u32>(rom.size())) == Save::Type::Flash128);
  std::vector<u8> odd = loop_cart();
  odd.resize(0x1000);
  std::memcpy(odd.data() + 0x402, "SRAM_V113", 9);
  CHECK(Save::detect(odd.data(), static_cast<u32>(odd.size())) == Save::Type::None);
}

// WAITCNT: every value, against GBATEK's table. Cycles per access are
// 1 + the wait states; a 32-bit access on the 16-bit cartridge bus is a
// 16-bit N and an S (S and S in a burst).
void test_waitcnt() {
  Gba g;
  CHECK(g.load_rom(loop_cart()));
  g.reset();
  static const u32 first[4] = {4, 3, 2, 8};
  auto at = [&](u32 addr) { return g.cpu->timing7[addr >> 15]; };
  for (u32 w = 0; w < 0x800; ++w) {
    g.write16(0x04000204, static_cast<u16>(w));
    const u32 sram = 1 + first[w & 3];
    const u32 ws[3][2] = {{1 + first[(w >> 2) & 3], (w & 0x10) ? 2u : 3u},
                          {1 + first[(w >> 5) & 3], (w & 0x80) ? 2u : 5u},
                          {1 + first[(w >> 8) & 3], (w & 0x400) ? 2u : 9u}};
    for (u32 k = 0; k < 3; ++k)
      for (u32 mirror : {0u, 0x01000000u, 0x01FF8000u}) {
        const u8* t = at(0x08000000 + k * 0x02000000 + mirror);
        const u32 n = ws[k][0], s = ws[k][1];
        if (t[0] != n || t[1] != s || t[2] != n + s || t[3] != 2 * s) {
          std::fprintf(stderr, "WAITCNT %03x: wait state %u %08x: %u %u %u %u, want %u %u %u %u\n", w, k, 0x08000000 + k * 0x02000000 + mirror, t[0], t[1], t[2], t[3], n, s, n + s, 2 * s);
          CHECK(false);
        }
      }
    for (u32 a : {0x0E000000u, 0x0E00FFFFu, 0x0F000000u}) { const u8* t = at(a); CHECK(t[0] == sram && t[1] == sram && t[2] == sram && t[3] == sram); }
  }
  // The rest never changes: BIOS and IWRAM single cycles, EWRAM 3 (16-bit
  // bus), palette and VRAM 1 (16-bit bus), OAM 1.
  for (u32 w : {0x0000u, 0x4317u, 0x07FFu}) {
    g.write16(0x04000204, static_cast<u16>(w));
    auto is = [&](u32 addr, u32 n16, u32 n32) { const u8* t = at(addr); return t[0] == n16 && t[1] == n16 && t[2] == n32 && t[3] == n32; };
    CHECK(is(0x00000000, 1, 1) && is(0x03000000, 1, 1) && is(0x02000000, 3, 6) && is(0x02038000, 3, 6));
    CHECK(is(0x05000000, 1, 2) && is(0x06000000, 1, 2) && is(0x07000000, 1, 1));
  }
}

// The cartridge RTC, driven pin by pin as Pokemon's code does it.
void test_rtc() {
  Gba g;
  std::vector<u8> rom = loop_cart();
  rom.resize(0x400);
  std::memcpy(rom.data() + 0x300, "SIIRTC_V001", 11);
  CHECK(g.load_rom(std::move(rom)));
  g.reset();
  CHECK(g.rtc.present);
  std::tm when{};
  when.tm_year = 126; when.tm_mon = 9; when.tm_mday = 17; when.tm_hour = 21; when.tm_min = 5; when.tm_sec = 30; when.tm_isdst = -1;
  g.rtc.fixed_now = static_cast<s64>(std::mktime(&when));   // Sat 17 Oct 2026, 21:05:30
  const u32 DATA = 0x080000C4, DIR = 0x080000C6, CTL = 0x080000C8;
  g.write16(CTL, 1);                       // readable
  CHECK(g.read16(CTL) == 1);
  auto begin = [&] { g.write16(DIR, 7); g.write16(DATA, 1); g.write16(DATA, 5); };   // SCK high, then CS high
  auto end = [&] { g.write16(DATA, 1); };
  auto out_byte = [&](u8 b) {
    for (int i = 0; i < 8; ++i) { const u16 sio = static_cast<u16>(((b >> i) & 1) << 1); g.write16(DATA, 4 | sio); g.write16(DATA, 5 | sio); }
  };
  auto in_byte = [&] {
    g.write16(DIR, 5);                     // SIO from the chip
    u8 b = 0;
    for (int i = 0; i < 8; ++i) { g.write16(DATA, 4); g.write16(DATA, 5); b |= static_cast<u8>(((g.read16(DATA) >> 1) & 1) << i); }
    return b;
  };
  auto read_datetime = [&](u8 out[7]) { begin(); out_byte(0xA6); for (int i = 0; i < 7; ++i) out[i] = in_byte(); end(); };
  u8 t[7];
  read_datetime(t);
  CHECK(t[0] == 0x26 && t[1] == 0x10 && t[2] == 0x17 && t[3] == 6);   // 2026-10-17, Saturday
  CHECK((t[4] & 0x3F) == 0x21 && t[5] == 0x05 && t[6] == 0x30);
  // Set 2001-02-03 04:05:06 and read it back.
  begin(); out_byte(0x26);
  for (u8 b : {u8{0x01}, u8{0x02}, u8{0x03}, u8{0x06}, u8{0x04}, u8{0x05}, u8{0x06}}) out_byte(b);
  end();
  read_datetime(t);
  CHECK(t[0] == 0x01 && t[1] == 0x02 && t[2] == 0x03 && (t[4] & 0x3F) == 0x04 && t[5] == 0x05 && t[6] == 0x06);
  // The ROM around the port still reads as ROM.
  CHECK(g.read32(0x08000000) == 0xEAFFFFFE);
}

} // namespace

int main() {
  test_save_detection();
  test_waitcnt();
  test_rtc();
  test_eeprom();
  test_psg();
  test_psg_channels();
  test_direct_sound();
  test_bus();
  test_timers_dma_irq();
  test_suites();
  test_state_round_trip();
  test_state_refused_intact();
  std::puts("gba: ok");
  return 0;
}
