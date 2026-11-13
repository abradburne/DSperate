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
void GlPresent::set_overlay(int, const u8*, int, int, int, int) {}
bool GlPresent::drawable_size(int&, int&) const { return false; }
} // namespace ds::sdl
#else
#include "frontend/sdl/debug_options.h"
#include "frontend/sdl/gl_util.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
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
// The LCD shaders (frontend/video/lcd_shader.h, after GBA DS): the screen
// nearest-scaled as above, brightened by u_gain (every channel alike, at
// most until the brightest reaches 255, so the hue stays: GBA DS's ccGain),
// times the view's mask, one ordinary texture read a pixel. The mask is one
// cell at a whole-number scale, repeated by the sampler (GL_REPEAT), else the
// whole view; u_ms is how many of it fit across and down the view, so v_uv *
// u_ms lands on each mask texel's centre (nearest). On the Mali-G52 an
// integer-texture fetch with a modulo per pixel (the first version) cost
// ~3.8 ms a frame at 4x on both screens; this is a plain RGBA8 read.
const char* kLcdCommon = R"(
uniform mediump sampler2D u_mask;
uniform highp vec2 u_ms;
uniform float u_gain;
vec3 unpack8(uint c) { return vec3(float((c >> 16) & 255u), float((c >> 8) & 255u), float(c & 255u)); }
// The colour in mediump (the Mali's 16-bit floats, twice the rate; 8-bit
// colours need no more), the mask's coordinate in highp (whole pixels to 1024).
vec4 lcd(vec3 c) {   // c: 0..255
  c *= min(u_gain, 255.0 / max(max(c.r, max(c.g, c.b)), 1.0));
  return vec4(c * (1.0 / 255.0) * texture(u_mask, v_uv * u_ms).rgb, 1.0);
}
)";
const std::string kFbLcdFS = std::string(R"(#version 310 es
precision mediump float; precision highp int; precision highp usampler2D; precision mediump sampler2D;
in highp vec2 v_uv;
uniform highp usampler2D u_fb;
out vec4 o_col;
)") + kLcdCommon + R"(
void main() { o_col = lcd(unpack8(texelFetch(u_fb, ivec2(min(v_uv * vec2(256.0, 192.0), vec2(255.0, 191.0))), 0).r)); }
)";
// The same over the hi-res screen (GlHires' texture, nearest here).
const std::string kTexLcdFS = std::string(R"(#version 310 es
precision mediump float; precision highp int; precision highp usampler2D; precision mediump sampler2D;
in highp vec2 v_uv;
uniform highp sampler2D u_tex;
out vec4 o_col;
)") + kLcdCommon + R"(
void main() { o_col = lcd(texture(u_tex, v_uv).rgb * 255.0); }
)";
// The overlay keeps its alpha, for blending over the screens.
const char* kOverlayFS = R"(#version 310 es
precision highp float;
in vec2 v_uv;
uniform highp sampler2D u_tex;
out vec4 o_col;
void main() { o_col = texture(u_tex, v_uv); }
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
  GLuint pic_tex = 0; int pic_w = 0, pic_h = 0;   // draw_picture's, made on first use
  // set_overlay's: what the frame loop handed over (under ov_mu, since a
  // present may run on the present thread while the loop goes on), and the
  // texture it is uploaded to at the next present.
  std::mutex ov_mu;
  struct Overlay {
    std::vector<u8> px; int w = 0, h = 0, x = 0, y = 0; bool fresh = false;   // under ov_mu
    GLuint tex = 0; int tw = 0, th = 0, tx = 0, ty = 0; bool on = false;      // the present thread's
  } ov[kOverlays];
  GLuint p_ov = 0;
  // The LCD shaders' programs (0: not built, the views go plain), their
  // uniforms, and each view's mask, made when the view's size, the look or
  // its strength changes (a layout change, a reopen).
  GLuint p_fb_lcd = 0, p_tex_lcd = 0;
  GLint fb_lcd_m = -1, fb_lcd_gain = -1, tex_lcd_m = -1, tex_lcd_gain = -1;
  struct LcdMaskTex { GLuint tex = 0; int w = 0, h = 0, mw = 0, mh = 0; frontend::LcdShader s = frontend::LcdShader::Off; double strength = -1; };
  LcdMaskTex lcd_mask[2];
  int fb_k[2] = {0, 0};
  std::vector<u32> shown;   // may_skip: the 1x frame this window shows
  u32 presents = 0;         // present-ppm
  // DS_DEBUG=latency: GL_TIMESTAMP_EXT queries placed before each swap, read
  // back a few swaps later and turned into CLOCK_MONOTONIC (latency_log.h).
  GLuint tsq[4] = {}; u64 tsq_post[4] = {}; u32 tsq_n = 0;
  int index = 0;            // the window's order of opening
  int shown_screen = -1;
};

namespace {
// View i's LCD shader mask at w x h, made when the view's size, the look or
// its strength has changed; null: the view is drawn plain (no look, bilinear,
// no program). On the present thread, the context current. A whole-number
// scale builds one cell (4 x 4 words at the DS+'s 4x); any other, the
// view's every pixel once, 16 samples each, a few tens of ms.
const GlPresent::Impl::LcdMaskTex* lcd_mask(GlPresent::Impl& d, int i, int w, int h, frontend::LcdShader s, double strength) {
  if (d.linear || !d.p_fb_lcd || !frontend::lcd_shader_drawn(s, strength)) return nullptr;
  GlPresent::Impl::LcdMaskTex& m = d.lcd_mask[i];
  if (m.tex && m.w == w && m.h == h && m.s == s && m.strength == strength) return &m;
  frontend::LcdMask mask;
  frontend::lcd_shader_mask(s, strength, w, h, 256, 192, mask);
  if (mask.px.empty()) return nullptr;
  if (m.tex) glDeleteTextures(1, &m.tex);
  // 0x00RRGGBB words to RGBA8 bytes, the sampler repeating it over the view.
  std::vector<u8> rgba(mask.px.size() * 4);
  for (size_t k = 0; k < mask.px.size(); ++k) {
    const u32 c = mask.px[k];
    rgba[k * 4 + 0] = static_cast<u8>(c >> 16); rgba[k * 4 + 1] = static_cast<u8>(c >> 8);
    rgba[k * 4 + 2] = static_cast<u8>(c); rgba[k * 4 + 3] = 255;
  }
  m.tex = gl::texture(GL_RGBA8, mask.mw, mask.mh);
  glBindTexture(GL_TEXTURE_2D, m.tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, mask.mw, mask.mh, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
  m.w = w; m.h = h; m.mw = mask.mw; m.mh = mask.mh; m.s = s; m.strength = strength;
  return &m;
}
} // namespace

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
  d.p_fb = prog(kFbFS); d.p_tex = prog(kTexFS); d.p_ov = prog(kOverlayFS);
  glUseProgram(d.p_fb); glUniform1i(glGetUniformLocation(d.p_fb, "u_fb"), 0);
  glUseProgram(d.p_tex); glUniform1i(glGetUniformLocation(d.p_tex, "u_tex"), 0);
  if (d.p_ov) { glUseProgram(d.p_ov); glUniform1i(glGetUniformLocation(d.p_ov, "u_tex"), 0); }
  // The LCD shaders: without them (a driver that won't build them) the views are drawn plain.
  d.p_fb_lcd = prog(kFbLcdFS.c_str()); d.p_tex_lcd = prog(kTexLcdFS.c_str());
  if (d.p_fb_lcd) {
    glUseProgram(d.p_fb_lcd);
    glUniform1i(glGetUniformLocation(d.p_fb_lcd, "u_fb"), 0); glUniform1i(glGetUniformLocation(d.p_fb_lcd, "u_mask"), 1);
    d.fb_lcd_m = glGetUniformLocation(d.p_fb_lcd, "u_ms"); d.fb_lcd_gain = glGetUniformLocation(d.p_fb_lcd, "u_gain");
  }
  if (d.p_tex_lcd) {
    glUseProgram(d.p_tex_lcd);
    glUniform1i(glGetUniformLocation(d.p_tex_lcd, "u_tex"), 0); glUniform1i(glGetUniformLocation(d.p_tex_lcd, "u_mask"), 1);
    d.tex_lcd_m = glGetUniformLocation(d.p_tex_lcd, "u_ms"); d.tex_lcd_gain = glGetUniformLocation(d.p_tex_lcd, "u_gain");
  }
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
    glDeleteTextures(6, &d.fbtex[0][0]); if (d.pic_tex) glDeleteTextures(1, &d.pic_tex); glDeleteVertexArrays(1, &d.vao); glDeleteProgram(d.p_fb); glDeleteProgram(d.p_tex);
    for (Impl::Overlay& o : d.ov) if (o.tex) glDeleteTextures(1, &o.tex);
    if (d.p_ov) glDeleteProgram(d.p_ov);
    for (Impl::LcdMaskTex& m : d.lcd_mask) if (m.tex) glDeleteTextures(1, &m.tex);
    if (d.p_fb_lcd) glDeleteProgram(d.p_fb_lcd);
    if (d.p_tex_lcd) glDeleteProgram(d.p_tex_lcd);
    if (--g_contexts == 0) g_first = nullptr;
    SDL_GL_DeleteContext(d.ctx);
  }
  d_.reset();
}

void GlPresent::make_current() { if (d_ && d_->ctx) SDL_GL_MakeCurrent(d_->win, d_->ctx); }

void GlPresent::set_overlay(int slot, const u8* rgba, int w, int h, int x, int y) {
  if (!d_ || slot < 0 || slot >= kOverlays) return;
  Impl& d = *d_;
  const std::lock_guard<std::mutex> lk(d.ov_mu);
  Impl::Overlay& o = d.ov[slot];
  if (rgba && w > 0 && h > 0) {
    o.px.assign(rgba, rgba + static_cast<size_t>(w) * h * 4);
    o.w = w; o.h = h; o.x = x; o.y = y;
  } else {
    o.px.clear(); o.w = o.h = 0;
  }
  o.fresh = true;
}

bool GlPresent::drawable_size(int& w, int& h) const {
  if (!d_ || !d_->win) return false;
  SDL_GL_GetDrawableSize(d_->win, &w, &h);
  return w > 0 && h > 0;
}

void GlPresent::draw(const frontend::View* views, int nviews, const u32* const fb[2]) {
  if (!d_) return;
  PresentThread& pt = PresentThread::get();
  pt.wait(pt.post([&] { draw_now(views, nviews, fb); }));
}

void GlPresent::draw_now(const frontend::View* views, int nviews, const u32* const fb[2]) {
  if (!d_) return;
  Impl& d = *d_;
  // The overlay handed over since the last present, if any: a change to it
  // is a change to the picture, so no skipping it.
  bool ov_changed = false;
  {
    const std::lock_guard<std::mutex> lk(d.ov_mu);
    for (const Impl::Overlay& o : d.ov) ov_changed = ov_changed || o.fresh;
  }
  if (may_skip_ && !ov_changed && nviews == 1 && views[0].shown && !(hires_tex_ && views[0].screen == hires_screen_)) {
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
  bool lcd_any = false;
  for (int i = 0; i < nviews; ++i) {
    const frontend::View& v = views[i];
    if (!v.shown) continue;
    glViewport(v.rect.x, wh - v.rect.y - v.rect.h, v.rect.w, v.rect.h);
    const bool hires = hires_tex_ && v.screen == hires_screen_;
    // The LCD shader over a view at least the screen's size (smaller, a PiP
    // inset or a dominant layout's second screen, has no room for its cells).
    const Impl::LcdMaskTex* mask = i < 2 && v.rect.w >= 256 && v.rect.h >= 192 ? lcd_mask(d, i, v.rect.w, v.rect.h, lcd_, lcd_strength_) : nullptr;
    const GLuint lp = mask ? (hires ? d.p_tex_lcd : d.p_fb_lcd) : 0;
    if (lp) {
      lcd_any = true;
      glUseProgram(lp);
      glUniform2f(hires ? d.tex_lcd_m : d.fb_lcd_m, static_cast<float>(v.rect.w) / static_cast<float>(mask->mw), static_cast<float>(v.rect.h) / static_cast<float>(mask->mh));
      glUniform1f(hires ? d.tex_lcd_gain : d.fb_lcd_gain, static_cast<float>(frontend::lcd_shader_gain(lcd_)) / 100.0f);
      glActiveTexture(GL_TEXTURE1);
      glBindTexture(GL_TEXTURE_2D, mask->tex);
      glActiveTexture(GL_TEXTURE0);
    }
    if (hires) {
      if (!lp) glUseProgram(d.p_tex);
      glBindTexture(GL_TEXTURE_2D, hires_tex_);
      const GLint flt = d.linear ? GL_LINEAR : GL_NEAREST;
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, flt); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, flt);
    } else {
      if (!lp) glUseProgram(d.p_fb);
      // A ring per screen: rewriting the texture the GPU still reads made Panfrost shadow it.
      const GLuint t = d.fbtex[v.screen][d.fb_k[v.screen]]; d.fb_k[v.screen] = (d.fb_k[v.screen] + 1) % 3;
      glBindTexture(GL_TEXTURE_2D, t);
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 192, GL_RED_INTEGER, GL_UNSIGNED_INT, fb[v.screen]);
    }
    glDrawArrays(GL_TRIANGLES, 0, 3);
  }
  if (ov_changed && d.p_ov) {
    const std::lock_guard<std::mutex> lk(d.ov_mu);
    for (Impl::Overlay& o : d.ov) {
      if (!o.fresh) continue;
      o.fresh = false;
      o.on = !o.px.empty();
      if (!o.on) continue;
      if (!o.tex || o.tw != o.w || o.th != o.h) {
        if (o.tex) glDeleteTextures(1, &o.tex);
        glGenTextures(1, &o.tex); glBindTexture(GL_TEXTURE_2D, o.tex);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, o.w, o.h);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        o.tw = o.w; o.th = o.h;
      }
      o.tx = o.x; o.ty = o.y;
      glBindTexture(GL_TEXTURE_2D, o.tex);
      glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, o.w, o.h, GL_RGBA, GL_UNSIGNED_BYTE, o.px.data());
    }
  }
  bool overlaid = false;
  for (const Impl::Overlay& o : d.ov) {
    if (!o.on || !o.tex) continue;
    // Its own pixels, 1:1, blended over what the screens left.
    overlaid = true;
    glViewport(o.tx, wh - o.ty - o.th, o.tw, o.th);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
    glUseProgram(d.p_ov);
    glBindTexture(GL_TEXTURE_2D, o.tex);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDisable(GL_BLEND);
  }
  glBindVertexArray(0);
  const int hires_shown = hires_tex_ ? hires_screen_ : -1;
  hires_tex_ = 0;
  // A window one 1x screen fills exactly must show it scaled by nearest neighbour.
  const bool filled = !overlaid && !lcd_any && nviews == 1 && views[0].shown && hires_shown != views[0].screen && views[0].rect.x == 0 && views[0].rect.y == 0 &&
                      views[0].rect.w == ww && views[0].rect.h == wh;
  hash_present(d.index, ww, wh, filled ? fb[views[0].screen] : nullptr);
  dump_present(d.index, ++d.presents, ww, wh);
  if (latlog::on()) gpu_stamp(d);
  SDL_GL_SwapWindow(d.win);
}

void GlPresent::draw_picture(const u8* rgba, int w, int h) {
  if (!d_ || !rgba || w <= 0 || h <= 0) return;
  PresentThread& pt = PresentThread::get();
  pt.wait(pt.post([&] {
    Impl& d = *d_;
    SDL_GL_MakeCurrent(d.win, d.ctx);
    if (!d.pic_tex || d.pic_w != w || d.pic_h != h) {
      if (d.pic_tex) glDeleteTextures(1, &d.pic_tex);
      glGenTextures(1, &d.pic_tex); glBindTexture(GL_TEXTURE_2D, d.pic_tex);
      glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, w, h);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      d.pic_w = w; d.pic_h = h;
    }
    glBindTexture(GL_TEXTURE_2D, d.pic_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    int ww = 0, wh = 0;
    SDL_GL_GetDrawableSize(d.win, &ww, &wh);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST);
    glViewport(0, 0, ww, wh);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
    // The picture's shape kept, centred: the panel's own size fills it exactly.
    const float s = std::min(static_cast<float>(ww) / static_cast<float>(w), static_cast<float>(wh) / static_cast<float>(h));
    const int vw = static_cast<int>(static_cast<float>(w) * s), vh = static_cast<int>(static_cast<float>(h) * s);
    glViewport((ww - vw) / 2, (wh - vh) / 2, vw, vh);
    glUseProgram(d.p_tex);
    glBindVertexArray(d.vao);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, d.pic_tex);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    d.shown_screen = -1;   // whatever may_skip remembered is gone from the window
    dump_present(d.index, ++d.presents, ww, wh);
    SDL_GL_SwapWindow(d.win);
  }));
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
