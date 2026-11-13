// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
// LCD shaders (video.lcd_shader): a fixed pattern multiplied over the
// nearest-scaled picture, one cell per DS pixel (in GBA mode per GBA pixel:
// Gpu::show_gba lays the 240x160 frame 1:1 into the top screen's, so the two
// are the same cells). The presenters that draw them (the GLES present, the
// Vulkan present at whole-number scales) take the mask built here once per
// view size, and multiply it in with one fetch a pixel; the subpixel looks
// brighten the picture first (lcd_shader_gain) so they don't leave it dim.
//
// Ported from GBA DS (https://github.com/kyannon/gba-ds, by kyannon,
// GPL-3.0; src/ui.c pipe_grid, pipe_grid_fine, pattern_at, pipe_pattern,
// shader_mask and subPresets, at commit 7189176 "Release v1.5"). The
// subpixels follow mGBA's AGB-001 and RetroArch's lcd3x, the soft cells
// RetroArch's zfast_lcd.
#include "core/types.h"

#include <string>
#include <vector>

namespace ds::frontend {

// Menu order, as GBA DS's Display > Shader.
enum class LcdShader : u8 { Off, Grid, Scanlines, Soft, SubLight, Sub, SubStrong, Count };

// "off" | "grid" | "scanlines" | "soft" | "subpixels_light" | "subpixels" |
// "subpixels_strong" (GBA DS's "subpixels-light" spelling too); false (and
// Grid, the default) for anything else.
bool parse_lcd_shader(const std::string& v, LcdShader& out);
const char* lcd_shader_name(LcdShader s);
inline bool lcd_subpixels(LcdShader s) { return s >= LcdShader::SubLight && s <= LcdShader::SubStrong; }

// The brightening, in percent (100: none), the subpixel looks give the
// picture before their stripes darken it again: every channel scaled alike,
// at most until the brightest reaches 255, so the colour keeps its hue.
int lcd_shader_gain(LcdShader s);

// Whether the look changes the picture at all: not off, and the grid,
// scanlines and soft cells only with some strength (video.lcd_grid, 0..1);
// the subpixels have their own fixed strengths.
bool lcd_shader_drawn(LcdShader s, double strength);

// The mask for gw x gh cells over a dw x dh view, as 0xFFRRGGBB multipliers
// (255: the picture as it is), rows top first. Where the cells are a whole
// number of panel pixels across (and down) the pattern repeats every cell,
// so only one cell is built: `mw` x `mh` is what was, and pixel (x, y) of
// the view takes word (y % mh) * mw + x % mw. Otherwise it is the whole view.
struct LcdMask {
  std::vector<u32> px;
  int mw = 0, mh = 0;
};
void lcd_shader_mask(LcdShader s, double strength, int dw, int dh, int gw, int gh, LcdMask& out);

} // namespace ds::frontend
