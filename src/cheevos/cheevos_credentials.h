// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// RetroAchievements sign-ins on disk: the file formats DSperate reads and writes,
// and which one wins when there are several. No network, no rcheevos.
#pragma once

#include <string>

#include "core/types.h"

namespace ds::cheevos {

// The token alone is enough to act as the player, so every file holding one is
// mode 0600. A password is never stored.
struct Credentials {
  std::string username;
  std::string token;
  bool empty() const { return username.empty() || token.empty(); }
};

// DSperate's own sign-in, "<dir>/cheevos.token": the username, then the token, a line
// each. `dir` is the config directory. A missing file yields empty credentials, not an error.
bool load_credentials(const std::string& dir, Credentials& out, std::string& err);
bool save_credentials(const std::string& dir, const Credentials& in, std::string& err);
void clear_credentials(const std::string& dir);

// A token file another program wrote (--cheevos-token, the launcher's RA_TOKEN_FILE):
// the token alone (PPSSPP's format) or "username\ntoken" (ours, DSlot's).
bool read_token_file(const std::string& path, Credentials& out, std::string& err);

// The sign-in the CFW's front end already holds (ROCKNIX system.cfg, RetroArch
// retroarch.cfg). Only the username and token are read; the cleartext password in
// these files is never touched. Never writes. read_cfw_credentials parses one file;
// import_cfw_credentials walks the known locations ($DS_CHEEVOS_CFW_CONFIG first),
// reporting which one it used in `source`.
bool read_cfw_credentials(const std::string& path, Credentials& out);
bool import_cfw_credentials(Credentials& out, std::string& source);

// Where a sign-in can come from, in the order they are tried.
enum class CredentialSource : u8 {
  None,
  TokenFile,   // cheevos.token_file / --cheevos-token: named by the player, never written
  Launcher,    // RA_TOKEN_FILE (and RA_USERNAME): the launcher's, which it keeps
  Own,         // <config>/cheevos.token: DSperate's own, from its own sign-in
  System,      // the CFW front end's (cheevos.use_system_login)
};
const char* source_name(CredentialSource s);

struct CredentialInputs {
  std::string token_file;            // cheevos.token_file / --cheevos-token
  std::string username;              // cheevos.username / --cheevos-user
  std::string launcher_token_file;   // RA_TOKEN_FILE
  std::string launcher_username;     // RA_USERNAME
  std::string config_dir;            // where DSperate's own cheevos.token lives
  bool use_system_login = true;

  // Started by a launcher that keeps the sign-in (DSlot): it owns the token file, so
  // signing in or out inside DSperate writes nothing (docs/retroachievements.md).
  bool launcher_owned() const { return !launcher_token_file.empty() && token_file.empty(); }

  // RA_TOKEN_FILE and RA_USERNAME from the environment into the launcher fields.
  void read_launcher_env();
};

struct ResolvedCredentials {
  Credentials creds;                 // empty: not signed in
  CredentialSource source = CredentialSource::None;
  std::string from;                  // the file it came from
  std::string problems;              // what was tried and failed, one line each, for the log
};

// The first usable sign-in, in CredentialSource's order. A token file that cannot be
// read, or gives no username from anywhere, is reported and the next one tried.
ResolvedCredentials resolve_credentials(const CredentialInputs& in);

} // namespace ds::cheevos
