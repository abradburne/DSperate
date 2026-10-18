// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
// The hi-res 3D's shaders (gl_hires.cpp), GLSL ES 3.10. The fragment code
// follows the DS's own arithmetic -- 6-bit channels, 5-bit alpha, integer
// blends -- as the CPU raster does it (render3d.cpp, kernels_ref.cpp), so a
// pixel the hi-res raster covers as the 1x one does comes out the same.
//
//   kRasterVS + kRasterFS   a polygon's fans. Built three ways: plain (the
//                           translucent tail), PREPASS (the opaque prefix's
//                           depth test, keeping each pixel's winner) and,
//                           with kFullVS, RESOLVE (the winners shaded once
//                           a pixel, full screen).
//   kLinePlanes + kLayer3D  engine A's kept lines and the 3D layer as the
//     + kFastMain           composite reads them, with SCALE and LINE_WORDS
//     + kCompositeMain      defined: pass 1 (kFastMain) settles the pixels the
//                           3D layer plays no part in or simply covers; pass
//                           2 (kCompositeMain) runs engine A's colour effects
//                           and master brightness on the rest. EDGE_FOG
//                           compiles edge marking and fog into kLayer3D.
namespace ds::sdl::hires_glsl {

inline constexpr char kRasterVS[] = R"(#version 310 es
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

// ds_shade.glsl's shade_pixel, the texel arena a storage buffer. Measured on
// Golden Sun's title at 2x: shading every overlapping fragment of the opaque
// prefix (~4x the screen) was the bulk of the 3D's GPU time on the one-core
// G52, hence the visibility buffer (PREPASS, then RESOLVE).
inline constexpr char kRasterFS[] = R"(
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

inline constexpr char kFullVS[] = R"(#version 310 es
out vec2 v_uv;
void main() {
  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
  v_uv = p;
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

// The 3D layer as the composite reads it (layer3d()). With EDGE_FOG, edge
// marking (DISP3DCNT bit 5) then fog (bit 7) as the DS's final pass does
// them (render3d.cpp's final_pass_ref), at SCALE x: the id plane says which
// polygon (or the rear plane) is on top and whether it carries the fog bit,
// the depth plane its DS depth. An edge pixel takes its id's edge colour
// where a neighbour one DS pixel away (SCALE hi-res pixels) has another id
// and lies further back. A translucent polygon leaves the id plane as it
// was, so its pixels keep the fog bit of what is under them (the DS ANDs its
// own in). A build of its own: compiled in, it cost frames without either
// 0.6 GPU ms (NSMB at 2x).
inline constexpr char kLayer3D[] = R"(
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
    uint ab = uint(round(c.a * 255.0)), a = ab == 0u ? 0u : ((ab + 1u) >> 3) - 1u;   // 255 is alpha 31 ((a + 1) * 8, saturated)
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
)";

// Engine A's kept lines (Engine2D::HiresLine) in a storage buffer, copied as
// they lie in memory, LINE_WORDS words a line: the out, under, top and second
// records (6-bit RGB; words 0, 256, 512, 768), then the top id, top kind, top
// alpha, second id, window and class bytes (planes 0-5 from word 1024), then
// the line's registers (word 1408: BLDCNT | MASTER_BRIGHT << 16; 1409: EVA,
// EVB, EVY and the valid flag, a byte each). Compiled for one SCALE: a
// division by a uniform was half the composite's GPU time on the G52.
inline constexpr char kLinePlanes[] = R"(
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
)";

// Pass 1, every SCALE x pixel, from its class: 0 = the 3D layer plays no
// part, so engine A's own 1x pixel is exact; 1 = the 3D layer on top with
// nothing applied after it, so the hi-res sample is the pixel (where it
// covers; elsewhere what it covers). Master brightness reaches every pixel
// of a shown line, so there a class 1 pixel takes the full composite too (a
// capture is taken before it). The rest is discarded, left for pass 2.
inline constexpr char kFastMain[] = R"(
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

// Pass 2: engine A's colour effects and master brightness (kernels_ref.cpp
// composite_line / output_line) per SCALE x pixel, the 3D layer's entries
// taken from the hi-res frame.
inline constexpr char kCompositeMain[] = R"(
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
  uint ha = ha8 == 0u ? 0u : ((ha8 + 1u) >> 3) - 1u;   // 255 is alpha 31 ((a + 1) * 8, saturated)
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

} // namespace ds::sdl::hires_glsl
