// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Internet through the emulated access point: NetDriver carrying the AP's
// Ethernet frames to/from a user-mode TCP/IP stack (vendored libslirp). Guest
// connections become ordinary host sockets, so it works on a wlan0-only
// handheld with no privileges needed. DS sees plain DHCP: 10.0.2.15/.2/.3
// (slirp's conventional addresses).
//
// NetDriver::recv runs on the ARM7's timeline, so nothing here may block:
// poll is zero-timeout. libslirp is not thread-safe; only this thread touches it.
#pragma once

#include "core/io/wifi_transport.h"
#include <cstddef>
#include <deque>
#include <string>
#include <vector>

struct Slirp;

namespace ds::net {

class SlirpDriver final : public io::NetDriver {
public:
  // DHCP always names slirp's 10.0.2.3; Custom rewrites queries addressed there.
  enum class Dns {
    Host,      // slirp answers 10.0.2.3 itself, forwarding to the host resolver
    Custom,    // rewritten to custom_addr
  };

  SlirpDriver();
  ~SlirpDriver() override;
  SlirpDriver(const SlirpDriver&) = delete;
  SlirpDriver& operator=(const SlirpDriver&) = delete;

  bool start(Dns dns = Dns::Host, u32 dns_addr = 0);   // dns_addr: host-order IPv4, ignored unless Custom
  void stop();
  bool ok() const { return slirp_ != nullptr; }
  const std::string& error() const { return err_; }

  void process();   // drives timers/sockets outside guest frame traffic; call once a frame

  // NetDriver
  int send(const u8* data, int len) override;
  int recv(u8* data) override;

  unsigned frames_out() const { return frames_out_; }
  unsigned frames_in() const { return frames_in_; }
  unsigned frames_dropped() const { return frames_dropped_; }   // rx queue was full
  unsigned dns_rewritten() const { return dns_rewritten_; }

  static constexpr u32 kVirtualDns = 0x0A000203;   // 10.0.2.3, host order

  // Rewrites DNS destination (outbound) / source (inbound) of one Ethernet
  // frame in place; true if it matched (IPv4 UDP port 53, from `from`) and rewrote.
  static bool rewrite_dns_dst(u8* frame, int len, u32 from, u32 to);
  static bool rewrite_dns_src(u8* frame, int len, u32 from, u32 to);

  // recv()'s max frame size, kept under the AP's 2048-byte buffer minus its
  // 40-byte 802.11 header (NetDriver::recv takes no buffer-length param).
  static constexpr std::size_t kMtu = 1500;
  static constexpr std::size_t kMaxFrame = kMtu + 14 + 4;

private:
  friend struct SlirpCallbacks;   // callbacks live in the .cpp, need libslirp's types

  struct Timer {
    int id = 0;
    void* cb_opaque = nullptr;
    s64 expire_ms = -1;      // < 0: not armed
  };

  void pump();               // fill, poll(0), process -- the whole stack step
  void fire_due_timers();
  s64  now_ms() const;

  Slirp* slirp_ = nullptr;
  std::string err_;
  Dns dns_ = Dns::Host;
  u32 dns_addr_ = 0;                 // host order
  std::deque<std::vector<u8>> rx_;   // frames from the stack waiting for the guest
  struct PollFd { int fd; short events; short revents; };
  std::vector<PollFd> pollfds_;
  unsigned frames_out_ = 0, frames_in_ = 0, frames_dropped_ = 0, dns_rewritten_ = 0;
  std::deque<Timer> timers_;         // deque: slirp holds pointers to these
  s64 start_ns_ = 0;
};

} // namespace ds::net
