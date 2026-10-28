// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// The badge PNG decoder: hand-made images (zlib-compressed with Python's zlib
// when this test was written) covering the colour types, sub-byte depths and
// every filter; interlace refused.
#include <string>
#include <vector>

#include "check.h"
#include "cheevos/cheevos_png.h"

namespace {
static const unsigned char kRgba[] = {137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,0,2,0,0,0,2,8,6,0,0,0,114,182,13,36,0,0,0,23,73,68,65,84,120,156,99,248,207,192,240,31,8,27,24,129,244,127,46,17,121,6,0,59,135,5,186,203,253,97,46,0,0,0,0,73,69,78,68,174,66,96,130};
static const unsigned char kPal[] = {137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,0,4,0,0,0,1,2,3,0,0,0,132,82,231,94,0,0,0,12,80,76,84,69,255,0,0,0,255,0,0,0,255,9,9,9,92,113,126,134,0,0,0,2,116,82,78,83,255,128,8,15,179,106,0,0,0,10,73,68,65,84,120,156,99,144,6,0,0,29,0,28,142,244,245,33,0,0,0,0,73,69,78,68,174,66,96,130};
static const unsigned char kGrey[] = {137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,0,3,0,0,0,2,8,0,0,0,0,184,31,57,198,0,0,0,16,73,68,65,84,120,156,99,226,18,145,99,86,22,17,1,0,2,29,0,141,137,208,129,234,0,0,0,0,73,69,78,68,174,66,96,130};
static const unsigned char kRgb[] = {137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,0,2,0,0,0,1,8,2,0,0,0,123,64,232,221,0,0,0,15,73,68,65,84,120,156,99,97,100,98,102,97,101,3,0,0,91,0,26,252,119,183,236,0,0,0,0,73,69,78,68,174,66,96,130};
static const unsigned char kInterlaced[] = {137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,0,1,0,0,0,1,8,0,0,0,1,77,121,171,195,0,0,0,10,73,68,65,84,120,156,99,96,7,0,0,9,0,8,32,35,195,140,0,0,0,0,73,69,78,68,174,66,96,130};

bool dec(const unsigned char* d, size_t n, int& w, int& h, std::vector<ds::u32>& px) {
  std::string err;
  const bool ok = ds::cheevos::decode_png(d, n, w, h, px, err);
  if (!ok) std::fprintf(stderr, "decode: %s\n", err.c_str());
  return ok;
}
} // namespace

int main() {
  int w = 0, h = 0;
  std::vector<ds::u32> px;
  // RGBA, filters None and Sub.
  CHECK(dec(kRgba, sizeof kRgba, w, h, px));
  CHECK(w == 2 && h == 2);
  CHECK(px[0] == 0xFFFF0000u);
  CHECK(px[1] == 0x8000FF00u);
  CHECK(px[2] == 0xFF0000FFu);
  CHECK(px[3] == 0xFF0A141Eu);
  // Palette at 2 bits, with tRNS for the first two entries.
  CHECK(dec(kPal, sizeof kPal, w, h, px));
  CHECK(w == 4 && h == 1);
  CHECK(px[0] == 0xFFFF0000u);
  CHECK(px[1] == 0x8000FF00u);
  CHECK(px[2] == 0xFF0000FFu);
  CHECK(px[3] == 0xFF090909u);
  // Grey, filters Up and Average.
  CHECK(dec(kGrey, sizeof kGrey, w, h, px));
  CHECK(w == 3 && h == 2);
  CHECK(px[0] == 0xFF0A0A0Au && px[1] == 0xFF141414u && px[2] == 0xFF1E1E1Eu);
  CHECK(px[3] == 0xFF282828u && px[4] == 0xFF323232u && px[5] == 0xFF3C3C3Cu);
  // RGB with the Paeth filter: the second pixel adds the first.
  CHECK(dec(kRgb, sizeof kRgb, w, h, px));
  CHECK(w == 2 && h == 1);
  CHECK(px[0] == 0xFF010203u);
  CHECK(px[1] == 0xFF050709u);
  // Interlaced images are refused, not misread.
  std::string err;
  CHECK(!ds::cheevos::decode_png(kInterlaced, sizeof kInterlaced, w, h, px, err));
  CHECK(!err.empty());
  // Garbage is refused.
  CHECK(!ds::cheevos::decode_png(kRgba, 20, w, h, px, err));
  std::puts("cheevos_png: ok");
  return 0;
}
