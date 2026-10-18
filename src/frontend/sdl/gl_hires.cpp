// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "frontend/sdl/gl_hires.h"

#if !DSPERATE_GLES
namespace ds::sdl {
struct GlHires::Impl {};
GlHires::GlHires() = default;
GlHires::~GlHires() = default;
bool GlHires::init(u32, std::string* why) { if (why) *why = "built without GLES"; return false; }
unsigned GlHires::render(const gpu::Renderer3D::DisplayList&, const std::vector<u32>&, u32, u64, const gpu::Engine2D::HiresLine*, Output) { return 0; }
} // namespace ds::sdl
#else
#include "frontend/sdl/debug_options.h"
#include "frontend/sdl/gl_hires_shaders.h"
#include "frontend/sdl/gl_util.h"

#include <GLES2/gl2ext.h>   // EXT_disjoint_timer_query's GL_TIME_ELAPSED_EXT

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace ds::sdl {

namespace {

using gpu::vk::GpuPoly;
using gpu::vk::GpuVert;
using HiresLine = gpu::Engine2D::HiresLine;

constexpr u32 kArenaW = 4096, kArenaH = (2u << 20) / kArenaW;   // Renderer3D's display arena, 8 MB
constexpr int kSets = 3;      // per-render inputs in a ring (Impl::Set)
constexpr int kOutputs = 6;   // composited pictures in a ring (render())

// One fan vertex, placed as the lean raster places it (vk/shaders/lean.vert).
struct Vtx {
  float pos[4];
  float rgb[3];
  float st[2];
  u32 attr, texparam, tex_offset, tex_wh, flags;
  float dz;   // DS depth (z, or the W-buffer's normalised w), the depth-equal offset applied
};

// What a run of the translucent tail is drawn with. Polygons go into one run
// while these stay the same; each run is a draw (two for a split one).
struct RunKey {
  enum class Kind : u8 { Opaque, Translucent, ShadowMask, Shadow };
  Kind kind = Kind::Opaque;
  bool depth_write = false;   // POLYGON_ATTR bit 11 (translucent depth update)
  // Translucent polygons at alpha 31: their texture decides per texel
  // whether a pixel is opaque, so the run is drawn twice -- its alpha-31
  // pixels as an opaque polygon's, then the rest blended.
  bool split = false;
  bool restart = false;   // a shadow mask after anything else: the marks start again
  u8 stencil_id = 0;      // translucent and shadow: polygon id + 1, the stencil's own-id test
  bool operator==(const RunKey& o) const {
    return kind == o.kind && depth_write == o.depth_write && split == o.split && restart == o.restart && stencil_id == o.stencil_id;
  }
  bool operator!=(const RunKey& o) const { return !(*this == o); }
};

struct Run {
  u32 first;   // first vertex
  u32 mid;     // a split run: the first vertex of its blended part
  RunKey key;
};

// The frame's fans: the opaque prefix (vertices [0, opaque_end)), then the
// tail's runs up to `total`.
struct Fans {
  u32 opaque_end = 0, total = 0;
  std::vector<Run> runs;
};

// Uniform locations, per program.
struct RasterLocs { GLint dispcnt = -1, alpha_ref = -1, toon = -1, wbuf = -1, split = -1, shadow = -1; };
struct PrepassLocs { GLint alpha_ref = -1, wbuf = -1; };
struct ResolveLocs { GLint dispcnt = -1, toon = -1; };
// The composite's edge marking and fog (kLayer3D with EDGE_FOG).
struct LayerLocs { GLint edge = -1, edge_on = -1, fog_on = -1, fog_alpha = -1, fog_color = -1, fog_offset = -1, fog_shift = -1, density = -1; };
struct CompositeProgram { GLuint prog = 0; GLint output = -1; LayerLocs layer; };

// hires-stats: the present thread's CPU time per section of a render,
// printed every 120 renders.
struct CpuTimes {
  enum Section { kArena, kFans, kVertices, kTailCalls, kLines, kCompositeCalls, kCount };
  using clock = std::chrono::steady_clock;
  double ms[kCount] = {};
  u32 renders = 0;
  void add(Section s, clock::time_point a, clock::time_point b) { ms[s] += std::chrono::duration<double, std::milli>(b - a).count(); }
  void report(size_t verts, size_t runs) {
    if (renders < 120) return;
    std::fprintf(stderr, "hires3d cpu ms/render: arena %.2f, fans %.2f, vbo %.2f, raster calls %.2f, planes %.2f, composite calls %.2f (%zu verts, %zu runs)\n",
                 ms[kArena] / renders, ms[kFans] / renders, ms[kVertices] / renders, ms[kTailCalls] / renders, ms[kLines] / renders, ms[kCompositeCalls] / renders, verts, runs);
    for (double& x : ms) x = 0;
    renders = 0;
  }
};

// hires-stages: GPU time per section through EXT_disjoint_timer_query, in a
// ring of four renders whose results are read three renders late (sooner
// would stall). On a tiler a section's time includes work flushed into it,
// so these rank the sections; hires-bench measures them.
class GpuStages {
public:
  enum Section { kPrepass, kResolve, kTail, kEnd3d, kComposite, kLinesUpload, kCount };
  void next_render() {
    if (!made_) {
      made_ = true;
      get_ = gl::proc<GetQuery>("glGetQueryObjectui64vEXT");
      for (auto& r : q_) glGenQueries(kCount, r);
    }
    ++renders_;
    if (renders_ <= 3 || !get_) return;
    const u32 old = (renders_ - 3) % 4;
    for (int k = 0; k < kCount; ++k) { GLuint64 ns = 0; get_(q_[old][k], GL_QUERY_RESULT, &ns); ms_[k] += ns / 1e6; }
    if (++summed_ < 120) return;
    std::fprintf(stderr, "gpu ms: pre %.2f, winners %.2f, tail %.2f, end of 3d %.2f, composite %.2f, planes upload %.2f\n",
                 ms_[kPrepass] / summed_, ms_[kResolve] / summed_, ms_[kTail] / summed_, ms_[kEnd3d] / summed_, ms_[kComposite] / summed_, ms_[kLinesUpload] / summed_);
    for (double& x : ms_) x = 0;
    summed_ = 0;
  }
  // Ends the running section and starts `s` (-1: none).
  void begin(int s) {
    if (running_ >= 0) glEndQuery(GL_TIME_ELAPSED_EXT);
    running_ = s;
    if (s >= 0) glBeginQuery(GL_TIME_ELAPSED_EXT, q_[renders_ % 4][s]);
  }

private:
  using GetQuery = void (*)(GLuint, GLenum, GLuint64*);
  bool made_ = false;
  GetQuery get_ = nullptr;
  GLuint q_[4][kCount] = {};
  u32 renders_ = 0, summed_ = 0;
  double ms_[kCount] = {};
  int running_ = -1;
};

} // namespace

struct GlHires::Impl {
  u32 S = 2;
  GLsizei W = 512, H = 384;

  // Programs.
  GLuint raster = 0, prepass = 0, resolve = 0;
  RasterLocs raster_l;
  PrepassLocs prepass_l;
  ResolveLocs resolve_l;
  CompositeProgram fast[2], full[2];   // [1]: with edge marking and fog (EDGE_FOG)
  // glColorMaski (OES/EXT_draw_buffers_indexed): the id plane's writes
  // switched on and off without changing the draw buffers, which on a tiler
  // ends the batch (a store and a reload of every attachment).
  using ColorMaski = void (*)(GLuint, GLboolean, GLboolean, GLboolean, GLboolean);
  ColorMaski color_maski = nullptr;

  // The 3D layer at S x: colour (raw 6-bit channels, alpha as (a + 1) * 8),
  // the id plane (polygon id | 0x40 fog bit | 0x80 where an opaque polygon
  // drew; edge marking and fog read it) and depth-stencil (stencil: the id +
  // 1 of the translucent polygon that last blended a pixel, 0 where an
  // opaque one drew; bit 7 the shadow masks' marks).
  GLuint fbo3d = 0, col3d = 0, id3d = 0, dep3d = 0;
  // The visibility buffer: per pixel the prefix's winner (id + 1 << 18 |
  // vertex colour) and its texel coordinate, over the same depth-stencil.
  GLuint fbo_pre = 0, g0 = 0, g1 = 0;
  // The polygons' records for the resolve (two uvec4 a polygon), a ring.
  GLuint polys[3] = {};
  int polys_k = 0;
  std::vector<u32> poly_records;
  // The texture arena, as far as uploaded, and of which generation.
  GLuint arena = 0;
  u32 arena_up = 0;
  u64 arena_epoch = ~0ull;
  // Composited pictures, a ring, sharing one depth buffer (pass 1 marks
  // what it drew, so pass 2 shades only the rest).
  GLuint fbo_out[kOutputs] = {}, out[kOutputs] = {}, dep_out = 0;
  int slot = 0;
  GLuint vao_empty = 0;   // full-screen triangles

  // The per-render inputs (engine A's lines, the vertices) in a ring of
  // sets: rewriting the one the GPU still reads made Panfrost shadow it (a
  // new buffer, mapped and faulted, every upload), ~3 ms a frame of kernel
  // time on the present thread. `lines` is written through an
  // unsynchronised map once `fence` (the last composite that read it) is done.
  struct Set { GLuint lines = 0, vao = 0, vbo = 0; GLsync fence = nullptr; size_t vbo_cap = 0; };
  Set sets[kSets];
  int lines_k = 0, verts_k = 0;   // this render's sets: the composite's, the 3D draw's

  // The frame being drawn: its fans, and what the last draw3d left for the composite.
  Fans fans;
  std::vector<Vtx> vb, vb_late;   // the fans; a split run's blended part while it is built
  GLuint layer3d = 0;             // 0: no 3D drawn for the composite
  bool edge_on = false, fog_on = false, fog_alpha = false;
  gpu::vk::GpuPost post{};

  // Debugging (debug_options.h).
  const bool stats = debug::options().hires_stats;
  const bool stages_on = debug::options().hires_stages;
  CpuTimes cpu;
  GpuStages gpu;
  u32 renders = 0, renders_1x_nolist = 0, renders_1x_stale = 0;
  bool benching = false;
  void stage(int s) { if (stages_on) gpu.begin(s); }

  ~Impl() {
    for (Set& st : sets) {
      if (st.lines) glDeleteBuffers(1, &st.lines);
      if (st.fence) glDeleteSync(st.fence);
      if (st.vbo) glDeleteBuffers(1, &st.vbo);
      if (st.vao) glDeleteVertexArrays(1, &st.vao);
    }
    glDeleteTextures(kOutputs, out);
    glDeleteFramebuffers(kOutputs, fbo_out);
    if (dep_out) glDeleteRenderbuffers(1, &dep_out);
    if (arena) glDeleteBuffers(1, &arena);
    glDeleteBuffers(3, polys);
    for (GLuint t : {col3d, id3d, dep3d, g0, g1}) if (t) glDeleteTextures(1, &t);
    for (GLuint f : {fbo3d, fbo_pre}) if (f) glDeleteFramebuffers(1, &f);
    if (vao_empty) glDeleteVertexArrays(1, &vao_empty);
    for (GLuint p : {raster, prepass, resolve, fast[0].prog, fast[1].prog, full[0].prog, full[1].prog}) if (p) glDeleteProgram(p);
  }

  bool build_programs(std::string* why);
  bool build_targets(std::string* why);
  void build_sets();
  // draw3d's stages.
  void upload_arena(const std::vector<u32>& words, u32 top, u64 epoch);
  void build_fans(const gpu::Renderer3D::DisplayList& dl, bool wbuf);
  void upload_vertices();
  void draw_prepass(const gpu::vk::GpuFrame& f, bool wbuf, bool skip);
  void draw_resolve(const gpu::Renderer3D::DisplayList& dl, bool want_id, bool skip);
  void draw_tail(const gpu::Renderer3D::DisplayList& dl, bool wbuf, bool want_id, bool skip);
  // composite's.
  bool upload_lines(const HiresLine* lines, bool skip_full);
  void draw_composite(Output output, bool full_pass, bool skip);
};

GlHires::GlHires() = default;
GlHires::~GlHires() = default;

// ---- setup ----

bool GlHires::Impl::build_programs(std::string* why) {
  using namespace hires_glsl;
  const std::string version = "#version 310 es\n";
  raster = gl::program(kRasterVS, (version + kRasterFS).c_str(), why);
  prepass = gl::program(kRasterVS, (version + "#define PREPASS 1\n" + kRasterFS).c_str(), why);
  resolve = gl::program(kFullVS, (version + "#define RESOLVE 1\n" + kRasterFS).c_str(), why);
  if (!raster || !prepass || !resolve) return false;
  prepass_l = {glGetUniformLocation(prepass, "u_alpha_ref"), glGetUniformLocation(prepass, "u_wbuf")};
  glUseProgram(resolve);
  glUniform1i(glGetUniformLocation(resolve, "u_g0"), 1);
  glUniform1i(glGetUniformLocation(resolve, "u_g1"), 2);
  resolve_l = {glGetUniformLocation(resolve, "u_dispcnt"), glGetUniformLocation(resolve, "u_toon")};
  raster_l = {glGetUniformLocation(raster, "u_dispcnt"), glGetUniformLocation(raster, "u_alpha_ref"), glGetUniformLocation(raster, "u_toon"),
              glGetUniformLocation(raster, "u_wbuf"), glGetUniformLocation(raster, "u_split"), glGetUniformLocation(raster, "u_shadow")};
  glUseProgram(raster);
  glUniform1i(glGetUniformLocation(raster, "u_win"), 3);
  glUniform1i(raster_l.shadow, 0);

  // The composite, compiled for this scale, with and without edge marking and fog.
  const std::string defs = version + "#define SCALE " + std::to_string(S) + "\n#define LINE_WORDS " + std::to_string(sizeof(HiresLine) / 4) + "\n";
  for (int v = 0; v < 2; ++v) {
    const std::string head = defs + (v ? "#define EDGE_FOG 1\n" : "") + kLinePlanes + kLayer3D;
    for (CompositeProgram* cp : {&fast[v], &full[v]}) {
      cp->prog = gl::program(kFullVS, (head + (cp == &fast[v] ? kFastMain : kCompositeMain)).c_str(), why);
      if (!cp->prog) return false;
      cp->output = glGetUniformLocation(cp->prog, "u_mode");
      // Both passes read the same planes, on the same units.
      glUseProgram(cp->prog);
      glUniform1i(glGetUniformLocation(cp->prog, "u_3d"), 0);
      glUniform1i(glGetUniformLocation(cp->prog, "u_id"), 1);
      glUniform1i(glGetUniformLocation(cp->prog, "u_depth"), 2);
      if (!v) continue;
      const GLuint p = cp->prog;
      cp->layer = {glGetUniformLocation(p, "u_edge"), glGetUniformLocation(p, "u_edge_on"), glGetUniformLocation(p, "u_fog_on"), glGetUniformLocation(p, "u_fog_alpha"),
                   glGetUniformLocation(p, "u_fog_color"), glGetUniformLocation(p, "u_fog_offset"), glGetUniformLocation(p, "u_fog_shift"), glGetUniformLocation(p, "u_density")};
    }
  }
  const char* ext = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
  if (ext && (std::strstr(ext, "GL_OES_draw_buffers_indexed") || std::strstr(ext, "GL_EXT_draw_buffers_indexed"))) {
    color_maski = gl::proc<ColorMaski>("glColorMaskiOES");
    if (!color_maski) color_maski = gl::proc<ColorMaski>("glColorMaskiEXT");
  }
  return true;
}

bool GlHires::Impl::build_targets(std::string* why) {
  auto complete = [&](const char* what) {
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) return true;
    if (why) *why = std::string(what) + " framebuffer incomplete";
    return false;
  };
  const GLenum two[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
  glGenBuffers(3, polys);
  for (GLuint b : polys) { glBindBuffer(GL_SHADER_STORAGE_BUFFER, b); glBufferData(GL_SHADER_STORAGE_BUFFER, DS_MAX_POLYS * 32, nullptr, GL_STREAM_DRAW); }
  glGenBuffers(1, &arena);
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, arena);
  glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(kArenaW) * kArenaH * 4, nullptr, GL_STATIC_DRAW);
  // Depth as a float, as the lean raster's: W-buffer frames store 1/w, and at
  // DS depths of tens of thousands neighbouring values are closer than a
  // 24-bit fixed step, so a surface and its decals compared equal (NSMB's
  // world map lost its paths). A texture: edge marking and fog read it.
  col3d = gl::texture(GL_RGBA8, W, H);
  dep3d = gl::texture(GL_DEPTH32F_STENCIL8, W, H);
  id3d = gl::texture(GL_R8UI, W, H);
  glGenFramebuffers(1, &fbo3d);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo3d);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, col3d, 0);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, id3d, 0);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, dep3d, 0);
  glDrawBuffers(2, two);
  if (!complete("3D")) return false;
  g0 = gl::texture(GL_R32UI, W, H);
  g1 = gl::texture(GL_R32UI, W, H);
  glGenFramebuffers(1, &fbo_pre);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo_pre);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g0, 0);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, g1, 0);
  glDrawBuffers(2, two);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, dep3d, 0);
  if (!complete("pre-pass")) return false;
  glGenRenderbuffers(1, &dep_out);
  glBindRenderbuffer(GL_RENDERBUFFER, dep_out);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, W, H);
  for (int i = 0; i < kOutputs; ++i) {
    out[i] = gl::texture(GL_RGBA8, W, H);
    glGenFramebuffers(1, &fbo_out[i]);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_out[i]);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, out[i], 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, dep_out);
    if (!complete("output")) return false;
  }
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  return true;
}

void GlHires::Impl::build_sets() {
  for (Set& st : sets) {
    glGenBuffers(1, &st.lines);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, st.lines);
    glBufferData(GL_SHADER_STORAGE_BUFFER, 192 * sizeof(HiresLine), nullptr, GL_DYNAMIC_DRAW);
  }
  glGenVertexArrays(1, &vao_empty);
  auto at = [](size_t off) { return reinterpret_cast<void*>(off); };
  for (Set& st : sets) {
    glGenVertexArrays(1, &st.vao);
    glBindVertexArray(st.vao);
    glGenBuffers(1, &st.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, st.vbo);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(Vtx), at(offsetof(Vtx, pos)));
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vtx), at(offsetof(Vtx, rgb)));
    glEnableVertexAttribArray(2); glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vtx), at(offsetof(Vtx, st)));
    glEnableVertexAttribArray(3); glVertexAttribIPointer(3, 4, GL_UNSIGNED_INT, sizeof(Vtx), at(offsetof(Vtx, attr)));   // attr .. tex_wh
    glEnableVertexAttribArray(4); glVertexAttribIPointer(4, 1, GL_UNSIGNED_INT, sizeof(Vtx), at(offsetof(Vtx, flags)));
    glEnableVertexAttribArray(5); glVertexAttribPointer(5, 1, GL_FLOAT, GL_FALSE, sizeof(Vtx), at(offsetof(Vtx, dz)));
  }
  glBindVertexArray(0);
}

bool GlHires::init(u32 scale, std::string* why) {
  d_ = std::make_unique<Impl>();
  Impl& d = *d_;
  d.S = scale_ = std::clamp(scale, 2u, 4u);
  d.W = static_cast<GLsizei>(256 * d.S);
  d.H = static_cast<GLsizei>(192 * d.S);
  if (!d.build_programs(why) || !d.build_targets(why)) return false;
  d.build_sets();
  if (glGetError() != GL_NO_ERROR) { if (why) *why = "GL error during setup"; return false; }
  return true;
}

// ---- a render ----

unsigned GlHires::render(const gpu::Renderer3D::DisplayList& dl, const std::vector<u32>& arena, u32 arena_top, u64 arena_epoch,
                         const HiresLine* lines, Output output) {
  if (!d_) return 0;
  Impl& d = *d_;
  if (!d.benching && ++d.renders == debug::options().hires_bench_at) bench(dl, arena, arena_top, arena_epoch, lines, output);
  if (!dl.valid || dl.epoch != arena_epoch || !lines) {
    if (d.stats) {
      d.renders_1x_nolist += !dl.valid;
      d.renders_1x_stale += dl.valid && dl.epoch != arena_epoch;
      std::fprintf(stderr, "hires3d: frame drawn at 1x (%u without a list, %u stale arena)\n", d.renders_1x_nolist, d.renders_1x_stale);
    }
    return 0;
  }
  if (!draw3d(dl, arena, arena_top, arena_epoch)) return 0;
  return composite(lines, output);
}

// hires-bench: the frame drawn 60 times back to back per stage mask, glFinish'd.
void GlHires::bench(const gpu::Renderer3D::DisplayList& dl, const std::vector<u32>& arena, u32 arena_top, u64 arena_epoch,
                    const HiresLine* lines, Output output) {
  Impl& d = *d_;
  d.benching = true;
  for (u32 mask : debug::options().hires_bench_masks) {
    skip_ = mask;
    render(dl, arena, arena_top, arena_epoch, lines, output);
    glFinish();
    const auto t0 = std::chrono::steady_clock::now();
    for (int k = 0; k < 60; ++k) render(dl, arena, arena_top, arena_epoch, lines, output);
    glFinish();
    std::fprintf(stderr, "bench skip %u: %.2f ms a render\n", mask, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 60);
  }
  skip_ = 0;
  d.benching = false;
}

bool GlHires::draw3d(const gpu::Renderer3D::DisplayList& dl, const std::vector<u32>& arena, u32 arena_top, u64 arena_epoch) {
  using clock = CpuTimes::clock;
  Impl& d = *d_;
  d.layer3d = 0;
  d.verts_k = (d.verts_k + 1) % kSets;
  const gpu::vk::GpuFrame& f = dl.frame;
  const bool wbuf = (f.flags & DS_FF_WBUFFER) != 0;
  // The id plane is for edge marking and fog alone: without them, one set of
  // draw buffers for the whole 3D (changing them ends the GPU's batch: a
  // store and a reload of every attachment, per translucent run).
  const bool edge_on = (f.dispcnt & 0x20u) != 0, fog_on = (f.dispcnt & 0x80u) != 0;
  const bool want_id = edge_on || fog_on;

  const auto t0 = clock::now();
  d.upload_arena(arena, arena_top, arena_epoch);
  const auto t1 = clock::now();
  d.build_fans(dl, wbuf);
  const auto t2 = clock::now();
  d.upload_vertices();
  const auto t3 = clock::now();

  if (d.stages_on) d.gpu.next_render();
  d.stage(GpuStages::kPrepass);
  d.draw_prepass(f, wbuf, skip_ & kPrepass);
  d.stage(GpuStages::kResolve);
  d.draw_resolve(dl, want_id, skip_ & kResolve);
  d.stage(GpuStages::kTail);
  d.draw_tail(dl, wbuf, want_id, skip_ & kTail);
  d.stage(GpuStages::kEnd3d);
  glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDepthMask(GL_TRUE); glDisable(GL_STENCIL_TEST);
  if (!want_id) {   // no edge marking or fog: depth, stencil and ids die with the pass (a tiler need not write them out)
    const GLenum dead[2] = {GL_DEPTH_STENCIL_ATTACHMENT, GL_COLOR_ATTACHMENT1};
    glInvalidateFramebuffer(GL_FRAMEBUFFER, 2, dead);
  }
  // Edge marking and fog: in the composite, where it reads the layer.
  d.layer3d = d.col3d;
  d.edge_on = edge_on && !(skip_ & kEdgeFog);
  d.fog_on = fog_on && !(skip_ & kEdgeFog);
  d.fog_alpha = (f.dispcnt & 0x40u) != 0;
  d.post = dl.post;
  d.stage(-1);

  if (d.stats) {
    const auto t4 = clock::now();
    d.cpu.add(CpuTimes::kArena, t0, t1);
    d.cpu.add(CpuTimes::kFans, t1, t2);
    d.cpu.add(CpuTimes::kVertices, t2, t3);
    d.cpu.add(CpuTimes::kTailCalls, t3, t4);
    ++d.cpu.renders;
  }
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  return true;
}

// Textures new since the last frame: the arena is append-only per epoch.
void GlHires::Impl::upload_arena(const std::vector<u32>& words, u32 top, u64 epoch) {
  if (arena_epoch != epoch) { arena_epoch = epoch; arena_up = 0; }
  if (top <= arena_up) return;
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, arena);
  glBufferSubData(GL_SHADER_STORAGE_BUFFER, static_cast<GLintptr>(arena_up) * 4, static_cast<GLsizeiptr>(top - arena_up) * 4, words.data() + arena_up);
  arena_up = top;
}

// The polygons as fans, placed as lean.vert places them: the opaque prefix
// (the pre-pass) in list order, then the tail in list order, in runs.
void GlHires::Impl::build_fans(const gpu::Renderer3D::DisplayList& dl, bool wbuf) {
  vb.clear();
  vb_late.clear();
  fans.runs.clear();
  auto vert = [&](const GpuPoly& p, u32 vi) {
    const GpuVert& vt = dl.verts[p.first_vert + vi];
    const float w = std::max(static_cast<float>(vt.w), 1.f);
    // Vertices where the DS put them: the lean raster's right-edge push (its
    // 1x span rule under MSAA) differs between polygons sharing an edge, and
    // at hi-res that opened cracks inside models. A zero-width or -height
    // polygon (a line to the DS) still gets one pixel.
    float ox = 0.f, oy = 0.f;
    if (p.xmax == p.xmin) ox = (vi == 1 || vi == 2) ? 1.f : 0.f;
    if (p.ybot == p.ytop) oy = (vi == 1 || vi == 2) ? 1.f : 0.f;
    const float x = (static_cast<float>(vt.sx) + ox) / 256.f * 2.f - 1.f, y = (static_cast<float>(vt.sy) + oy) / 192.f * 2.f - 1.f;
    float z = std::clamp(static_cast<float>(vt.z), 0.f, 16777215.f);
    if (p.attr & 0x4000u) z = std::max(z - (wbuf ? 255.f : 512.f), 0.f);   // depth-equal: within the DS's tolerance
    Vtx o{};
    o.pos[0] = x * w; o.pos[1] = y * w; o.pos[2] = 0.f; o.pos[3] = w;   // depth comes from the fragment shader
    o.dz = z;
    o.rgb[0] = static_cast<float>(vt.r); o.rgb[1] = static_cast<float>(vt.g); o.rgb[2] = static_cast<float>(vt.b);
    o.st[0] = static_cast<float>(vt.s); o.st[1] = static_cast<float>(vt.t);
    o.attr = p.attr; o.texparam = p.texparam; o.tex_offset = p.tex_offset; o.tex_wh = p.tex_w | (p.tex_h << 16);
    o.flags = p.flags | ((static_cast<u32>(&p - dl.polys.data()) + 1u) << 12);   // the pre-pass's winner id
    return o;
  };
  auto fan = [&](const GpuPoly& p, std::vector<Vtx>& to) {
    for (u32 t = 1; t + 1 < p.nverts; ++t) { to.push_back(vert(p, 0)); to.push_back(vert(p, t)); to.push_back(vert(p, t + 1)); }
  };
  // The prefix in list order: on a depth tie the DS keeps the earlier
  // polygon, so a reorder (near to far) flickered where polygons meet at
  // equal depth (NSMB's map).
  const u32 prefix = std::min(dl.frame.first_ordered, dl.npoly);
  for (u32 k = 0; k < prefix; ++k) {
    const GpuPoly& p = dl.polys[k];
    if (p.nverts < 3 || (p.flags & (DS_PF_SHADOW_MASK | DS_PF_SHADOW))) continue;
    fan(p, vb);
  }
  fans.opaque_end = static_cast<u32>(vb.size());
  // A split run's blended part is gathered apart and goes after its opaque one.
  auto start_run = [&](const RunKey* key) {
    if (!fans.runs.empty() && fans.runs.back().key.split) {
      fans.runs.back().mid = static_cast<u32>(vb.size());
      vb.insert(vb.end(), vb_late.begin(), vb_late.end());
      vb_late.clear();
    }
    if (key) fans.runs.push_back(Run{static_cast<u32>(vb.size()), 0, *key});
  };
  auto join = [&](const RunKey& key) { if (fans.runs.empty() || fans.runs.back().key != key) start_run(&key); };
  bool last_mask = false;
  for (u32 k = prefix; k < dl.npoly; ++k) {
    const GpuPoly& p = dl.polys[k];
    if (p.nverts < 3) continue;
    const u8 id1 = static_cast<u8>(((p.attr >> 24) & 0x3Fu) + 1u);
    const bool depth_write = (p.attr & (1u << 11)) != 0;
    RunKey key;
    // Shadow volumes: a mask (shadow, id 0) marks where its depth test fails;
    // a shadow (id > 0) draws only there. Consecutive masks add up; the
    // first mask after anything else starts the marks again.
    if (p.flags & DS_PF_SHADOW_MASK) {
      key.kind = RunKey::Kind::ShadowMask;
      key.restart = !last_mask;
      if (key.restart) start_run(&key); else join(key);
      last_mask = true;
      fan(p, vb);
      continue;
    }
    last_mask = false;
    if (p.flags & DS_PF_SHADOW) {
      key.kind = RunKey::Kind::Shadow;
      key.depth_write = depth_write;
      key.stencil_id = id1;
      join(key);
      fan(p, vb);
      continue;
    }
    // Translucent runs also break on the polygon id: the stencil keeps the
    // DS rule that a translucent pixel does not blend over one of its own id.
    const bool tr = (p.flags & DS_PF_TRANSLUCENT) != 0;
    // A translucent polygon at alpha 31 (translucent for its texture's alpha)
    // has opaque pixels too, which the DS draws as an opaque polygon's: they
    // write the depth, so a farther polygon later in the list stays behind.
    key.kind = tr ? RunKey::Kind::Translucent : RunKey::Kind::Opaque;
    key.depth_write = depth_write;
    key.stencil_id = tr ? id1 : 0;
    key.split = tr && ((p.attr >> 16) & 0x1Fu) == 31u;
    join(key);
    if (!key.split) { fan(p, vb); continue; }
    // Each part takes only the polygons that can have such pixels, by their
    // texture's alpha classes. Untextured or decal, the alpha is 31.
    const bool fixed = !(p.flags & DS_PF_TEXTURED) || (((p.attr >> 4) & 1u) != 0);
    if (fixed || (p.flags & DS_PF_TEX_FULL)) fan(p, vb);
    if (!fixed && (p.flags & DS_PF_TEX_PARTIAL)) fan(p, vb_late);
  }
  start_run(nullptr);
  fans.total = static_cast<u32>(vb.size());
}

void GlHires::Impl::upload_vertices() {
  Set& st = sets[verts_k];
  glBindVertexArray(st.vao);
  glBindBuffer(GL_ARRAY_BUFFER, st.vbo);
  const size_t bytes = std::max<size_t>(vb.size(), 1) * sizeof(Vtx);
  if (bytes > st.vbo_cap) { st.vbo_cap = bytes * 2; glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(bytes * 2), nullptr, GL_STREAM_DRAW); }
  if (!vb.empty()) glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(vb.size() * sizeof(Vtx)), vb.data());
}

// The opaque prefix, pass 1: the DS depth test in list order (on a tie the
// earlier polygon keeps the pixel, as on the DS), keeping each pixel's winner.
void GlHires::Impl::draw_prepass(const gpu::vk::GpuFrame& f, bool wbuf, bool skip) {
  const float clear_z = static_cast<float>(f.clear_depth & 0xFFFFFF);
  glViewport(0, 0, W, H);
  glDisable(GL_SCISSOR_TEST);
  glDepthMask(GL_TRUE); glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glDisable(GL_BLEND); glDisable(GL_STENCIL_TEST);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, arena);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo_pre);
  const GLuint none[4] = {0, 0, 0, 0};
  glClearBufferuiv(GL_COLOR, 0, none);
  glClearBufferuiv(GL_COLOR, 1, none);
  glClearBufferfi(GL_DEPTH_STENCIL, 0, std::min(clear_z, 16777215.f) / 16777216.f, 0);
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LESS);
  if (!fans.opaque_end || skip) return;
  glUseProgram(prepass);
  glUniform1ui(prepass_l.alpha_ref, f.alpha_ref);
  glUniform1i(prepass_l.wbuf, wbuf ? 1 : 0);
  glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(fans.opaque_end));
}

// Pass 2, the resolve: each pixel's winner shaded, once, full screen, over
// the rear plane.
void GlHires::Impl::draw_resolve(const gpu::Renderer3D::DisplayList& dl, bool want_id, bool skip) {
  const gpu::vk::GpuFrame& f = dl.frame;
  glBindFramebuffer(GL_FRAMEBUFFER, fbo3d);
  const GLenum bufs[2] = {GL_COLOR_ATTACHMENT0, want_id ? static_cast<GLenum>(GL_COLOR_ATTACHMENT1) : static_cast<GLenum>(GL_NONE)};
  glDrawBuffers(2, bufs);
  // Each attachment its own clear (the id plane is an integer one): the
  // rear plane's colour, and its id and fog bit with no polygon drawn.
  const u32 cc = f.clear_color, ca = (cc >> 24) & 0x1F;
  const GLfloat col[4] = {static_cast<float>(cc & 0x3F) / 255.f, static_cast<float>((cc >> 8) & 0x3F) / 255.f, static_cast<float>((cc >> 16) & 0x3F) / 255.f,
                          ca ? static_cast<float>(std::min(255u, (ca + 1) * 8)) / 255.f : 0.f};
  const GLuint rear_id[4] = {((f.clear_attr >> 24) & 0x3Fu) | ((f.clear_attr >> 9) & 0x40u), 0, 0, 0};
  glClearBufferfv(GL_COLOR, 0, col);
  glClearBufferuiv(GL_COLOR, 1, rear_id);
  if (!fans.opaque_end || skip) return;
  // The polygons' records, into the ring's next buffer.
  polys_k = (polys_k + 1) % 3;
  poly_records.resize(static_cast<size_t>(dl.npoly) * 8);
  for (u32 k = 0; k < dl.npoly; ++k) {
    const GpuPoly& p = dl.polys[k];
    u32* r = &poly_records[k * 8];
    r[0] = p.attr; r[1] = p.texparam; r[2] = p.tex_offset; r[3] = p.tex_w | (p.tex_h << 16); r[4] = p.flags; r[5] = r[6] = r[7] = 0;
  }
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, polys[polys_k]);
  glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, static_cast<GLsizeiptr>(poly_records.size() * 4), poly_records.data());
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, polys[polys_k]);
  glDisable(GL_DEPTH_TEST);
  glUseProgram(resolve);
  glUniform1ui(resolve_l.dispcnt, f.dispcnt);
  glUniform1uiv(resolve_l.toon, 32, dl.post.toon);
  glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, g0);
  glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, g1);
  glActiveTexture(GL_TEXTURE0);
  glBindVertexArray(vao_empty);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glBindVertexArray(sets[verts_k].vao);
  glEnable(GL_DEPTH_TEST);
}

// The translucent tail and the shadow volumes, run by run, in list order.
void GlHires::Impl::draw_tail(const gpu::Renderer3D::DisplayList& dl, bool wbuf, bool want_id, bool skip) {
  const gpu::vk::GpuFrame& f = dl.frame;
  glEnable(GL_STENCIL_TEST);
  glStencilMask(0x7F);
  glStencilFunc(GL_ALWAYS, 0, 0x7F);
  glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
  // The shader declares the polygons' records (binding 1) for shadows' own-id
  // test: something must be bound there even in a frame with no opaque
  // prefix, where the resolve did not bind them (Panfrost reads every
  // declared buffer's address at each draw, and crashed on none).
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, polys[polys_k]);
  glUseProgram(raster);
  glUniform1ui(raster_l.dispcnt, f.dispcnt);
  glUniform1ui(raster_l.alpha_ref, f.alpha_ref);
  glUniform1uiv(raster_l.toon, 32, dl.post.toon);
  glUniform1i(raster_l.wbuf, wbuf ? 1 : 0);
  glUniform1i(raster_l.split, 0);
  // The id plane takes opaque pixels only (a translucent one keeps the
  // opaque id under it, and its fog bit); switched only when it changes.
  bool id_on = want_id;
  auto id_writes = [&](bool on) {
    if (!want_id || id_on == on) return;
    id_on = on;
    if (color_maski) color_maski(1, on, on, on, on);
    else { const GLenum b[2] = {GL_COLOR_ATTACHMENT0, on ? static_cast<GLenum>(GL_COLOR_ATTACHMENT1) : static_cast<GLenum>(GL_NONE)}; glDrawBuffers(2, b); }
  };
  bool win_bound = false;   // a shadow's own-id test reads the visibility buffer
  const std::vector<Run>& runs = fans.runs;
  for (size_t r = 0; r < runs.size() && !skip; ++r) {
    const Run& run = runs[r];
    const RunKey& key = run.key;
    const u32 v0 = run.first, v1 = r + 1 < runs.size() ? runs[r + 1].first : fans.total;
    if (v1 <= v0) continue;
    if (key.kind == RunKey::Kind::ShadowMask) {   // the stencil mark where the depth test fails, nothing drawn
      glStencilMask(0x80);
      if (key.restart) { const GLint zero = 0; glClearBufferiv(GL_STENCIL, 0, &zero); }
      glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
      glDisable(GL_BLEND); glDepthMask(GL_FALSE);
      glStencilFunc(GL_ALWAYS, 0x80, 0x80);
      glStencilOp(GL_KEEP, GL_REPLACE, GL_KEEP);
      glDrawArrays(GL_TRIANGLES, static_cast<GLint>(v0), static_cast<GLsizei>(v1 - v0));
      glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
      glStencilMask(0x7F);
      glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
      if (color_maski && want_id && !id_on) color_maski(1, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);   // glColorMask set every buffer's
      continue;
    }
    const bool shadow = key.kind == RunKey::Kind::Shadow;
    const bool blended = key.kind == RunKey::Kind::Translucent || shadow;
    const GLint sref = key.stencil_id;
    if (shadow) {
      if (!win_bound) {
        glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D, g0); glActiveTexture(GL_TEXTURE0);
        win_bound = true;
      }
      glUniform1i(raster_l.shadow, fans.opaque_end ? 1 : 0);
    }
    u32 b0 = v0;   // where the run's blended draw starts
    if (key.split) {   // its opaque pixels first, as an opaque polygon's
      id_writes(true);
      glStencilFunc(GL_ALWAYS, 0, 0x7F); glDisable(GL_BLEND); glDepthMask(GL_TRUE);
      glUniform1i(raster_l.split, 1);
      if (run.mid > v0) glDrawArrays(GL_TRIANGLES, static_cast<GLint>(v0), static_cast<GLsizei>(run.mid - v0));
      glUniform1i(raster_l.split, 2);
      b0 = run.mid;
    }
    id_writes(!blended);
    if (shadow) glStencilFunc(GL_EQUAL, 0x80 | sref, 0x80);   // only where a mask marked; writes its id where it draws
    else if (blended) glStencilFunc(GL_NOTEQUAL, sref, 0x7F);  // not over its own id's blend; writes its id where it draws
    else glStencilFunc(GL_ALWAYS, 0, 0x7F);                    // an opaque pixel: any id may blend over it again
    if (blended) {
      glEnable(GL_BLEND);
      glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE);
      glBlendEquationSeparate(GL_FUNC_ADD, GL_MAX);
      glDepthMask(key.depth_write ? GL_TRUE : GL_FALSE);
    } else {
      glDisable(GL_BLEND);
      glDepthMask(GL_TRUE);
    }
    if (v1 > b0) glDrawArrays(GL_TRIANGLES, static_cast<GLint>(b0), static_cast<GLsizei>(v1 - b0));
    glUniform1i(raster_l.split, 0);
    if (shadow) glUniform1i(raster_l.shadow, 0);
  }
  id_writes(true);   // as the next render's clear expects
}

unsigned GlHires::composite(const HiresLine* lines, Output output) {
  using clock = CpuTimes::clock;
  Impl& d = *d_;
  if (!d.layer3d) return 0;
  d.lines_k = (d.lines_k + 1) % kSets;
  d.stage(GpuStages::kLinesUpload);
  const auto t0 = clock::now();
  const bool full = d.upload_lines(lines, skip_ & kComposite);
  d.stage(-1);
  const auto t1 = clock::now();
  d.stage(GpuStages::kComposite);
  d.draw_composite(output, full, skip_ & kComposite);
  d.stage(-1);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  if (d.stats) {
    d.cpu.add(CpuTimes::kLines, t0, t1);
    d.cpu.add(CpuTimes::kCompositeCalls, t1, clock::now());
    d.cpu.report(d.vb.size(), d.fans.runs.size());
  }
  return d.out[d.slot];
}

// Engine A's lines for the composite, copied as they lie (kLinePlanes reads
// them): records and classes, and the registers after them. The full
// composite's planes (top, second, ids, kind, alpha, window) only when a
// line takes it: a class 2 pixel, or master brightness over a class 1 one.
// Returns whether any line does.
bool GlHires::Impl::upload_lines(const HiresLine* lines, bool skip_full) {
  static_assert(sizeof(HiresLine) % 4 == 0 && offsetof(HiresLine, out) == 0 && offsetof(HiresLine, under) == 1024 && offsetof(HiresLine, top) == 2048 &&
                offsetof(HiresLine, second) == 3072 && offsetof(HiresLine, top_id) == 4096 && offsetof(HiresLine, top_kind) == 4352 &&
                offsetof(HiresLine, top_alpha) == 4608 && offsetof(HiresLine, second_id) == 4864 && offsetof(HiresLine, win) == 5120 &&
                offsetof(HiresLine, cls) == 5376 && offsetof(HiresLine, bldcnt) == 5632 && offsetof(HiresLine, mbright) == 5634 &&
                offsetof(HiresLine, eva) == 5636 && offsetof(HiresLine, evb) == 5637 && offsetof(HiresLine, evy) == 5638 && offsetof(HiresLine, valid) == 5639,
                "kLinePlanes' layout");
  bool full = false;
  for (u32 y = 0; y < 192; ++y) full |= lines[y].valid && lines[y].full;
  if (skip_full) full = false;
  Set& st = sets[lines_k];
  if (st.fence) { glClientWaitSync(st.fence, GL_SYNC_FLUSH_COMMANDS_BIT, 100'000'000); glDeleteSync(st.fence); st.fence = nullptr; }
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, st.lines);
  if (u8* m = static_cast<u8*>(glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, 192 * sizeof(HiresLine), GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT))) {
    if (full) std::memcpy(m, lines, 192 * sizeof(HiresLine));
    else for (u32 y = 0; y < 192; ++y) {
      u8* l = m + y * sizeof(HiresLine);
      const u8* src = reinterpret_cast<const u8*>(&lines[y]);
      std::memcpy(l, src, offsetof(HiresLine, top));   // out, under
      std::memcpy(l + offsetof(HiresLine, cls), src + offsetof(HiresLine, cls), offsetof(HiresLine, valid) + 1 - offsetof(HiresLine, cls));   // classes, registers
    }
    glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
  }
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, st.lines);
  return full;
}

// The composite at S x, into the ring's next picture. Pass 1 writes depth 0
// where it drew (the full-screen triangle's z is 0.5; ALWAYS writes it),
// cleared 1 where it discarded; pass 2 runs only where depth is still 1
// (LESS against 0.5 fails elsewhere: early-Z skips those pixels).
void GlHires::Impl::draw_composite(Output output, bool full_pass, bool skip) {
  slot = (slot + 1) % kOutputs;
  glBindFramebuffer(GL_FRAMEBUFFER, fbo_out[slot]);
  glViewport(0, 0, W, H);
  glBindVertexArray(vao_empty);
  glClearDepthf(1.f);
  glClear(GL_DEPTH_BUFFER_BIT);
  glEnable(GL_DEPTH_TEST); glDepthFunc(GL_ALWAYS); glDepthMask(GL_TRUE);
  glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, dep3d);
  glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, id3d);
  glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, layer3d);
  const int v = edge_on || fog_on ? 1 : 0;
  auto layer_uniforms = [&](const LayerLocs& l) {
    if (!v) return;
    glUniform1i(l.edge_on, edge_on ? 1 : 0);
    glUniform1i(l.fog_on, fog_on ? 1 : 0);
    glUniform1uiv(l.edge, 8, post.edge);
    glUniform1i(l.fog_alpha, fog_alpha ? 1 : 0);
    glUniform1ui(l.fog_color, post.fog_color);
    glUniform1ui(l.fog_offset, post.fog_offset);
    glUniform1ui(l.fog_shift, post.fog_shift);
    glUniform1uiv(l.density, 34, post.density);
  };
  glUseProgram(fast[v].prog);
  glUniform1i(fast[v].output, output);
  layer_uniforms(fast[v].layer);
  if (!skip) glDrawArrays(GL_TRIANGLES, 0, 3);
  if (full_pass) {
    glDepthFunc(GL_LESS);
    glDepthMask(GL_FALSE);
    glUseProgram(full[v].prog);
    glUniform1i(full[v].output, output);
    layer_uniforms(full[v].layer);
    glDrawArrays(GL_TRIANGLES, 0, 3);
  }
  const GLenum dead = GL_DEPTH_ATTACHMENT;   // the composite's depth: only within it
  glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, &dead);
  glDisable(GL_DEPTH_TEST);
  glDepthMask(GL_TRUE);
  glBindVertexArray(0);
  sets[lines_k].fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
}

} // namespace ds::sdl

#endif // DSPERATE_GLES
