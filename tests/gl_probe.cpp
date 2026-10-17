// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// gl_probe DUMP [iterations] [scale] [out.ppm]: draws a recorded frame
// (headless --dump-gpu-frame, vk_dump.h) through OpenGL ES 3.1 at 256S x 192S
// over and over and reports the cost -- the same measurement vk_probe makes of
// the Vulkan raster, to compare the two drivers on one device. Polygons are
// expanded to triangle fans on the CPU with the lean raster's vertex placement
// and depth (vk/shaders/lean.vert); the opaque prefix draws with the depth
// test, the tail in list order with the DS alpha blend's factors; the result
// is read back as the Vulkan path copies its record to the host. No stencil
// rules, fog or shadows: the cost of the bulk of the frame, not exactness.
// GLP_SHADER=const draws a constant colour instead of the DS shading.
// Not in the CMake build (it links EGL and GLESv2 directly):
//   g++ -O2 -std=c++17 -Isrc tests/gl_probe.cpp -o gl_probe -lEGL -lGLESv2
#include "core/gpu/vk/vk_dump.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl31.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace ds;
using namespace ds::gpu::vk;

namespace {

struct Vtx {
  float pos[4];                 // clip space
  float rgb[3];                 // 9-bit vertex colour
  float st[2];                  // 12.4 texcoords
  u32 attr, texparam, tex_offset, tex_wh, flags;   // the polygon's, flat
};

const char* kVS = R"(#version 310 es
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

const char* kFSConst = R"(#version 310 es
precision highp float; precision highp int;
in vec3 v_rgb; in vec2 v_st; flat in uvec4 v_poly; flat in uint v_flags;
out vec4 o_col;
void main() { o_col = vec4(0.1, 0.2, 0.3, 1.0); }
)";

// ds_shade.glsl's shade_pixel, with the texel arena in a 2D R32UI texture (4096 wide).
const char* kFSDs = R"(#version 310 es
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
  uint ab = a == 0u ? 0u : min(255u, (a + 1u) * 8u);
  o_col = vec4(float(r), float(g), float(b), float(ab)) / 255.0;
}
)";

GLuint compile(GLenum type, const char* src) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, nullptr);
  glCompileShader(s);
  GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) { char log[4096]; glGetShaderInfoLog(s, sizeof log, nullptr, log); std::fprintf(stderr, "shader: %s\n", log); std::exit(1); }
  return s;
}

} // namespace

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "usage: gl_probe DUMP [iterations] [scale] [out.ppm]\n"); return 2; }
  const int iters = argc > 2 ? std::atoi(argv[2]) : 200;
  const u32 S = argc > 3 ? static_cast<u32>(std::clamp(std::atoi(argv[3]), 1, 4)) : 1u;
  const char* ppm = argc > 4 ? argv[4] : nullptr;
  const bool constant = std::getenv("GLP_SHADER") && !std::strcmp(std::getenv("GLP_SHADER"), "const");

  std::FILE* f = std::fopen(argv[1], "rb");
  if (!f) { std::fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
  GpuDumpHeader h;
  if (std::fread(&h, sizeof h, 1, f) != 1 || h.magic != GpuDumpHeader::kMagic) { std::fprintf(stderr, "not a gpu frame dump\n"); return 1; }
  std::vector<GpuPoly> gp(h.npoly); std::vector<GpuVert> gv(h.nvert); std::vector<u32> tex(h.ntexels);
  auto rd = [&](void* p, size_t n) { return n == 0 || std::fread(p, n, 1, f) == 1; };
  if (!rd(gp.data(), sizeof(GpuPoly) * gp.size()) || !rd(gv.data(), sizeof(GpuVert) * gv.size()) || !rd(tex.data(), sizeof(u32) * tex.size())) { std::fprintf(stderr, "short dump\n"); return 1; }
  std::fclose(f);
  const bool wbuf = (h.f.flags & DS_FF_WBUFFER) != 0;
  std::printf("frame %llu: %u polygons (%u opaque prefix), %u vertices, %u texel words%s, %ux, shader %s\n", (unsigned long long)h.frame, h.npoly,
              h.f.first_ordered, h.nvert, h.ntexels, wbuf ? ", W-buffer" : "", S, constant ? "const" : "ds");

  // EGL, surfaceless, ES 3.1.
  auto get_display = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
  EGLDisplay dpy = get_display ? get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr) : EGL_NO_DISPLAY;
  if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, nullptr, nullptr)) { std::fprintf(stderr, "no EGL display\n"); return 1; }
  eglBindAPI(EGL_OPENGL_ES_API);
  const EGLint cattr[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 1, EGL_NONE};
  EGLContext ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, cattr);
  if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) { std::fprintf(stderr, "no ES 3.1 context\n"); return 1; }
  std::printf("GL: %s | %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));

  const GLsizei W = static_cast<GLsizei>(256 * S), H = static_cast<GLsizei>(192 * S);
  GLuint fbo, col, dep;
  glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glGenRenderbuffers(1, &col); glBindRenderbuffer(GL_RENDERBUFFER, col); glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, W, H);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, col);
  glGenRenderbuffers(1, &dep); glBindRenderbuffer(GL_RENDERBUFFER, dep); glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, W, H);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, dep);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { std::fprintf(stderr, "framebuffer incomplete\n"); return 1; }
  glViewport(0, 0, W, H);

  // Texel arena as a 4096-wide R32UI texture.
  const u32 trows = std::max<u32>(1, (h.ntexels + 4095) / 4096);
  tex.resize(static_cast<size_t>(trows) * 4096, 0);
  GLuint ttex; glGenTextures(1, &ttex); glBindTexture(GL_TEXTURE_2D, ttex);
  glTexStorage2D(GL_TEXTURE_2D, 1, GL_R32UI, 4096, static_cast<GLsizei>(trows));
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

  GLuint prog = glCreateProgram();
  glAttachShader(prog, compile(GL_VERTEX_SHADER, kVS));
  glAttachShader(prog, compile(GL_FRAGMENT_SHADER, constant ? kFSConst : kFSDs));
  glLinkProgram(prog);
  GLint ok = 0; glGetProgramiv(prog, GL_LINK_STATUS, &ok);
  if (!ok) { char log[4096]; glGetProgramInfoLog(prog, sizeof log, nullptr, log); std::fprintf(stderr, "link: %s\n", log); return 1; }
  glUseProgram(prog);
  if (!constant) {
    glUniform1i(glGetUniformLocation(prog, "u_texels"), 0);
    glUniform1ui(glGetUniformLocation(prog, "u_dispcnt"), h.f.dispcnt);
    glUniform1ui(glGetUniformLocation(prog, "u_alpha_ref"), h.f.alpha_ref);
    glUniform1uiv(glGetUniformLocation(prog, "u_toon"), 32, h.post.toon);
  }

  // Fans on the CPU, placed as lean.vert places them.
  std::vector<Vtx> vb; vb.reserve(static_cast<size_t>(h.nvert) * 3);
  u32 opaque_verts = 0;
  for (u32 k = 0; k < h.npoly; ++k) {
    const GpuPoly& p = gp[k];
    if (k == h.f.first_ordered) opaque_verts = static_cast<u32>(vb.size());
    if (p.nverts < 3) continue;
    auto vert = [&](u32 vi) {
      const GpuVert& vt = gv[p.first_vert + vi];
      const float w = std::max(static_cast<float>(vt.w), 1.f);
      const float cx = 0.5f * static_cast<float>(p.xmin + p.xmax);
      const float sxf = static_cast<float>(vt.sx), syf = static_cast<float>(vt.sy);
      float ox = sxf > cx ? 1.f : 0.f, oy = 0.f;
      if (ox > 0.f) {
        const u32 n = p.nverts;
        const float sxp = static_cast<float>(gv[p.first_vert + (vi + n - 1) % n].sx), sxn = static_cast<float>(gv[p.first_vert + (vi + 1) % n].sx);
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
    for (u32 t = 1; t + 1 < p.nverts; ++t) { vb.push_back(vert(0)); vb.push_back(vert(t)); vb.push_back(vert(t + 1)); }
  }
  if (h.f.first_ordered >= h.npoly) opaque_verts = static_cast<u32>(vb.size());
  std::printf("%zu triangles (%u in the opaque prefix)\n", vb.size() / 3, opaque_verts / 3);

  GLuint vbo, vao;
  glGenVertexArrays(1, &vao); glBindVertexArray(vao);
  glGenBuffers(1, &vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo);
  glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vb.size() * sizeof(Vtx)), nullptr, GL_STREAM_DRAW);
  glEnableVertexAttribArray(0); glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(Vtx), reinterpret_cast<void*>(offsetof(Vtx, pos)));
  glEnableVertexAttribArray(1); glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vtx), reinterpret_cast<void*>(offsetof(Vtx, rgb)));
  glEnableVertexAttribArray(2); glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vtx), reinterpret_cast<void*>(offsetof(Vtx, st)));
  glEnableVertexAttribArray(3); glVertexAttribIPointer(3, 4, GL_UNSIGNED_INT, sizeof(Vtx), reinterpret_cast<void*>(offsetof(Vtx, attr)));
  glEnableVertexAttribArray(4); glVertexAttribIPointer(4, 1, GL_UNSIGNED_INT, sizeof(Vtx), reinterpret_cast<void*>(offsetof(Vtx, flags)));

  const u32 cc = h.f.clear_color, ca = (cc >> 24) & 0x1F;
  const float cz = static_cast<float>(h.f.clear_depth & 0xFFFFFF);
  const float clear_depth = wbuf ? 1.f / std::max(cz, 1.f) : std::min(1.f, cz / 16777215.f);
  std::vector<u8> readback(static_cast<size_t>(W) * H * 4);
  const bool no_read = std::getenv("GLP_NOREAD") != nullptr;

  auto frame = [&]() {
    // Per frame, as the emulator would: new vertices and the arena's texels.
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(vb.size() * sizeof(Vtx)), vb.data());
    glBindTexture(GL_TEXTURE_2D, ttex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 4096, static_cast<GLsizei>(trows), GL_RED_INTEGER, GL_UNSIGNED_INT, tex.data());
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glClearColor(static_cast<float>(cc & 0x3F) / 255.f, static_cast<float>((cc >> 8) & 0x3F) / 255.f, static_cast<float>((cc >> 16) & 0x3F) / 255.f, ca ? static_cast<float>(std::min(255u, (ca + 1) * 8)) / 255.f : 0.f);
    glClearDepthf(clear_depth);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(wbuf ? GL_GREATER : GL_LESS);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    if (opaque_verts) glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(opaque_verts));
    if (vb.size() > opaque_verts) {
      glEnable(GL_BLEND);
      glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE);
      glBlendEquationSeparate(GL_FUNC_ADD, GL_MAX);
      glDepthMask(GL_FALSE);
      glDrawArrays(GL_TRIANGLES, static_cast<GLint>(opaque_verts), static_cast<GLsizei>(vb.size() - opaque_verts));
    }
    if (no_read) glFinish();
    else glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, readback.data());
  };

  frame();   // warm-up: shader variants, first allocations
  std::vector<double> ms; ms.reserve(iters);
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < iters; ++i) {
    const auto a = std::chrono::steady_clock::now();
    frame();
    ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count());
  }
  const double total = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  std::sort(ms.begin(), ms.end());
  auto q = [&](double p) { return ms[std::min(ms.size() - 1, static_cast<size_t>(p * ms.size()))]; };
  std::printf("draw+%s per frame: median %.2f ms, p90 %.2f, p99 %.2f, max %.2f (%d frames, %.1f ms total)\n", no_read ? "finish" : "readback",
              q(0.5), q(0.9), q(0.99), ms.back(), iters, total);
  if (ppm) {
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, readback.data());
    std::FILE* o = std::fopen(ppm, "wb");
    if (!o) { std::fprintf(stderr, "cannot write %s\n", ppm); return 1; }
    std::fprintf(o, "P6\n%d %d\n255\n", W, H);
    for (GLsizei y = 0; y < H; ++y) for (GLsizei x = 0; x < W; ++x) {   // GL's origin is the bottom left, clip y up: the DS's row 0 is GL's row 0
      const u8* p = &readback[(static_cast<size_t>(y) * W + x) * 4];
      const unsigned char px[3] = {static_cast<unsigned char>(p[0] * 255 / 63), static_cast<unsigned char>(p[1] * 255 / 63), static_cast<unsigned char>(p[2] * 255 / 63)};
      std::fwrite(px, 1, 3, o);
    }
    std::fclose(o);
    std::printf("wrote %s\n", ppm);
  }
  return 0;
}
