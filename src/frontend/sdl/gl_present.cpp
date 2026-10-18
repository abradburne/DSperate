// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "frontend/sdl/gl_present.h"
#include "frontend/sdl/present_thread.h"
#include "frontend/sdl/latency_log.h"

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
#include <cstring>
#include <cstdlib>
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
  GLuint p_fb = 0, p_tex = 0, vao = 0, fbtex[2][3] = {};
  int fb_k[2] = {0, 0};
  std::vector<u32> shown;   // may_skip: the 1x frame this window shows
  u32 frames = 0;           // DS_PRESENT_PPM
  // DS_LATENCY: GL_TIMESTAMP_EXT queries placed before each swap, read back
  // a few swaps later and turned into CLOCK_MONOTONIC (latency_log.h).
  GLuint tsq[4] = {}; u64 tsq_post[4] = {}; u32 tsq_n = 0;
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
  hires_tex_ = 0;
  // DS_PRESENT_PPM=<n>+<count>:<prefix>: what this window shows, frames n..n+count-1,
  // as <prefix>.<window>.<frame>.ppm (debug; reads the back buffer before the swap).
  if (static const char* dump = std::getenv("DS_PRESENT_PPM"); dump && std::strchr(dump, ':')) {
    static const u32 n0 = static_cast<u32>(std::atoi(dump)), cnt = std::strchr(dump, '+') ? static_cast<u32>(std::atoi(std::strchr(dump, '+') + 1)) : 1;
    ++d.frames;
    if (d.frames >= n0 && d.frames < n0 + cnt) {
      std::vector<unsigned char> px(static_cast<size_t>(ww) * wh * 4);
      glReadPixels(0, 0, ww, wh, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
      const std::string path = std::string(std::strchr(dump, ':') + 1) + "." + std::to_string(d.index) + "." + std::to_string(d.frames) + ".ppm";
      if (std::FILE* o = std::fopen(path.c_str(), "wb")) {
        std::fprintf(o, "P6\n%d %d\n255\n", ww, wh);
        for (int y = wh - 1; y >= 0; --y) for (int x = 0; x < ww; ++x) std::fwrite(&px[(static_cast<size_t>(y) * ww + x) * 4], 1, 3, o);
        std::fclose(o);
      }
    }
  }
  if (latlog::on()) gpu_stamp(d);
  SDL_GL_SwapWindow(d.win);
}

void GlPresent::gpu_stamp(Impl& d) {
  constexpr GLenum TIMESTAMP = 0x8E28, RESULT = 0x8866, AVAILABLE = 0x8867;
  using Counter = void (*)(GLuint, GLenum);
  using Get = void (*)(GLuint, GLenum, GLuint64*);
  using GetI = void (*)(GLenum, GLint64*);
  static const Counter counter = reinterpret_cast<Counter>(SDL_GL_GetProcAddress("glQueryCounterEXT"));
  static const Get get = reinterpret_cast<Get>(SDL_GL_GetProcAddress("glGetQueryObjectui64vEXT"));
  static const GetI geti = reinterpret_cast<GetI>(SDL_GL_GetProcAddress("glGetInteger64vEXT"));
  if (!counter || !get || !geti) return;
  if (!d.tsq[0]) glGenQueries(4, d.tsq);
  // The oldest still out, once the GPU has it; its GPU time against the GPU's clock now.
  if (d.tsq_n >= 3) {
    const u32 k = (d.tsq_n - 3) % 4;
    GLuint64 avail = 0, t = 0;
    get(d.tsq[k], AVAILABLE, &avail);
    if (avail) {
      get(d.tsq[k], RESULT, &t);
      GLint64 gnow = 0;
      geti(TIMESTAMP, &gnow);
      const u64 cnow = latlog::now_ns();
      latlog::gpu_done(d.tsq_post[k], lat_screen_, cnow - static_cast<u64>(gnow - static_cast<GLint64>(t)));
    }
  }
  const u32 k = d.tsq_n % 4;
  counter(d.tsq[k], TIMESTAMP);
  d.tsq_post[k] = latlog::current();
  ++d.tsq_n;
}

} // namespace ds::sdl

#endif // DSPERATE_GLES
