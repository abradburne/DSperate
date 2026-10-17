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
#include <GLES3/gl31.h>

#include <cstdio>

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
  auto sh = [](GLenum t, const char* src) {
    GLuint s = glCreateShader(t); glShaderSource(s, 1, &src, nullptr); glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[1024]; glGetShaderInfoLog(s, sizeof log, nullptr, log); std::fprintf(stderr, "gl present: %s\n", log); }
    return s;
  };
  GLuint p = glCreateProgram();
  GLuint v = sh(GL_VERTEX_SHADER, kVS), f = sh(GL_FRAGMENT_SHADER, fs);
  glAttachShader(p, v); glAttachShader(p, f); glLinkProgram(p); glDeleteShader(v); glDeleteShader(f);
  return p;
}
} // namespace

struct GlPresent::Impl {
  SDL_Window* win = nullptr;
  SDL_GLContext ctx = nullptr;
  bool linear = false;
  GLuint p_fb = 0, p_tex = 0, vao = 0, fbtex[2] = {0, 0};
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
  ++g_contexts;
  SDL_GL_MakeCurrent(win, d.ctx);
  SDL_GL_SetSwapInterval(vsync ? 1 : 0);
  d.p_fb = prog(kFbFS); d.p_tex = prog(kTexFS);
  glUseProgram(d.p_fb); glUniform1i(glGetUniformLocation(d.p_fb, "u_fb"), 0);
  glUseProgram(d.p_tex); glUniform1i(glGetUniformLocation(d.p_tex, "u_tex"), 0);
  glGenVertexArrays(1, &d.vao);
  for (GLuint& t : d.fbtex) {
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
    glDeleteTextures(2, d.fbtex); glDeleteVertexArrays(1, &d.vao); glDeleteProgram(d.p_fb); glDeleteProgram(d.p_tex);
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
      glBindTexture(GL_TEXTURE_2D, d.fbtex[v.screen]);
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 192, GL_RED_INTEGER, GL_UNSIGNED_INT, fb[v.screen]);
    }
    glDrawArrays(GL_TRIANGLES, 0, 3);
  }
  glBindVertexArray(0);
  hires_tex_ = 0;
  SDL_GL_SwapWindow(d.win);
}

} // namespace ds::sdl

#endif // DSPERATE_GLES
