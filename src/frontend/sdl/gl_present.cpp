// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "frontend/sdl/gl_present.h"
#include "frontend/sdl/present_thread.h"

#if !DSPERATE_GLES
namespace ds::sdl {
struct GlPresent::Impl {};
GlPresent::GlPresent() = default;
GlPresent::~GlPresent() = default;
bool GlPresent::open(SDL_Window*, bool, bool) { return false; }
void GlPresent::close() {}
void GlPresent::make_current() {}
void GlPresent::draw(const frontend::View*, int, const u32* const[2]) {}
void GlPresent::draw_now(const frontend::View*, int, const u32* const[2]) {}
} // namespace ds::sdl
#else
#include "frontend/sdl/debug_options.h"
#include "frontend/sdl/gl_util.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace ds::sdl {

namespace {
SDL_GLContext g_first = nullptr;   // the context group every window joins
int g_contexts = 0;

const char* kVS = R"(#version 310 es
out vec2 v_uv;
void main() {
  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
  v_uv = vec2(p.x, 1.0 - p.y);   // texture row 0 is the top line
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";
// A 1x screen: 0xAARRGGBB words, nearest.
const char* kFbFS = R"(#version 310 es
precision highp float; precision highp usampler2D;
in vec2 v_uv;
uniform highp usampler2D u_fb;
out vec4 o_col;
void main() {
  uint c = texelFetch(u_fb, ivec2(min(v_uv * vec2(256.0, 192.0), vec2(255.0, 191.0))), 0).r;
  o_col = vec4(float((c >> 16) & 255u), float((c >> 8) & 255u), float(c & 255u), 255.0) / 255.0;
}
)";
const char* kTexFS = R"(#version 310 es
precision highp float;
in vec2 v_uv;
uniform highp sampler2D u_tex;
out vec4 o_col;
void main() { o_col = vec4(texture(u_tex, v_uv).rgb, 1.0); }
)";

GLuint prog(const char* fs) {
  std::string why;
  const GLuint p = gl::program(kVS, fs, &why);
  if (!p) std::fprintf(stderr, "gl present: %s\n", why.c_str());
  return p;
}

// present-hash (debug_options.h): a line per present of a window -- the
// window, its present count, an FNV-1a hash of the picture (a word a pixel,
// as glReadPixels gives it) and, for a window one 1x screen fills exactly,
// the hash of that screen scaled up by nearest neighbour, which the picture
// must equal (0 otherwise). Compares two builds' output frame by frame.
void hash_present(int window, int ww, int wh, const u32* filled_by) {
  static std::FILE* const out = [] { const std::string& p = debug::options().present_hash; return p.empty() ? nullptr : std::fopen(p.c_str(), "w"); }();
  if (!out) return;
  static std::vector<unsigned char> px;
  static u32 presents[8] = {};
  px.resize(static_cast<size_t>(ww) * wh * 4);
  glReadPixels(0, 0, ww, wh, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
  u64 h = 0xcbf29ce484222325ull;
  for (size_t i = 0; i < px.size(); i += 4) { u32 w; std::memcpy(&w, &px[i], 4); h = (h ^ w) * 0x100000001b3ull; }
  u64 e = 0;
  if (filled_by) {
    e = 0xcbf29ce484222325ull;
    for (int y = wh - 1; y >= 0; --y) for (int x = 0; x < ww; ++x) {   // glReadPixels' order: bottom row first
      const u32 c = filled_by[(y * 192 / wh) * 256 + x * 256 / ww];
      e = (e ^ (((c >> 16) & 0xFF) | (c & 0xFF00) | ((c & 0xFF) << 16) | 0xFF000000u)) * 0x100000001b3ull;
    }
  }
  std::fprintf(out, "%d %u %016llx %016llx\n", window, ++presents[window & 7], static_cast<unsigned long long>(h), static_cast<unsigned long long>(e));
  std::fflush(out);
}

// present-ppm: what a window shows at its presents N..N+COUNT-1, as
// <prefix>.<window>.<present>.ppm (the back buffer, before the swap).
void dump_present(int window, u32 present, int ww, int wh) {
  const debug::Options& o = debug::options();
  if (o.present_ppm_prefix.empty() || present < o.present_ppm_from || present >= o.present_ppm_from + o.present_ppm_count) return;
  std::vector<unsigned char> px(static_cast<size_t>(ww) * wh * 4);
  glReadPixels(0, 0, ww, wh, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
  const std::string path = o.present_ppm_prefix + "." + std::to_string(window) + "." + std::to_string(present) + ".ppm";
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return;
  std::fprintf(f, "P6\n%d %d\n255\n", ww, wh);
  for (int y = wh - 1; y >= 0; --y) for (int x = 0; x < ww; ++x) std::fwrite(&px[(static_cast<size_t>(y) * ww + x) * 4], 1, 3, f);
  std::fclose(f);
}
} // namespace

struct GlPresent::Impl {
  SDL_Window* win = nullptr;
  SDL_GLContext ctx = nullptr;
  bool linear = false;
  GLuint p_fb = 0, p_tex = 0, vao = 0, fbtex[2][3] = {};
  int fb_k[2] = {0, 0};
  std::vector<u32> shown;   // may_skip: the 1x frame this window shows
  u32 presents = 0;         // present-ppm
  int index = 0;            // the window's order of opening
  int shown_screen = -1;
};

GlPresent::GlPresent() = default;
GlPresent::~GlPresent() { close(); }

bool GlPresent::open(SDL_Window* win, bool vsync, bool linear) {
  d_ = std::make_unique<Impl>();
  Impl& d = *d_;
  d.win = win; d.linear = linear;
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
  if (g_first) { SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1); SDL_GL_MakeCurrent(SDL_GL_GetCurrentWindow() ? SDL_GL_GetCurrentWindow() : win, g_first); }
  d.ctx = SDL_GL_CreateContext(win);
  SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);
  if (!d.ctx) { std::fprintf(stderr, "gl present: no GLES 3.1 context (%s)\n", SDL_GetError()); d_.reset(); return false; }
  if (!g_first) g_first = d.ctx;
  d.index = g_contexts;
  ++g_contexts;
  SDL_GL_MakeCurrent(win, d.ctx);
  SDL_GL_SetSwapInterval(vsync ? 1 : 0);
  d.p_fb = prog(kFbFS); d.p_tex = prog(kTexFS);
  glUseProgram(d.p_fb); glUniform1i(glGetUniformLocation(d.p_fb, "u_fb"), 0);
  glUseProgram(d.p_tex); glUniform1i(glGetUniformLocation(d.p_tex, "u_tex"), 0);
  glGenVertexArrays(1, &d.vao);
  for (auto& ring : d.fbtex) for (GLuint& t : ring) {
    glGenTextures(1, &t); glBindTexture(GL_TEXTURE_2D, t);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_R32UI, 256, 192);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  }
  std::fprintf(stderr, "video: GLES present, %s | %s, vsync %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION), vsync ? "on" : "off");
  const bool ok = glGetError() == GL_NO_ERROR;
  SDL_GL_MakeCurrent(win, nullptr);   // from here on the present thread's
  return ok;
}

void GlPresent::close() {
  if (!d_) return;
  Impl& d = *d_;
  PresentThread& pt = PresentThread::get();
  pt.wait(pt.post([&] { if (d.ctx) SDL_GL_MakeCurrent(d.win, nullptr); }));   // released there, taken here to tear down
  if (d.ctx) {
    SDL_GL_MakeCurrent(d.win, d.ctx);
    glDeleteTextures(6, &d.fbtex[0][0]); glDeleteVertexArrays(1, &d.vao); glDeleteProgram(d.p_fb); glDeleteProgram(d.p_tex);
    if (--g_contexts == 0) g_first = nullptr;
    SDL_GL_DeleteContext(d.ctx);
  }
  d_.reset();
}

void GlPresent::make_current() { if (d_ && d_->ctx) SDL_GL_MakeCurrent(d_->win, d_->ctx); }

void GlPresent::draw(const frontend::View* views, int nviews, const u32* const fb[2]) {
  if (!d_) return;
  PresentThread& pt = PresentThread::get();
  pt.wait(pt.post([&] { draw_now(views, nviews, fb); }));
}

void GlPresent::draw_now(const frontend::View* views, int nviews, const u32* const fb[2]) {
  if (!d_) return;
  Impl& d = *d_;
  if (may_skip_ && nviews == 1 && views[0].shown && !(hires_tex_ && views[0].screen == hires_screen_)) {
    const u32* px = fb[views[0].screen];
    if (d.shown_screen == views[0].screen && d.shown.size() == 256 * 192 && std::memcmp(d.shown.data(), px, 256 * 192 * 4) == 0) { hires_tex_ = 0; return; }
    d.shown.assign(px, px + 256 * 192); d.shown_screen = views[0].screen;
  } else d.shown_screen = -1;
  SDL_GL_MakeCurrent(d.win, d.ctx);
  int ww = 0, wh = 0;
  SDL_GL_GetDrawableSize(d.win, &ww, &wh);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glViewport(0, 0, ww, wh);
  glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST);
  glClearColor(0.f, 0.f, 0.f, 1.f);
  glClear(GL_COLOR_BUFFER_BIT);
  glBindVertexArray(d.vao);
  glActiveTexture(GL_TEXTURE0);
  for (int i = 0; i < nviews; ++i) {
    const frontend::View& v = views[i];
    if (!v.shown) continue;
    glViewport(v.rect.x, wh - v.rect.y - v.rect.h, v.rect.w, v.rect.h);
    if (hires_tex_ && v.screen == hires_screen_) {
      glUseProgram(d.p_tex);
      glBindTexture(GL_TEXTURE_2D, hires_tex_);
      const GLint flt = d.linear ? GL_LINEAR : GL_NEAREST;
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, flt); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, flt);
    } else {
      glUseProgram(d.p_fb);
      // A ring per screen: rewriting the texture the GPU still reads made Panfrost shadow it.
      const GLuint t = d.fbtex[v.screen][d.fb_k[v.screen]]; d.fb_k[v.screen] = (d.fb_k[v.screen] + 1) % 3;
      glBindTexture(GL_TEXTURE_2D, t);
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 192, GL_RED_INTEGER, GL_UNSIGNED_INT, fb[v.screen]);
    }
    glDrawArrays(GL_TRIANGLES, 0, 3);
  }
  glBindVertexArray(0);
  const int hires_shown = hires_tex_ ? hires_screen_ : -1;
  hires_tex_ = 0;
  // A window one 1x screen fills exactly must show it scaled by nearest neighbour.
  const bool filled = nviews == 1 && views[0].shown && hires_shown != views[0].screen && views[0].rect.x == 0 && views[0].rect.y == 0 &&
                      views[0].rect.w == ww && views[0].rect.h == wh;
  hash_present(d.index, ww, wh, filled ? fb[views[0].screen] : nullptr);
  dump_present(d.index, ++d.presents, ww, wh);
  SDL_GL_SwapWindow(d.win);
}

} // namespace ds::sdl

#endif // DSPERATE_GLES
