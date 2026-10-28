// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// A small PNG decoder for achievement badges (64x64 images from the
// RetroAchievements media host), written from the PNG specification over the
// vendored miniz inflate. Non-interlaced images only; bit depth 8 for every
// colour type, and 1/2/4 for greyscale and palette.
#pragma once
#include <cstddef>
#include <string>
#include <vector>
#include "core/types.h"

namespace ds::cheevos {

// `argb` gets w*h pixels, 0xAARRGGBB, rows top to bottom. False with a reason
// in `err` for anything the decoder does not handle (interlace, 16-bit).
bool decode_png(const u8* data, size_t size, int& w, int& h, std::vector<u32>& argb, std::string& err);

} // namespace ds::cheevos
