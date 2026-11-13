// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// The masks of GBA DS's display shaders (https://github.com/kyannon/gba-ds,
// by kyannon, GPL-3.0: src/ui.c at commit 7189176 "Release v1.5"), ported
// from its SDL textures to plain words: pipe_grid, pipe_grid_fine,
// pattern_at, pipe_pattern and shader_mask, with subPresets' strengths and
// gains. Only what they draw is the same; the cache and the textures are the
// presenters' own (gl_present.cpp, gpu_present.cpp).
#include "frontend/video/lcd_shader.h"

#include <algorithm>
#include <cmath>

namespace ds::frontend {

namespace {

constexpr const char* kNames[] = {"off", "grid", "scanlines", "soft", "subpixels_light", "subpixels", "subpixels_strong"};
static_assert(sizeof kNames / sizeof kNames[0] == static_cast<size_t>(LcdShader::Count));

// The subpixel looks' strengths: a stripe's other two colours, the gap
// column, the bottom row; and the brightening that makes up for them (GBA
// DS's subPresets). Light is for dim screens (about 0.9 of the picture's
// brightness under the stripes), strong for bright ones (about 0.7).
struct SubPreset { float other, gap, row; int gain; };
constexpr SubPreset kSub[3] = {
  {0.80f, 0.80f, 0.92f, 112},   // light
  {0.62f, 0.65f, 0.85f, 125},   // (about 0.8)
  {0.45f, 0.50f, 0.75f, 140},   // strong
};

u32 grey(int c) { return 0xFF000000u | static_cast<u32>(c) << 16 | static_cast<u32>(c) << 8 | static_cast<u32>(c); }

// Each cell k x k panel pixels whose right column and bottom row are
// darkened, like the gaps between LCD pixels; from 5 pixels up the top and
// left edges get a lighter line, for depth. `cw` x `ch` cells.
void grid(int cw, int ch, int k, int strength, std::vector<u32>& px) {
  const int line = 255 * (100 - strength) / 100;
  const int soft = 255 * (100 - strength / 3) / 100;
  px.resize(static_cast<size_t>(cw) * k * ch * k);
  for (int y = 0; y < ch * k; ++y) {
    const int cy = y % k;
    for (int x = 0; x < cw * k; ++x) {
      const int cx = x % k;
      int c = 255;
      if (cx == k - 1 || cy == k - 1) c = line;
      else if (k >= 5 && (cx == 0 || cy == 0)) c = soft;
      px[static_cast<size_t>(y) * cw * k + x] = grey(c);
    }
  }
}

// Total gap length in [0, t): cells of size p whose last g * p is gap.
double gap_upto(double t, double p, double g) {
  const double n = std::floor(t / p), r = t - n * p, edge = (1 - g) * p;
  return n * g * p + (r > edge ? r - edge : 0);
}

// The grid for cells that are not a whole number of panel pixels (or under
// 3 of them), made at the panel's own resolution: gw x gh cells over a
// dw x dh view, the gap between cells a quarter of a cell wide (as at 4x),
// each panel pixel darkened by how much of it the gap covers. The same
// darkness and texture as the whole-pixel grid, without its uneven lines.
void grid_fine(int dw, int dh, int gw, int gh, int strength, std::vector<u32>& px) {
  const double g = 0.25, pw = static_cast<double>(dw) / gw, ph = static_cast<double>(dh) / gh, k = strength / 100.0;
  std::vector<double> cx(static_cast<size_t>(dw));
  for (int x = 0; x < dw; ++x) cx[x] = gap_upto(x + 1, pw, g) - gap_upto(x, pw, g);
  px.resize(static_cast<size_t>(dw) * dh);
  for (int y = 0; y < dh; ++y) {
    const double cy = gap_upto(y + 1, ph, g) - gap_upto(y, ph, g);
    for (int x = 0; x < dw; ++x) {
      const double cover = cx[x] + cy - cx[x] * cy;   // the share of the pixel in either line
      px[static_cast<size_t>(y) * dw + x] = grey(static_cast<int>(255 * (1 - k * cover) + 0.5));
    }
  }
}

// One of the other looks at a point in a cell (u, v in 0..1, across and
// down), as a multiplier for red, green and blue.
void pattern_at(LcdShader sh, double u, double v, double k, float out[3]) {
  out[0] = out[1] = out[2] = 1;
  if (sh == LcdShader::Scanlines) {
    // the bottom quarter of each row of pixels, darker than the grid's
    // lines (there are half as many), as mGBA's Scanlines at its 0.5
    if (v >= 0.75) out[0] = out[1] = out[2] = static_cast<float>(1 - std::min(1.0, k * 1.4));
  } else if (sh == LcdShader::Soft) {
    // darker toward the cell's edges, most at the corners: zfast_lcd's falloff
    const double d = 2 * std::max(std::fabs(u - 0.5), std::fabs(v - 0.5));
    out[0] = out[1] = out[2] = static_cast<float>(1 - std::min(1.0, k * 1.3) * d * d * d * d);
  } else if (lcd_subpixels(sh)) {
    // red, green and blue stripes and a gap, a quarter of the cell each
    // (one panel pixel each at 4x), and a darker bottom row
    const SubPreset& p = kSub[static_cast<int>(sh) - static_cast<int>(LcdShader::SubLight)];
    const int col = static_cast<int>(u * 4);
    if (col >= 3) out[0] = out[1] = out[2] = p.gap;
    else for (int c = 0; c < 3; ++c) out[c] = c == col ? 1.0f : p.other;
    if (v >= 0.75) for (int c = 0; c < 3; ++c) out[c] *= p.row;
  }
}

// The other looks' mask: gw x gh cells over a dw x dh view (whole panel
// pixels per cell or not), each pixel the average of the pattern over 4 x 4
// points in it, so the pattern is as sharp as the panel allows and keeps its
// overall darkness.
void pattern(LcdShader sh, int dw, int dh, int gw, int gh, int strength, std::vector<u32>& px) {
  constexpr int N = 4;
  const double pw = static_cast<double>(dw) / gw, ph = static_cast<double>(dh) / gh, k = strength / 100.0;
  // three stripes don't fit in under 3 panel pixels: there each pixel gets
  // their average, in grey, so the picture has the subpixels' texture and
  // darkness without a colour cast
  const bool grey_only = pw < 3 || ph < 3;
  px.resize(static_cast<size_t>(dw) * dh);
  for (int y = 0; y < dh; ++y) {
    for (int x = 0; x < dw; ++x) {
      float sum[3] = {0, 0, 0};
      for (int sy = 0; sy < N; ++sy) {
        const double fy = (y + (sy + 0.5) / N) / ph, cv = fy - std::floor(fy);
        for (int sx = 0; sx < N; ++sx) {
          const double fx = (x + (sx + 0.5) / N) / pw;
          float m[3];
          pattern_at(sh, fx - std::floor(fx), cv, k, m);
          for (int c = 0; c < 3; ++c) sum[c] += m[c];
        }
      }
      if (grey_only) sum[0] = sum[1] = sum[2] = (sum[0] + sum[1] + sum[2]) / 3;
      const u32 r = static_cast<u32>(255 * sum[0] / (N * N) + 0.5f), g = static_cast<u32>(255 * sum[1] / (N * N) + 0.5f),
                b = static_cast<u32>(255 * sum[2] / (N * N) + 0.5f);
      px[static_cast<size_t>(y) * dw + x] = 0xFF000000u | r << 16 | g << 8 | b;
    }
  }
}

} // namespace

bool parse_lcd_shader(const std::string& v, LcdShader& out) {
  std::string s = v;
  std::replace(s.begin(), s.end(), '-', '_');   // GBA DS's own spelling
  for (u8 i = 0; i < static_cast<u8>(LcdShader::Count); ++i)
    if (s == kNames[i]) { out = static_cast<LcdShader>(i); return true; }
  out = LcdShader::Grid;
  return false;
}

const char* lcd_shader_name(LcdShader s) { return s < LcdShader::Count ? kNames[static_cast<u8>(s)] : "grid"; }

int lcd_shader_gain(LcdShader s) { return lcd_subpixels(s) ? kSub[static_cast<int>(s) - static_cast<int>(LcdShader::SubLight)].gain : 100; }

bool lcd_shader_drawn(LcdShader s, double strength) {
  if (s == LcdShader::Off || s >= LcdShader::Count) return false;
  return lcd_subpixels(s) || strength > 0.0;
}

void lcd_shader_mask(LcdShader s, double strength, int dw, int dh, int gw, int gh, LcdMask& out) {
  out.px.clear();
  out.mw = out.mh = 0;
  if (dw <= 0 || dh <= 0 || gw <= 0 || gh <= 0 || !lcd_shader_drawn(s, strength)) return;
  const int st = static_cast<int>(std::lround(std::clamp(strength, 0.0, 1.0) * 100.0));
  // A whole number of panel pixels a cell across (down): the pattern
  // repeats every cell that way, so one cell is the mask's width (height).
  const bool rx = dw % gw == 0, ry = dh % gh == 0;
  const int mw = rx ? dw / gw : dw, mh = ry ? dh / gh : dh, cw = rx ? 1 : gw, ch = ry ? 1 : gh;
  // The panel pixels per cell when the picture is drawn at one whole multiple
  // (the grid then draws its lines a whole panel pixel each), else 0.
  const int kc = rx && ry && dw / gw == dh / gh ? dw / gw : 0;
  const bool small = dw < gw * 3 || dh < gh * 3;
  if (s == LcdShader::Grid || (s == LcdShader::Soft && small)) {
    // (soft cells have no room for their falloff in under 3 panel pixels:
    // the fine grid there)
    if (kc > 0 && s == LcdShader::Grid) grid(1, 1, kc, st, out.px);
    else grid_fine(mw, mh, cw, ch, st, out.px);
  } else {
    pattern(s, mw, mh, cw, ch, st, out.px);
  }
  out.mw = mw; out.mh = mh;
}

} // namespace ds::frontend
