// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// NEON twins of kernels_ref.cpp. Same names, same signatures, bit-identical output.
//
// 18-bit colour records stay packed in u32 lanes (R 0-5, G 8-13, B 16-21);
// byte planes (ids, kinds, masks) are handled 16 at a time, widened to u32
// lane masks where they gate colour lanes.
#include "core/gpu/kernels.h"
#include "core/div64.h"

#if DSPERATE_NEON
#include <arm_neon.h>
#include "core/gpu/neon_compat.h"
#include <cstring>

namespace ds::gpu::kern::neon {

namespace {

// u8x16 mask -> four u32x4 lane masks.
struct Mask4 { uint32x4_t m[4]; };
// Sign-extending: 0xFF becomes an all-ones lane mask; values below 0x80 (e.g. alphas) widen unchanged.
inline Mask4 widen(uint8x16_t m8) {
  const int8x16_t s = vreinterpretq_s8_u8(m8);
  const int16x8_t lo = vmovl_s8(vget_low_s8(s)), hi = vmovl_s8(vget_high_s8(s));
  return {{vreinterpretq_u32_s32(vmovl_s16(vget_low_s16(lo))), vreinterpretq_u32_s32(vmovl_s16(vget_high_s16(lo))),
           vreinterpretq_u32_s32(vmovl_s16(vget_low_s16(hi))), vreinterpretq_u32_s32(vmovl_s16(vget_high_s16(hi)))}};
}
// four u32x4 -> u8x16 (low bytes).
inline uint8x16_t narrow(uint32x4_t a, uint32x4_t b, uint32x4_t c, uint32x4_t d) {
  return vcombine_u8(vmovn_u16(vcombine_u16(vmovn_u32(a), vmovn_u32(b))), vmovn_u16(vcombine_u16(vmovn_u32(c), vmovn_u32(d))));
}

// Arithmetic form of direct_colour(), 16 words: each 5-bit field becomes a
// byte plane (x2, bit 15 dropped), interleaved out with the alpha byte.
inline void direct16(const u16* v, Pixel* out) {
  const uint16x8_t lo = vld1q_u16(v), hi = vld1q_u16(v + 8);
  const uint8x16_t m = vdupq_n_u8(0x3E);
  uint8x16x4_t px;
  px.val[0] = vandq_u8(vcombine_u8(vmovn_u16(vshlq_n_u16(lo, 1)), vmovn_u16(vshlq_n_u16(hi, 1))), m);
  px.val[1] = vandq_u8(vcombine_u8(vshrn_n_u16(lo, 4), vshrn_n_u16(hi, 4)), m);
  px.val[2] = vandq_u8(vcombine_u8(vmovn_u16(vshrq_n_u16(lo, 9)), vmovn_u16(vshrq_n_u16(hi, 9))), m);
  px.val[3] = vdupq_n_u8(0xFF);
  vst4q_u8(reinterpret_cast<u8*>(out), px);
}

// Blend of two records with per-lane weights, 4 pixels. R and B share one
// multiply chain (fields cap at 63*32+16 < 2^11, so they don't meet); after
// the shift a field is at most 126, so bit 6 is the overflow flag for the 0x3F clamp.
inline uint32x4_t blend4(uint32x4_t a, uint32x4_t b, uint32x4_t ea, uint32x4_t eb, u32 round_r, u32 shift) {
  const uint32x4_t mrb = vdupq_n_u32(0x3F003F), mg = vdupq_n_u32(0x3F00);
  uint32x4_t rb = vmlaq_u32(vmlaq_u32(vdupq_n_u32(round_r * 0x010001), vandq_u32(a, mrb), ea), vandq_u32(b, mrb), eb);
  uint32x4_t g = vmlaq_u32(vmlaq_u32(vdupq_n_u32(round_r << 8), vandq_u32(a, mg), ea), vandq_u32(b, mg), eb);
  const int32x4_t sh = vdupq_n_s32(-static_cast<s32>(shift));
  rb = vshlq_u32(rb, sh); g = vshlq_u32(g, sh);
  const uint32x4_t orb = vandq_u32(vshrq_n_u32(rb, 6), vdupq_n_u32(0x010001)), og = vandq_u32(vshrq_n_u32(g, 6), vdupq_n_u32(0x100));
  rb = vorrq_u32(vandq_u32(rb, mrb), vsubq_u32(vshlq_n_u32(orb, 6), orb));
  g = vorrq_u32(vandq_u32(g, mg), vsubq_u32(vshlq_n_u32(og, 6), og));
  return vorrq_u32(vorrq_u32(rb, g), vdupq_n_u32(0xFF000000));
}

inline uint32x4_t brighten4(uint32x4_t v, u32 factor, u32 bias) {
  const uint32x4_t mrb = vdupq_n_u32(0x3F003F), mg = vdupq_n_u32(0x003F00);
  const uint32x4_t rb = vandq_u32(v, mrb), g = vandq_u32(v, mg);
  uint32x4_t drb = vmlaq_u32(vdupq_n_u32(bias * 0x010001), vsubq_u32(mrb, rb), vdupq_n_u32(factor));
  uint32x4_t dg = vmlaq_u32(vdupq_n_u32(bias * 0x000100), vsubq_u32(mg, g), vdupq_n_u32(factor));
  drb = vandq_u32(vshrq_n_u32(drb, 4), mrb);
  dg = vandq_u32(vshrq_n_u32(dg, 4), mg);
  return vorrq_u32(vorrq_u32(vaddq_u32(rb, drb), vaddq_u32(g, dg)), vdupq_n_u32(0xFF000000));
}
inline uint32x4_t darken4(uint32x4_t v, u32 factor, u32 bias) {
  const uint32x4_t mrb = vdupq_n_u32(0x3F003F), mg = vdupq_n_u32(0x003F00);
  const uint32x4_t rb = vandq_u32(v, mrb), g = vandq_u32(v, mg);
  uint32x4_t drb = vmlaq_u32(vdupq_n_u32(bias * 0x010001), rb, vdupq_n_u32(factor));
  uint32x4_t dg = vmlaq_u32(vdupq_n_u32(bias * 0x000100), g, vdupq_n_u32(factor));
  drb = vandq_u32(vshrq_n_u32(drb, 4), mrb);
  dg = vandq_u32(vshrq_n_u32(dg, 4), mg);
  return vorrq_u32(vorrq_u32(vsubq_u32(rb, drb), vsubq_u32(g, dg)), vdupq_n_u32(0xFF000000));
}

// Same maths on byte planes, 16 pixels a call, via ld4 deinterleave. Per-lane
// byte weights mean bitmap-sprite/3D blends (per-pixel weight) cost the same
// as the fixed EVA/EVB blend. Inputs are 6-bit fields (caller masks).
// (a*ea + b*eb + 2^(shift-1)) >> shift, clamped to 63.
template <int shift>
inline uint8x16_t blend16(uint8x16_t a, uint8x16_t b, uint8x16_t ea, uint8x16_t eb) {
  const uint16x8_t rnd = vdupq_n_u16(1u << (shift - 1));
  const uint16x8_t lo = vmlal_u8(vmlal_u8(rnd, vget_low_u8(a), vget_low_u8(ea)), vget_low_u8(b), vget_low_u8(eb));
  const uint16x8_t hi = compat::mlal_high_u8(compat::mlal_high_u8(rnd, a, ea), b, eb);
  return vminq_u8(vcombine_u8(vshrn_n_u16(lo, shift), vshrn_n_u16(hi, shift)), vdupq_n_u8(63));
}
// c + (((63 - c) * factor + bias) >> 4); scalar's & 0x3F after shift is a no-op here, dropped.
inline uint8x16_t brighten16(uint8x16_t c, uint8x8_t factor, u16 bias) {
  const uint8x16_t inv = vsubq_u8(vdupq_n_u8(63), c);
  const uint16x8_t lo = vmlal_u8(vdupq_n_u16(bias), vget_low_u8(inv), factor);
  const uint16x8_t hi = vmlal_u8(vdupq_n_u16(bias), vget_high_u8(inv), factor);
  return vaddq_u8(c, vcombine_u8(vshrn_n_u16(lo, 4), vshrn_n_u16(hi, 4)));
}
// c - ((c * factor + bias) >> 4), likewise.
inline uint8x16_t darken16(uint8x16_t c, uint8x8_t factor, u16 bias) {
  const uint16x8_t lo = vmlal_u8(vdupq_n_u16(bias), vget_low_u8(c), factor);
  const uint16x8_t hi = vmlal_u8(vdupq_n_u16(bias), vget_high_u8(c), factor);
  return vsubq_u8(c, vcombine_u8(vshrn_n_u16(lo, 4), vshrn_n_u16(hi, 4)));
}

} // namespace


bool line_has_translucent_3d(const Pixel* line3d) {
  const uint32x4_t m = vdupq_n_u32(0x1F), v31 = vdupq_n_u32(31), zero = vdupq_n_u32(0);
  uint32x4_t acc = zero;
  for (u32 i = 0; i < 256; i += 4) {
    const uint32x4_t a = vandq_u32(vshrq_n_u32(vld1q_u32(line3d + i), 24), m);
    acc = vorrq_u32(acc, vbicq_u32(vmvnq_u32(vceqq_u32(a, zero)), vceqq_u32(a, v31)));
  }
  return compat::maxv_u32(acc) != 0;
}




void composite_line(u32 bldcnt, u32 eva, u32 evb, u32 evy, const Pixel* top, const Pixel* second,
                    const u8* top_id, const u8* top_kind, const u8* top_alpha, const u8* second_id,
                    const u8* win, Pixel* out) {
  const u32 effect = (bldcnt >> 6) & 3;
  const uint8x16_t v_t1 = vdupq_n_u8(effect ? static_cast<u8>(bldcnt) : 0), v_t2 = vdupq_n_u8(static_cast<u8>(bldcnt >> 8));
  const uint8x16_t veva = vdupq_n_u8(static_cast<u8>(eva)), vevb = vdupq_n_u8(static_cast<u8>(evb));
  const uint8x8_t vevy = vdup_n_u8(static_cast<u8>(evy));
  const uint8x16_t v63 = vdupq_n_u8(63), vff = vdupq_n_u8(0xFF), v16 = vdupq_n_u8(16), v32 = vdupq_n_u8(32);
  for (u32 i = 0; i < 256; i += 16) {
    const uint8x16_t kind = vld1q_u8(top_kind + i);
    const uint8x16_t t2hit = vtstq_u8(vld1q_u8(second_id + i), v_t2);
    // No effect selected -> v_t1 = 0, so t1hit/fx is empty.
    const uint8x16_t t1hit = vandq_u8(vtstq_u8(vld1q_u8(top_id + i), v_t1), vtstq_u8(vld1q_u8(win + i), vdupq_n_u8(0x20)));
    const uint8x16_t is_bitmap = vceqq_u8(kind, vdupq_n_u8(K_OBJ_BITMAP));
    const uint8x16_t objblend = vandq_u8(vorrq_u8(vceqq_u8(kind, vdupq_n_u8(K_OBJ_SEMI)), is_bitmap), t2hit);
    const uint8x16_t blend3d = vandq_u8(vceqq_u8(kind, vdupq_n_u8(K_3D)), t2hit);
    const uint8x16_t fx = vbicq_u8(vbicq_u8(t1hit, objblend), blend3d);
    // Most blocks blend nothing: copy through whole records, no de-interleave.
    if (compat::maxv_u8(vorrq_u8(vorrq_u8(objblend, blend3d), fx)) == 0) {
      const uint32x4_t alpha = vdupq_n_u32(0xFF000000);
      for (u32 k = 0; k < 16; k += 4) vst1q_u32(out + i + k, vorrq_u32(vld1q_u32(top + i + k), alpha));
      continue;
    }
    uint8x16x4_t a = vld4q_u8(reinterpret_cast<const u8*>(top + i));
    const bool has_obj = compat::maxv_u8(objblend) != 0, has_3d = compat::maxv_u8(blend3d) != 0, has_fx = compat::maxv_u8(fx) != 0;
    const uint8x16x4_t b = vld4q_u8(reinterpret_cast<const u8*>(second + i));
    uint8x16_t a6[3], b6[3], o[3];
    for (u32 c = 0; c < 3; ++c) { a6[c] = vandq_u8(a.val[c], v63); b6[c] = vandq_u8(b.val[c], v63); o[c] = a.val[c]; }
    if (has_fx) {
      for (u32 c = 0; c < 3; ++c) {
        uint8x16_t o_fx = a.val[c];
        if (effect == 1) o_fx = vbslq_u8(t2hit, blend16<4>(a6[c], b6[c], veva, vevb), a.val[c]);
        else if (effect == 2) o_fx = brighten16(a6[c], vevy, 8);
        else if (effect == 3) o_fx = darken16(a6[c], vevy, 7);
        o[c] = vbslq_u8(fx, o_fx, a.val[c]);
      }
    }
    if (has_3d) {
      // 3D blend with (alpha + 1) of 32; alpha 31 passes the top record through.
      const uint8x16_t a3 = vaddq_u8(vandq_u8(a.val[3], vdupq_n_u8(0x1F)), vdupq_n_u8(1));
      const uint8x16_t eb3 = vsubq_u8(v32, a3), keep = vceqq_u8(a3, v32);
      for (u32 c = 0; c < 3; ++c) o[c] = vbslq_u8(blend3d, vbslq_u8(keep, a.val[c], blend16<5>(a6[c], b6[c], a3, eb3)), o[c]);
    }
    if (has_obj) {
      // OBJ blend: bitmap sprites use their own alpha as EVA, 16 - EVA as EVB.
      const uint8x16_t ea = vbslq_u8(is_bitmap, vld1q_u8(top_alpha + i), veva);
      const uint8x16_t eb = vbslq_u8(is_bitmap, vsubq_u8(v16, ea), vevb);
      for (u32 c = 0; c < 3; ++c) o[c] = vbslq_u8(objblend, blend16<4>(a6[c], b6[c], ea, eb), o[c]);
    }
    const uint8x16x4_t rec = {o[0], o[1], o[2], vff};
    vst4q_u8(reinterpret_cast<u8*>(out + i), rec);
  }
}

void palette_to_18(const u16* pal, Pixel* out, u32 n) {
  for (u32 i = 0; i < n; i += 8) {
    const uint16x8_t c = vld1q_u16(pal + i);
    const uint16x8_t r = vshlq_n_u16(vandq_u16(c, vdupq_n_u16(0x1F)), 1);
    const uint16x8_t g = vorrq_u16(vshrq_n_u16(vandq_u16(c, vdupq_n_u16(0x3E0)), 4), vshrq_n_u16(c, 15));
    const uint16x8_t b = vshrq_n_u16(vandq_u16(c, vdupq_n_u16(0x7C00)), 9);
    const uint32x4_t lo = vorrq_u32(vorrq_u32(vmovl_u16(vget_low_u16(r)), vshlq_n_u32(vmovl_u16(vget_low_u16(g)), 8)), vshlq_n_u32(vmovl_u16(vget_low_u16(b)), 16));
    const uint32x4_t hi = vorrq_u32(vorrq_u32(vmovl_u16(vget_high_u16(r)), vshlq_n_u32(vmovl_u16(vget_high_u16(g)), 8)), vshlq_n_u32(vmovl_u16(vget_high_u16(b)), 16));
    vst1q_u32(out + i, lo);
    vst1q_u32(out + i + 4, hi);
  }
}

// 16-entry palette lookup as a 64-byte table lookup: each index is expanded
// to the four byte offsets of its 32-bit record.

namespace {
inline uint8x16x4_t pal16_table(const Pixel* pal) {
  const u8* p = reinterpret_cast<const u8*>(pal);
  return {{vld1q_u8(p), vld1q_u8(p + 16), vld1q_u8(p + 32), vld1q_u8(p + 48)}};
}
// 8 indices -> 8 records through a 16-entry palette held as a 64-byte table.
inline void pal16_row(uint8x8_t idx, const uint8x16x4_t& table, Pixel* px) {
  const uint8x8_t i4 = vshl_n_u8(idx, 2);
  const uint8x8x2_t z1 = vzip_u8(i4, i4);
  const uint8x16_t twice = vcombine_u8(z1.val[0], z1.val[1]);
  const uint8x16x2_t z2 = vzipq_u8(twice, twice);
  const uint8x16_t step = {0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3};
  vst1q_u8(reinterpret_cast<u8*>(px), compat::tbl4q_u8(table, vaddq_u8(z2.val[0], step)));
  vst1q_u8(reinterpret_cast<u8*>(px) + 16, compat::tbl4q_u8(table, vaddq_u8(z2.val[1], step)));
}
}



namespace {
// Priority rule on 16 plane entries; `opq` marks opaque sprite pixels, `valid` the lanes inside the row.
inline void obj_plot16(uint8x16_t opq, uint8x16_t valid, uint16x8_t v0, uint16x8_t v1, u8 attr, u8 alpha, u16* px, u8* oattr, u8* oalpha) {
  const uint8x16_t old = vld1q_u8(oattr);
  const uint8x16_t old_opaque = vtstq_u8(old, vdupq_n_u8(OA_OPAQUE));
  const uint8x16_t higher = vcgtq_u8(vandq_u8(old, vdupq_n_u8(OA_PRIO)), vdupq_n_u8(attr & OA_PRIO));
  const uint8x16_t win = vandq_u8(vandq_u8(opq, vorrq_u8(vmvnq_u8(old_opaque), higher)), valid);
  const uint8x16_t stamp = vandq_u8(vbicq_u8(vmvnq_u8(opq), old_opaque), valid);
  uint8x16_t a = vbslq_u8(stamp, vorrq_u8(vandq_u8(old, vdupq_n_u8(static_cast<u8>(~(OA_MOSAIC | OA_PRIO)))), vdupq_n_u8(attr & (OA_TOUCHED | OA_MOSAIC | OA_PRIO))), old);
  a = vbslq_u8(win, vdupq_n_u8(attr | OA_OPAQUE), a);
  vst1q_u8(oattr, a);
  vst1q_u8(oalpha, vbslq_u8(win, vdupq_n_u8(alpha), vld1q_u8(oalpha)));
  const int8x16_t ws = vreinterpretq_s8_u8(win);
  const uint16x8_t m0 = vreinterpretq_u16_s16(vmovl_s8(vget_low_s8(ws))), m1 = vreinterpretq_u16_s16(vmovl_s8(vget_high_s8(ws)));
  vst1q_u16(px, vbslq_u16(m0, v0, vld1q_u16(px)));
  vst1q_u16(px + 8, vbslq_u16(m1, v1, vld1q_u16(px + 8)));
}
const uint8x16_t kLane16 = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
}




// Table pointer hoisted per 16 pixels when ids agree (lines are usually long runs of one layer).
void resolve16_top(const u16* top, const u8* top_tid, const Pixel* const* tables, const Pixel* line3d,
                   Pixel* top_px, u8* top_id) {
  static const u8 id_tab[16] = {L_BG0, L_BG1, L_BG2, L_BG3, L_OBJ, L_OBJ, L_OBJ, L_BACKDROP, 0, 0, 0, 0, 0, 0, 0, 0};
  const uint8x16_t ids = vld1q_u8(id_tab);
  for (u32 i = 0; i < 256; i += 16) {
    const uint8x16_t tt = vld1q_u8(top_tid + i);
    vst1q_u8(top_id + i, compat::tbl1q_u8(ids, tt));
    const u8 t0 = top_tid[i];
    if (compat::uniform_u8_mem(top_tid + i) && !(line3d && t0 == T_BG0)) {
      const Pixel* tab = tables[t0];
      if (tab == direct_table()) direct16(top + i, top_px + i);
      else for (u32 k = 0; k < 16; ++k) top_px[i + k] = tab[top[i + k] & 0x7FFF] | 0xFF000000;
    } else {
      for (u32 k = 0; k < 16; ++k) {
        const u8 t = top_tid[i + k];
        top_px[i + k] = (line3d && t == T_BG0) ? line3d[i + k] : resolve_one(tables[t], top[i + k]);
      }
    }
  }
}

void composite_line_fade(u32 bldcnt, u32 evy, const Pixel* top, const u8* top_id, const u8* win, Pixel* out) {
  const u32 effect = (bldcnt >> 6) & 3;
  const uint8x16_t v_t1 = vdupq_n_u8(static_cast<u8>(bldcnt));
  const uint8x16_t v63 = vdupq_n_u8(63), vff = vdupq_n_u8(0xFF);
  const uint8x8_t vevy = vdup_n_u8(static_cast<u8>(evy));
  for (u32 i = 0; i < 256; i += 16) {
    const uint8x16_t hit = vandq_u8(vtstq_u8(vld1q_u8(top_id + i), v_t1), vtstq_u8(vld1q_u8(win + i), vdupq_n_u8(0x20)));
    uint8x16x4_t a = vld4q_u8(reinterpret_cast<const u8*>(top + i));
    for (u32 c = 0; c < 3; ++c) {
      const uint8x16_t a6 = vandq_u8(a.val[c], v63);
      a.val[c] = vbslq_u8(hit, effect == 2 ? brighten16(a6, vevy, 8) : darken16(a6, vevy, 7), a.val[c]);
    }
    a.val[3] = vff;
    vst4q_u8(reinterpret_cast<u8*>(out + i), a);
  }
}

void resolve16_full(const u16* top, const u8* top_tid, const u16* second, const u8* second_tid,
                    const Pixel* const* tables, const u8* attr, const u8* alpha, const Pixel* line3d,
                    Pixel* top_px, Pixel* second_px, u8* top_id, u8* top_kind, u8* top_alpha, u8* second_id) {
  static const u8 id_tab[16] = {L_BG0, L_BG1, L_BG2, L_BG3, L_OBJ, L_OBJ, L_OBJ, L_BACKDROP, 0, 0, 0, 0, 0, 0, 0, 0};
  const uint8x16_t ids = vld1q_u8(id_tab), zero = vdupq_n_u8(0);
  for (u32 i = 0; i < 256; i += 16) {
    const uint8x16_t tt = vld1q_u8(top_tid + i), st = vld1q_u8(second_tid + i);
    vst1q_u8(top_id + i, compat::tbl1q_u8(ids, tt));
    vst1q_u8(second_id + i, compat::tbl1q_u8(ids, st));
    const uint8x16_t at = vld1q_u8(attr + i);
    const uint8x16_t isobj = vandq_u8(vcgeq_u8(tt, vdupq_n_u8(T_OBJ_STD)), vcleq_u8(tt, vdupq_n_u8(T_OBJ_DIRECT)));
    uint8x16_t kind = vbslq_u8(vtstq_u8(at, vdupq_n_u8(OA_BITMAP)), vdupq_n_u8(K_OBJ_BITMAP), vbslq_u8(vtstq_u8(at, vdupq_n_u8(OA_SEMI)), vdupq_n_u8(K_OBJ_SEMI), zero));
    kind = vandq_u8(kind, isobj);
    uint8x16_t al = vandq_u8(vld1q_u8(alpha + i), isobj);
    if (line3d) {
      const uint8x16_t is3d = vceqq_u8(tt, zero);
      kind = vbslq_u8(is3d, vdupq_n_u8(K_3D), kind);
      const uint8x16_t a3 = narrow(vshrq_n_u32(vld1q_u32(line3d + i), 24), vshrq_n_u32(vld1q_u32(line3d + i + 4), 24),
                                  vshrq_n_u32(vld1q_u32(line3d + i + 8), 24), vshrq_n_u32(vld1q_u32(line3d + i + 12), 24));
      al = vbslq_u8(is3d, vandq_u8(a3, vdupq_n_u8(0x1F)), al);
    }
    vst1q_u8(top_kind + i, kind);
    vst1q_u8(top_alpha + i, al);
    // Hoist both table pointers when the block is uniform; the 3D override
    // stays per-pixel by disabling the top hoist when BG0 is selected.
    const u8 t0 = top_tid[i], s0 = second_tid[i];
    const bool top_run = compat::uniform_u8_mem(top_tid + i) && !(line3d && t0 == T_BG0);
    const bool second_run = compat::uniform_u8_mem(second_tid + i);
    if (top_run && second_run) {
      const Pixel* top_tab = tables[t0];
      const Pixel* second_tab = tables[s0];
      if (top_tab == direct_table()) direct16(top + i, top_px + i);
      else for (u32 k = 0; k < 16; ++k) top_px[i + k] = top_tab[top[i + k] & 0x7FFF] | 0xFF000000;
      if (second_tab == direct_table()) direct16(second + i, second_px + i);
      else for (u32 k = 0; k < 16; ++k) second_px[i + k] = second_tab[second[i + k] & 0x7FFF] | 0xFF000000;
    } else {
      for (u32 k = 0; k < 16; ++k) {
        const u8 t = top_tid[i + k];
        top_px[i + k] = (line3d && t == T_BG0) ? line3d[i + k] : resolve_one(tables[t], top[i + k]);
        second_px[i + k] = resolve_one(tables[second_tid[i + k]], second[i + k]);
      }
    }
  }
}

void obj_row_idx16(const u8* idx, u32 n, u16 pal_base, u8 attr, u16* v, u8* oattr, u8* oalpha) {
  const uint16x8_t base = vdupq_n_u16(static_cast<u16>(LV_OPAQUE | pal_base));
  for (u32 i = 0; i < n; i += 16) {
    const uint8x16_t x = vld1q_u8(idx + i);
    const uint8x16_t valid = vcltq_u8(kLane16, vdupq_n_u8(static_cast<u8>(n - i > 16 ? 16 : n - i)));
    obj_plot16(vmvnq_u8(vceqq_u8(x, vdupq_n_u8(0))), valid, vorrq_u16(base, vmovl_u8(vget_low_u8(x))), vorrq_u16(base, vmovl_u8(vget_high_u8(x))),
               attr, 0, v + i, oattr + i, oalpha + i);
  }
}

void obj_row_bmp16(const u16* col, u32 n, u8 attr, u8 alpha, u16* v, u8* oattr, u8* oalpha) {
  for (u32 i = 0; i < n; i += 16) {
    const uint16x8_t a = vld1q_u16(col + i), b = vld1q_u16(col + i + 8);
    const uint8x16_t opaque = vtstq_u8(vcombine_u8(vshrn_n_u16(a, 8), vshrn_n_u16(b, 8)), vdupq_n_u8(0x80));
    const uint8x16_t valid = vcltq_u8(kLane16, vdupq_n_u8(static_cast<u8>(n - i > 16 ? 16 : n - i)));
    obj_plot16(opaque, valid, a, b, attr, alpha, v + i, oattr + i, oalpha + i);
  }
}

bool layer16_3d(const u32* line3d, u16* v) {
  const uint16x8_t lane = {0, 1, 2, 3, 4, 5, 6, 7}, opq = vdupq_n_u16(LV_OPAQUE);
  uint16x8_t any = vdupq_n_u16(0);
  for (u32 i = 0; i < 256; i += 8) {
    const uint16x8_t alpha = vcombine_u16(vmovn_u32(vshrq_n_u32(vld1q_u32(line3d + i), 24)), vmovn_u32(vshrq_n_u32(vld1q_u32(line3d + i + 4), 24)));
    const uint16x8_t m = vmvnq_u16(vceqq_u16(alpha, vdupq_n_u16(0)));
    any = vorrq_u16(any, m);
    vst1q_u16(v + i, vandq_u16(m, vorrq_u16(opq, vaddq_u16(lane, vdupq_n_u16(static_cast<u16>(i))))));
  }
  return compat::maxv_u16(any) != 0;
}

namespace {
// Two text tiles (16 pixels): flip is a per-half byte reverse gated by ctl
// bit 4, palette from ctl bits 0-3 (pal_shift 4/8/none), index 0 transparent.
template <int pal_shift>
[[gnu::always_inline]] inline uint8x16_t text_pair(uint8x16_t idx, const u8* ctl, u16* v) {
  const uint8x8_t c2 = vreinterpret_u8_u16(vld1_dup_u16(reinterpret_cast<const u16*>(ctl)));   // [c0 c1 c0 c1 ..]
  const uint8x16_t ctl16 = vcombine_u8(vdup_lane_u8(c2, 0), vdup_lane_u8(c2, 1));
  idx = vbslq_u8(vtstq_u8(ctl16, vdupq_n_u8(0x10)), vrev64q_u8(idx), idx);
  const int8x16_t nz = vreinterpretq_s8_u8(vtstq_u8(idx, idx));
  const uint16x8_t m0 = vreinterpretq_u16_s16(vmovl_s8(vget_low_s8(nz))), m1 = vreinterpretq_u16_s16(vmovl_s8(vget_high_s8(nz)));
  uint16x8_t o0 = vorrq_u16(vmovl_u8(vget_low_u8(idx)), vdupq_n_u16(LV_OPAQUE));
  uint16x8_t o1 = vorrq_u16(vmovl_u8(vget_high_u8(idx)), vdupq_n_u16(LV_OPAQUE));
  if (pal_shift == 4) {
    const uint8x16_t base = vshlq_n_u8(vandq_u8(ctl16, vdupq_n_u8(0xF)), 4);
    o0 = vorrq_u16(o0, vmovl_u8(vget_low_u8(base))); o1 = vorrq_u16(o1, vmovl_u8(vget_high_u8(base)));
  } else if (pal_shift == 8) {
    const uint8x16_t pal = vandq_u8(ctl16, vdupq_n_u8(0xF));
    o0 = vorrq_u16(o0, vshll_n_u8(vget_low_u8(pal), 8)); o1 = vorrq_u16(o1, vshll_n_u8(vget_high_u8(pal), 8));
  }
  vst1q_u16(v, vandq_u16(m0, o0));
  vst1q_u16(v + 8, vandq_u16(m1, o1));
  return idx;
}
// One tile, for an odd tail.
[[gnu::always_inline]] inline uint8x8_t text_one(uint8x8_t idx, u8 c, u16 base, u16* v) {
  idx = vbsl_u8(vdup_n_u8((c & 0x10) ? 0xFF : 0), vrev64_u8(idx), idx);
  const uint16x8_t i16 = vmovl_u8(idx);
  const uint16x8_t m = vmvnq_u16(vceqq_u16(i16, vdupq_n_u16(0)));
  vst1q_u16(v, vandq_u16(m, vorrq_u16(i16, vdupq_n_u16(static_cast<u16>(LV_OPAQUE | base)))));
  return idx;
}
}  // namespace

bool text_ctl(const u16* tiles, u8* ctl) {
  const uint16x8_t t0 = vdupq_n_u16(tiles[0]);
  uint16x8_t same = vdupq_n_u16(0xFFFF);
  for (u32 t = 0; t < 32; t += 8) {
    const uint16x8_t tv = vld1q_u16(tiles + t);
    vst1_u8(ctl + t, vorr_u8(vmovn_u16(vshrq_n_u16(tv, 12)), vand_u8(vshrn_n_u16(tv, 6), vdup_n_u8(0x10))));
    same = vandq_u16(same, vceqq_u16(tv, t0));
  }
  ctl[32] = static_cast<u8>((tiles[32] >> 12) | ((tiles[32] >> 6) & 0x10));
  return compat::minv_u16(same) != 0 && tiles[32] == tiles[0];
}

bool text_row_16(const u8* packed, const u8* ctl, u32 n, u16* v) {
  uint8x16_t any = vdupq_n_u8(0);
  u32 t = 0;
  for (; t + 2 <= n; t += 2, packed += 8, v += 16) {
    const uint8x8_t raw = vld1_u8(packed);                    // two tiles' packed nibbles
    const uint8x8x2_t nib = vzip_u8(vand_u8(raw, vdup_n_u8(0xF)), vshr_n_u8(raw, 4));
    any = vorrq_u8(any, text_pair<4>(vcombine_u8(nib.val[0], nib.val[1]), ctl + t, v));
  }
  if (t < n) {
    u32 w; std::memcpy(&w, packed, 4);
    const uint8x8_t raw = vreinterpret_u8_u32(vdup_n_u32(w));
    const uint8x8x2_t nib = vzip_u8(vand_u8(raw, vdup_n_u8(0xF)), vshr_n_u8(raw, 4));
    any = vorrq_u8(any, vcombine_u8(text_one(nib.val[0], ctl[t], static_cast<u16>((ctl[t] & 0xF) << 4), v), vdup_n_u8(0)));
  }
  return compat::maxv_u8(any) != 0;
}

bool text_row_256(const u8* rows, const u8* ctl, u32 n, bool ext, u16* v) {
  uint8x16_t any = vdupq_n_u8(0);
  u32 t = 0;
  for (; t + 2 <= n; t += 2, rows += 16, v += 16) {
    const uint8x16_t idx = vld1q_u8(rows);
    any = vorrq_u8(any, ext ? text_pair<8>(idx, ctl + t, v) : text_pair<0>(idx, ctl + t, v));
  }
  if (t < n) any = vorrq_u8(any, vcombine_u8(text_one(vld1_u8(rows), ctl[t], ext ? static_cast<u16>((ctl[t] & 0xF) << 8) : 0, v), vdup_n_u8(0)));
  return compat::maxv_u8(any) != 0;
}

namespace {
// 16 lanes of the window test for a BG (bit wbit) or OBJ (bit 4).
inline uint8x16_t win_mask(const u8* win, u8 wbit) { return vtstq_u8(vld1q_u8(win), vdupq_n_u8(wbit)); }
// u16 opaque bits of 16 values as a byte mask.
inline uint8x16_t opaque_mask(uint16x8_t a, uint16x8_t b) { return vtstq_u8(vcombine_u8(vshrn_n_u16(a, 8), vshrn_n_u16(b, 8)), vdupq_n_u8(0x80)); }
inline void split16(uint8x16_t m8, uint16x8_t& m0, uint16x8_t& m1) {
  const int8x16_t ws = vreinterpretq_s8_u8(m8);
  m0 = vreinterpretq_u16_s16(vmovl_s8(vget_low_s8(ws))); m1 = vreinterpretq_u16_s16(vmovl_s8(vget_high_s8(ws)));
}
// The OBJ table id per lane from the attribute byte.
inline uint8x16_t obj_tid16(uint8x16_t a) {
  const uint8x16_t bitmap = vtstq_u8(a, vdupq_n_u8(OA_BITMAP)), std = vtstq_u8(a, vdupq_n_u8(OA_STDPAL));
  return vbslq_u8(bitmap, vdupq_n_u8(T_OBJ_DIRECT), vbslq_u8(std, vdupq_n_u8(T_OBJ_STD), vdupq_n_u8(T_OBJ_EXT)));
}
inline void merge16(uint8x16_t m8, uint16x8_t v0, uint16x8_t v1, uint8x16_t tid, u16* top, u8* top_tid, u16* second, u8* second_tid) {
  uint16x8_t m0, m1; split16(m8, m0, m1);
  const uint16x8_t t0 = vld1q_u16(top), t1 = vld1q_u16(top + 8);
  if (second) {
    vst1q_u16(second, vbslq_u16(m0, t0, vld1q_u16(second)));
    vst1q_u16(second + 8, vbslq_u16(m1, t1, vld1q_u16(second + 8)));
    vst1q_u8(second_tid, vbslq_u8(m8, vld1q_u8(top_tid), vld1q_u8(second_tid)));
  }
  vst1q_u16(top, vbslq_u16(m0, v0, t0));
  vst1q_u16(top + 8, vbslq_u16(m1, v1, t1));
  vst1q_u8(top_tid, vbslq_u8(m8, tid, vld1q_u8(top_tid)));
}
}

// Never tests the mask before merging: a vmaxvq + branch readback costs more
// than the merge it would skip, even on a mostly-transparent line.
void select16_nowin(const u16* v, u8 tid, u16* top, u8* top_tid, u16* second, u8* second_tid) {
  const uint8x16_t vtid = vdupq_n_u8(tid);
  for (u32 i = 0; i < 256; i += 16)
    merge16(opaque_mask(vld1q_u16(v + i), vld1q_u16(v + i + 8)), vld1q_u16(v + i), vld1q_u16(v + i + 8), vtid,
            top + i, top_tid + i, second + i, second_tid + i);
}

void select16_obj_nowin(const u16* v, const u8* attr, u32 prio, u16* top, u8* top_tid, u16* second, u8* second_tid) {
  const uint8x16_t vprio = vdupq_n_u8(static_cast<u8>(prio));
  for (u32 i = 0; i < 256; i += 16) {
    const uint8x16_t a = vld1q_u8(attr + i);
    const uint8x16_t m8 = vandq_u8(vtstq_u8(a, vdupq_n_u8(OA_OPAQUE)), vceqq_u8(vandq_u8(a, vdupq_n_u8(OA_PRIO)), vprio));
    merge16(m8, vld1q_u16(v + i), vld1q_u16(v + i + 8), obj_tid16(a), top + i, top_tid + i, second + i, second_tid + i);
  }
}

void select16(const u16* v, const u8* win, u8 wbit, u8 tid, u16* top, u8* top_tid, u16* second, u8* second_tid) {
  const uint8x16_t vtid = vdupq_n_u8(tid);
  for (u32 i = 0; i < 256; i += 32) {
    for (u32 j = i; j < i + 32; j += 16) {
      const uint16x8_t a = vld1q_u16(v + j), b = vld1q_u16(v + j + 8);
      const uint8x16_t m8 = vandq_u8(opaque_mask(a, b), win_mask(win + j, wbit));
      merge16(m8, a, b, vtid, top + j, top_tid + j, second + j, second_tid + j);
    }
  }
}

void select16_obj(const u16* v, const u8* attr, const u8* win, u32 prio, u16* top, u8* top_tid, u16* second, u8* second_tid) {
  const uint8x16_t vprio = vdupq_n_u8(static_cast<u8>(prio));
  for (u32 i = 0; i < 256; i += 16) {
    const uint8x16_t a = vld1q_u8(attr + i);
    uint8x16_t m8 = vandq_u8(vtstq_u8(a, vdupq_n_u8(OA_OPAQUE)), vceqq_u8(vandq_u8(a, vdupq_n_u8(OA_PRIO)), vprio));
    m8 = vandq_u8(m8, win_mask(win + i, 0x10));
    merge16(m8, vld1q_u16(v + i), vld1q_u16(v + i + 8), obj_tid16(a), top + i, top_tid + i, second + i, second_tid + i);
  }
}

void select16_flat_nowin(const u16* v, u8 tid, u16* top, u8* top_tid) {
  const uint8x16_t vtid = vdupq_n_u8(tid);
  for (u32 i = 0; i < 256; i += 16)
    merge16(opaque_mask(vld1q_u16(v + i), vld1q_u16(v + i + 8)), vld1q_u16(v + i), vld1q_u16(v + i + 8), vtid,
            top + i, top_tid + i, nullptr, nullptr);
}

void select16_obj_flat_nowin(const u16* v, const u8* attr, u32 prio, u16* top, u8* top_tid) {
  const uint8x16_t vprio = vdupq_n_u8(static_cast<u8>(prio));
  for (u32 i = 0; i < 256; i += 16) {
    const uint8x16_t a = vld1q_u8(attr + i);
    const uint8x16_t m8 = vandq_u8(vtstq_u8(a, vdupq_n_u8(OA_OPAQUE)), vceqq_u8(vandq_u8(a, vdupq_n_u8(OA_PRIO)), vprio));
    merge16(m8, vld1q_u16(v + i), vld1q_u16(v + i + 8), obj_tid16(a), top + i, top_tid + i, nullptr, nullptr);
  }
}

void select16_flat(const u16* v, const u8* win, u8 wbit, u8 tid, u16* top, u8* top_tid) {
  const uint8x16_t vtid = vdupq_n_u8(tid);
  for (u32 i = 0; i < 256; i += 32) {
    for (u32 j = i; j < i + 32; j += 16) {
      const uint16x8_t a = vld1q_u16(v + j), b = vld1q_u16(v + j + 8);
      const uint8x16_t m8 = vandq_u8(opaque_mask(a, b), win_mask(win + j, wbit));
      merge16(m8, a, b, vtid, top + j, top_tid + j, nullptr, nullptr);
    }
  }
}

void select16_obj_flat(const u16* v, const u8* attr, const u8* win, u32 prio, u16* top, u8* top_tid) {
  const uint8x16_t vprio = vdupq_n_u8(static_cast<u8>(prio));
  for (u32 i = 0; i < 256; i += 16) {
    const uint8x16_t a = vld1q_u8(attr + i);
    uint8x16_t m8 = vandq_u8(vtstq_u8(a, vdupq_n_u8(OA_OPAQUE)), vceqq_u8(vandq_u8(a, vdupq_n_u8(OA_PRIO)), vprio));
    m8 = vandq_u8(m8, win_mask(win + i, 0x10));
    merge16(m8, vld1q_u16(v + i), vld1q_u16(v + i + 8), obj_tid16(a), top + i, top_tid + i, nullptr, nullptr);
  }
}

void resolve16(const u16* top, const u8* top_tid, const Pixel* const* tables, Pixel* out) {
  // Table pointer hoisted per 16 pixels when ids agree (runs of one table are the common case).
  for (u32 i = 0; i < 256; i += 16) {
    if (compat::uniform_u8_mem(top_tid + i)) {
      const Pixel* tab = tables[top_tid[i]];
      if (tab == direct_table()) direct16(top + i, out + i);
      else for (u32 k = 0; k < 16; ++k) out[i + k] = tab[top[i + k] & 0x7FFF] | 0xFF000000;
    } else {
      for (u32 k = 0; k < 16; ++k) out[i + k] = resolve_one(tables[top_tid[i + k]], top[i + k]);
    }
  }
}

void resolve16_one(const u16* v, const Pixel* table, Pixel* out) {
  if (table == direct_table()) { for (u32 i = 0; i < 256; i += 16) direct16(v + i, out + i); return; }
  // Scalar on purpose: eight independent table loads in flight per iteration keeps an
  // in-order core busy through load latency; nothing crosses the vector/GPR boundary.
  for (u32 i = 0; i < 256; i += 8) {
    u64 a, b; std::memcpy(&a, v + i, 8); std::memcpy(&b, v + i + 4, 8);
    Pixel o[8];
    o[0] = table[(a      ) & 0x7FFF] | 0xFF000000; o[1] = table[(a >> 16) & 0x7FFF] | 0xFF000000;
    o[2] = table[(a >> 32) & 0x7FFF] | 0xFF000000; o[3] = table[(a >> 48) & 0x7FFF] | 0xFF000000;
    o[4] = table[(b      ) & 0x7FFF] | 0xFF000000; o[5] = table[(b >> 16) & 0x7FFF] | 0xFF000000;
    o[6] = table[(b >> 32) & 0x7FFF] | 0xFF000000; o[7] = table[(b >> 48) & 0x7FFF] | 0xFF000000;
    std::memcpy(out + i, o, sizeof o);
  }
}

// 16 texels a step, then 8, then scalar: nothing may be written past n.
bool bmp_row_8(const u8* idx, u32 n, u16* v) {
  const uint16x8_t op = vdupq_n_u16(0x8000);
  uint8x16_t acc = vdupq_n_u8(0);
  u32 i = 0;
  auto eight = [&](uint8x8_t x) {
    const uint16x8_t w = vmovl_u8(x), nz = vandq_u16(vshll_n_u8(vtst_u8(x, x), 8), op);
    return vorrq_u16(w, nz);
  };
  for (; i + 16 <= n; i += 16) {
    const uint8x16_t x = vld1q_u8(idx + i);
    acc = vorrq_u8(acc, x);
    vst1q_u16(v + i, eight(vget_low_u8(x)));
    vst1q_u16(v + i + 8, eight(vget_high_u8(x)));
  }
  if (i + 8 <= n) {
    const uint8x8_t x = vld1_u8(idx + i);
    acc = vorrq_u8(acc, vcombine_u8(x, x));
    vst1q_u16(v + i, eight(x));
    i += 8;
  }
  bool any = compat::maxv_u8(acc) != 0;
  for (; i < n; ++i) { const u8 x = idx[i]; v[i] = x ? static_cast<u16>(LV_OPAQUE | x) : 0; any |= x != 0; }
  return any;
}
bool bmp_row_16(const u16* col, u32 n, u16* v) {
  const uint16x8_t op = vdupq_n_u16(0x8000);
  uint16x8_t acc = vdupq_n_u16(0);
  u32 i = 0;
  for (; i + 8 <= n; i += 8) {
    const uint16x8_t c = vld1q_u16(col + i);
    const uint16x8_t o = vandq_u16(c, vtstq_u16(c, op));
    acc = vorrq_u16(acc, o);
    vst1q_u16(v + i, o);
  }
  bool any = compat::maxv_u16(acc) != 0;
  for (; i < n; ++i) { const u16 c = col[i]; v[i] = (c & 0x8000) ? c : 0; any |= (c & 0x8000) != 0; }
  return any;
}

void master_brightness(u16 reg, u32* dst) {
  const u32 mode = reg >> 14;
  u32 factor = reg & 0x1F;
  if (factor > 16) factor = 16;
  if (mode != 1 && mode != 2) return;
  const uint32x4_t mrb = vdupq_n_u32(0x3F003F), mg = vdupq_n_u32(0x003F00), vf = vdupq_n_u32(factor), opaque = vdupq_n_u32(0xFF000000);
  for (u32 i = 0; i < 256; i += 4) {
    const uint32x4_t v = vld1q_u32(dst + i);
    const uint32x4_t rb = vandq_u32(v, mrb), g = vandq_u32(v, mg);
    uint32x4_t o;
    if (mode == 1) {
      const uint32x4_t drb = vandq_u32(vshrq_n_u32(vmulq_u32(vsubq_u32(mrb, rb), vf), 4), mrb);
      const uint32x4_t dg = vandq_u32(vshrq_n_u32(vmulq_u32(vsubq_u32(mg, g), vf), 4), mg);
      o = vorrq_u32(vaddq_u32(rb, drb), vaddq_u32(g, dg));
    } else {
      const uint32x4_t drb = vandq_u32(vshrq_n_u32(vmlaq_u32(vdupq_n_u32(0xF * 0x010001), rb, vf), 4), mrb);
      const uint32x4_t dg = vandq_u32(vshrq_n_u32(vmlaq_u32(vdupq_n_u32(0xF * 0x000100), g, vf), 4), mg);
      o = vorrq_u32(vsubq_u32(rb, drb), vsubq_u32(g, dg));
    }
    vst1q_u32(dst + i, vorrq_u32(o, opaque));
  }
}

namespace {
inline uint32x4_t expand4(uint32x4_t c) {
  uint32x4_t v = vshlq_n_u32(vandq_u32(c, vdupq_n_u32(0x3F)), 18);
  v = vorrq_u32(v, vshlq_n_u32(vandq_u32(c, vdupq_n_u32(0x3F00)), 2));
  v = vorrq_u32(v, vshrq_n_u32(vandq_u32(c, vdupq_n_u32(0x3F0000)), 14));
  v = vorrq_u32(v, vshrq_n_u32(vandq_u32(v, vdupq_n_u32(0xC0C0C0)), 6));
  return vorrq_u32(v, vdupq_n_u32(0xFF000000));
}
}

void expand_colours(u32* dst) {
  for (u32 i = 0; i < 256; i += 4) vst1q_u32(dst + i, expand4(vld1q_u32(dst + i)));
}

// 6->8 expansion: (c<<2)|(c>>4) per plane, stored 0xAARRGGBB order by one st4.
void output_line(const Pixel* src, u16 reg, u32* dst) {
  const u32 mode = reg >> 14;
  u32 factor = reg & 0x1F;
  if (factor > 16) factor = 16;
  const uint8x8_t vf = vdup_n_u8(static_cast<u8>(factor));
  const uint8x16_t v63 = vdupq_n_u8(63), vff = vdupq_n_u8(0xFF);
  const bool bright = mode == 1, dark = mode == 2;
  for (u32 i = 0; i < 256; i += 16) {
    const uint8x16x4_t p = vld4q_u8(reinterpret_cast<const u8*>(src + i));
    uint8x16_t c[3];
    for (u32 k = 0; k < 3; ++k) {
      c[k] = vandq_u8(p.val[k], v63);
      if (bright) c[k] = brighten16(c[k], vf, 0);
      else if (dark) c[k] = darken16(c[k], vf, 15);
      c[k] = vorrq_u8(vshlq_n_u8(c[k], 2), vshrq_n_u8(c[k], 4));
    }
    const uint8x16x4_t rec = {c[2], c[1], c[0], vff};
    vst4q_u8(reinterpret_cast<u8*>(dst + i), rec);
  }
}

// BGR555 in: 5-bit fields narrowed to already-doubled byte planes, then the same brightness and expansion.
void output_vram_line(const u16* src, u16 reg, u32* dst) {
  const u32 mode = reg >> 14;
  u32 factor = reg & 0x1F;
  if (factor > 16) factor = 16;
  const uint8x8_t vf = vdup_n_u8(static_cast<u8>(factor));
  const uint8x16_t vff = vdupq_n_u8(0xFF);
  const uint16x8_t m3e = vdupq_n_u16(0x3E);
  const bool bright = mode == 1, dark = mode == 2;
  for (u32 i = 0; i < 256; i += 16) {
    const uint16x8_t lo = vld1q_u16(src + i), hi = vld1q_u16(src + i + 8);
    uint8x16_t c[3];
    c[0] = vcombine_u8(vmovn_u16(vandq_u16(vshlq_n_u16(lo, 1), m3e)), vmovn_u16(vandq_u16(vshlq_n_u16(hi, 1), m3e)));
    c[1] = vcombine_u8(vmovn_u16(vandq_u16(vshrq_n_u16(lo, 4), m3e)), vmovn_u16(vandq_u16(vshrq_n_u16(hi, 4), m3e)));
    c[2] = vcombine_u8(vmovn_u16(vandq_u16(vshrq_n_u16(lo, 9), m3e)), vmovn_u16(vandq_u16(vshrq_n_u16(hi, 9), m3e)));
    for (u32 k = 0; k < 3; ++k) {
      if (bright) c[k] = brighten16(c[k], vf, 0);
      else if (dark) c[k] = darken16(c[k], vf, 15);
      c[k] = vorrq_u8(vshlq_n_u8(c[k], 2), vshrq_n_u8(c[k], 4));
    }
    const uint8x16x4_t rec = {c[2], c[1], c[0], vff};
    vst4q_u8(reinterpret_cast<u8*>(dst + i), rec);
  }
}

// Runs are short (2-3 pixels at the scales the handhelds use, more on a wide
// panel), so this stores a quad when one fits and falls back to scalar for the
// tail rather than setting up a vector loop that rarely runs.
// Sixteen pixels a step in byte planes: one vld4 gives the record's four
// channels, the 5-bit channels are a shift and a mask, and the alpha bit
// lands in bit 15 by widening the 0/0xFF byte mask and shifting it 15 (only
// bit 0 survives the halfword).
void capture_a15(const Pixel* src, u32 n, u16* dst) {
  const uint8x16_t v1f = vdupq_n_u8(0x1F);
  for (u32 i = 0; i < n; i += 16) {
    const uint8x16x4_t p = vld4q_u8(reinterpret_cast<const u8*>(src + i));
    const uint8x16_t r = vandq_u8(vshrq_n_u8(p.val[0], 1), v1f);
    const uint8x16_t g = vandq_u8(vshrq_n_u8(p.val[1], 1), v1f);
    const uint8x16_t b = vandq_u8(vshrq_n_u8(p.val[2], 1), v1f);
    const uint8x16_t a = vtstq_u8(p.val[3], p.val[3]);
    auto half = [&](u32 o, uint8x8_t r8, uint8x8_t g8, uint8x8_t b8, uint8x8_t a8) {
      uint16x8_t w = vmovl_u8(r8);
      w = vorrq_u16(w, vshlq_n_u16(vmovl_u8(g8), 5));
      w = vorrq_u16(w, vshlq_n_u16(vmovl_u8(b8), 10));
      w = vorrq_u16(w, vshlq_n_u16(vmovl_u8(a8), 15));
      vst1q_u16(dst + i + o, w);
    };
    half(0, vget_low_u8(r), vget_low_u8(g), vget_low_u8(b), vget_low_u8(a));
    half(8, vget_high_u8(r), vget_high_u8(g), vget_high_u8(b), vget_high_u8(a));
  }
}

// Blend in 16-bit lanes: each product is at most 31*16, so both terms plus
// rounding bias fit a halfword. Per-source alpha becomes a 0/1 byte mask applied before the multiply.
void capture_blend(const Pixel* srca, const u16* srcb, u32 n, u32 eva, u32 evb, u16* dst) {
  const uint8x16_t v1f = vdupq_n_u8(0x1F);
  const uint8x8_t va8 = vdup_n_u8(static_cast<u8>(eva)), vb8 = vdup_n_u8(static_cast<u8>(evb));
  const uint8x16_t ea = vdupq_n_u8(eva ? 0xFF : 0), eb = vdupq_n_u8(evb ? 0xFF : 0);
  const uint16x8_t bias = vdupq_n_u16(8), v31 = vdupq_n_u16(31), topbit = vdupq_n_u16(0x8000);
  for (u32 i = 0; i < n; i += 16) {
    const uint8x16x4_t p = vld4q_u8(reinterpret_cast<const u8*>(srca + i));
    const uint8x16_t aam = vtstq_u8(p.val[3], p.val[3]);
    const uint8x16_t ra = vandq_u8(vandq_u8(vshrq_n_u8(p.val[0], 1), v1f), aam);
    const uint8x16_t ga = vandq_u8(vandq_u8(vshrq_n_u8(p.val[1], 1), v1f), aam);
    const uint8x16_t ba = vandq_u8(vandq_u8(vshrq_n_u8(p.val[2], 1), v1f), aam);
    const uint16x8_t w0 = vld1q_u16(srcb + i), w1 = vld1q_u16(srcb + i + 8);
    const uint8x16_t abm = vcombine_u8(vmovn_u16(vtstq_u16(w0, topbit)), vmovn_u16(vtstq_u16(w1, topbit)));
    auto bnarrow = [&](uint16x8_t c0, uint16x8_t c1) {
      return vandq_u8(vandq_u8(vcombine_u8(vmovn_u16(c0), vmovn_u16(c1)), v1f), abm);
    };
    const uint8x16_t rb = bnarrow(w0, w1);
    const uint8x16_t gb = bnarrow(vshrq_n_u16(w0, 5), vshrq_n_u16(w1, 5));
    const uint8x16_t bb = bnarrow(vshrq_n_u16(w0, 10), vshrq_n_u16(w1, 10));
    const uint8x16_t ad = vorrq_u8(vandq_u8(aam, ea), vandq_u8(abm, eb));
    auto half = [&](u32 o, auto lane) {
      auto blend1 = [&](uint8x16_t ca, uint8x16_t cb) {
        uint16x8_t s = vmlal_u8(vmull_u8(lane(ca), va8), lane(cb), vb8);
        return vminq_u16(vshrq_n_u16(vaddq_u16(s, bias), 4), v31);
      };
      uint16x8_t w = blend1(ra, rb);
      w = vorrq_u16(w, vshlq_n_u16(blend1(ga, gb), 5));
      w = vorrq_u16(w, vshlq_n_u16(blend1(ba, bb), 10));
      w = vorrq_u16(w, vshlq_n_u16(vmovl_u8(lane(ad)), 15));
      vst1q_u16(dst + i + o, w);
    };
    half(0, [](uint8x16_t v) { return vget_low_u8(v); });
    half(8, [](uint8x16_t v) { return vget_high_u8(v); });
  }
}

void scale_row(const u32* src, const u16* xrun, u32* dst) {
  for (u32 s = 0; s < 256; ++s) {
    const u32 c = src[s];
    u32 x = xrun[s];
    const u32 end = xrun[s + 1];
    const uint32x4_t v = vdupq_n_u32(c);
    for (; x + 4 <= end; x += 4) vst1q_u32(dst + x, v);
    for (; x < end; ++x) dst[x] = c;
  }
}

void scale_row_straddle(const u32* src, const u32* seam, const u8* w, const u16* xrun, u32* dst) {
  for (u32 s = 0; s < 256; ++s) {
    const u32 c = src[s];
    u32 x = xrun[s];
    const u32 end = xrun[s + 1];
    const u32 n = end - x;
    if (!w[s] || n == 0) {
      const uint32x4_t v = vdupq_n_u32(c);
      for (; x + 4 <= end; x += 4) vst1q_u32(dst + x, v);
      for (; x < end; ++x) dst[x] = c;
      continue;
    }
    const u32 cs = seam[s];
    switch (n) {
    case 1: dst[x] = cs; break;
    case 2: vst1_u32(dst + x, uint32x2_t{c, cs}); break;
    case 3: vst1_u32(dst + x, vdup_n_u32(c)); dst[x + 2] = cs; break;
    default: {
      const u32 last = end - 1;
      const uint32x4_t v = vdupq_n_u32(c);
      for (; x + 4 <= last; x += 4) vst1q_u32(dst + x, v);
      for (; x < last; ++x) dst[x] = c;
      dst[last] = cs;
    }
    }
  }
}

// (a*256 + b*w - a*w + 128) >> 8 per byte; intermediate wraps but true value <= 255*256+128, so exact.
void blend_line_w(const u32* a, const u32* b, const u8* w, u32* out) {
  const uint8x8_t k128 = vdup_n_u8(128);
  for (u32 i = 0; i < 256; i += 4) {
    const uint8x16_t va = vreinterpretq_u8_u32(vld1q_u32(a + i)), vb = vreinterpretq_u8_u32(vld1q_u32(b + i));
    const uint8x8_t w4 = vreinterpret_u8_u32(vdup_n_u32(*reinterpret_cast<const u32*>(w + i)));
    const uint8x8x2_t z1 = vzip_u8(w4, w4);              // w0 w0 w1 w1 w2 w2 w3 w3 ...
    const uint8x8x2_t z2 = vzip_u8(z1.val[0], z1.val[0]); // w0 w0 w0 w0 w1 w1 w1 w1 | w2 w2 w2 w2 w3 w3 w3 w3
    const uint8x8_t wlo = z2.val[0], whi = z2.val[1];
    uint16x8_t tlo = vshll_n_u8(vget_low_u8(va), 8), thi = vshll_n_u8(vget_high_u8(va), 8);
    tlo = vmlal_u8(tlo, vget_low_u8(vb), wlo);  thi = vmlal_u8(thi, vget_high_u8(vb), whi);
    tlo = vmlsl_u8(tlo, vget_low_u8(va), wlo);  thi = vmlsl_u8(thi, vget_high_u8(va), whi);
    tlo = vaddw_u8(tlo, k128);                  thi = vaddw_u8(thi, k128);
    vst1q_u32(out + i, vreinterpretq_u32_u8(vcombine_u8(vshrn_n_u16(tlo, 8), vshrn_n_u16(thi, 8))));
  }
}

// blend_line_w arithmetic, four pixels a step (weights pre-spread in wlo/whi).
static inline uint32x4_t lerp4(uint8x16_t va, uint8x16_t vb, uint8x8_t wlo, uint8x8_t whi) {
  const uint8x8_t k128 = vdup_n_u8(128);
  uint16x8_t tlo = vshll_n_u8(vget_low_u8(va), 8), thi = vshll_n_u8(vget_high_u8(va), 8);
  tlo = vmlal_u8(tlo, vget_low_u8(vb), wlo);  thi = vmlal_u8(thi, vget_high_u8(vb), whi);
  tlo = vmlsl_u8(tlo, vget_low_u8(va), wlo);  thi = vmlsl_u8(thi, vget_high_u8(va), whi);
  tlo = vaddw_u8(tlo, k128);                  thi = vaddw_u8(thi, k128);
  return vreinterpretq_u32_u8(vcombine_u8(vshrn_n_u16(tlo, 8), vshrn_n_u16(thi, 8)));
}

static inline void spread_w4(uint8x8_t w4, uint8x8_t& wlo, uint8x8_t& whi) {
  const uint8x8x2_t z1 = vzip_u8(w4, w4);
  const uint8x8x2_t z2 = vzip_u8(z1.val[0], z1.val[0]);
  wlo = z2.val[0]; whi = z2.val[1];
}

// Four two-pixel loads (src[s], src[s+1]), zipped so left pixels land in one register, right in another.
void lerp_row_gather(const u32* src, const u16* sx, const u8* wx, u32 n, u32* out) {
  u32 x = 0;
  for (; x + 4 <= n; x += 4) {
    const uint32x2_t p0 = vld1_u32(src + sx[x]),     p1 = vld1_u32(src + sx[x + 1]);
    const uint32x2_t p2 = vld1_u32(src + sx[x + 2]), p3 = vld1_u32(src + sx[x + 3]);
    const uint32x2x2_t z01 = vzip_u32(p0, p1), z23 = vzip_u32(p2, p3);
    const uint8x16_t va = vreinterpretq_u8_u32(vcombine_u32(z01.val[0], z23.val[0]));
    const uint8x16_t vb = vreinterpretq_u8_u32(vcombine_u32(z01.val[1], z23.val[1]));
    u32 w4u; std::memcpy(&w4u, wx + x, 4);
    uint8x8_t wlo, whi;
    spread_w4(vreinterpret_u8_u32(vdup_n_u32(w4u)), wlo, whi);
    vst1q_u32(out + x, lerp4(va, vb, wlo, whi));
  }
  for (; x < n; ++x) {
    const u32 a = src[sx[x]], b = src[sx[x] + 1], f = wx[x];
    u32 r = 0;
    for (u32 sh = 0; sh < 32; sh += 8) {
      const u32 xa = (a >> sh) & 255, ya = (b >> sh) & 255;
      r |= ((xa * (256 - f) + ya * f + 128) >> 8) << sh;
    }
    out[x] = r;
  }
}

void lerp_rows(const u32* a, const u32* b, u32 w, u32 n, u32* out) {
  const uint8x8_t wlo = vdup_n_u8(static_cast<u8>(w)), whi = wlo;
  u32 i = 0;
  for (; i + 4 <= n; i += 4)
    vst1q_u32(out + i, lerp4(vreinterpretq_u8_u32(vld1q_u32(a + i)), vreinterpretq_u8_u32(vld1q_u32(b + i)), wlo, whi));
  for (; i < n; ++i) {
    u32 r = 0;
    for (u32 sh = 0; sh < 32; sh += 8) {
      const u32 xa = (a[i] >> sh) & 255, ya = (b[i] >> sh) & 255;
      r |= ((xa * (256 - w) + ya * w + 128) >> 8) << sh;
    }
    out[i] = r;
  }
}

// The dimmed copies of the 256 source pixels are made first, four at a time
// (bytes widened to 16 bits, multiplied by f with alpha's lane at 256, and
// narrowed back), then the runs are filled as scale_row does. Every
// destination pixel is stored exactly once: the destination is usually a
// write-combined dmabuf, where a scalar store landing on top of a vector
// store's last lane costs a second bus write. Runs of two and three (every
// scale the handhelds use) are one or two stores each.
void scale_row_grid(const u32* src, const u16* xrun, u32 f, u32 min_run, u32 pitch, bool seam_row, u32* dst) {
  if (min_run < 2) min_run = 2;
  if (pitch < 1) pitch = 1;
  alignas(16) u32 dimmed[256];
  // f == 0 (black seams: the "integer scale, leave the spare pixels dark"
  // look) needs no dimmed copies at all: every seam is the one constant.
  if (f == 0) {
    if (seam_row) {
      const u32 w = xrun[256];
      const uint32x4_t k = vdupq_n_u32(0xFF000000u);
      u32 x = 0;
      for (; x + 4 <= w; x += 4) vst1q_u32(dst + x, k);
      for (; x < w; ++x) dst[x] = 0xFF000000u;
      return;
    }
    for (u32& d : dimmed) d = 0xFF000000u;
  } else {
    const uint16x8_t fv = {static_cast<u16>(f), static_cast<u16>(f), static_cast<u16>(f), 256,
                           static_cast<u16>(f), static_cast<u16>(f), static_cast<u16>(f), 256};
    for (u32 s = 0; s < 256; s += 4) {
      const uint8x16_t c = vreinterpretq_u8_u32(vld1q_u32(src + s));
      const uint16x8_t lo = vmulq_u16(vmovl_u8(vget_low_u8(c)), fv), hi = vmulq_u16(vmovl_u8(vget_high_u8(c)), fv);
      vst1q_u32(dimmed + s, vreinterpretq_u32_u8(vcombine_u8(vshrn_n_u16(lo, 8), vshrn_n_u16(hi, 8))));
    }
    if (seam_row) { scale_row(dimmed, xrun, dst); return; }
  }
  for (u32 s = 0; s < 256; ++s) {
    const u32 c = src[s], cd = dimmed[s];
    u32 x = xrun[s];
    const u32 end = xrun[s + 1];
    const u32 n = end - x;
    if (n < min_run || s % pitch != 0) {                 // plain run, as scale_row
      const uint32x4_t v = vdupq_n_u32(c);
      for (; x + 4 <= end; x += 4) vst1q_u32(dst + x, v);
      for (; x < end; ++x) dst[x] = c;
      continue;
    }
    switch (n) {
    case 2: vst1_u32(dst + x, uint32x2_t{cd, c}); break;
    case 3: dst[x] = cd; vst1_u32(dst + x + 1, vdup_n_u32(c)); break;
    default: {
      dst[x++] = cd;
      const uint32x4_t v = vdupq_n_u32(c);
      for (; x + 4 <= end; x += 4) vst1q_u32(dst + x, v);
      for (; x < end; ++x) dst[x] = c;
    }
    }
  }
}


// ---- 3D span stages ----
// Four pixels per step (buffers are 256 wide, rounding n up is safe). A
// correctly-rounded f64 quotient of two u32s, truncated, is the exact integer
// quotient, so the f64 divide reproduces integer division with no fix-up.

namespace {
using compat::udiv_exact;   // f64 quotient on A64, scalar VFP lanes on ARMv7
inline int32x4_t mul_hi8_add(int32x4_t base, uint32x4_t d, uint32x4_t f) {   // base + ((d * f) >> 8), 64-bit product, low 32 bits kept
  const uint64x2_t lo = vshrq_n_u64(vmull_u32(vget_low_u32(d), vget_low_u32(f)), 8);
  const uint64x2_t hi = vshrq_n_u64(compat::mull_high_u32(d, f), 8);
  return vaddq_s32(base, vreinterpretq_s32_u32(vcombine_u32(vmovn_u64(lo), vmovn_u64(hi))));
}
const int32x4_t kLane = {0, 1, 2, 3};

// base + ((d*f)>>8): same result in 32 or 64 bits when d*f < 2^32; the 32-bit
// form is one instruction, the 64-bit one six on ARMv7.
[[gnu::always_inline]] inline int32x4_t mul8_add(int32x4_t base, uint32x4_t d, uint32x4_t f, bool narrow) {
  if (narrow) return vaddq_s32(base, vreinterpretq_s32_u32(vshrq_n_u32(vmulq_u32(d, f), 8)));
  return mul_hi8_add(base, d, f);
}

#if defined(__arm__)
// Largest factor in fac[0, n) only; entries past n are slack the kernels
// overwrite and may hold a prior span's data.
inline u32 fac_max(const u32* fac, u32 n) {
  uint32x4_t acc = vdupq_n_u32(0);
  u32 i = 0;
  for (; i + 4 <= n; i += 4) acc = vmaxq_u32(acc, vld1q_u32(fac + i));
  if (i < n) acc = vmaxq_u32(acc, vandq_u32(vld1q_u32(fac + i), vcltq_u32(vreinterpretq_u32_s32(kLane), vdupq_n_u32(n - i))));
  return compat::maxv_u32(acc);
}
#endif

// Whether every product the attribute kernels form over this span fits 32
// bits: factor <= 256 (above only from a wrapped perspective numerator) and
// endpoint difference < 2^24 keeps d*f < 2^32. ARMv7 only: AArch64's 64-bit
// form is already one instruction and never scans.
#if defined(__arm__)
inline bool narrow_ok(u32 fmax, u32 dmax) { return fmax <= 256 && dmax < (1u << 24); }
// The caller's bound when given, else the scan (skipped when nothing can narrow).
inline u32 fac_bound_of(const u32* fac, u32 n, u32 fmax, u32 dmax) {
  if (fmax <= 256 || dmax >= (1u << 24)) return fmax;
  return fac_max(fac, n);
}
#endif
}

void span_factor(s32 xv0, u32 n, s32 xdiff, s32 w0n, s32 w0d, s32 w1d, u32* fac) {
  // num/den are linear in xv, so they step by a constant. Lanes the Newton
  // estimate can't prove exact are collected and the span redone exactly once (not per-pixel).
  const int32x4_t x0 = vaddq_s32(vdupq_n_s32(xv0), kLane);
  uint32x4_t num = vshlq_n_u32(vreinterpretq_u32_s32(vmulq_s32(x0, vdupq_n_s32(w0n))), 8);
  const uint32x4_t dnum = vdupq_n_u32(static_cast<u32>(w0n * 4) << 8);
  const uint32x4_t dden = vdupq_n_u32(static_cast<u32>(w0d * 4) - static_cast<u32>(w1d * 4));
  const uint32x4_t one = vdupq_n_u32(1), big = vdupq_n_u32(0x3FFFFF);
  uint32x4_t bad = vdupq_n_u32(0);
  uint32x4_t den;
  if (w0d == w1d) {
    // W constant: denominator collapses to xdiff*w0d, factor is linear in x
    // -- a step ramp, no division. Step held in 16.16, so the ramp can drift
    // a couple units from exact over a long span (deliberate).
    const u32 dscalar = static_cast<u32>(xdiff) * static_cast<u32>(w0d);
    if (dscalar == 0) {
      const uint32x4_t z = vdupq_n_u32(0);
      for (u32 i = 0; i < n; i += 4) vst1q_u32(fac + i, z);
      return;
    }
    const u32 A = static_cast<u32>(w0n) << 8;
    const u32 step = static_cast<u32>(div_u64(static_cast<u64>(A) << 16, dscalar));
    uint32x4_t acc = vmulq_u32(vreinterpretq_u32_s32(vaddq_s32(vdupq_n_s32(xv0), kLane)), vdupq_n_u32(step));
    const uint32x4_t inc = vdupq_n_u32(step * 4);
    for (u32 i = 0; i < n; i += 4) {
      vst1q_u32(fac + i, vshrq_n_u32(acc, 16));
      acc = vaddq_u32(acc, inc);
    }
    return;
  } else {
    den = vreinterpretq_u32_s32(vaddq_s32(vmulq_s32(x0, vdupq_n_s32(w0d)),
                                          vmulq_s32(vsubq_s32(vdupq_n_s32(xdiff), x0), vdupq_n_s32(w1d))));
    for (u32 i = 0; i < n; i += 4) {
      const uint32x4_t zero = compat::ceqz_u32(den);
      const uint32x4_t d = vorrq_u32(den, vandq_u32(zero, one));
      const float32x4_t fd = vcvtq_f32_u32(d);
      float32x4_t rcp = vrecpeq_f32(fd);
      rcp = vmulq_f32(rcp, vrecpsq_f32(fd, rcp));                       // ~16 bits, enough for an 8-bit quotient
      uint32x4_t q = vcvtq_u32_f32(vmulq_f32(vcvtq_f32_u32(num), rcp));
      const uint32x4_t unsafe = vorrq_u32(vcgeq_u32(q, big), vcltq_u32(vaddq_u32(num, d), num));
      uint32x4_t r = vsubq_u32(num, vmulq_u32(q, d));                   // wraps negative when q is one too many
      const uint32x4_t over = vcgtq_u32(r, num);
      q = vaddq_u32(q, over);                                           // -1 on those lanes
      r = vaddq_u32(r, vandq_u32(over, d));
      const uint32x4_t under = vcgeq_u32(r, d);
      q = vsubq_u32(q, under);                                          // +1 on those lanes
      r = vsubq_u32(r, vandq_u32(under, d));
      bad = vorrq_u32(bad, vorrq_u32(unsafe, vorrq_u32(vcgeq_u32(r, d), vcgtq_u32(r, num))));
      vst1q_u32(fac + i, vbicq_u32(q, zero));
      num = vaddq_u32(num, dnum);
      den = vaddq_u32(den, dden);
    }
  }
  if (compat::maxv_u32(bad) == 0) return;
  // Anything the fast path could not prove exact: redo the span by division.
  num = vshlq_n_u32(vreinterpretq_u32_s32(vmulq_s32(x0, vdupq_n_s32(w0n))), 8);
  den = vreinterpretq_u32_s32(vaddq_s32(vmulq_s32(x0, vdupq_n_s32(w0d)),
                                        vmulq_s32(vsubq_s32(vdupq_n_s32(xdiff), x0), vdupq_n_s32(w1d))));
  for (u32 i = 0; i < n; i += 4) {
    const uint32x4_t zero = compat::ceqz_u32(den);
    const uint32x4_t d = vorrq_u32(den, vandq_u32(zero, one));
    vst1q_u32(fac + i, vbicq_u32(udiv_exact(num, d), zero));
    num = vaddq_u32(num, dnum);
    den = vaddq_u32(den, dden);
  }
}

#if defined(__arm__)
void span_attr_persp(s32 y0, s32 y1, const u32* fac, u32 n, s32* out, u32 fmax) {
  if (y0 == y1) { const int32x4_t v = vdupq_n_s32(y0); for (u32 i = 0; i < n; i += 4) vst1q_s32(out + i, v); return; }
  const bool up = y0 < y1;
  const u32 dd = static_cast<u32>(up ? y1 - y0 : y0 - y1);
  const bool narrow = narrow_ok(fac_bound_of(fac, n, fmax, dd), dd);
  const int32x4_t base = vdupq_n_s32(up ? y0 : y1);
  const uint32x4_t d = vdupq_n_u32(dd);
  const uint32x4_t k256 = vdupq_n_u32(256);
  for (u32 i = 0; i < n; i += 4) {
    uint32x4_t f = vld1q_u32(fac + i);
    if (!up) f = vsubq_u32(k256, f);
    vst1q_s32(out + i, mul8_add(base, d, f, narrow));
  }
}

namespace {
// Endpoint constants for attributes [k0, k1): base, |y1-y0|, direction, flatness, max diff (for narrow_ok).
struct AttrSet {
  int32x4_t base[5]; uint32x4_t d[5]; bool up[5], flat[5];
  u32 dmax = 0;
  AttrSet(const s32* y0, const s32* y1, int k0, int k1) {
    for (int k = k0; k < k1; ++k) {
      flat[k] = y0[k] == y1[k];
      up[k] = y0[k] < y1[k];
      base[k] = vdupq_n_s32(flat[k] ? y0[k] : (up[k] ? y0[k] : y1[k]));
      const u32 dk = static_cast<u32>(up[k] ? y1[k] - y0[k] : y0[k] - y1[k]);
      d[k] = vdupq_n_u32(dk);
      if (!flat[k] && dk > dmax) dmax = dk;
    }
  }
};

// fac loaded once per four pixels instead of once per attribute; endpoint constants stay in registers.
template <bool narrow>
void span_attrs5_body(const AttrSet& a, const u32* fac, u32 n, s32* const* out) {
  const uint32x4_t k256 = vdupq_n_u32(256);
  for (u32 i = 0; i < n; i += 4) {
    const uint32x4_t f = vld1q_u32(fac + i);
    const uint32x4_t fi = vsubq_u32(k256, f);
    for (int k = 0; k < 5; ++k) {
      if (a.flat[k]) vst1q_s32(out[k] + i, a.base[k]);
      else vst1q_s32(out[k] + i, mul8_add(a.base[k], a.d[k], a.up[k] ? f : fi, narrow));
    }
  }
}

// Narrowed on store: colour to 6-bit channel, texcoords to s16. s and t only (colour chains skipped).
template <bool narrow>
void span_attrs2n_body(const AttrSet& a, const u32* fac, u32 n, s16* sc, s16* tc) {
  const uint32x4_t k256 = vdupq_n_u32(256);
  s16* const tout[2] = {sc, tc};
  for (u32 i = 0; i < n; i += 8) {
    const uint32x4_t f0 = vld1q_u32(fac + i), f1 = vld1q_u32(fac + i + 4);
    const uint32x4_t i0 = vsubq_u32(k256, f0), i1 = vsubq_u32(k256, f1);
    for (int k = 3; k < 5; ++k) {
      int32x4_t v0, v1;
      if (a.flat[k]) { v0 = a.base[k]; v1 = a.base[k]; }
      else { v0 = mul8_add(a.base[k], a.d[k], a.up[k] ? f0 : i0, narrow); v1 = mul8_add(a.base[k], a.d[k], a.up[k] ? f1 : i1, narrow); }
      vst1q_s16(tout[k - 3] + i, vreinterpretq_s16_u16(vcombine_u16(vmovn_u32(vreinterpretq_u32_s32(v0)),
                                                                   vmovn_u32(vreinterpretq_u32_s32(v1)))));
    }
  }
}

template <bool narrow>
void span_attrs5n_body(const AttrSet& a, const u32* fac, u32 n, u8* vr, u8* vg, u8* vb, s16* sc, s16* tc) {
  const uint32x4_t k256 = vdupq_n_u32(256);
  u8* const cout[3] = {vr, vg, vb};
  s16* const tout[2] = {sc, tc};
  for (u32 i = 0; i < n; i += 8) {
    const uint32x4_t f0 = vld1q_u32(fac + i), f1 = vld1q_u32(fac + i + 4);
    const uint32x4_t i0 = vsubq_u32(k256, f0), i1 = vsubq_u32(k256, f1);
    for (int k = 0; k < 5; ++k) {
      int32x4_t v0, v1;
      if (a.flat[k]) { v0 = a.base[k]; v1 = a.base[k]; }
      else { v0 = mul8_add(a.base[k], a.d[k], a.up[k] ? f0 : i0, narrow); v1 = mul8_add(a.base[k], a.d[k], a.up[k] ? f1 : i1, narrow); }
      if (k < 3) {   // (v >> 3) & 0xFF: the two narrowing moves mask it
        const uint16x8_t w = vcombine_u16(vmovn_u32(vshrq_n_u32(vreinterpretq_u32_s32(v0), 3)),
                                          vmovn_u32(vshrq_n_u32(vreinterpretq_u32_s32(v1), 3)));
        vst1_u8(cout[k] + i, vmovn_u16(w));
      } else {       // (s16)v
        vst1q_s16(tout[k - 3] + i, vreinterpretq_s16_u16(vcombine_u16(vmovn_u32(vreinterpretq_u32_s32(v0)),
                                                                     vmovn_u32(vreinterpretq_u32_s32(v1)))));
      }
    }
  }
}
} // namespace

void span_attrs5(const s32* y0, const s32* y1, const u32* fac, u32 n, s32* const* out, u32 fmax) {
  const AttrSet a(y0, y1, 0, 5);
  if (a.dmax == 0 || narrow_ok(fac_bound_of(fac, n, fmax, a.dmax), a.dmax)) span_attrs5_body<true>(a, fac, n, out);
  else span_attrs5_body<false>(a, fac, n, out);
}

void span_attrs2n(const s32* y0, const s32* y1, const u32* fac, u32 n, s16* sc, s16* tc, u32 fmax) {
  const AttrSet a(y0, y1, 3, 5);
  if (a.dmax == 0 || narrow_ok(fac_bound_of(fac, n, fmax, a.dmax), a.dmax)) span_attrs2n_body<true>(a, fac, n, sc, tc);
  else span_attrs2n_body<false>(a, fac, n, sc, tc);
}

void span_attrs5n(const s32* y0, const s32* y1, const u32* fac, u32 n, u8* vr, u8* vg, u8* vb, s16* sc, s16* tc, u32 fmax) {
  const AttrSet a(y0, y1, 0, 5);
  if (a.dmax == 0 || narrow_ok(fac_bound_of(fac, n, fmax, a.dmax), a.dmax)) span_attrs5n_body<true>(a, fac, n, vr, vg, vb, sc, tc);
  else span_attrs5n_body<false>(a, fac, n, vr, vg, vb, sc, tc);
}
#else
// AArch64: pre-narrow-path form (fmax unused).
void span_attr_persp(s32 y0, s32 y1, const u32* fac, u32 n, s32* out, u32) {
  if (y0 == y1) { const int32x4_t v = vdupq_n_s32(y0); for (u32 i = 0; i < n; i += 4) vst1q_s32(out + i, v); return; }
  const bool up = y0 < y1;
  const int32x4_t base = vdupq_n_s32(up ? y0 : y1);
  const uint32x4_t d = vdupq_n_u32(static_cast<u32>(up ? y1 - y0 : y0 - y1));
  const uint32x4_t k256 = vdupq_n_u32(256);
  for (u32 i = 0; i < n; i += 4) {
    uint32x4_t f = vld1q_u32(fac + i);
    if (!up) f = vsubq_u32(k256, f);
    vst1q_s32(out + i, mul_hi8_add(base, d, f));
  }
}

// The five attributes in one pass over the span: `fac` is loaded once per
// four pixels instead of once per attribute, and the endpoint constants for
// all five stay in registers.
void span_attrs5(const s32* y0, const s32* y1, const u32* fac, u32 n, s32* const* out, u32) {
  const uint32x4_t k256 = vdupq_n_u32(256);
  int32x4_t base[5]; uint32x4_t d[5]; bool up[5], flat[5];
  for (int k = 0; k < 5; ++k) {
    flat[k] = y0[k] == y1[k];
    up[k] = y0[k] < y1[k];
    base[k] = vdupq_n_s32(flat[k] ? y0[k] : (up[k] ? y0[k] : y1[k]));
    d[k] = vdupq_n_u32(static_cast<u32>(up[k] ? y1[k] - y0[k] : y0[k] - y1[k]));
  }
  for (u32 i = 0; i < n; i += 4) {
    const uint32x4_t f = vld1q_u32(fac + i);
    const uint32x4_t fi = vsubq_u32(k256, f);
    for (int k = 0; k < 5; ++k) {
      if (flat[k]) vst1q_s32(out[k] + i, base[k]);
      else vst1q_s32(out[k] + i, mul_hi8_add(base[k], d[k], up[k] ? f : fi));
    }
  }
}

// Narrowed on store: colour to 6-bit channel, texcoords to s16. s and t only.
void span_attrs2n(const s32* y0, const s32* y1, const u32* fac, u32 n, s16* sc, s16* tc, u32) {
  const uint32x4_t k256 = vdupq_n_u32(256);
  int32x4_t base[2]; uint32x4_t d[2]; bool up[2], flat[2];
  for (int k = 0; k < 2; ++k) {
    const s32 a = y0[k + 3], b = y1[k + 3];
    flat[k] = a == b;
    up[k] = a < b;
    base[k] = vdupq_n_s32(flat[k] ? a : (up[k] ? a : b));
    d[k] = vdupq_n_u32(static_cast<u32>(up[k] ? b - a : a - b));
  }
  s16* const tout[2] = {sc, tc};
  for (u32 i = 0; i < n; i += 8) {
    const uint32x4_t f0 = vld1q_u32(fac + i), f1 = vld1q_u32(fac + i + 4);
    const uint32x4_t i0 = vsubq_u32(k256, f0), i1 = vsubq_u32(k256, f1);
    for (int k = 0; k < 2; ++k) {
      int32x4_t v0, v1;
      if (flat[k]) { v0 = base[k]; v1 = base[k]; }
      else { v0 = mul_hi8_add(base[k], d[k], up[k] ? f0 : i0); v1 = mul_hi8_add(base[k], d[k], up[k] ? f1 : i1); }
      vst1q_s16(tout[k] + i, vreinterpretq_s16_u16(vcombine_u16(vmovn_u32(vreinterpretq_u32_s32(v0)),
                                                                vmovn_u32(vreinterpretq_u32_s32(v1)))));
    }
  }
}

void span_attrs5n(const s32* y0, const s32* y1, const u32* fac, u32 n, u8* vr, u8* vg, u8* vb, s16* sc, s16* tc, u32) {
  const uint32x4_t k256 = vdupq_n_u32(256);
  int32x4_t base[5]; uint32x4_t d[5]; bool up[5], flat[5];
  for (int k = 0; k < 5; ++k) {
    flat[k] = y0[k] == y1[k];
    up[k] = y0[k] < y1[k];
    base[k] = vdupq_n_s32(flat[k] ? y0[k] : (up[k] ? y0[k] : y1[k]));
    d[k] = vdupq_n_u32(static_cast<u32>(up[k] ? y1[k] - y0[k] : y0[k] - y1[k]));
  }
  u8* const cout[3] = {vr, vg, vb};
  s16* const tout[2] = {sc, tc};
  for (u32 i = 0; i < n; i += 8) {
    const uint32x4_t f0 = vld1q_u32(fac + i), f1 = vld1q_u32(fac + i + 4);
    const uint32x4_t i0 = vsubq_u32(k256, f0), i1 = vsubq_u32(k256, f1);
    for (int k = 0; k < 5; ++k) {
      int32x4_t v0, v1;
      if (flat[k]) { v0 = base[k]; v1 = base[k]; }
      else { v0 = mul_hi8_add(base[k], d[k], up[k] ? f0 : i0); v1 = mul_hi8_add(base[k], d[k], up[k] ? f1 : i1); }
      if (k < 3) {   // (v >> 3) & 0xFF: the two narrowing moves mask it
        const uint16x8_t w = vcombine_u16(vmovn_u32(vshrq_n_u32(vreinterpretq_u32_s32(v0), 3)),
                                          vmovn_u32(vshrq_n_u32(vreinterpretq_u32_s32(v1), 3)));
        vst1_u8(cout[k] + i, vmovn_u16(w));
      } else {       // (s16)v
        vst1q_s16(tout[k - 3] + i, vreinterpretq_s16_u16(vcombine_u16(vmovn_u32(vreinterpretq_u32_s32(v0)),
                                                                     vmovn_u32(vreinterpretq_u32_s32(v1)))));
      }
    }
  }
}
#endif

// Vector form of span_attr_linear's body: q = d*xv/xdiff by reciprocal, same
// one-compare fix-up, xv mirrored when endpoints descend.
struct LinAttr {
  int32x4_t base;
  uint32x4_t d;
  bool flat, up;
  void set(s32 y0, s32 y1) {
    flat = y0 == y1;
    up = y0 < y1;
    base = vdupq_n_s32(flat ? y0 : (up ? y0 : y1));
    d = vdupq_n_u32(static_cast<u32>(up ? y1 - y0 : y0 - y1));
  }
  [[gnu::always_inline]] int32x4_t at(int32x4_t xv, int32x4_t vxdiff, uint32x4_t vm, uint32x4_t vxd, bool has_m) const {
    if (flat) return base;
    const int32x4_t x = up ? xv : vsubq_s32(vxdiff, xv);
    const uint32x4_t num = vmulq_u32(d, vreinterpretq_u32_s32(x));
    uint32x4_t q = num;
    if (has_m) {
      q = vcombine_u32(vshrn_n_u64(vmull_u32(vget_low_u32(num), vget_low_u32(vm)), 32), vshrn_n_u64(compat::mull_high_u32(num, vm), 32));
      q = vaddq_u32(q, vcgtq_u32(vmulq_u32(q, vxd), num));   // all-ones lane = -1
    }
    return vaddq_s32(base, vreinterpretq_s32_u32(q));
  }
};

// Hoisted: fused kernels need this once for all five attributes, not once
// each (each is a 64-bit division this kernel exists to avoid repeating).
static inline u32 lin_recip(s32 xdiff) {
  return xdiff >= 2 ? recip_ceil32(static_cast<u32>(xdiff)) : 0;
}

void span_attrs5n_lin(const s32* y0, const s32* y1, s32 xv0, u32 n, s32 xdiff, u8* vr, u8* vg, u8* vb, s16* sc, s16* tc) {
  LinAttr a[5];
  for (int k = 0; k < 5; ++k) a[k].set(y0[k], y1[k]);
  const int32x4_t vxdiff = vdupq_n_s32(xdiff);
  const u32 m = lin_recip(xdiff);
  const uint32x4_t vm = vdupq_n_u32(m), vxd = vreinterpretq_u32_s32(vxdiff);
  u8* const cout[3] = {vr, vg, vb};
  s16* const tout[2] = {sc, tc};
  for (u32 i = 0; i < n; i += 8) {
    const int32x4_t xv0v = vaddq_s32(vdupq_n_s32(xv0 + static_cast<s32>(i)), kLane);
    const int32x4_t xv1v = vaddq_s32(xv0v, vdupq_n_s32(4));
    for (int k = 0; k < 5; ++k) {
      int32x4_t v0, v1;
      if (m) { v0 = a[k].at(xv0v, vxdiff, vm, vxd, true);  v1 = a[k].at(xv1v, vxdiff, vm, vxd, true); }
      else   { v0 = a[k].at(xv0v, vxdiff, vm, vxd, false); v1 = a[k].at(xv1v, vxdiff, vm, vxd, false); }
      if (k < 3) {   // (v >> 3) & 0xFF: the two narrowing moves mask it
        const uint16x8_t w = vcombine_u16(vmovn_u32(vshrq_n_u32(vreinterpretq_u32_s32(v0), 3)),
                                          vmovn_u32(vshrq_n_u32(vreinterpretq_u32_s32(v1), 3)));
        vst1_u8(cout[k] + i, vmovn_u16(w));
      } else {       // (s16)v
        vst1q_s16(tout[k - 3] + i, vreinterpretq_s16_u16(vcombine_u16(vmovn_u32(vreinterpretq_u32_s32(v0)),
                                                                     vmovn_u32(vreinterpretq_u32_s32(v1)))));
      }
    }
  }
}

void span_attrs2n_lin(const s32* y0, const s32* y1, s32 xv0, u32 n, s32 xdiff, s16* sc, s16* tc) {
  LinAttr a[2];
  for (int k = 0; k < 2; ++k) a[k].set(y0[k + 3], y1[k + 3]);
  const int32x4_t vxdiff = vdupq_n_s32(xdiff);
  const u32 m = lin_recip(xdiff);
  const uint32x4_t vm = vdupq_n_u32(m), vxd = vreinterpretq_u32_s32(vxdiff);
  s16* const tout[2] = {sc, tc};
  for (u32 i = 0; i < n; i += 8) {
    const int32x4_t xv0v = vaddq_s32(vdupq_n_s32(xv0 + static_cast<s32>(i)), kLane);
    const int32x4_t xv1v = vaddq_s32(xv0v, vdupq_n_s32(4));
    for (int k = 0; k < 2; ++k) {
      int32x4_t v0, v1;
      if (m) { v0 = a[k].at(xv0v, vxdiff, vm, vxd, true);  v1 = a[k].at(xv1v, vxdiff, vm, vxd, true); }
      else   { v0 = a[k].at(xv0v, vxdiff, vm, vxd, false); v1 = a[k].at(xv1v, vxdiff, vm, vxd, false); }
      vst1q_s16(tout[k] + i, vreinterpretq_s16_u16(vcombine_u16(vmovn_u32(vreinterpretq_u32_s32(v0)),
                                                                vmovn_u32(vreinterpretq_u32_s32(v1)))));
    }
  }
}

void span_attr_linear(s32 y0, s32 y1, s32 xv0, u32 n, s32 xdiff, s32* out) {
  if (y0 == y1) { const int32x4_t v = vdupq_n_s32(y0); for (u32 i = 0; i < n; i += 4) vst1q_s32(out + i, v); return; }
  const bool up = y0 < y1;
  const int32x4_t base = vdupq_n_s32(up ? y0 : y1);
  const uint32x4_t d = vdupq_n_u32(static_cast<u32>(up ? y1 - y0 : y0 - y1));
  const int32x4_t vxdiff = vdupq_n_s32(xdiff);
  // q = (n * ceil(2^32/xdiff)) >> 32 is exact or one too many; q*xdiff > n fixes it.
  const u32 m = lin_recip(xdiff);
  const uint32x4_t vm = vdupq_n_u32(m), vxd = vreinterpretq_u32_s32(vxdiff);
  for (u32 i = 0; i < n; i += 4) {
    int32x4_t xv = vaddq_s32(vdupq_n_s32(xv0 + static_cast<s32>(i)), kLane);
    if (!up) xv = vsubq_s32(vxdiff, xv);
    const uint32x4_t num = vmulq_u32(d, vreinterpretq_u32_s32(xv));
    uint32x4_t q = num;
    if (m) {
      q = vcombine_u32(vshrn_n_u64(vmull_u32(vget_low_u32(num), vget_low_u32(vm)), 32), vshrn_n_u64(compat::mull_high_u32(num, vm), 32));
      q = vaddq_u32(q, vcgtq_u32(vmulq_u32(q, vxd), num));   // all-ones lane = -1
    }
    vst1q_s32(out + i, vaddq_s32(base, vreinterpretq_s32_u32(q)));
  }
}

void span_z_linear(s32 z0, s32 z1, s32 xv0, u32 n, s32 xdiff, s32 xrecip, s32* out) {
  if (z0 == z1) { const int32x4_t v = vdupq_n_s32(z0); for (u32 i = 0; i < n; i += 4) vst1q_s32(out + i, v); return; }
  const bool up = z0 < z1;
  const int32x4_t base = vdupq_n_s32(up ? z0 : z1);
  const uint32x4_t disp = vdupq_n_u32(static_cast<u32>((up ? z1 - z0 : z0 - z1) >> 9));
  const uint32x4_t vrecip = vdupq_n_u32(static_cast<u32>(xrecip));
  const int32x4_t vxdiff = vdupq_n_s32(xdiff);
  for (u32 i = 0; i < n; i += 4) {
    int32x4_t xv = vaddq_s32(vdupq_n_s32(xv0 + static_cast<s32>(i)), kLane);
    if (!up) xv = vsubq_s32(vxdiff, xv);
    const uint32x4_t df = vmulq_u32(disp, vreinterpretq_u32_s32(xv));          // disp * factor < 2^24
    const uint64x2_t lo = vshrq_n_u64(vmull_u32(vget_low_u32(df), vget_low_u32(vrecip)), 13);
    const uint64x2_t hi = vshrq_n_u64(compat::mull_high_u32(df, vrecip), 13);
    vst1q_s32(out + i, vaddq_s32(base, vreinterpretq_s32_u32(vcombine_u32(vmovn_u64(lo), vmovn_u64(hi)))));
  }
}

void span_z_const(s32 z, u32 n, s32* out) {
  const int32x4_t v = vdupq_n_s32(z);
  for (u32 i = 0; i < n; i += 4) vst1q_s32(out + i, v);
}

void clear_image_run(const u16* col, const u16* dep, u32 n, u32 polyid, u32* color, u32* depth, u32* attr) {
  // 16 pixels a step as byte planes; 5-bit channel to 6 bits as c*2+(c!=0). Exactly n written, tail scalar.
  const uint16x8_t m5 = vdupq_n_u16(0x1F), one16 = vdupq_n_u16(1), bit15 = vdupq_n_u16(0x8000);
  const uint32x4_t vpid = vdupq_n_u32(polyid), v1ff = vdupq_n_u32(0x1FF);
  auto c6 = [&](uint16x8_t c5) { return vaddq_u16(vshlq_n_u16(c5, 1), vandq_u16(vtstq_u16(c5, c5), one16)); };
  auto plane = [&](uint16x8_t lo, uint16x8_t hi) { return vcombine_u8(vmovn_u16(lo), vmovn_u16(hi)); };
  u32 i = 0;
  for (; i + 16 <= n; i += 16) {
    const uint16x8_t c0 = vld1q_u16(col + i), c1 = vld1q_u16(col + i + 8);
    uint8x16x4_t o;
    o.val[0] = plane(c6(vandq_u16(c0, m5)), c6(vandq_u16(c1, m5)));
    o.val[1] = plane(c6(vandq_u16(vshrq_n_u16(c0, 5), m5)), c6(vandq_u16(vshrq_n_u16(c1, 5), m5)));
    o.val[2] = plane(c6(vandq_u16(vshrq_n_u16(c0, 10), m5)), c6(vandq_u16(vshrq_n_u16(c1, 10), m5)));
    o.val[3] = plane(vandq_u16(vtstq_u16(c0, bit15), m5), vandq_u16(vtstq_u16(c1, bit15), m5));
    vst4q_u8(reinterpret_cast<u8*>(color + i), o);
    for (u32 k = 0; k < 2; ++k) {
      const uint16x8_t d = vld1q_u16(dep + i + k * 8);
      const uint16x8_t dz = vbicq_u16(d, bit15), da = vandq_u16(d, bit15);
      vst1q_u32(depth + i + k * 8, vaddq_u32(vshll_n_u16(vget_low_u16(dz), 9), v1ff));
      vst1q_u32(depth + i + k * 8 + 4, vaddq_u32(compat::shll_high_n_u16<9>(dz), v1ff));
      vst1q_u32(attr + i + k * 8, vorrq_u32(vmovl_u16(vget_low_u16(da)), vpid));
      vst1q_u32(attr + i + k * 8 + 4, vorrq_u32(compat::movl_high_u16(da), vpid));
    }
  }
  for (; i < n; ++i) {
    const u32 c = col[i], d = dep[i];
    auto s6 = [](u32 c5) { return c5 ? c5 * 2 + 1 : 0; };
    color[i] = s6(c & 0x1F) | (s6((c >> 5) & 0x1F) << 8) | (s6((c >> 10) & 0x1F) << 16) | ((c & 0x8000) ? 0x1F000000u : 0);
    depth[i] = ((d & 0x7FFF) * 0x200) + 0x1FF;
    attr[i] = polyid | (d & 0x8000);
  }
}

// One instantiation per depth mode: test decided once per span, not per four
// pixels. `under` marks the lower layer a candidate when the top pixel carries
// edge flags and the lower pixel passes the same test, so a span occluded by
// both layers costs its depth stage alone (the resolve re-tests the lower
// pixel; this only narrows what reaches it).
template <int mode, bool under>
static u32 depth_candidates_m(const s32* z, const u32* dstz, const u32* dstattr, u32 n, u8* pass, u32 under_off) {
  const uint32x4_t one = vdupq_n_u32(1), two = vdupq_n_u32(2);
  uint32x4_t any = vdupq_n_u32(0);
  auto test = [](int32x4_t zv, int32x4_t d, uint32x4_t a) -> uint32x4_t {
    if constexpr (mode == 0) { (void)a; return vcltq_s32(zv, d); }
    else if constexpr (mode == 1) {
      const uint32x4_t back = vceqq_u32(vandq_u32(a, vdupq_n_u32(0x00400010)), vdupq_n_u32(0x10));
      return vbslq_u32(back, vcleq_s32(zv, d), vcltq_s32(zv, d));
    } else if constexpr (mode == 2) { (void)a; return vcleq_u32(vreinterpretq_u32_s32(vaddq_s32(vsubq_s32(d, zv), vdupq_n_s32(0x200))), vdupq_n_u32(0x400)); }
    else { (void)a; return vcleq_u32(vreinterpretq_u32_s32(vaddq_s32(vsubq_s32(d, zv), vdupq_n_s32(0xFF))), vdupq_n_u32(0x1FE)); }
  };
  for (u32 i = 0; i < n; i += 4) {
    const int32x4_t zv = vld1q_s32(z + i), d = vreinterpretq_s32_u32(vld1q_u32(dstz + i));
    const uint32x4_t a = vld1q_u32(dstattr + i);
    const uint32x4_t ok = test(zv, d, a);
    uint32x4_t v;
    if constexpr (under) {
      const uint32x4_t cand = vbicq_u32(vtstq_u32(a, vdupq_n_u32(0xF)), ok);
      v = vandq_u32(ok, one);
      if (compat::maxv_u32(cand)) {
        const int32x4_t ud = vreinterpretq_s32_u32(vld1q_u32(dstz + under_off + i));
        uint32x4_t ua = a;
        if constexpr (mode == 1) ua = vld1q_u32(dstattr + under_off + i);
        v = vorrq_u32(v, vandq_u32(vandq_u32(cand, test(zv, ud, ua)), two));
      }
    } else { v = vandq_u32(ok, one); (void)two; (void)under_off; }
    const uint16x4_t v16 = vmovn_u32(v);
    const uint8x8_t v8 = vmovn_u16(vcombine_u16(v16, v16));
    vst1_lane_u32(reinterpret_cast<u32*>(pass + i), vreinterpret_u32_u8(v8), 0);
    any = vorrq_u32(any, v);
  }
  if (compat::maxv_u32(any) == 0) return 0;
  u32 first = 0; while (!pass[first]) ++first;
  u32 last = n; while (last && !pass[last - 1]) --last;
  if (!last) return 0;   // the only hits were in the rounded-up tail
  return (first << 16) | last;
}

// Shadow polygons: the stencil picks the layer per pixel (kernels.h).
template <int mode>
static u32 depth_candidates_shadow_m(const s32* z, const u32* dstz, const u32* dstattr, const u8* stencil, u32 n, u8* pass, u32 under_off) {
  const uint32x4_t one = vdupq_n_u32(1), two = vdupq_n_u32(2), four = vdupq_n_u32(4);
  uint32x4_t any = vdupq_n_u32(0);
  auto test = [](int32x4_t zv, int32x4_t d, uint32x4_t a) -> uint32x4_t {
    if constexpr (mode == 0) { (void)a; return vcltq_s32(zv, d); }
    else if constexpr (mode == 1) {
      const uint32x4_t back = vceqq_u32(vandq_u32(a, vdupq_n_u32(0x00400010)), vdupq_n_u32(0x10));
      return vbslq_u32(back, vcleq_s32(zv, d), vcltq_s32(zv, d));
    } else if constexpr (mode == 2) { (void)a; return vcleq_u32(vreinterpretq_u32_s32(vaddq_s32(vsubq_s32(d, zv), vdupq_n_s32(0x200))), vdupq_n_u32(0x400)); }
    else { (void)a; return vcleq_u32(vreinterpretq_u32_s32(vaddq_s32(vsubq_s32(d, zv), vdupq_n_s32(0xFF))), vdupq_n_u32(0x1FE)); }
  };
  for (u32 i = 0; i < n; i += 4) {
    u32 st4; std::memcpy(&st4, stencil + i, 4);
    const uint32x4_t st = vmovl_u16(vget_low_u16(vmovl_u8(vreinterpret_u8_u32(vdup_n_u32(st4)))));
    const uint32x4_t top_named = vtstq_u32(st, one), under_named = vtstq_u32(st, two);
    const int32x4_t zv = vld1q_s32(z + i), d = vreinterpretq_s32_u32(vld1q_u32(dstz + i));
    const uint32x4_t a = vld1q_u32(dstattr + i);
    const uint32x4_t ok = vandq_u32(top_named, test(zv, d, a));
    uint32x4_t v = vandq_u32(ok, vorrq_u32(one, vandq_u32(under_named, four)));
    if (under_off) {
      // Under candidates: top named and failed with edge flags, or top unnamed.
      const uint32x4_t fallback = vandq_u32(vbicq_u32(vandq_u32(top_named, under_named), ok), vtstq_u32(a, vdupq_n_u32(0xF)));
      const uint32x4_t direct = vbicq_u32(under_named, top_named);
      const uint32x4_t cand = vorrq_u32(fallback, direct);
      if (compat::maxv_u32(cand)) {
        const int32x4_t ud = vreinterpretq_s32_u32(vld1q_u32(dstz + under_off + i));
        uint32x4_t ua = a;
        if constexpr (mode == 1) ua = vbslq_u32(direct, a, vld1q_u32(dstattr + under_off + i));   // direct: the top's attributes
        v = vorrq_u32(v, vandq_u32(vandq_u32(cand, test(zv, ud, ua)), two));
      }
    }
    const uint16x4_t v16 = vmovn_u32(v);
    const uint8x8_t v8 = vmovn_u16(vcombine_u16(v16, v16));
    vst1_lane_u32(reinterpret_cast<u32*>(pass + i), vreinterpret_u32_u8(v8), 0);
    any = vorrq_u32(any, v);
  }
  if (compat::maxv_u32(any) == 0) return 0;
  u32 first = 0; while (!pass[first]) ++first;
  u32 last = n; while (last && !pass[last - 1]) --last;
  if (!last) return 0;
  return (first << 16) | last;
}

u32 depth_candidates_shadow(int mode, const s32* z, const u32* dstz, const u32* dstattr, const u8* stencil, u32 n, u8* pass, u32 under_off) {
  switch (mode) {
  case 0:  return depth_candidates_shadow_m<0>(z, dstz, dstattr, stencil, n, pass, under_off);
  case 1:  return depth_candidates_shadow_m<1>(z, dstz, dstattr, stencil, n, pass, under_off);
  case 2:  return depth_candidates_shadow_m<2>(z, dstz, dstattr, stencil, n, pass, under_off);
  default: return depth_candidates_shadow_m<3>(z, dstz, dstattr, stencil, n, pass, under_off);
  }
}

u32 depth_candidates(int mode, const s32* z, const u32* dstz, const u32* dstattr, u32 n, u8* pass, u32 under_off) {
  if (under_off) {
    switch (mode) {
    case 0:  return depth_candidates_m<0, true>(z, dstz, dstattr, n, pass, under_off);
    case 1:  return depth_candidates_m<1, true>(z, dstz, dstattr, n, pass, under_off);
    case 2:  return depth_candidates_m<2, true>(z, dstz, dstattr, n, pass, under_off);
    default: return depth_candidates_m<3, true>(z, dstz, dstattr, n, pass, under_off);
    }
  }
  switch (mode) {
  case 0:  return depth_candidates_m<0, false>(z, dstz, dstattr, n, pass, 0);
  case 1:  return depth_candidates_m<1, false>(z, dstz, dstattr, n, pass, 0);
  case 2:  return depth_candidates_m<2, false>(z, dstz, dstattr, n, pass, 0);
  default: return depth_candidates_m<3, false>(z, dstz, dstattr, n, pass, 0);
  }
}

} // namespace ds::gpu::kern::neon

#endif // DSPERATE_NEON
