// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
// Small GLES 3.1 helpers shared by the GLES present (gl_present.cpp) and the
// hi-res 3D (gl_hires.cpp). Only built with DSPERATE_GLES.
#include <GLES3/gl31.h>
#include <SDL2/SDL.h>

#include <string>

namespace ds::sdl::gl {

// A linked program from vertex and fragment source, or 0 with the driver's
// log in `why`.
GLuint program(const char* vs, const char* fs, std::string* why);

// An immutable 2D texture, nearest-sampled and clamped.
GLuint texture(GLenum format, GLsizei w, GLsizei h);

// An extension entry point (nullptr when the driver has none).
template <class Fn> Fn proc(const char* name) { return reinterpret_cast<Fn>(SDL_GL_GetProcAddress(name)); }

} // namespace ds::sdl::gl
