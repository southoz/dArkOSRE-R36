// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Portable reference kernels. These define the behaviour; the NEON twins in
// kernels_neon.cpp must match them bit for bit.
#include "core/gpu/kernels.h"

namespace ds::gpu::kern {
const Pixel* direct_table() { static const Pixel sentinel[1] = {}; return sentinel; }
}

namespace ds::gpu::kern::ref {

namespace {

inline Pixel blend(Pixel a, Pixel b, u32 eva, u32 evb) {
  u32 r = (((a & 0x00003F) * eva) + ((b & 0x00003F) * evb) + 0x000008) >> 4;
  u32 g = ((((a & 0x003F00) * eva) + ((b & 0x003F00) * evb) + 0x000800) >> 4) & 0x007F00;
  u32 bl = ((((a & 0x3F0000) * eva) + ((b & 0x3F0000) * evb) + 0x080000) >> 4) & 0x7F0000;
  if (r > 0x3F) r = 0x3F;
  if (g > 0x3F00) g = 0x3F00;
  if (bl > 0x3F0000) bl = 0x3F0000;
  return r | g | bl | 0xFF000000;
}

// 3D/2D blend with the 3D layer's 5-bit alpha (alpha+1 of 32).
inline Pixel blend_3d(Pixel a, Pixel b) {
  const u32 eva = ((a >> 24) & 0x1F) + 1, evb = 32 - eva;
  if (eva == 32) return a;
  u32 r = (((a & 0x00003F) * eva) + ((b & 0x00003F) * evb) + 0x000010) >> 5;
  u32 g = ((((a & 0x003F00) * eva) + ((b & 0x003F00) * evb) + 0x001000) >> 5) & 0x007F00;
  u32 bl = ((((a & 0x3F0000) * eva) + ((b & 0x3F0000) * evb) + 0x100000) >> 5) & 0x7F0000;
  if (r > 0x3F) r = 0x3F;
  if (g > 0x3F00) g = 0x3F00;
  if (bl > 0x3F0000) bl = 0x3F0000;
  return r | g | bl | 0xFF000000;
}

inline Pixel brighten(Pixel v, u32 factor, u32 bias) {
  u32 rb = v & 0x3F003F, g = v & 0x003F00;
  rb += (((((0x3F003F - rb) * factor) + (bias * 0x010001)) >> 4) & 0x3F003F);
  g  += (((((0x003F00 - g) * factor) + (bias * 0x000100)) >> 4) & 0x003F00);
  return rb | g | 0xFF000000;
}
inline Pixel darken(Pixel v, u32 factor, u32 bias) {
  u32 rb = v & 0x3F003F, g = v & 0x003F00;
  rb -= ((((rb * factor) + (bias * 0x010001)) >> 4) & 0x3F003F);
  g  -= ((((g * factor) + (bias * 0x000100)) >> 4) & 0x003F00);
  return rb | g | 0xFF000000;
}

} // namespace

bool line_has_translucent_3d(const Pixel* line3d) {
  for (u32 i = 0; i < 256; ++i) { const u32 a = (line3d[i] >> 24) & 0x1F; if (a != 0 && a != 31) return true; }
  return false;
}

void composite_line(u32 bldcnt, u32 eva, u32 evb, u32 evy, const Pixel* top, const Pixel* second,
                    const u8* top_id, const u8* top_kind, const u8* top_alpha, const u8* second_id,
                    const u8* win, Pixel* out) {
  const u32 effect = (bldcnt >> 6) & 3;
  for (u32 i = 0; i < 256; ++i) {
    const Pixel a = top[i], b = second[i];
    const u32 t1 = top_id[i], t2 = static_cast<u32>(second_id[i]) << 8;
    const u8 kind = top_kind[i];
    Pixel o = a;
    if ((kind == K_OBJ_SEMI || kind == K_OBJ_BITMAP) && (bldcnt & t2)) {
      // Semi-transparent and bitmap sprites blend whenever the layer below is
      // a second target, regardless of the selected effect.
      const u32 ea = (kind == K_OBJ_BITMAP) ? top_alpha[i] : eva;
      const u32 eb = (kind == K_OBJ_BITMAP) ? 16 - ea : evb;
      o = blend(a, b, ea, eb);
    } else if (kind == K_3D && (bldcnt & t2)) {
      o = blend_3d(a, b);
    } else if ((bldcnt & t1) && (win[i] & 0x20)) {
      switch (effect) {
      case 1: if (bldcnt & t2) o = blend(a, b, eva, evb); break;
      case 2: o = brighten(a, evy, 0x8); break;
      case 3: o = darken(a, evy, 0x7); break;
      default: break;
      }
    }
    out[i] = (o & 0x00FFFFFF) | 0xFF000000;
  }
}

void palette_to_18(const u16* pal, Pixel* out, u32 n) {
  for (u32 i = 0; i < n; ++i) {
    const u32 c = pal[i];
    out[i] = ((c & 0x001F) << 1) | ((((c & 0x03E0) >> 4) | ((c & 0x8000) >> 15)) << 8) | (((c & 0x7C00) >> 9) << 16);
  }
}




namespace {
inline void obj_plot(u32 i, u16 value, bool opaque, u8 attr, u8 alpha, u16* px, u8* oattr, u8* oalpha) {
  const u8 old = oattr[i];
  if (opaque && (!(old & OA_OPAQUE) || (attr & OA_PRIO) < (old & OA_PRIO))) {
    px[i] = value; oattr[i] = attr | OA_OPAQUE; oalpha[i] = alpha;
  } else if (!opaque && !(old & OA_OPAQUE)) {
    oattr[i] = (old & ~(OA_MOSAIC | OA_PRIO)) | (attr & (OA_TOUCHED | OA_MOSAIC | OA_PRIO));
  }
}
}




namespace {
inline u8 obj_tid(u8 attr) { return (attr & OA_BITMAP) ? T_OBJ_DIRECT : (attr & OA_STDPAL) ? T_OBJ_STD : T_OBJ_EXT; }
}

void select16(const u16* v, const u8* win, u8 wbit, u8 tid, u16* top, u8* top_tid, u16* second, u8* second_tid) {
  for (u32 i = 0; i < 256; ++i) {
    if (!(v[i] & LV_OPAQUE) || !(win[i] & wbit)) continue;
    second[i] = top[i]; second_tid[i] = top_tid[i];
    top[i] = v[i]; top_tid[i] = tid;
  }
}

void select16_obj(const u16* v, const u8* attr, const u8* win, u32 prio, u16* top, u8* top_tid, u16* second, u8* second_tid) {
  for (u32 i = 0; i < 256; ++i) {
    const u8 a = attr[i];
    if (!(a & OA_OPAQUE) || (a & OA_PRIO) != prio || !(win[i] & 0x10)) continue;
    second[i] = top[i]; second_tid[i] = top_tid[i];
    top[i] = v[i]; top_tid[i] = obj_tid(a);
  }
}

void select16_nowin(const u16* v, u8 tid, u16* top, u8* top_tid, u16* second, u8* second_tid) {
  for (u32 i = 0; i < 256; ++i) {
    if (!(v[i] & LV_OPAQUE)) continue;
    second[i] = top[i]; second_tid[i] = top_tid[i];
    top[i] = v[i]; top_tid[i] = tid;
  }
}

void select16_obj_nowin(const u16* v, const u8* attr, u32 prio, u16* top, u8* top_tid, u16* second, u8* second_tid) {
  for (u32 i = 0; i < 256; ++i) {
    const u8 a = attr[i];
    if (!(a & OA_OPAQUE) || (a & OA_PRIO) != prio) continue;
    second[i] = top[i]; second_tid[i] = top_tid[i];
    top[i] = v[i]; top_tid[i] = obj_tid(a);
  }
}

void select16_flat(const u16* v, const u8* win, u8 wbit, u8 tid, u16* top, u8* top_tid) {
  for (u32 i = 0; i < 256; ++i) {
    if (!(v[i] & LV_OPAQUE) || !(win[i] & wbit)) continue;
    top[i] = v[i]; top_tid[i] = tid;
  }
}

void select16_flat_nowin(const u16* v, u8 tid, u16* top, u8* top_tid) {
  for (u32 i = 0; i < 256; ++i) if (v[i] & LV_OPAQUE) { top[i] = v[i]; top_tid[i] = tid; }
}

void select16_obj_flat_nowin(const u16* v, const u8* attr, u32 prio, u16* top, u8* top_tid) {
  for (u32 i = 0; i < 256; ++i) {
    const u8 a = attr[i];
    if (!(a & OA_OPAQUE) || (a & OA_PRIO) != prio) continue;
    top[i] = v[i]; top_tid[i] = obj_tid(a);
  }
}

void select16_obj_flat(const u16* v, const u8* attr, const u8* win, u32 prio, u16* top, u8* top_tid) {
  for (u32 i = 0; i < 256; ++i) {
    const u8 a = attr[i];
    if (!(a & OA_OPAQUE) || (a & OA_PRIO) != prio || !(win[i] & 0x10)) continue;
    top[i] = v[i]; top_tid[i] = obj_tid(a);
  }
}

void resolve16(const u16* top, const u8* top_tid, const Pixel* const* tables, Pixel* out) {
  for (u32 i = 0; i < 256; ++i) out[i] = resolve_one(tables[top_tid[i]], top[i]);
}

void resolve16_one(const u16* v, const Pixel* table, Pixel* out) {
  for (u32 i = 0; i < 256; ++i) out[i] = resolve_one(table, v[i]);
}

void resolve16_top(const u16* top, const u8* top_tid, const Pixel* const* tables, const Pixel* line3d,
                   Pixel* top_px, u8* top_id) {
  static const u8 id_of[T_COUNT] = {L_BG0, L_BG1, L_BG2, L_BG3, L_OBJ, L_OBJ, L_OBJ, L_BACKDROP, 0};
  for (u32 i = 0; i < 256; ++i) {
    const u8 tt = top_tid[i];
    top_id[i] = id_of[tt];
    top_px[i] = (line3d && tt == T_BG0) ? line3d[i] : resolve_one(tables[tt], top[i]);
  }
}

void composite_line_fade(u32 bldcnt, u32 evy, const Pixel* top, const u8* top_id, const u8* win, Pixel* out) {
  const u32 effect = (bldcnt >> 6) & 3;
  for (u32 i = 0; i < 256; ++i) {
    Pixel o = top[i];
    if ((bldcnt & top_id[i]) && (win[i] & 0x20))
      o = effect == 2 ? brighten(o, evy, 0x8) : darken(o, evy, 0x7);
    out[i] = (o & 0x00FFFFFF) | 0xFF000000;
  }
}

void resolve16_full(const u16* top, const u8* top_tid, const u16* second, const u8* second_tid,
                    const Pixel* const* tables, const u8* attr, const u8* alpha, const Pixel* line3d,
                    Pixel* top_px, Pixel* second_px, u8* top_id, u8* top_kind, u8* top_alpha, u8* second_id) {
  static const u8 id_of[T_COUNT] = {L_BG0, L_BG1, L_BG2, L_BG3, L_OBJ, L_OBJ, L_OBJ, L_BACKDROP, 0};
  for (u32 i = 0; i < 256; ++i) {
    const u8 tt = top_tid[i], st = second_tid[i];
    top_px[i] = resolve_one(tables[tt], top[i]);
    second_px[i] = resolve_one(tables[st], second[i]);
    top_id[i] = id_of[tt]; second_id[i] = id_of[st];
    u8 kind = K_NORMAL, a = 0;
    if (tt == T_BG0 && line3d) { kind = K_3D; a = (line3d[i] >> 24) & 0x1F; top_px[i] = line3d[i]; }
    else if (tt >= T_OBJ_STD && tt <= T_OBJ_DIRECT) {
      const u8 at = attr[i];
      kind = (at & OA_BITMAP) ? K_OBJ_BITMAP : (at & OA_SEMI) ? K_OBJ_SEMI : K_NORMAL;
      a = alpha[i];
    }
    top_kind[i] = kind; top_alpha[i] = a;
  }
}

void obj_row_idx16(const u8* idx, u32 n, u16 pal_base, u8 attr, u16* v, u8* oattr, u8* oalpha) {
  for (u32 i = 0; i < n; ++i) obj_plot(i, static_cast<u16>(LV_OPAQUE | pal_base | idx[i]), idx[i] != 0, attr, 0, v, oattr, oalpha);
}

void obj_row_bmp16(const u16* col, u32 n, u8 attr, u8 alpha, u16* v, u8* oattr, u8* oalpha) {
  for (u32 i = 0; i < n; ++i) obj_plot(i, col[i], col[i] & 0x8000, attr, alpha, v, oattr, oalpha);
}

bool layer16_3d(const u32* line3d, u16* v) {
  u32 any = 0;
  for (u32 i = 0; i < 256; ++i) { const u32 a = line3d[i] >> 24; any |= a; v[i] = a ? static_cast<u16>(LV_OPAQUE | i) : 0; }
  return any != 0;
}

bool text_ctl(const u16* tiles, u8* ctl) {
  bool uniform = true;
  for (u32 t = 0; t < 33; ++t) {
    ctl[t] = static_cast<u8>((tiles[t] >> 12) | ((tiles[t] >> 6) & 0x10));
    uniform &= tiles[t] == tiles[0];
  }
  return uniform;
}

bool text_row_16(const u8* packed, const u8* ctl, u32 n, u16* v) {
  bool any = false;
  for (u32 t = 0; t < n; ++t, packed += 4, v += 8) {
    const u16 base = static_cast<u16>((ctl[t] & 0xF) << 4);
    const bool flip = ctl[t] & 0x10;
    for (u32 i = 0; i < 8; ++i) {
      const u32 j = flip ? 7 - i : i;
      const u8 idx = (packed[j >> 1] >> ((j & 1) * 4)) & 0xF;
      v[i] = idx ? static_cast<u16>(LV_OPAQUE | base | idx) : 0; any |= idx != 0;
    }
  }
  return any;
}

bool text_row_256(const u8* rows, const u8* ctl, u32 n, bool ext, u16* v) {
  bool any = false;
  for (u32 t = 0; t < n; ++t, rows += 8, v += 8) {
    const u16 base = ext ? static_cast<u16>((ctl[t] & 0xF) << 8) : 0;
    const bool flip = ctl[t] & 0x10;
    for (u32 i = 0; i < 8; ++i) {
      const u8 idx = rows[flip ? 7 - i : i];
      v[i] = idx ? static_cast<u16>(LV_OPAQUE | base | idx) : 0; any |= idx != 0;
    }
  }
  return any;
}

bool bmp_row_8(const u8* idx, u32 n, u16* v) {
  bool any = false;
  for (u32 i = 0; i < n; ++i) { const u8 x = idx[i]; v[i] = x ? static_cast<u16>(LV_OPAQUE | x) : 0; any |= x != 0; }
  return any;
}
bool bmp_row_16(const u16* col, u32 n, u16* v) {
  bool any = false;
  for (u32 i = 0; i < n; ++i) { const u16 c = col[i]; v[i] = (c & 0x8000) ? c : 0; any |= (c & 0x8000) != 0; }
  return any;
}

void master_brightness(u16 reg, u32* dst) {
  const u32 mode = reg >> 14;
  u32 factor = reg & 0x1F;
  if (factor > 16) factor = 16;
  if (mode == 1) {
    for (u32 i = 0; i < 256; ++i) {
      const u32 v = dst[i]; u32 rb = v & 0x3F003F, g = v & 0x003F00;
      rb += ((((0x3F003F - rb) * factor) >> 4) & 0x3F003F);
      g  += ((((0x003F00 - g) * factor) >> 4) & 0x003F00);
      dst[i] = rb | g | 0xFF000000;
    }
  } else if (mode == 2) {
    for (u32 i = 0; i < 256; ++i) {
      const u32 v = dst[i]; u32 rb = v & 0x3F003F, g = v & 0x003F00;
      rb -= ((((rb * factor) + (0xF * 0x010001)) >> 4) & 0x3F003F);
      g  -= ((((g * factor) + (0xF * 0x000100)) >> 4) & 0x003F00);
      dst[i] = rb | g | 0xFF000000;
    }
  }
}

void expand_colours(u32* dst) {
  for (u32 i = 0; i < 256; ++i) {
    const u32 c = dst[i];
    const u32 v = ((c & 0x3F) << 18) | ((c & 0x3F00) << 2) | ((c & 0x3F0000) >> 14);
    dst[i] = v | ((v & 0xC0C0C0) >> 6) | 0xFF000000;
  }
}

void output_line(const Pixel* src, u16 reg, u32* dst) {
  for (u32 i = 0; i < 256; ++i) dst[i] = src[i];
  master_brightness(reg, dst);
  expand_colours(dst);
}

void output_vram_line(const u16* src, u16 reg, u32* dst) {
  for (u32 i = 0; i < 256; ++i) { const u32 c = src[i]; dst[i] = ((c & 0x001F) << 1) | (((c & 0x03E0) >> 4) << 8) | (((c & 0x7C00) >> 9) << 16); }
  master_brightness(reg, dst);
  expand_colours(dst);
}

void capture_a15(const Pixel* src, u32 n, u16* dst) {
  for (u32 i = 0; i < n; ++i) {
    const u32 v = src[i];
    const u32 r = (v >> 1) & 0x1F, g = (v >> 9) & 0x1F, b = (v >> 17) & 0x1F, a = (v >> 24) ? 1u : 0u;
    dst[i] = static_cast<u16>(r | (g << 5) | (b << 10) | (a << 15));
  }
}

void capture_blend(const Pixel* srca, const u16* srcb, u32 n, u32 eva, u32 evb, u16* dst) {
  for (u32 i = 0; i < n; ++i) {
    const u32 v = srca[i];
    const u32 ra = (v >> 1) & 0x1F, ga = (v >> 9) & 0x1F, ba = (v >> 17) & 0x1F, aa = (v >> 24) ? 1u : 0u;
    const u32 w = srcb[i];
    const u32 rb = w & 0x1F, gb = (w >> 5) & 0x1F, bb = (w >> 10) & 0x1F, ab = w >> 15;
    u32 rd = ((ra * aa * eva) + (rb * ab * evb) + 8) >> 4;
    u32 gd = ((ga * aa * eva) + (gb * ab * evb) + 8) >> 4;
    u32 bd = ((ba * aa * eva) + (bb * ab * evb) + 8) >> 4;
    const u32 ad = (eva > 0 ? aa : 0) | (evb > 0 ? ab : 0);
    if (rd > 0x1F) rd = 0x1F;
    if (gd > 0x1F) gd = 0x1F;
    if (bd > 0x1F) bd = 0x1F;
    dst[i] = static_cast<u16>(rd | (gd << 5) | (bd << 10) | (ad << 15));
  }
}

void scale_row(const u32* src, const u16* xrun, u32* dst) {
  for (u32 s = 0; s < 256; ++s) {
    const u32 c = src[s];
    for (u32 x = xrun[s]; x < xrun[s + 1]; ++x) dst[x] = c;
  }
}

static inline u32 dim_px(u32 c, u32 f) {
  const u32 rb = ((c & 0x00FF00FFu) * f >> 8) & 0x00FF00FFu;
  const u32 g  = ((c & 0x0000FF00u) * f >> 8) & 0x0000FF00u;
  return (c & 0xFF000000u) | rb | g;
}

void scale_row_straddle(const u32* src, const u32* seam, const u8* w, const u16* xrun, u32* dst) {
  for (u32 s = 0; s < 256; ++s) {
    const u32 c = src[s];
    const u32 x0 = xrun[s], x1 = xrun[s + 1];
    for (u32 x = x0; x < x1; ++x) dst[x] = c;
    if (w[s] && x1 > x0) dst[x1 - 1] = seam[s];
  }
}

void blend_line_w(const u32* a, const u32* b, const u8* w, u32* out) {
  for (u32 i = 0; i < 256; ++i) {
    const u32 x = a[i], y = b[i], f = w[i];
    u32 r = 0;
    for (u32 sh = 0; sh < 32; sh += 8) {
      const u32 xa = (x >> sh) & 255, ya = (y >> sh) & 255;
      r |= ((xa * (256 - f) + ya * f + 128) >> 8) << sh;
    }
    out[i] = r;
  }
}

static inline u32 lerp_px(u32 x, u32 y, u32 f) {
  u32 r = 0;
  for (u32 sh = 0; sh < 32; sh += 8) {
    const u32 xa = (x >> sh) & 255, ya = (y >> sh) & 255;
    r |= ((xa * (256 - f) + ya * f + 128) >> 8) << sh;
  }
  return r;
}

void lerp_row_gather(const u32* src, const u16* sx, const u8* wx, u32 n, u32* out) {
  for (u32 x = 0; x < n; ++x) out[x] = lerp_px(src[sx[x]], src[sx[x] + 1], wx[x]);
}

void lerp_rows(const u32* a, const u32* b, u32 w, u32 n, u32* out) {
  for (u32 i = 0; i < n; ++i) out[i] = lerp_px(a[i], b[i], w);
}

void scale_row_grid(const u32* src, const u16* xrun, u32 f, u32 min_run, u32 pitch, bool seam_row, u32* dst) {
  if (min_run < 2) min_run = 2;
  if (pitch < 1) pitch = 1;
  for (u32 s = 0; s < 256; ++s) {
    const u32 c = src[s], cd = f ? dim_px(c, f) : 0xFF000000u;   // f == 0: opaque black, whatever the source alpha
    const u32 x0 = xrun[s], x1 = xrun[s + 1];
    if (seam_row) { for (u32 x = x0; x < x1; ++x) dst[x] = cd; continue; }
    for (u32 x = x0; x < x1; ++x) dst[x] = c;
    if (x1 - x0 >= min_run && s % pitch == 0) dst[x0] = cd;
  }
}


// ---- 3D span stages ----

void span_factor(s32 xv0, u32 n, s32 xdiff, s32 w0n, s32 w0d, s32 w1d, u32* fac) {
  if (w0d == w1d) {
    // W constant: factor is linear in x, generated as a 16.16 ramp (approximation of num/den).
    const u32 d = static_cast<u32>(xdiff) * static_cast<u32>(w0d);
    if (d == 0) { for (u32 i = 0; i < n; ++i) fac[i] = 0; return; }
    const u32 step = static_cast<u32>((static_cast<u64>(static_cast<u32>(w0n) << 8) << 16) / d);
    for (u32 i = 0; i < n; ++i) fac[i] = (static_cast<u32>(xv0 + static_cast<s32>(i)) * step) >> 16;
    return;
  }
  for (u32 i = 0; i < n; ++i) {
    const s32 xv = xv0 + static_cast<s32>(i);
    const u32 num = static_cast<u32>(xv * w0n) << 8;
    const u32 den = static_cast<u32>(xv * w0d) + static_cast<u32>((xdiff - xv) * w1d);
    fac[i] = den == 0 ? 0 : num / den;
  }
}

void span_attr_persp(s32 y0, s32 y1, const u32* fac, u32 n, s32* out, u32) {
  if (y0 == y1) { for (u32 i = 0; i < n; ++i) out[i] = y0; return; }
  if (y0 < y1) { const s64 d = y1 - y0; for (u32 i = 0; i < n; ++i) out[i] = y0 + static_cast<s32>((d * fac[i]) >> 8); return; }
  const s64 d = y0 - y1;
  for (u32 i = 0; i < n; ++i) out[i] = y1 + static_cast<s32>((d * (256 - fac[i])) >> 8);
}

void span_attrs5(const s32* y0, const s32* y1, const u32* fac, u32 n, s32* const* out, u32 fmax) {
  for (int k = 0; k < 5; ++k) span_attr_persp(y0[k], y1[k], fac, n, out[k], fmax);
}

void span_attrs2n(const s32* y0, const s32* y1, const u32* fac, u32 n, s16* sc, s16* tc, u32 fmax) {
  s32 tmp[2][272];
  span_attr_persp(y0[3], y1[3], fac, n, tmp[0], fmax);
  span_attr_persp(y0[4], y1[4], fac, n, tmp[1], fmax);
  for (u32 i = 0; i < n; ++i) { sc[i] = static_cast<s16>(tmp[0][i]); tc[i] = static_cast<s16>(tmp[1][i]); }
}

void span_attrs5n(const s32* y0, const s32* y1, const u32* fac, u32 n, u8* vr, u8* vg, u8* vb, s16* sc, s16* tc, u32 fmax) {
  s32 tmp[5][272];
  s32* out[5] = {tmp[0], tmp[1], tmp[2], tmp[3], tmp[4]};
  span_attrs5(y0, y1, fac, n, out, fmax);
  for (u32 i = 0; i < n; ++i) {
    vr[i] = static_cast<u8>((static_cast<u32>(tmp[0][i]) >> 3) & 0xFF);
    vg[i] = static_cast<u8>((static_cast<u32>(tmp[1][i]) >> 3) & 0xFF);
    vb[i] = static_cast<u8>((static_cast<u32>(tmp[2][i]) >> 3) & 0xFF);
    sc[i] = static_cast<s16>(tmp[3][i]);
    tc[i] = static_cast<s16>(tmp[4][i]);
  }
}

// One linear attribute at one pixel (as span_attr_linear).
static inline s32 lin_at(s32 y0, s32 y1, s32 xv, s32 xdiff) {
  if (y0 == y1) return y0;
  if (y0 < y1) return y0 + static_cast<s32>(static_cast<s64>(y1 - y0) * xv / xdiff);
  return y1 + static_cast<s32>(static_cast<s64>(y0 - y1) * (xdiff - xv) / xdiff);
}

void span_attrs5n_lin(const s32* y0, const s32* y1, s32 xv0, u32 n, s32 xdiff, u8* vr, u8* vg, u8* vb, s16* sc, s16* tc) {
  u8* const cout[3] = {vr, vg, vb};
  s16* const tout[2] = {sc, tc};
  for (u32 i = 0; i < n; ++i) {
    const s32 xv = xv0 + static_cast<s32>(i);
    for (int k = 0; k < 3; ++k) cout[k][i] = static_cast<u8>((static_cast<u32>(lin_at(y0[k], y1[k], xv, xdiff)) >> 3) & 0xFF);
    for (int k = 3; k < 5; ++k) tout[k - 3][i] = static_cast<s16>(lin_at(y0[k], y1[k], xv, xdiff));
  }
}

void span_attrs2n_lin(const s32* y0, const s32* y1, s32 xv0, u32 n, s32 xdiff, s16* sc, s16* tc) {
  s16* const tout[2] = {sc, tc};
  for (u32 i = 0; i < n; ++i) {
    const s32 xv = xv0 + static_cast<s32>(i);
    for (int k = 3; k < 5; ++k) tout[k - 3][i] = static_cast<s16>(lin_at(y0[k], y1[k], xv, xdiff));
  }
}

void span_attr_linear(s32 y0, s32 y1, s32 xv0, u32 n, s32 xdiff, s32* out) {
  if (y0 == y1) { for (u32 i = 0; i < n; ++i) out[i] = y0; return; }
  for (u32 i = 0; i < n; ++i) {
    const s32 xv = xv0 + static_cast<s32>(i);
    if (y0 < y1) out[i] = y0 + static_cast<s32>(static_cast<s64>(y1 - y0) * xv / xdiff);
    else out[i] = y1 + static_cast<s32>(static_cast<s64>(y0 - y1) * (xdiff - xv) / xdiff);
  }
}

void span_z_linear(s32 z0, s32 z1, s32 xv0, u32 n, s32 xdiff, s32 xrecip, s32* out) {
  if (z0 == z1) { for (u32 i = 0; i < n; ++i) out[i] = z0; return; }
  s32 base, disp;
  if (z0 < z1) { base = z0; disp = z1 - z0; } else { base = z1; disp = z0 - z1; }
  disp >>= 9;
  for (u32 i = 0; i < n; ++i) {
    const s32 xv = xv0 + static_cast<s32>(i);
    const s32 factor = z0 < z1 ? xv : xdiff - xv;
    out[i] = base + static_cast<s32>((static_cast<s64>(disp) * factor * xrecip) >> 13);
  }
}

void span_z_const(s32 z, u32 n, s32* out) {
  for (u32 i = 0; i < n; ++i) out[i] = z;
}

void clear_image_run(const u16* col, const u16* dep, u32 n, u32 polyid, u32* color, u32* depth, u32* attr) {
  auto c6 = [](u32 c5) { return c5 ? c5 * 2 + 1 : 0; };
  for (u32 i = 0; i < n; ++i) {
    const u32 c = col[i], d = dep[i];
    color[i] = c6(c & 0x1F) | (c6((c >> 5) & 0x1F) << 8) | (c6((c >> 10) & 0x1F) << 16) | ((c & 0x8000) ? 0x1F000000u : 0);
    depth[i] = ((d & 0x7FFF) * 0x200) + 0x1FF;
    attr[i] = polyid | (d & 0x8000);
  }
}

u32 depth_candidates(int mode, const s32* z, const u32* dstz, const u32* dstattr, u32 n, u8* pass, u32 under_off) {
  u32 first = n, last = 0;
  auto test = [mode](s32 zi, u32 dz, u32 da) {
    const s32 d = static_cast<s32>(dz);
    switch (mode) {
    case 0: return zi < d;
    case 1: return (da & 0x00400010) == 0x00000010 ? zi <= d : zi < d;
    case 2: return static_cast<u32>((d - zi) + 0x200) <= 0x400;
    default: return static_cast<u32>((d - zi) + 0xFF) <= 0x1FE;
    }
  };
  for (u32 i = 0; i < n; ++i) {
    const bool ok = test(z[i], dstz[i], dstattr[i]);
    const u8 v = ok ? 1 : ((under_off && (dstattr[i] & 0xF) && test(z[i], dstz[i + under_off], dstattr[i + under_off])) ? 2 : 0);
    pass[i] = v;
    if (v) { if (i < first) first = i; last = i; }
  }
  return first < n ? (first << 16) | (last + 1) : 0;
}

u32 depth_candidates_shadow(int mode, const s32* z, const u32* dstz, const u32* dstattr, const u8* stencil, u32 n, u8* pass, u32 under_off) {
  u32 first = n, last = 0;
  auto test = [mode](s32 zi, u32 dz, u32 da) {
    const s32 d = static_cast<s32>(dz);
    switch (mode) {
    case 0: return zi < d;
    case 1: return (da & 0x00400010) == 0x00000010 ? zi <= d : zi < d;
    case 2: return static_cast<u32>((d - zi) + 0x200) <= 0x400;
    default: return static_cast<u32>((d - zi) + 0xFF) <= 0x1FE;
    }
  };
  for (u32 i = 0; i < n; ++i) {
    const u8 st = stencil[i];
    u8 v = 0;
    if (st & 1) {
      if (test(z[i], dstz[i], dstattr[i])) v = (st & 2) ? 5 : 1;
      else if ((st & 2) && under_off && (dstattr[i] & 0xF) && test(z[i], dstz[i + under_off], dstattr[i + under_off])) v = 2;
    } else if (st & 2) {
      if (under_off && test(z[i], dstz[i + under_off], dstattr[i])) v = 2;
    }
    pass[i] = v;
    if (v) { if (i < first) first = i; last = i; }
  }
  return first < n ? (first << 16) | (last + 1) : 0;
}

} // namespace ds::gpu::kern::ref
