// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "cheevos/cheevos_client.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core/nds.h"
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

#define CLOG(...) do { if (verbose()) std::fprintf(stderr, "cheevos: " __VA_ARGS__); } while (0)

const char* token_name = "/cheevos.token";

// "Warning" achievement ids; rcheevos excludes them from summaries. Casual-only
// build, so suppress the toast but keep it in the log.
constexpr u32 WARNING_ACHIEVEMENT_ID = 101000001;
bool is_warning(u32 id) { return id >= WARNING_ACHIEVEMENT_ID; }

Client* session_of(const rc_client_t* c) {
  return static_cast<Client*>(rc_client_get_userdata(c));
}

uint32_t rc_read_memory(uint32_t address, uint8_t* buffer, uint32_t num_bytes, rc_client_t* c) {
  Client* self = session_of(c);
  if (!self) { std::memset(buffer, 0, num_bytes); return 0; }
  return self->read(address, buffer, num_bytes);
}

void rc_server_call(const rc_api_request_t* request, rc_client_server_callback_t callback,
                    void* callback_data, rc_client_t* c) {
  Client* self = session_of(c);
  if (!self || !request) return;
  Request req;
  req.url = request->url ? request->url : "";
  req.body = request->post_data ? request->post_data : "";
  req.content_type = request->content_type ? request->content_type : "";
  self->enqueue(std::move(req), reinterpret_cast<void*>(callback), callback_data);
}

void rc_event_handler(const rc_client_event_t* event, rc_client_t* c) {
  Client* self = session_of(c);
  if (self) self->handle_event(event);
}

void rc_log(const char* message, const rc_client_t* c) {
  (void)c;
  std::fprintf(stderr, "cheevos: rc: %s\n", message ? message : "");
}

} // namespace

// ---------------------------------------------------------------------------
// Credentials

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

// ---------------------------------------------------------------------------
// Session

Client::Client() = default;
Client::~Client() { shutdown(); }

const char* Client::transport_name() const { return http_ ? http_->name() : "none"; }

void Client::set_encore(bool on) {
  encore_ = on;
  if (client_) rc_client_set_encore_mode_enabled(static_cast<rc_client_t*>(client_), on ? 1 : 0);
}

bool Client::encore() const {
  if (!client_) return encore_;
  return rc_client_get_encore_mode_enabled(static_cast<rc_client_t*>(client_)) != 0;
}

void Client::post(Message::Kind kind, std::string text, std::string detail, u32 points) {
  std::lock_guard<std::mutex> lk(mu_);
  messages_.push_back(Message{kind, std::move(text), std::move(detail), points});
}

std::vector<Message> Client::take_messages() {
  std::lock_guard<std::mutex> lk(mu_);
  std::vector<Message> out;
  out.swap(messages_);
  return out;
}

bool Client::start(std::string& err) {
  if (client_) return true;
  err.clear();

  http_ = make_curl_backend(err);
  if (!http_) {
    unavailable_ = "no HTTP support on this device (" + err + ")";
    state_ = State::Off;
    return false;
  }

  rc_client_t* c = rc_client_create(&rc_read_memory, &rc_server_call);
  if (!c) {
    err = "rc_client_create failed";
    unavailable_ = err;
    http_.reset();
    return false;
  }
  client_ = c;
  rc_client_set_userdata(c, this);

  // rc_client defaults hardcore ON; force off since save states/cheats would
  // corrupt a hardcore unlock claim on the server.
  rc_client_set_hardcore_enabled(c, 0);

  // Restricts guest memory reads to the emulation thread (do_frame/idle).
  rc_client_set_allow_background_memory_reads(c, 0);

  rc_client_set_encore_mode_enabled(c, encore_ ? 1 : 0);

  if (verbose()) rc_client_enable_logging(c, RC_CLIENT_LOG_LEVEL_VERBOSE, &rc_log);
  rc_client_set_event_handler(c, &rc_event_handler);

  worker_ = std::thread([this] { worker_loop(); });
  state_ = State::SignedOut;
  CLOG("session up, transport %s, user agent %s\n", transport_name(), user_agent());
  return true;
}

void Client::shutdown() {
  if (worker_.joinable()) {
    {
      std::lock_guard<std::mutex> lk(mu_);
      stop_ = true;
    }
    cv_.notify_all();
    worker_.join();
  }
  if (client_) {
    // Queued work is dropped; calling back into rc_client mid-teardown would crash.
    rc_client_destroy(static_cast<rc_client_t*>(client_));
    client_ = nullptr;
  }
  {
    std::lock_guard<std::mutex> lk(mu_);
    jobs_.clear();
    done_.clear();
  }
  http_.reset();
  state_ = State::Off;
}

// ---------------------------------------------------------------------------
// The worker

void Client::worker_loop() {
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
      std::unique_lock<std::mutex> lk(mu_);
      cv_.wait(lk, [this] { return stop_ || !jobs_.empty(); });
      if (stop_) return;
      job = std::move(jobs_.front());
      jobs_.pop_front();
    }

    CLOG("-> %s%s\n", job.req.url.c_str(), job.req.body.empty() ? "" : " (POST)");
    Response res = http_->perform(job.req, user_agent());
    CLOG("<- status %d, %zu bytes%s%s\n", res.status, res.body.size(),
         res.error.empty() ? "" : ", ", res.error.c_str());

    {
      std::lock_guard<std::mutex> lk(mu_);
      done_.push_back(Completion{std::move(res), job.callback, job.callback_data});
    }
  }
}

void Client::enqueue(Request req, void* callback, void* callback_data) {
  Job job;
  job.req = std::move(req);
  job.callback = callback;
  job.callback_data = callback_data;
  {
    std::lock_guard<std::mutex> lk(mu_);
    jobs_.push_back(std::move(job));
  }
  cv_.notify_one();
}

void Client::drain_completions() {
  for (;;) {
    Completion done;
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (done_.empty()) return;
      done = std::move(done_.front());
      done_.pop_front();
    }
    rc_api_server_response_t res{};
    res.body = done.res.body.c_str();
    res.body_length = done.res.body.size();
    // Status 0 (never reached server) is treated by rcheevos as retryable.
    res.http_status_code = done.res.status;
    auto cb = reinterpret_cast<rc_client_server_callback_t>(done.callback);
    if (cb) cb(&res, done.callback_data);
  }
}

// ---------------------------------------------------------------------------
// Per-frame

void Client::frame() {
  if (!client_) return;
  drain_completions();
  rc_client_do_frame(static_cast<rc_client_t*>(client_));
}

void Client::idle() {
  if (!client_) return;
  drain_completions();
  rc_client_idle(static_cast<rc_client_t*>(client_));
}

u32 Client::read(u32 address, u8* buffer, u32 num_bytes) {
  return mem_.read(address, buffer, num_bytes);
}

// ---------------------------------------------------------------------------
// Sign-in

namespace {

void login_done(int result, const char* error_message, rc_client_t* c, void* userdata) {
  (void)userdata;
  Client* self = static_cast<Client*>(rc_client_get_userdata(c));
  if (!self) return;
  if (result == RC_OK) {
    const rc_client_user_t* u = rc_client_get_user_info(c);
    self->on_signed_in(u && u->display_name ? u->display_name : "");
  } else {
    self->on_sign_in_failed(error_message ? error_message : "sign-in failed");
  }
}

void load_done(int result, const char* error_message, rc_client_t* c, void* userdata) {
  (void)userdata;
  Client* self = static_cast<Client*>(rc_client_get_userdata(c));
  if (!self) return;
  if (result == RC_OK) {
    const rc_client_game_t* g = rc_client_get_game_info(c);
    const std::string title = g && g->title ? g->title : "";
    const u32 id = g ? g->id : 0;
    // Zero achievements on RC_OK means a known dump with no published set.
    if (self->achievements().empty()) self->on_empty_set(title, id);
    else self->on_game_loaded(title, id);
  } else if (result == RC_NO_GAME_LOADED) {
    self->on_no_set();
  } else {
    self->on_game_failed(error_message ? error_message : "could not load achievements");
  }
}

} // namespace

void Client::sign_in(const std::string& username, const std::string& password) {
  if (!client_) return;
  state_ = State::SigningIn;
  rc_client_begin_login_with_password(static_cast<rc_client_t*>(client_), username.c_str(),
                                      password.c_str(), &login_done, nullptr);
}

void Client::sign_in_with_token(const std::string& username, const std::string& token) {
  if (!client_) return;
  state_ = State::SigningIn;
  rc_client_begin_login_with_token(static_cast<rc_client_t*>(client_), username.c_str(),
                                   token.c_str(), &login_done, nullptr);
}

void Client::sign_out() {
  if (!client_) return;
  rc_client_logout(static_cast<rc_client_t*>(client_));
  state_ = State::SignedOut;
  post(Message::Kind::Info, "Signed out of RetroAchievements");
}

std::string Client::username() const {
  if (!client_) return {};
  const rc_client_user_t* u = rc_client_get_user_info(static_cast<rc_client_t*>(client_));
  return u && u->username ? u->username : std::string{};
}

std::string Client::token() const {
  if (!client_) return {};
  const rc_client_user_t* u = rc_client_get_user_info(static_cast<rc_client_t*>(client_));
  return u && u->token ? u->token : std::string{};
}

// ---------------------------------------------------------------------------
// Game

void Client::load_game(NDS& nds, const std::string& hash) {
  if (!client_) return;
  std::string err;
  if (!mem_.attach(nds, err)) {
    post(Message::Kind::Problem, "Achievements unavailable", err);
    return;
  }
  hash_ = hash;
  state_ = State::LoadingGame;
  rc_client_begin_load_game(static_cast<rc_client_t*>(client_), hash.c_str(), &load_done, nullptr);
}

void Client::unload_game() {
  if (!client_) return;
  rc_client_unload_game(static_cast<rc_client_t*>(client_));
  if (state_ == State::Playing || state_ == State::LoadingGame || state_ == State::NoSet ||
      state_ == State::EmptySet)
    state_ = State::SignedIn;
}

std::string Client::game_title() const {
  if (!client_) return {};
  const rc_client_game_t* g = rc_client_get_game_info(static_cast<rc_client_t*>(client_));
  return g && g->title ? g->title : std::string{};
}

u32 Client::game_id() const {
  if (!client_) return 0;
  const rc_client_game_t* g = rc_client_get_game_info(static_cast<rc_client_t*>(client_));
  return g ? g->id : 0;
}

// ---------------------------------------------------------------------------
// Outcomes

void Client::on_signed_in(const std::string& display_name) {
  state_ = State::SignedIn;
  post(Message::Kind::Info, "Signed in to RetroAchievements",
       display_name.empty() ? std::string{} : display_name);
  CLOG("signed in as %s\n", display_name.c_str());
}

void Client::on_sign_in_failed(const std::string& why) {
  state_ = State::SignedOut;
  post(Message::Kind::Problem, "RetroAchievements sign-in failed", why);
}

void Client::on_game_loaded(const std::string& title, u32 id) {
  state_ = State::Playing;
  post(Message::Kind::Info, title.empty() ? "Achievements loaded" : title,
       "achievements active");
  CLOG("game %u loaded: %s\n", id, title.c_str());
}

// No popup: nothing has gone wrong, nothing to do.
void Client::on_empty_set(const std::string& title, u32 id) {
  state_ = State::EmptySet;
  CLOG("game %u known (%s) but has no published achievements\n", id, title.c_str());
}

void Client::on_no_set() {
  state_ = State::NoSet;
  // Hash identifies a dump, not a game; a legit ROM can still fail to match.
  post(Message::Kind::Problem, "No achievements for this ROM",
       hash_.empty() ? "RetroAchievements does not recognise this dump"
                     : "RetroAchievements does not recognise this dump (hash " + hash_ + ")");
}

void Client::on_game_failed(const std::string& why) {
  state_ = State::SignedIn;
  post(Message::Kind::Problem, "Could not load achievements", why);
}

// ---------------------------------------------------------------------------
// The loaded set

std::vector<Client::Achievement> Client::achievements() const {
  std::vector<Achievement> out;
  if (!client_) return out;
  rc_client_t* c = static_cast<rc_client_t*>(client_);
  // Core category only; unofficial achievements are opt-in separately.
  rc_client_achievement_list_t* list =
      rc_client_create_achievement_list(c, RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE,
                                        RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
  if (!list) return out;
  for (u32 b = 0; b < list->num_buckets; ++b) {
    const rc_client_achievement_bucket_t& bucket = list->buckets[b];
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
  rc_client_destroy_achievement_list(list);
  return out;
}

Client::Summary Client::summary() const {
  Summary s;
  for (const Achievement& a : achievements()) {
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
  if (!client_) return false;
  rc_client_t* c = static_cast<rc_client_t*>(client_);
  if (!rc_client_get_game_info(c)) return false;
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
  if (!client_ || !data || size == 0) return false;
  rc_client_t* c = static_cast<rc_client_t*>(client_);
  if (!rc_client_get_game_info(c)) return false;
  // rcheevos validates against the loaded set, so callers can reset() on failure.
  return rc_client_deserialize_progress_sized(c, data, size) == RC_OK;
}

void Client::reset() {
  if (client_) rc_client_reset(static_cast<rc_client_t*>(client_));
}

// ---------------------------------------------------------------------------
// Events from rcheevos

void Client::handle_event(const void* event_ptr) {
  const rc_client_event_t* e = static_cast<const rc_client_event_t*>(event_ptr);
  if (!e) return;
  Client* self = this;

  switch (e->type) {
    case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
      if (e->achievement && is_warning(e->achievement->id)) {
        CLOG("ignoring warning achievement %u: %s\n", e->achievement->id,
             e->achievement->title ? e->achievement->title : "");
      } else if (e->achievement) {
        self->post(Message::Kind::Unlock,
                   e->achievement->title ? e->achievement->title : "Achievement unlocked",
                   e->achievement->description ? e->achievement->description : "",
                   e->achievement->points);
      }
      break;
    case RC_CLIENT_EVENT_GAME_COMPLETED:
      self->post(Message::Kind::Info, "All achievements earned", self->game_title());
      break;
    case RC_CLIENT_EVENT_SUBSET_COMPLETED:
      self->post(Message::Kind::Info, "Subset completed");
      break;
    case RC_CLIENT_EVENT_SERVER_ERROR:
      self->post(Message::Kind::Problem, "RetroAchievements error",
                 e->server_error && e->server_error->error_message ? e->server_error->error_message : "");
      break;
    case RC_CLIENT_EVENT_DISCONNECTED:
      self->post(Message::Kind::Problem, "RetroAchievements offline", "unlocks will be sent when the connection returns");
      break;
    case RC_CLIENT_EVENT_RECONNECTED:
      self->post(Message::Kind::Info, "RetroAchievements reconnected", "pending unlocks sent");
      break;
    case RC_CLIENT_EVENT_RESET:
      CLOG("ignoring a reset event; hardcore is not supported\n");
      break;
    default:
      break;
  }
}

} // namespace ds::cheevos
