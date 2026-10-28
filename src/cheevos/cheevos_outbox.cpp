// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "cheevos/cheevos_outbox.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>

#include <fcntl.h>
#include <unistd.h>

namespace ds::cheevos {
namespace {

// One line each: user, achievement, hash, unlock time, hardcore, tab-separated, under
// a first line naming the format.
constexpr char kHeader[] = "dsperate-outbox 1";

bool plain(const std::string& s) {
  return !s.empty() && s.find_first_of("\t\r\n") == std::string::npos;
}

bool same_user(const std::string& a, const std::string& b) {
  return a.size() == b.size() && ::strncasecmp(a.c_str(), b.c_str(), a.size()) == 0;
}

int hex(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

std::string url_decode(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '+') out += ' ';
    else if (s[i] == '%' && i + 2 < s.size() && hex(s[i + 1]) >= 0 && hex(s[i + 2]) >= 0) {
      out += static_cast<char>(hex(s[i + 1]) * 16 + hex(s[i + 2]));
      i += 2;
    } else out += s[i];
  }
  return out;
}

bool to_u64(const std::string& s, unsigned long long& v) {
  if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos) return false;
  v = std::strtoull(s.c_str(), nullptr, 10);
  return true;
}

} // namespace

bool Outbox::open(const std::string& path, std::string& err) {
  err.clear();
  path_.clear();
  items_.clear();
  std::FILE* f = std::fopen(path.c_str(), "r");
  if (!f) {
    if (errno != ENOENT) { err = path + ": " + std::strerror(errno); return false; }
    path_ = path;
    return true;
  }
  char line[1024];
  bool first = true;
  while (std::fgets(line, sizeof line, f)) {
    std::string s(line);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    if (first) {
      first = false;
      if (s != kHeader) { std::fclose(f); err = path + ": not an outbox this DSperate reads"; return false; }
      continue;
    }
    std::string f5[5];
    size_t at = 0;
    int n = 0;
    for (; n < 5; ++n) {
      const size_t tab = s.find('\t', at);
      f5[n] = s.substr(at, tab == std::string::npos ? std::string::npos : tab - at);
      if (tab == std::string::npos) { ++n; break; }
      at = tab + 1;
    }
    unsigned long long id = 0, when = 0;
    if (n != 5 || !plain(f5[0]) || !to_u64(f5[1], id) || id == 0 || id > 0xFFFFFFFFull ||
        !to_u64(f5[3], when) || (f5[4] != "0" && f5[4] != "1"))
      continue;   // a damaged line costs that unlock, not the rest
    PendingUnlock p;
    p.user = f5[0];
    p.achievement = static_cast<u32>(id);
    p.hash = f5[2];
    p.unlocked_at = static_cast<s64>(when);
    p.hardcore = f5[4] == "1";
    items_.push_back(std::move(p));
  }
  std::fclose(f);
  path_ = path;
  return true;
}

void Outbox::add(const PendingUnlock& p) {
  if (!is_open() || !plain(p.user) || p.achievement == 0 || p.hash.find_first_of("\t\r\n") != std::string::npos)
    return;
  for (const PendingUnlock& q : items_)
    if (q.achievement == p.achievement && same_user(q.user, p.user)) return;
  items_.push_back(p);
  save();
}

void Outbox::remove(const std::string& user, u32 achievement) {
  if (!is_open()) return;
  const size_t before = items_.size();
  for (size_t i = 0; i < items_.size();) {
    if (items_[i].achievement == achievement && same_user(items_[i].user, user)) items_.erase(items_.begin() + i);
    else ++i;
  }
  if (items_.size() != before) save();
}

// 0600 from creation, then renamed over the old one: never half a file, never readable by others.
bool Outbox::save() {
  if (items_.empty()) {
    if (::unlink(path_.c_str()) != 0 && errno != ENOENT) return false;
    return true;
  }
  std::string body = std::string(kHeader) + "\n";
  for (const PendingUnlock& p : items_)
    body += p.user + "\t" + std::to_string(p.achievement) + "\t" + p.hash + "\t" +
            std::to_string(p.unlocked_at < 0 ? 0 : p.unlocked_at) + "\t" + (p.hardcore ? "1" : "0") + "\n";
  const std::string tmp = path_ + ".tmp";
  ::unlink(tmp.c_str());
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0) return false;
  const bool ok = ::write(fd, body.data(), body.size()) == static_cast<ssize_t>(body.size()) && ::fsync(fd) == 0;
  ::close(fd);
  if (!ok || std::rename(tmp.c_str(), path_.c_str()) != 0) { ::unlink(tmp.c_str()); return false; }
  return true;
}

bool parse_award_request(const std::string& body, s64 now, PendingUnlock& out) {
  out = PendingUnlock{};
  std::string r, a, h, o;
  size_t at = 0;
  while (at <= body.size()) {
    const size_t amp = body.find('&', at);
    const std::string kv = body.substr(at, amp == std::string::npos ? std::string::npos : amp - at);
    const size_t eq = kv.find('=');
    if (eq != std::string::npos) {
      const std::string k = kv.substr(0, eq), v = url_decode(kv.substr(eq + 1));
      if (k == "r") r = v;
      else if (k == "u") out.user = v;
      else if (k == "a") a = v;
      else if (k == "h") h = v;
      else if (k == "m") out.hash = v;
      else if (k == "o") o = v;
    }
    if (amp == std::string::npos) break;
    at = amp + 1;
  }
  unsigned long long id = 0, ago = 0;
  if (r != "awardachievement" || out.user.empty() || !to_u64(a, id) || id == 0 || id > 0xFFFFFFFFull) {
    out = PendingUnlock{};
    return false;
  }
  out.achievement = static_cast<u32>(id);
  out.hardcore = h == "1";
  out.unlocked_at = now - (to_u64(o, ago) ? static_cast<s64>(ago) : 0);
  return true;
}

bool award_settled(int http_status) {
  if (http_status < 200 || http_status >= 500) return false;
  return http_status != 401 && http_status != 403 && http_status != 408 && http_status != 429;
}

} // namespace ds::cheevos
