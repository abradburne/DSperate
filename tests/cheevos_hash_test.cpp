// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// The RetroAchievements hash, DS and GBA. The DS golden values below were computed
// independently of this code (a few lines of Python over the same synthetic
// ROM, following the rule in rcheevos' hash_rom.c: MD5 of the 0x160-byte
// header, then the ARM9 binary, then the ARM7 binary, then 0xA00 bytes of
// icon block, zero-padded if the file is short). So they check our wiring
// against the specification rather than against itself.
//
// The test that earns its keep is hash_ignores_the_secure_area_rewrite. Cart
// re-encrypts the secure area in place through RomSource::patch(), and the
// secure area is the first 0x800 bytes of the ARM9 binary -- exactly what the
// hash covers. Get that wrong and nothing fails: the hash is simply one
// RetroAchievements has never seen, and the game quietly has no achievements.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>

#include "check.h"
#include "cheevos/cheevos_hash.h"
#include "core/cart/rom_source.h"
#include "core/cart/zip.h"
#include "core/gba/gba.h"
#include "core/nds.h"

using ds::u8;
using ds::u32;

namespace {

constexpr u32 ARM9_OFF = 0x4000, ARM9_LEN = 0x1000;
constexpr u32 ARM7_OFF = 0x8000, ARM7_LEN = 0x0800;
constexpr u32 ICON_OFF = 0xA000, ICON_LEN = 0x0A00;

// A ROM whose every byte is determined by its offset, so the golden hashes are
// reproducible by anything that can count. `total` sets where the file ends,
// which is how the short-icon case is built.
std::vector<u8> synthetic_rom(u32 total) {
  std::vector<u8> b(total);
  for (u32 i = 0; i < total; ++i) b[i] = static_cast<u8>((i * 31 + 7) & 0xFF);
  auto w32 = [&b](u32 off, u32 v) {
    b[off + 0] = static_cast<u8>(v);
    b[off + 1] = static_cast<u8>(v >> 8);
    b[off + 2] = static_cast<u8>(v >> 16);
    b[off + 3] = static_cast<u8>(v >> 24);
  };
  w32(0x20, ARM9_OFF);
  w32(0x2C, ARM9_LEN);
  w32(0x30, ARM7_OFF);
  w32(0x3C, ARM7_LEN);
  w32(0x68, ICON_OFF);
  return b;
}

std::string hash_of(const ds::cart::RomSource& src) {
  std::string out, err;
  CHECK(ds::cheevos::rom_hash(src, "synthetic", out, err));
  CHECK(err.empty());
  CHECK(out.size() == 32);
  return out;
}

const char* FULL_HASH  = "9d072ade9c1badd3a4325488f687f253";
const char* SHORT_HASH = "5f1e526181ad5db9f5d20f625f5f9622";

void hash_matches_the_specification() {
  const auto src = ds::cart::RomSource::from_memory(synthetic_rom(ICON_OFF + ICON_LEN));
  CHECK(src);
  CHECK(hash_of(*src) == FULL_HASH);
}

// A homebrew ROM that stops inside its icon block. rcheevos zero-pads the
// remainder, which it can only do if our reader reports the short count --
// RomSource would otherwise happily serve 0xFF past the end of the image, and
// 0xFF padding hashes differently from 0x00 padding.
void short_icon_block_is_zero_padded() {
  const auto src = ds::cart::RomSource::from_memory(synthetic_rom(ICON_OFF + 0x400));
  CHECK(src);
  CHECK(hash_of(*src) == SHORT_HASH);
  CHECK(std::strcmp(SHORT_HASH, FULL_HASH) != 0);
}

// The reason read_unpatched exists.
void hash_ignores_the_secure_area_rewrite() {
  auto src = ds::cart::RomSource::from_memory(synthetic_rom(ICON_OFF + ICON_LEN));
  CHECK(src);
  CHECK(hash_of(*src) == FULL_HASH);

  // What Cart does at construction: take a writable copy of the secure-area
  // page and rewrite it. 0xAA stands in for the re-encryption.
  u8* page = src->patch(ARM9_OFF);
  CHECK(page != nullptr);
  std::memset(page, 0xAA, 0x800);

  // The rewrite is visible through the cart's own read path...
  u8 probe[8] = {};
  src->read(ARM9_OFF, probe, sizeof probe);
  CHECK(probe[0] == 0xAA);
  // ...and invisible to the hash.
  src->read_unpatched(ARM9_OFF, probe, sizeof probe);
  CHECK(probe[0] != 0xAA);
  CHECK(hash_of(*src) == FULL_HASH);
}

// Mapped and owned sources are supposed to be indistinguishable; the hash is
// one more place that has to hold, and it is the path a real ROM file takes.
void a_mapped_file_hashes_the_same() {
  const auto bytes = synthetic_rom(ICON_OFF + ICON_LEN);
  char path[] = "/tmp/dsperate_cheevos_hash_testXXXXXX";
  const int fd = mkstemp(path);
  CHECK(fd >= 0);
  CHECK(write(fd, bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size()));
  close(fd);

  std::string err;
  const auto src = ds::cart::RomSource::map_file(path, err);
  unlink(path);
  CHECK(src);
  CHECK(src->mapped());
  CHECK(hash_of(*src) == FULL_HASH);
}

// Not a DS ROM at all: a failure has to come back as one, with a reason, and
// without a hash anyone could mistake for real.
void a_file_too_small_is_refused() {
  const auto src = ds::cart::RomSource::from_memory(std::vector<u8>(0x40, 0u));
  CHECK(src);
  std::string out = "untouched", err;
  CHECK(!ds::cheevos::rom_hash(*src, "tiny", out, err));
  CHECK(out.empty());
  CHECK(!err.empty());
}

// ---------------------------------------------------------------------------
// GBA. rcheevos' rule (rhash: RC_CONSOLE_GAMEBOY_ADVANCE is a whole-file hash) is
// the MD5 of the file, every byte of it. The golden value is Python's hashlib over
// the same bytes: hashlib.md5(bytes((i*29+11)&0xFF for i in range(0x30123))).

constexpr u32 GBA_LEN = 0x30123;   // not a power of two: the image GBA mode pads past it must not count
const char* GBA_HASH = "f8f464525b9a2c5b7336390aa0484e3d";

std::vector<u8> synthetic_gba() {
  std::vector<u8> b(GBA_LEN);
  for (u32 i = 0; i < GBA_LEN; ++i) b[i] = static_cast<u8>((i * 29 + 11) & 0xFF);
  return b;
}

std::string write_temp(const std::vector<u8>& bytes, const char* suffix) {
  std::string path = std::string("/tmp/dsperate_cheevos_gba_XXXXXX") + suffix;
  const int fd = mkstemps(path.data(), static_cast<int>(std::strlen(suffix)));
  CHECK(fd >= 0);
  CHECK(write(fd, bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size()));
  close(fd);
  return path;
}

// One stored (method 0) entry: what a zipped GBA ROM is at its plainest.
std::vector<u8> stored_zip(const std::string& name, const std::vector<u8>& data) {
  std::vector<u8> z;
  auto u16le = [&z](u32 v) { z.push_back(u8(v)); z.push_back(u8(v >> 8)); };
  auto u32le = [&z](u32 v) { for (int i = 0; i < 4; ++i) z.push_back(u8(v >> (8 * i))); };
  const u32 crc = ds::cart::crc32_update(0, data.data(), data.size());
  const u32 n = static_cast<u32>(data.size());
  u32le(0x04034B50); u16le(20); u16le(0); u16le(0); u16le(0); u16le(0);
  u32le(crc); u32le(n); u32le(n); u16le(static_cast<u32>(name.size())); u16le(0);
  z.insert(z.end(), name.begin(), name.end());
  z.insert(z.end(), data.begin(), data.end());
  const u32 cd = static_cast<u32>(z.size());
  u32le(0x02014B50); u16le(20); u16le(20); u16le(0); u16le(0); u16le(0); u16le(0);
  u32le(crc); u32le(n); u32le(n); u16le(static_cast<u32>(name.size()));
  u16le(0); u16le(0); u16le(0); u16le(0); u32le(0); u32le(0);
  z.insert(z.end(), name.begin(), name.end());
  const u32 cd_len = static_cast<u32>(z.size()) - cd;
  u32le(0x06054B50); u16le(0); u16le(0); u16le(1); u16le(1); u32le(cd_len); u32le(cd); u16le(0);
  return z;
}

void the_gba_hash_is_the_whole_file() {
  const std::vector<u8> rom = synthetic_gba();
  std::string out, err;
  CHECK(ds::cheevos::gba_rom_hash(rom.data(), rom.size(), out, err));
  CHECK(err.empty());
  CHECK(out == GBA_HASH);
  // One byte more or less is another dump.
  CHECK(ds::cheevos::gba_rom_hash(rom.data(), rom.size() - 1, out, err) && out != GBA_HASH);
}

// What the session hashes: the image GBA mode loaded, from a file and from a zip
// alike, `rom_size` bytes of it and none of the open-bus pattern past the end.
void a_loaded_gba_rom_hashes_as_its_file() {
  const std::vector<u8> rom = synthetic_gba();
  for (const bool zipped : {false, true}) {
    const std::string path = zipped ? write_temp(stored_zip("game.gba", rom), ".zip") : write_temp(rom, ".gba");
    ds::NDS nds;
    std::string err;
    const bool loaded = nds.load_gba(path, "", &err);
    unlink(path.c_str());
    CHECK(loaded);
    CHECK(nds.gba && nds.gba->rom_size == GBA_LEN);
    std::string out;
    CHECK(ds::cheevos::gba_rom_hash(nds.gba->rom.get(), nds.gba->rom_size, out, err));
    CHECK(out == GBA_HASH);
  }
}

void a_gba_file_too_small_is_refused() {
  const std::vector<u8> tiny(0x40, 0u);
  std::string out = "untouched", err;
  CHECK(!ds::cheevos::gba_rom_hash(tiny.data(), tiny.size(), out, err));
  CHECK(out.empty());
  CHECK(!err.empty());
}

} // namespace

int main() {
  hash_matches_the_specification();
  short_icon_block_is_zero_padded();
  hash_ignores_the_secure_area_rewrite();
  a_mapped_file_hashes_the_same();
  a_file_too_small_is_refused();
  the_gba_hash_is_the_whole_file();
  a_loaded_gba_rom_hashes_as_its_file();
  a_gba_file_too_small_is_refused();
  std::printf("cheevos_hash: ok\n");
  return 0;
}
