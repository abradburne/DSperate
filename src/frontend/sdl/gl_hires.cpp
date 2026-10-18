// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "frontend/sdl/gl_hires.h"
#include "core/gpu/kernels.h"

#if !DSPERATE_GLES
namespace ds::sdl {
struct GlHires::Impl {};
GlHires::GlHires() = default;
GlHires::~GlHires() = default;
bool GlHires::init(u32, std::string* why) { if (why) *why = "built without GLES"; return false; }
unsigned GlHires::render(const gpu::Renderer3D::DisplayList&, const std::vector<u32>&, u32, u64, const gpu::Engine2D::HiresLine*, int) { return 0; }
unsigned GlHires::recomposite(const gpu::Engine2D::HiresLine*, int) { return 0; }
bool GlHires::brightened(const gpu::Engine2D::HiresLine*) { return false; }
} // namespace ds::sdl
#else
#include <GLES3/gl31.h>
#include <SDL2/SDL.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace ds::sdl {

namespace {

bool g_ab_no_shadow = false;   // DS_HIRES_AB's second render

using clk = std::chrono::steady_clock;
double ms(clk::time_point a, clk::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); }

using gpu::vk::GpuPoly;
using gpu::vk::GpuVert;

// One fan vertex, placed as the lean raster places it (vk/shaders/lean.vert).
struct Vtx {
  float pos[4];
  float rgb[3];
  float st[2];
  u32 attr, texparam, tex_offset, tex_wh, flags;
  float dz;                     // DS depth (z, or the W-buffer's normalised w), the depth-equal offset applied
};

constexpr u32 kArenaW = 4096, kArenaH = (2u << 20) / kArenaW;   // Renderer3D's display arena, 8 MB

const char* kRasterVS = R"(#version 310 es
layout(location = 0) in vec4 a_pos;
layout(location = 1) in vec3 a_rgb;
layout(location = 2) in vec2 a_st;
layout(location = 3) in uvec4 a_poly;   // attr, texparam, tex_offset, tex_w | tex_h << 16
layout(location = 4) in uint a_flags;
layout(location = 5) in float a_dz;
out vec3 v_rgb;
out vec2 v_st;
flat out uvec4 v_poly;
flat out uint v_flags;
out float v_zp;    // the DS depth, perspective-correct (W-buffer frames)
out vec2 v_zw;     // (z w, w): their ratio is z linear across the screen (Z-buffer frames; GLES has no noperspective)
void main() { gl_Position = a_pos; v_rgb = a_rgb; v_st = a_st; v_poly = a_poly; v_flags = a_flags; v_zp = a_dz; v_zw = vec2(a_dz * a_pos.w, a_pos.w); }
)";

// ds_shade.glsl's shade_pixel, the texel arena a storage buffer. Variants:
// (none) a polygon drawn as is (the translucent tail); PREPASS the opaque
// prefix's depth test and visibility record; RESOLVE that record shaded, once
// a pixel, full screen. Measured on Golden Sun's title at 2x (DS_HIRES_BENCH):
// shading every overlapping fragment of the prefix (~4x the screen) was the
// bulk of the 3D's GPU time on the one-core G52.
const char* kRasterFS = R"(
precision highp float; precision highp int; precision highp usampler2D;
layout(std430, binding = 0) readonly buffer Texels { uint texels[]; };   // the arena, linear
uniform uint u_dispcnt, u_alpha_ref;
uniform uint u_toon[32];
uniform int u_wbuf;
uniform int u_split;   // a translucent polygon's pixels: 0 all, 1 the opaque ones (alpha 31), 2 the rest
uniform int u_shadow;  // a shadow polygon: it does not draw over its own polygon id
#if !defined(PREPASS) && !defined(RESOLVE)
// The opaque prefix's winner at this pixel (the pre-pass's g0, id + 1 << 18)
// and the polygons' records: a shadow's own-id test without reading the id
// plane the pass is drawing into.
uniform highp usampler2D u_win;
layout(std430, binding = 1) readonly buffer PolysT { uvec4 polys_t[]; };
#endif
#ifdef RESOLVE
layout(std430, binding = 1) readonly buffer Polys { uvec4 polys[]; };   // two a polygon: attr, texparam, tex_offset, w | h << 16; flags
uniform highp usampler2D u_g0, u_g1;   // the pre-pass's: winner id << 18 | vertex colour; texel s, t
#else
in vec3 v_rgb; in vec2 v_st; flat in uvec4 v_poly; flat in uint v_flags;
in float v_zp; in vec2 v_zw;
#endif
#ifdef PREPASS
layout(location = 0) out uint o_g0;
layout(location = 1) out uint o_g1;
#else
out vec4 o_col;
layout(location = 1) out uint o_id;   // edge marking and fog: the polygon id | 0x40 (its fog bit) | 0x80 (an opaque polygon drew here)
#endif
uint texel(uint i) { return texels[i]; }
uint c15_to_18(uint c, uint shift) { uint v = ((shift == 0u ? (c << 1) : (c >> shift)) & 0x3Eu); return v != 0u ? v + 1u : v; }
#ifndef RESOLVE
float ds_depth() {
  // The DS's own depth, rounded to its integer, over 2^24: exact in a float
  // depth buffer, nearer is smaller in both modes, and surfaces at the same
  // depth tie exactly, so LESS (the earlier polygon keeps the pixel) decides
  // as on the DS. (Through GL's -1..1 clip depth the W-buffer's 1/w lost all
  // its precision: neighbouring surfaces fought and parts vanished.)
  float zds = u_wbuf != 0 ? v_zp : v_zw.x / v_zw.y;
  return clamp(floor(zds + 0.5), 0.0, 16777215.0) / 16777216.0;
}
#endif
// The texel at the DS texel coordinate (s, t), wrapped, flipped or clamped.
uint fetch(uint texparam, uint tex_offset, uint wh, int s, int t) {
  int w = int(wh & 0xFFFFu), h = int(wh >> 16);
  bool rep_s = (texparam & 0x10000u) != 0u, rep_t = (texparam & 0x20000u) != 0u;
  bool flp_s = (texparam & 0x40000u) != 0u, flp_t = (texparam & 0x80000u) != 0u;
  if (rep_s) { if (flp_s && (s & w) != 0) s = (w - 1) - (s & (w - 1)); else s &= w - 1; } else s = clamp(s, 0, w - 1);
  if (rep_t) { if (flp_t && (t & h) != 0) t = (h - 1) - (t & (h - 1)); else t &= h - 1; } else t = clamp(t, 0, h - 1);
  return texel(tex_offset + uint(t * w + s));
}
// ds_shade.glsl's shade_pixel: 6-bit r, g, b and 5-bit alpha.
uvec4 shade(uint attr, uint texparam, uint tex_offset, uint wh, uint flags, uint vr, uint vg, uint vb, int s, int t) {
  uint blendmode = (attr >> 4) & 3u, polyalpha = (attr >> 16) & 0x1Fu;
  uint r, g, b, a;
  bool highlight = (u_dispcnt & 2u) != 0u;
  if (blendmode == 2u) {
    if (highlight) { vg = vr; vb = vr; }
    else { uint tc = u_toon[vr >> 1]; vr = c15_to_18(tc, 0u); vg = c15_to_18(tc, 4u); vb = c15_to_18(tc, 9u); }
  }
  if ((flags & 8u) != 0u) {
    uint tpk = fetch(texparam, tex_offset, wh, s, t);
    uint tcol = tpk & 0xFFFFu, ta = (tpk >> 16) & 0x1Fu;
    uint tr = c15_to_18(tcol, 0u), tg = c15_to_18(tcol, 4u), tb = c15_to_18(tcol, 9u);
    if ((blendmode & 1u) != 0u) {
      if (ta == 0u) { r = vr; g = vg; b = vb; }
      else if (ta == 31u) { r = tr; g = tg; b = tb; }
      else { r = ((tr * ta) + (vr * (31u - ta))) >> 5; g = ((tg * ta) + (vg * (31u - ta))) >> 5; b = ((tb * ta) + (vb * (31u - ta))) >> 5; }
      a = polyalpha;
    } else {
      r = ((tr + 1u) * (vr + 1u) - 1u) >> 6; g = ((tg + 1u) * (vg + 1u) - 1u) >> 6; b = ((tb + 1u) * (vb + 1u) - 1u) >> 6;
      a = ((ta + 1u) * (polyalpha + 1u) - 1u) >> 5;
    }
  } else { r = vr; g = vg; b = vb; a = polyalpha; }
  if (blendmode == 2u && highlight) {
    uint tc = u_toon[vr >> 1];
    r = min(r + c15_to_18(tc, 0u), 63u); g = min(g + c15_to_18(tc, 4u), 63u); b = min(b + c15_to_18(tc, 9u), 63u);
  }
  return uvec4(r, g, b, a);
}
void main() {
#ifdef RESOLVE
  // Each pixel the pre-pass's winner covers, shaded once from what it kept.
  ivec2 px = ivec2(gl_FragCoord.xy);
  uint g0 = texelFetch(u_g0, px, 0).r;
  uint id = g0 >> 18;
  if (id == 0u) discard;   // no opaque polygon: the clear stays
  uint g1 = texelFetch(u_g1, px, 0).r;
  uvec4 p = polys[2u * (id - 1u)], q = polys[2u * (id - 1u) + 1u];
  uint attr = p.x;
  uvec4 c = shade(attr, p.y, p.z, p.w, q.x, (g0 >> 12) & 0x3Fu, (g0 >> 6) & 0x3Fu, g0 & 0x3Fu, int(g1 << 16) >> 16, int(g1) >> 16);
  uint r = c.x, g = c.y, b = c.z, a = c.w;
#else
  uint attr = v_poly.x;
  int s = int(floor(v_st.x + 0.01)) >> 4, t = int(floor(v_st.y + 0.01)) >> 4;
#ifdef PREPASS
  // Only what the alpha test and the depth need: a texture's alpha only
  // where the texture has one (else the alpha is the polygon's). The
  // winner's id, vertex colour and texel coordinate are kept for the resolve.
  uint polyalpha = (attr >> 16) & 0x1Fu, a = polyalpha;
  if ((v_flags & 0x48u) == 0x48u && ((attr >> 4) & 1u) == 0u) {
    uint ta = (fetch(v_poly.y, v_poly.z, v_poly.w, s, t) >> 16) & 0x1Fu;
    a = ((ta + 1u) * (polyalpha + 1u) - 1u) >> 5;
  }
  if (a <= u_alpha_ref) discard;
  vec3 rgb = clamp(v_rgb, vec3(0.0), vec3(511.0));
  uint vr = uint(int(round(rgb.r)) >> 3) & 0x3Fu, vg = uint(int(round(rgb.g)) >> 3) & 0x3Fu, vb = uint(int(round(rgb.b)) >> 3) & 0x3Fu;
  o_g0 = ((v_flags >> 12) << 18) | (vr << 12) | (vg << 6) | vb;
  o_g1 = (uint(s) & 0xFFFFu) | (uint(t) << 16);
  gl_FragDepth = ds_depth();
  return;
#else
  vec3 rgb = clamp(v_rgb, vec3(0.0), vec3(511.0));
  uint vr = uint(int(round(rgb.r)) >> 3) & 0x3Fu, vg = uint(int(round(rgb.g)) >> 3) & 0x3Fu, vb = uint(int(round(rgb.b)) >> 3) & 0x3Fu;
  uvec4 c = shade(attr, v_poly.y, v_poly.z, v_poly.w, v_flags, vr, vg, vb, s, t);
  uint r = c.x, g = c.y, b = c.z, a = c.w;
  if (a <= u_alpha_ref) discard;
  if (u_split != 0 && ((a == 31u) != (u_split == 1))) discard;
  if (u_shadow != 0) {
    uint w = texelFetch(u_win, ivec2(gl_FragCoord.xy), 0).r >> 18;
    if (w != 0u && ((polys_t[(w - 1u) * 2u].x >> 24) & 0x3Fu) == ((attr >> 24) & 0x3Fu)) discard;
  }
#endif
#endif
#ifndef PREPASS
  // Raw 6-bit channels; alpha (a + 1) * 8 so the blend's SRC_ALPHA is (a + 1) / 32, as the lean raster.
  uint ab = a == 0u ? 0u : min(255u, (a + 1u) * 8u);
  o_col = vec4(float(r), float(g), float(b), float(ab)) / 255.0;
  o_id = ((attr >> 24) & 0x3Fu) | ((attr >> 9) & 0x40u) | 0x80u;
#if !defined(RESOLVE)
  gl_FragDepth = ds_depth();
#endif
#endif
}
)";

const char* kFullVS = R"(#version 310 es
out vec2 v_uv;
void main() {
  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
  v_uv = p;
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

// Edge marking (DISP3DCNT bit 5) at S x: a pixel an opaque polygon drew takes
// its id's edge colour where a neighbour one DS pixel away (S hi-res pixels,
// so the line is as wide as the DS's) has another id and lies further back.
// Edge marking, then fog, as the DS's final pass does them (render3d.cpp's
// final_pass_ref), on the 3D layer at S x: the id plane says which polygon
// (or the rear plane) is on top and whether it carries the fog bit, the depth
// plane its DS depth. A translucent polygon leaves the id plane as it was, so
// its pixels keep the fog bit of what is under them (the DS ANDs its own in).
// Done where the composite reads the 3D layer (layer3d), not as a pass of its own,
// in a build of the composite of its own (EDGE_FOG): compiled in, the code
// slowed the composite of frames without either by 0.6 ms (NSMB at 2x).
#define LAYER_3D R"(
precision highp usampler2D;
uniform highp sampler2D u_3d, u_depth;
uniform highp usampler2D u_id;
uniform uint u_edge[8];
uniform int u_edge_on, u_fog_on, u_fog_alpha;   // DISP3DCNT bit 5; bit 7; bit 6 (fog the alpha only)
uniform uint u_fog_color, u_fog_offset, u_fog_shift;
uniform uint u_density[34];
uint c15_to_18(uint c, uint shift) { uint v = ((shift == 0u ? (c << 1) : (c >> shift)) & 0x3Eu); return v != 0u ? v + 1u : v; }
vec4 layer3d(ivec2 p) {
  vec4 c = texelFetch(u_3d, p, 0);
#ifdef EDGE_FOG
  uint id = texelFetch(u_id, p, 0).r;
  float z = texelFetch(u_depth, p, 0).r;
  if (u_edge_on != 0 && (id & 0x80u) != 0u) {
    ivec2 hi = textureSize(u_3d, 0) - 1;
    bool edge = false;
    for (int k = 1; k <= SCALE && !edge; ++k) {
      ivec2 o[4] = ivec2[4](ivec2(k, 0), ivec2(-k, 0), ivec2(0, k), ivec2(0, -k));
      for (int i = 0; i < 4; ++i) {
        ivec2 q = clamp(p + o[i], ivec2(0), hi);
        uint nid = texelFetch(u_id, q, 0).r;
        float nz = texelFetch(u_depth, q, 0).r;
        bool further = nz > z;   // the DS's depth in both modes: larger is further
        if ((nid & 0x3Fu) != (id & 0x3Fu) && further) { edge = true; break; }
      }
    }
    if (edge) {
      uint e = u_edge[(id & 0x3Fu) >> 3];
      c.rgb = vec3(float(c15_to_18(e, 0u)), float(c15_to_18(e, 4u)), float(c15_to_18(e, 9u))) / 255.0;
    }
  }
  if (u_fog_on != 0 && (id & 0x40u) != 0u) {
    // Renderer3D::fog_density: (Z - offset) >> 2 << shift, bits 0-16 the
    // fraction between two of the 33 table entries, 17 up the entry.
    uint zi = uint(z * 16777216.0 + 0.5);
    uint e = 0u, frac = 0u;
    if (zi >= u_fog_offset) {
      uint zz = ((zi - u_fog_offset) >> 2) << u_fog_shift;
      e = zz >> 17;
      if (e >= 32u) { e = 32u; frac = 0u; } else frac = zz & 0x1FFFFu;
    }
    uint d = (u_density[e] * (0x20000u - frac) + u_density[e + 1u] * frac) >> 17;
    if (d >= 127u) d = 128u;
    // The colour plane holds raw 6-bit channels and the alpha as (a + 1) * 8.
    uvec3 rgb = uvec3(round(c.rgb * 255.0));
    uint ab = uint(round(c.a * 255.0)), a = ab == 0u ? 0u : (ab >> 3) - 1u;
    if (u_fog_alpha == 0) {
      uvec3 f = uvec3(c15_to_18(u_fog_color, 0u), c15_to_18(u_fog_color, 4u), c15_to_18(u_fog_color, 9u));
      rgb = (f * d + rgb * (128u - d)) >> 7;
    }
    a = (((u_fog_color >> 16) & 0x1Fu) * d + a * (128u - d)) >> 7;
    ab = a == 0u ? 0u : min(255u, (a + 1u) * 8u);
    c = vec4(vec3(rgb), float(ab)) / 255.0;
  }
#endif
  return c;
}
)"

// Engine A's kept lines (Engine2D::HiresLine) in a buffer, copied as they lie
// in memory, LINE_WORDS words a line: the out, under, top and second records
// (6-bit RGB; words 0, 256, 512, 768), then the top id, top kind, top alpha,
// second id, window and class bytes (planes 0-5 from word 1024), then the
// line's registers (word 1408: BLDCNT | MASTER_BRIGHT << 16; 1409: EVA, EVB,
// EVY and the valid flag, a byte each). Compiled for one scale (SCALE): a
// division by a uniform was half the composite's GPU time on the G52.
#define LINE_PLANES R"(
precision highp float; precision highp int; precision highp sampler2D;
layout(std430, binding = 2) readonly buffer Lines { uint lw[]; };
uint px(ivec2 d, int plane) { return lw[d.y * LINE_WORDS + plane * 256 + d.x]; }   // 0 out, 1 under, 2 top, 3 second
uint byte_of(ivec2 d, int plane) { return (lw[d.y * LINE_WORDS + 1024 + plane * 64 + (d.x >> 2)] >> ((d.x & 3) * 8)) & 0xFFu; }   // 0 top id ... 5 class
uniform int u_mode;   // 0 shown, 1 captured graphics (no master brightness)
out vec4 o_col;
uint up(uint c, uint f, uint bias) { return c + ((((63u - c) * f) + bias) >> 4); }
uint dn(uint c, uint f, uint bias) { return c - (((c * f) + bias) >> 4); }
uint brighten(uint v, uint f, uint bias) { return up(v & 0x3Fu, f, bias) | (up((v >> 8) & 0x3Fu, f, bias) << 8) | (up((v >> 16) & 0x3Fu, f, bias) << 16); }
uint darken(uint v, uint f, uint bias) { return dn(v & 0x3Fu, f, bias) | (dn((v >> 8) & 0x3Fu, f, bias) << 8) | (dn((v >> 16) & 0x3Fu, f, bias) << 16); }
// Master brightness (kernels_ref.cpp output_line), then 6 -> 8 bits.
vec4 finish(uint o, uint mb) {
  uint mode = mb >> 14, f = min(mb & 0x1Fu, 16u);
  if (mode == 1u) o = brighten(o, f, 0u);
  else if (mode == 2u) o = darken(o, f, 0xFu);
  uvec3 c6 = uvec3(o & 0x3Fu, (o >> 8) & 0x3Fu, (o >> 16) & 0x3Fu);
  return vec4(vec3((c6 << 2) | (c6 >> 4)) / 255.0, 1.0);
}
)"

// Pass 1 of the composite, every S x pixel, from the pixel's class: 0 = the
// 3D layer plays no part, so engine A's own 1x pixel is exact; 1 = the 3D
// layer on top with nothing applied after it, so the hi-res sample is the
// pixel (where it covers; elsewhere what it covers). Master brightness
// reaches every pixel of a shown line, so there a class 1 pixel takes the
// full composite too (a capture is taken before it). The full composite is
// left (discarded) for pass 2.
const char* kFastFS = LINE_PLANES LAYER_3D R"(
void main() {
  ivec2 p = ivec2(gl_FragCoord.xy), d = p / SCALE;
  uint mb = u_mode == 1 ? 0u : lw[d.y * LINE_WORDS + 1408] >> 16;
  bool bright = ((mb >> 14) == 1u || (mb >> 14) == 2u) && (mb & 0x1Fu) != 0u;
  uint cls = (lw[d.y * LINE_WORDS + 1409] & 0xFF000000u) != 0u ? byte_of(d, 5) : 0u;
  if (cls == 2u || (cls == 1u && bright)) discard;
  if (cls == 1u) {
    vec4 h = layer3d(p);
    if (h.a > 0.0) { uvec3 c6 = uvec3(round(h.rgb * 255.0)); o_col = vec4(vec3((c6 << 2) | (c6 >> 4)) / 255.0, 1.0); return; }
    o_col = finish(px(d, 1), 0u);
    return;
  }
  o_col = finish(px(d, 0), mb);
}
)";

// Engine A's colour effects and master brightness (kernels_ref.cpp
// composite_line / output_line) per S x pixel, with the 3D layer's entries
// taken from the hi-res frame.
const char* kCompositeFS = LINE_PLANES LAYER_3D R"(
uint blend(uint a, uint b, uint eva, uint evb) {
  uint r = min((((a & 0x3Fu) * eva) + ((b & 0x3Fu) * evb) + 8u) >> 4, 0x3Fu);
  uint g = min(((((a >> 8) & 0x3Fu) * eva) + (((b >> 8) & 0x3Fu) * evb) + 8u) >> 4, 0x3Fu);
  uint bl = min(((((a >> 16) & 0x3Fu) * eva) + (((b >> 16) & 0x3Fu) * evb) + 8u) >> 4, 0x3Fu);
  return r | (g << 8) | (bl << 16);
}
uint blend_3d(uint a, uint b) {
  uint eva = ((a >> 24) & 0x1Fu) + 1u, evb = 32u - eva;
  if (eva == 32u) return a & 0xFFFFFFu;
  uint r = min((((a & 0x3Fu) * eva) + ((b & 0x3Fu) * evb) + 16u) >> 5, 0x3Fu);
  uint g = min(((((a >> 8) & 0x3Fu) * eva) + (((b >> 8) & 0x3Fu) * evb) + 16u) >> 5, 0x3Fu);
  uint bl = min(((((a >> 16) & 0x3Fu) * eva) + (((b >> 16) & 0x3Fu) * evb) + 16u) >> 5, 0x3Fu);
  return r | (g << 8) | (bl << 16);
}
void main() {
  ivec2 p = ivec2(gl_FragCoord.xy), d = p / SCALE;
  uint r0 = lw[d.y * LINE_WORDS + 1408], r1 = lw[d.y * LINE_WORDS + 1409];
  uint top = px(d, 2), second = px(d, 3);
  uint top_id = byte_of(d, 0), kind = byte_of(d, 1), talpha = byte_of(d, 2), second_id = byte_of(d, 3), win = byte_of(d, 4);
  vec4 h = layer3d(p);
  uint ha8 = uint(round(h.a * 255.0));
  uint ha = ha8 == 0u ? 0u : (ha8 >> 3) - 1u;
  uint hpx = uint(round(h.r * 255.0)) | (uint(round(h.g * 255.0)) << 8) | (uint(round(h.b * 255.0)) << 16) | (ha << 24);
  if (kind == 3u) {
    if (ha != 0u) { top = hpx; talpha = ha; }
    else { top = second; top_id = second_id; kind = 0u; talpha = 0u; }   // past the polygon's hi-res edge: what lay beneath
  } else if (second_id == 1u && ha != 0u) second = hpx;
  uint bldcnt = r0 & 0xFFFFu, eva = r1 & 0x1Fu, evb = (r1 >> 8) & 0x1Fu, evy = (r1 >> 16) & 0x1Fu;
  uint effect = (bldcnt >> 6) & 3u;
  uint t2 = second_id << 8;
  uint o = top & 0xFFFFFFu;
  if ((kind == 1u || kind == 2u) && (bldcnt & t2) != 0u) {
    uint ea = kind == 2u ? talpha : eva, eb = kind == 2u ? 16u - ea : evb;
    o = blend(top, second, ea, eb);
  } else if (kind == 3u && (bldcnt & t2) != 0u) {
    o = blend_3d(top, second);
  } else if ((bldcnt & top_id) != 0u && (win & 0x20u) != 0u) {
    if (effect == 1u) { if ((bldcnt & t2) != 0u) o = blend(top, second, eva, evb); }
    else if (effect == 2u) o = brighten(top, evy, 8u);
    else if (effect == 3u) o = darken(top, evy, 7u);
  }
  o_col = finish(o, u_mode == 1 ? 0u : r0 >> 16);
}
)";

GLuint compile(GLenum type, const char* src, std::string* why) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, nullptr);
  glCompileShader(s);
  GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) { char log[2048]; glGetShaderInfoLog(s, sizeof log, nullptr, log); if (why) *why = log; glDeleteShader(s); return 0; }
  return s;
}

GLuint program(const char* vs, const char* fs, std::string* why) {
  const GLuint v = compile(GL_VERTEX_SHADER, vs, why), f = compile(GL_FRAGMENT_SHADER, fs, why);
  if (!v || !f) return 0;
  GLuint p = glCreateProgram();
  glAttachShader(p, v); glAttachShader(p, f); glLinkProgram(p);
  glDeleteShader(v); glDeleteShader(f);
  GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
  if (!ok) { char log[2048]; glGetProgramInfoLog(p, sizeof log, nullptr, log); if (why) *why = log; glDeleteProgram(p); return 0; }
  return p;
}

GLuint tex2d(GLenum fmt, GLsizei w, GLsizei h) {
  GLuint t; glGenTextures(1, &t); glBindTexture(GL_TEXTURE_2D, t);
  glTexStorage2D(GL_TEXTURE_2D, 1, fmt, w, h);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  return t;
}

} // namespace

struct GlHires::Impl {
  u32 S = 2;
  GLuint raster = 0, comp[2] = {}, fast[2] = {};   // [1]: with edge marking and fog (EDGE_FOG)
  // The opaque prefix as a visibility buffer: `pre` runs the DS depth test
  // (the depth from the shader, so nothing is rejected early) with a cheap
  // shader and keeps each pixel's winner, vertex colour and texel coordinate
  // (g0, g1); `resolve` then shades each pixel once, full screen.
  GLuint pre = 0, resolve = 0, g0 = 0, g1 = 0, fbo_pre = 0;
  GLint p_alpha_ref = -1, p_wbuf = -1, w_dispcnt = -1, w_toon = -1;
  GLuint polys[3] = {}; int polys_k = 0;   // the polygons' records for the resolve, a ring
  std::vector<u32> precs;
  GLuint dep_out = 0;
  GLint r_wbuf = -1;
  GLint u_dispcnt = -1, u_alpha_ref = -1, u_toon = -1, u_mode[2] = {-1, -1}, f_mode[2] = {-1, -1}, r_split = -1, r_shadow = -1;
  u32 nruns = 0;   // DS_HIRES_DEBUG: the last render's tail runs
  // glColorMaski (OES/EXT_draw_buffers_indexed): the id plane's writes switched
  // on and off without changing the draw buffers, which on a tiler ends the
  // batch (a store and a reload of every attachment).
  using ColorMaski = void (*)(GLuint, GLboolean, GLboolean, GLboolean, GLboolean);
  ColorMaski color_maski = nullptr;
  GLuint vao = 0, vbo = 0, vao_empty = 0;
  GLuint arena = 0;
  u32 arena_up = 0; u64 arena_epoch = ~0ull;   // words of the arena on the GPU, and of which generation
  GLuint fbo3d = 0, col3d = 0, dep3d = 0, id3d = 0;
  // The composite programs' edge marking and fog (LAYER_3D), and what the
  // last draw3d left for them.
  struct LayerLocs { GLint edge = -1, edge_on = -1, fog_on = -1, fog_alpha = -1, fog_color = -1, fog_offset = -1, fog_shift = -1, density = -1; };
  LayerLocs fast_l, comp_l;   // EDGE_FOG's
  gpu::vk::GpuPost post{};
  bool edge_on = false, fog_on = false, fog_alpha = false;
  static constexpr int kOut = 6;   // a ring: the last frames' results (shown, and captures shown a frame or two later) stay valid
  GLuint fbo_out[kOut] = {}, out[kOut] = {};
  int slot = 0;
  std::vector<Vtx> vb;
  // The per-frame inputs (planes, classes, vertices) in a ring of sets, one
  // per render: rewriting the one the GPU still reads made Panfrost shadow
  // it (a new buffer, mapped and faulted, every upload), ~3 ms a frame of
  // kernel time on the present thread. t_*, vao and vbo name this render's.
  static constexpr int kSets = 3;
  // lines: engine A's kept lines for a composite, written through an
  // unsynchronised map once `fence` (the last composite that read it) is done.
  struct Set { GLuint lines = 0, vao = 0, vbo = 0; GLsync fence = nullptr; size_t vbo_cap = 0; };
  Set sets[kSets];
  int set_k = 0, vb_k = 0;   // the planes' set (a composite's), the vertices' (a 3D draw's)
  GLuint layer3d = 0;        // the last draw3d's 3D layer for the composite, 0 when none
  // DS_HIRES_DEBUG: CPU time per section (arena, fans, vbo, raster calls, planes, composite calls).
  const bool timing = std::getenv("DS_HIRES_DEBUG") != nullptr;
  double t[6] = {}; u32 tn = 0;
  // DS_HIRES_STAGES: GPU time per section (EXT_disjoint_timer_query) -- pre, winners, tail,
  // edge, composite, planes upload -- in a ring of four renders, read three renders late.
  // A composite again in the same render overwrites its sections' queries.
  const bool stg = std::getenv("DS_HIRES_STAGES") != nullptr;
  GLuint tq[4][6] = {}; u32 tq_n = 0; double sg[6] = {}; u32 sgn = 0; int cur_q = -1;
  void stages_next() {   // a new render: collect the one three back
    if (!stg) return;
    using GetQ = void (*)(GLuint, GLenum, GLuint64*);
    static const GetQ getq = reinterpret_cast<GetQ>(SDL_GL_GetProcAddress("glGetQueryObjectui64vEXT"));
    if (!tq[0][0]) for (auto& r : tq) glGenQueries(6, r);
    ++tq_n;
    if (tq_n > 3 && getq) {
      const u32 old = (tq_n - 3) % 4;
      for (int k = 0; k < 6; ++k) { GLuint64 ns = 0; getq(tq[old][k], 0x8866 /* GL_QUERY_RESULT */, &ns); sg[k] += ns / 1e6; }
      if (++sgn == 120) {
        std::fprintf(stderr, "gpu ms: pre %.2f, winners %.2f, tail %.2f, edge %.2f, composite %.2f, planes upload %.2f\n", sg[0] / sgn, sg[1] / sgn, sg[2] / sgn, sg[3] / sgn, sg[4] / sgn, sg[5] / sgn);
        for (double& x : sg) x = 0;
        sgn = 0;
      }
    }
  }
  void stage(int k) {   // ends the running section, starts section k (-1: none)
    if (!stg) return;
    if (cur_q >= 0) glEndQuery(0x88BF /* GL_TIME_ELAPSED_EXT */);
    cur_q = k;
    if (k >= 0) glBeginQuery(0x88BF, tq[tq_n % 4][k]);
  }

  ~Impl() {
    for (Set& st : sets) {
      if (st.lines) glDeleteBuffers(1, &st.lines);
      if (st.fence) glDeleteSync(st.fence);
      if (st.vbo) glDeleteBuffers(1, &st.vbo);
      if (st.vao) glDeleteVertexArrays(1, &st.vao);
    }
    glDeleteTextures(kOut, out);
    GLuint texs[] = {col3d};
    if (arena) glDeleteBuffers(1, &arena);
    for (GLuint t : texs) if (t) glDeleteTextures(1, &t);
    if (pre) glDeleteProgram(pre);
    if (resolve) glDeleteProgram(resolve);
    if (g0) glDeleteTextures(1, &g0);
    if (g1) glDeleteTextures(1, &g1);
    glDeleteBuffers(3, polys);
    if (fbo_pre) glDeleteFramebuffers(1, &fbo_pre);
    if (dep3d) glDeleteTextures(1, &dep3d);
    if (id3d) glDeleteTextures(1, &id3d);
    if (fbo3d) glDeleteFramebuffers(1, &fbo3d);
    glDeleteFramebuffers(kOut, fbo_out);
    if (vao_empty) glDeleteVertexArrays(1, &vao_empty);
    if (raster) glDeleteProgram(raster);
    for (GLuint pr : {comp[0], comp[1], fast[0], fast[1]}) if (pr) glDeleteProgram(pr);
    if (dep_out) glDeleteRenderbuffers(1, &dep_out);
  }
};

GlHires::GlHires() = default;
GlHires::~GlHires() = default;

bool GlHires::init(u32 scale, std::string* why) {
  d_ = std::make_unique<Impl>();
  Impl& d = *d_;
  d.S = scale_ = std::clamp(scale, 2u, 4u);
  const GLsizei W = static_cast<GLsizei>(256 * d.S), H = static_cast<GLsizei>(192 * d.S);
  const std::string fs_late = std::string("#version 310 es\n") + kRasterFS;
  d.raster = program(kRasterVS, fs_late.c_str(), why);
  const std::string fs_pre = std::string("#version 310 es\n#define PREPASS 1\n") + kRasterFS;
  const std::string fs_res = std::string("#version 310 es\n#define RESOLVE 1\n") + kRasterFS;
  d.pre = program(kRasterVS, fs_pre.c_str(), why);
  d.resolve = program(kFullVS, fs_res.c_str(), why);
  if (!d.pre || !d.resolve) return false;
  d.p_alpha_ref = glGetUniformLocation(d.pre, "u_alpha_ref"); d.p_wbuf = glGetUniformLocation(d.pre, "u_wbuf");
  glUseProgram(d.resolve);
  glUniform1i(glGetUniformLocation(d.resolve, "u_g0"), 1);
  glUniform1i(glGetUniformLocation(d.resolve, "u_g1"), 2);
  d.w_dispcnt = glGetUniformLocation(d.resolve, "u_dispcnt"); d.w_toon = glGetUniformLocation(d.resolve, "u_toon");
  glGenBuffers(3, d.polys);
  for (GLuint b : d.polys) { glBindBuffer(GL_SHADER_STORAGE_BUFFER, b); glBufferData(GL_SHADER_STORAGE_BUFFER, DS_MAX_POLYS * 32, nullptr, GL_STREAM_DRAW); }
  const std::string scale_def = "#version 310 es\n#define SCALE " + std::to_string(d.S) + "\n#define LINE_WORDS " + std::to_string(sizeof(gpu::Engine2D::HiresLine) / 4) + "\n";
  for (int v = 0; v < 2; ++v) {
    const std::string defs = scale_def + (v ? "#define EDGE_FOG 1\n" : "");
    d.comp[v] = program(kFullVS, (defs + kCompositeFS).c_str(), why);
    d.fast[v] = program(kFullVS, (defs + kFastFS).c_str(), why);
    if (!d.comp[v] || !d.fast[v]) return false;
    d.f_mode[v] = glGetUniformLocation(d.fast[v], "u_mode");
    d.u_mode[v] = glGetUniformLocation(d.comp[v], "u_mode");
  }
  if (!d.raster) return false;
  // The composite's two passes read the same planes, on the same units.
  for (GLuint pr : {d.fast[0], d.comp[0], d.fast[1], d.comp[1]}) {
    glUseProgram(pr);
    glUniform1i(glGetUniformLocation(pr, "u_3d"), 0);
    glUniform1i(glGetUniformLocation(pr, "u_id"), 1);
    glUniform1i(glGetUniformLocation(pr, "u_depth"), 2);
    if (pr != d.fast[1] && pr != d.comp[1]) continue;
    Impl::LayerLocs& l = pr == d.fast[1] ? d.fast_l : d.comp_l;
    l.edge = glGetUniformLocation(pr, "u_edge"); l.edge_on = glGetUniformLocation(pr, "u_edge_on"); l.fog_on = glGetUniformLocation(pr, "u_fog_on");
    l.fog_alpha = glGetUniformLocation(pr, "u_fog_alpha"); l.fog_color = glGetUniformLocation(pr, "u_fog_color"); l.fog_offset = glGetUniformLocation(pr, "u_fog_offset");
    l.fog_shift = glGetUniformLocation(pr, "u_fog_shift"); l.density = glGetUniformLocation(pr, "u_density");
  }
  d.u_dispcnt = glGetUniformLocation(d.raster, "u_dispcnt");
  d.r_wbuf = glGetUniformLocation(d.raster, "u_wbuf");
  d.r_split = glGetUniformLocation(d.raster, "u_split");
  d.r_shadow = glGetUniformLocation(d.raster, "u_shadow");
  d.u_alpha_ref = glGetUniformLocation(d.raster, "u_alpha_ref");
  d.u_toon = glGetUniformLocation(d.raster, "u_toon");
  glUseProgram(d.raster);
  glUniform1i(glGetUniformLocation(d.raster, "u_win"), 3);
  {
    const char* ext = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    if (ext && (std::strstr(ext, "GL_OES_draw_buffers_indexed") || std::strstr(ext, "GL_EXT_draw_buffers_indexed"))) {
      d.color_maski = reinterpret_cast<Impl::ColorMaski>(SDL_GL_GetProcAddress("glColorMaskiOES"));
      if (!d.color_maski) d.color_maski = reinterpret_cast<Impl::ColorMaski>(SDL_GL_GetProcAddress("glColorMaskiEXT"));
    }
  }
  glUniform1i(d.r_shadow, 0);

  glGenBuffers(1, &d.arena); glBindBuffer(GL_SHADER_STORAGE_BUFFER, d.arena);
  glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(kArenaW) * kArenaH * 4, nullptr, GL_STATIC_DRAW);
  d.col3d = tex2d(GL_RGBA8, W, H);
  // Float depth, as the lean raster's: W-buffer frames store 1/w, and at DS depths of tens of
  // thousands neighbouring values are closer than a 24-bit fixed step, so a surface and its
  // decals compared equal (NSMB's world map lost its paths).
  d.dep3d = tex2d(GL_DEPTH32F_STENCIL8, W, H);   // a texture: edge marking reads it
  d.id3d = tex2d(GL_R8UI, W, H);
  glGenFramebuffers(1, &d.fbo3d); glBindFramebuffer(GL_FRAMEBUFFER, d.fbo3d);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, d.col3d, 0);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, d.id3d, 0);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, d.dep3d, 0);
  { const GLenum bufs[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1}; glDrawBuffers(2, bufs); }
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { if (why) *why = "3D framebuffer incomplete"; return false; }
  d.g0 = tex2d(GL_R32UI, W, H); d.g1 = tex2d(GL_R32UI, W, H);
  glGenFramebuffers(1, &d.fbo_pre); glBindFramebuffer(GL_FRAMEBUFFER, d.fbo_pre);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, d.g0, 0);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, d.g1, 0);
  { const GLenum bufs[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1}; glDrawBuffers(2, bufs); }
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, d.dep3d, 0);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { if (why) *why = "pre-pass framebuffer incomplete"; return false; }
  // The composite's depth: pass 1 marks what it drew, so pass 2 shades only the rest.
  glGenRenderbuffers(1, &d.dep_out); glBindRenderbuffer(GL_RENDERBUFFER, d.dep_out); glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, W, H);
  for (int i = 0; i < Impl::kOut; ++i) {
    d.out[i] = tex2d(GL_RGBA8, W, H);
    glGenFramebuffers(1, &d.fbo_out[i]); glBindFramebuffer(GL_FRAMEBUFFER, d.fbo_out[i]);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, d.out[i], 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, d.dep_out);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { if (why) *why = "output framebuffer incomplete"; return false; }
  }
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  for (Impl::Set& st : d.sets) {
    glGenBuffers(1, &st.lines); glBindBuffer(GL_SHADER_STORAGE_BUFFER, st.lines);
    glBufferData(GL_SHADER_STORAGE_BUFFER, 192 * sizeof(gpu::Engine2D::HiresLine), nullptr, GL_DYNAMIC_DRAW);
  }

  glGenVertexArrays(1, &d.vao_empty);
  for (Impl::Set& st : d.sets) {
  glGenVertexArrays(1, &st.vao); glBindVertexArray(st.vao);
  glGenBuffers(1, &st.vbo); glBindBuffer(GL_ARRAY_BUFFER, st.vbo);
  glEnableVertexAttribArray(0); glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(Vtx), reinterpret_cast<void*>(offsetof(Vtx, pos)));
  glEnableVertexAttribArray(1); glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vtx), reinterpret_cast<void*>(offsetof(Vtx, rgb)));
  glEnableVertexAttribArray(2); glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vtx), reinterpret_cast<void*>(offsetof(Vtx, st)));
  glEnableVertexAttribArray(3); glVertexAttribIPointer(3, 4, GL_UNSIGNED_INT, sizeof(Vtx), reinterpret_cast<void*>(offsetof(Vtx, attr)));
  glEnableVertexAttribArray(4); glVertexAttribIPointer(4, 1, GL_UNSIGNED_INT, sizeof(Vtx), reinterpret_cast<void*>(offsetof(Vtx, flags)));
  glEnableVertexAttribArray(5); glVertexAttribPointer(5, 1, GL_FLOAT, GL_FALSE, sizeof(Vtx), reinterpret_cast<void*>(offsetof(Vtx, dz)));
  }
  glBindVertexArray(0);
  if (glGetError() != GL_NO_ERROR) { if (why) *why = "GL error during setup"; return false; }
  return true;
}

unsigned GlHires::render(const gpu::Renderer3D::DisplayList& dl, const std::vector<u32>& arena, u32 arena_top, u64 arena_epoch,
                         const gpu::Engine2D::HiresLine* lines, int mode) {
  // DS_HIRES_BENCH=N[:skip,skip,...]: at render N, draw that frame again 60 times
  // for each skip mask (DS_HIRES_SKIP bits), glFinish'd, and print the cost (debug).
  if (static const char* b = std::getenv("DS_HIRES_BENCH"); b && d_) {
    static u32 n = 0; static bool in = false;
    if (!in && ++n == static_cast<u32>(std::atoi(b))) {
      in = true;
      const char* l = std::strchr(b, ':');
      std::vector<int> masks{0};
      if (l) { masks.clear(); for (const char* q = l + 1; *q; ) { masks.push_back(std::atoi(q)); while (*q && *q != ',') ++q; if (*q) ++q; } }
      for (int m : masks) {
        bench_skip_ = m;
        render(dl, arena, arena_top, arena_epoch, lines, mode); glFinish();
        const auto t0 = std::chrono::steady_clock::now();
        for (int k = 0; k < 60; ++k) render(dl, arena, arena_top, arena_epoch, lines, mode);
        glFinish();
        std::fprintf(stderr, "bench skip %d: %.2f ms a render\n", m, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 60);
      }
      bench_skip_ = 0; in = false;
    }
  }
  if (!d_ || !dl.valid || dl.epoch != arena_epoch || !lines) {
    if (std::getenv("DS_HIRES_DEBUG")) {
      static u32 nv = 0, ne = 0; nv += !dl.valid; ne += dl.valid && dl.epoch != arena_epoch;
      std::fprintf(stderr, "hires3d: frame drawn at 1x (%u without a list, %u stale arena)\n", nv, ne);
    }
    return 0;
  }
  if (!draw3d(dl, arena, arena_top, arena_epoch)) return 0;
  const unsigned tex = composite(lines, mode);
  Impl& d = *d_;
  const GLsizei W = static_cast<GLsizei>(256 * d.S), H = static_cast<GLsizei>(192 * d.S);
  glBindFramebuffer(GL_FRAMEBUFFER, d.fbo_out[d.slot]);
  // DS_HIRES_PPM=<n>:<path>: the n-th composited frame, read back once (debug).
  static u32 renders = 0;
  ++renders;
  // DS_HIRES_PPM=<n>[+<count>]:<path>: frames n..n+count-1, the path suffixed .<frame> after the first.
  static const char* dump = std::getenv("DS_HIRES_PPM");
  static const u32 dump_n = dump ? static_cast<u32>(std::atoi(dump)) : 0, dump_k = dump && std::strchr(dump, '+') && std::strchr(dump, '+') < std::strchr(dump, ':') ? static_cast<u32>(std::atoi(std::strchr(dump, '+') + 1)) : 1;
  if (dump && std::strchr(dump, ':') && renders >= dump_n && renders < dump_n + dump_k) {
    std::vector<u8> px(static_cast<size_t>(W) * H * 4);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    const std::string path = std::string(std::strchr(dump, ':') + 1) + (renders == dump_n ? "" : "." + std::to_string(renders));
    if (std::FILE* o = std::fopen(path.c_str(), "wb")) {
      std::fprintf(o, "P6\n%d %d\n255\n", W, H);
      for (size_t i = 0; i < static_cast<size_t>(W) * H; ++i) std::fwrite(&px[i * 4], 1, 3, o);
      std::fclose(o);
      std::fprintf(stderr, "hires3d: frame %u written to %s\n", renders, path.c_str());
    }
  }
  // DS_HIRES_AB=<n>:<prefix> (debug): render n three ways, for one frame side
  // by side: <prefix>.hi.ppm as drawn, <prefix>.noshadow.ppm the same list
  // drawn again without shadow volumes, <prefix>.1x.ppm the CPU's own 1x
  // engine A line output (the reference: its raster has fog and shadows).
  static const char* ab = std::getenv("DS_HIRES_AB");
  static bool in_ab = false;
  if (ab && !in_ab && lines && std::strchr(ab, ':') && renders == static_cast<u32>(std::atoi(ab))) {
    const std::string pre = std::strchr(ab, ':') + 1;
    auto save = [&](const std::string& path) {
      std::vector<u8> px(static_cast<size_t>(W) * H * 4);
      glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
      if (std::FILE* o = std::fopen(path.c_str(), "wb")) {
        std::fprintf(o, "P6\n%d %d\n255\n", W, H);
        for (size_t i = 0; i < static_cast<size_t>(W) * H; ++i) std::fwrite(&px[i * 4], 1, 3, o);
        std::fclose(o);
      }
    };
    save(pre + ".hi.ppm");
    if (std::FILE* o = std::fopen((pre + ".1x.ppm").c_str(), "wb")) {
      std::fprintf(o, "P6\n256 192\n255\n");
      u16 v15[256]; u32 rgb[256];
      for (u32 y = 0; y < 192; ++y) {
        gpu::kern::active::capture_a15(lines[y].out, 256, v15);
        gpu::kern::active::output_vram_line(v15, 0, rgb);
        for (u32 x = 0; x < 256; ++x) { const u8 c[3] = {static_cast<u8>(rgb[x] >> 16), static_cast<u8>(rgb[x] >> 8), static_cast<u8>(rgb[x])}; std::fwrite(c, 1, 3, o); }
      }
      std::fclose(o);
    }
    const int slot = d.slot;
    in_ab = true; g_ab_no_shadow = true;
    render(dl, arena, arena_top, arena_epoch, lines, mode);
    glBindFramebuffer(GL_FRAMEBUFFER, d.fbo_out[d.slot]);
    save(pre + ".noshadow.ppm");
    in_ab = false; g_ab_no_shadow = false;
    d.slot = slot;
    std::fprintf(stderr, "hires3d: render %u written to %s.{hi,noshadow,1x}.ppm\n", renders - 1, pre.c_str());
  }
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  return tex;
}

unsigned GlHires::recomposite(const gpu::Engine2D::HiresLine* lines, int mode) {
  return d_ && lines ? composite(lines, mode) : 0;
}

bool GlHires::brightened(const gpu::Engine2D::HiresLine* lines) {
  for (u32 y = 0; y < 192; ++y) {
    const u16 mb = lines[y].mbright;
    if (((mb >> 14) == 1 || (mb >> 14) == 2) && (mb & 0x1F)) return true;
  }
  return false;
}

bool GlHires::draw3d(const gpu::Renderer3D::DisplayList& dl, const std::vector<u32>& arena, u32 arena_top, u64 arena_epoch) {
  Impl& d = *d_;
  d.layer3d = 0;
  d.vb_k = (d.vb_k + 1) % Impl::kSets;
  d.vao = d.sets[d.vb_k].vao; d.vbo = d.sets[d.vb_k].vbo;
  const auto T0 = clk::now();
  const u32 S = d.S;
  const GLsizei W = static_cast<GLsizei>(256 * S), H = static_cast<GLsizei>(192 * S);
  const gpu::vk::GpuFrame& f = dl.frame;
  const bool wbuf = (f.flags & DS_FF_WBUFFER) != 0;

  // Textures new since the last frame (the arena is append-only per epoch).
  if (d.arena_epoch != arena_epoch) { d.arena_epoch = arena_epoch; d.arena_up = 0; }
  if (arena_top > d.arena_up) {
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, d.arena);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, static_cast<GLintptr>(d.arena_up) * 4, static_cast<GLsizeiptr>(arena_top - d.arena_up) * 4, arena.data() + d.arena_up);
    d.arena_up = arena_top;
  }

  const auto T1 = clk::now();
  // Fans, placed as lean.vert places them: the opaque prefix (the pre-pass),
  // then the tail in list order, in runs.
  d.vb.clear();
  std::vector<std::pair<u32, u32>> runs;   // tail: [first vertex, key] where key = translucent | depth-write << 1
  auto vert = [&](const GpuPoly& p, u32 vi) {
    const GpuVert& vt = dl.verts[p.first_vert + vi];
    const float w = std::max(static_cast<float>(vt.w), 1.f);
    const float sxf = static_cast<float>(vt.sx), syf = static_cast<float>(vt.sy);
    // Vertices where the DS put them: the lean raster's right-edge push (its
    // 1x span rule under MSAA) differs between polygons sharing an edge, and
    // at hi-res that opened cracks inside models. A zero-width or -height
    // polygon (a line to the DS) still gets one pixel.
    float ox = 0.f, oy = 0.f;
    if (p.xmax == p.xmin) ox = (vi == 1 || vi == 2) ? 1.f : 0.f;
    if (p.ybot == p.ytop) oy = (vi == 1 || vi == 2) ? 1.f : 0.f;
    const float x = (sxf + ox) / 256.f * 2.f - 1.f, y = (syf + oy) / 192.f * 2.f - 1.f;
    float z = std::clamp(static_cast<float>(vt.z), 0.f, 16777215.f);
    if (p.attr & 0x4000u) z = std::max(z - (wbuf ? 255.f : 512.f), 0.f);
    Vtx o{};
    o.pos[0] = x * w; o.pos[1] = y * w; o.pos[2] = 0.f; o.pos[3] = w;   // depth comes from the fragment shader
    o.dz = z;
    o.rgb[0] = static_cast<float>(vt.r); o.rgb[1] = static_cast<float>(vt.g); o.rgb[2] = static_cast<float>(vt.b);
    o.st[0] = static_cast<float>(vt.s); o.st[1] = static_cast<float>(vt.t);
    o.attr = p.attr; o.texparam = p.texparam; o.tex_offset = p.tex_offset; o.tex_wh = p.tex_w | (p.tex_h << 16);
    o.flags = p.flags | ((static_cast<u32>(&p - dl.polys.data()) + 1u) << 12);   // the pre-pass's winner id
    return o;
  };
  auto fan = [&](const GpuPoly& p) { for (u32 t = 1; t + 1 < p.nverts; ++t) { d.vb.push_back(vert(p, 0)); d.vb.push_back(vert(p, t)); d.vb.push_back(vert(p, t + 1)); } };
  const u32 prefix = std::min(f.first_ordered, dl.npoly);
  // The prefix in list order: on a depth tie the DS keeps the earlier
  // polygon, so a reorder (near to far) flickered where polygons meet at
  // equal depth (NSMB's map).
  for (u32 k = 0; k < prefix; ++k) {
    const GpuPoly& p = dl.polys[k];
    if (p.nverts < 3 || (p.flags & (DS_PF_SHADOW_MASK | DS_PF_SHADOW))) continue;
    fan(p);
  }
  const u32 opaque_end = static_cast<u32>(d.vb.size());
  bool last_mask = false;
  const bool no_shadow = g_ab_no_shadow;
  for (u32 k = prefix; k < dl.npoly; ++k) {
    const GpuPoly& p = dl.polys[k];
    if (p.nverts < 3 || (no_shadow && (p.flags & (DS_PF_SHADOW_MASK | DS_PF_SHADOW)))) continue;
    // Shadow volumes: a mask (shadow, id 0) marks where its depth test fails;
    // a shadow (id > 0) draws only there. Consecutive masks add up; the first
    // mask after anything else starts the marks again (bit 13).
    if (p.flags & DS_PF_SHADOW_MASK) {
      const u32 key = (1u << 11) | (last_mask ? 0u : 1u << 13);
      if (runs.empty() || runs.back().second != key || !last_mask) runs.emplace_back(static_cast<u32>(d.vb.size()), key);
      last_mask = true;
      fan(p);
      continue;
    }
    last_mask = false;
    if (p.flags & DS_PF_SHADOW) {
      const u32 key = 1u | ((p.attr & (1u << 11)) ? 2u : 0u) | ((((p.attr >> 24) & 0x3Fu) + 1u) << 2) | (1u << 12);
      if (runs.empty() || runs.back().second != key) runs.emplace_back(static_cast<u32>(d.vb.size()), key);
      fan(p);
      continue;
    }
    // Translucent runs also break on the polygon id: the stencil keeps the DS
    // rule that a translucent pixel does not blend over one of its own id.
    const bool tr = (p.flags & DS_PF_TRANSLUCENT) != 0;
    // A translucent polygon at alpha 31 (translucent for its texture's alpha)
    // has opaque pixels too, which the DS draws as an opaque polygon's: they
    // write the depth, so a farther polygon later in the list stays behind.
    const bool opq = tr && ((p.attr >> 16) & 0x1Fu) == 31u;
    const u32 key = (tr ? 1u : 0u) | ((p.attr & (1u << 11)) ? 2u : 0u) | (tr ? ((((p.attr >> 24) & 0x3Fu) + 1u) << 2) : 0u) | (opq ? 1u << 10 : 0u);
    if (runs.empty() || runs.back().second != key) runs.emplace_back(static_cast<u32>(d.vb.size()), key);
    fan(p);
  }
  const u32 total = static_cast<u32>(d.vb.size());
  d.nruns = static_cast<u32>(runs.size());
  // DS_HIRES_POLYS=x0,y0,x1,y1 (DS pixels): the polygons over that box, once, at render 60 (debug).
  if (static const char* pd = std::getenv("DS_HIRES_POLYS"); pd) {
    static u32 n = 0;
    static const u32 at = std::getenv("DS_HIRES_POLYS_AT") ? static_cast<u32>(std::atoi(std::getenv("DS_HIRES_POLYS_AT"))) : 60;
    if (++n == at) {
      int x0 = 0, y0 = 0, x1 = 0, y1 = 0; std::sscanf(pd, "%d,%d,%d,%d", &x0, &y0, &x1, &y1);
      std::fprintf(stderr, "polys over %d,%d-%d,%d (first_ordered %u, %s):\n", x0, y0, x1, y1, f.first_ordered, wbuf ? "W" : "Z");
      for (u32 k = 0; k < dl.npoly; ++k) {
        const GpuPoly& p = dl.polys[k];
        if (p.xmax < x0 || p.xmin > x1 || p.ybot < y0 || p.ytop > y1) continue;
        s32 zl = 0x7FFFFFFF, zh = -0x7FFFFFFF;
        for (u32 v = 0; v < p.nverts; ++v) { zl = std::min(zl, dl.verts[p.first_vert + v].z); zh = std::max(zh, dl.verts[p.first_vert + v].z); }
        std::fprintf(stderr, "  #%u attr %08x flags %02x id %u alpha %u eq %d dw %d n %u x %d-%d y %d-%d z %d-%d\n", k, p.attr, p.flags, (p.attr >> 24) & 63, (p.attr >> 16) & 31,
                     (p.attr >> 14) & 1, (p.attr >> 11) & 1, p.nverts, p.xmin, p.xmax, p.ytop, p.ybot, zl, zh);
      }
    }
  }

  const auto T2 = clk::now();
  glBindVertexArray(d.vao);
  glBindBuffer(GL_ARRAY_BUFFER, d.vbo);
  const size_t bytes = std::max<size_t>(d.vb.size(), 1) * sizeof(Vtx);
  if (bytes > d.sets[d.vb_k].vbo_cap) { d.sets[d.vb_k].vbo_cap = bytes * 2; glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(bytes * 2), nullptr, GL_STREAM_DRAW); }
  if (!d.vb.empty()) glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(d.vb.size() * sizeof(Vtx)), d.vb.data());

  const auto T3 = clk::now();
  const int skip = bench_skip_;
  d.stages_next();
  auto stage = [&](int k) { d.stage(k); };
  stage(0);
  // The 3D layer at S x.
  glViewport(0, 0, W, H);
  glDisable(GL_SCISSOR_TEST);
  const u32 cc = f.clear_color, ca = (cc >> 24) & 0x1F;
  const float cz = static_cast<float>(f.clear_depth & 0xFFFFFF);
  glDepthMask(GL_TRUE); glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glDisable(GL_BLEND); glDisable(GL_STENCIL_TEST);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, d.arena);
  // The opaque prefix, pass 1: the depth test, in list order (on a tie the
  // earlier polygon keeps the pixel, as on the DS), and the winners.
  glBindFramebuffer(GL_FRAMEBUFFER, d.fbo_pre);
  {
    const GLuint none[4] = {0, 0, 0, 0};
    glClearBufferuiv(GL_COLOR, 0, none);
    glClearBufferuiv(GL_COLOR, 1, none);
    glClearBufferfi(GL_DEPTH_STENCIL, 0, std::min(cz, 16777215.f) / 16777216.f, 0);
  }
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LESS);
  if (opaque_end && !(skip & 1)) {
    glUseProgram(d.pre);
    glUniform1ui(d.p_alpha_ref, f.alpha_ref); glUniform1i(d.p_wbuf, wbuf ? 1 : 0);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(opaque_end));
  }
  stage(1);
  // Pass 2, the resolve: the full shading, once a pixel -- each winner's.
  glBindFramebuffer(GL_FRAMEBUFFER, d.fbo3d);
  // The id plane is for edge marking and fog alone: without them, one set of
  // draw buffers for the whole pass (changing them ends the GPU's batch: a
  // store and a reload of every attachment, per translucent run).
  const bool edge_on = (f.dispcnt & 0x20u) != 0, fog_on = (f.dispcnt & 0x80u) != 0;
  const bool want_id = edge_on || fog_on;
  { const GLenum bufs[2] = {GL_COLOR_ATTACHMENT0, want_id ? static_cast<GLenum>(GL_COLOR_ATTACHMENT1) : static_cast<GLenum>(GL_NONE)}; glDrawBuffers(2, bufs); }
  {   // each attachment its own clear: the id plane is an integer one
    const GLfloat col[4] = {static_cast<float>(cc & 0x3F) / 255.f, static_cast<float>((cc >> 8) & 0x3F) / 255.f, static_cast<float>((cc >> 16) & 0x3F) / 255.f, ca ? static_cast<float>(std::min(255u, (ca + 1) * 8)) / 255.f : 0.f};
    const GLuint rear_id[4] = {((f.clear_attr >> 24) & 0x3Fu) | ((f.clear_attr >> 9) & 0x40u), 0, 0, 0};   // the rear plane: its id and fog bit, no polygon drew
    glClearBufferfv(GL_COLOR, 0, col);
    glClearBufferuiv(GL_COLOR, 1, rear_id);
  }
  if (opaque_end && !(skip & 2)) {
    // The polygons' records, into the ring's next buffer.
    d.polys_k = (d.polys_k + 1) % 3;
    d.precs.resize(static_cast<size_t>(dl.npoly) * 8);
    for (u32 k = 0; k < dl.npoly; ++k) {
      const GpuPoly& p = dl.polys[k]; u32* r = &d.precs[k * 8];
      r[0] = p.attr; r[1] = p.texparam; r[2] = p.tex_offset; r[3] = p.tex_w | (p.tex_h << 16); r[4] = p.flags; r[5] = r[6] = r[7] = 0;
    }
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, d.polys[d.polys_k]);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, static_cast<GLsizeiptr>(d.precs.size() * 4), d.precs.data());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, d.polys[d.polys_k]);
    glDisable(GL_DEPTH_TEST);
    glUseProgram(d.resolve);
    glUniform1ui(d.w_dispcnt, f.dispcnt); glUniform1uiv(d.w_toon, 32, dl.post.toon);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, d.g0);
    glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, d.g1);
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(d.vao_empty);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(d.vao);
    glEnable(GL_DEPTH_TEST);
  }
  stage(2);
  // Stencil: the id + 1 of the translucent polygon that last blended a pixel,
  // 0 where an opaque one drew last.
  // Bit 7: the shadow masks' marks, which the rest leave alone.
  glEnable(GL_STENCIL_TEST);
  glStencilMask(0x7F);
  glStencilFunc(GL_ALWAYS, 0, 0x7F);
  glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
  glUseProgram(d.raster);
  glUniform1ui(d.u_dispcnt, f.dispcnt); glUniform1ui(d.u_alpha_ref, f.alpha_ref); glUniform1uiv(d.u_toon, 32, dl.post.toon); glUniform1i(d.r_wbuf, wbuf ? 1 : 0); glUniform1i(d.r_split, 0);
  // The id plane takes opaque pixels only (a translucent one keeps the opaque
  // id under it, and its fog bit); switched only when it changes.
  int id_on = want_id ? 1 : 0;
  auto id_writes = [&](bool on) {
    if (!want_id || id_on == (on ? 1 : 0)) return;
    id_on = on ? 1 : 0;
    if (d.color_maski) d.color_maski(1, on, on, on, on);
    else { const GLenum b[2] = {GL_COLOR_ATTACHMENT0, on ? static_cast<GLenum>(GL_COLOR_ATTACHMENT1) : static_cast<GLenum>(GL_NONE)}; glDrawBuffers(2, b); }
  };
  bool win_bound = false;
  for (size_t r = 0; r < runs.size() && !(skip & 4); ++r) {
    const u32 v0 = runs[r].first, v1 = r + 1 < runs.size() ? runs[r + 1].first : total;
    const bool trans = runs[r].second & 1, dw = (runs[r].second & 2) != 0;
    const GLint sref = static_cast<GLint>((runs[r].second >> 2) & 0xFFu);
    if (v1 <= v0) continue;
    if (runs[r].second & (1u << 11)) {   // shadow masks: the stencil mark where the depth test fails, nothing drawn
      glStencilMask(0x80);
      if (runs[r].second & (1u << 13)) { const GLint zero = 0; glClearBufferiv(GL_STENCIL, 0, &zero); }
      glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
      glDisable(GL_BLEND); glDepthMask(GL_FALSE);
      glStencilFunc(GL_ALWAYS, 0x80, 0x80);
      glStencilOp(GL_KEEP, GL_REPLACE, GL_KEEP);
      glDrawArrays(GL_TRIANGLES, static_cast<GLint>(v0), static_cast<GLsizei>(v1 - v0));
      glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
      glStencilMask(0x7F);
      glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
      if (d.color_maski && want_id && !id_on) d.color_maski(1, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);   // glColorMask set every buffer's
      continue;
    }
    const bool shadow = (runs[r].second & (1u << 12)) != 0;
    if (shadow) {
      if (!win_bound) {
        glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D, d.g0); glActiveTexture(GL_TEXTURE0);
        if (opaque_end) glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, d.polys[d.polys_k]);
        win_bound = true;
      }
      glUniform1i(d.r_shadow, opaque_end ? 1 : 0);
    }
    if (trans && (runs[r].second & (1u << 10))) {   // its opaque pixels first, as an opaque polygon's
      id_writes(true);
      glStencilFunc(GL_ALWAYS, 0, 0x7F); glDisable(GL_BLEND); glDepthMask(GL_TRUE);
      glUniform1i(d.r_split, 1);
      glDrawArrays(GL_TRIANGLES, static_cast<GLint>(v0), static_cast<GLsizei>(v1 - v0));
      glUniform1i(d.r_split, 2);
    }
    id_writes(!trans);
    if (shadow) glStencilFunc(GL_EQUAL, 0x80 | sref, 0x80);   // only where a mask marked; writes its id where it draws
    else if (trans) glStencilFunc(GL_NOTEQUAL, sref, 0x7F);   // not over its own id's blend; writes its id where it draws
    else glStencilFunc(GL_ALWAYS, 0, 0x7F);                  // an opaque pixel: any id may blend over it again
    if (trans) {
      glEnable(GL_BLEND);
      glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE);
      glBlendEquationSeparate(GL_FUNC_ADD, GL_MAX);
      glDepthMask(dw ? GL_TRUE : GL_FALSE);
    } else { glDisable(GL_BLEND); glDepthMask(GL_TRUE); }
    glDrawArrays(GL_TRIANGLES, static_cast<GLint>(v0), static_cast<GLsizei>(v1 - v0));
    glUniform1i(d.r_split, 0);
    if (shadow) glUniform1i(d.r_shadow, 0);
  }
  id_writes(true);   // as the next render's clear expects
  stage(3);
  glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDepthMask(GL_TRUE); glDisable(GL_STENCIL_TEST);
  if (!want_id) {   // no edge marking or fog: depth, stencil and ids die with the pass (a tiler need not write them out)
    const GLenum dead[2] = {GL_DEPTH_STENCIL_ATTACHMENT, GL_COLOR_ATTACHMENT1};
    glInvalidateFramebuffer(GL_FRAMEBUFFER, 2, dead);
  }
  // Edge marking and fog: in the composite, where it reads the layer.
  d.layer3d = d.col3d;
  d.edge_on = edge_on && !(skip & 8); d.fog_on = fog_on && !(skip & 8); d.fog_alpha = (f.dispcnt & 0x40u) != 0;
  d.post = dl.post;

  stage(-1);
  const auto T4 = clk::now();
  if (d.timing) { d.t[0] += ms(T0, T1); d.t[1] += ms(T1, T2); d.t[2] += ms(T2, T3); d.t[3] += ms(T3, T4); ++d.tn; }
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  return true;
}

unsigned GlHires::composite(const gpu::Engine2D::HiresLine* lines, int mode) {
  Impl& d = *d_;
  if (!d.layer3d) return 0;
  const GLuint layer3d = d.layer3d;
  const GLsizei W = static_cast<GLsizei>(256 * d.S), H = static_cast<GLsizei>(192 * d.S);
  d.set_k = (d.set_k + 1) % Impl::kSets;
  auto stage = [&](int k) { d.stage(k); };
  stage(5);
  const auto T4 = clk::now();
  // Engine A's lines for the composite, copied as they lie in Gpu's (LINE_PLANES
  // reads them): records and classes, and the registers after them. The full
  // composite's planes (top, second, ids, kind, alpha, window) only when a
  // line takes it: a class 2 pixel, or master brightness over a class 1 one.
  using HL = gpu::Engine2D::HiresLine;
  static_assert(sizeof(HL) % 4 == 0 && offsetof(HL, out) == 0 && offsetof(HL, under) == 1024 && offsetof(HL, top) == 2048 && offsetof(HL, second) == 3072 &&
                offsetof(HL, top_id) == 4096 && offsetof(HL, top_kind) == 4352 && offsetof(HL, top_alpha) == 4608 && offsetof(HL, second_id) == 4864 &&
                offsetof(HL, win) == 5120 && offsetof(HL, cls) == 5376 && offsetof(HL, bldcnt) == 5632 && offsetof(HL, mbright) == 5634 &&
                offsetof(HL, eva) == 5636 && offsetof(HL, evb) == 5637 && offsetof(HL, evy) == 5638 && offsetof(HL, valid) == 5639, "LINE_PLANES' layout");
  bool full = false;
  for (u32 y = 0; y < 192; ++y) full |= lines[y].valid && lines[y].full;
  if (bench_skip_ & 16) full = false;
  Impl::Set& st = d.sets[d.set_k];
  if (st.fence) { glClientWaitSync(st.fence, GL_SYNC_FLUSH_COMMANDS_BIT, 100'000'000); glDeleteSync(st.fence); st.fence = nullptr; }
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, st.lines);
  if (u8* m = static_cast<u8*>(glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, 192 * sizeof(HL), GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT))) {
    if (full) std::memcpy(m, lines, 192 * sizeof(HL));
    else for (u32 y = 0; y < 192; ++y) {
      u8* l = m + y * sizeof(HL);
      const u8* src = reinterpret_cast<const u8*>(&lines[y]);
      std::memcpy(l, src, 2048);                                              // out, under
      std::memcpy(l + offsetof(HL, cls), src + offsetof(HL, cls), offsetof(HL, valid) + 1 - offsetof(HL, cls));   // classes, registers
    }
    glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
  }
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, st.lines);
  stage(-1);
  const auto T5 = clk::now();
  stage(4);
  // The composite at S x.
  d.slot = (d.slot + 1) % Impl::kOut;
  glBindFramebuffer(GL_FRAMEBUFFER, d.fbo_out[d.slot]);
  glViewport(0, 0, W, H);
  glBindVertexArray(d.vao_empty);
  // Pass 1: depth 0 where it drew (gl_FragCoord.z of the full-screen triangle
  // is 0.5; ALWAYS writes it), cleared 1 where it discarded.
  glClearDepthf(1.f); glClear(GL_DEPTH_BUFFER_BIT);
  glEnable(GL_DEPTH_TEST); glDepthFunc(GL_ALWAYS); glDepthMask(GL_TRUE);
  glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, d.dep3d);
  glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, d.id3d);
  glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, layer3d);
  const int v = d.edge_on || d.fog_on ? 1 : 0;
  auto layer_uniforms = [&](const Impl::LayerLocs& l) {
    if (!v) return;
    glUniform1i(l.edge_on, d.edge_on ? 1 : 0); glUniform1i(l.fog_on, d.fog_on ? 1 : 0);
    glUniform1uiv(l.edge, 8, d.post.edge); glUniform1i(l.fog_alpha, d.fog_alpha ? 1 : 0);
    glUniform1ui(l.fog_color, d.post.fog_color); glUniform1ui(l.fog_offset, d.post.fog_offset); glUniform1ui(l.fog_shift, d.post.fog_shift);
    glUniform1uiv(l.density, 34, d.post.density);
  };
  glUseProgram(d.fast[v]);
  glUniform1i(d.f_mode[v], mode);
  layer_uniforms(d.fast_l);
  if (!(bench_skip_ & 16)) glDrawArrays(GL_TRIANGLES, 0, 3);
  // Pass 2: the full composite, only where depth is still 1 (LESS against 0.5 fails elsewhere: early-Z skips them).
  if (full) {
    glDepthFunc(GL_LESS); glDepthMask(GL_FALSE);
    glUseProgram(d.comp[v]);
    glUniform1i(d.u_mode[v], mode);
    layer_uniforms(d.comp_l);
    glDrawArrays(GL_TRIANGLES, 0, 3);
  }
  { const GLenum dead = GL_DEPTH_ATTACHMENT; glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, &dead); }   // the composite's depth: only within it
  glDisable(GL_DEPTH_TEST); glDepthMask(GL_TRUE);
  glBindVertexArray(0);
  st.fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
  stage(-1);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  if (d.timing) {
    const auto T6 = clk::now();
    d.t[4] += ms(T4, T5); d.t[5] += ms(T5, T6);
    if (d.tn >= 120) {
      std::fprintf(stderr, "hires3d cpu ms/render: arena %.2f, fans %.2f, vbo %.2f, raster calls %.2f, planes %.2f, composite calls %.2f (%zu verts, %u runs)\n",
                   d.t[0] / d.tn, d.t[1] / d.tn, d.t[2] / d.tn, d.t[3] / d.tn, d.t[4] / d.tn, d.t[5] / d.tn, d.vb.size(), d.nruns);
      for (double& x : d.t) x = 0;
      d.tn = 0;
    }
  }
  return d.out[d.slot];
}

} // namespace ds::sdl

#endif // DSPERATE_GLES
