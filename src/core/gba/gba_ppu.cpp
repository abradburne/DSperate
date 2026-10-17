// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/gba/gba_ppu.h"
#include "core/gba/gba.h"
#include "core/state/state.h"

#include <algorithm>
#include <cstring>

namespace ds::gba {

namespace {
inline u32 expand(u16 c) {
  const u32 r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
  return 0xFF000000u | ((r << 3 | r >> 2) << 16) | ((g << 3 | g >> 2) << 8) | (b << 3 | b >> 2);
}
inline s32 ref28(u32 v) { return static_cast<s32>(v << 4) >> 4; }

constexpr u8 OBJ_W[3][4] = {{8, 16, 32, 64}, {16, 32, 32, 64}, {8, 8, 16, 32}};
constexpr u8 OBJ_H[3][4] = {{8, 16, 32, 64}, {8, 8, 16, 32}, {16, 32, 32, 64}};

// BGs each mode draws.
constexpr u8 MODE_BGS[8] = {0xF, 0x7, 0xC, 0x4, 0x4, 0x4, 0, 0};
} // namespace

u16 Ppu::reg16(u32 off) const { return static_cast<u16>(g_.io[off] | (g_.io[off + 1] << 8)); }

void Ppu::reset() {
  fb_.fill(0xFF000000u);
  for (int i = 0; i < 2; ++i) { ref_x_[i] = ref_y_[i] = 0; }
}

void Ppu::vblank() {
  for (int i = 0; i < 2; ++i) {
    const u32 base = 0x28 + i * 0x10;
    ref_x_[i] = ref28(reg16(base) | (u32{reg16(base + 2)} << 16));
    ref_y_[i] = ref28(reg16(base + 4) | (u32{reg16(base + 6)} << 16));
  }
}

void Ppu::ref_written(u32 off) {
  const int i = off >= 0x38 ? 1 : 0;
  const u32 base = 0x28 + i * 0x10;
  if ((off & 7) < 4) ref_x_[i] = ref28(reg16(base) | (u32{reg16(base + 2)} << 16));
  else ref_y_[i] = ref28(reg16(base + 4) | (u32{reg16(base + 6)} << 16));
}

void Ppu::line_done() {
  for (int i = 0; i < 2; ++i) {
    const u32 base = 0x20 + i * 0x10;
    ref_x_[i] += static_cast<s16>(reg16(base + 2));   // PB
    ref_y_[i] += static_cast<s16>(reg16(base + 6));   // PD
  }
}

void Ppu::draw_text(int bg, u32 y) {
  const u8* vram = g_.vram.get();
  const u16* pal = reinterpret_cast<const u16*>(g_.palette.get());
  const u16 cnt = reg16(0x08 + bg * 2);
  const u32 hofs = reg16(0x10 + bg * 4) & 0x1FF, vofs = reg16(0x12 + bg * 4) & 0x1FF;
  const u16 mosaic = reg16(0x4C);
  u32 ye = y;
  if (cnt & 0x40) ye -= y % (((mosaic >> 4) & 15) + 1);
  const u32 size = cnt >> 14;
  const u32 w = (size & 1) ? 512 : 256, h = (size & 2) ? 512 : 256;
  const u32 sy = (ye + vofs) & (h - 1);
  const u32 screen = ((cnt >> 8) & 0x1F) * 0x800, chars = ((cnt >> 2) & 3) * 0x4000;
  const bool bpp8 = cnt & 0x80;
  u16* out = bg_[bg].data();
  for (u32 x = 0; x < 240; ++x) {
    const u32 sx = (x + hofs) & (w - 1);
    u32 block = sx >> 8;
    if (sy >= 256) block += (w == 512) ? 2 : 1;
    const u32 ea = (screen + block * 0x800 + ((sy & 255) >> 3) * 64 + ((sx & 255) >> 3) * 2) & 0xFFFF;
    const u16 e = static_cast<u16>(vram[ea] | (vram[ea + 1] << 8));
    u32 px = sx & 7, py = sy & 7;
    if (e & 0x400) px = 7 - px;
    if (e & 0x800) py = 7 - py;
    const u32 tile = e & 0x3FF;
    u32 idx;
    if (bpp8) {
      const u32 a = chars + tile * 64 + py * 8 + px;
      idx = a < 0x10000 ? vram[a] : 0;
      out[x] = idx ? static_cast<u16>((pal[idx] & 0x7FFF) | OPAQUE) : 0;
    } else {
      const u32 a = chars + tile * 32 + py * 4 + (px >> 1);
      idx = a < 0x10000 ? (vram[a] >> ((px & 1) * 4)) & 15 : 0;
      out[x] = idx ? static_cast<u16>((pal[(e >> 12) * 16 + idx] & 0x7FFF) | OPAQUE) : 0;
    }
  }
}

void Ppu::draw_affine(int bg) {
  const u8* vram = g_.vram.get();
  const u16* pal = reinterpret_cast<const u16*>(g_.palette.get());
  const int i = bg - 2;
  const u16 cnt = reg16(0x08 + bg * 2);
  const s32 pa = static_cast<s16>(reg16(0x20 + i * 0x10)), pc = static_cast<s16>(reg16(0x24 + i * 0x10));
  const s32 size = 128 << (cnt >> 14);
  const u32 screen = ((cnt >> 8) & 0x1F) * 0x800, chars = ((cnt >> 2) & 3) * 0x4000;
  const bool wrap = cnt & 0x2000;
  s32 tx = ref_x_[i], ty = ref_y_[i];
  u16* out = bg_[bg].data();
  for (u32 x = 0; x < 240; ++x, tx += pa, ty += pc) {
    s32 px = tx >> 8, py = ty >> 8;
    if (wrap) { px &= size - 1; py &= size - 1; }
    else if (px < 0 || py < 0 || px >= size || py >= size) { out[x] = 0; continue; }
    const u32 tile = vram[(screen + (py >> 3) * (size >> 3) + (px >> 3)) & 0xFFFF];
    const u32 a = chars + tile * 64 + (py & 7) * 8 + (px & 7);
    const u32 idx = a < 0x10000 ? vram[a] : 0;
    out[x] = idx ? static_cast<u16>((pal[idx] & 0x7FFF) | OPAQUE) : 0;
  }
}

void Ppu::draw_bitmap(u32 mode) {
  const u8* vram = g_.vram.get();
  const u16* pal = reinterpret_cast<const u16*>(g_.palette.get());
  const s32 pa = static_cast<s16>(reg16(0x20)), pc = static_cast<s16>(reg16(0x24));
  const u32 frame = (reg16(0) & 0x10) ? 0xA000 : 0;
  const s32 w = mode == 5 ? 160 : 240, h = mode == 5 ? 128 : 160;
  s32 tx = ref_x_[0], ty = ref_y_[0];
  u16* out = bg_[2].data();
  for (u32 x = 0; x < 240; ++x, tx += pa, ty += pc) {
    const s32 px = tx >> 8, py = ty >> 8;
    if (px < 0 || py < 0 || px >= w || py >= h) { out[x] = 0; continue; }
    if (mode == 4) {
      const u32 idx = vram[frame + py * 240 + px];
      out[x] = idx ? static_cast<u16>((pal[idx] & 0x7FFF) | OPAQUE) : 0;
    } else {
      const u32 a = (mode == 5 ? frame : 0) + (py * w + px) * 2;
      out[x] = static_cast<u16>(((vram[a] | (vram[a + 1] << 8)) & 0x7FFF) | OPAQUE);
    }
  }
}

void Ppu::draw_sprites(u32 y) {
  obj_.fill(0); obj_prio_.fill(4); obj_flags_.fill(0); objwin_.fill(0);
  const u16 dispcnt = reg16(0);
  if (!(dispcnt & 0x1000)) return;
  const u8* vram = g_.vram.get();
  const u16* oam = reinterpret_cast<const u16*>(g_.oam.get());
  const u16* pal = reinterpret_cast<const u16*>(g_.palette.get()) + 256;
  const bool one_d = dispcnt & 0x40, bitmap = (dispcnt & 7) >= 3;
  const u16 mosaic = reg16(0x4C);
  const u32 mos_h = ((mosaic >> 8) & 15) + 1, mos_v = ((mosaic >> 12) & 15) + 1;
  bool any_mosaic = false;
  for (int i = 0; i < 128; ++i) {
    const u16 a0 = oam[i * 4], a1 = oam[i * 4 + 1], a2 = oam[i * 4 + 2];
    const bool affine = a0 & 0x100;
    if (!affine && (a0 & 0x200)) continue;          // disabled
    const u32 mode = (a0 >> 10) & 3, shape = a0 >> 14, size = a1 >> 14;
    if (mode == 3 || shape == 3) continue;
    const s32 w = OBJ_W[shape][size], h = OBJ_H[shape][size];
    const bool dbl = affine && (a0 & 0x200);
    const s32 bw = dbl ? w * 2 : w, bh = dbl ? h * 2 : h;
    const s32 oy = a0 & 0xFF;
    s32 ly = static_cast<s32>((y - oy) & 0xFF);
    if (ly >= bh) continue;
    const bool mos = a0 & 0x1000;
    if (mos) {
      const s32 my = static_cast<s32>(((y - y % mos_v) - oy) & 0xFF);
      if (my < bh) ly = my;
      any_mosaic = true;
    }
    s32 ox = a1 & 0x1FF;
    if (ox >= 240) ox -= 512;
    const bool bpp8 = a0 & 0x2000;
    const u32 tile = a2 & 0x3FF, prio = (a2 >> 10) & 3, bank = a2 >> 12;
    if (bitmap && tile < 512) continue;
    const u32 row_tiles = one_d ? static_cast<u32>(w / 8) * (bpp8 ? 2 : 1) : 32;
    const u8 flags = static_cast<u8>((mode == 1 ? 1 : 0) | (mos ? 2 : 0));
    s32 pa = 256, pb = 0, pc = 0, pd = 256;
    if (affine) {
      const u32 p = ((a1 >> 9) & 0x1F) * 16;
      pa = static_cast<s16>(oam[p + 3]); pb = static_cast<s16>(oam[p + 7]);
      pc = static_cast<s16>(oam[p + 11]); pd = static_cast<s16>(oam[p + 15]);
    }
    const s32 iy = ly - bh / 2;
    for (s32 ix = 0; ix < bw; ++ix) {
      const s32 X = ox + ix;
      if (X < 0 || X >= 240) continue;
      s32 tx, ty;
      if (affine) {
        const s32 dx = ix - bw / 2;
        tx = ((pa * dx + pb * iy) >> 8) + w / 2;
        ty = ((pc * dx + pd * iy) >> 8) + h / 2;
        if (tx < 0 || ty < 0 || tx >= w || ty >= h) continue;
      } else {
        tx = (a1 & 0x1000) ? w - 1 - ix : ix;
        ty = (a1 & 0x2000) ? h - 1 - ly : ly;
      }
      const u32 t = tile + static_cast<u32>(ty >> 3) * row_tiles + static_cast<u32>(tx >> 3) * (bpp8 ? 2 : 1);
      u32 idx;
      u16 col;
      if (bpp8) {
        idx = vram[0x10000 + (t & 0x3FF) * 32 + (ty & 7) * 8 + (tx & 7)];
        col = pal[idx];
      } else {
        idx = (vram[0x10000 + (t & 0x3FF) * 32 + (ty & 7) * 4 + ((tx & 7) >> 1)] >> ((tx & 1) * 4)) & 15;
        col = pal[bank * 16 + idx];
      }
      if (!idx) continue;
      if (mode == 2) { objwin_[X] = 1; continue; }
      if (prio < obj_prio_[X]) {
        obj_[X] = static_cast<u16>((col & 0x7FFF) | OPAQUE);
        obj_prio_[X] = static_cast<u8>(prio);
        obj_flags_[X] = flags;
      }
    }
  }
  if (any_mosaic && mos_h > 1) {
    for (u32 x = 0; x < 240; ++x) {
      if (!(obj_flags_[x] & 2)) continue;
      const u32 s = x - x % mos_h;
      if (s == x || !(obj_flags_[s] & 2)) continue;
      obj_[x] = obj_[s]; obj_prio_[x] = obj_prio_[s]; obj_flags_[x] = obj_flags_[s];
    }
  }
}

void Ppu::build_windows(u32 y) {
  const u16 dispcnt = reg16(0);
  if (!(dispcnt & 0xE000)) { win_.fill(0x3F); return; }
  const u16 winin = reg16(0x48), winout = reg16(0x4A);
  win_.fill(static_cast<u8>(winout & 0x3F));
  if (dispcnt & 0x8000)
    for (u32 x = 0; x < 240; ++x) if (objwin_[x]) win_[x] = static_cast<u8>((winout >> 8) & 0x3F);
  for (int w = 1; w >= 0; --w) {
    if (!(dispcnt & (0x2000 << w))) continue;
    const u16 hr = reg16(0x40 + w * 2), vr = reg16(0x44 + w * 2);
    const u32 x1 = hr >> 8, x2 = hr & 0xFF, y1 = vr >> 8, y2 = vr & 0xFF;
    const bool in_y = y1 <= y2 ? (y >= y1 && y < y2) : (y >= y1 || y < y2);
    if (!in_y) continue;
    const u8 v = static_cast<u8>((winin >> (8 * w)) & 0x3F);
    for (u32 x = 0; x < 240; ++x) {
      const bool in_x = x1 <= x2 ? (x >= x1 && x < x2) : (x >= x1 || x < x2);
      if (in_x) win_[x] = v;
    }
  }
}

void Ppu::compose(u32 y) {
  const u16 dispcnt = reg16(0);
  const u16* pal = reinterpret_cast<const u16*>(g_.palette.get());
  const u16 bd = pal[0] & 0x7FFF;
  const u16 bldcnt = reg16(0x50), bldalpha = reg16(0x52);
  const u32 eff = (bldcnt >> 6) & 3;
  const u32 eva = std::min<u32>(16, bldalpha & 0x1F), evb = std::min<u32>(16, (bldalpha >> 8) & 0x1F);
  const u32 evy = std::min<u32>(16, reg16(0x54) & 0x1F);
  const u32 bgs = MODE_BGS[dispcnt & 7] & ((dispcnt >> 8) & 0xF);
  // BGs in drawing order: priority, then number.
  int order[4], prios[4], n = 0;
  for (u32 p = 0; p < 4; ++p)
    for (int b = 0; b < 4; ++b)
      if ((bgs & (1u << b)) && (reg16(0x08 + b * 2) & 3) == p) { order[n] = b; prios[n] = static_cast<int>(p); ++n; }
  const u32 first = bldcnt & 0x3F, second = (bldcnt >> 8) & 0x3F;
  u32* out = fb_.data() + y * 240;
  for (u32 x = 0; x < 240; ++x) {
    const u8 w = win_[x];
    u16 c[2] = {bd, bd};
    u32 id[2] = {5, 5};
    bool semi = false;
    int found = 0, k = 0;
    for (u32 p = 0; p < 4 && found < 2; ++p) {
      if (obj_prio_[x] == p && (w & 0x10)) {
        if (found == 0) semi = obj_flags_[x] & 1;
        c[found] = obj_[x] & 0x7FFF; id[found] = 4; ++found;
      }
      for (; k < n && prios[k] == static_cast<int>(p) && found < 2; ++k) {
        const int b = order[k];
        if ((w & (1u << b)) && (bg_[b][x] & OPAQUE)) { c[found] = bg_[b][x] & 0x7FFF; id[found] = static_cast<u32>(b); ++found; }
      }
      while (k < n && prios[k] == static_cast<int>(p)) ++k;
    }
    u16 col = c[0];
    if (w & 0x20) {
      const bool top1 = first & (1u << id[0]), bot2 = found >= 1 && (second & (1u << id[1]));
      u32 mode = 0;
      if (semi && bot2) mode = 1;
      else if (eff == 1 && top1 && bot2) mode = 1;
      else if ((eff == 2 || eff == 3) && top1) mode = eff;
      if (mode == 1) {
        const u32 a = c[0], b = c[1];
        const u32 r = std::min<u32>(31, ((a & 31) * eva + (b & 31) * evb) >> 4);
        const u32 g = std::min<u32>(31, (((a >> 5) & 31) * eva + ((b >> 5) & 31) * evb) >> 4);
        const u32 bl = std::min<u32>(31, (((a >> 10) & 31) * eva + ((b >> 10) & 31) * evb) >> 4);
        col = static_cast<u16>(r | g << 5 | bl << 10);
      } else if (mode == 2) {
        u32 r = col & 31, g = (col >> 5) & 31, b = (col >> 10) & 31;
        r += ((31 - r) * evy) >> 4; g += ((31 - g) * evy) >> 4; b += ((31 - b) * evy) >> 4;
        col = static_cast<u16>(r | g << 5 | b << 10);
      } else if (mode == 3) {
        u32 r = col & 31, g = (col >> 5) & 31, b = (col >> 10) & 31;
        r -= (r * evy) >> 4; g -= (g * evy) >> 4; b -= (b * evy) >> 4;
        col = static_cast<u16>(r | g << 5 | b << 10);
      }
    }
    out[x] = expand(col);
  }
}

void Ppu::render_line(u32 y) {
  const u16 dispcnt = reg16(0);
  if (dispcnt & 0x80) {   // forced blank: white
    std::fill(fb_.begin() + y * 240, fb_.begin() + (y + 1) * 240, 0xFFFFFFFFu);
    return;
  }
  const u32 mode = dispcnt & 7;
  const u32 bgs = MODE_BGS[mode] & ((dispcnt >> 8) & 0xF);
  const u16 mosaic = reg16(0x4C);
  const u32 mos_h = (mosaic & 15) + 1;
  for (int b = 0; b < 4; ++b) {
    if (!(bgs & (1u << b))) continue;
    if (mode == 0 || (mode == 1 && b < 2)) draw_text(b, y);
    else if (mode <= 2) draw_affine(b);
    else draw_bitmap(mode);
    if ((reg16(0x08 + b * 2) & 0x40) && mos_h > 1) {
      u16* l = bg_[b].data();
      for (u32 x = 0; x < 240; ++x) l[x] = l[x - x % mos_h];
    }
  }
  draw_sprites(y);
  build_windows(y);
  compose(y);
}

template <class S> void Ppu::sync_state(S& s) {
  s.begin("GPPU");
  s.fields(ref_x_, ref_y_);
  s.blob(fb_.data(), fb_.size() * sizeof(u32));
  s.end();
}
template void Ppu::sync_state<state::Writer>(state::Writer&);
template void Ppu::sync_state<state::Reader>(state::Reader&);

} // namespace ds::gba
