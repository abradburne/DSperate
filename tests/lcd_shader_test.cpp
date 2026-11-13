// SPDX-License-Identifier: GPL-3.0-or-later
// The LCD shaders' masks (frontend/video/lcd_shader.h, after GBA DS): names,
// gains, when they draw, and their cells at the scales the presenters use.
#include "check.h"
#include "frontend/video/lcd_shader.h"

#include <cstdlib>

using namespace ds::frontend;
using ds::u8;
using ds::u32;

namespace {
int r8(u32 p) { return static_cast<int>(p >> 16 & 255); }
int g8(u32 p) { return static_cast<int>(p >> 8 & 255); }
int b8(u32 p) { return static_cast<int>(p & 255); }
bool near(int a, int b) { return std::abs(a - b) <= 1; }
} // namespace

int main() {
  // Names: DSperate's underscores and GBA DS's hyphens; anything else is the grid.
  LcdShader s;
  CHECK(parse_lcd_shader("subpixels_strong", s) && s == LcdShader::SubStrong);
  CHECK(parse_lcd_shader("subpixels-light", s) && s == LcdShader::SubLight);
  CHECK(parse_lcd_shader("off", s) && s == LcdShader::Off);
  CHECK(!parse_lcd_shader("crt", s) && s == LcdShader::Grid);
  for (u8 i = 0; i < static_cast<u8>(LcdShader::Count); ++i) {
    CHECK(parse_lcd_shader(lcd_shader_name(static_cast<LcdShader>(i)), s) && s == static_cast<LcdShader>(i));
  }

  // Gains: only the subpixels brighten, as GBA DS's presets.
  CHECK(lcd_shader_gain(LcdShader::Grid) == 100 && lcd_shader_gain(LcdShader::Soft) == 100);
  CHECK(lcd_shader_gain(LcdShader::SubLight) == 112 && lcd_shader_gain(LcdShader::Sub) == 125 && lcd_shader_gain(LcdShader::SubStrong) == 140);

  // Drawn: off never; the grid, scanlines and soft cells with some strength; the subpixels always.
  CHECK(!lcd_shader_drawn(LcdShader::Off, 1.0));
  CHECK(!lcd_shader_drawn(LcdShader::Grid, 0.0) && lcd_shader_drawn(LcdShader::Grid, 0.3));
  CHECK(!lcd_shader_drawn(LcdShader::Scanlines, 0.0) && lcd_shader_drawn(LcdShader::Sub, 0.0));
  LcdMask m;
  lcd_shader_mask(LcdShader::Grid, 0.0, 1024, 768, 256, 192, m);
  CHECK(m.px.empty() && m.mw == 0);

  // The grid at 4x (the DS+): one 4 x 4 cell, its right column and bottom row darkened.
  lcd_shader_mask(LcdShader::Grid, 0.5, 1024, 768, 256, 192, m);
  CHECK(m.mw == 4 && m.mh == 4 && m.px.size() == 16);
  CHECK(r8(m.px[0]) == 255 && r8(m.px[3]) == 127 && r8(m.px[12]) == 127 && r8(m.px[15]) == 127 && (m.px[0] >> 24) == 255);
  // At 5x the top and left edges get a lighter line.
  lcd_shader_mask(LcdShader::Grid, 0.6, 1280, 960, 256, 192, m);
  CHECK(m.mw == 5 && r8(m.px[0]) == 255 * (100 - 20) / 100 && r8(m.px[7]) == 255 && r8(m.px[4]) == 255 * 40 / 100);

  // Scanlines at 4x: the bottom row only.
  lcd_shader_mask(LcdShader::Scanlines, 0.5, 1024, 768, 256, 192, m);
  CHECK(m.mw == 4 && m.mh == 4);
  for (int x = 0; x < 4; ++x) CHECK(r8(m.px[x]) == 255 && r8(m.px[8 + x]) == 255 && near(r8(m.px[12 + x]), 77));

  // Soft cells at 4x: darkest at the corners, symmetric.
  lcd_shader_mask(LcdShader::Soft, 0.5, 1024, 768, 256, 192, m);
  CHECK(r8(m.px[0]) < r8(m.px[5]) && r8(m.px[0]) == r8(m.px[15]) && r8(m.px[1]) == r8(m.px[2]));

  // Subpixels at 4x: red, green, blue, gap a pixel each, the bottom row darker.
  lcd_shader_mask(LcdShader::Sub, 0.0, 1024, 768, 256, 192, m);
  CHECK(m.mw == 4 && m.mh == 4);
  CHECK(r8(m.px[0]) == 255 && near(g8(m.px[0]), 158) && near(b8(m.px[0]), 158));
  CHECK(g8(m.px[1]) == 255 && near(r8(m.px[1]), 158) && b8(m.px[2]) == 255);
  CHECK(near(r8(m.px[3]), 166) && r8(m.px[3]) == g8(m.px[3]) && g8(m.px[3]) == b8(m.px[3]));
  CHECK(near(r8(m.px[12]), 217));   // 255 * 0.85
  // GBA mode: the same cells, the GBA's pixels being the DS frame's (Gpu::show_gba lays them 1:1).

  // Under 3 panel pixels a cell (2x): the stripes' average, in grey.
  lcd_shader_mask(LcdShader::Sub, 0.0, 512, 384, 256, 192, m);
  CHECK(m.mw == 2 && m.mh == 2);
  for (u32 p : m.px) CHECK(r8(p) == g8(p) && g8(p) == b8(p) && r8(p) < 255);

  // Between whole-number scales: the whole view.
  lcd_shader_mask(LcdShader::SubLight, 0.0, 1000, 750, 256, 192, m);
  CHECK(m.mw == 1000 && m.mh == 750 && m.px.size() == 1000u * 750u);
  lcd_shader_mask(LcdShader::Grid, 0.4, 1000, 750, 256, 192, m);   // the fine grid
  CHECK(m.mw == 1000 && m.mh == 750);
  int dark = 0;
  for (int x = 0; x < 1000; ++x) if (r8(m.px[static_cast<size_t>(x)]) < 255) ++dark;
  CHECK(dark > 200 && dark < 600);   // a quarter of each ~3.9-pixel cell, spread over one or two pixels
  std::printf("lcd_shader: ok\n");
  return 0;
}
