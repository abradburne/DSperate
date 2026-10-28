// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "cheevos_menu.h"

#include <cstdio>

#include "config.h"

namespace ds::sdl {
namespace {

const char* key_of(CheevosHost::Option o) {
  switch (o) {
  case CheevosHost::Option::Toasts:     return "cheevos.toasts";
  case CheevosHost::Option::Screenshot: return "cheevos.auto_screenshot";
  case CheevosHost::Option::Encore:     return "cheevos.encore";
  }
  return "";
}

} // namespace

std::string CheevosMenu::status() const {
  const cheevos::Client& c = s_.client();
  switch (c.state()) {
  case cheevos::State::Off:
    return c.unavailable_reason().empty() ? "TURNED OFF" : c.unavailable_reason();
  case cheevos::State::SignedOut:   return "NOT SIGNED IN";
  case cheevos::State::SigningIn:   return "SIGNING IN...";
  case cheevos::State::SignedIn:    return "SIGNED IN AS " + c.username();
  case cheevos::State::LoadingGame: return "LOADING ACHIEVEMENTS...";
  case cheevos::State::Playing:     return "SIGNED IN AS " + c.username() + " - CASUAL";
  case cheevos::State::EmptySet:    return "NO ACHIEVEMENTS PUBLISHED FOR THIS GAME YET";
  case cheevos::State::NoSet:       return "NO ACHIEVEMENTS FOR THIS ROM - HASH " + c.game_hash();
  }
  return "";
}

std::string CheevosMenu::progress() const {
  const cheevos::Client::Summary& sum = s_.summary();
  if (sum.total == 0) return {};
  char buf[128];
  // Encore explains why an already-earned achievement unlocks again.
  std::snprintf(buf, sizeof buf, "%u/%u EARNED  %u/%u POINTS%s",
                sum.unlocked, sum.total, sum.points_earned, sum.points,
                s_.client().encore() ? "  (ENCORE)" : "");
  return buf;
}

bool CheevosMenu::signed_in() const {
  const auto st = s_.client().state();
  return st == cheevos::State::SignedIn || st == cheevos::State::Playing ||
         st == cheevos::State::LoadingGame || st == cheevos::State::NoSet ||
         st == cheevos::State::EmptySet;
}

CheevosHost::Row CheevosMenu::row(int i) const {
  Row r;
  if (i < 0 || i >= row_count()) return r;
  const cheevos::Client::Achievement& a = s_.rows()[static_cast<size_t>(i)];
  r.title = a.title;
  r.detail = (!a.unlocked && !a.progress.empty()) ? a.progress + "   " + a.description : a.description;
  r.points = a.points;
  r.unlocked = a.unlocked;
  r.unsupported = a.unsupported;
  return r;
}

// Read the way Config::flag reads it; only toasts default on.
bool CheevosMenu::option(Option o) const {
  return parse_flag(settings_.get(key_of(o)), o == Option::Toasts);
}

void CheevosMenu::set_option(Option o, bool on) {
  settings_.set(key_of(o), on ? "true" : "false");
  // Encore is read at game load; tell the session now for next load.
  if (o == Option::Encore) s_.client().set_encore(on);
}

} // namespace ds::sdl
