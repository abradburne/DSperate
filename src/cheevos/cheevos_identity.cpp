// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "cheevos/cheevos_identity.h"

#include <string>

#include "rc_client.h"

namespace ds::cheevos {

const char* client_version() { return DSPERATE_CHEEVOS_VERSION; }

const char* product_token() {
  static const std::string token = std::string(kClientName) + "/" + DSPERATE_CHEEVOS_VERSION;
  return token.c_str();
}

namespace {

std::string agent_for(const char* core) {
  char clause[64] = {};
  rc_client_get_user_agent_clause(nullptr, clause, sizeof clause);   // "rcheevos/12.4"
  return std::string(product_token()) + " " + core + "/" + DSPERATE_CHEEVOS_VERSION + " " + clause;
}

} // namespace

const char* user_agent(Core core) {
  static const std::string nds = agent_for("nds");
  static const std::string gba = agent_for("gba");
  return (core == Core::Gba ? gba : nds).c_str();
}

} // namespace ds::cheevos
