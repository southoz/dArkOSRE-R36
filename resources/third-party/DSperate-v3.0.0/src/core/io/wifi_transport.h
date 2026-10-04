// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// External Wi-Fi interfaces, both optional (unset = frames go nowhere).
// MpTransport: raw DS frames (12-byte TX header + 802.11) between emulator
// instances, shaped like melonDS's MPInterface for wire compatibility.
// NetDriver: Ethernet frames to the host network.
// Both polled from the ARM7 timeline: implementations must not block in
// recv_*, except recv_replies (bounded wait for clients' slots).
#pragma once

#include "core/types.h"

namespace ds::io {

class MpTransport {
public:
  virtual ~MpTransport() = default;
  virtual void begin() {}
  virtual void end() {}
  // Regular frames (LOC1-3 slots and beacons).
  virtual int  send_packet(const u8* data, int len, u64 timestamp) = 0;
  virtual int  recv_packet(u8* data, u64* timestamp) = 0;   // > 0: bytes; 0: nothing
  // MP protocol: host CMD, client reply in its slot, host ack.
  virtual int  send_cmd(const u8* data, int len, u64 timestamp) = 0;
  virtual int  send_reply(const u8* data, int len, u64 timestamp, u16 aid) = 0;
  virtual int  send_ack(const u8* data, int len, u64 timestamp) = 0;
  virtual int  recv_host_packet(u8* data, u64* timestamp) = 0;   // < 0: the host is gone
  // Non-blocking recv_host_packet; a client uses this to fetch early without
  // stalling on its own timeline.
  virtual int  peek_host_packet(u8* data, u64* timestamp) { (void)data; (void)timestamp; return 0; }
  // Gathers replies of clients in aidmask into 15 x 1024-byte slots; returns
  // the mask of clients that answered.
  virtual u16  recv_replies(u8* data, u64 timestamp, u16 aidmask) = 0;
};

class NetDriver {
public:
  virtual ~NetDriver() = default;
  virtual int send(const u8* data, int len) = 0;
  virtual int recv(u8* data) = 0;   // > 0: bytes of one Ethernet frame; 0: nothing
};

} // namespace ds::io
