// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// The pause menu's RetroAchievements pages (menu.h's CheevosHost) over a
// cheevos::Session: what the account page says, the list, the switches.
#pragma once

#include "cheevos/cheevos_session.h"
#include "menu.h"
#include "settings.h"

namespace ds::sdl {

class CheevosMenu final : public CheevosHost {
public:
  CheevosMenu(cheevos::Session& session, SettingsHost& settings) : s_(session), settings_(settings) {}

  std::string status() const override;
  std::string progress() const override;
  bool signed_in() const override;
  bool has_set() const override { return !s_.rows().empty(); }
  int row_count() const override { return static_cast<int>(s_.rows().size()); }
  Row row(int i) const override;
  void sign_in(const std::string& user, const std::string& password) override { s_.sign_in(user, password); }
  void sign_out() override { s_.sign_out(); }
  bool option(Option o) const override;
  void set_option(Option o, bool on) override;

private:
  cheevos::Session& s_;
  SettingsHost& settings_;
};

} // namespace ds::sdl
