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

// compose's step: where m is set, the layer's colour c and id go on top and
// what was on top goes second. Separate arrays, a mask a pixel: it vectorises.
void paint(u16* __restrict top, u16* __restrict sec, u16* __restrict top_id, u16* __restrict sec_id,
           const u16* __restrict m, const u16* __restrict c, u16 id) {
  for (u32 x = 0; x < 240; ++x) {
    const u16 k = m[x];
    sec[x] = static_cast<u16>((sec[x] & ~k) | (top[x] & k)); sec_id[x] = static_cast<u16>((sec_id[x] & ~k) | (top_id[x] & k));
    top[x] = static_cast<u16>((top[x] & ~k) | (c[x] & k)); top_id[x] = static_cast<u16>((top_id[x] & ~k) | (id & k));
  }
}
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
  // A tile at a time: its map entry and its row of pixels once, then the pixels of it on this line.
  for (u32 x = 0; x < 240;) {
    const u32 sx = (x + hofs) & (w - 1);
    u32 block = sx >> 8;
    if (sy >= 256) block += (w == 512) ? 2 : 1;
    const u32 ea = (screen + block * 0x800 + ((sy & 255) >> 3) * 64 + ((sx & 255) >> 3) * 2) & 0xFFFF;
    const u16 e = static_cast<u16>(vram[ea] | (vram[ea + 1] << 8));
    const u32 py = (e & 0x800) ? 7 - (sy & 7) : sy & 7;
    const u32 tile = e & 0x3FF;
    const u32 x0 = sx & 7, n = std::min<u32>(8 - x0, 240 - x);
    const u32 flip = (e & 0x400) ? 7 : 0;   // px = (x0 + i) ^ 7 when flipped
    if (bpp8) {
      const u32 a = chars + tile * 64 + py * 8;   // 8-aligned: all in or all out of BG VRAM
      u64 row = 0;
      if (a < 0x10000) std::memcpy(&row, vram + a, 8);
      for (u32 i = 0; i < n; ++i) {
        const u32 idx = static_cast<u32>(row >> (((x0 + i) ^ flip) * 8)) & 0xFF;
        out[x + i] = idx ? static_cast<u16>((pal[idx] & 0x7FFF) | OPAQUE) : 0;
      }
    } else {
      const u32 a = chars + tile * 32 + py * 4;
      u32 row = 0;
      if (a < 0x10000) std::memcpy(&row, vram + a, 4);
      const u16* bank = pal + (e >> 12) * 16;
      for (u32 i = 0; i < n; ++i) {
        const u32 idx = (row >> (((x0 + i) ^ flip) * 4)) & 15;
        out[x + i] = idx ? static_cast<u16>((bank[idx] & 0x7FFF) | OPAQUE) : 0;
      }
    }
    x += n;
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
  obj_prios_ = 0;
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
    const s32 ix0 = std::max<s32>(0, -ox), ix1 = std::min<s32>(bw, 240 - ox);   // the on-screen columns
    auto put = [&](s32 X, u32 idx, u16 col) {
      if (!idx) return;
      if (mode == 2) { objwin_[X] = 1; return; }
      if (prio < obj_prio_[X]) {
        obj_prios_ |= static_cast<u8>(1u << prio);
        obj_[X] = static_cast<u16>((col & 0x7FFF) | OPAQUE);
        obj_prio_[X] = static_cast<u16>(prio);
        obj_flags_[X] = flags;
      }
    };
    const u16* bank_pal = pal + bank * 16;
    if (!affine) {
      // One row of the sprite: its tiles' row bytes, read along x.
      const s32 ty = (a1 & 0x2000) ? h - 1 - ly : ly;
      const bool hflip = a1 & 0x1000;
      const u32 trow = tile + static_cast<u32>(ty >> 3) * row_tiles;
      const u8* rowp = vram + 0x10000 + (ty & 7) * (bpp8 ? 8 : 4);
      for (s32 ix = ix0; ix < ix1; ++ix) {
        const s32 tx = hflip ? w - 1 - ix : ix;
        const u32 t = (trow + static_cast<u32>(tx >> 3) * (bpp8 ? 2 : 1)) & 0x3FF;
        if (bpp8) { const u32 idx = rowp[t * 32 + (tx & 7)]; put(ox + ix, idx, pal[idx]); }
        else { const u32 idx = (rowp[t * 32 + ((tx & 7) >> 1)] >> ((tx & 1) * 4)) & 15; put(ox + ix, idx, bank_pal[idx]); }
      }
      continue;
    }
    for (s32 ix = ix0; ix < ix1; ++ix) {
      const s32 dx = ix - bw / 2;
      const s32 tx = ((pa * dx + pb * iy) >> 8) + w / 2;
      const s32 ty = ((pc * dx + pd * iy) >> 8) + h / 2;
      if (tx < 0 || ty < 0 || tx >= w || ty >= h) continue;
      const u32 t = tile + static_cast<u32>(ty >> 3) * row_tiles + static_cast<u32>(tx >> 3) * (bpp8 ? 2 : 1);
      if (bpp8) { const u32 idx = vram[0x10000 + (t & 0x3FF) * 32 + (ty & 7) * 8 + (tx & 7)]; put(ox + ix, idx, pal[idx]); }
      else { const u32 idx = (vram[0x10000 + (t & 0x3FF) * 32 + (ty & 7) * 4 + ((tx & 7) >> 1)] >> ((tx & 1) * 4)) & 15; put(ox + ix, idx, bank_pal[idx]); }
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
  // The two frontmost pixels of each column, painted back to front: per
  // priority from 3 to 0, the BGs from the highest number down, then the
  // sprites (which win over a BG of the same priority). Each layer drawn
  // pushes the one in front to second place; the backdrop (id 5) is behind
  // all. Branch-free (a mask a pixel), so the loops vectorise.
  alignas(16) u16 top[240], sec[240], top_id[240], sec_id[240];
  std::fill_n(top, 240, bd); std::fill_n(sec, 240, bd);
  std::fill_n(top_id, 240, u16{5}); std::fill_n(sec_id, 240, u16{5});
  alignas(16) u16 m[240], c[240];
  for (int p = 3; p >= 0; --p) {
    for (int b = 3; b >= 0; --b) {
      if (!(bgs & (1u << b)) || (reg16(0x08 + b * 2) & 3) != static_cast<u32>(p)) continue;
      const u16* l = bg_[b].data();
      for (u32 x = 0; x < 240; ++x) { m[x] = static_cast<u16>(0u - ((win_[x] >> b) & (l[x] >> 15) & 1u)); c[x] = static_cast<u16>(l[x] & 0x7FFF); }
      paint(top, sec, top_id, sec_id, m, c, static_cast<u16>(b));
    }
    if (!(obj_prios_ & (1u << p))) continue;   // no sprite pixel at this priority on the line
    const u16 pp = static_cast<u16>(p);
    for (u32 x = 0; x < 240; ++x) { m[x] = static_cast<u16>((obj_prio_[x] == pp ? 0xFFFFu : 0u) & (0u - ((win_[x] >> 4) & 1u))); c[x] = static_cast<u16>(obj_[x] & 0x7FFF); }
    paint(top, sec, top_id, sec_id, m, c, 4);
  }
  const u32 first = bldcnt & 0x3F, second = (bldcnt >> 8) & 0x3F;
  u32* out = fb_.data() + y * 240;
  // No colour effect can reach a pixel without the effects window bit, or
  // with no effect selected and no semi-transparent sprite on top: then the
  // pixel is the top colour.
  u32 any_fx = 0;
  if (eff) for (u32 x = 0; x < 240; ++x) any_fx |= win_[x] & 0x20u;
  else for (u32 x = 0; x < 240; ++x) any_fx |= win_[x] & 0x20u & (0u - static_cast<u32>((top_id[x] == 4) & obj_flags_[x] & 1));
  if (!any_fx) {
    for (u32 x = 0; x < 240; ++x) out[x] = expand(top[x]);
    return;
  }
  // Each pixel's effect, branch-free: alpha blending with the second layer
  // (a semi-transparent sprite on top, or effect 1 with the top a first
  // target), else brightness up (2) or down (3) where the top is a first
  // target; only inside the effects window. All three are worked out and the
  // one that applies is selected.
  const u32 e1 = eff == 1, e2 = eff == 2, e3 = eff == 3;
  for (u32 x = 0; x < 240; ++x) {
    const u32 a = top[x], b = sec[x], ti = top_id[x], si = sec_id[x];
    const u32 fx = (win_[x] >> 5) & 1;
    const u32 semi = (ti == 4) & obj_flags_[x] & 1;
    const u32 top1 = (first >> ti) & 1, bot2 = (ti != 5) & (second >> si) & 1;
    const u32 blend = fx & bot2 & (semi | (e1 & top1));
    const u32 up = fx & (blend ^ 1) & top1 & e2, down = fx & (blend ^ 1) & top1 & e3;
    const u32 ar = a & 31, ag = (a >> 5) & 31, ab = (a >> 10) & 31;
    const u32 br = b & 31, bg = (b >> 5) & 31, bb = (b >> 10) & 31;
    const u32 mr = std::min<u32>(31, (ar * eva + br * evb) >> 4), mg = std::min<u32>(31, (ag * eva + bg * evb) >> 4), mb = std::min<u32>(31, (ab * eva + bb * evb) >> 4);
    const u32 ur = ar + (((31 - ar) * evy) >> 4), ug = ag + (((31 - ag) * evy) >> 4), ub = ab + (((31 - ab) * evy) >> 4);
    const u32 dr = ar - ((ar * evy) >> 4), dg = ag - ((ag * evy) >> 4), db = ab - ((ab * evy) >> 4);
    const u32 m_blend = 0u - blend, m_up = 0u - up, m_down = 0u - down, m_none = ~(m_blend | m_up | m_down);
    const u32 col = ((mr | mg << 5 | mb << 10) & m_blend) | ((ur | ug << 5 | ub << 10) & m_up) | ((dr | dg << 5 | db << 10) & m_down) | (a & m_none);
    out[x] = expand(static_cast<u16>(col));
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
