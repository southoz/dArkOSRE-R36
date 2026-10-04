// SPDX-License-Identifier: GPL-3.0-or-later
// The internet driver: the DNS rewrite's checksum arithmetic, and the DS's
// first conversation with the user-mode stack -- ARP for the gateway, then
// DHCP for an address.
//
// No traffic leaves this machine: everything here is between the driver and
// libslirp's own emulated network, so the test runs the same on a build
// machine with no route to anywhere.
#include "net/slirp_driver.h"
#include "check.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using ds::u8;
using ds::u16;
using ds::u32;
using ds::net::SlirpDriver;

namespace {

const u8 kDsMac[6] = {0x00, 0x09, 0xBF, 0x12, 0x34, 0x56};
constexpr u32 kGateway = 0x0A000202;   // 10.0.2.2
constexpr u32 kLeased  = 0x0A00020F;   // 10.0.2.15

void st16(u8* p, u16 v) { p[0] = static_cast<u8>(v >> 8); p[1] = static_cast<u8>(v); }
void st32(u8* p, u32 v) {
  p[0] = static_cast<u8>(v >> 24); p[1] = static_cast<u8>(v >> 16);
  p[2] = static_cast<u8>(v >> 8);  p[3] = static_cast<u8>(v);
}
u16 ld16(const u8* p) { return static_cast<u16>((p[0] << 8) | p[1]); }
u32 ld32(const u8* p) {
  return (static_cast<u32>(p[0]) << 24) | (static_cast<u32>(p[1]) << 16) |
         (static_cast<u32>(p[2]) << 8) | p[3];
}

// One's-complement sum, folded. Over a correct header or a correct
// pseudo-header + payload this comes out 0xFFFF, which is how the tests
// check a patched checksum without knowing how it was patched.
u16 ones_sum(const u8* data, std::size_t len, u32 seed = 0) {
  u32 sum = seed;
  for (std::size_t i = 0; i + 1 < len; i += 2) sum += ld16(&data[i]);
  if (len & 1) sum += static_cast<u32>(data[len - 1]) << 8;
  while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
  return static_cast<u16>(sum);
}

bool ip_checksum_ok(const u8* ip) {
  const std::size_t ihl = static_cast<std::size_t>(ip[0] & 0xF) * 4;
  return ones_sum(ip, ihl) == 0xFFFF;
}

bool udp_checksum_ok(const u8* ip) {
  const std::size_t ihl = static_cast<std::size_t>(ip[0] & 0xF) * 4;
  const u8* udp = ip + ihl;
  const std::size_t udp_len = ld16(&udp[4]);
  if (ld16(&udp[6]) == 0) return true;     // none was computed
  // The pseudo-header: source and destination addresses, the protocol, and
  // the UDP length. This is why a bare IP-header patch is not enough -- the
  // addresses are inside the UDP checksum too.
  u32 seed = 0;
  seed += ld16(&ip[12]) + ld16(&ip[14]);   // source
  seed += ld16(&ip[16]) + ld16(&ip[18]);   // destination
  seed += 17;                              // protocol
  seed += static_cast<u32>(udp_len);
  return ones_sum(udp, udp_len, seed) == 0xFFFF;
}

// A DNS query from the DS to `dst`, with correct checksums.
std::vector<u8> dns_query(u32 src, u32 dst, bool with_udp_checksum = true) {
  std::vector<u8> f(14 + 20 + 8 + 12, 0);
  std::memset(&f[0], 0xFF, 6);             // to the gateway's MAC, whatever it is
  std::memcpy(&f[6], kDsMac, 6);
  st16(&f[12], 0x0800);
  u8* ip = &f[14];
  ip[0] = 0x45;
  st16(&ip[2], static_cast<u16>(f.size() - 14));
  ip[8] = 64;                              // TTL
  ip[9] = 17;                              // UDP
  st32(&ip[12], src);
  st32(&ip[16], dst);
  st16(&ip[10], static_cast<u16>(~ones_sum(ip, 20)));
  u8* udp = ip + 20;
  st16(&udp[0], 4096);                     // an ephemeral source port
  st16(&udp[2], 53);
  st16(&udp[4], 8 + 12);
  u8* payload = udp + 8;
  st16(&payload[0], 0x1234);               // a DNS header's worth of body
  st16(&payload[2], 0x0100);
  st16(&payload[4], 1);
  if (with_udp_checksum) {
    u32 seed = ld16(&ip[12]) + ld16(&ip[14]) + ld16(&ip[16]) + ld16(&ip[18]) + 17 + (8 + 12);
    st16(&udp[6], static_cast<u16>(~ones_sum(udp, 8 + 12, seed)));
  }
  return f;
}

// The same frame seen coming back: source and destination swapped, and the
// UDP ports with them.
std::vector<u8> dns_reply(u32 src, u32 dst) {
  std::vector<u8> f = dns_query(dst, src);
  u8* ip = &f[14];
  st32(&ip[12], src);
  st32(&ip[16], dst);
  u8* udp = ip + 20;
  st16(&udp[0], 53);
  st16(&udp[2], 4096);
  st16(&ip[10], 0);
  st16(&ip[10], static_cast<u16>(~ones_sum(ip, 20)));
  st16(&udp[6], 0);
  u32 seed = ld16(&ip[12]) + ld16(&ip[14]) + ld16(&ip[16]) + ld16(&ip[18]) + 17 + (8 + 12);
  st16(&udp[6], static_cast<u16>(~ones_sum(udp, 8 + 12, seed)));
  return f;
}

// ---- the rewrite --------------------------------------------------------

void test_dns_rewrite_out() {
  constexpr u32 kWiimmfi = 0xB23E2BD4;     // 178.62.43.212
  std::vector<u8> f = dns_query(kLeased, SlirpDriver::kVirtualDns);
  const u8* ip = &f[14];
  CHECK(ip_checksum_ok(ip));               // the fixture itself is sound
  CHECK(udp_checksum_ok(ip));

  CHECK(SlirpDriver::rewrite_dns_dst(f.data(), static_cast<int>(f.size()),
                                     SlirpDriver::kVirtualDns, kWiimmfi));
  CHECK(ld32(&ip[16]) == kWiimmfi);        // it went where it was told
  CHECK(ld32(&ip[12]) == kLeased);         // and the source is untouched
  // Both checksums still verify: this is the whole point of the incremental
  // patch, and getting either wrong means every query is silently dropped by
  // the first host that checks it.
  CHECK(ip_checksum_ok(ip));
  CHECK(udp_checksum_ok(ip));
}

void test_dns_rewrite_round_trip() {
  constexpr u32 kElsewhere = 0x09090909;   // 9.9.9.9
  const std::vector<u8> original = dns_query(kLeased, SlirpDriver::kVirtualDns);
  std::vector<u8> f = original;
  CHECK(SlirpDriver::rewrite_dns_dst(f.data(), static_cast<int>(f.size()),
                                     SlirpDriver::kVirtualDns, kElsewhere));
  CHECK(f != original);
  // Back the other way, byte for byte. A checksum patch that is merely
  // self-consistent would pass the test above and still fail this one.
  CHECK(SlirpDriver::rewrite_dns_dst(f.data(), static_cast<int>(f.size()),
                                     kElsewhere, SlirpDriver::kVirtualDns));
  CHECK(f == original);
}

void test_dns_rewrite_in() {
  constexpr u32 kWiimmfi = 0xB23E2BD4;
  // The reply really comes from Wiimmfi, but the DS asked 10.0.2.3 and its
  // socket will only match an answer from there.
  std::vector<u8> f = dns_reply(kWiimmfi, kLeased);
  const u8* ip = &f[14];
  CHECK(ip_checksum_ok(ip));
  CHECK(udp_checksum_ok(ip));

  CHECK(SlirpDriver::rewrite_dns_src(f.data(), static_cast<int>(f.size()),
                                     kWiimmfi, SlirpDriver::kVirtualDns));
  CHECK(ld32(&ip[12]) == SlirpDriver::kVirtualDns);
  CHECK(ld32(&ip[16]) == kLeased);
  CHECK(ip_checksum_ok(ip));
  CHECK(udp_checksum_ok(ip));
}

void test_dns_rewrite_leaves_everything_else_alone() {
  constexpr u32 kWiimmfi = 0xB23E2BD4;
  const int n = static_cast<int>(dns_query(kLeased, SlirpDriver::kVirtualDns).size());

  // A query to some other nameserver the player configured by hand: not ours.
  std::vector<u8> other = dns_query(kLeased, 0x08080808);
  CHECK(!SlirpDriver::rewrite_dns_dst(other.data(), n, SlirpDriver::kVirtualDns, kWiimmfi));

  // Not UDP.
  std::vector<u8> tcp = dns_query(kLeased, SlirpDriver::kVirtualDns);
  tcp[14 + 9] = 6;
  CHECK(!SlirpDriver::rewrite_dns_dst(tcp.data(), n, SlirpDriver::kVirtualDns, kWiimmfi));

  // Not IPv4 (an ARP frame's ethertype).
  std::vector<u8> arp = dns_query(kLeased, SlirpDriver::kVirtualDns);
  st16(&arp[12], 0x0806);
  CHECK(!SlirpDriver::rewrite_dns_dst(arp.data(), n, SlirpDriver::kVirtualDns, kWiimmfi));

  // Not port 53.
  std::vector<u8> http = dns_query(kLeased, SlirpDriver::kVirtualDns);
  st16(&http[14 + 20 + 2], 80);
  CHECK(!SlirpDriver::rewrite_dns_dst(http.data(), n, SlirpDriver::kVirtualDns, kWiimmfi));

  // A fragment past the first carries no UDP header to read a port from.
  std::vector<u8> frag = dns_query(kLeased, SlirpDriver::kVirtualDns);
  st16(&frag[14 + 6], 0x0001);
  CHECK(!SlirpDriver::rewrite_dns_dst(frag.data(), n, SlirpDriver::kVirtualDns, kWiimmfi));

  // Truncated: shorter than the headers it claims. Nothing may be read past
  // the end of it.
  std::vector<u8> tiny = dns_query(kLeased, SlirpDriver::kVirtualDns);
  CHECK(!SlirpDriver::rewrite_dns_dst(tiny.data(), 20, SlirpDriver::kVirtualDns, kWiimmfi));
}

void test_dns_rewrite_keeps_an_absent_udp_checksum_absent() {
  constexpr u32 kWiimmfi = 0xB23E2BD4;
  // A zero UDP checksum means the sender computed none, and 0 is not a value
  // the patch may turn into something else: over IPv4 it must stay 0.
  std::vector<u8> f = dns_query(kLeased, SlirpDriver::kVirtualDns, /*with_udp_checksum=*/false);
  CHECK(SlirpDriver::rewrite_dns_dst(f.data(), static_cast<int>(f.size()),
                                     SlirpDriver::kVirtualDns, kWiimmfi));
  CHECK(ld16(&f[14 + 20 + 6]) == 0);
  CHECK(ip_checksum_ok(&f[14]));
}

// ---- the stack ----------------------------------------------------------

// Drive the driver until it hands back a frame, or give up. recv() never
// blocks, so a reply that needs a timer to fire needs the pump run again.
std::vector<u8> wait_frame(SlirpDriver& d, int tries = 200) {
  u8 buf[SlirpDriver::kMaxFrame];
  for (int i = 0; i < tries; ++i) {
    const int len = d.recv(buf);
    if (len > 0) return std::vector<u8>(buf, buf + len);
    d.process();
  }
  return {};
}

void test_arp_for_the_gateway() {
  SlirpDriver d;
  CHECK(d.start(SlirpDriver::Dns::Host));
  CHECK(d.ok());

  std::vector<u8> req(42, 0);
  std::memset(&req[0], 0xFF, 6);
  std::memcpy(&req[6], kDsMac, 6);
  st16(&req[12], 0x0806);                  // ARP
  u8* arp = &req[14];
  st16(&arp[0], 1); st16(&arp[2], 0x0800);
  arp[4] = 6; arp[5] = 4;
  st16(&arp[6], 1);                        // request
  std::memcpy(&arp[8], kDsMac, 6);
  st32(&arp[14], kLeased);
  st32(&arp[24], kGateway);
  CHECK(d.send(req.data(), static_cast<int>(req.size())) > 0);

  const std::vector<u8> reply = wait_frame(d);
  CHECK(reply.size() >= 42);
  CHECK(ld16(&reply[12]) == 0x0806);
  CHECK(ld16(&reply[14 + 6]) == 2);        // a reply
  CHECK(ld32(&reply[14 + 14]) == kGateway);        // from the gateway
  CHECK(std::memcmp(&reply[0], kDsMac, 6) == 0);   // addressed to us
}

// The conversation a DS actually has on association: DISCOVER, OFFER,
// REQUEST, ACK. Reaching the end of it is what "the console got on the
// network" means -- everything after it is ordinary traffic.
void test_dhcp_lease() {
  SlirpDriver d;
  CHECK(d.start(SlirpDriver::Dns::Host));

  auto bootp = [&](u8 message_type) {
    std::vector<u8> f(14 + 20 + 8 + 240 + 8, 0);
    std::memset(&f[0], 0xFF, 6);
    std::memcpy(&f[6], kDsMac, 6);
    st16(&f[12], 0x0800);
    u8* ip = &f[14];
    ip[0] = 0x45;
    st16(&ip[2], static_cast<u16>(f.size() - 14));
    ip[8] = 64; ip[9] = 17;
    st32(&ip[12], 0);                      // 0.0.0.0: it has no address yet
    st32(&ip[16], 0xFFFFFFFF);             // broadcast
    st16(&ip[10], static_cast<u16>(~ones_sum(ip, 20)));
    u8* udp = ip + 20;
    st16(&udp[0], 68); st16(&udp[2], 67);
    st16(&udp[4], static_cast<u16>(f.size() - 14 - 20));
    u8* bp = udp + 8;
    bp[0] = 1;                             // BOOTREQUEST
    bp[1] = 1; bp[2] = 6;                  // Ethernet, 6-byte address
    st32(&bp[4], 0xD59E4A7E);              // xid: any value, echoed back
    std::memcpy(&bp[28], kDsMac, 6);
    st32(&bp[236], 0x63825363);            // the magic cookie
    bp[240] = 53; bp[241] = 1; bp[242] = message_type;
    bp[243] = 255;                         // end
    return f;
  };

  std::vector<u8> discover = bootp(1);
  CHECK(d.send(discover.data(), static_cast<int>(discover.size())) > 0);
  const std::vector<u8> offer = wait_frame(d);
  CHECK(offer.size() > 14 + 20 + 8 + 240);
  const u8* bp = &offer[14 + 20 + 8];
  CHECK(bp[0] == 2);                                  // BOOTREPLY
  CHECK(ld32(&bp[16]) == kLeased);                    // yiaddr: 10.0.2.15
  CHECK(std::memcmp(&bp[28], kDsMac, 6) == 0);        // ours

  // Walk the options for the message type and the nameserver the DS will be
  // told to use -- that address is what the DNS rewrite keys on, so it is
  // worth asserting rather than assuming.
  bool saw_offer = false, saw_dns = false;
  for (std::size_t i = 240; i + 1 < offer.size() - (14 + 20 + 8);) {
    const u8 code = bp[i];
    if (code == 255) break;
    if (code == 0) { ++i; continue; }
    const u8 olen = bp[i + 1];
    if (code == 53 && olen == 1 && bp[i + 2] == 2) saw_offer = true;
    if (code == 6 && olen >= 4 && ld32(&bp[i + 2]) == SlirpDriver::kVirtualDns) saw_dns = true;
    i += 2u + olen;
  }
  CHECK(saw_offer);
  CHECK(saw_dns);

  std::vector<u8> request = bootp(3);
  CHECK(d.send(request.data(), static_cast<int>(request.size())) > 0);
  const std::vector<u8> ack = wait_frame(d);
  CHECK(ack.size() > 14 + 20 + 8 + 240);
  CHECK(ack[14 + 20 + 8] == 2);
  CHECK(ld32(&ack[14 + 20 + 8 + 16]) == kLeased);
}

void test_stop_is_idempotent() {
  SlirpDriver d;
  CHECK(d.start(SlirpDriver::Dns::Host));
  d.stop();
  CHECK(!d.ok());
  d.stop();
  // Nothing works after a stop, and nothing crashes either.
  u8 buf[SlirpDriver::kMaxFrame];
  CHECK(d.recv(buf) == 0);
  CHECK(d.send(buf, 64) == 0);
  d.process();
}

// A Custom DNS with no address to rewrite to would black-hole every query;
// the driver falls back to the host resolver instead.
void test_custom_dns_without_an_address_falls_back() {
  SlirpDriver d;
  CHECK(d.start(SlirpDriver::Dns::Custom, 0));
  CHECK(d.ok());
}

} // namespace

int main() {
  test_dns_rewrite_out();
  test_dns_rewrite_round_trip();
  test_dns_rewrite_in();
  test_dns_rewrite_leaves_everything_else_alone();
  test_dns_rewrite_keeps_an_absent_udp_checksum_absent();
  test_arp_for_the_gateway();
  test_dhcp_lease();
  test_stop_is_idempotent();
  test_custom_dns_without_an_address_falls_back();
  std::printf("slirp_driver: ok\n");
  return 0;
}
