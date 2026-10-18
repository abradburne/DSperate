// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#if DSPERATE_GLES
#include "frontend/sdl/gl_util.h"

namespace ds::sdl::gl {

namespace {
GLuint shader(GLenum type, const char* src, std::string* why) {
  const GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, nullptr);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (ok) return s;
  char log[2048];
  glGetShaderInfoLog(s, sizeof log, nullptr, log);
  if (why) *why = log;
  glDeleteShader(s);
  return 0;
}
} // namespace

GLuint program(const char* vs, const char* fs, std::string* why) {
  const GLuint v = shader(GL_VERTEX_SHADER, vs, why), f = shader(GL_FRAGMENT_SHADER, fs, why);
  if (!v || !f) { glDeleteShader(v); glDeleteShader(f); return 0; }
  const GLuint p = glCreateProgram();
  glAttachShader(p, v);
  glAttachShader(p, f);
  glLinkProgram(p);
  glDeleteShader(v);
  glDeleteShader(f);
  GLint ok = 0;
  glGetProgramiv(p, GL_LINK_STATUS, &ok);
  if (ok) return p;
  char log[2048];
  glGetProgramInfoLog(p, sizeof log, nullptr, log);
  if (why) *why = log;
  glDeleteProgram(p);
  return 0;
}

GLuint texture(GLenum format, GLsizei w, GLsizei h) {
  GLuint t = 0;
  glGenTextures(1, &t);
  glBindTexture(GL_TEXTURE_2D, t);
  glTexStorage2D(GL_TEXTURE_2D, 1, format, w, h);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  return t;
}

} // namespace ds::sdl::gl

#endif // DSPERATE_GLES
