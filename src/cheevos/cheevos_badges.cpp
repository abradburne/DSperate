// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "cheevos/cheevos_badges.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>

#include <sys/stat.h>
#include <unistd.h>

#include "cheevos/cheevos_http.h"
#include "cheevos/cheevos_png.h"

namespace ds::cheevos {
namespace {

bool read_file(const std::string& path, std::vector<u8>& out) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  out.clear();
  u8 buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.insert(out.end(), buf, buf + n);
  std::fclose(f);
  return !out.empty();
}

// Written beside its final name, then renamed: a reader never sees half a file.
void write_file_atomic(const std::string& path, const std::string& body) {
  const std::string tmp = path + ".part";
  FILE* f = std::fopen(tmp.c_str(), "wb");
  if (!f) return;
  const bool ok = std::fwrite(body.data(), 1, body.size(), f) == body.size();
  std::fclose(f);
  if (ok) ::rename(tmp.c_str(), path.c_str()); else ::unlink(tmp.c_str());
}

} // namespace

std::string badge_cache_dir() {
  std::string base;
  if (const char* x = std::getenv("XDG_CACHE_HOME"); x && *x) base = x;
  else if (const char* h = std::getenv("HOME"); h && *h) base = std::string(h) + "/.cache";
  else return {};
  ::mkdir(base.c_str(), 0755);
  base += "/retroachievements";
  ::mkdir(base.c_str(), 0755);
  base += "/badges";
  ::mkdir(base.c_str(), 0755);
  return base;
}

bool valid_badge_name(const std::string& name) {
  if (name.empty()) return false;
  for (const char c : name)
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')) return false;
  return true;
}

BadgeResult load_badge(const std::string& name, Backend& http, const char* user_agent,
                       const std::atomic<bool>* cancel, Badge& out, std::string& err) {
  err.clear();
  if (!valid_badge_name(name)) { err = "not a badge name"; return BadgeResult::Undecodable; }
  const std::string dir = badge_cache_dir();
  const std::string path = dir.empty() ? std::string() : dir + "/" + name + ".png";
  std::vector<u8> png;
  if (path.empty() || !read_file(path, png)) {
    Request req;
    req.url = "https://media.retroachievements.org/Badge/" + name + ".png";
    Response res = http.perform(req, user_agent, cancel);
    if (res.status != 200 || res.body.empty()) {
      err = res.error.empty() ? "status " + std::to_string(res.status) : res.error;
      return BadgeResult::NotFetched;
    }
    if (!path.empty()) write_file_atomic(path, res.body);
    png.assign(res.body.begin(), res.body.end());
  }
  out = Badge{};
  out.name = name;
  if (!decode_png(png.data(), png.size(), out.w, out.h, out.px, err)) return BadgeResult::Undecodable;
  return BadgeResult::Ok;
}

} // namespace ds::cheevos
