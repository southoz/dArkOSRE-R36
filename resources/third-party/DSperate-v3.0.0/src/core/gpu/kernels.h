// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"
#include "core/gpu/engine2d.h"

// Line-stage kernels of the 2D pipeline and the output stage, as free
// functions over plane pointers. Each has a portable C++ reference
// (kernels_ref.cpp) and, on AArch64, a NEON twin (kernels_neon.cpp) with the
// same name and signature. `kern::active` is the one the renderer calls.
//
// All pixel arrays are 256 entries and 16-byte aligned; `n` counts in the
// palette kernel are multiples of 8.

namespace ds::gpu::kern {

// Direct colour (BGR555, bit 15 ignored) resolves by arithmetic, not table:
// `direct_table()` is a sentinel the resolve kernels compare against.
const Pixel* direct_table();
inline Pixel direct_colour(u16 c) {
  return ((c & 0x001F) << 1) | (((c & 0x03E0) >> 4) << 8) | (((c & 0x7C00) >> 9) << 16);
}
// One resolved pixel through `tab`, whichever kind it is (mixed blocks).
inline Pixel resolve_one(const Pixel* tab, u16 c) {
  return (tab == direct_table() ? direct_colour(c) : tab[c & 0x7FFF]) | 0xFF000000;
}

#define DS_KERNEL_LIST(NS)                                                                                   \
  /* Priority select of one layer line into top/second values and table ids. */                             \
  void NS##select16(const u16* v, const u8* win, u8 wbit, u8 tid, u16* top, u8* top_tid, u16* second, u8* second_tid); \
  /* OBJ line at one priority level; table id comes from the attribute byte. */                              \
  void NS##select16_obj(const u16* v, const u8* attr, const u8* win, u32 prio, u16* top, u8* top_tid, u16* second, u8* second_tid); \
  /* Without the second record, for lines no colour effect can touch. */                                     \
  void NS##select16_flat(const u16* v, const u8* win, u8 wbit, u8 tid, u16* top, u8* top_tid);              \
  void NS##select16_obj_flat(const u16* v, const u8* attr, const u8* win, u32 prio, u16* top, u8* top_tid);  \
  /* Window-free variants: skip loading/testing the window plane. */                                         \
  void NS##select16_nowin(const u16* v, u8 tid, u16* top, u8* top_tid, u16* second, u8* second_tid); \
  void NS##select16_obj_nowin(const u16* v, const u8* attr, u32 prio, u16* top, u8* top_tid, u16* second, u8* second_tid); \
  void NS##select16_flat_nowin(const u16* v, u8 tid, u16* top, u8* top_tid); \
  void NS##select16_obj_flat_nowin(const u16* v, const u8* attr, u32 prio, u16* top, u8* top_tid);  \
  /* Winning values through their tables to 18-bit records, alpha 0xFF. */                                   \
  void NS##resolve16(const u16* top, const u8* top_tid, const Pixel* const* tables, Pixel* out);             \
  /* Single-opaque-layer fast path: one layer through one table. */                                          \
  void NS##resolve16_one(const u16* v, const Pixel* table, Pixel* out);                                      \
  /* Top and second values to composite records: colours, layer ids as BLDCNT masks, kind, alpha. */         \
  void NS##resolve16_full(const u16* top, const u8* top_tid, const u16* second, const u8* second_tid,        \
                          const Pixel* const* tables, const u8* attr, const u8* alpha, const Pixel* line3d,  \
                          Pixel* top_px, Pixel* second_px, u8* top_id, u8* top_kind, u8* top_alpha, u8* second_id); \
  /* Top records only, for lines whose composite never reads the second target. */                           \
  void NS##resolve16_top(const u16* top, const u8* top_tid, const Pixel* const* tables, const Pixel* line3d, \
                         Pixel* top_px, u8* top_id);                                                        \
  /* Any 3D pixel with alpha strictly between 0 and 31 (bits 24-28). */                                      \
  bool NS##line_has_translucent_3d(const Pixel* line3d);                                                    \
  /* Fade-only colour effects: brighten/darken the first target. No blend, so no second/kind/alpha. */       \
  void NS##composite_line_fade(u32 bldcnt, u32 evy, const Pixel* top, const u8* top_id, const u8* win, Pixel* out); \
  /* Colour effects: blend/brighten/darken with OBJ and 3D override rules. */                                \
  void NS##composite_line(u32 bldcnt, u32 eva, u32 evb, u32 evy, const Pixel* top, const Pixel* second,      \
                          const u8* top_id, const u8* top_kind, const u8* top_alpha, const u8* second_id,     \
                          const u8* win, Pixel* out);                                                        \
  /* BGR555 palette entries -> 18-bit records (bit 15 = low green bit). */                                   \
  void NS##palette_to_18(const u16* pal, Pixel* out, u32 n);                                                 \
  /* OBJ row plot: opaque wins over transparent/lower priority; transparent stamps priority+mosaic. Paletted: \
     idx!=0 opaque, value 0x8000|pal_base|idx, alpha 0. Bitmap: bit 15 opaque, value col. May touch 15 entries past n. */ \
  void NS##obj_row_idx16(const u8* idx, u32 n, u16 pal_base, u8 attr, u16* v, u8* oattr, u8* oalpha);        \
  void NS##obj_row_bmp16(const u16* col, u32 n, u8 attr, u8 alpha, u16* v, u8* oattr, u8* oalpha);           \
  /* 3D layer as a layer line: 0x8000|x where alpha is non-zero, 0 elsewhere. */                             \
  bool NS##layer16_3d(const u32* line3d, u16* v);                                                           \
  /* Text BG row, 16-colour tiles: ctl[t] = palette (bits 0-3) | 0x10 hflip. Writes 8*n values               \
     0x8000|pal<<4|idx (0 for index 0); returns whether any is opaque. */                                    \
  bool NS##text_row_16(const u8* packed, const u8* ctl, u32 n, u16* v);                                      \
  /* Control bytes for a 33-entry text map row: palette (bits 12-15)|hflip (bit 10->4); returns whether all 33 match. */ \
  bool NS##text_ctl(const u16* tiles, u8* ctl);                                                              \
  /* Text BG row, 256-colour tiles: values 0x8000|(ext?pal<<8:0)|idx. */                                     \
  bool NS##text_row_256(const u8* rows, const u8* ctl, u32 n, bool ext, u16* v);                             \
  /* Bitmap BG row, left to right (identity-matrix rotscale layer). 8-bit: 0x8000|idx (0 for index 0);      \
     direct colour: BGR555 word where bit 15 set, else 0. Returns whether any is opaque. */                  \
  bool NS##bmp_row_8(const u8* idx, u32 n, u16* v);                                                          \
  bool NS##bmp_row_16(const u16* col, u32 n, u16* v);                                                        \
  /* Output stage: master brightness on 18-bit records, then 6->8 bit expansion to 0xAARRGGBB. */            \
  void NS##master_brightness(u16 reg, u32* dst);                                                             \
  void NS##expand_colours(u32* dst);                                                                         \
  /* Both in one pass. */                                                                                     \
  void NS##output_line(const Pixel* src, u16 reg, u32* dst);                                                 \
  /* From a BGR555 line. */                                                                                   \
  void NS##output_vram_line(const u16* src, u16 reg, u32* dst);                                               \
  /* Display capture, source A only: 18-bit records (alpha bits 24-31, non-zero=opaque) -> BGR555, bit 15 = alpha bit. */ \
  void NS##capture_a15(const Pixel* src, u32 n, u16* dst);                                                   \
  /* Capture blend: A as above, B a BGR555 line, alpha in bit 15. Per channel ((ca*aa*eva)+(cb*ab*evb)+8)>>4  \
     clamped to 31; alpha bit (eva?aa:0)|(evb?ab:0). eva/evb in 0..16. Missing B is a zeroed line. */         \
  void NS##capture_blend(const Pixel* srca, const u16* srcb, u32 n, u32 eva, u32 evb, u16* dst);             \
  /* Nearest-neighbour scale to xrun[256] destination pixels. xrun[257]: source pixel s covers destination   \
     [xrun[s], xrun[s+1]) -- inverse of dst_x -> src_x map, so no gather needed. Monotonic non-decreasing. */ \
  void NS##scale_row(const u32* src, const u16* xrun, u32* dst);                                             \
  /* scale_row with the LCD grid: a run of >= min_run pixels has its first pixel's RGB scaled by f/256       \
     (alpha kept); shorter runs written plain. min_run = ceil(width/256). pitch seams every pitch-th source  \
     pixel. seam_row dims every pixel instead. f in 0..255; 0 writes opaque black rather than scaling. */     \
  void NS##scale_row_grid(const u32* src, const u16* xrun, u32 f, u32 min_run, u32 pitch, bool seam_row, u32* dst); \
  /* Box-filter seams for fractional scale: run s's last pixel straddles source s/s+1 when w[s]!=0 and is    \
     written as seam[s] instead of src[s]. */                                                                \
  void NS##scale_row_straddle(const u32* src, const u32* seam, const u8* w, const u16* xrun, u32* dst);     \
  /* out[i] = a[i] + (b[i]-a[i])*w[i]/256 per byte, rounded. w[i]=128 is the midpoint. */                    \
  void NS##blend_line_w(const u32* a, const u32* b, const u8* w, u32* out);                                 \
  /* Bilinear horizontal pass, as blend_line_w. sx[x] <= 254 (caller clamps right edge). */                  \
  void NS##lerp_row_gather(const u32* src, const u16* sx, const u8* wx, u32 n, u32* out);                   \
  /* Bilinear vertical pass: out[i] = a[i] + (b[i]-a[i])*w/256 per byte, rounded, one weight for the row. */ \
  void NS##lerp_rows(const u32* a, const u32* b, u32 w, u32 n, u32* out);                                   \
  /* 3D span stages (render3d.cpp). Perspective factor, 8 fractional bits: num=(xv*w0n)<<8 (32-bit wrap),    \
     den=xv*w0d+(xdiff-xv)*w1d, 0 when den is 0. */                                                          \
  void NS##span_factor(s32 xv0, u32 n, s32 xdiff, s32 w0n, s32 w0d, s32 w1d, u32* fac);                      \
  /* y0+((y1-y0)*f>>8) for y0<y1, else y1+((y0-y1)*(256-f)>>8); y0==y1 -> y0. fmax bounds fac[0,n) (~0u if   \
     unknown); picks 32/64-bit product form only, never the result. Same fmax semantics below. */            \
  void NS##span_attr_persp(s32 y0, s32 y1, const u32* fac, u32 n, s32* out, u32 fmax);                      \
  /* Five span attributes (r g b s t) in one pass; perspective factor loaded once. */                        \
  void NS##span_attrs5(const s32* y0, const s32* y1, const u32* fac, u32 n, s32* const* out, u32 fmax);      \
  /* Narrowed on store: colour as (v>>3)&0xFF, texcoords as s16. Eight pixels/step, span buffers padded by 8. */ \
  void NS##span_attrs5n(const s32* y0, const s32* y1, const u32* fac, u32 n, u8* vr, u8* vg, u8* vb, s16* sc, s16* tc, u32 fmax); \
  /* As span_attrs5n but for a span whose three colour endpoints are equal (caller fills constant colour). */ \
  void NS##span_attrs2n(const s32* y0, const s32* y1, const u32* fac, u32 n, s16* sc, s16* tc, u32 fmax); \
  /* y0+(y1-y0)*xv/xdiff for y0<y1, else y1+(y0-y1)*(xdiff-xv)/xdiff (truncating); |y1-y0|*xdiff < 2^32. */  \
  void NS##span_attr_linear(s32 y0, s32 y1, s32 xv0, u32 n, s32 xdiff, s32* out);                            \
  /* Linear twins of span_attrs5n/span_attrs2n, fused to avoid the per-call 64-bit reciprocal and staging buffer. */ \
  void NS##span_attrs5n_lin(const s32* y0, const s32* y1, s32 xv0, u32 n, s32 xdiff, u8* vr, u8* vg, u8* vb, s16* sc, s16* tc); \
  void NS##span_attrs2n_lin(const s32* y0, const s32* y1, s32 xv0, u32 n, s32 xdiff, s16* sc, s16* tc);      \
  /* Z-buffer depth: base + ((disp>>9)*factor*xrecip>>13), base/disp/factor chosen by z0<z1. */               \
  void NS##span_z_linear(s32 z0, s32 z1, s32 xv0, u32 n, s32 xdiff, s32 xrecip, s32* out);                    \
  /* Constant depth over n pixels (rounded up to 4). */                                                       \
  void NS##span_z_const(s32 z, u32 n, s32* out);                                                              \
  /* Clear image (DISP3DCNT bit 14). colour -> 18-bit record (c to c*2+1, 0 stays 0), alpha 31 if bit 15 set; \
     depth -> (d&0x7FFF)*0x200+0x1FF; attr -> polyid|(d&0x8000). */                                           \
  void NS##clear_image_run(const u16* col, const u16* dep, u32 n, u32 polyid, u32* color, u32* depth, u32* attr); \
  /* Depth pre-pass: pass[i] = 1 where z passes mode's test vs top pixel (0: z<dst; 1: z<=dst for opaque      \
     back-facing dst else z<dst; 2: within 0x200 either way; 3: within 0xFF), 2 where it fails but the top    \
     pixel carries edge flags and the same test passes against the pixel underneath (dstz/dstattr +          \
     under_off; only when under_off is non-zero), else 0. Returns (first<<16)|(last+1) of non-zero entries,   \
     0 if none. n may round up to a multiple of 4. */                                                         \
  u32  NS##depth_candidates(int mode, const s32* z, const u32* dstz, const u32* dstattr, u32 n, u8* pass, u32 under_off); \
  /* The same for a shadow polygon, steered by the stencil the shadow mask left: bit 0 names the top pixel,   \
     bit 1 the one underneath. pass[i] = 1 (| 4 when bit 1 is set: the top plot may also reach the layer     \
     under) where bit 0 is set and z passes the top; 2 where the pixel underneath takes it instead: bit 0     \
     unset (tested with the TOP pixel's attributes, as the hardware does), or bit 0 set, the top failed, bit  \
     1 set and the top carries edge flags (tested with its own attributes). The under layer only exists       \
     with under_off. */                                                                                       \
  u32  NS##depth_candidates_shadow(int mode, const s32* z, const u32* dstz, const u32* dstattr, const u8* stencil, u32 n, u8* pass, u32 under_off);

namespace ref { DS_KERNEL_LIST() }
#if DSPERATE_NEON
namespace neon { DS_KERNEL_LIST() }
namespace active = neon;
#else
namespace active = ref;
#endif

#undef DS_KERNEL_LIST

} // namespace ds::gpu::kern
