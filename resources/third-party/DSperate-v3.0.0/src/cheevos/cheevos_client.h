// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// RetroAchievements session: sign in, load a game's achievement set, evaluate
// it as the emulator runs, report unlocks. Casual mode only.
//
// Threading: rc_client and rcheevos callbacks run only on the frame() caller
// (the emulation thread). HTTP runs on one worker thread; frame() drains
// finished responses before calling rc_client_do_frame.
#pragma once

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "cheevos/cheevos_http.h"
#include "cheevos/cheevos_memory.h"
#include "core/types.h"

namespace ds { class NDS; }

namespace ds::cheevos {

// Something the player should be told about (frontend renders as a toast).
struct Message {
  enum class Kind : u8 { Unlock, Info, Problem, };
  Kind kind = Kind::Info;
  std::string text;
  std::string detail;
  u32 points = 0;   // unlock value; 0 for non-unlocks and 0-point achievements
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
  bool start(std::string& err);
  void shutdown();

  State state() const { return state_; }

  // Applies at next load_game(); call before it. rcheevos only reads it at load.
  void set_encore(bool on);
  bool encore() const;
  const char* transport_name() const;
  const std::string& unavailable_reason() const { return unavailable_; }

  // Asynchronous; watch state() and take_messages().
  void sign_in(const std::string& username, const std::string& password);
  void sign_in_with_token(const std::string& username, const std::string& token);
  void sign_out();
  std::string username() const;
  std::string token() const;

  // Asynchronous.
  void load_game(NDS& nds, const std::string& hash);
  void unload_game();
  std::string game_title() const;
  u32 game_id() const;
  const std::string& game_hash() const { return hash_; }

  // Call once per emulated frame, right after NDS::run_frame().
  void frame();
  // Call instead of frame() while paused, at least once a second.
  void idle();

  // deserialize_progress() returns false on a mismatched blob; caller must
  // reset() rather than apply it, to avoid a false unlock.
  bool serialize_progress(std::vector<u8>& out) const;
  bool deserialize_progress(const u8* data, size_t size);
  void reset();

  std::vector<Message> take_messages();

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
  Summary summary() const;

  // Called by rcheevos completion callbacks; public for that reason only.
  void on_signed_in(const std::string& display_name);
  void on_sign_in_failed(const std::string& why);
  void on_game_loaded(const std::string& title, u32 id);
  void on_no_set();
  void on_empty_set(const std::string& title, u32 id);
  void on_game_failed(const std::string& why);

  // Called by the .cpp's free-function trampolines; public for that reason only.
  void enqueue(Request req, void* callback, void* callback_data);
  void handle_event(const void* event);
  u32 read(u32 address, u8* buffer, u32 num_bytes);

private:
  void worker_loop();
  void post(Message::Kind kind, std::string text, std::string detail = {}, u32 points = 0);
  void drain_completions();

  struct Job {
    Request req;
    void* callback = nullptr;        // rc_client_server_callback_t
    void* callback_data = nullptr;
  };
  struct Completion {
    Response res;
    void* callback = nullptr;
    void* callback_data = nullptr;
  };

  void* client_ = nullptr;           // rc_client_t*
  std::unique_ptr<Backend> http_;
  Memory mem_;
  State state_ = State::Off;
  bool encore_ = false;
  std::string hash_;                 // the identity of the dump in the slot
  std::string unavailable_;

  std::thread worker_;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Job> jobs_;
  std::deque<Completion> done_;
  std::vector<Message> messages_;
  bool stop_ = false;
};

// Credential file is mode 0600: the token alone is enough to act as the
// player. Password is never stored.
struct Credentials {
  std::string username;
  std::string token;
  bool empty() const { return username.empty() || token.empty(); }
};
// `dir` is the config directory. A missing file yields empty credentials, not an error.
bool load_credentials(const std::string& dir, Credentials& out, std::string& err);
bool save_credentials(const std::string& dir, const Credentials& in, std::string& err);
void clear_credentials(const std::string& dir);

// Credentials the CFW already holds from its own sign-in (ROCKNIX
// system.cfg, RetroArch retroarch.cfg). Only the token is read; the
// cleartext password in these files is never touched. Never writes.
// read_cfw_credentials parses one file; import_cfw_credentials walks the
// known locations, reporting which one it used in `source`.

// Token file named via --cheevos-token: PPSSPP's token-only format, or our
// own "username\ntoken" format.
bool read_token_file(const std::string& path, Credentials& out, std::string& err);

bool read_cfw_credentials(const std::string& path, Credentials& out);
bool import_cfw_credentials(Credentials& out, std::string& source);

} // namespace ds::cheevos
