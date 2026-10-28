// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// The RetroAchievements memory window and the condition runtime on top of it.
// Everything here is local: no session, no server, no network.
//
// Three things are being pinned down, and each has a failure mode that is
// silent rather than loud:
//
//   * The address translation. Get it wrong and achievements evaluate against
//     the wrong bytes -- they do not error, they just never fire, or worse,
//     fire when they should not.
//   * Unbacked addresses reading zero *and reporting themselves unbacked*, so
//     rcheevos disables those achievements instead of comparing against
//     whatever the DS left lying around.
//   * One rc_runtime_do_frame per emulated frame. rcheevos keeps a delta per
//     memory reference, so an extra call per frame corrupts hit counts and edge
//     triggers with nothing in any log to say so. The hit-count test below is
//     the regression guard for that rule.
#include <cstdio>
#include <cstring>
#include <string>

#include "check.h"
#include "cheevos/cheevos_identity.h"
#include "cheevos/cheevos_memory.h"
#include "core/gba/gba.h"
#include "core/mem/bus.h"
#include "core/nds.h"

extern "C" {
#include "rc_runtime.h"
}

using namespace ds;

namespace {

// The RetroAchievements addresses, as rc_consoles.h publishes them for the DS.
constexpr u32 RA_MAIN = 0x000000;                       // 4 MB of main RAM
constexpr u32 RA_HOLE = 0x400000;                       // 12 MB of DSi-only padding
constexpr u32 RA_DTCM = 0x1000000;                      // 16 KB of data TCM
constexpr u32 MAIN_LEN = mem::Bus::MAIN_RAM_SIZE;
constexpr u32 DTCM_LEN = mem::Bus::DTCM_SIZE;

struct Fixture {
  NDS nds;
  cheevos::Memory mem;
  Fixture() {
    std::string err;
    CHECK(mem.attach(nds, err));
    CHECK(err.empty());
    CHECK(mem.attached());
  }
  u8* main_ram() { return nds.bus.main_ram.get(); }
  u8* dtcm() { return nds.bus.dtcm.get(); }
};

void the_map_is_the_one_we_expect() {
  Fixture f;
  // Main RAM and data TCM, with the hole between them. The last address the map
  // describes is the end of DTCM.
  CHECK(f.mem.max_address() == RA_DTCM + DTCM_LEN - 1);
  CHECK(f.mem.supported(RA_MAIN));
  CHECK(f.mem.supported(RA_MAIN + MAIN_LEN - 1));
  CHECK(f.mem.supported(RA_DTCM));
  CHECK(f.mem.supported(RA_DTCM + DTCM_LEN - 1));
}

void main_ram_and_dtcm_read_through() {
  Fixture f;
  f.main_ram()[0] = 0x11;
  f.main_ram()[MAIN_LEN - 1] = 0x22;
  f.dtcm()[0] = 0x33;
  f.dtcm()[DTCM_LEN - 1] = 0x44;

  u8 b = 0;
  CHECK(f.mem.read(RA_MAIN, &b, 1) == 1 && b == 0x11);
  CHECK(f.mem.read(RA_MAIN + MAIN_LEN - 1, &b, 1) == 1 && b == 0x22);
  CHECK(f.mem.read(RA_DTCM, &b, 1) == 1 && b == 0x33);
  CHECK(f.mem.read(RA_DTCM + DTCM_LEN - 1, &b, 1) == 1 && b == 0x44);

  // A multi-byte read, little-endian through peek() as rcheevos sees it.
  f.main_ram()[0x100] = 0x78; f.main_ram()[0x101] = 0x56;
  f.main_ram()[0x102] = 0x34; f.main_ram()[0x103] = 0x12;
  CHECK(cheevos::Memory::peek(RA_MAIN + 0x100, 4, &f.mem) == 0x12345678u);
  CHECK(cheevos::Memory::peek(RA_MAIN + 0x100, 2, &f.mem) == 0x5678u);
  CHECK(cheevos::Memory::peek(RA_MAIN + 0x100, 1, &f.mem) == 0x78u);
}

// The DSi-only hole. It is a real region in rcheevos' map, and we must answer
// for it with zero and "unbacked" -- not with whatever the DS has at that
// address, which is how a condition written against DSi RAM turns into a false
// unlock instead of a disabled achievement.
void the_dsi_hole_reads_zero_and_is_unbacked() {
  Fixture f;
  std::memset(f.main_ram(), 0xFF, 64);

  u8 b[8];
  std::memset(b, 0xAA, sizeof b);
  CHECK(f.mem.read(RA_HOLE, b, sizeof b) == 0);         // nothing backed
  for (u8 v : b) CHECK(v == 0);                          // and it reads zero
  CHECK(!f.mem.supported(RA_HOLE));
  CHECK(!f.mem.supported(RA_HOLE + 0xBFFFFF));
  CHECK(cheevos::Memory::peek(RA_HOLE, 4, &f.mem) == 0);
}

// A read that starts in main RAM and runs into the hole has to report the
// partial count: that is the signal rcheevos uses to decide an address is not
// fully supported here.
void a_read_across_the_boundary_is_partial() {
  Fixture f;
  std::memset(f.main_ram() + MAIN_LEN - 2, 0x5A, 2);

  u8 b[4];
  std::memset(b, 0xAA, sizeof b);
  CHECK(f.mem.read(RA_MAIN + MAIN_LEN - 2, b, 4) == 2);
  CHECK(b[0] == 0x5A && b[1] == 0x5A);
  CHECK(b[2] == 0 && b[3] == 0);
  CHECK(!f.mem.supported(RA_MAIN + MAIN_LEN - 2, 4));
  CHECK(f.mem.supported(RA_MAIN + MAIN_LEN - 2, 2));
}

void past_the_map_reads_zero() {
  Fixture f;
  u8 b[4];
  std::memset(b, 0xAA, sizeof b);
  const u32 past = f.mem.max_address() + 1;
  CHECK(f.mem.read(past, b, sizeof b) == 0);
  for (u8 v : b) CHECK(v == 0);
  CHECK(!f.mem.supported(past));
}

// rcheevos may evaluate before a game is loaded; that must be quiet, not a
// crash through a null pointer.
void an_unattached_window_is_safe() {
  cheevos::Memory mem;
  CHECK(!mem.attached());
  u8 b[4];
  std::memset(b, 0xAA, sizeof b);
  CHECK(mem.read(RA_MAIN, b, sizeof b) == 0);
  for (u8 v : b) CHECK(v == 0);
  CHECK(cheevos::Memory::peek(RA_MAIN, 4, &mem) == 0);
  CHECK(cheevos::Memory::read_memory(RA_MAIN, b, 4, nullptr) == 0);
}

// ---------------------------------------------------------------------------
// The runtime on top of the window.

int g_triggered = 0;
u32 g_triggered_id = 0;

void on_event(const rc_runtime_event_t* e) {
  if (e->type == RC_RUNTIME_EVENT_ACHIEVEMENT_TRIGGERED) {
    ++g_triggered;
    g_triggered_id = e->id;
  }
}

struct Runtime {
  rc_runtime_t rt;
  cheevos::Memory* mem;
  explicit Runtime(cheevos::Memory& m) : mem(&m) { rc_runtime_init(&rt); g_triggered = 0; g_triggered_id = 0; }
  ~Runtime() { rc_runtime_destroy(&rt); }
  void activate(u32 id, const char* memaddr) {
    CHECK(rc_runtime_activate_achievement(&rt, id, memaddr, nullptr, 0) == RC_OK);
  }
  // Exactly what the emulator will do: one call per emulated frame.
  void frame() { rc_runtime_do_frame(&rt, on_event, cheevos::Memory::peek, mem, nullptr); }
};

// A condition over main RAM fires on the frame it becomes true, and the id that
// comes back is the one we activated.
void a_trigger_fires_on_the_right_frame() {
  Fixture f;
  Runtime r(f.mem);
  r.activate(7, "0xH000010=5");          // 8-bit at RA 0x10 equals 5

  // rcheevos holds a new achievement in WAITING until its condition has been
  // false once, so that loading into an already-satisfied state does not unlock
  // it. Frame one does that.
  f.main_ram()[0x10] = 0;
  r.frame();
  CHECK(g_triggered == 0);

  f.main_ram()[0x10] = 5;
  r.frame();
  CHECK(g_triggered == 1);
  CHECK(g_triggered_id == 7);

  // And it does not fire again while it stays true.
  r.frame();
  r.frame();
  CHECK(g_triggered == 1);
}

// Data TCM is reachable the same way -- it is the second backed region, so a
// condition there exercises the offset arithmetic rather than the base case.
//
// Note the right-hand side is decimal. In RetroAchievements' syntax an
// unprefixed "0x..." is a *16-bit memory read*, not a constant, so
// "0xH1000004=0x2A" asks whether the byte at 0x1000004 equals the halfword at
// address 0x2A -- which is a perfectly valid condition, just not the one
// intended. A hex constant is written "h2A". Cost an hour the first time.
void a_trigger_can_read_data_tcm() {
  Fixture f;
  Runtime r(f.mem);
  r.activate(9, "0xH1000004=42");
  f.dtcm()[4] = 0;
  r.frame();
  CHECK(g_triggered == 0);
  f.dtcm()[4] = 0x2A;
  r.frame();
  CHECK(g_triggered == 1);
  CHECK(g_triggered_id == 9);
}

// THE frame-pacing guard. A hit count says "this condition held for N frames",
// which is only true if do_frame is called once per frame. Call it twice per
// frame and this fires after five; miss calls and it never fires. Either way
// the symptom in the wild is an achievement that unlocks at the wrong time,
// with nothing logged, so it is pinned here.
void hit_counts_follow_the_frame_count() {
  Fixture f;
  Runtime r(f.mem);
  r.activate(11, "0xH000020=1.10.");     // true for 10 frames

  f.main_ram()[0x20] = 0;
  r.frame();                             // out of WAITING
  CHECK(g_triggered == 0);

  f.main_ram()[0x20] = 1;
  for (int i = 0; i < 9; ++i) {
    r.frame();
    CHECK(g_triggered == 0);             // not yet: only i+1 hits
  }
  r.frame();                             // the tenth hit
  CHECK(g_triggered == 1);
}

// A condition over the DSi hole must not unlock. The window hands rcheevos
// zero for it, so a condition looking for a non-zero value can never be true
// -- which is the conservative half of the design; the other half is
// rc_runtime_validate_addresses, which phase 3 wires to mark it unsupported.
void a_condition_over_the_hole_never_fires() {
  Fixture f;
  Runtime r(f.mem);
  r.activate(13, "0xH400000=5");
  std::memset(f.main_ram(), 5, 64);      // the bytes it would alias if mis-mapped
  for (int i = 0; i < 20; ++i) r.frame();
  CHECK(g_triggered == 0);
}

// ---------------------------------------------------------------------------
// GBA mode: rcheevos' GBA map. 32 KB of internal work RAM (0x03000000) first,
// then 256 KB of on-board work RAM (0x02000000), then 64 KB of the cartridge's
// save memory as the bus shows it at 0x0E000000.

constexpr u32 RA_IWRAM = 0x000000, RA_EWRAM = 0x008000, RA_SAVE = 0x048000, RA_GBA_END = 0x057FFF;

struct GbaFixture {
  NDS nds;
  cheevos::Memory mem;
  GbaFixture() {
    nds.gba = std::make_unique<gba::Gba>();
    std::string err;
    CHECK(mem.attach(nds, err));   // GBA mode: the machine's map is the GBA's
    CHECK(err.empty());
  }
  gba::Gba& g() { return *nds.gba; }
};

void the_gba_map_is_the_one_we_expect() {
  GbaFixture f;
  CHECK(f.mem.max_address() == RA_GBA_END);
  f.g().iwram.get()[0] = 0x11;
  f.g().iwram.get()[gba::IWRAM_SIZE - 1] = 0x22;
  f.g().ewram.get()[0] = 0x33;
  f.g().ewram.get()[gba::EWRAM_SIZE - 1] = 0x44;
  u8 b = 0;
  CHECK(f.mem.read(RA_IWRAM, &b, 1) == 1 && b == 0x11);
  CHECK(f.mem.read(RA_IWRAM + gba::IWRAM_SIZE - 1, &b, 1) == 1 && b == 0x22);
  CHECK(f.mem.read(RA_EWRAM, &b, 1) == 1 && b == 0x33);
  CHECK(f.mem.read(RA_EWRAM + gba::EWRAM_SIZE - 1, &b, 1) == 1 && b == 0x44);
  CHECK(RA_EWRAM + gba::EWRAM_SIZE == RA_SAVE);

  // A read across the two RAMs is whole, in address order.
  f.g().iwram.get()[gba::IWRAM_SIZE - 2] = 0x78;
  f.g().iwram.get()[gba::IWRAM_SIZE - 1] = 0x56;
  f.g().ewram.get()[0] = 0x34;
  f.g().ewram.get()[1] = 0x12;
  CHECK(cheevos::Memory::peek(RA_EWRAM - 2, 4, &f.mem) == 0x12345678u);
  CHECK(f.mem.supported(RA_IWRAM, gba::IWRAM_SIZE + gba::EWRAM_SIZE));

  // Past the map: zero, and unbacked.
  u8 past[4];
  std::memset(past, 0xAA, sizeof past);
  CHECK(f.mem.read(RA_GBA_END + 1, past, sizeof past) == 0);
  for (u8 v : past) CHECK(v == 0);
  CHECK(!f.mem.supported(RA_GBA_END + 1));
  CHECK(cheevos::Memory::peek(0x02000000, 4, &f.mem) == 0);   // a bus address is not an RA one
}

// The save memory follows the chip: none (or an EEPROM, which is not on that bus)
// reads zero and unbacked; SRAM's 32 KB mirror through the 64 KB; Flash shows the
// bank the game has selected. Read without touching the chip's command state.
void the_gba_save_memory_follows_the_chip() {
  GbaFixture f;
  gba::Save& s = f.g().save;
  u8 b[2] = {0xAA, 0xAA};
  CHECK(s.type() == gba::Save::Type::None);
  CHECK(f.mem.read(RA_SAVE, b, 2) == 0 && b[0] == 0 && b[1] == 0);
  CHECK(!f.mem.supported(RA_SAVE));
  // A read from on-board RAM into it is partial: that is rcheevos' "unsupported".
  CHECK(f.mem.read(RA_SAVE - 1, b, 2) == 1);

  s.set_type(gba::Save::Type::EepromUnsized);
  CHECK(!f.mem.supported(RA_SAVE));

  s.set_type(gba::Save::Type::Sram);
  s.write8(0x0E000010, 0x5A);
  s.write8(0x0E007FFF, 0x6B);
  CHECK(f.mem.supported(RA_SAVE, 0x10000));
  CHECK(f.mem.read(RA_SAVE + 0x10, b, 1) == 1 && b[0] == 0x5A);
  CHECK(f.mem.read(RA_SAVE + 0x8010, b, 1) == 1 && b[0] == 0x5A);   // the mirror
  CHECK(f.mem.read(RA_SAVE + 0x7FFF, b, 1) == 1 && b[0] == 0x6B);
  CHECK(f.mem.read(RA_SAVE + 0x11, b, 1) == 1 && b[0] == 0xFF);     // an unwritten SRAM byte

  // Flash 128 KB: write a byte in each bank, then switch banks.
  s.set_type(gba::Save::Type::Flash128);
  auto cmd = [&s](u8 c) { s.write8(0x0E005555, 0xAA); s.write8(0x0E002AAA, 0x55); s.write8(0x0E005555, c); };
  auto bank = [&](u8 n) { cmd(0xB0); s.write8(0x0E000000, n); };
  auto program = [&](u32 a, u8 v) { cmd(0xA0); s.write8(0x0E000000 + a, v); };
  bank(0); program(0x20, 0x01);
  bank(1); program(0x20, 0x02);
  CHECK(f.mem.read(RA_SAVE + 0x20, b, 1) == 1 && b[0] == 0x02);
  bank(0);
  CHECK(f.mem.read(RA_SAVE + 0x20, b, 1) == 1 && b[0] == 0x01);
  // In ID mode the chip answers the bus with its ID; RetroAchievements still sees
  // the stored bytes, and looking leaves the chip in ID mode.
  cmd(0x90);
  CHECK(f.mem.read(RA_SAVE, b, 1) == 1 && b[0] == 0xFF);
  CHECK(s.read8(0x0E000000) == 0xC2);
  cmd(0xF0);
  CHECK(s.read8(0x0E000020) == 0x01);
}

// A condition over on-board work RAM fires, through the GBA map.
void a_gba_trigger_fires() {
  GbaFixture f;
  Runtime r(f.mem);
  r.activate(21, "0xH008010=5");   // EWRAM byte 0x10
  f.g().ewram.get()[0x10] = 0;
  r.frame();
  CHECK(g_triggered == 0);
  f.g().ewram.get()[0x10] = 5;
  r.frame();
  CHECK(g_triggered == 1 && g_triggered_id == 21);
}

// ---------------------------------------------------------------------------

// The string the server decides whether to talk to us at all by.
void the_user_agent_is_well_formed() {
  // "DSperate-nibOS/<version> nds/<version> rcheevos/<version>": a product name of our
  // own with no spaces (RetroAchievements refuses one that is not unique, and upstream's
  // is "DSperate"), a core segment, then rcheevos' clause.
  for (const cheevos::Core core : {cheevos::Core::Nds, cheevos::Core::Gba}) {
    const std::string ua = cheevos::user_agent(core);
    const std::string product = std::string(cheevos::kClientName) + "/";
    CHECK(ua.rfind(product, 0) == 0);
    CHECK(std::string(cheevos::kClientName) != "DSperate");
    CHECK(std::string(cheevos::kClientName).find(' ') == std::string::npos);
    CHECK(ua.rfind(cheevos::product_token(), 0) == 0);

    const size_t sp1 = ua.find(' ');
    CHECK(sp1 != std::string::npos);
    const size_t sp2 = ua.find(' ', sp1 + 1);
    CHECK(sp2 != std::string::npos);
    CHECK(ua.find(' ', sp2 + 1) == std::string::npos);   // three clauses, no more
    const std::string core_clause = ua.substr(sp1 + 1, sp2 - sp1 - 1);
    CHECK(core_clause == std::string(core == cheevos::Core::Gba ? "gba/" : "nds/") + cheevos::client_version());
    CHECK(ua.compare(sp2 + 1, 9, "rcheevos/") == 0);

    // The version is numeric, dotted and non-empty, so the server can compare it.
    const std::string version = ua.substr(product.size(), sp1 - product.size());
    CHECK(version == cheevos::client_version());
    CHECK(!version.empty());
    for (char c : version) CHECK((c >= '0' && c <= '9') || c == '.');
    CHECK(version.find('.') != std::string::npos);
    std::printf("cheevos_memory: user agent %s\n", ua.c_str());
  }
}

} // namespace

// On the DSi machine the map is rcheevos' DSi one: the hole is real RAM, so the
// whole 16 MB reads through at the same addresses, and data TCM follows it.
void the_dsi_map_backs_all_of_main_ram() {
  NDS nds;
  nds.dsi = true;
  cheevos::Memory mem;
  std::string err;
  CHECK(mem.attach(nds, err));
  CHECK(err.empty());
  constexpr u32 DSI_LEN = mem::Bus::MAIN_RAM_SIZE_DSI;
  nds.bus.main_ram.get()[RA_HOLE] = 0x5C;              // above the DS's 4 MB
  nds.bus.main_ram.get()[DSI_LEN - 1] = 0x6D;
  nds.bus.dtcm.get()[0] = 0x7E;
  u8 b = 0;
  CHECK(mem.read(RA_HOLE, &b, 1) == 1 && b == 0x5C);
  CHECK(mem.read(DSI_LEN - 1, &b, 1) == 1 && b == 0x6D);
  CHECK(mem.read(RA_DTCM, &b, 1) == 1 && b == 0x7E);
  CHECK(mem.supported(RA_MAIN, DSI_LEN));
  CHECK(mem.max_address() == RA_DTCM + DTCM_LEN - 1);
  u8 past[2] = {0xAA, 0xAA};
  CHECK(mem.read(RA_DTCM + DTCM_LEN, past, 2) == 0 && past[0] == 0);
}

int main() {
  the_map_is_the_one_we_expect();
  the_dsi_map_backs_all_of_main_ram();
  main_ram_and_dtcm_read_through();
  the_dsi_hole_reads_zero_and_is_unbacked();
  a_read_across_the_boundary_is_partial();
  past_the_map_reads_zero();
  an_unattached_window_is_safe();
  a_trigger_fires_on_the_right_frame();
  a_trigger_can_read_data_tcm();
  hit_counts_follow_the_frame_count();
  a_condition_over_the_hole_never_fires();
  the_gba_map_is_the_one_we_expect();
  the_gba_save_memory_follows_the_chip();
  a_gba_trigger_fires();
  the_user_agent_is_well_formed();
  std::printf("cheevos_memory: ok\n");
  return 0;
}
