// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Unlocks the server has not confirmed yet, kept on disk so that one earned while
// offline survives a quit: written when rcheevos asks to award an achievement,
// struck off when the server's answer settles it, sent again at the next sign-in
// (docs/retroachievements.md). No token is kept: a replay is signed with the
// sign-in of the day, for the same user only. The file is mode 0600 all the same.
#pragma once

#include <string>
#include <vector>

#include "core/types.h"

namespace ds::cheevos {

struct PendingUnlock {
  std::string user;          // the account it was earned on
  u32 achievement = 0;
  std::string hash;          // the dump it was earned on (the award names it)
  s64 unlocked_at = 0;       // Unix seconds; the replay says how long ago
  bool hardcore = false;     // as asked for; always false in this build
};

class Outbox {
public:
  // Reads what is waiting at `path` (a missing file is an empty outbox). Until
  // open() succeeds, add() and remove() do nothing.
  bool open(const std::string& path, std::string& err);
  bool is_open() const { return !path_.empty(); }
  const std::string& path() const { return path_; }

  // Kept once per user and achievement: a retry keeps the first unlock time.
  void add(const PendingUnlock& p);
  void remove(const std::string& user, u32 achievement);
  const std::vector<PendingUnlock>& pending() const { return items_; }

private:
  bool save();
  std::string path_;
  std::vector<PendingUnlock> items_;
};

// The award an rcheevos "awardachievement" request body asks for (form-encoded:
// r, u, a, h, m, o), its unlock time taken from `now` less o. False for any other request.
bool parse_award_request(const std::string& body, s64 now, PendingUnlock& out);

// Whether the server's answer settles an award, so it leaves the outbox: any answer
// but none at all, a server error, or one a new sign-in or version could change
// (401, 403, 408, 429).
bool award_settled(int http_status);

} // namespace ds::cheevos
