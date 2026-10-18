// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
// What the SDL frontend does differently in GBA mode (core/gba: NDS::gba set
// by NDS::load_gba): the game's identity for states, screenshots and cheats,
// its save file, and the line it prints when it starts. main.cpp calls these
// where it handles the DS card's.
#include "core/types.h"

#include <string>

namespace ds { struct NDS; }
namespace ds::gba { struct Gba; }

namespace ds::sdl {

// The four-letter code states, screenshots and cheats are keyed by: the DS
// card's, the GBA cartridge's (unprintable bytes as '_'), or NONE (firmware boot).
std::string game_code(const NDS& nds);
// A game is loaded: a DS card or a GBA cartridge.
bool has_game(const NDS& nds);

// The save file: the chip's bytes (SRAM, Flash or EEPROM, the type the ROM
// says), as mGBA writes them. Writing goes through a temporary file and a
// rename, so a power cut leaves the previous save; it clears save.dirty.
void load_gba_save(gba::Gba& g, const std::string& path, bool verbose);
void write_gba_save(gba::Gba& g, const std::string& path);

// "gba: <title> [<code>], BIOS <what>" on stderr.
void print_gba_game(const gba::Gba& g, const std::string& bios_path);

} // namespace ds::sdl
