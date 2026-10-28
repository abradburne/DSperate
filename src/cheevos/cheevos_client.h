// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// RetroAchievements client: sign in, load a game's achievement set, evaluate
// it as the emulator runs, report what happens as typed events. Casual mode only.
// rc_client stays inside cheevos_client.cpp.
//
// Threading: rc_client and rcheevos callbacks run only on the frame() caller
// (the emulation thread). HTTP runs on one worker thread, rcheevos' requests
// before badge downloads; frame() drains finished responses before calling
// rc_client_do_frame.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "cheevos/cheevos_badges.h"
#include "cheevos/cheevos_identity.h"
#include "core/types.h"

namespace ds { class NDS; }

namespace ds::cheevos {

// Something that happened that the player may be told about; the frontend maps
// each type to what it shows (a toast, mostly) in one place.
struct Event {
  enum class Type : u8 {
    SignedIn,          // title: the display name
    SignInFailed,      // description: why
    SignedOut,
    Note,              // title, description: something to tell the player (no failure)
    Unavailable,       // title, description: achievements cannot run for this game
    GameStarted,       // the placard: title, id (game), hardcore, unlocked/total/unsupported
    NoSet,             // description: the dump's hash
    LoadFailed,        // description: why
    Unlock,            // id, title, description, points, badge, unlock_time
    Progress,          // id, title, badge, progress: show the indicator, or update it
    ProgressHide,
    ChallengeShow,     // id, title, description, badge
    ChallengeHide,     // id
    GameCompleted,     // title: the game
    SubsetCompleted,   // title: the subset
    ServerError,       // title: the request, description: the server's words
    Disconnected,      // unlocks are waiting for the network
    Reconnected,       // and have been sent
  };
  Type type = Type::Note;
  u32 id = 0;
  std::string title;
  std::string description;
  u32 points = 0;
  std::string badge;          // the achievement's badge name; fetch_badge() gets the picture
  s64 unlock_time = 0;        // Unix seconds
  std::string progress;       // measured progress, "5/10" or "50%"
  bool hardcore = false;      // GameStarted: the mode the game plays in (Casual here, always)
  u32 unlocked = 0, total = 0, unsupported = 0;   // GameStarted
};

enum class State : u8 {
  Off,          // not started, or no transport on this device
  SignedOut,
  SigningIn,
  SignedIn,
  LoadingGame,
  Playing,      // a set is loaded and being evaluated
  NoSet,        // signed in, but RetroAchievements does not know this dump
  EmptySet,     // dump known, game has no published achievements yet
};

class Client {
public:
  Client();
  ~Client();
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  // False with a reason in `err` when there is no usable libcurl; not fatal.
  // `outbox` is where unconfirmed unlocks wait (cheevos_outbox.h); empty keeps none.
  bool start(std::string& err, const std::string& outbox = {});
  // Abandons a request in flight (within about a second) and joins the worker;
  // unlocks not yet confirmed stay in the outbox for the next start.
  void shutdown();

  State state() const;

  // The system played, named in every request's User-Agent from then on: before
  // start() for the sign-in to carry it. load_game() sets it too.
  void set_core(Core core);
  Core core() const;

  // Applies at next load_game(); call before it. rcheevos only reads it at load.
  void set_encore(bool on);
  bool encore() const;
  const char* transport_name() const;
  const std::string& unavailable_reason() const;

  // Asynchronous; watch state() and take_events().
  void sign_in(const std::string& username, const std::string& password);
  void sign_in_with_token(const std::string& username, const std::string& token);
  void sign_out();
  std::string username() const;
  std::string token() const;

  // Asynchronous. Evaluates `nds`' memory as RetroAchievements maps `core`'s
  // (cheevos_memory.h): the GBA's in GBA mode.
  void load_game(NDS& nds, const std::string& hash, Core core = Core::Nds);
  void unload_game();
  std::string game_title() const;
  u32 game_id() const;
  const std::string& game_hash() const;

  // Call once per emulated frame, right after NDS::run_frame() (a GBA frame in GBA mode).
  void frame();
  // Call instead of frame() while paused, at least once a second.
  void idle();

  // deserialize_progress() returns false on a mismatched blob; caller must
  // reset() rather than apply it, to avoid a false unlock.
  bool serialize_progress(std::vector<u8>& out) const;
  bool deserialize_progress(const u8* data, size_t size);
  void reset();

  std::vector<Event> take_events();

  // The badge image for `name`, from the shared cache or the media host, fetched and
  // decoded on the worker thread when it has nothing else to do; it arrives through
  // take_badges(). Nothing happens for a name already asked for this session.
  void fetch_badge(const std::string& name);
  std::vector<Badge> take_badges();

  struct Achievement {
    std::string title, description, progress;
    u32 id = 0, points = 0;
    bool unlocked = false;      // the account holds it
    bool active = false;        // armed, can trigger now (both true in encore)
    bool unsupported = false;   // a condition reads memory we do not back
  };
  struct Summary {
    u32 total = 0, unlocked = 0, active = 0, unsupported = 0;
    u32 points = 0, points_earned = 0;
  };
  std::vector<Achievement> achievements() const;
  static Summary summarize(const std::vector<Achievement>& list);
  Summary summary() const { return summarize(achievements()); }

  // Unlocks earned but not yet confirmed by the server, this session's and earlier ones'.
  size_t unconfirmed_unlocks() const;

  struct Impl;

private:
  std::unique_ptr<Impl> impl_;
};

} // namespace ds::cheevos
