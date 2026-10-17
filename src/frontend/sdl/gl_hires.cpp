// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "frontend/sdl/gl_hires.h"

#if !DSPERATE_GLES
namespace ds::sdl {
struct GlHires::Impl {};
GlHires::GlHires() = default;
GlHires::~GlHires() = default;
bool GlHires::init(u32, std::string* why) { if (why) *why = "built without GLES"; return false; }
unsigned GlHires::render(const gpu::Renderer3D::DisplayList&, const std::vector<u32>&, u32, u64, const gpu::Engine2D::HiresLine*, const u32*) { return 0; }
} // namespace ds::sdl
#else
#include <GLES3/gl31.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace ds::sdl {

namespace {

using gpu::vk::GpuPoly;
using gpu::vk::GpuVert;

// One fan vertex, placed as the lean raster places it (vk/shaders/lean.vert).
struct Vtx {
  float pos[4];
  float rgb[3];
  float st[2];
  u32 attr, texparam, tex_offset, tex_wh, flags;
};

constexpr u32 kArenaW = 4096, kArenaH = (2u << 20) / kArenaW;   // Renderer3D's display arena, 8 MB

const char* kRasterVS = R"(#version 310 es
layout(location = 0) in vec4 a_pos;
layout(location = 1) in vec3 a_rgb;
layout(location = 2) in vec2 a_st;
layout(location = 3) in uvec4 a_poly;   // attr, texparam, tex_offset, tex_w | tex_h << 16
layout(location = 4) in uint a_flags;
out vec3 v_rgb;
out vec2 v_st;
flat out uvec4 v_poly;
flat out uint v_flags;
void main() { gl_Position = a_pos; v_rgb = a_rgb; v_st = a_st; v_poly = a_poly; v_flags = a_flags; }
)";

// ds_shade.glsl's shade_pixel; the texel arena is a 4096-wide R32UI texture.
const char* kRasterFS = R"(#version 310 es
precision highp float; precision highp int; precision highp usampler2D;
in vec3 v_rgb; in vec2 v_st; flat in uvec4 v_poly; flat in uint v_flags;
uniform highp usampler2D u_texels;
uniform uint u_dispcnt, u_alpha_ref;
uniform uint u_toon[32];
out vec4 o_col;
uint texel(uint i) { return texelFetch(u_texels, ivec2(int(i & 4095u), int(i >> 12)), 0).r; }
uint c15_to_18(uint c, uint shift) { uint v = ((shift == 0u ? (c << 1) : (c >> shift)) & 0x3Eu); return v != 0u ? v + 1u : v; }
void main() {
  uint attr = v_poly.x, texparam = v_poly.y, tex_offset = v_poly.z;
  int w = int(v_poly.w & 0xFFFFu), h = int(v_poly.w >> 16);
  uint blendmode = (attr >> 4) & 3u, polyalpha = (attr >> 16) & 0x1Fu;
  vec3 rgb = clamp(v_rgb, vec3(0.0), vec3(511.0));
  uint vr = uint(int(round(rgb.r)) >> 3) & 0x3Fu, vg = uint(int(round(rgb.g)) >> 3) & 0x3Fu, vb = uint(int(round(rgb.b)) >> 3) & 0x3Fu;
  uint r, g, b, a;
  bool highlight = (u_dispcnt & 2u) != 0u;
  if (blendmode == 2u) {
    if (highlight) { vg = vr; vb = vr; }
    else { uint tc = u_toon[vr >> 1]; vr = c15_to_18(tc, 0u); vg = c15_to_18(tc, 4u); vb = c15_to_18(tc, 9u); }
  }
  if ((v_flags & 8u) != 0u) {
    int s = int(floor(v_st.x + 0.01)) >> 4, t = int(floor(v_st.y + 0.01)) >> 4;
    bool rep_s = (texparam & 0x10000u) != 0u, rep_t = (texparam & 0x20000u) != 0u;
    bool flp_s = (texparam & 0x40000u) != 0u, flp_t = (texparam & 0x80000u) != 0u;
    if (rep_s) { if (flp_s && (s & w) != 0) s = (w - 1) - (s & (w - 1)); else s &= w - 1; } else s = clamp(s, 0, w - 1);
    if (rep_t) { if (flp_t && (t & h) != 0) t = (h - 1) - (t & (h - 1)); else t &= h - 1; } else t = clamp(t, 0, h - 1);
    uint tpk = texel(tex_offset + uint(t * w + s));
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
  if (a <= u_alpha_ref) discard;
  // Raw 6-bit channels; alpha (a + 1) * 8 so the blend's SRC_ALPHA is (a + 1) / 32, as the lean raster.
  uint ab = a == 0u ? 0u : min(255u, (a + 1u) * 8u);
  o_col = vec4(float(r), float(g), float(b), float(ab)) / 255.0;
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

// Engine A's colour effects and master brightness (kernels_ref.cpp
// composite_line / output_line) per S x pixel, with the 3D layer's entries
// taken from the hi-res frame.
const char* kCompositeFS = R"(#version 310 es
precision highp float; precision highp int; precision highp usampler2D; precision highp sampler2D;
uniform highp usampler2D u_top, u_second, u_meta, u_win, u_regs, u_fb;
uniform highp sampler2D u_3d;
uniform int u_scale;
out vec4 o_col;
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
uint up(uint c, uint f, uint bias) { return c + ((((63u - c) * f) + bias) >> 4); }
uint dn(uint c, uint f, uint bias) { return c - (((c * f) + bias) >> 4); }
uint brighten(uint v, uint f, uint bias) { return up(v & 0x3Fu, f, bias) | (up((v >> 8) & 0x3Fu, f, bias) << 8) | (up((v >> 16) & 0x3Fu, f, bias) << 16); }
uint darken(uint v, uint f, uint bias) { return dn(v & 0x3Fu, f, bias) | (dn((v >> 8) & 0x3Fu, f, bias) << 8) | (dn((v >> 16) & 0x3Fu, f, bias) << 16); }
void main() {
  ivec2 p = ivec2(gl_FragCoord.xy);
  ivec2 d = p / u_scale;
  uvec2 regs = texelFetch(u_regs, ivec2(d.y, 0), 0).rg;
  if ((regs.y >> 31) == 0u) {   // no 3D on the line: its finished 1x pixel
    uint c = texelFetch(u_fb, d, 0).r;
    o_col = vec4(float((c >> 16) & 255u), float((c >> 8) & 255u), float(c & 255u), 255.0) / 255.0;
    return;
  }
  uint top = texelFetch(u_top, d, 0).r, second = texelFetch(u_second, d, 0).r, meta = texelFetch(u_meta, d, 0).r;
  uint win = texelFetch(u_win, d, 0).r;
  uint top_id = meta & 0xFFu, kind = (meta >> 8) & 0xFFu, talpha = (meta >> 16) & 0xFFu, second_id = meta >> 24;
  vec4 h = texelFetch(u_3d, p, 0);
  uint ha8 = uint(round(h.a * 255.0));
  uint ha = ha8 == 0u ? 0u : (ha8 >> 3) - 1u;
  uint hpx = uint(round(h.r * 255.0)) | (uint(round(h.g * 255.0)) << 8) | (uint(round(h.b * 255.0)) << 16) | (ha << 24);
  if (kind == 3u) {
    if (ha != 0u) { top = hpx; talpha = ha; }
    else { top = second; top_id = second_id; kind = 0u; talpha = 0u; }   // past the polygon's hi-res edge: what lay beneath
  } else if (second_id == 1u && ha != 0u) second = hpx;
  uint bldcnt = regs.x & 0xFFFFu, eva = (regs.x >> 16) & 0x1Fu, evb = (regs.x >> 21) & 0x1Fu, evy = (regs.x >> 26) & 0x1Fu;
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
  // Master brightness, then 6 -> 8 bits.
  uint mb = regs.y & 0xFFFFu, mode = mb >> 14, f = min(mb & 0x1Fu, 16u);
  if (mode == 1u) o = brighten(o, f, 0u);
  else if (mode == 2u) o = darken(o, f, 0xFu);
  uvec3 c6 = uvec3(o & 0x3Fu, (o >> 8) & 0x3Fu, (o >> 16) & 0x3Fu);
  uvec3 c8 = (c6 << 2) | (c6 >> 4);
  o_col = vec4(vec3(c8) / 255.0, 1.0);
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
  GLuint raster = 0, comp = 0;
  GLint u_dispcnt = -1, u_alpha_ref = -1, u_toon = -1, u_scale = -1;
  GLuint vao = 0, vbo = 0, vao_empty = 0;
  size_t vbo_cap = 0;
  GLuint arena = 0;
  u32 arena_up = 0; u64 arena_epoch = ~0ull;   // words of the arena on the GPU, and of which generation
  GLuint fbo3d = 0, col3d = 0, dep3d = 0;
  GLuint fbo_out = 0, out = 0;
  GLuint t_top = 0, t_second = 0, t_meta = 0, t_win = 0, t_regs = 0, t_fb = 0;
  std::vector<Vtx> vb;
  std::vector<u32> top, second, meta, regs;
  std::vector<u8> win;

  ~Impl() {
    GLuint texs[] = {arena, col3d, out, t_top, t_second, t_meta, t_win, t_regs, t_fb};
    for (GLuint t : texs) if (t) glDeleteTextures(1, &t);
    if (dep3d) glDeleteRenderbuffers(1, &dep3d);
    if (fbo3d) glDeleteFramebuffers(1, &fbo3d);
    if (fbo_out) glDeleteFramebuffers(1, &fbo_out);
    if (vbo) glDeleteBuffers(1, &vbo);
    if (vao) glDeleteVertexArrays(1, &vao);
    if (vao_empty) glDeleteVertexArrays(1, &vao_empty);
    if (raster) glDeleteProgram(raster);
    if (comp) glDeleteProgram(comp);
  }
};

GlHires::GlHires() = default;
GlHires::~GlHires() = default;

bool GlHires::init(u32 scale, std::string* why) {
  d_ = std::make_unique<Impl>();
  Impl& d = *d_;
  d.S = scale_ = std::clamp(scale, 2u, 4u);
  const GLsizei W = static_cast<GLsizei>(256 * d.S), H = static_cast<GLsizei>(192 * d.S);
  d.raster = program(kRasterVS, kRasterFS, why);
  d.comp = program(kFullVS, kCompositeFS, why);
  if (!d.raster || !d.comp) return false;
  d.u_dispcnt = glGetUniformLocation(d.raster, "u_dispcnt");
  d.u_alpha_ref = glGetUniformLocation(d.raster, "u_alpha_ref");
  d.u_toon = glGetUniformLocation(d.raster, "u_toon");
  glUseProgram(d.raster);
  glUniform1i(glGetUniformLocation(d.raster, "u_texels"), 0);
  glUseProgram(d.comp);
  const char* names[] = {"u_top", "u_second", "u_meta", "u_win", "u_regs", "u_fb", "u_3d"};
  for (int i = 0; i < 7; ++i) glUniform1i(glGetUniformLocation(d.comp, names[i]), i);
  d.u_scale = glGetUniformLocation(d.comp, "u_scale");
  glUniform1i(d.u_scale, static_cast<GLint>(d.S));

  d.arena = tex2d(GL_R32UI, kArenaW, static_cast<GLsizei>(kArenaH));
  d.col3d = tex2d(GL_RGBA8, W, H);
  glGenRenderbuffers(1, &d.dep3d); glBindRenderbuffer(GL_RENDERBUFFER, d.dep3d); glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, W, H);
  glGenFramebuffers(1, &d.fbo3d); glBindFramebuffer(GL_FRAMEBUFFER, d.fbo3d);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, d.col3d, 0);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, d.dep3d);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { if (why) *why = "3D framebuffer incomplete"; return false; }
  d.out = tex2d(GL_RGBA8, W, H);
  glGenFramebuffers(1, &d.fbo_out); glBindFramebuffer(GL_FRAMEBUFFER, d.fbo_out);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, d.out, 0);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { if (why) *why = "output framebuffer incomplete"; return false; }
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  d.t_top = tex2d(GL_R32UI, 256, 192); d.t_second = tex2d(GL_R32UI, 256, 192); d.t_meta = tex2d(GL_R32UI, 256, 192);
  d.t_win = tex2d(GL_R8UI, 256, 192); d.t_regs = tex2d(GL_RG32UI, 192, 1); d.t_fb = tex2d(GL_R32UI, 256, 192);
  d.top.resize(256 * 192); d.second.resize(256 * 192); d.meta.resize(256 * 192); d.win.resize(256 * 192); d.regs.resize(192 * 2);

  glGenVertexArrays(1, &d.vao_empty);
  glGenVertexArrays(1, &d.vao); glBindVertexArray(d.vao);
  glGenBuffers(1, &d.vbo); glBindBuffer(GL_ARRAY_BUFFER, d.vbo);
  glEnableVertexAttribArray(0); glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(Vtx), reinterpret_cast<void*>(offsetof(Vtx, pos)));
  glEnableVertexAttribArray(1); glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vtx), reinterpret_cast<void*>(offsetof(Vtx, rgb)));
  glEnableVertexAttribArray(2); glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vtx), reinterpret_cast<void*>(offsetof(Vtx, st)));
  glEnableVertexAttribArray(3); glVertexAttribIPointer(3, 4, GL_UNSIGNED_INT, sizeof(Vtx), reinterpret_cast<void*>(offsetof(Vtx, attr)));
  glEnableVertexAttribArray(4); glVertexAttribIPointer(4, 1, GL_UNSIGNED_INT, sizeof(Vtx), reinterpret_cast<void*>(offsetof(Vtx, flags)));
  glBindVertexArray(0);
  if (glGetError() != GL_NO_ERROR) { if (why) *why = "GL error during setup"; return false; }
  return true;
}

unsigned GlHires::render(const gpu::Renderer3D::DisplayList& dl, const std::vector<u32>& arena, u32 arena_top, u64 arena_epoch,
                         const gpu::Engine2D::HiresLine* lines, const u32* fb) {
  if (!d_ || !dl.valid || dl.epoch != arena_epoch || !lines || !fb) return 0;
  Impl& d = *d_;
  const u32 S = d.S;
  const GLsizei W = static_cast<GLsizei>(256 * S), H = static_cast<GLsizei>(192 * S);
  const gpu::vk::GpuFrame& f = dl.frame;
  const bool wbuf = (f.flags & DS_FF_WBUFFER) != 0;

  // Textures new since the last frame (the arena is append-only per epoch).
  if (d.arena_epoch != arena_epoch) { d.arena_epoch = arena_epoch; d.arena_up = 0; }
  if (arena_top > d.arena_up) {
    const u32 r0 = d.arena_up / kArenaW, r1 = (arena_top + kArenaW - 1) / kArenaW;
    glBindTexture(GL_TEXTURE_2D, d.arena);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, static_cast<GLint>(r0), kArenaW, static_cast<GLsizei>(r1 - r0), GL_RED_INTEGER, GL_UNSIGNED_INT, arena.data() + static_cast<size_t>(r0) * kArenaW);
    d.arena_up = arena_top;
  }

  // Fans, placed as lean.vert places them; the prefix first, then the tail in runs.
  d.vb.clear();
  std::vector<std::pair<u32, u32>> runs;   // tail: [first vertex, key] where key = translucent | depth-write << 1
  u32 opaque_end = 0;
  for (u32 k = 0; k < dl.npoly; ++k) {
    const GpuPoly& p = dl.polys[k];
    if (k == f.first_ordered) opaque_end = static_cast<u32>(d.vb.size());
    if (p.nverts < 3) continue;
    if (p.flags & (DS_PF_SHADOW_MASK | DS_PF_SHADOW)) continue;   // shadow volumes: not yet
    if (k >= f.first_ordered) {
      const u32 key = ((p.flags & DS_PF_TRANSLUCENT) ? 1u : 0u) | ((p.attr & (1u << 11)) ? 2u : 0u);
      if (runs.empty() || runs.back().second != key) runs.emplace_back(static_cast<u32>(d.vb.size()), key);
    }
    auto vert = [&](u32 vi) {
      const GpuVert& vt = dl.verts[p.first_vert + vi];
      const float w = std::max(static_cast<float>(vt.w), 1.f);
      const float cx = 0.5f * static_cast<float>(p.xmin + p.xmax);
      const float sxf = static_cast<float>(vt.sx), syf = static_cast<float>(vt.sy);
      float ox = sxf > cx ? 1.f : 0.f, oy = 0.f;
      if (ox > 0.f) {
        const u32 n = p.nverts;
        const float sxp = static_cast<float>(dl.verts[p.first_vert + (vi + n - 1) % n].sx), sxn = static_cast<float>(dl.verts[p.first_vert + (vi + 1) % n].sx);
        if (sxp == sxf || sxn == sxf) ox = 0.f;
      }
      if (p.xmax == p.xmin) ox = (vi == 1 || vi == 2) ? 1.f : 0.f;
      if (p.ybot == p.ytop) oy = (vi == 1 || vi == 2) ? 1.f : 0.f;
      const float x = (sxf + ox) / 256.f * 2.f - 1.f, y = (syf + oy) / 192.f * 2.f - 1.f;
      float z = std::clamp(static_cast<float>(vt.z), 0.f, 16777215.f);
      if (p.attr & 0x4000u) z = std::max(z - (wbuf ? 255.f : 512.f), 0.f);
      const float zc = wbuf ? (1.f / std::max(z, 1.f)) : (z / 16777215.f);
      Vtx o{};
      o.pos[0] = x * w; o.pos[1] = y * w; o.pos[2] = (zc * 2.f - 1.f) * w; o.pos[3] = w;
      o.rgb[0] = static_cast<float>(vt.r); o.rgb[1] = static_cast<float>(vt.g); o.rgb[2] = static_cast<float>(vt.b);
      o.st[0] = static_cast<float>(vt.s); o.st[1] = static_cast<float>(vt.t);
      o.attr = p.attr; o.texparam = p.texparam; o.tex_offset = p.tex_offset; o.tex_wh = p.tex_w | (p.tex_h << 16); o.flags = p.flags;
      return o;
    };
    for (u32 t = 1; t + 1 < p.nverts; ++t) { d.vb.push_back(vert(0)); d.vb.push_back(vert(t)); d.vb.push_back(vert(t + 1)); }
  }
  if (f.first_ordered >= dl.npoly) opaque_end = static_cast<u32>(d.vb.size());
  const u32 total = static_cast<u32>(d.vb.size());

  glBindVertexArray(d.vao);
  glBindBuffer(GL_ARRAY_BUFFER, d.vbo);
  const size_t bytes = std::max<size_t>(d.vb.size(), 1) * sizeof(Vtx);
  if (bytes > d.vbo_cap) { d.vbo_cap = bytes * 2; glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(d.vbo_cap), nullptr, GL_STREAM_DRAW); }
  if (!d.vb.empty()) glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(d.vb.size() * sizeof(Vtx)), d.vb.data());

  // The 3D layer at S x.
  glBindFramebuffer(GL_FRAMEBUFFER, d.fbo3d);
  glViewport(0, 0, W, H);
  glDisable(GL_SCISSOR_TEST);
  const u32 cc = f.clear_color, ca = (cc >> 24) & 0x1F;
  glClearColor(static_cast<float>(cc & 0x3F) / 255.f, static_cast<float>((cc >> 8) & 0x3F) / 255.f, static_cast<float>((cc >> 16) & 0x3F) / 255.f, ca ? static_cast<float>(std::min(255u, (ca + 1) * 8)) / 255.f : 0.f);
  const float cz = static_cast<float>(f.clear_depth & 0xFFFFFF);
  glClearDepthf(wbuf ? 1.f / std::max(cz, 1.f) : std::min(1.f, cz / 16777215.f));
  glDepthMask(GL_TRUE); glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
  glUseProgram(d.raster);
  glUniform1ui(d.u_dispcnt, f.dispcnt);
  glUniform1ui(d.u_alpha_ref, f.alpha_ref);
  glUniform1uiv(d.u_toon, 32, dl.post.toon);
  glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, d.arena);
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(wbuf ? GL_GREATER : GL_LESS);
  glDisable(GL_BLEND);
  if (opaque_end) glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(opaque_end));
  for (size_t r = 0; r < runs.size(); ++r) {
    const u32 v0 = runs[r].first, v1 = r + 1 < runs.size() ? runs[r + 1].first : total;
    const bool trans = runs[r].second & 1, dw = (runs[r].second & 2) != 0;
    if (trans) {
      glEnable(GL_BLEND);
      glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE);
      glBlendEquationSeparate(GL_FUNC_ADD, GL_MAX);
      glDepthMask(dw ? GL_TRUE : GL_FALSE);
    } else { glDisable(GL_BLEND); glDepthMask(GL_TRUE); }
    if (v1 > v0) glDrawArrays(GL_TRIANGLES, static_cast<GLint>(v0), static_cast<GLsizei>(v1 - v0));
  }
  glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDepthMask(GL_TRUE);

  // Engine A's planes for the composite.
  for (u32 y = 0; y < 192; ++y) {
    const gpu::Engine2D::HiresLine& h = lines[y];
    d.regs[y * 2] = static_cast<u32>(h.bldcnt) | (static_cast<u32>(h.eva) << 16) | (static_cast<u32>(h.evb) << 21) | (static_cast<u32>(h.evy) << 26);
    d.regs[y * 2 + 1] = static_cast<u32>(h.mbright) | (h.valid ? 0x80000000u : 0u);
    if (!h.valid) continue;
    std::memcpy(&d.top[y * 256], h.top, 256 * 4);
    std::memcpy(&d.second[y * 256], h.second, 256 * 4);
    std::memcpy(&d.win[y * 256], h.win, 256);
    for (u32 x = 0; x < 256; ++x) d.meta[y * 256 + x] = static_cast<u32>(h.top_id[x]) | (static_cast<u32>(h.top_kind[x]) << 8) | (static_cast<u32>(h.top_alpha[x]) << 16) | (static_cast<u32>(h.second_id[x]) << 24);
  }
  auto up = [](GLuint t, GLsizei w, GLsizei h, GLenum fmt, GLenum type, const void* px) { glBindTexture(GL_TEXTURE_2D, t); glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, fmt, type, px); };
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  up(d.t_top, 256, 192, GL_RED_INTEGER, GL_UNSIGNED_INT, d.top.data());
  up(d.t_second, 256, 192, GL_RED_INTEGER, GL_UNSIGNED_INT, d.second.data());
  up(d.t_meta, 256, 192, GL_RED_INTEGER, GL_UNSIGNED_INT, d.meta.data());
  up(d.t_win, 256, 192, GL_RED_INTEGER, GL_UNSIGNED_BYTE, d.win.data());
  up(d.t_regs, 192, 1, GL_RG_INTEGER, GL_UNSIGNED_INT, d.regs.data());
  up(d.t_fb, 256, 192, GL_RED_INTEGER, GL_UNSIGNED_INT, fb);

  // The composite at S x.
  glBindFramebuffer(GL_FRAMEBUFFER, d.fbo_out);
  glViewport(0, 0, W, H);
  glUseProgram(d.comp);
  const GLuint ts[7] = {d.t_top, d.t_second, d.t_meta, d.t_win, d.t_regs, d.t_fb, d.col3d};
  for (int i = 0; i < 7; ++i) { glActiveTexture(GL_TEXTURE0 + i); glBindTexture(GL_TEXTURE_2D, ts[i]); }
  glActiveTexture(GL_TEXTURE0);
  glBindVertexArray(d.vao_empty);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glBindVertexArray(0);
  // DS_HIRES_PPM=<n>:<path>: the n-th composited frame, read back once (debug).
  static u32 renders = 0;
  ++renders;
  if (static const char* dump = std::getenv("DS_HIRES_PPM"); dump && std::strchr(dump, ':') && renders == static_cast<u32>(std::atoi(dump))) {
    std::vector<u8> px(static_cast<size_t>(W) * H * 4);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    if (std::FILE* o = std::fopen(std::strchr(dump, ':') + 1, "wb")) {
      std::fprintf(o, "P6\n%d %d\n255\n", W, H);
      for (size_t i = 0; i < static_cast<size_t>(W) * H; ++i) std::fwrite(&px[i * 4], 1, 3, o);
      std::fclose(o);
      std::fprintf(stderr, "hires3d: frame %u written to %s\n", renders, std::strchr(dump, ':') + 1);
    }
  }
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  return d.out;
}

} // namespace ds::sdl

#endif // DSPERATE_GLES
