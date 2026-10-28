// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// One run's RetroAchievements, as a frontend drives it: the sign-in at start and who
// keeps it, the identity of the game in the slot, the set asked for once signed in,
// a save state's progress held until the set is there, the account page's list.
// Knows nothing of a screen: what to show arrives as Client events.
//
// Everything here runs on the emulation thread.
#pragma once

#include <string>
#include <vector>

#include "cheevos/cheevos_client.h"
#include "cheevos/cheevos_credentials.h"

namespace ds { class NDS; }

namespace ds::cheevos {

struct SessionOptions {
  CredentialInputs credentials;    // the config's and the launcher's sign-ins
  bool encore = false;
  std::string outbox_path;         // where unconfirmed unlocks wait; empty keeps none
  std::string launcher_name = "the launcher";   // named when the launcher keeps the sign-in
  bool hardcore_asked = false;     // RA_HARDCORE=1: noted; Casual is what is played
};

class Session {
public:
  // Starts the client and signs in with the first usable sign-in. False when
  // achievements cannot run on this device (the reason is logged and kept).
  bool start(const SessionOptions& opts);
  void shutdown() { client_.shutdown(); }
  bool running() const { return client_.state() != State::Off; }
  Client& client() { return client_; }
  const Client& client() const { return client_; }

  // The game now in the slot: `name` is its path (rcheevos hashes some formats by
  // name), `fallback_hash` the identity of a DSiWare title taken when it was read
  // (there is no cart to hash). Without a cart or a fallback, nothing is asked for.
  void identify(NDS& nds, const std::string& name, const std::string& fallback_hash = {});
  // Another game in the slot: the old set goes, the new one is asked for once signed in.
  void change_game(NDS& nds, const std::string& name, const std::string& fallback_hash = {});

  // Once per emulated frame, right after NDS::run_frame(): exactly once, since rcheevos
  // keeps a delta per memory reference and a missed frame or a second call breaks
  // single-frame edge triggers.
  void frame(NDS& nds);
  // Instead of frame() while paused, at least once a second.
  void idle(NDS& nds);
  // The machine was reset: rcheevos starts its counting again.
  void after_reset();

  // Save states. save_progress: false when there is nothing to carry. progress_loaded:
  // what a state carried (an empty blob: none), applied once the set is there; works
  // before start(), for a state loaded at boot.
  bool save_progress(u32& game_id, std::vector<u8>& blob) const;
  void progress_loaded(u32 game_id, std::vector<u8> blob);

  // What happened since the last call, each already logged and acted on here
  // (a sign-in kept or not); the frontend decides what to show.
  std::vector<Event> take_events();

  // The account page's sign-in and sign-out. Started by a launcher that keeps the
  // sign-in, they last for this run and write nothing (the launcher owns that file);
  // run on its own, a password sign-in is kept in <config>/cheevos.token and a
  // sign-out removes it.
  void sign_in(const std::string& username, const std::string& password);
  void sign_out();
  bool launcher_owns_sign_in() const { return opts_.credentials.launcher_owned(); }
  CredentialSource sign_in_source() const { return source_; }

  // The achievement list and its counts, cached: building it allocates.
  void refresh();
  const std::vector<Client::Achievement>& rows() const { return rows_; }
  const Client::Summary& summary() const { return summary_; }
  // True once per change of the client's state, with the list refreshed.
  bool state_changed();

  // One line for the log.
  static std::string describe(const Event& e);

private:
  void apply_pending();
  void catch_up(NDS& nds);
  void note(std::string title, std::string description);

  struct Pending {
    bool waiting = false;      // a state was loaded and is not yet accounted for
    u32 game_id = 0;
    std::vector<u8> blob;      // empty: the state carried no progress, so reset
  };

  Client client_;
  SessionOptions opts_;
  CredentialSource source_ = CredentialSource::None;
  std::string hash_;           // the running game's identity; empty if it could not be hashed
  bool set_asked_ = false;
  bool menu_sign_in_ = false;  // a password sign-in from the menu is under way
  Pending pending_;
  std::vector<Event> notes_;   // the session's own, ahead of the client's
  std::vector<Client::Achievement> rows_;
  Client::Summary summary_{};
  State drawn_ = State::Off;
};

} // namespace ds::cheevos
