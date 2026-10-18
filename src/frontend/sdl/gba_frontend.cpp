// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "frontend/sdl/gba_frontend.h"
#include "core/gba/gba.h"
#include "core/nds.h"

#include <cstdio>
#include <vector>

namespace ds::sdl {

std::string game_code(const NDS& nds) {
  if (nds.cart) return std::string(nds.cart->header().game_code, 4);
  if (nds.gba) {
    const u32 c = nds.gba->game_code();
    std::string s(reinterpret_cast<const char*>(&c), 4);
    for (char& ch : s) if (ch < 0x20 || ch >= 0x7f) ch = '_';
    return s;
  }
  return "NONE";
}

bool has_game(const NDS& nds) { return nds.cart || nds.gba; }

void load_gba_save(gba::Gba& g, const std::string& path, bool verbose) {
  if (verbose) std::fprintf(stderr, "save: %s\n", gba::Save::name(g.save.type()));
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return;
  std::vector<u8> data(gba::SAVE_MAX);
  const size_t got = std::fread(data.data(), 1, data.size(), f);
  std::fclose(f);
  if (got && g.save.load(data.data(), got) && verbose) std::fprintf(stderr, "save: loaded %zu bytes from %s\n", got, path.c_str());
}

void write_gba_save(gba::Gba& g, const std::string& path) {
  const std::vector<u8> data = g.save.data();
  if (data.empty()) return;
  const std::string tmp = path + ".tmp";
  std::FILE* f = std::fopen(tmp.c_str(), "wb");
  if (!f) { std::fprintf(stderr, "save: cannot write %s\n", tmp.c_str()); return; }
  const bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
  std::fclose(f);
  if (!ok || std::rename(tmp.c_str(), path.c_str()) != 0) { std::fprintf(stderr, "save: cannot write %s\n", path.c_str()); return; }
  g.save.dirty = false;
}

void print_gba_game(const gba::Gba& g, const std::string& bios_path) {
  const u32 code = g.game_code();
  std::fprintf(stderr, "gba: %s [%.4s], BIOS %s\n", g.title().c_str(), reinterpret_cast<const char*>(&code), g.bios_native ? bios_path.c_str() : "built in (Cult-of-GBA)");
}

} // namespace ds::sdl
