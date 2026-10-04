// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Port of melonDS's net/LAN.cpp (GPL-3.0-or-later, melonDS team); wire
// format must stay byte-identical to interoperate with a real melonDS.
#include "net/lan_mp.h"
#include <enet/enet.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ds::net {

namespace {
constexpr u32 kDiscoveryMagic = 0x444E414C;   // "LAND"
constexpr u32 kLanMagic       = 0x504E414C;   // "LANP"
constexpr u32 kPacketMagic    = 0x4946494E;   // "NIFI"
constexpr u32 kProtocolVersion = 1;
constexpr u32 kLocalhost = 0x0100007F;        // 127.0.0.1 in network order
constexpr u16 kDiscoveryPort = 7063, kLanPort = 7064;
enum { Chan_Cmd = 0, Chan_MP = 1 };
enum : u8 { Cmd_ClientInit = 1, Cmd_PlayerInfo, Cmd_PlayerList, Cmd_PlayerConnect, Cmd_PlayerDisconnect };

struct DiscoveryData {          // melonDS's, byte for byte
  u32 magic, version, tick;
  char session_name[64];
  u8 num_players, max_players, status;
  u8 pad_;
};
static_assert(sizeof(DiscoveryData) == 80, "melonDS wire layout");
struct MpPacketHeader {         // melonDS's MPPacketHeader
  u32 magic, sender_id, type, length;
  u64 timestamp;
};
static_assert(sizeof(MpPacketHeader) == 24, "melonDS wire layout");

u32 ms_now() {
  using namespace std::chrono;
  return static_cast<u32>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}
bool lan_log() { static const bool on = std::getenv("DS_WIFI_LOG") != nullptr; return on; }
#define LAN_LOG(...) do { if (lan_log()) std::fprintf(stderr, "[lan] " __VA_ARGS__); } while (0)
void put32(u8* p, u32 v) { p[0] = u8(v); p[1] = u8(v >> 8); p[2] = u8(v >> 16); p[3] = u8(v >> 24); }
u32 get32(const u8* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (u32(p[3]) << 24); }
} // namespace

LanMp::LanMp() {
  if (enet_initialize() != 0) { err_ = "enet_initialize failed"; return; }
  inited_ = true;
}
LanMp::~LanMp() {
  end_session();
  if (inited_) enet_deinitialize();
}

std::map<u32, LanMp::Session> LanMp::sessions() { std::lock_guard<std::mutex> l(sessions_mutex_); return sessions_; }

std::vector<LanMp::Player> LanMp::players() {
  std::lock_guard<std::mutex> l(players_mutex_);
  std::vector<Player> out;
  for (const Player& p : players_) {
    if (p.status == PlayerStatus::None) continue;
    Player q = p;
    if (q.id == me_.id) { q.is_local = true; q.address = kLocalhost; }
    else { q.is_local = false; if (q.status == PlayerStatus::Host) q.address = host_address_; }
    out.push_back(q);
  }
  return out;
}

// ---- discovery ---------------------------------------------------------------
bool LanMp::start_discovery() {
  if (!inited_) return false;
  discovery_fd_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (discovery_fd_ < 0) { err_ = "discovery socket"; return false; }
  int one = 1;
  ::setsockopt(discovery_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in sa{}; sa.sin_family = AF_INET; sa.sin_addr.s_addr = htonl(INADDR_ANY); sa.sin_port = htons(kDiscoveryPort);
  if (::bind(discovery_fd_, reinterpret_cast<sockaddr*>(&sa), sizeof sa) < 0 ||
      ::setsockopt(discovery_fd_, SOL_SOCKET, SO_BROADCAST, &one, sizeof one) < 0) {
    err_ = "discovery bind"; ::close(discovery_fd_); discovery_fd_ = -1; return false;
  }
  discovery_last_ms_ = ms_now();
  { std::lock_guard<std::mutex> l(sessions_mutex_); sessions_.clear(); }
  active_ = true;
  return true;
}
void LanMp::end_discovery() {
  if (discovery_fd_ >= 0) { ::close(discovery_fd_); discovery_fd_ = -1; }
  if (!is_host_) active_ = false;
}

void LanMp::process_discovery() {
  if (discovery_fd_ < 0) return;
  const u32 tick = ms_now();
  if (tick - discovery_last_ms_ < 1000) return;
  discovery_last_ms_ = tick;
  if (is_host_) {
    DiscoveryData b{};
    b.magic = kDiscoveryMagic; b.version = kProtocolVersion; b.tick = tick;
    std::snprintf(b.session_name, sizeof b.session_name, "%s's game", me_.name);
    b.num_players = static_cast<u8>(num_players_); b.max_players = static_cast<u8>(max_players_); b.status = 0;
    sockaddr_in sa{}; sa.sin_family = AF_INET; sa.sin_addr.s_addr = htonl(INADDR_BROADCAST); sa.sin_port = htons(kDiscoveryPort);
    ::sendto(discovery_fd_, &b, sizeof b, 0, reinterpret_cast<sockaddr*>(&sa), sizeof sa);
    return;
  }
  poll_discovery(tick);
}

void LanMp::poll_discovery(u32 tick) {
  std::lock_guard<std::mutex> l(sessions_mutex_);
  for (;;) {
    fd_set fd; FD_ZERO(&fd); FD_SET(discovery_fd_, &fd);
    timeval tv{0, 0};
    if (::select(discovery_fd_ + 1, &fd, nullptr, nullptr, &tv) <= 0) break;
    DiscoveryData b;
    sockaddr_in ra{}; socklen_t ralen = sizeof ra;
    const ssize_t n = ::recvfrom(discovery_fd_, &b, sizeof b, 0, reinterpret_cast<sockaddr*>(&ra), &ralen);
    if (n < static_cast<ssize_t>(sizeof b)) continue;
    if (b.magic != kDiscoveryMagic || b.version != kProtocolVersion) continue;
    if (b.max_players > 16 || b.num_players > b.max_players) continue;
    const u32 key = ntohl(ra.sin_addr.s_addr);
    auto it = sessions_.find(key);
    if (it != sessions_.end() && b.tick <= it->second.tick) continue;
    b.session_name[63] = '\0';
    Session& s = sessions_[key];
    s.first_seen_ms = tick; s.tick = b.tick; s.name = b.session_name;
    s.num_players = b.num_players; s.max_players = b.max_players; s.status = b.status;
  }
  for (auto it = sessions_.begin(); it != sessions_.end();) {
    if (tick - it->second.first_seen_ms >= 5000) it = sessions_.erase(it); else ++it;
  }
}

LanMp::Role LanMp::start_auto(const std::string& player_name, int scan_ms, int max_players,
                              bool host_fallback) {
  if (!start_discovery()) return Role::None;
  const u32 start = ms_now();
  u32 found = 0; std::string found_name;
  while (static_cast<int>(ms_now() - start) < scan_ms) {
    poll_discovery(ms_now());
    found = first_with_room(&found_name);
    if (found) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  end_discovery();
  if (found) {
    char ip[32]; std::snprintf(ip, sizeof ip, "%u.%u.%u.%u", found >> 24, (found >> 16) & 255, (found >> 8) & 255, found & 255);
    LAN_LOG("found \"%s\" at %s\n", found_name.c_str(), ip);
    if (start_client(player_name, ip)) { peer_name_ = found_name; return Role::Guest; }
    LAN_LOG("join failed (%s)%s\n", err_.c_str(), host_fallback ? "; hosting instead" : "");
  }
  if (!host_fallback) {
    if (err_.empty()) err_ = "no session heard on the LAN";
    return Role::None;
  }
  return start_host(player_name, max_players) ? Role::Host : Role::None;
}

// ---- session -----------------------------------------------------------------
bool LanMp::start_host(const std::string& player_name, int max_players) {
  if (!inited_ || max_players > 16 || max_players < 1) return false;
  ENetAddress addr; addr.host = ENET_HOST_ANY; addr.port = kLanPort;
  host_ = enet_host_create(&addr, 16, 2, 0, 0);
  if (!host_) { err_ = "enet_host_create (port 7064 busy?)"; return false; }
  {
    std::lock_guard<std::mutex> l(players_mutex_);
    players_.fill(Player{});
    Player& p = players_[0];
    p.id = 0; std::strncpy(p.name, player_name.c_str(), 31); p.status = PlayerStatus::Host; p.address = kLocalhost;
    num_players_ = 1; max_players_ = max_players;
    me_ = p;
  }
  host_address_ = kLocalhost;
  last_host_id_ = -1; last_host_peer_ = nullptr;
  active_ = true; is_host_ = true;
  start_discovery();
  LAN_LOG("hosting as \"%s\", up to %d players\n", me_.name, max_players);
  return true;
}

bool LanMp::start_client(const std::string& player_name, const std::string& hostname) {
  if (!inited_) return false;
  host_ = enet_host_create(nullptr, 16, 2, 0, 0);
  if (!host_) { err_ = "enet_host_create"; return false; }
  ENetAddress addr;
  if (enet_address_set_host(&addr, hostname.c_str()) != 0) { err_ = "unknown host " + hostname; enet_host_destroy(host_); host_ = nullptr; return false; }
  addr.port = kLanPort;
  ENetPeer* peer = enet_host_connect(host_, &addr, 2, 0);
  if (!peer) { err_ = "enet_host_connect"; enet_host_destroy(host_); host_ = nullptr; return false; }
  {
    std::lock_guard<std::mutex> l(players_mutex_);
    me_ = Player{};
    std::strncpy(me_.name, player_name.c_str(), 31); me_.status = PlayerStatus::Connecting;
  }
  ENetEvent ev;
  int conn = 0;
  const u32 start = ms_now();
  const int conn_timeout = 5000;
  for (;;) {
    const u32 cur = ms_now();
    const int timeout = conn_timeout - static_cast<int>(cur - start);
    if (timeout < 0) break;
    if (enet_host_service(host_, &ev, static_cast<u32>(timeout)) <= 0) break;
    if (conn == 0 && ev.type == ENET_EVENT_TYPE_CONNECT) { conn = 1; continue; }
    if (conn == 1 && ev.type == ENET_EVENT_TYPE_RECEIVE) {
      const u8* d = ev.packet->data;
      const bool init = ev.channelID == Chan_Cmd && d[0] == Cmd_ClientInit && ev.packet->dataLength == 11 &&
                        get32(d + 1) == kLanMagic && get32(d + 5) == kProtocolVersion && d[10] <= 16;
      if (!init) { enet_packet_destroy(ev.packet); continue; }
      max_players_ = d[10];
      me_.id = d[9];
      enet_packet_destroy(ev.packet);
      u8 cmd[9 + sizeof(Player)];
      cmd[0] = Cmd_PlayerInfo; put32(cmd + 1, kLanMagic); put32(cmd + 5, kProtocolVersion);
      std::memcpy(cmd + 9, &me_, sizeof(Player));
      enet_peer_send(ev.peer, Chan_Cmd, enet_packet_create(cmd, sizeof cmd, ENET_PACKET_FLAG_RELIABLE));
      conn = 2;
      break;
    }
    if (ev.type == ENET_EVENT_TYPE_DISCONNECT) { conn = 0; break; }
    if (ev.type == ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(ev.packet);
  }
  if (conn != 2) {
    err_ = conn == 0 ? "no answer from " + hostname : "host refused";
    enet_peer_reset(peer); enet_host_destroy(host_); host_ = nullptr;
    return false;
  }
  host_address_ = addr.host;
  last_host_id_ = -1; last_host_peer_ = nullptr;
  peers_[0] = peer;
  peer->data = &players_[0];
  active_ = true; is_host_ = false;
  LAN_LOG("joined %s as player %d \"%s\"\n", hostname.c_str(), me_.id, me_.name);
  return true;
}

void LanMp::end_session() {
  if (!active_) return;
  if (is_host_) end_discovery();
  active_ = false;
  while (!rx_.empty()) { enet_packet_destroy(rx_.front()); rx_.pop(); }
  for (int i = 0; i < 16; ++i) {
    if (i == me_.id) continue;
    if (peers_[i]) enet_peer_disconnect(peers_[i], 0);
    peers_[i] = nullptr;
  }
  if (host_) { enet_host_flush(host_); enet_host_destroy(host_); host_ = nullptr; }
  is_host_ = false;
}

void LanMp::host_update_player_list() {
  u8 cmd[2 + sizeof(players_)];
  cmd[0] = Cmd_PlayerList; cmd[1] = static_cast<u8>(num_players_);
  std::memcpy(cmd + 2, players_.data(), sizeof(players_));
  enet_host_broadcast(host_, Chan_Cmd, enet_packet_create(cmd, sizeof cmd, ENET_PACKET_FLAG_RELIABLE));
}

void LanMp::process_host_event(ENetEvent& ev) {
  switch (ev.type) {
  case ENET_EVENT_TYPE_CONNECT: {
    if (num_players_ >= max_players_ || num_players_ >= 16) { enet_peer_disconnect(ev.peer, 0); break; }
    int id = 0;
    for (; id < 16; ++id) { if (id >= num_players_) break; if (players_[id].status == PlayerStatus::None) break; }
    if (id >= 16) { enet_peer_disconnect(ev.peer, 0); break; }
    u8 cmd[11];
    cmd[0] = Cmd_ClientInit; put32(cmd + 1, kLanMagic); put32(cmd + 5, kProtocolVersion);
    cmd[9] = static_cast<u8>(id); cmd[10] = static_cast<u8>(max_players_);
    enet_peer_send(ev.peer, Chan_Cmd, enet_packet_create(cmd, 11, ENET_PACKET_FLAG_RELIABLE));
    {
      std::lock_guard<std::mutex> l(players_mutex_);
      players_[id].id = id; players_[id].status = PlayerStatus::Connecting; players_[id].address = ev.peer->address.host;
      ev.peer->data = &players_[id];
      num_players_++;
    }
    peers_[id] = ev.peer;
    LAN_LOG("client %d connecting\n", id);
    break;
  }
  case ENET_EVENT_TYPE_DISCONNECT: {
    Player* p = static_cast<Player*>(ev.peer->data);
    if (!p) break;
    connected_mask_ &= static_cast<u16>(~(1 << p->id));
    peers_[p->id] = nullptr;
    LAN_LOG("client %d left\n", p->id);
    p->id = 0; p->status = PlayerStatus::None;
    num_players_--;
    host_update_player_list();
    break;
  }
  case ENET_EVENT_TYPE_RECEIVE: {
    if (ev.packet->dataLength < 1) break;
    const u8* d = ev.packet->data;
    switch (d[0]) {
    case Cmd_PlayerInfo: {
      if (ev.packet->dataLength != 9 + sizeof(Player)) break;
      if (get32(d + 1) != kLanMagic || get32(d + 5) != kProtocolVersion) { enet_peer_disconnect(ev.peer, 0); break; }
      Player p; std::memcpy(&p, d + 9, sizeof p); p.name[31] = '\0';
      Player* mine = static_cast<Player*>(ev.peer->data);
      if (!mine || p.id != mine->id) { enet_peer_disconnect(ev.peer, 0); break; }
      {
        std::lock_guard<std::mutex> l(players_mutex_);
        p.status = PlayerStatus::Client; p.address = ev.peer->address.host;
        *mine = p;
      }
      LAN_LOG("client %d is \"%s\"\n", p.id, p.name);
      host_update_player_list();
      // catch up a late joiner that missed the power-on connect broadcast
      if (connected_mask_ & (1 << me_.id)) { const u8 c = Cmd_PlayerConnect; enet_peer_send(ev.peer, Chan_Cmd, enet_packet_create(&c, 1, ENET_PACKET_FLAG_RELIABLE)); }
      break;
    }
    case Cmd_PlayerConnect: if (ev.packet->dataLength == 1) if (Player* p = static_cast<Player*>(ev.peer->data)) connected_mask_ |= static_cast<u16>(1 << p->id); break;
    case Cmd_PlayerDisconnect: if (ev.packet->dataLength == 1) if (Player* p = static_cast<Player*>(ev.peer->data)) connected_mask_ &= static_cast<u16>(~(1 << p->id)); break;
    default: break;
    }
    enet_packet_destroy(ev.packet);
    break;
  }
  default: break;
  }
}

void LanMp::process_client_event(ENetEvent& ev) {
  switch (ev.type) {
  case ENET_EVENT_TYPE_CONNECT: {
    int pid = -1;
    for (int i = 0; i < 16; ++i) {
      if (i == me_.id || players_[i].status != PlayerStatus::Client) continue;
      if (players_[i].address == ev.peer->address.host) { pid = i; break; }
    }
    if (pid < 0) { enet_peer_disconnect(ev.peer, 0); break; }
    peers_[pid] = ev.peer;
    ev.peer->data = &players_[pid];
    if (connected_mask_ & (1 << me_.id)) { const u8 c = Cmd_PlayerConnect; enet_peer_send(ev.peer, Chan_Cmd, enet_packet_create(&c, 1, ENET_PACKET_FLAG_RELIABLE)); }
    break;
  }
  case ENET_EVENT_TYPE_DISCONNECT: {
    Player* p = static_cast<Player*>(ev.peer->data);
    if (!p) break;
    connected_mask_ &= static_cast<u16>(~(1 << p->id));
    peers_[p->id] = nullptr;
    { std::lock_guard<std::mutex> l(players_mutex_); p->status = PlayerStatus::Disconnected; }
    break;
  }
  case ENET_EVENT_TYPE_RECEIVE: {
    if (ev.packet->dataLength < 1) break;
    const u8* d = ev.packet->data;
    switch (d[0]) {
    case Cmd_PlayerList: {
      if (ev.packet->dataLength != 2 + sizeof(players_) || d[1] > 16) break;
      {
        std::lock_guard<std::mutex> l(players_mutex_);
        num_players_ = d[1];
        std::memcpy(players_.data(), d + 2, sizeof(players_));
        for (Player& p : players_) p.name[31] = '\0';
      }
      // clients connect directly to each other; host only introduces them
      for (int i = 0; i < 16; ++i) {
        if (i == me_.id || players_[i].status != PlayerStatus::Client || peers_[i]) continue;
        ENetAddress pa; pa.host = players_[i].address; pa.port = kLanPort;
        enet_host_connect(host_, &pa, 2, 0);
      }
      break;
    }
    case Cmd_PlayerConnect: if (ev.packet->dataLength == 1) if (Player* p = static_cast<Player*>(ev.peer->data)) connected_mask_ |= static_cast<u16>(1 << p->id); break;
    case Cmd_PlayerDisconnect: if (ev.packet->dataLength == 1) if (Player* p = static_cast<Player*>(ev.peer->data)) connected_mask_ &= static_cast<u16>(~(1 << p->id)); break;
    default: break;
    }
    enet_packet_destroy(ev.packet);
    break;
  }
  default: break;
  }
}

void LanMp::process_lan(int type) {
  if (!host_) return;
  struct WaitClock {
    LanMp& l; int type; std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    ~WaitClock() { if (type != 2) return; const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); l.wait_count_++; l.wait_total_ms_ += ms; if (ms > l.wait_max_ms_) l.wait_max_ms_ = ms; if (ms >= RECV_TIMEOUT_MS - 1) l.wait_timeouts_++; }
  } wait_clock{*this, type};
  u32 time_last = ms_now();
  while (!rx_.empty()) {
    ENetPacket* pkt = rx_.front();
    auto* h = reinterpret_cast<MpPacketHeader*>(pkt->data);
    const u32 packettime = h->magic;   // overwritten with the receive time on arrival
    // Bounds the backlog rather than dropping on real-time lag.
    if (packettime > time_last || packettime < time_last - STALE_MS) { rx_.pop(); enet_packet_destroy(pkt); continue; }
    if (type == 2) return;
    if (type == 1) {
      if (h->type == 0) return;
      rx_.pop(); enet_packet_destroy(pkt);
    }
    break;
  }
  int timeout = type == 2 ? RECV_TIMEOUT_MS : 0;
  time_last = ms_now();
  ENetEvent ev;
  while (enet_host_service(host_, &ev, static_cast<u32>(timeout)) > 0) {
    if (ev.type == ENET_EVENT_TYPE_RECEIVE && ev.channelID == Chan_MP) {
      auto* h = reinterpret_cast<MpPacketHeader*>(ev.packet->data);
      const bool good = ev.packet->dataLength >= sizeof(MpPacketHeader) && h->magic == kPacketMagic && h->sender_id != static_cast<u32>(me_.id);
      if (!good) { enet_packet_destroy(ev.packet); }
      else {
        h->magic = ms_now();
        ev.packet->userData = ev.peer;
        rx_.push(ev.packet);
        return;
      }
    } else {
      if (is_host_) process_host_event(ev); else process_client_event(ev);
    }
    if (type == 2) {
      const u32 t = ms_now();
      if (t < time_last) return;
      timeout -= static_cast<int>(t - time_last);
      if (timeout <= 0) return;
      time_last = t;
    }
  }
}

u32 LanMp::first_with_room(std::string* name) {
  std::lock_guard<std::mutex> l(sessions_mutex_);
  for (const auto& [ip, s] : sessions_)
    if (s.num_players < s.max_players) { if (name) *name = s.name; return ip; }
  return 0;
}

bool LanMp::scan_join(const std::string& player_name) {
  std::string found_name;
  const u32 found = first_with_room(&found_name);
  end_discovery();
  if (!found) { err_ = "no session heard on the LAN"; return false; }
  char ip[32]; std::snprintf(ip, sizeof ip, "%u.%u.%u.%u", found >> 24, (found >> 16) & 255, (found >> 8) & 255, found & 255);
  LAN_LOG("found \"%s\" at %s\n", found_name.c_str(), ip);
  if (!start_client(player_name, ip)) return false;
  peer_name_ = found_name;
  return true;
}

void LanMp::scan_step() {
  if (!active_) process_discovery();
}

void LanMp::process() {
  if (!active_) return;
  process_discovery();
  process_lan(0);
  if (++frame_count_ >= 60) {
    frame_count_ = 0;
    std::lock_guard<std::mutex> l(players_mutex_);
    for (int i = 0; i < 16; ++i) {
      if (players_[i].status == PlayerStatus::None || i == me_.id || !peers_[i]) continue;
      players_[i].ping = peers_[i]->roundTripTime;
    }
  }
}

// ---- MpTransport -------------------------------------------------------------
void LanMp::begin() {
  if (!host_) return;
  connected_mask_ |= static_cast<u16>(1 << me_.id);
  last_host_id_ = -1; last_host_peer_ = nullptr;
  const u8 cmd = Cmd_PlayerConnect;
  enet_host_broadcast(host_, Chan_Cmd, enet_packet_create(&cmd, 1, ENET_PACKET_FLAG_RELIABLE));
}
void LanMp::end() {
  if (!host_) return;
  connected_mask_ &= static_cast<u16>(~(1 << me_.id));
  const u8 cmd = Cmd_PlayerDisconnect;
  enet_host_broadcast(host_, Chan_Cmd, enet_packet_create(&cmd, 1, ENET_PACKET_FLAG_RELIABLE));
}

int LanMp::send_generic(u32 type, const u8* data, int len, u64 timestamp) {
  if (!host_) return 0;
  ENetPacket* pkt = enet_packet_create(nullptr, sizeof(MpPacketHeader) + len, ENET_PACKET_FLAG_UNSEQUENCED);
  MpPacketHeader h{kPacketMagic, static_cast<u32>(me_.id), type, static_cast<u32>(len), timestamp};
  std::memcpy(pkt->data, &h, sizeof h);
  if (len) std::memcpy(pkt->data + sizeof h, data, static_cast<size_t>(len));
  if ((type & 0xFFFF) == 2 && last_host_peer_) enet_peer_send(last_host_peer_, Chan_MP, pkt);
  else enet_host_broadcast(host_, Chan_MP, pkt);
  enet_host_flush(host_);
  return len;
}

int LanMp::recv_generic(u8* data, bool block, u64* timestamp) {
  if (!host_) return 0;
  process_lan(block ? 2 : 1);
  if (rx_.empty()) return 0;
  ENetPacket* pkt = rx_.front(); rx_.pop();
  auto* h = reinterpret_cast<MpPacketHeader*>(pkt->data);
  u32 len = h->length;
  if (len) {
    if (len > 2048) len = 2048;
    std::memcpy(data, pkt->data + sizeof(MpPacketHeader), len);
    if (h->type == 1) { last_host_id_ = static_cast<int>(h->sender_id); last_host_peer_ = static_cast<ENetPeer*>(pkt->userData); }
  }
  if (timestamp) *timestamp = h->timestamp;
  enet_packet_destroy(pkt);
  return static_cast<int>(len);
}

int LanMp::send_packet(const u8* data, int len, u64 timestamp) { return send_generic(0, data, len, timestamp); }
int LanMp::recv_packet(u8* data, u64* timestamp) { return recv_generic(data, false, timestamp); }
int LanMp::send_cmd(const u8* data, int len, u64 timestamp) {
  // drop stale queued replies so they aren't mistaken for an answer to this CMD
  std::queue<ENetPacket*> keep;
  while (!rx_.empty()) {
    ENetPacket* pkt = rx_.front(); rx_.pop();
    if ((reinterpret_cast<MpPacketHeader*>(pkt->data)->type & 0xFFFF) == 2) enet_packet_destroy(pkt);
    else keep.push(pkt);
  }
  rx_.swap(keep);
  return send_generic(1, data, len, timestamp);
}
int LanMp::send_reply(const u8* data, int len, u64 timestamp, u16 aid) { return send_generic(2 | (u32(aid) << 16), data, len, timestamp); }
int LanMp::send_ack(const u8* data, int len, u64 timestamp) { return send_generic(3, data, len, timestamp); }
int LanMp::peek_host_packet(u8* data, u64* timestamp) {
  if (!host_) return 0;
  if (last_host_id_ != -1 && !(connected_mask_ & (1 << last_host_id_))) return -1;
  process_lan(0);
  if (rx_.empty()) return 0;
  if (reinterpret_cast<MpPacketHeader*>(rx_.front()->data)->type == 0) return 0;   // regular frame: leave for the client's own poll
  ENetPacket* pkt = rx_.front(); rx_.pop();
  auto* h = reinterpret_cast<MpPacketHeader*>(pkt->data);
  u32 len = h->length;
  if (len) {
    if (len > 2048) len = 2048;
    std::memcpy(data, pkt->data + sizeof(MpPacketHeader), len);
    if (h->type == 1) { last_host_id_ = static_cast<int>(h->sender_id); last_host_peer_ = static_cast<ENetPeer*>(pkt->userData); }
  }
  if (timestamp) *timestamp = h->timestamp;
  enet_packet_destroy(pkt);
  return static_cast<int>(len);
}

int LanMp::recv_host_packet(u8* data, u64* timestamp) {
  if (last_host_id_ != -1 && !(connected_mask_ & (1 << last_host_id_))) return -1;
  return recv_generic(data, true, timestamp);
}

u16 LanMp::recv_replies(u8* packets, u64 timestamp, u16 aidmask) {
  if (!host_) return 0;
  u16 ret = 0;
  u16 seen = static_cast<u16>(1 << me_.id);
  if ((seen & connected_mask_) == connected_mask_) return 0;
  for (;;) {
    process_lan(2);
    if (rx_.empty()) return ret;
    ENetPacket* pkt = rx_.front(); rx_.pop();
    auto* h = reinterpret_cast<MpPacketHeader*>(pkt->data);
    const bool good = (h->type & 0xFFFF) == 2 && h->timestamp >= timestamp - 32;
    if (good) {
      u32 len = h->length;
      if (len) {
        if (len > 1024) len = 1024;
        const u32 aid = h->type >> 16;
        if (aid >= 1 && aid <= 15) { std::memcpy(&packets[(aid - 1) * 1024], pkt->data + sizeof(MpPacketHeader), len); ret |= static_cast<u16>(1 << aid); }
      }
      seen |= static_cast<u16>(1 << h->sender_id);
      if ((seen & connected_mask_) == connected_mask_ || (ret & aidmask) == aidmask) { enet_packet_destroy(pkt); return ret; }
    }
    enet_packet_destroy(pkt);
  }
}

} // namespace ds::net
