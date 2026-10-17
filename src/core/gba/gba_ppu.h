// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// The GBA's picture processor, drawn a line at a time at each line's HBlank
// from the live registers and memories (no journal: a write lands on the next
// line drawn, which is the hardware's granularity for nearly everything).
// Modes 0-5, text and affine BGs, the three bitmap modes, 128 sprites
// (normal, affine, double size, semi-transparent, OBJ window), windows,
// mosaic, alpha blending and brightness.
//
// Why not the DS's Engine2D: it is built around the DS's VRAM banking
// (VramMap), its write journal and lazy batches, 256-pixel lines and 18-bit
// colour; the GBA's fixed 96 KB layout, its bitmap modes and 240-pixel lines
// would need an adapter larger than this file, and would put GBA branches on
// the DS's hottest path.
#pragma once
#include "core/types.h"

#include <array>

namespace ds::gba {

struct Gba;

class Ppu {
public:
  explicit Ppu(Gba& g) : g_(g) {}
  void reset();
  void render_line(u32 y);
  // Affine reference points: reloaded from BGnX/BGnY at VBlank and on a
  // write to them, advanced by PB/PD after every drawn line.
  void vblank();
  void line_done();
  void ref_written(u32 off);     // I/O offset 0x28-0x2F or 0x38-0x3F

  const u32* framebuffer() const { return fb_.data(); }
  template <class S> void sync_state(S& s);

private:
  Gba& g_;
  std::array<u32, 240 * 160> fb_{};
  s32 ref_x_[2] = {}, ref_y_[2] = {};   // BG2, BG3 internal (28-bit signed, 8 fraction bits)

  // One line's layers: BGR555 with bit 15 set where opaque.
  static constexpr u16 OPAQUE = 0x8000;
  std::array<std::array<u16, 240>, 4> bg_{};
  std::array<u16, 240> obj_{};
  std::array<u8, 240> obj_prio_{};       // 0-3; 4 = no sprite pixel
  std::array<u8, 240> obj_flags_{};      // bit 0 semi-transparent, bit 1 mosaic
  std::array<u8, 240> objwin_{};         // in the OBJ window
  std::array<u8, 240> win_{};            // per pixel: bits 0-3 BG, 4 OBJ, 5 colour effects

  u16 reg16(u32 off) const;
  void draw_text(int bg, u32 y);
  void draw_affine(int bg);
  void draw_bitmap(u32 mode);
  void draw_sprites(u32 y);
  void build_windows(u32 y);
  void compose(u32 y);
};

} // namespace ds::gba
