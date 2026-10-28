// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "cheevos/cheevos_client.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <mutex>
#include <thread>

#include <pthread.h>
#include <sched.h>
#include <strings.h>
#include <unistd.h>

#include "cheevos/cheevos_http.h"
#include "cheevos/cheevos_memory.h"
#include "cheevos/cheevos_outbox.h"
#include "core/nds.h"
#include "core/off_realtime.h"
#include "rc_api_runtime.h"
#include "rc_client.h"

namespace ds::cheevos {
namespace {

bool verbose() {
  static const bool on = [] {
    const char* e = std::getenv("DS_CHEEVOS_VERBOSE");
    return e && std::atoi(e) != 0;
  }();
  return on;
}

// CLOG only with DS_CHEEVOS_VERBOSE; WLOG always: what went wrong is worth a line.
#define CLOG(...) do { if (verbose()) std::fprintf(stderr, "cheevos: " __VA_ARGS__); } while (0)
#define WLOG(...) std::fprintf(stderr, "cheevos: " __VA_ARGS__)

// "Warning" achievement ids; rcheevos excludes them from summaries. Casual-only
// build, so suppress the toast but keep it in the log.
constexpr u32 WARNING_ACHIEVEMENT_ID = 101000001;
bool is_warning(u32 id) { return id >= WARNING_ACHIEVEMENT_ID; }

s64 now_seconds() { return static_cast<s64>(std::time(nullptr)); }

bool same_user(const std::string& a, const std::string& b) {
  return a.size() == b.size() && ::strncasecmp(a.c_str(), b.c_str(), a.size()) == 0;
}

} // namespace

struct Client::Impl {
  struct Job {
    Request req;
    const char* user_agent = nullptr;
    std::string badge;                             // set: a badge fetch, not an rc_api request
    rc_client_server_callback_t callback = nullptr;
    void* callback_data = nullptr;
    u32 award = 0;                                 // an award request: the achievement
    std::string award_user;
    bool replay = false;                           // an award from the outbox, not from rc_client
  };
  struct Completion {
    Response res;
    Job job;
  };

  rc_client_t* rc = nullptr;
  std::unique_ptr<Backend> http;
  Memory mem;
  State state = State::Off;
  bool encore = false;
  Core core = Core::Nds;
  std::string hash;                  // the identity of the dump in the slot
  std::string unavailable;
  Outbox outbox;
  std::vector<u32> replaying;        // outbox awards in flight

  std::thread worker;
  mutable std::mutex mu;
  std::condition_variable cv;
  std::deque<Job> jobs;              // rcheevos' requests: always first
  std::deque<Job> badge_jobs;        // badges: when there is nothing else
  std::deque<Completion> done;
  std::vector<Event> events;
  std::vector<Badge> badges;
  std::vector<std::string> badges_asked;
  std::atomic<bool> stop{false};

  void post(Event e) {
    std::lock_guard<std::mutex> lk(mu);
    events.push_back(std::move(e));
  }
  void post(Event::Type type, std::string title = {}, std::string description = {}) {
    Event e;
    e.type = type;
    e.title = std::move(title);
    e.description = std::move(description);
    post(std::move(e));
  }

  void enqueue(Job job) {
    {
      std::lock_guard<std::mutex> lk(mu);
      jobs.push_back(std::move(job));
    }
    cv.notify_one();
  }

  void worker_loop();
  void drain_completions();
  void settle_award(const Job& job, const Response& res);
  void replay_outbox();
  void handle_event(const rc_client_event_t* e);
  std::vector<Achievement> list() const;

  void on_signed_in(const std::string& display_name);
  void on_game_loaded(const std::string& title, u32 id);
};

namespace {

Client::Impl* impl_of(const rc_client_t* c) {
  return static_cast<Client::Impl*>(rc_client_get_userdata(c));
}

uint32_t rc_read_memory(uint32_t address, uint8_t* buffer, uint32_t num_bytes, rc_client_t* c) {
  Client::Impl* self = impl_of(c);
  if (!self) { std::memset(buffer, 0, num_bytes); return 0; }
  return self->mem.read(address, buffer, num_bytes);
}

// On the emulation thread, inside rc_client. An award is written to the outbox before
// it is sent: if the answer never comes, the next start sends it again.
void rc_server_call(const rc_api_request_t* request, rc_client_server_callback_t callback,
                    void* callback_data, rc_client_t* c) {
  Client::Impl* self = impl_of(c);
  if (!self || !request) return;
  Client::Impl::Job job;
  job.req.url = request->url ? request->url : "";
  job.req.body = request->post_data ? request->post_data : "";
  job.req.content_type = request->content_type ? request->content_type : "";
  job.user_agent = user_agent(self->core);
  job.callback = callback;
  job.callback_data = callback_data;
  PendingUnlock p;
  if (parse_award_request(job.req.body, now_seconds(), p)) {
    job.award = p.achievement;
    job.award_user = p.user;
    self->outbox.add(p);
  }
  self->enqueue(std::move(job));
}

void rc_event_handler(const rc_client_event_t* event, rc_client_t* c) {
  Client::Impl* self = impl_of(c);
  if (self && event) self->handle_event(event);
}

// rcheevos' warnings and errors always reach the log; its chatter only with DS_CHEEVOS_VERBOSE.
void rc_log(const char* message, const rc_client_t* c) {
  (void)c;
  std::fprintf(stderr, "cheevos: rc: %s\n", message ? message : "");
}

} // namespace

// ---------------------------------------------------------------------------
// Session

Client::Client() : impl_(std::make_unique<Impl>()) {}
Client::~Client() { shutdown(); }

State Client::state() const { return impl_->state; }
const char* Client::transport_name() const { return impl_->http ? impl_->http->name() : "none"; }
const std::string& Client::unavailable_reason() const { return impl_->unavailable; }
const std::string& Client::game_hash() const { return impl_->hash; }

void Client::set_core(Core core) { impl_->core = core; }
Core Client::core() const { return impl_->core; }

void Client::set_encore(bool on) {
  impl_->encore = on;
  if (impl_->rc) rc_client_set_encore_mode_enabled(impl_->rc, on ? 1 : 0);
}

bool Client::encore() const {
  if (!impl_->rc) return impl_->encore;
  return rc_client_get_encore_mode_enabled(impl_->rc) != 0;
}

std::vector<Event> Client::take_events() {
  std::lock_guard<std::mutex> lk(impl_->mu);
  std::vector<Event> out;
  out.swap(impl_->events);
  return out;
}

bool Client::start(std::string& err, const std::string& outbox) {
  Impl& m = *impl_;
  if (m.rc) return true;
  err.clear();

  m.http = make_curl_backend(err);
  if (!m.http) {
    m.unavailable = "no HTTP support on this device (" + err + ")";
    m.state = State::Off;
    return false;
  }

  rc_client_t* c = rc_client_create(&rc_read_memory, &rc_server_call);
  if (!c) {
    err = "rc_client_create failed";
    m.unavailable = err;
    m.http.reset();
    return false;
  }
  m.rc = c;
  rc_client_set_userdata(c, &m);

  // rc_client defaults hardcore ON; force off since save states/cheats would
  // corrupt a hardcore unlock claim on the server.
  rc_client_set_hardcore_enabled(c, 0);

  // Restricts guest memory reads to the emulation thread (do_frame/idle).
  rc_client_set_allow_background_memory_reads(c, 0);

  rc_client_set_encore_mode_enabled(c, m.encore ? 1 : 0);

  rc_client_enable_logging(c, verbose() ? RC_CLIENT_LOG_LEVEL_VERBOSE : RC_CLIENT_LOG_LEVEL_WARN, &rc_log);
  rc_client_set_event_handler(c, &rc_event_handler);

  if (!outbox.empty()) {
    std::string oerr;
    if (!m.outbox.open(outbox, oerr)) WLOG("unlocks earned offline will not outlast this run: %s\n", oerr.c_str());
    else if (!m.outbox.pending().empty())
      WLOG("%zu unlock%s from an earlier run waiting to be sent (%s)\n", m.outbox.pending().size(),
           m.outbox.pending().size() == 1 ? "" : "s", outbox.c_str());
  }

  m.stop = false;
  m.worker = std::thread([&m] { m.worker_loop(); });
  m.state = State::SignedOut;
  CLOG("session up, transport %s, user agent %s\n", transport_name(), user_agent(m.core));
  return true;
}

void Client::shutdown() {
  Impl& m = *impl_;
  if (m.worker.joinable()) {
    {
      std::lock_guard<std::mutex> lk(m.mu);
      m.stop = true;   // also ends a request in flight (Backend::perform's cancel)
    }
    m.cv.notify_all();
    m.worker.join();
  }
  if (m.rc) {
    // Queued work is dropped; calling back into rc_client mid-teardown would crash.
    // An award among it is still in the outbox.
    rc_client_destroy(m.rc);
    m.rc = nullptr;
  }
  {
    std::lock_guard<std::mutex> lk(m.mu);
    m.jobs.clear();
    m.badge_jobs.clear();
    m.done.clear();
  }
  m.replaying.clear();
  m.http.reset();
  m.state = State::Off;
}

size_t Client::unconfirmed_unlocks() const { return impl_->outbox.pending().size(); }

// ---------------------------------------------------------------------------
// The worker

void Client::Impl::worker_loop() {
  // May inherit SCHED_RR from the process; a thread blocking on a socket must not
  // hold real-time priority.
#if defined(__linux__)
  sched_param sp{};
  sp.sched_priority = 0;
  if (pthread_setschedparam(pthread_self(), SCHED_OTHER, &sp) != 0 && verbose())
    std::perror("cheevos: worker SCHED_OTHER");
  if (nice(10) == -1 && errno != 0 && verbose()) std::perror("cheevos: worker nice");
#endif

  for (;;) {
    Job job;
    {
      std::unique_lock<std::mutex> lk(mu);
      cv.wait(lk, [this] { return stop || !jobs.empty() || !badge_jobs.empty(); });
      if (stop) return;
      // A badge only when no unlock, ping or sign-in is waiting: it is decoration.
      std::deque<Job>& from = jobs.empty() ? badge_jobs : jobs;
      job = std::move(from.front());
      from.pop_front();
    }

    if (!job.badge.empty()) {
      Badge b;
      std::string err;
      const BadgeResult r = load_badge(job.badge, *http, job.user_agent, &stop, b, err);
      std::lock_guard<std::mutex> lk(mu);
      if (r == BadgeResult::Ok) badges.push_back(std::move(b));
      else {
        CLOG("badge %s: %s\n", job.badge.c_str(), err.c_str());
        // Not had: asked again at the next unlock that wants it (the network may be back).
        if (r == BadgeResult::NotFetched)
          badges_asked.erase(std::remove(badges_asked.begin(), badges_asked.end(), job.badge), badges_asked.end());
      }
      continue;
    }
    CLOG("-> %s%s\n", job.req.url.c_str(), job.req.body.empty() ? "" : " (POST)");
    Response res = http->perform(job.req, job.user_agent, &stop);
    if (res.cancelled) return;
    CLOG("<- status %d, %zu bytes%s%s\n", res.status, res.body.size(),
         res.error.empty() ? "" : ", ", res.error.c_str());
    job.req = Request{};

    std::lock_guard<std::mutex> lk(mu);
    done.push_back(Completion{std::move(res), std::move(job)});
  }
}

void Client::fetch_badge(const std::string& name) {
  Impl& m = *impl_;
  if (!valid_badge_name(name) || !m.worker.joinable()) return;
  {
    std::lock_guard<std::mutex> lk(m.mu);
    for (const std::string& n : m.badges_asked) if (n == name) return;
    m.badges_asked.push_back(name);
    Impl::Job job;
    job.badge = name;
    job.user_agent = user_agent(m.core);
    m.badge_jobs.push_back(std::move(job));
  }
  m.cv.notify_one();
}

std::vector<Badge> Client::take_badges() {
  std::lock_guard<std::mutex> lk(impl_->mu);
  std::vector<Badge> out;
  out.swap(impl_->badges);
  return out;
}

// On the emulation thread: the outbox is only ever touched here and in rc_server_call.
void Client::Impl::settle_award(const Job& job, const Response& res) {
  if (job.replay) replaying.erase(std::remove(replaying.begin(), replaying.end(), job.award), replaying.end());
  if (!award_settled(res.status)) {
    if (job.replay)
      WLOG("the unlock of achievement %u earned offline is still not sent (%s); kept for later\n", job.award,
           res.error.empty() ? ("status " + std::to_string(res.status)).c_str() : res.error.c_str());
    return;
  }
  outbox.remove(job.award_user, job.award);
  if (job.replay) WLOG("sent the unlock of achievement %u earned offline (status %d)\n", job.award, res.status);
}

// Runs rcheevos' handling of each answer, on the emulation thread. A big set takes
// rcheevos longer than RLIMIT_RTTIME allows a real-time thread (Bubble Bobble Double
// Shot's 446 KB on a Cortex-A55), so any answers are handled as an ordinary task.
void Client::Impl::drain_completions() {
  {
    std::lock_guard<std::mutex> lk(mu);
    if (done.empty()) return;
  }
  const OffRealtime off_rt;
  for (;;) {
    Completion d;
    {
      std::lock_guard<std::mutex> lk(mu);
      if (done.empty()) return;
      d = std::move(done.front());
      done.pop_front();
    }
    if (d.job.award) settle_award(d.job, d.res);
    if (!d.job.callback) continue;
    rc_api_server_response_t res{};
    if (d.res.status == 0) {
      // Never reached the server (DNS, connect, TLS, a timeout): rcheevos retries a
      // retryable client error, and the curl error is the body it logs.
      res.http_status_code = RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
      if (d.res.error.empty()) d.res.error = "no response";
      res.body = d.res.error.c_str();
      res.body_length = d.res.error.size();
    } else {
      res.http_status_code = d.res.status;
      res.body = d.res.body.c_str();
      res.body_length = d.res.body.size();
    }
    d.job.callback(&res, d.job.callback_data);
  }
}

// The earlier runs' unconfirmed unlocks for the account now signed in, signed with its token.
void Client::Impl::replay_outbox() {
  if (!rc || !outbox.is_open()) return;
  const rc_client_user_t* u = rc_client_get_user_info(rc);
  if (!u || !u->username || !u->token) return;
  const s64 now = now_seconds();
  for (const PendingUnlock& p : outbox.pending()) {
    if (!same_user(p.user, u->username)) continue;
    if (std::find(replaying.begin(), replaying.end(), p.achievement) != replaying.end()) continue;
    rc_api_award_achievement_request_t params{};
    params.username = u->username;
    params.api_token = u->token;
    params.achievement_id = p.achievement;
    params.hardcore = p.hardcore ? 1 : 0;
    params.game_hash = p.hash.empty() ? nullptr : p.hash.c_str();
    const s64 ago = now > p.unlocked_at ? now - p.unlocked_at : 0;
    params.seconds_since_unlock = static_cast<uint32_t>(ago > 0xFFFFFFFFll ? 0xFFFFFFFFll : ago);
    rc_api_request_t request{};
    if (rc_api_init_award_achievement_request(&request, &params) != RC_OK) continue;
    Job job;
    job.req.url = request.url ? request.url : "";
    job.req.body = request.post_data ? request.post_data : "";
    job.req.content_type = request.content_type ? request.content_type : "";
    job.user_agent = user_agent(core);
    job.award = p.achievement;
    job.award_user = p.user;
    job.replay = true;
    rc_api_destroy_request(&request);
    replaying.push_back(p.achievement);
    CLOG("sending the unlock of achievement %u from an earlier run (%lld s ago)\n", p.achievement,
         static_cast<long long>(ago));
    enqueue(std::move(job));
  }
}

// ---------------------------------------------------------------------------
// Per-frame

void Client::frame() {
  if (!impl_->rc) return;
  impl_->drain_completions();
  rc_client_do_frame(impl_->rc);
}

void Client::idle() {
  if (!impl_->rc) return;
  impl_->drain_completions();
  rc_client_idle(impl_->rc);
}

// ---------------------------------------------------------------------------
// Sign-in

namespace {

void login_done(int result, const char* error_message, rc_client_t* c, void* userdata) {
  (void)userdata;
  Client::Impl* self = impl_of(c);
  if (!self) return;
  if (result == RC_OK) {
    const rc_client_user_t* u = rc_client_get_user_info(c);
    self->on_signed_in(u && u->display_name ? u->display_name : "");
  } else {
    self->state = State::SignedOut;
    self->post(Event::Type::SignInFailed, {}, error_message ? error_message : "sign-in failed");
  }
}

void load_done(int result, const char* error_message, rc_client_t* c, void* userdata) {
  (void)userdata;
  Client::Impl* self = impl_of(c);
  if (!self) return;
  if (result == RC_OK) {
    const rc_client_game_t* g = rc_client_get_game_info(c);
    const std::string title = g && g->title ? g->title : "";
    const u32 id = g ? g->id : 0;
    // Zero achievements on RC_OK means a known dump with no published set. No popup:
    // nothing has gone wrong, nothing to do.
    if (self->list().empty()) {
      self->state = State::EmptySet;
      CLOG("game %u known (%s) but has no published achievements\n", id, title.c_str());
    } else {
      self->on_game_loaded(title, id);
    }
  } else if (result == RC_NO_GAME_LOADED) {
    self->state = State::NoSet;
    // Hash identifies a dump, not a game; a legit ROM can still fail to match.
    self->post(Event::Type::NoSet, {}, self->hash);
  } else {
    self->state = State::SignedIn;
    self->post(Event::Type::LoadFailed, {}, error_message ? error_message : "could not load achievements");
  }
}

} // namespace

void Client::Impl::on_signed_in(const std::string& display_name) {
  state = State::SignedIn;
  post(Event::Type::SignedIn, display_name);
  CLOG("signed in as %s\n", display_name.c_str());
  replay_outbox();
}

// The placard: RetroAchievements asks that the mode be shown when a game starts.
void Client::Impl::on_game_loaded(const std::string& title, u32 id) {
  state = State::Playing;
  const Summary s = summarize(list());
  Event e;
  e.type = Event::Type::GameStarted;
  e.id = id;
  e.title = title;
  e.hardcore = rc_client_get_hardcore_enabled(rc) != 0;
  e.unlocked = s.unlocked;
  e.total = s.total;
  e.unsupported = s.unsupported;
  post(std::move(e));
  CLOG("game %u loaded: %s, %u/%u unlocked, %u unsupported\n", id, title.c_str(), s.unlocked, s.total, s.unsupported);
}

void Client::sign_in(const std::string& username, const std::string& password) {
  if (!impl_->rc) return;
  impl_->state = State::SigningIn;
  rc_client_begin_login_with_password(impl_->rc, username.c_str(), password.c_str(), &login_done, nullptr);
}

void Client::sign_in_with_token(const std::string& username, const std::string& token) {
  if (!impl_->rc) return;
  impl_->state = State::SigningIn;
  rc_client_begin_login_with_token(impl_->rc, username.c_str(), token.c_str(), &login_done, nullptr);
}

void Client::sign_out() {
  if (!impl_->rc) return;
  rc_client_logout(impl_->rc);
  impl_->state = State::SignedOut;
  impl_->post(Event::Type::SignedOut);
}

std::string Client::username() const {
  if (!impl_->rc) return {};
  const rc_client_user_t* u = rc_client_get_user_info(impl_->rc);
  return u && u->username ? u->username : std::string{};
}

std::string Client::token() const {
  if (!impl_->rc) return {};
  const rc_client_user_t* u = rc_client_get_user_info(impl_->rc);
  return u && u->token ? u->token : std::string{};
}

// ---------------------------------------------------------------------------
// Game

void Client::load_game(NDS& nds, const std::string& hash, Core core) {
  Impl& m = *impl_;
  if (!m.rc) return;
  std::string err;
  if (!m.mem.attach(nds, err)) {
    m.post(Event::Type::Unavailable, "Achievements unavailable", err);
    return;
  }
  m.hash = hash;
  m.core = core;
  m.state = State::LoadingGame;
  rc_client_begin_load_game(m.rc, hash.c_str(), &load_done, nullptr);
}

void Client::unload_game() {
  Impl& m = *impl_;
  if (!m.rc) return;
  rc_client_unload_game(m.rc);
  if (m.state == State::Playing || m.state == State::LoadingGame || m.state == State::NoSet ||
      m.state == State::EmptySet)
    m.state = State::SignedIn;
}

std::string Client::game_title() const {
  if (!impl_->rc) return {};
  const rc_client_game_t* g = rc_client_get_game_info(impl_->rc);
  return g && g->title ? g->title : std::string{};
}

u32 Client::game_id() const {
  if (!impl_->rc) return 0;
  const rc_client_game_t* g = rc_client_get_game_info(impl_->rc);
  return g ? g->id : 0;
}

// ---------------------------------------------------------------------------
// The loaded set

std::vector<Client::Achievement> Client::Impl::list() const {
  std::vector<Achievement> out;
  if (!rc) return out;
  // Core category only; unofficial achievements are opt-in separately.
  rc_client_achievement_list_t* l =
      rc_client_create_achievement_list(rc, RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE,
                                        RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
  if (!l) return out;
  for (u32 b = 0; b < l->num_buckets; ++b) {
    const rc_client_achievement_bucket_t& bucket = l->buckets[b];
    for (u32 i = 0; i < bucket.num_achievements; ++i) {
      const rc_client_achievement_t* a = bucket.achievements[i];
      if (!a || is_warning(a->id)) continue;
      Achievement info;
      info.title = a->title ? a->title : "";
      info.description = a->description ? a->description : "";
      info.progress = a->measured_progress;
      info.id = a->id;
      info.points = a->points;
      // unlocked = account holds it; active = can trigger now (both true in encore).
      info.unlocked = a->state == RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED ||
                      (a->unlocked & RC_CLIENT_ACHIEVEMENT_UNLOCKED_SOFTCORE) != 0;
      info.active = a->state == RC_CLIENT_ACHIEVEMENT_STATE_ACTIVE;
      info.unsupported = a->state == RC_CLIENT_ACHIEVEMENT_STATE_DISABLED ||
                         bucket.bucket_type == RC_CLIENT_ACHIEVEMENT_BUCKET_UNSUPPORTED;
      out.push_back(std::move(info));
    }
  }
  rc_client_destroy_achievement_list(l);
  return out;
}

std::vector<Client::Achievement> Client::achievements() const { return impl_->list(); }

Client::Summary Client::summarize(const std::vector<Achievement>& list) {
  Summary s;
  for (const Achievement& a : list) {
    ++s.total;
    s.points += a.points;
    if (a.unlocked) { ++s.unlocked; s.points_earned += a.points; }
    if (a.active) ++s.active;
    if (a.unsupported) ++s.unsupported;
  }
  return s;
}

// ---------------------------------------------------------------------------
// Progress, for save states

bool Client::serialize_progress(std::vector<u8>& out) const {
  out.clear();
  rc_client_t* c = impl_->rc;
  if (!c || !rc_client_get_game_info(c)) return false;
  const size_t n = rc_client_progress_size(c);
  if (n == 0) return false;
  out.resize(n);
  if (rc_client_serialize_progress_sized(c, out.data(), out.size()) != RC_OK) {
    out.clear();
    return false;
  }
  return true;
}

bool Client::deserialize_progress(const u8* data, size_t size) {
  rc_client_t* c = impl_->rc;
  if (!c || !data || size == 0 || !rc_client_get_game_info(c)) return false;
  // rcheevos validates against the loaded set, so callers can reset() on failure.
  return rc_client_deserialize_progress_sized(c, data, size) == RC_OK;
}

void Client::reset() {
  if (impl_->rc) rc_client_reset(impl_->rc);
}

// ---------------------------------------------------------------------------
// Events from rcheevos

void Client::Impl::handle_event(const rc_client_event_t* e) {
  const rc_client_achievement_t* a = e->achievement;
  auto achievement_event = [&](Event::Type type) {
    Event ev;
    ev.type = type;
    ev.id = a->id;
    ev.title = a->title ? a->title : "";
    ev.description = a->description ? a->description : "";
    ev.points = a->points;
    ev.badge = a->badge_name;
    ev.progress = a->measured_progress;
    return ev;
  };
  // A "warning" achievement is not a real one: in the log, never on screen.
  if (a && is_warning(a->id)) {
    CLOG("ignoring event %u for warning achievement %u: %s\n", e->type, a->id, a->title ? a->title : "");
    return;
  }

  switch (e->type) {
    case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
      if (a) {
        Event ev = achievement_event(Event::Type::Unlock);
        ev.unlock_time = a->unlock_time ? static_cast<s64>(a->unlock_time) : now_seconds();
        post(std::move(ev));
      }
      break;
    case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_SHOW:
    case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_UPDATE:
      if (a) post(achievement_event(Event::Type::Progress));
      break;
    case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_HIDE:
      post(Event::Type::ProgressHide);
      break;
    case RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_SHOW:
      if (a) post(achievement_event(Event::Type::ChallengeShow));
      break;
    case RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_HIDE:
      if (a) post(achievement_event(Event::Type::ChallengeHide));
      break;
    case RC_CLIENT_EVENT_GAME_COMPLETED: {
      const rc_client_game_t* g = rc_client_get_game_info(rc);
      post(Event::Type::GameCompleted, g && g->title ? g->title : "");
      break;
    }
    case RC_CLIENT_EVENT_SUBSET_COMPLETED:
      post(Event::Type::SubsetCompleted, e->subset && e->subset->title ? e->subset->title : "");
      break;
    case RC_CLIENT_EVENT_SERVER_ERROR:
      // Not requeued by rcheevos: the player is told.
      post(Event::Type::ServerError,
           e->server_error && e->server_error->api ? e->server_error->api : "",
           e->server_error && e->server_error->error_message ? e->server_error->error_message : "");
      break;
    case RC_CLIENT_EVENT_DISCONNECTED:
      post(Event::Type::Disconnected);
      break;
    case RC_CLIENT_EVENT_RECONNECTED:
      post(Event::Type::Reconnected);
      replay_outbox();   // the network is back: the earlier runs' too
      break;
    case RC_CLIENT_EVENT_RESET:
      CLOG("ignoring a reset event; hardcore is not supported\n");
      break;
    default:
      // Leaderboards: rc_client submits them itself; nothing is drawn for them yet.
      CLOG("event %u not shown\n", e->type);
      break;
  }
}

} // namespace ds::cheevos
