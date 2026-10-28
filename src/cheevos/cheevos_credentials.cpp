// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "cheevos/cheevos_credentials.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ds::cheevos {
namespace {

const char* token_name = "/cheevos.token";

} // namespace

bool load_credentials(const std::string& dir, Credentials& out, std::string& err) {
  out = Credentials{};
  err.clear();
  const std::string path = dir + token_name;
  std::FILE* f = std::fopen(path.c_str(), "r");
  if (!f) return true;

  char line[512];
  std::string fields[2];
  for (int i = 0; i < 2 && std::fgets(line, sizeof line, f); ++i) {
    std::string s(line);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    fields[i] = s;
  }
  std::fclose(f);
  out.username = fields[0];
  out.token = fields[1];
  if (out.empty()) {
    out = Credentials{};
    err = "the saved credentials are incomplete";
    return false;
  }
  return true;
}

bool save_credentials(const std::string& dir, const Credentials& in, std::string& err) {
  err.clear();
  if (in.empty()) { err = "nothing to save"; return false; }
  if (in.username.find('\n') != std::string::npos || in.token.find('\n') != std::string::npos) {
    err = "credentials contain a newline";
    return false;
  }

  // 0600 from creation; write to temp file and rename to avoid a half-written file.
  const std::string path = dir + token_name;
  const std::string tmp = path + ".tmp";
  ::unlink(tmp.c_str());
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0) { err = std::strerror(errno); return false; }
  const std::string body = in.username + "\n" + in.token + "\n";
  const ssize_t n = ::write(fd, body.data(), body.size());
  const bool ok = n == static_cast<ssize_t>(body.size());
  ::close(fd);
  if (!ok || std::rename(tmp.c_str(), path.c_str()) != 0) {
    err = std::strerror(errno);
    ::unlink(tmp.c_str());
    return false;
  }
  return true;
}

void clear_credentials(const std::string& dir) {
  const std::string path = dir + token_name;
  ::unlink(path.c_str());
}

bool read_token_file(const std::string& path, Credentials& out, std::string& err) {
  out = Credentials{};
  err.clear();
  std::FILE* f = std::fopen(path.c_str(), "r");
  if (!f) { err = path + ": " + std::strerror(errno); return false; }

  // At most two words: token-only file, or "username\ntoken".
  std::vector<std::string> words;
  char line[512];
  while (words.size() < 3 && std::fgets(line, sizeof line, f)) {
    std::string s(line);
    size_t at = 0;
    while (at < s.size() && words.size() < 3) {
      const size_t b = s.find_first_not_of(" \t\r\n", at);
      if (b == std::string::npos) break;
      const size_t e = s.find_first_of(" \t\r\n", b);
      words.push_back(s.substr(b, e == std::string::npos ? e : e - b));
      at = e == std::string::npos ? s.size() : e;
    }
  }
  std::fclose(f);

  if (words.empty()) { err = path + ": no token in the file"; return false; }
  if (words.size() == 1) { out.token = words[0]; return true; }
  out.username = words[0];
  out.token = words[1];
  return true;
}

bool read_cfw_credentials(const std::string& path, Credentials& out) {
  out = Credentials{};
  std::FILE* f = std::fopen(path.c_str(), "r");
  if (!f) return false;

  // Only these keys are read; the cleartext password in these files is never touched.
  struct Want { const char* key; std::string* into; };
  const Want wants[] = {
    {"global.retroachievements.username", &out.username},
    {"global.retroachievements.token",    &out.token},
    {"cheevos_username",                  &out.username},
    {"cheevos_token",                     &out.token},
  };

  char line[1024];
  while (std::fgets(line, sizeof line, f)) {
    std::string s(line);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    const size_t eq = s.find('=');
    if (eq == std::string::npos) continue;
    std::string key = s.substr(0, eq);
    std::string val = s.substr(eq + 1);
    auto trim = [](std::string& v) {
      size_t a = v.find_first_not_of(" \t");
      size_t b = v.find_last_not_of(" \t");
      v = a == std::string::npos ? std::string{} : v.substr(a, b - a + 1);
    };
    trim(key);
    trim(val);
    if (key.empty() || key[0] == '#') continue;
    if (val.size() >= 2 && val.front() == '"' && val.back() == '"') val = val.substr(1, val.size() - 2);
    if (val.empty()) continue;
    for (const Want& w : wants) {
      if (key == w.key) { *w.into = val; break; }
    }
  }
  std::fclose(f);
  return !out.empty();
}

bool import_cfw_credentials(Credentials& out, std::string& source) {
  out = Credentials{};
  source.clear();
  std::vector<std::string> paths;
  if (const char* e = std::getenv("DS_CHEEVOS_CFW_CONFIG")) {
    if (*e) paths.push_back(e);
  }
  paths.push_back("/storage/.config/system/configs/system.cfg");
  paths.push_back("/storage/.config/retroarch/retroarch.cfg");
  if (const char* home = std::getenv("HOME")) {
    if (*home) paths.push_back(std::string(home) + "/.config/retroarch/retroarch.cfg");
  }

  for (const std::string& p : paths) {
    if (read_cfw_credentials(p, out)) { source = p; return true; }
  }
  out = Credentials{};
  return false;
}

const char* source_name(CredentialSource s) {
  switch (s) {
  case CredentialSource::None:      return "none";
  case CredentialSource::TokenFile: return "the token file";
  case CredentialSource::Launcher:  return "the launcher's sign-in";
  case CredentialSource::Own:       return "DSperate's own sign-in";
  case CredentialSource::System:    return "the system's sign-in";
  }
  return "";
}

void CredentialInputs::read_launcher_env() {
  const char* f = std::getenv("RA_TOKEN_FILE");
  const char* u = std::getenv("RA_USERNAME");
  launcher_token_file = f ? f : "";
  launcher_username = u ? u : "";
}

ResolvedCredentials resolve_credentials(const CredentialInputs& in) {
  ResolvedCredentials r;
  std::string err;
  auto problem = [&](const std::string& line) { r.problems += line; r.problems += '\n'; };

  // A token file named by the player wins over everything; the launcher's stands in for
  // one when the player named none. Neither is ever written back: they are not ours.
  const bool launcher = in.launcher_owned();
  const std::string& file = launcher ? in.launcher_token_file : in.token_file;
  if (!file.empty()) {
    Credentials c;
    if (!read_token_file(file, c, err)) {
      problem(err);
    } else {
      if (c.username.empty() && launcher) c.username = in.launcher_username;
      if (c.username.empty()) c.username = in.username;
      if (c.username.empty())
        problem(file + " has a token but no username; pass --cheevos-user or set cheevos.username");
      else {
        r.creds = c;
        r.source = launcher ? CredentialSource::Launcher : CredentialSource::TokenFile;
        r.from = file;
        return r;
      }
    }
  }

  if (!in.config_dir.empty()) {
    Credentials c;
    if (!load_credentials(in.config_dir, c, err)) problem(err);
    else if (!c.empty()) {
      r.creds = c;
      r.source = CredentialSource::Own;
      r.from = in.config_dir + token_name;
      return r;
    }
  }

  // Failing that, the CFW front end's own sign-in: ours wins once we have one.
  if (in.use_system_login) {
    Credentials c;
    std::string from;
    if (import_cfw_credentials(c, from)) {
      r.creds = c;
      r.source = CredentialSource::System;
      r.from = from;
      return r;
    }
  }
  return r;
}

} // namespace ds::cheevos
