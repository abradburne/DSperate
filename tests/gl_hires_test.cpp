// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// The hi-res 3D path (frontend/sdl/gl_hires.cpp) end to end. Each scene is
// set up the way a game would: registers and geometry commands written
// through the bus, frames run, and what the frontend reads after a frame
// (the polygon list, the texture arena, engine A's kept lines) drawn again
// by GlHires at 2x and 4x on an offscreen GLES 3.1 context. Two checks:
//
//  - against the CPU's own 1x picture: wherever the 1x picture is uniform
//    over a pixel and its neighbours (away from polygon edges, texel and
//    colour steps), every hi-res sample inside that pixel must equal it, up
//    to what a scene states (Scene::tolerance). This holds on any
//    conforming driver.
//  - against golden hashes of the whole hi-res picture, recorded for the GL
//    renderer they were taken on (kGolden). Rasterisation and interpolation
//    differ between drivers, so a renderer not in the table runs the first
//    check only. `--print` prints this renderer's table.
//
// `--dump DIR` writes each scene's 1x and hi-res pictures there as PPM, for a look.
// Exits 77 (skipped) when no GLES 3.1 context can be made.
#include "check.h"
#include "core/nds.h"
#include "frontend/sdl/gl_hires.h"

#include <GLES3/gl31.h>
#include <SDL2/SDL.h>

#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

using namespace ds;

namespace {

// ---- the bus, as a game writes it ----

void w32(NDS& nds, u32 a, u32 v) { nds.bus.dma_write32(Cpu::ARM9, a, v); }
void w16(NDS& nds, u32 a, u16 v) { nds.bus.dma_write16(Cpu::ARM9, a, v); }
void w8(NDS& nds, u32 a, u8 v) { nds.bus.dma_write8(Cpu::ARM9, a, v); }
constexpr u32 CMD(u32 c) { return 0x04000400 + c * 4; }   // geometry command ports

constexpr u32 DISPCNT = 0x04000000, BG1CNT = 0x0400000A, BG0CNT = 0x04000008;
constexpr u32 WIN0H = 0x04000040, WIN0V = 0x04000044, WININ = 0x04000048, WINOUT = 0x0400004A;
constexpr u32 BLDCNT = 0x04000050, BLDALPHA = 0x04000052;
constexpr u32 DISP3DCNT = 0x04000060, DISPCAPCNT = 0x04000064, MASTER_BRIGHT = 0x0400006C;
constexpr u32 VRAMCNT_A = 0x04000240, VRAMCNT_B = 0x04000241, VRAMCNT_C = 0x04000242, VRAMCNT_E = 0x04000244;
constexpr u32 POWCNT1 = 0x04000304;
constexpr u32 EDGE_COLOR = 0x04000330, CLEAR_COLOR = 0x04000350, CLEAR_DEPTH = 0x04000354;
constexpr u32 FOG_COLOR = 0x04000358, FOG_OFFSET = 0x0400035C, FOG_TABLE = 0x04000360;
constexpr u32 PALETTE_A = 0x05000000;

constexpr u16 rgb15(u32 r, u32 g, u32 b) { return static_cast<u16>(r | (g << 5) | (b << 10)); }

// Screen pixels to the 4.12 clip coordinates of an identity projection.
s16 cx(int px) { return static_cast<s16>((px * 8192) / 256 - 4096); }
s16 cy(int py) { return static_cast<s16>(4096 - (py * 8192) / 192); }

struct Geo {
  NDS& nds;
  void cmd(u32 c, u32 v) { w32(nds, CMD(c), v); }
  void identity() { cmd(0x10, 0); cmd(0x15, 0); cmd(0x10, 1); cmd(0x15, 0); cmd(0x10, 2); cmd(0x15, 0); }
  void viewport() { cmd(0x60, 0xBFFF0000); }
  // A frustum (near 0.5, far 4) for row vectors: w = -z.
  void perspective() {
    const s32 m[16] = {4096, 0, 0, 0, 0, 4096, 0, 0, 0, 0, -5266, -4096, 0, 0, -4681, 0};
    cmd(0x10, 0);
    for (s32 v : m) cmd(0x16, static_cast<u32>(v));
    cmd(0x10, 1);
  }
  // POLYGON_ATTR: mode 0 modulate / 1 decal / 2 toon / 3 shadow; both faces drawn.
  void attr(u32 alpha, u32 id, u32 mode = 0, bool fog = false, bool depth_write = false) {
    cmd(0x29, (mode << 4) | 0xC0u | (depth_write ? 1u << 11 : 0u) | (fog ? 1u << 15 : 0u) | (alpha << 16) | (id << 24));
  }
  void notex() { cmd(0x2A, 0); }
  // TEXIMAGE_PARAM: a w x h texture (8 << sw, 8 << sh) at `off` bytes, repeating.
  void tex(u32 fmt, u32 off, u32 sw, u32 sh, bool color0_transparent = false) {
    cmd(0x2A, (off >> 3) | (3u << 16) | (sw << 20) | (sh << 23) | (fmt << 26) | (color0_transparent ? 1u << 29 : 0u));
  }
  void pal(u32 off) { cmd(0x2B, off >> 4); }
  void color(u16 c) { cmd(0x20, c); }
  void st(int s, int t) { cmd(0x22, (static_cast<u32>(t * 16) << 16) | (static_cast<u32>(s * 16) & 0xFFFF)); }
  void vtx(int px, int py, s16 z) {   // VTX_16
    cmd(0x23, (static_cast<u32>(static_cast<u16>(cy(py))) << 16) | static_cast<u16>(cx(px)));
    cmd(0x23, static_cast<u16>(z));
  }
  void vtx3(double x, double y, double z) {   // VTX_16 in world units
    auto f = [](double v) { return static_cast<u16>(static_cast<s16>(v * 4096.0)); };
    cmd(0x23, (static_cast<u32>(f(y)) << 16) | f(x));
    cmd(0x23, f(z));
  }
  // An axis-aligned quad from (x0, y0) to (x1, y1), texels 1:1 from (s0, t0).
  void quad(int x0, int y0, int x1, int y1, s16 z, int s0 = 0, int t0 = 0) {
    cmd(0x40, 1);
    st(s0, t0); vtx(x0, y0, z);
    st(s0 + (x1 - x0), t0); vtx(x1, y0, z);
    st(s0 + (x1 - x0), t0 + (y1 - y0)); vtx(x1, y1, z);
    st(s0, t0 + (y1 - y0)); vtx(x0, y1, z);
    cmd(0x41, 0);
  }
  void tri(int x0, int y0, u16 c0, int x1, int y1, u16 c1, int x2, int y2, u16 c2, s16 z) {
    cmd(0x40, 0);
    color(c0); vtx(x0, y0, z); color(c1); vtx(x1, y1, z); color(c2); vtx(x2, y2, z);
    cmd(0x41, 0);
  }
  void swap(bool wbuffer = false) { cmd(0x50, 1u | (wbuffer ? 2u : 0u)); }   // translucent polygons in submission order
};

// Textures, written through LCDC mapping and then handed to the 3D engine.
// Slot 0 of texture memory is bank B, the palettes bank E.
constexpr u32 TEX_CHECKER = 0x0000;   // 32x32 direct colour (format 7), 4x4-texel cells
constexpr u32 TEX_A3I5 = 0x1000;      // 32x32 A3I5 (format 1): alpha 0, partial and full bands
void load_textures(NDS& nds) {
  w8(nds, VRAMCNT_B, 0x80);   // LCDC: 0x06820000
  w8(nds, VRAMCNT_E, 0x80);   // LCDC: 0x06880000
  for (u32 y = 0; y < 32; ++y)
    for (u32 x = 0; x < 32; ++x) {
      const bool c = ((x >> 2) ^ (y >> 2)) & 1;
      w16(nds, 0x06820000 + TEX_CHECKER + (y * 32 + x) * 2, static_cast<u16>(0x8000 | (c ? rgb15(31, 28, 4) : rgb15(4, 10, 25))));
    }
  // A3I5: columns of 8 texels: alpha 0, alpha 3 (of 7), alpha 7, alpha 7; index by row band.
  for (u32 y = 0; y < 32; ++y)
    for (u32 x = 0; x < 32; x += 2) {
      auto texel = [&](u32 xx) { const u32 a3 = (xx < 8) ? 0 : (xx < 16) ? 3 : 7; return static_cast<u8>((a3 << 5) | ((y >> 3) & 3)); };
      w16(nds, 0x06820000 + TEX_A3I5 + y * 32 + x, static_cast<u16>(texel(x) | (texel(x + 1) << 8)));
    }
  const u16 pal4[4] = {rgb15(31, 0, 0), rgb15(0, 31, 0), rgb15(0, 0, 31), rgb15(31, 31, 31)};
  for (u32 i = 0; i < 4; ++i) w16(nds, 0x06880000 + i * 2, pal4[i]);
  w8(nds, VRAMCNT_B, 0x83);   // texture slot 0
  w8(nds, VRAMCNT_E, 0x83);   // texture palette slots 0-3
}

// BG1: a text layer over the right half of the screen, 8x8 tiles of two
// colours, so the 3D layer meets 2D priority and colour effects.
void load_bg1(NDS& nds, u32 priority) {
  w8(nds, VRAMCNT_A, 0x81);   // engine A BG at 0x06000000
  for (u32 i = 0; i < 16; ++i) w16(nds, 0x06004000 + 32 + i * 2, 0x1111);   // tile 1: colour 1
  for (u32 i = 0; i < 16; ++i) w16(nds, 0x06004000 + 64 + i * 2, 0x2222);   // tile 2: colour 2
  for (u32 ty = 0; ty < 32; ++ty)
    for (u32 tx = 0; tx < 32; ++tx) w16(nds, 0x06000000 + (ty * 32 + tx) * 2, tx < 16 ? 0 : static_cast<u16>(1 + ((tx ^ ty) & 1)));
  w16(nds, PALETTE_A + 2, rgb15(31, 16, 0));
  w16(nds, PALETTE_A + 4, rgb15(0, 20, 31));
  w16(nds, PALETTE_A, rgb15(2, 2, 6));   // backdrop
  w16(nds, BG1CNT, static_cast<u16>((1u << 2) | priority));   // tiles at 0x06004000, map at 0x06000000
}

struct Scene {
  const char* name;
  std::function<void(NDS&)> setup;   // registers and VRAM, once
  std::function<void(Geo&)> draw;    // the frame's geometry, every frame
  int mode = 0;                      // GlHires::render mode: 0 shown, 1 captured
  // Translucent polygons blend in GL's fixed function on 8-bit targets, which
  // rounds where the DS truncates: such pixels may be one step (of 64) off.
  // Two translucent layers over each other: two steps. kNotCompared: a
  // known difference (see the scene), golden hashes only.
  u32 tolerance = 0;
  // Pixels compared lie this far from any step in the 1x picture. The DS
  // interpolates perspective approximately, so its texel steps can sit a
  // fraction of a pixel from GL's exact ones: 2 for perspective textures.
  int margin = 1;
};
constexpr u32 kNotCompared = ~0u;

const u16 kRed = rgb15(31, 4, 4), kGreen = rgb15(4, 31, 8), kBlue = rgb15(6, 8, 31), kGrey = rgb15(18, 18, 18);

std::vector<Scene> scenes() {
  std::vector<Scene> s;
  auto base = [](NDS& nds, u32 disp3d) {
    w16(nds, POWCNT1, 0x820F);
    w32(nds, DISPCNT, 0x00010108);   // display mode 1, BG0 on and 3D
    w16(nds, DISP3DCNT, static_cast<u16>(disp3d));
    w32(nds, CLEAR_COLOR, 0x001F0000u | rgb15(3, 6, 12));
    w32(nds, CLEAR_DEPTH, 0x7FFF);
  };
  s.push_back({"opaque", [base](NDS& nds) { base(nds, 0x0001); load_textures(nds); },
               [](Geo& g) {
                 g.identity(); g.viewport();
                 g.notex(); g.attr(31, 1); g.color(kGrey); g.quad(16, 16, 120, 100, 0x400);
                 g.attr(31, 2); g.color(kRed); g.quad(60, 40, 180, 140, 0x200);       // in front of the grey
                 g.attr(31, 3); g.color(kGreen); g.quad(100, 70, 240, 180, 0x600);    // behind both
                 g.attr(31, 4); g.tri(20, 120, kRed, 90, 186, kGreen, 10, 186, kBlue, 0x100);   // Gouraud
                 g.attr(31, 5); g.tex(7, TEX_CHECKER, 2, 2); g.color(0x7FFF); g.quad(150, 8, 246, 72, 0x100);
                 g.swap();
               }});
  s.push_back({"translucent", [base](NDS& nds) { base(nds, 0x0009); load_textures(nds); },
               [](Geo& g) {
                 g.identity(); g.viewport();
                 g.notex(); g.attr(31, 1); g.color(kGrey); g.quad(0, 0, 256, 192, 0x700);   // the floor
                 g.attr(16, 10); g.color(kRed); g.quad(20, 20, 140, 120, 0x300);
                 g.attr(16, 10); g.color(kBlue); g.quad(80, 50, 200, 150, 0x200);    // same id: no blend over the red
                 g.attr(12, 11); g.color(kGreen); g.quad(110, 30, 230, 110, 0x100);  // another id: blends
                 g.attr(20, 12, 0, false, true); g.color(kBlue); g.quad(30, 130, 120, 185, 0x500);   // writes depth
                 // Alpha 31 with an A3I5 texture: drawn as opaque where the texel is, blended where it is partial.
                 g.attr(31, 13); g.tex(1, TEX_A3I5, 2, 2); g.pal(0); g.color(0x7FFF); g.quad(150, 120, 246, 184, 0x080);
                 g.swap();
               }, 0, 2});
  s.push_back({"shadow", [base](NDS& nds) { base(nds, 0x0009); },
               [](Geo& g) {
                 g.identity(); g.viewport();
                 g.notex(); g.attr(31, 1); g.color(kGrey); g.quad(0, 0, 256, 192, 0x400);
                 g.attr(31, 2); g.color(kGreen); g.quad(150, 30, 230, 160, 0x300);   // a box the shadow also falls on
                 g.attr(20, 0, 3); g.color(0); g.quad(40, 40, 200, 150, 0x600);      // the mask: behind the floor, so marked where it fails
                 g.attr(20, 5, 3); g.color(0); g.quad(40, 40, 200, 150, 0x080);      // the shadow: in front, drawn only where marked
                 g.swap();
               }, 0, 1});
  s.push_back({"edge_fog",
               [base](NDS& nds) {
                 base(nds, 0x00A9 | (1u << 8));   // texture, blend, edge marking, fog (colour and alpha), shift 1
                 for (u32 i = 0; i < 8; ++i) w16(nds, EDGE_COLOR + i * 2, rgb15(31 - i * 3, i * 4, 16));
                 w32(nds, FOG_COLOR, 0x001F0000u | rgb15(20, 20, 26));
                 w16(nds, FOG_OFFSET, 0x5000);
                 for (u32 i = 0; i < 32; ++i) w8(nds, FOG_TABLE + i, static_cast<u8>(i * 4));
               },
               [](Geo& g) {
                 g.identity(); g.viewport();
                 g.notex(); g.attr(31, 8, 0, true); g.color(kGrey); g.quad(0, 0, 256, 192, 0x0E00);
                 g.attr(31, 16, 0, true); g.color(kRed); g.quad(30, 30, 130, 120, 0x0400);
                 g.attr(31, 24, 0, true); g.color(kBlue); g.quad(90, 70, 220, 170, 0x0900);
                 g.attr(31, 32, 0, false); g.color(kGreen); g.quad(160, 10, 250, 60, 0x0600);   // no fog bit
                 g.attr(16, 40, 0, true); g.color(kGreen); g.quad(20, 140, 120, 190, 0x0200);   // translucent: keeps the fog bit under it
                 g.swap();
               }, 0, 1});
  // Perspective, W-buffered: a textured floor receding to the horizon, fogged
  // by distance, a box in front of it and a translucent pane between.
  s.push_back({"perspective_wbuffer",
               [base](NDS& nds) {
                 base(nds, 0x00A9 | (4u << 8));
                 load_textures(nds);
                 for (u32 i = 0; i < 8; ++i) w16(nds, EDGE_COLOR + i * 2, rgb15(31, 31 - i * 4, 0));
                 w32(nds, FOG_COLOR, 0x001F0000u | rgb15(24, 26, 30));
                 w16(nds, FOG_OFFSET, 0x0000);
                 for (u32 i = 0; i < 32; ++i) w8(nds, FOG_TABLE + i, static_cast<u8>(i * 4));
               },
               [](Geo& g) {
                 g.identity(); g.perspective(); g.viewport();
                 g.attr(31, 8, 0, true); g.tex(7, TEX_CHECKER, 2, 2); g.color(0x7FFF);
                 g.cmd(0x40, 1);
                 // Texels affine in the plane (20 a world unit): a quad whose texture
                 // is not (a trapezoid mapped to a square) is drawn as two triangles
                 // here and as spans on the DS, which interpolate it differently.
                 g.st(18, 0); g.vtx3(-0.6, -0.4, -0.6); g.st(42, 0); g.vtx3(0.6, -0.4, -0.6);
                 g.st(60, 28); g.vtx3(1.5, -0.4, -2.0); g.st(0, 28); g.vtx3(-1.5, -0.4, -2.0);
                 g.cmd(0x41, 0);
                 g.notex(); g.attr(31, 16, 0, true); g.color(kRed);
                 g.cmd(0x40, 1);
                 g.vtx3(-0.5, -0.3, -1.4); g.vtx3(-0.1, -0.3, -1.4); g.vtx3(-0.1, 0.2, -1.4); g.vtx3(-0.5, 0.2, -1.4);
                 g.cmd(0x41, 0);
                 g.attr(14, 24, 0, true); g.color(kBlue);
                 g.cmd(0x40, 1);
                 g.vtx3(-0.2, -0.35, -1.0); g.vtx3(0.35, -0.35, -1.0); g.vtx3(0.35, 0.1, -1.0); g.vtx3(-0.2, 0.1, -1.0);
                 g.cmd(0x41, 0);
                 g.swap(true);
               }, 0, 1, 2});
  // 2D over and under the 3D: BG1 (right half) in front of it, blended with
  // it as the second target, the 3D in front of the backdrop elsewhere; a
  // window without effects; master brightness over all.
  s.push_back({"composite_2d",
               [base](NDS& nds) {
                 base(nds, 0x0009);
                 w32(nds, DISPCNT, 0x00012308);   // + BG1, window 0
                 w16(nds, BG0CNT, 1);             // the 3D at priority 1
                 load_bg1(nds, 0);                // BG1 in front of it
                 w16(nds, BLDCNT, static_cast<u16>(0x0003 | (1u << 6) | (0x2101u << 8)));   // BG0/BG1 over BG0/BG1/backdrop: alpha blend
                 w16(nds, BLDALPHA, 0x0A06);
                 w16(nds, WIN0H, static_cast<u16>((150u << 8) | 230u));   // x 150..230
                 w16(nds, WIN0V, static_cast<u16>((100u << 8) | 180u));   // y 100..180
                 w16(nds, WININ, 0x001F);    // inside: layers, no effects
                 w16(nds, WINOUT, 0x003F);   // outside: layers and effects
                 w16(nds, MASTER_BRIGHT, static_cast<u16>((1u << 14) | 5u));   // brighten by 5/16
               },
               [](Geo& g) {
                 g.identity(); g.viewport();
                 g.notex(); g.attr(31, 1); g.color(kRed); g.quad(20, 20, 200, 120, 0x300);
                 g.attr(18, 2); g.color(kBlue); g.quad(100, 60, 190, 110, 0x200);   // translucent, over the red
                 g.attr(31, 3); g.color(kGreen); g.quad(30, 110, 240, 190, 0x100);
                 g.swap();
               }, 0, 1});
  // A known gap, kept out of the 1x comparison: a translucent polygon over a
  // transparent rear plane. The DS writes it unblended (the rear plane's
  // alpha is 0); GL blends it with the clear colour, and the 2D compositor
  // then blends that with what is behind the 3D.
  s.push_back({"translucent_over_clear",
               [base](NDS& nds) {
                 base(nds, 0x0009);
                 w32(nds, CLEAR_COLOR, rgb15(3, 6, 12));   // alpha 0
                 w32(nds, DISPCNT, 0x00010308);   // + BG1
                 w16(nds, BG0CNT, 0);
                 load_bg1(nds, 1);
                 w16(nds, BLDCNT, static_cast<u16>(0x0001 | (1u << 6) | (0x2002u << 8)));
                 w16(nds, BLDALPHA, 0x0A06);
               },
               [](Geo& g) {
                 g.identity(); g.viewport();
                 g.notex(); g.attr(18, 2); g.color(kBlue); g.quad(60, 40, 220, 160, 0x200);
                 g.swap();
               }, 0, kNotCompared});
  s.push_back({"capture",
               [base](NDS& nds) {
                 base(nds, 0x0001);
                 load_textures(nds);
                 w8(nds, VRAMCNT_C, 0x80);   // LCDC: the capture's destination
                 w16(nds, MASTER_BRIGHT, static_cast<u16>((2u << 14) | 6u));   // darken: the shown picture has it, the capture not
               },
               [](Geo& g) {
                 w32(g.nds, DISPCAPCNT, 0x80000000u | (2u << 16) | (3u << 20));   // engine A's screen, 256x192, into bank C: every frame, as it ends each capture
                 g.identity(); g.viewport();
                 g.notex(); g.attr(31, 1); g.color(kGrey); g.quad(16, 16, 240, 176, 0x400);
                 g.attr(31, 2); g.tex(7, TEX_CHECKER, 2, 2); g.color(0x7FFF); g.quad(60, 40, 200, 150, 0x200);
                 g.swap();
               },
               1});
  return s;
}

// ---- golden hashes: FNV-1a over the RGBA bytes of the hi-res picture ----

struct Golden { const char* renderer; const char* scene; u32 scale; u64 hash; };
constexpr Golden kGolden[] = {
#include "data/gl_hires/golden.inc"
    {"", "", 0, 0},   // the end
};

u64 fnv(const std::vector<u8>& px) {
  u64 h = 0xcbf29ce484222325ull;
  for (u8 b : px) h = (h ^ b) * 0x100000001b3ull;
  return h;
}

// ---- one scene, one scale ----

struct Result { u64 hash; u32 checked, mismatched; };

void write_ppm(const std::string& path, const u8* rgb, u32 w, u32 h, u32 stride) {
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return;
  std::fprintf(f, "P6\n%u %u\n255\n", w, h);
  for (u32 i = 0; i < w * h; ++i) std::fwrite(rgb + i * stride, 1, 3, f);
  std::fclose(f);
}

Result run_scene(const Scene& sc, u32 scale, const char* dump) {
  NDS nds;
  nds.gpu3d.renderer().set_display_list(true);
  nds.gpu.set_hires(true);
  sc.setup(nds);
  Geo g{nds};
  // The list drawn at frame N is shown at N + 1 and read after N + 2's run.
  // Registers written between frames reach the frame after next (a frame
  // ends at its line 0, already begun): five frames end on a capture.
  for (int f = 0; f < 5; ++f) { sc.draw(g); nds.run_frame(); }
  const int scr = nds.gpu.screen_a();
  const auto& ref = nds.gpu.frame_3d_shown();
  const gpu::Engine2D::HiresLine* lines = nds.gpu.hires_lines();
  CHECK(scr >= 0 && ref.dlist && lines);
  if (sc.mode == 1) CHECK(gpu::Gpu::hires_capture(nds.gpu.captured_last()) == 1);
  u32 atop = 0; u64 aep = 0;
  const std::vector<u32>& arena = nds.gpu3d.renderer().display_arena(&atop, &aep);

  sdl::GlHires gh;
  std::string why;
  CHECK(gh.init(scale, &why));
  const unsigned tex = gh.render(*ref.dlist, arena, atop, aep, lines, sc.mode);
  CHECK(tex != 0);
  const u32 W = 256 * scale, H = 192 * scale;
  GLuint fbo = 0;
  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
  std::vector<u8> px(static_cast<size_t>(W) * H * 4);
  glReadPixels(0, 0, static_cast<GLsizei>(W), static_cast<GLsizei>(H), GL_RGBA, GL_UNSIGNED_BYTE, px.data());
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glDeleteFramebuffers(1, &fbo);
  CHECK(glGetError() == GL_NO_ERROR);

  // The 1x reference: the screen as shown, or for a capture engine A's
  // lines before master brightness (the 6-bit records, expanded as shown).
  std::vector<u32> ref1x(256 * 192);
  if (sc.mode == 0) std::memcpy(ref1x.data(), nds.gpu.framebuffer(scr), ref1x.size() * 4);
  else for (u32 y = 0; y < 192; ++y) for (u32 x = 0; x < 256; ++x) {
    const u32 o = lines[y].out[x];
    auto e = [](u32 c6) { return (c6 << 2) | (c6 >> 4); };
    ref1x[y * 256 + x] = (e(o & 0x3F) << 16) | (e((o >> 8) & 0x3F) << 8) | e((o >> 16) & 0x3F);
  }
  if (dump) {
    std::vector<u8> rgb(256 * 192 * 3);
    for (u32 i = 0; i < 256 * 192; ++i) { rgb[i * 3] = static_cast<u8>(ref1x[i] >> 16); rgb[i * 3 + 1] = static_cast<u8>(ref1x[i] >> 8); rgb[i * 3 + 2] = static_cast<u8>(ref1x[i]); }
    const std::string stem = std::string(dump) + "/" + sc.name;
    write_ppm(stem + ".1x.ppm", rgb.data(), 256, 192, 3);
    write_ppm(stem + "." + std::to_string(scale) + "x.ppm", px.data(), W, H, 4);
  }
  Result r{fnv(px), 0, 0};
  const int m = sc.margin;
  if (sc.tolerance == kNotCompared) return r;
  for (int y = m; y + m < 192; ++y)
    for (int x = m; x + m < 256; ++x) {
      const u32 c = ref1x[y * 256 + x] & 0xFFFFFF;
      bool uniform = true;
      for (int dy = -m; dy <= m && uniform; ++dy)
        for (int dx = -m; dx <= m; ++dx) if ((ref1x[(y + dy) * 256 + x + dx] & 0xFFFFFF) != c) { uniform = false; break; }
      if (!uniform) continue;
      ++r.checked;
      bool same = true;
      for (u32 j = 0; j < scale; ++j)
        for (u32 i = 0; i < scale; ++i) {
          const u8* p = &px[((static_cast<u32>(y) * scale + j) * W + static_cast<u32>(x) * scale + i) * 4];
          for (u32 k = 0; k < 3; ++k) {
            const int d = static_cast<int>(p[k] >> 2) - static_cast<int>((c >> (16 - 8 * k) & 0xFF) >> 2);   // in the DS's 6 bits
            if (d > static_cast<int>(sc.tolerance) || -d > static_cast<int>(sc.tolerance)) same = false;
          }
        }
      if (!same) {
        if (r.mismatched < 4) {
          const u8* p = &px[(static_cast<u32>(y) * scale * W + static_cast<u32>(x) * scale) * 4];
          std::fprintf(stderr, "  %s %ux: (%d,%d) 1x %06x, hi-res %02x%02x%02x\n", sc.name, scale, x, y, c, p[0], p[1], p[2]);
        }
        ++r.mismatched;
      }
    }
  return r;
}

} // namespace

int main(int argc, char** argv) {
  bool print = false;
  const char* dump = nullptr;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--print")) print = true;
    else if (!std::strcmp(argv[i], "--dump") && i + 1 < argc) dump = argv[++i];
  }
  SDL_SetHint(SDL_HINT_VIDEODRIVER, "offscreen");
  if (SDL_Init(SDL_INIT_VIDEO) != 0) { std::printf("gl_hires: skipped (no SDL video: %s)\n", SDL_GetError()); return 77; }
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
  SDL_Window* win = SDL_CreateWindow("gl_hires_test", 0, 0, 64, 64, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
  SDL_GLContext ctx = win ? SDL_GL_CreateContext(win) : nullptr;
  if (!ctx) { std::printf("gl_hires: skipped (no GLES 3.1 context: %s)\n", SDL_GetError()); return 77; }
  const std::string renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
  std::printf("gl_hires: %s\n", renderer.c_str());

  int failures = 0, golden_checked = 0;
  for (const Scene& sc : scenes())
    for (u32 scale : {2u, 4u}) {
      const Result r = run_scene(sc, scale, dump);
      if (print) std::printf("{\"%s\", \"%s\", %u, 0x%016llxull},\n", renderer.c_str(), sc.name, scale, static_cast<unsigned long long>(r.hash));
      // A scene must leave enough uniform area to say something.
      if (sc.tolerance != kNotCompared && (r.checked < 5000 || r.mismatched)) {
        std::fprintf(stderr, "%s %ux: %u of %u uniform pixels differ from 1x\n", sc.name, scale, r.mismatched, r.checked);
        ++failures;
      }
      for (const Golden& gd : kGolden)
        if (renderer == gd.renderer && !std::strcmp(sc.name, gd.scene) && scale == gd.scale) {
          ++golden_checked;
          if (gd.hash != r.hash) {
            std::fprintf(stderr, "%s %ux: picture %016llx, golden %016llx\n", sc.name, scale, static_cast<unsigned long long>(r.hash), static_cast<unsigned long long>(gd.hash));
            ++failures;
          }
        }
    }
  SDL_GL_DeleteContext(ctx);
  SDL_DestroyWindow(win);
  SDL_Quit();
  if (failures) { std::fprintf(stderr, "gl_hires: %d failure(s)\n", failures); return 1; }
  std::printf("gl_hires: ok (%d golden pictures%s)\n", golden_checked, golden_checked ? "" : "; none recorded for this renderer");
  return 0;
}
