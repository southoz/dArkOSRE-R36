// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Local wireless over the LAN: MpTransport carrying DS frames between
// instances, in melonDS's wire format (interoperable with real melonDS). One
// host, up to fifteen clients; discovery is UDP broadcast on port 7063, game
// traffic is ENet on port 7064.
//
// Runs on the emulation thread: process() drives ENet/discovery once a
// frame, sends are immediate, recvs poll; recv_replies blocks up to
// recv_timeout_ms -- the one place a session can stall a frame.
#pragma once

#include "core/io/wifi_transport.h"
#include <array>
#include <map>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

struct _ENetHost; struct _ENetPeer; struct _ENetPacket; struct _ENetEvent;

namespace ds::net {

class LanMp final : public io::MpTransport {
public:
  LanMp();
  ~LanMp() override;
  LanMp(const LanMp&) = delete;
  LanMp& operator=(const LanMp&) = delete;

  enum class PlayerStatus : u32 { None = 0, Client, Host, Connecting, Disconnected };
  // melonDS's Player, byte for byte (travels in the player-list command).
  struct Player {
    s32  id = 0;
    char name[32] = {};
    PlayerStatus status = PlayerStatus::None;
    u32  address = 0;         // IPv4, network order as ENet holds it
    bool is_local = false;
    u32  ping = 0;
  };
  static_assert(sizeof(Player) == 52, "melonDS wire layout");
  struct Session {           // one host seen by discovery
    u32 first_seen_ms = 0;
    u32 tick = 0;
    std::string name;
    u8 num_players = 0, max_players = 0, status = 0;
  };

  bool ok() const { return inited_; }
  bool active() const { return active_; }
  bool is_host() const { return is_host_; }
  const std::string& error() const { return err_; }

  bool start_discovery();
  void end_discovery();
  bool start_host(const std::string& player_name, int max_players);
  // Listen for beacons up to scan_ms; join the first heard, else host unless
  // host_fallback is false (menu GUEST: never silently becomes the host).
  enum class Role { None, Host, Guest };
  Role start_auto(const std::string& player_name, int scan_ms = 2500, int max_players = 16,
                  bool host_fallback = true);
  std::string peer_name() const { return peer_name_; }   // the session joined by start_auto
  bool start_client(const std::string& player_name, const std::string& host);
  void end_session();
  std::map<u32, Session> sessions();      // keyed by host IPv4 (host order)
  std::vector<Player> players();
  int num_players() const { return num_players_; }
  int max_players() const { return max_players_; }
  int my_id() const { return me_.id; }

  void process();                          // once per frame
  void scan_step();                        // discovery-only scan; call between start_discovery()/end_discovery()
  // Ends a scan_step() scan: joins the first session with room if any, ends
  // discovery either way. Non-blocking counterpart to start_auto; guest only, no host fallback.
  bool scan_join(const std::string& player_name);
  // Time the emulation thread has blocked in the MP reply/host-packet waits.
  unsigned wait_count() const { return wait_count_; }
  double wait_total_ms() const { return wait_total_ms_; }
  double wait_max_ms() const { return wait_max_ms_; }
  unsigned wait_timeouts() const { return wait_timeouts_; }   // waits that ran the whole recv timeout

  // MpTransport
  void begin() override;
  void end() override;
  int  send_packet(const u8* data, int len, u64 timestamp) override;
  int  recv_packet(u8* data, u64* timestamp) override;
  int  send_cmd(const u8* data, int len, u64 timestamp) override;
  int  send_reply(const u8* data, int len, u64 timestamp, u16 aid) override;
  int  send_ack(const u8* data, int len, u64 timestamp) override;
  int  recv_host_packet(u8* data, u64* timestamp) override;
  int  peek_host_packet(u8* data, u64* timestamp) override;
  u16  recv_replies(u8* data, u64 timestamp, u16 aidmask) override;

private:
  void process_discovery();
  u32  first_with_room(std::string* name);   // the first session heard with a free slot
  void poll_discovery(u32 tick);          // read every beacon waiting on the socket
  void host_update_player_list();
  void process_host_event(_ENetEvent& ev);
  void process_client_event(_ENetEvent& ev);
  void process_lan(int type);              // 0 = poll, 1 = drop stale non-regular, 2 = wait
  int  send_generic(u32 type, const u8* data, int len, u64 timestamp);
  int  recv_generic(u8* data, bool block, u64* timestamp);

  bool inited_ = false, active_ = false, is_host_ = false;
  std::string err_;
  _ENetHost* host_ = nullptr;
  std::array<_ENetPeer*, 16> peers_{};
  int discovery_fd_ = -1;
  u32 discovery_last_ms_ = 0;
  std::map<u32, Session> sessions_;
  std::mutex sessions_mutex_;
  std::array<Player, 16> players_{};
  int num_players_ = 0, max_players_ = 0;
  std::mutex players_mutex_;
  Player me_;
  u32 host_address_ = 0;
  u16 connected_mask_ = 0;
  static constexpr int RECV_TIMEOUT_MS = 25;
  static constexpr u32 STALE_MS = 64;   // stale-packet window; melonDS uses 16
  int last_host_id_ = -1;
  _ENetPeer* last_host_peer_ = nullptr;
  std::queue<_ENetPacket*> rx_;
  u32 frame_count_ = 0;
  std::string peer_name_;
  unsigned wait_count_ = 0, wait_timeouts_ = 0;
  double wait_total_ms_ = 0, wait_max_ms_ = 0;
};

} // namespace ds::net
