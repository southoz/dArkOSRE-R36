// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
#include "net/slirp_driver.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <arpa/inet.h>
#include <poll.h>

extern "C" {
#include "libslirp.h"
}

namespace ds::net {
namespace {

bool slirp_log() { static const bool on = std::getenv("DS_WIFI_LOG") != nullptr; return on; }
#define SLIRP_LOG(...) do { if (slirp_log()) std::fprintf(stderr, "[slirp] " __VA_ARGS__); } while (0)

// slirp's conventional subnet, host order.
constexpr u32 kNetwork    = 0x0A000200;   // 10.0.2.0/24
constexpr u32 kNetmask    = 0xFFFFFF00;
constexpr u32 kHost       = 0x0A000202;   // 10.0.2.2, the gateway
constexpr u32 kDhcpStart  = 0x0A00020F;   // 10.0.2.15, what the DS is leased
constexpr u32 kNameserver = SlirpDriver::kVirtualDns;   // 10.0.2.3

constexpr std::size_t kMaxRxQueue = 64;   // frames; TCP retransmits past a drop

in_addr addr_of(u32 host_order) {
  in_addr a{};
  a.s_addr = htonl(host_order);
  return a;
}

u16 ld16be(const u8* p) { return static_cast<u16>((p[0] << 8) | p[1]); }
void st16be(u8* p, u16 v) { p[0] = static_cast<u8>(v >> 8); p[1] = static_cast<u8>(v); }
u32 ld32be(const u8* p) {
  return (static_cast<u32>(p[0]) << 24) | (static_cast<u32>(p[1]) << 16) |
         (static_cast<u32>(p[2]) << 8) | p[3];
}
void st32be(u8* p, u32 v) {
  p[0] = static_cast<u8>(v >> 24); p[1] = static_cast<u8>(v >> 16);
  p[2] = static_cast<u8>(v >> 8);  p[3] = static_cast<u8>(v);
}

// RFC 1624: fold a 32-bit address change into a one's-complement checksum
// without recomputing it over the whole datagram.
u16 checksum_patch(u16 old_sum, u32 old_addr, u32 new_addr) {
  u32 sum = static_cast<u16>(~old_sum);
  sum += static_cast<u16>(~(old_addr >> 16) & 0xFFFF) + static_cast<u16>(~old_addr & 0xFFFF);
  sum += (new_addr >> 16) + (new_addr & 0xFFFF);
  while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
  return static_cast<u16>(~sum);
}

// IPv4/UDP offsets of one Ethernet frame; false if not an IPv4 UDP datagram
// (fragment past the first has no UDP header).
struct UdpView {
  u8* ip = nullptr;      // start of the IPv4 header
  u8* udp = nullptr;     // start of the UDP header
};
bool udp_view(u8* frame, int len, UdpView* out) {
  if (len < 14 + 20 + 8) return false;
  if (ld16be(&frame[12]) != 0x0800) return false;          // not IPv4
  u8* ip = &frame[14];
  if ((ip[0] >> 4) != 4) return false;
  const int ihl = (ip[0] & 0xF) * 4;
  if (ihl < 20 || 14 + ihl + 8 > len) return false;
  if (ip[9] != 17) return false;                            // not UDP
  if ((ld16be(&ip[6]) & 0x1FFF) != 0) return false;         // a later fragment
  out->ip = ip;
  out->udp = ip + ihl;
  return true;
}

} // namespace

// A struct (not free functions) so members can reach SlirpDriver's privates
// as a friend, keeping libslirp's types out of the header.
struct SlirpCallbacks {
  static SlirpDriver* self(void* opaque) { return static_cast<SlirpDriver*>(opaque); }

  static ssize_t send_packet(const void* buf, size_t len, void* opaque) {
    SlirpDriver* d = self(opaque);
    if (len == 0 || len > SlirpDriver::kMaxFrame) return static_cast<ssize_t>(len);
    if (d->rx_.size() >= kMaxRxQueue) {
      // Dropping here is what libslirp expects; TCP backs off and retransmits.
      d->frames_dropped_++;
      return static_cast<ssize_t>(len);
    }
    d->rx_.emplace_back(static_cast<const u8*>(buf), static_cast<const u8*>(buf) + len);
    if (d->dns_ == SlirpDriver::Dns::Custom)
      SlirpDriver::rewrite_dns_src(d->rx_.back().data(), static_cast<int>(len), d->dns_addr_, kNameserver);
    d->frames_in_++;
    return static_cast<ssize_t>(len);
  }

  static void guest_error(const char* msg, void* opaque) {
    (void)opaque;
    SLIRP_LOG("guest error: %s\n", msg ? msg : "?");
  }

  static int64_t clock_get_ns(void* opaque) {
    (void)opaque;
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
  }

  static void* timer_new_opaque(SlirpTimerId id, void* cb_opaque, void* opaque) {
    SlirpDriver* d = self(opaque);
    d->timers_.push_back(SlirpDriver::Timer{static_cast<int>(id), cb_opaque, -1});
    return &d->timers_.back();
  }

  static void timer_free(void* timer, void* opaque) {
    SlirpDriver* d = self(opaque);
    // Disarm in place, don't erase: slirp keeps this pointer.
    for (auto& t : d->timers_) {
      if (&t == timer) { t.expire_ms = -1; t.cb_opaque = nullptr; return; }
    }
  }

  static void timer_mod(void* timer, int64_t expire_time, void* opaque) {
    (void)opaque;
    static_cast<SlirpDriver::Timer*>(timer)->expire_ms = expire_time;
  }

  static void notify(void* opaque) { (void)opaque; }

  // No-ops: process() hands libslirp a fresh poll set every call. Must be
  // non-null: at config version 4 libslirp calls these on every new socket.
  static void register_poll_fd(int fd, void* opaque) { (void)fd; (void)opaque; }
  static void unregister_poll_fd(int fd, void* opaque) { (void)fd; (void)opaque; }

  static int add_poll(slirp_os_socket fd, int events, void* opaque) {
    SlirpDriver* d = self(opaque);
    short ev = 0;
    if (events & SLIRP_POLL_IN)  ev |= POLLIN;
    if (events & SLIRP_POLL_OUT) ev |= POLLOUT;
    if (events & SLIRP_POLL_PRI) ev |= POLLPRI;
    if (events & SLIRP_POLL_ERR) ev |= POLLERR;
    if (events & SLIRP_POLL_HUP) ev |= POLLHUP;
    d->pollfds_.push_back(SlirpDriver::PollFd{static_cast<int>(fd), ev, 0});
    return static_cast<int>(d->pollfds_.size()) - 1;
  }

  static int get_revents(int idx, void* opaque) {
    SlirpDriver* d = self(opaque);
    if (idx < 0 || idx >= static_cast<int>(d->pollfds_.size())) return 0;
    const short rev = d->pollfds_[static_cast<std::size_t>(idx)].revents;
    int events = 0;
    if (rev & POLLIN)  events |= SLIRP_POLL_IN;
    if (rev & POLLOUT) events |= SLIRP_POLL_OUT;
    if (rev & POLLPRI) events |= SLIRP_POLL_PRI;
    if (rev & POLLERR) events |= SLIRP_POLL_ERR;
    if (rev & POLLHUP) events |= SLIRP_POLL_HUP;
    return events;
  }
};

SlirpDriver::SlirpDriver() = default;

SlirpDriver::~SlirpDriver() { stop(); }

bool SlirpDriver::start(Dns dns, u32 dns_addr) {
  if (slirp_) return true;
  dns_ = dns;
  dns_addr_ = dns_addr;
  if (dns_ == Dns::Custom && dns_addr_ == 0) {
    std::fprintf(stderr, "slirp: no DNS address given; using the host resolver\n");
    dns_ = Dns::Host;
  }

  SlirpConfig cfg{};
  cfg.version = 4;                 // timer_new_opaque; register_poll_socket needs 6
  cfg.restricted = 0;
  cfg.in_enabled = true;
  cfg.vnetwork = addr_of(kNetwork);
  cfg.vnetmask = addr_of(kNetmask);
  cfg.vhost = addr_of(kHost);
  cfg.vdhcp_start = addr_of(kDhcpStart);
  cfg.vnameserver = addr_of(kNameserver);
  cfg.vhostname = "DSperate";
  cfg.in6_enabled = false;         // the DS has no IPv6 stack
  cfg.if_mtu = kMtu;
  cfg.if_mru = kMtu;
  cfg.disable_host_loopback = false;

  static const SlirpCb cb = [] {
    SlirpCb c{};
    c.send_packet = &SlirpCallbacks::send_packet;
    c.guest_error = &SlirpCallbacks::guest_error;
    c.clock_get_ns = &SlirpCallbacks::clock_get_ns;
    c.timer_free = &SlirpCallbacks::timer_free;
    c.timer_mod = &SlirpCallbacks::timer_mod;
    c.notify = &SlirpCallbacks::notify;
    c.timer_new_opaque = &SlirpCallbacks::timer_new_opaque;
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
    c.register_poll_fd = &SlirpCallbacks::register_poll_fd;
    c.unregister_poll_fd = &SlirpCallbacks::unregister_poll_fd;
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
    return c;
  }();

  start_ns_ = SlirpCallbacks::clock_get_ns(this);
  slirp_ = slirp_new(&cfg, &cb, this);
  if (!slirp_) {
    err_ = "libslirp could not start";
    return false;
  }
  SLIRP_LOG("up -- DS 10.0.2.15, gateway 10.0.2.2, DNS %s\n",
            dns_ == Dns::Host ? "10.0.2.3 (host resolver)" : "rewritten");
  return true;
}

void SlirpDriver::stop() {
  if (slirp_) {
    slirp_cleanup(slirp_);
    slirp_ = nullptr;
  }
  timers_.clear();
  rx_.clear();
  pollfds_.clear();
}

s64 SlirpDriver::now_ms() const {
  return (SlirpCallbacks::clock_get_ns(const_cast<SlirpDriver*>(this)) - start_ns_) / 1000000;
}

void SlirpDriver::fire_due_timers() {
  const s64 now = now_ms();
  // id/cb_opaque copied and disarmed before the call: slirp_handle_timer may
  // re-arm this timer, which must not be undone afterward.
  for (auto& t : timers_) {
    if (t.expire_ms < 0 || t.expire_ms > now || !t.cb_opaque) continue;
    const int id = t.id;
    void* cb_opaque = t.cb_opaque;
    t.expire_ms = -1;
    slirp_handle_timer(slirp_, static_cast<SlirpTimerId>(id), cb_opaque);
  }
}

void SlirpDriver::pump() {
  if (!slirp_) return;

  pollfds_.clear();
  u32 timeout = 0;   // never wait: recv() is on the ARM7's timeline
  slirp_pollfds_fill_socket(slirp_, &timeout, &SlirpCallbacks::add_poll, this);

  int select_error = 0;
  if (!pollfds_.empty()) {
    static_assert(sizeof(PollFd) == sizeof(struct pollfd) &&
                  offsetof(PollFd, revents) == offsetof(struct pollfd, revents),
                  "PollFd must be poll(2)'s struct");
    const int n = ::poll(reinterpret_cast<struct pollfd*>(pollfds_.data()),
                         static_cast<nfds_t>(pollfds_.size()), 0);
    if (n < 0) select_error = 1;
  }
  slirp_pollfds_poll(slirp_, select_error, &SlirpCallbacks::get_revents, this);
  fire_due_timers();
}

void SlirpDriver::process() { pump(); }

int SlirpDriver::send(const u8* data, int len) {
  if (!slirp_ || len <= 0 || static_cast<std::size_t>(len) > kMaxFrame) return 0;
  if (dns_ == Dns::Custom) {
    u8 frame[kMaxFrame];
    std::memcpy(frame, data, static_cast<std::size_t>(len));
    if (rewrite_dns_dst(frame, len, kNameserver, dns_addr_)) {
      dns_rewritten_++;
      slirp_input(slirp_, frame, len);
      frames_out_++;
      pump();
      return len;
    }
  }
  slirp_input(slirp_, data, len);
  frames_out_++;
  pump();
  return len;
}

int SlirpDriver::recv(u8* data) {
  if (!slirp_) return 0;
  pump();
  if (rx_.empty()) return 0;
  const std::vector<u8>& frame = rx_.front();
  const int len = static_cast<int>(frame.size());
  std::memcpy(data, frame.data(), frame.size());
  rx_.pop_front();
  return len;
}

// ---- DNS policy ----
// Rewriting needs both directions: destination out, source back -- the reply
// must appear to come from the address the DS asked, or its socket won't match it.

bool SlirpDriver::rewrite_dns_dst(u8* frame, int len, u32 from, u32 to) {
  UdpView v;
  if (!udp_view(frame, len, &v)) return false;
  if (ld16be(&v.udp[2]) != 53) return false;                  // not a DNS query
  if (ld32be(&v.ip[16]) != from) return false;                // not ours to redirect

  st16be(&v.ip[10], checksum_patch(ld16be(&v.ip[10]), from, to));
  const u16 udp_sum = ld16be(&v.udp[6]);
  if (udp_sum != 0)   // 0 means no checksum was computed; leave it that way
    st16be(&v.udp[6], checksum_patch(udp_sum, from, to));
  st32be(&v.ip[16], to);
  return true;
}

bool SlirpDriver::rewrite_dns_src(u8* frame, int len, u32 from, u32 to) {
  UdpView v;
  if (!udp_view(frame, len, &v)) return false;
  if (ld16be(&v.udp[0]) != 53) return false;                  // not a DNS reply
  if (ld32be(&v.ip[12]) != from) return false;

  st16be(&v.ip[10], checksum_patch(ld16be(&v.ip[10]), from, to));
  const u16 udp_sum = ld16be(&v.udp[6]);
  if (udp_sum != 0)
    st16be(&v.udp[6], checksum_patch(udp_sum, from, to));
  st32be(&v.ip[12], to);
  return true;
}

} // namespace ds::net
