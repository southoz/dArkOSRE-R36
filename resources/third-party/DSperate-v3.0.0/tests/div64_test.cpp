// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// core/div64.h against the C++ operators: the 32-bit ARM paths go through
// double and a remainder fix, and must agree on every operand, including the
// ones that fall back. Elsewhere the helpers are the operators and this passes
// trivially; run it under qemu-arm on an ARMv7 build.
#include "core/div64.h"
#include "check.h"

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <initializer_list>

using namespace ds;

namespace {

u64 g_state = 0x9E3779B97F4A7C15ull;
u64 next() { g_state ^= g_state << 13; g_state ^= g_state >> 7; g_state ^= g_state << 17; return g_state; }

[[gnu::noinline]] s64 ref_s(s64 n, s32 d) { return n / d; }
[[gnu::noinline]] u64 ref_u(u64 n, u32 d) { return n / d; }

void check_s(s64 n, s32 d) {
  if (d == 0 || (n == INT64_MIN && d == -1)) return;
  const s64 got = div_s64(n, d), want = ref_s(n, d);
  if (got != want) { std::fprintf(stderr, "div_s64(%" PRId64 ", %d) = %" PRId64 ", want %" PRId64 "\n", n, d, got, want); CHECK(false); }
}

void check_u(u64 n, u32 d) {
  if (d == 0) return;
  const u64 got = div_u64(n, d), want = ref_u(n, d);
  if (got != want) { std::fprintf(stderr, "div_u64(%" PRIu64 ", %u) = %" PRIu64 ", want %" PRIu64 "\n", n, d, got, want); CHECK(false); }
}

} // namespace

int main() {
  // Edges: exact multiples and their neighbours (the remainder fix's cases),
  // the double path's range limits, and both signs.
  const s32 ds[] = {1, -1, 2, -2, 3, 7, -7, 255, 256, 4095, 0x4000, 65535, 65536, 0x7FFFFFFF, -0x7FFFFFFF - 1, 1000003, -1000003};
  for (s32 d : ds) {
    for (s64 q : {s64{0}, s64{1}, s64{-1}, s64{12345}, s64{-12345}, s64{0x7FFFFFFF}, s64{-0x7FFFFFFF}, s64{2147483000}, s64{1} << 40, -(s64{1} << 40)}) {
      s64 m;
      if (__builtin_mul_overflow(q, static_cast<s64>(d), &m) || m > INT64_MAX - 2 || m < INT64_MIN + 2) continue;
      for (s64 e = -2; e <= 2; ++e) check_s(m + e, d);
    }
    for (s64 n : {s64{1} << 51, (s64{1} << 51) - 1, -(s64{1} << 51), s64{1} << 52, INT64_MAX, INT64_MIN + 1}) check_s(n, d);
  }
  for (u32 d : {1u, 2u, 3u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu, 65536u, 12345u}) {
    for (u64 q : {u64{0}, u64{1}, u64{0xFFFFFFFF}, u64{4294967000}, u64{1} << 36}) {
      u64 m;
      if (__builtin_mul_overflow(q, static_cast<u64>(d), &m) || m > ~u64{0} - 4) continue;
      for (u64 e = 0; e <= 4; ++e) { check_u(m + e, d); if (m >= e) check_u(m - e, d); }
    }
    for (u64 n : {u64{1} << 52, (u64{1} << 52) - 1, u64{1} << 60, ~u64{0}}) check_u(n, d);
  }
  // Random operands at every width, the sizes the renderer uses most.
  for (int i = 0; i < 4000000; ++i) {
    const u64 r = next(), s = next();
    const int nb = static_cast<int>(r % 64), db = static_cast<int>((r >> 8) % 32) + 1;
    s64 n = static_cast<s64>(s >> (63 - nb));
    if (r & (1ull << 20)) n = -n;
    s32 d = static_cast<s32>(static_cast<u32>(next() >> (64 - db)));
    if (r & (1ull << 21)) d = -d;
    check_s(n, d);
    check_u(s >> (63 - nb), static_cast<u32>(next() >> (64 - db)));
  }
  // The span reciprocal against its 64-bit definition.
  for (u32 x = 2; x < 70000; ++x) CHECK(recip_ceil32(x) == static_cast<u32>(((1ull << 32) + x - 1) / x));
  for (u32 x : {0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu}) CHECK(recip_ceil32(x) == static_cast<u32>(((1ull << 32) + x - 1) / x));
  std::printf("div64: ok\n");
  return 0;
}
