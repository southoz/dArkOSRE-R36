// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// s64/s32 division via VFP double instead of the slow __udivmoddi4 softdiv
// on ARMv7. Matches C++ truncating semantics exactly; out-of-range operands
// fall back to the plain operator. Other targets divide 64-bit natively.
#pragma once
#include "core/types.h"

namespace ds {

#if defined(__arm__) && defined(__VFP_FP__) && !defined(__SOFTFP__)

// Exact while |n| < 2^52 and the quotient fits 31 bits; remainder check fixes
// the rare off-by-one from double rounding.
[[gnu::always_inline]] inline s64 div_s64(s64 n, s32 d) {
  const s32 hi = static_cast<s32>(n >> 32);
  if (d != 0 && hi >= -(1 << 19) && hi < (1 << 19)) {
    const double qd = (static_cast<double>(hi) * 4294967296.0 + static_cast<double>(static_cast<u32>(n))) / d;
    if (qd > -2147483000.0 && qd < 2147483000.0) {
      s64 q = static_cast<s32>(qd);
      const s64 r = n - q * d;
      const s64 ad = d < 0 ? -static_cast<s64>(d) : d;
      const s64 away = d < 0 ? -1 : 1;
      if (n >= 0) { if (r < 0) q -= away; else if (r >= ad) q += away; }
      else { if (r > 0) q += away; else if (-r >= ad) q -= away; }
      return q;
    }
  }
  return n / d;
}

// Unsigned counterpart of div_s64.
[[gnu::always_inline]] inline u64 div_u64(u64 n, u32 d) {
  const u32 hi = static_cast<u32>(n >> 32);
  if (d != 0 && hi < (1u << 20)) {
    const double qd = (static_cast<double>(hi) * 4294967296.0 + static_cast<double>(static_cast<u32>(n))) / static_cast<double>(d);
    if (qd < 4294967000.0) {
      u64 q = static_cast<u32>(qd);
      const u64 qd64 = q * d;
      if (qd64 > n) --q;
      else if (n - qd64 >= d) ++q;
      return q;
    }
  }
  return n / d;
}

// ceil(2^32 / x) for x >= 2, via one 32-bit divide.
[[gnu::always_inline]] inline u32 recip_ceil32(u32 x) { return 0xFFFFFFFFu / x + 1; }

#else

[[gnu::always_inline]] inline s64 div_s64(s64 n, s32 d) { return n / d; }
[[gnu::always_inline]] inline u64 div_u64(u64 n, u32 d) { return n / d; }
[[gnu::always_inline]] inline u32 recip_ceil32(u32 x) { return static_cast<u32>(((1ull << 32) + x - 1) / x); }

#endif

} // namespace ds
