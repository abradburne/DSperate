// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "cheevos/cheevos_png.h"

#include <cstdlib>
#include <cstring>

#include "core/cart/miniz/miniz_tinfl.h"

namespace ds::cheevos {
namespace {

u32 rd32(const u8* p) { return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | p[3]; }

// Inflates a whole zlib stream (header and adler32 included) into `out`.
bool inflate_zlib(const std::vector<u8>& in, std::vector<u8>& out, size_t want) {
  out.assign(want, 0);
  tinfl_decompressor d;
  tinfl_init(&d);
  size_t in_bytes = in.size(), out_bytes = want;
  const tinfl_status st = tinfl_decompress(&d, in.data(), &in_bytes, out.data(), out.data(), &out_bytes,
                                           TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
  return st == TINFL_STATUS_DONE && out_bytes == want;
}

int paeth(int a, int b, int c) {
  const int p = a + b - c;
  const int pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
  if (pa <= pb && pa <= pc) return a;
  return pb <= pc ? b : c;
}

} // namespace

bool decode_png(const u8* data, size_t size, int& w, int& h, std::vector<u32>& argb, std::string& err) {
  static const u8 kSig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  err.clear();
  argb.clear();
  if (size < 8 + 25 || std::memcmp(data, kSig, 8) != 0) { err = "not a PNG"; return false; }

  u32 width = 0, height = 0;
  int depth = 0, ctype = 0, interlace = 0;
  bool have_ihdr = false;
  std::vector<u8> idat, plte;
  std::vector<u8> trns;
  size_t pos = 8;
  while (pos + 12 <= size) {
    const u32 len = rd32(data + pos);
    const u8* type = data + pos + 4;
    const u8* body = data + pos + 8;
    if (len > size - pos - 12) { err = "chunk runs past the end"; return false; }
    if (std::memcmp(type, "IHDR", 4) == 0) {
      if (len != 13) { err = "bad IHDR"; return false; }
      width = rd32(body);
      height = rd32(body + 4);
      depth = body[8];
      ctype = body[9];
      interlace = body[12];
      have_ihdr = true;
    } else if (std::memcmp(type, "PLTE", 4) == 0) {
      plte.assign(body, body + len);
    } else if (std::memcmp(type, "tRNS", 4) == 0) {
      trns.assign(body, body + len);
    } else if (std::memcmp(type, "IDAT", 4) == 0) {
      idat.insert(idat.end(), body, body + len);
    } else if (std::memcmp(type, "IEND", 4) == 0) {
      break;
    }
    pos += 12 + len;   // the CRC is not checked: a corrupt badge decodes wrong, not unsafely
  }
  if (!have_ihdr) { err = "no IHDR"; return false; }
  if (interlace != 0) { err = "interlaced PNG"; return false; }
  if (width == 0 || height == 0 || width > 4096 || height > 4096) { err = "bad size"; return false; }

  int channels = 0;
  switch (ctype) {
    case 0: channels = 1; break;   // grey
    case 2: channels = 3; break;   // RGB
    case 3: channels = 1; break;   // palette
    case 4: channels = 2; break;   // grey + alpha
    case 6: channels = 4; break;   // RGBA
    default: err = "unknown colour type"; return false;
  }
  const bool sub_byte = depth == 1 || depth == 2 || depth == 4;
  if (!(depth == 8 || (sub_byte && (ctype == 0 || ctype == 3)))) { err = "unsupported bit depth"; return false; }
  if (ctype == 3 && plte.empty()) { err = "palette image without PLTE"; return false; }

  const size_t bits_per_px = static_cast<size_t>(channels) * depth;
  const size_t stride = (static_cast<size_t>(width) * bits_per_px + 7) / 8;
  const size_t bpp = (bits_per_px + 7) / 8;   // bytes per pixel for the filters, at least 1
  const size_t raw_len = (stride + 1) * height;
  std::vector<u8> raw;
  if (!inflate_zlib(idat, raw, raw_len)) { err = "inflate failed"; return false; }

  // Unfilter in place, each row against the one above.
  std::vector<u8> prev(stride, 0);
  std::vector<u8> cur(stride);
  argb.resize(static_cast<size_t>(width) * height);
  for (u32 y = 0; y < height; ++y) {
    const u8* row = raw.data() + static_cast<size_t>(y) * (stride + 1);
    const u8 filter = row[0];
    const u8* src = row + 1;
    for (size_t i = 0; i < stride; ++i) {
      const int a = i >= bpp ? cur[i - bpp] : 0;
      const int b = prev[i];
      const int c = i >= bpp ? prev[i - bpp] : 0;
      int v = src[i];
      switch (filter) {
        case 0: break;
        case 1: v += a; break;
        case 2: v += b; break;
        case 3: v += (a + b) / 2; break;
        case 4: v += paeth(a, b, c); break;
        default: err = "bad filter"; return false;
      }
      cur[i] = static_cast<u8>(v);
    }
    // Pixels out of the unfiltered row.
    u32* out = argb.data() + static_cast<size_t>(y) * width;
    for (u32 x = 0; x < width; ++x) {
      u32 r = 0, g = 0, b = 0, a = 255;
      if (sub_byte) {
        const size_t bit = static_cast<size_t>(x) * depth;
        const int v = (cur[bit / 8] >> (8 - depth - bit % 8)) & ((1 << depth) - 1);
        if (ctype == 3) {
          const size_t i = static_cast<size_t>(v) * 3;
          if (i + 2 < plte.size()) { r = plte[i]; g = plte[i + 1]; b = plte[i + 2]; }
          if (static_cast<size_t>(v) < trns.size()) a = trns[v];
        } else {
          r = g = b = static_cast<u32>(v * 255 / ((1 << depth) - 1));
          if (trns.size() >= 2 && v == ((trns[0] << 8) | trns[1])) a = 0;
        }
      } else {
        const u8* p = cur.data() + static_cast<size_t>(x) * channels;
        switch (ctype) {
          case 0:
            r = g = b = p[0];
            if (trns.size() >= 2 && p[0] == ((trns[0] << 8) | trns[1])) a = 0;
            break;
          case 2:
            r = p[0]; g = p[1]; b = p[2];
            if (trns.size() >= 6 && p[0] == ((trns[0] << 8) | trns[1]) && p[1] == ((trns[2] << 8) | trns[3]) &&
                p[2] == ((trns[4] << 8) | trns[5]))
              a = 0;
            break;
          case 3: {
            const size_t i = static_cast<size_t>(p[0]) * 3;
            if (i + 2 < plte.size()) { r = plte[i]; g = plte[i + 1]; b = plte[i + 2]; }
            if (p[0] < trns.size()) a = trns[p[0]];
            break;
          }
          case 4: r = g = b = p[0]; a = p[1]; break;
          case 6: r = p[0]; g = p[1]; b = p[2]; a = p[3]; break;
        }
      }
      out[x] = (a << 24) | (r << 16) | (g << 8) | b;
    }
    prev.swap(cur);
  }
  w = static_cast<int>(width);
  h = static_cast<int>(height);
  return true;
}

} // namespace ds::cheevos
