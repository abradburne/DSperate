// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "cheevos/cheevos_session.h"

#include <cstdio>
#include <cstdlib>

#include "cheevos/cheevos_hash.h"
#include "core/gba/gba.h"
#include "core/nds.h"
#include "core/off_realtime.h"

namespace ds::cheevos {
namespace {

bool verbose() {
  static const bool on = [] {
    const char* e = std::getenv("DS_CHEEVOS_VERBOSE");
    return e && std::atoi(e) != 0;
  }();
  return on;
}

} // namespace

bool Session::start(const SessionOptions& opts) {
  opts_ = opts;
  core_ = opts_.core;
  client_.set_core(core_);   // the sign-in names the system too
  std::string err;
  if (!client_.start(err, opts_.outbox_path)) {
    std::fprintf(stderr, "cheevos: %s\n", client_.unavailable_reason().c_str());
    return false;
  }
  std::fprintf(stderr, "cheevos: user agent %s\n", user_agent(core_));
  // Before anything loads: rcheevos evaluates encore at game load and ignores it afterwards.
  client_.set_encore(opts_.encore);
  if (opts_.hardcore_asked)
    std::fprintf(stderr, "cheevos: hardcore was asked for (RA_HARDCORE=1); DSperate plays Casual, hardcore is not supported yet\n");

  // A password is never kept; an expired token fails and the player signs in again.
  const ResolvedCredentials r = resolve_credentials(opts_.credentials);
  if (!r.problems.empty()) std::fprintf(stderr, "cheevos: %s", r.problems.c_str());
  source_ = r.source;
  if (r.creds.empty()) {
    std::fprintf(stderr, "cheevos: not signed in\n");
  } else {
    std::fprintf(stderr, "cheevos: signing in as %s with %s (%s)\n", r.creds.username.c_str(),
                 source_name(r.source), r.from.c_str());
    client_.sign_in_with_token(r.creds.username, r.creds.token);
  }
  return true;
}

void Session::identify(NDS& nds, const std::string& name, const std::string& fallback_hash) {
  hash_.clear();
  set_asked_ = false;
  if (!running()) return;
  core_ = nds.gba ? Core::Gba : Core::Nds;
  client_.set_core(core_);
  std::string err;
  if (nds.gba) {
    // The image load_gba read (unzipped, never patched): the file's bytes, as rcheevos hashes them.
    const OffRealtime off_rt;
    if (!gba_rom_hash(nds.gba->rom.get(), nds.gba->rom_size, hash_, err))
      std::fprintf(stderr, "cheevos: cannot identify this GBA ROM: %s\n", err.c_str());
    else if (verbose())
      std::fprintf(stderr, "cheevos: GBA ROM hash %s\n", hash_.c_str());
    return;
  }
  if (!nds.cart) { hash_ = fallback_hash; return; }
  // Safe after Cart's secure-area rewrite: read_unpatched reads past it.
  if (!rom_hash(nds.cart->source(), name, hash_, err, nds.dsi))
    std::fprintf(stderr, "cheevos: cannot identify this ROM: %s\n", err.c_str());
}

void Session::change_game(NDS& nds, const std::string& name, const std::string& fallback_hash) {
  if (!running()) return;
  client_.unload_game();
  identify(nds, name, fallback_hash);
}

// Watches for the moment sign-in (asynchronous) completes rather than asking for the
// set at boot; also what lets a sign-in from the menu load the set for a running game.
void Session::catch_up(NDS& nds) {
  if (set_asked_ || hash_.empty()) return;
  if (client_.state() != State::SignedIn) return;
  set_asked_ = true;
  client_.load_game(nds, hash_, core_);
}

// A state's progress goes in once a set exists for it, before rc_client_do_frame,
// so the runtime never evaluates stale progress.
void Session::apply_pending() {
  if (!pending_.waiting) return;
  const State st = client_.state();
  if (st == State::LoadingGame || st == State::SigningIn) return;   // not yet; keep waiting
  if (st != State::Playing) { pending_ = {}; return; }
  // Game id must match, or applying it is how a false unlock happens; a mismatch
  // resets rather than guesses.
  const bool ok = !pending_.blob.empty() && pending_.game_id != 0 && pending_.game_id == client_.game_id() &&
                  client_.deserialize_progress(pending_.blob.data(), pending_.blob.size());
  if (!ok) client_.reset();
  if (verbose())
    std::fprintf(stderr, "cheevos: state progress %s\n", ok ? "restored" : "reset (none carried, or it did not apply)");
  pending_ = {};
}

void Session::frame(NDS& nds) {
  if (!running()) return;
  apply_pending();
  client_.frame();
  catch_up(nds);
}

void Session::idle(NDS& nds) {
  if (!running()) return;
  client_.idle();
  catch_up(nds);
}

void Session::after_reset() {
  if (running()) client_.reset();
}

bool Session::save_progress(u32& game_id, std::vector<u8>& blob) const {
  if (!running()) return false;
  game_id = client_.game_id();
  return client_.serialize_progress(blob);
}

void Session::progress_loaded(u32 game_id, std::vector<u8> blob) {
  pending_ = {true, game_id, std::move(blob)};
}

void Session::note(std::string title, std::string description) {
  Event e;
  e.type = Event::Type::Note;
  e.title = std::move(title);
  e.description = std::move(description);
  notes_.push_back(std::move(e));
}

void Session::sign_in(const std::string& username, const std::string& password) {
  if (!running()) return;
  // Kept once it succeeds, when it is DSperate's to keep (see take_events).
  menu_sign_in_ = true;
  client_.sign_in(username, password);
}

void Session::sign_out() {
  if (!running()) return;
  menu_sign_in_ = false;
  client_.sign_out();
  set_asked_ = false;   // a later sign-in asks for the set again
  rows_.clear();
  summary_ = {};
  if (launcher_owns_sign_in()) {
    note("Signed out for this game", "Sign out in " + opts_.launcher_name + " to stay signed out");
  } else if (source_ == CredentialSource::Own) {
    clear_credentials(opts_.credentials.config_dir);
    source_ = CredentialSource::None;
  } else if (source_ == CredentialSource::TokenFile || source_ == CredentialSource::System) {
    // Not DSperate's file to remove: the next start signs in from it again.
    note("Signed out for this run", std::string("The sign-in comes from ") + source_name(source_));
  }
}

std::vector<Event> Session::take_events() {
  std::vector<Event> in = client_.take_events();
  std::vector<Event> out;
  out.reserve(in.size() + notes_.size() + 1);
  for (Event& e : in) {
    const Event::Type type = e.type;
    out.push_back(std::move(e));
    if (type == Event::Type::SignInFailed) menu_sign_in_ = false;
    if (type != Event::Type::SignedIn || !menu_sign_in_) continue;
    menu_sign_in_ = false;
    if (launcher_owns_sign_in()) {
      Event n;
      n.type = Event::Type::Note;
      n.title = "Signed in for this game";
      n.description = "Sign in in " + opts_.launcher_name + " to stay signed in";
      out.push_back(std::move(n));
      continue;
    }
    std::string err;
    if (save_credentials(opts_.credentials.config_dir, {client_.username(), client_.token()}, err))
      source_ = CredentialSource::Own;
    else
      std::fprintf(stderr, "cheevos: the sign-in is not kept: %s\n", err.c_str());
  }
  for (Event& n : notes_) out.push_back(std::move(n));
  notes_.clear();
  for (const Event& e : out) std::fprintf(stderr, "cheevos: %s\n", describe(e).c_str());
  return out;
}

void Session::refresh() {
  rows_ = client_.achievements();
  summary_ = Client::summarize(rows_);
}

bool Session::state_changed() {
  if (client_.state() == drawn_) return false;
  drawn_ = client_.state();
  refresh();   // the list and the counts move with it
  return true;
}

std::string Session::describe(const Event& e) {
  using T = Event::Type;
  auto with = [](std::string a, const std::string& b) { return b.empty() ? a : a + " -- " + b; };
  switch (e.type) {
  case T::SignedIn:        return with("signed in", e.title);
  case T::SignInFailed:    return with("sign-in failed", e.description);
  case T::SignedOut:       return "signed out";
  case T::Note:            return with(e.title, e.description);
  case T::Unavailable:     return with(e.title, e.description);
  case T::GameStarted: {
    char buf[160];
    std::snprintf(buf, sizeof buf, "%s mode, %u/%u unlocked, %u unsupported (game %u)",
                  e.hardcore ? "hardcore" : "Casual", e.unlocked, e.total, e.unsupported, e.id);
    return with(e.title.empty() ? "achievements active" : e.title, buf);
  }
  case T::NoSet:           return with("no achievements for this ROM", e.description.empty() ? "" : "hash " + e.description);
  case T::LoadFailed:      return with("could not load achievements", e.description);
  case T::Unlock:          return with("unlocked " + std::to_string(e.id) + " " + e.title, std::to_string(e.points) + " points");
  case T::Progress:        return with("progress " + std::to_string(e.id) + " " + e.title, e.progress);
  case T::ProgressHide:    return "progress hidden";
  case T::ChallengeShow:   return "challenge " + std::to_string(e.id) + " " + e.title;
  case T::ChallengeHide:   return "challenge " + std::to_string(e.id) + " over";
  case T::GameCompleted:   return with("all achievements earned", e.title);
  case T::SubsetCompleted: return with("subset completed", e.title);
  case T::ServerError:     return with("server error" + (e.title.empty() ? "" : " (" + e.title + ")"), e.description);
  case T::Disconnected:    return "offline; unlocks will be sent when the connection returns";
  case T::Reconnected:     return "reconnected; pending unlocks sent";
  }
  return "";
}

} // namespace ds::cheevos
