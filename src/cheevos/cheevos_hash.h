// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Hashes a ROM for RetroAchievements. Feeds bytes via RomSource rather than
// letting rcheevos open the file, so a zipped ROM hashes without unpacking
// and the secure-area rewrite never reaches the hash (read_unpatched).
// A GBA ROM is hashed from the image GBA mode loaded (unzipped, unpatched).
#pragma once

#include <cstddef>
#include <string>

#include "core/types.h"

namespace ds::cart { class RomSource; }

namespace ds::cheevos {

// 32 lowercase hex chars in `out`, or false with a reason in `err`. `name` is
// for messages only. Safe to call on any thread.
bool rom_hash(const cart::RomSource& src, const std::string& name,
              std::string& out, std::string& err, bool dsi = false);

// A GBA cartridge: rcheevos' rule is the MD5 of the whole file, so `rom` is the
// file's bytes (or its zip entry's) exactly, `size` of them. MD5 of a 32 MB
// cartridge runs longer than a real-time thread may (core/off_realtime.h).
bool gba_rom_hash(const u8* rom, size_t size, std::string& out, std::string& err);

} // namespace ds::cheevos
