// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/crypto/sha1.h"

#include <cstring>

namespace ds::crypto {
namespace {

inline u32 rol(u32 v, int n) { return (v << n) | (v >> (32 - n)); }

void block(u32 h[5], const u8* p) {
  u32 w[80];
  for (int i = 0; i < 16; ++i) w[i] = (u32{p[i * 4]} << 24) | (u32{p[i * 4 + 1]} << 16) | (u32{p[i * 4 + 2]} << 8) | p[i * 4 + 3];
  for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
  u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
  for (int i = 0; i < 80; ++i) {
    u32 f, k;
    if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5A827999; }
    else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1; }
    else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
    else             { f = b ^ c ^ d;                   k = 0xCA62C1D6; }
    const u32 t = rol(a, 5) + f + e + k + w[i];
    e = d; d = c; c = rol(b, 30); b = a; a = t;
  }
  h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

}  // namespace

void sha1(const u8* data, size_t len, u8 out[20]) {
  u32 h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
  size_t i = 0;
  for (; i + 64 <= len; i += 64) block(h, data + i);
  // The tail, the 0x80 terminator and the 64-bit big-endian bit length: one
  // block, or two when the length does not fit after the terminator.
  u8 tail[128] = {};
  const size_t rest = len - i;
  std::memcpy(tail, data + i, rest);
  tail[rest] = 0x80;
  const size_t n = rest + 1 + 8 <= 64 ? 64 : 128;
  const u64 bits = static_cast<u64>(len) * 8;
  for (int k = 0; k < 8; ++k) tail[n - 1 - k] = static_cast<u8>(bits >> (8 * k));
  block(h, tail);
  if (n == 128) block(h, tail + 64);
  for (int k = 0; k < 5; ++k)
    for (int b = 0; b < 4; ++b) out[k * 4 + b] = static_cast<u8>(h[k] >> (24 - 8 * b));
}

}  // namespace ds::crypto
