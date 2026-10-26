// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "frontend/sdl/debug_options.h"

#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace ds::sdl::debug {

namespace {

struct Key { const char* name; const char* value; const char* help; };
constexpr Key kKeys[] = {
    {"marks", nullptr, "launch stages on stderr (CLOCK_BOOTTIME)"},
    {"present-wait", nullptr, "show every frame: wait for the GPU instead of leaving one out"},
    {"present-hash", "FILE", "a hash of each window's picture per present"},
    {"present-ppm", "N[+COUNT]:PREFIX", "the windows' pictures at presents N.. as PPM"},
    {"menu-ppm", "PREFIX", "the shared pause menu's pictures as PPM, each time they change"},
    {"hires-stats", nullptr, "hi-res 3D: CPU ms per stage, frames drawn / captured"},
    {"hires-half", nullptr, "hi-res 3D at half rate (every other frame shown) throughout"},
    {"hires-switch", "FRAME:SCALE[+...]", "the 3D resolution changed at those frames, as the pause menu does"},
    {"hires-stages", nullptr, "hi-res 3D: GPU ms per stage (timer queries)"},
    {"hires-bench", "N[:MASK/MASK/...]", "hi-res 3D: render N drawn 60 times per stage mask, timed"},
    {"latency", "FILE", "button-to-flip latency, a line a frame"},
    {"vsync-commit-ms", "MS", "the vsync lock's commit time, instead of its estimate"},
};

void usage() {
  std::fprintf(stderr, "DS_DEBUG=key[=value],... where key is one of:\n");
  for (const Key& k : kKeys) {
    const std::string lhs = std::string(k.name) + (k.value ? std::string("=") + k.value : "");
    std::fprintf(stderr, "  %-30s %s\n", lhs.c_str(), k.help);
  }
}

// "N[+COUNT]:PREFIX"
bool parse_ppm(const std::string& v, Options& o) {
  const size_t colon = v.find(':');
  if (colon == std::string::npos || colon + 1 == v.size()) return false;
  const std::string head = v.substr(0, colon);
  const size_t plus = head.find('+');
  o.present_ppm_from = static_cast<u32>(std::strtoul(head.c_str(), nullptr, 10));
  o.present_ppm_count = plus == std::string::npos ? 1u : static_cast<u32>(std::strtoul(head.c_str() + plus + 1, nullptr, 10));
  o.present_ppm_prefix = v.substr(colon + 1);
  return true;
}

// "N[:MASK/MASK/...]"
void parse_bench(const std::string& v, Options& o) {
  o.hires_bench_at = static_cast<u32>(std::strtoul(v.c_str(), nullptr, 10));
  const size_t colon = v.find(':');
  if (colon == std::string::npos) { o.hires_bench_masks = {0}; return; }
  for (const char* p = v.c_str() + colon + 1; *p;) {
    char* end = nullptr;
    o.hires_bench_masks.push_back(static_cast<u32>(std::strtoul(p, &end, 10)));
    p = *end ? end + 1 : end;
  }
}

Options parse(const char* env) {
  Options o;
  if (!env || !*env) return o;
  const std::string s(env);
  for (size_t at = 0; at <= s.size();) {
    size_t end = s.find(',', at);
    if (end == std::string::npos) end = s.size();
    const std::string item = s.substr(at, end - at);
    at = end + 1;
    if (item.empty()) continue;
    const size_t eq = item.find('=');
    const std::string key = item.substr(0, eq), value = eq == std::string::npos ? "" : item.substr(eq + 1);
    if (key == "help") { usage(); std::exit(0); }
    else if (key == "marks") o.marks = true;
    else if (key == "present-wait") o.present_wait = true;
    else if (key == "present-hash" && !value.empty()) o.present_hash = value;
    else if (key == "present-ppm" && parse_ppm(value, o)) {}
    else if (key == "menu-ppm" && !value.empty()) o.menu_ppm = value;
    else if (key == "hires-stats") o.hires_stats = true;
    else if (key == "hires-half") o.hires_half = true;
    else if (key == "hires-switch") {
      for (size_t at = 0; at < value.size();) {
        const size_t end = std::min(value.find('+', at), value.size()), colon = value.find(':', at);
        if (colon < end) o.hires_switch.emplace_back(std::strtoull(value.c_str() + at, nullptr, 10), value.substr(colon + 1, end - colon - 1));
        at = end + 1;
      }
    }
    else if (key == "hires-stages") o.hires_stages = true;
    else if (key == "hires-bench" && !value.empty()) parse_bench(value, o);
    else if (key == "latency" && !value.empty()) o.latency = value;
    else if (key == "vsync-commit-ms" && !value.empty()) o.vsync_commit_ms = std::strtod(value.c_str(), nullptr);
    else std::fprintf(stderr, "DS_DEBUG: '%s' not understood (DS_DEBUG=help lists the keys)\n", item.c_str());
  }
  return o;
}

} // namespace

const Options& options() {
  static const Options o = parse(std::getenv("DS_DEBUG"));
  return o;
}

// The crash line reads this from a signal handler, so it is a fixed buffer
// and written with nothing that could lock or allocate.
static char g_last_mark[96] = "nothing";

const char* last_mark() { return g_last_mark; }

void mark(const char* stage) {
  std::strncpy(g_last_mark, stage, sizeof g_last_mark - 1);
  g_last_mark[sizeof g_last_mark - 1] = 0;
  if (!options().marks) return;
  timespec t{};
  clock_gettime(CLOCK_BOOTTIME, &t);
  std::fprintf(stderr, "dsperate: %.3f s %s\n", static_cast<double>(t.tv_sec) + t.tv_nsec / 1e9, stage);
}

} // namespace ds::sdl::debug
