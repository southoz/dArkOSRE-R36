// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Shim over NEON intrinsics that exist only in AArch64. Every name here has
// two bodies: the plain A64 one, and — under DS_A32_SUBSET — one built solely
// from intrinsics ARMv7 NEON also has. Both compile for AArch64; the flag
// restricts the instruction vocabulary, it does not produce a 32-bit build.
#pragma once

#include "core/types.h"

#include <arm_neon.h>
#include <cstring>

#ifndef DS_A32_SUBSET
#define DS_A32_SUBSET 0
#endif

namespace ds::gpu::kern::compat {

// ---- across-vector reductions --------------------------------------------
// AArch32 has no ADDV/MAXV/MINV; log2(lanes) pairwise steps on D registers instead.

[[gnu::always_inline]] inline u8 maxv_u8(uint8x16_t v) {
#if DS_A32_SUBSET
  uint8x8_t d = vpmax_u8(vget_low_u8(v), vget_high_u8(v));
  d = vpmax_u8(d, d);
  d = vpmax_u8(d, d);
  return vget_lane_u8(vpmax_u8(d, d), 0);
#else
  return vmaxvq_u8(v);
#endif
}

[[gnu::always_inline]] inline u8 minv_u8(uint8x16_t v) {
#if DS_A32_SUBSET
  uint8x8_t d = vpmin_u8(vget_low_u8(v), vget_high_u8(v));
  d = vpmin_u8(d, d);
  d = vpmin_u8(d, d);
  return vget_lane_u8(vpmin_u8(d, d), 0);
#else
  return vminvq_u8(v);
#endif
}

[[gnu::always_inline]] inline u8 maxv_u8(uint8x8_t v) {
#if DS_A32_SUBSET
  uint8x8_t d = vpmax_u8(v, v);
  d = vpmax_u8(d, d);
  return vget_lane_u8(vpmax_u8(d, d), 0);
#else
  return vmaxv_u8(v);
#endif
}

[[gnu::always_inline]] inline u16 minv_u16(uint16x8_t v) {
#if DS_A32_SUBSET
  uint16x4_t d = vpmin_u16(vget_low_u16(v), vget_high_u16(v));
  d = vpmin_u16(d, d);
  return vget_lane_u16(vpmin_u16(d, d), 0);
#else
  return vminvq_u16(v);
#endif
}

[[gnu::always_inline]] inline u16 maxv_u16(uint16x8_t v) {
#if DS_A32_SUBSET
  uint16x4_t d = vpmax_u16(vget_low_u16(v), vget_high_u16(v));
  d = vpmax_u16(d, d);
  return vget_lane_u16(vpmax_u16(d, d), 0);
#else
  return vmaxvq_u16(v);
#endif
}

[[gnu::always_inline]] inline u32 maxv_u32(uint32x4_t v) {
#if DS_A32_SUBSET
  uint32x2_t d = vpmax_u32(vget_low_u32(v), vget_high_u32(v));
  return vget_lane_u32(vpmax_u32(d, d), 0);
#else
  return vmaxvq_u32(v);
#endif
}

[[gnu::always_inline]] inline u32 addv_u32(uint32x4_t v) {
#if DS_A32_SUBSET
  const uint32x2_t d = vadd_u32(vget_low_u32(v), vget_high_u32(v));
  return vget_lane_u32(vpadd_u32(d, d), 0);
#else
  return vaddvq_u32(v);
#endif
}

[[gnu::always_inline]] inline bool uniform_u8(uint8x16_t v) {
#if DS_A32_SUBSET
  return maxv_u8(veorq_u8(v, vdupq_n_u8(vgetq_lane_u8(v, 0)))) == 0;
#else
  return vmaxvq_u8(v) == vminvq_u8(v);
#endif
}
// Avoids a vector->scalar readback: two 64-bit loads, replicate first byte, compare.
[[gnu::always_inline]] inline bool uniform_u8_mem(const u8* p) {
  u64 a, b; std::memcpy(&a, p, 8); std::memcpy(&b, p + 8, 8);
  const u64 r = (a & 0xFF) * 0x0101010101010101ull;
  return ((a ^ r) | (b ^ r)) == 0;
}

// ---- table lookups --------------------------------------------------------
// vtbl2_u8: 16-byte table; vtbl4_u8: 32-byte table. Both zero out-of-range lanes.

[[gnu::always_inline]] inline uint8x16_t tbl1q_u8(uint8x16_t t, uint8x16_t idx) {
#if DS_A32_SUBSET
  const uint8x8x2_t tt = {{vget_low_u8(t), vget_high_u8(t)}};
  return vcombine_u8(vtbl2_u8(tt, vget_low_u8(idx)), vtbl2_u8(tt, vget_high_u8(idx)));
#else
  return vqtbl1q_u8(t, idx);
#endif
}

[[gnu::always_inline]] inline uint8x16_t tbl2q_u8(uint8x16x2_t t, uint8x16_t idx) {
#if DS_A32_SUBSET
  const uint8x8x4_t tt = {{vget_low_u8(t.val[0]), vget_high_u8(t.val[0]),
                           vget_low_u8(t.val[1]), vget_high_u8(t.val[1])}};
  return vcombine_u8(vtbl4_u8(tt, vget_low_u8(idx)), vtbl4_u8(tt, vget_high_u8(idx)));
#else
  return vqtbl2q_u8(t, idx);
#endif
}

[[gnu::always_inline]] inline uint8x16_t tbl4q_u8(uint8x16x4_t t, uint8x16_t idx) {
#if DS_A32_SUBSET
  // 64 bytes is twice what vtbl4 addresses: look up in both 32-byte halves,
  // biasing the second by 32, so exactly one lookup is in-range and the two OR together.
  const uint8x8x4_t lo = {{vget_low_u8(t.val[0]), vget_high_u8(t.val[0]),
                           vget_low_u8(t.val[1]), vget_high_u8(t.val[1])}};
  const uint8x8x4_t hi = {{vget_low_u8(t.val[2]), vget_high_u8(t.val[2]),
                           vget_low_u8(t.val[3]), vget_high_u8(t.val[3])}};
  const uint8x16_t bias = vsubq_u8(idx, vdupq_n_u8(32));
  const uint8x16_t a = vcombine_u8(vtbl4_u8(lo, vget_low_u8(idx)), vtbl4_u8(lo, vget_high_u8(idx)));
  const uint8x16_t b = vcombine_u8(vtbl4_u8(hi, vget_low_u8(bias)), vtbl4_u8(hi, vget_high_u8(bias)));
  return vorrq_u8(a, b);
#else
  return vqtbl4q_u8(t, idx);
#endif
}

// ---- high-half widening ----------------------------------------------------

[[gnu::always_inline]] inline uint64x2_t mull_high_u32(uint32x4_t a, uint32x4_t b) {
#if DS_A32_SUBSET
  return vmull_u32(vget_high_u32(a), vget_high_u32(b));
#else
  return vmull_high_u32(a, b);
#endif
}

[[gnu::always_inline]] inline int64x2_t mull_high_s32(int32x4_t a, int32x4_t b) {
#if DS_A32_SUBSET
  return vmull_s32(vget_high_s32(a), vget_high_s32(b));
#else
  return vmull_high_s32(a, b);
#endif
}

[[gnu::always_inline]] inline int64x2_t mlal_high_s32(int64x2_t acc, int32x4_t a, int32x4_t b) {
#if DS_A32_SUBSET
  return vmlal_s32(acc, vget_high_s32(a), vget_high_s32(b));
#else
  return vmlal_high_s32(acc, a, b);
#endif
}

[[gnu::always_inline]] inline uint16x8_t mull_high_u8(uint8x16_t a, uint8x16_t b) {
#if DS_A32_SUBSET
  return vmull_u8(vget_high_u8(a), vget_high_u8(b));
#else
  return vmull_high_u8(a, b);
#endif
}

[[gnu::always_inline]] inline uint16x8_t mlal_high_u8(uint16x8_t acc, uint8x16_t a, uint8x16_t b) {
#if DS_A32_SUBSET
  return vmlal_u8(acc, vget_high_u8(a), vget_high_u8(b));
#else
  return vmlal_high_u8(acc, a, b);
#endif
}

[[gnu::always_inline]] inline uint16x8_t addl_high_u8(uint8x16_t a, uint8x16_t b) {
#if DS_A32_SUBSET
  return vaddl_u8(vget_high_u8(a), vget_high_u8(b));
#else
  return vaddl_high_u8(a, b);
#endif
}

[[gnu::always_inline]] inline uint16x8_t subl_high_u8(uint8x16_t a, uint8x16_t b) {
#if DS_A32_SUBSET
  return vsubl_u8(vget_high_u8(a), vget_high_u8(b));
#else
  return vsubl_high_u8(a, b);
#endif
}

[[gnu::always_inline]] inline uint16x8_t movl_high_u8(uint8x16_t a) {
#if DS_A32_SUBSET
  return vmovl_u8(vget_high_u8(a));
#else
  return vmovl_high_u8(a);
#endif
}

[[gnu::always_inline]] inline uint32x4_t movl_high_u16(uint16x8_t a) {
#if DS_A32_SUBSET
  return vmovl_u16(vget_high_u16(a));
#else
  return vmovl_high_u16(a);
#endif
}

[[gnu::always_inline]] inline int32x4_t movl_high_s16(int16x8_t a) {
#if DS_A32_SUBSET
  return vmovl_s16(vget_high_s16(a));
#else
  return vmovl_high_s16(a);
#endif
}

// Shift count must be a literal, hence template rather than a parameter.
template <int N>
[[gnu::always_inline]] inline uint32x4_t shll_high_n_u16(uint16x8_t a) {
#if DS_A32_SUBSET
  return vshll_n_u16(vget_high_u16(a), N);
#else
  return vshll_high_n_u16(a, N);
#endif
}

// ---- lane broadcast ---------------------------------------------------------
// VDUP.32 can only name a lane of a D register; extract the upper half first to reach it.
template <int N>
[[gnu::always_inline]] inline int32x4_t dup_laneq_s32(int32x4_t v) {
#if DS_A32_SUBSET
  return vdupq_lane_s32(N < 2 ? vget_low_s32(v) : vget_high_s32(v), N & 1);
#else
  return vdupq_laneq_s32(v, N);
#endif
}

// ---- compare against zero -------------------------------------------------
[[gnu::always_inline]] inline uint32x4_t ceqz_u32(uint32x4_t a) {
#if DS_A32_SUBSET
  return vceqq_u32(a, vdupq_n_u32(0));
#else
  return vceqzq_u32(a);
#endif
}

// ---- exact unsigned division ------------------------------------------------
// AArch32 NEON has no f64 vector type or vector divide, so the truncated f64
// quotient (== exact integer quotient) is done a lane at a time on scalar VFP.

// Pins each quotient in a scalar register so the vectoriser can't fuse the loop back into vdivq_f64.
[[gnu::always_inline]] inline double keep_scalar(double v) {
  asm("" : "+w"(v));
  return v;
}

[[gnu::always_inline]] inline uint32x4_t udiv_exact(uint32x4_t num, uint32x4_t den) {
#if DS_A32_SUBSET
  u32 n[4], d[4], q[4];
  vst1q_u32(n, num);
  vst1q_u32(d, den);
  for (int i = 0; i < 4; ++i)
    q[i] = static_cast<u32>(keep_scalar(static_cast<double>(n[i]) / static_cast<double>(d[i])));
  return vld1q_u32(q);
#else
  const float64x2_t nlo = vcvtq_f64_u64(vmovl_u32(vget_low_u32(num))), nhi = vcvtq_f64_u64(vmovl_u32(vget_high_u32(num)));
  const float64x2_t dlo = vcvtq_f64_u64(vmovl_u32(vget_low_u32(den))), dhi = vcvtq_f64_u64(vmovl_u32(vget_high_u32(den)));
  const uint64x2_t qlo = vcvtq_u64_f64(vdivq_f64(nlo, dlo)), qhi = vcvtq_u64_f64(vdivq_f64(nhi, dhi));
  return vcombine_u32(vmovn_u64(qlo), vmovn_u64(qhi));
#endif
}

// (u64 lanes) / d, exact, for products below 2^53.
[[gnu::always_inline]] inline uint64x2_t udiv64_exact(uint64x2_t n, double d0, double d1) {
#if DS_A32_SUBSET
  u64 n2[2], q[2];
  vst1q_u64(n2, n);
  q[0] = static_cast<u64>(keep_scalar(static_cast<double>(n2[0]) / d0));
  q[1] = static_cast<u64>(keep_scalar(static_cast<double>(n2[1]) / d1));
  return vld1q_u64(q);
#else
  const float64x2_t d = vsetq_lane_f64(d1, vdupq_n_f64(d0), 1);
  return vcvtq_u64_f64(vdivq_f64(vcvtq_f64_u64(n), d));
#endif
}

} // namespace ds::gpu::kern::compat
