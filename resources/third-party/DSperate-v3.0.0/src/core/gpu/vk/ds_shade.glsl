// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#ifndef DS_SHADE_GLSL
#define DS_SHADE_GLSL
// Texture sampling, colour combine, alpha blend, shared by the resolve pass
// and raster's ordered tail. Includer must declare Texels/Post and `pc` first.

// ---- texture ---------------------------------------------------------------

// Texel: 16-bit RGB555 in low half, 5-bit alpha above.
uint sample_tex(GpuPoly p, int s, int t) {
  int w = int(p.tex_w), h = int(p.tex_h);
  s >>= 4; t >>= 4;
  bool rep_s = (p.texparam & 0x10000u) != 0u, rep_t = (p.texparam & 0x20000u) != 0u;
  bool flp_s = (p.texparam & 0x40000u) != 0u, flp_t = (p.texparam & 0x80000u) != 0u;
  if (rep_s) { if (flp_s && (s & w) != 0) s = (w - 1) - (s & (w - 1)); else s &= w - 1; }
  else       s = clamp(s, 0, w - 1);
  if (rep_t) { if (flp_t && (t & h) != 0) t = (h - 1) - (t & (h - 1)); else t &= h - 1; }
  else       t = clamp(t, 0, h - 1);
#ifdef DS_TEXEL_BUFFER
  return texelFetch(texels_tb, int(p.tex_offset + uint(t * w + s))).r;
#else
  return texels[p.tex_offset + uint(t * w + s)];
#endif
}

// 15-bit channel -> 6 bits: bits 1-5 of the field, +1 when non-zero.
uint c15_to_18(uint c, uint shift) {
  uint v = ((shift == 0u ? (c << 1) : (c >> shift)) & 0x3Eu);
  return v != 0u ? v + 1u : v;
}

// ---- the pixel -------------------------------------------------------------

uint shade_pixel(GpuPoly p, uint blendmode, uint polyalpha, bool textured,
                 int vr9, int vg9, int vb9, int s, int t) {
  uint vr = uint(vr9 >> 3) & 0x3Fu, vg = uint(vg9 >> 3) & 0x3Fu, vb = uint(vb9 >> 3) & 0x3Fu;
  uint r, g, b, a;
  // Mode 2 = toon or highlight (DISP3DCNT bit 1). Toon replaces vertex colour
  // via the 32-entry table; highlight greys to its own red and ADDS the table
  // entry after texture combine (below); `vr` stays untouched for both lookups.
  bool highlight = (pc.f.dispcnt & 2u) != 0u;
  if (blendmode == 2u) {
    if (highlight) { vg = vr; vb = vr; }
    else {
      uint tc = ps.toon[vr >> 1];
      vr = c15_to_18(tc, 0u); vg = c15_to_18(tc, 4u); vb = c15_to_18(tc, 9u);
    }
  }
  if (textured) {
    uint packed = sample_tex(p, s, t);
    uint tcol = packed & 0xFFFFu, ta = (packed >> 16) & 0x1Fu;
    uint tr = c15_to_18(tcol, 0u), tg = c15_to_18(tcol, 4u), tb = c15_to_18(tcol, 9u);
    if ((blendmode & 1u) != 0u) {          // decal
      if (ta == 0u)       { r = vr; g = vg; b = vb; }
      else if (ta == 31u) { r = tr; g = tg; b = tb; }
      else {
        r = ((tr * ta) + (vr * (31u - ta))) >> 5;
        g = ((tg * ta) + (vg * (31u - ta))) >> 5;
        b = ((tb * ta) + (vb * (31u - ta))) >> 5;
      }
      a = polyalpha;
    } else {                                // modulate
      r = ((tr + 1u) * (vr + 1u) - 1u) >> 6;
      g = ((tg + 1u) * (vg + 1u) - 1u) >> 6;
      b = ((tb + 1u) * (vb + 1u) - 1u) >> 6;
      a = ((ta + 1u) * (polyalpha + 1u) - 1u) >> 5;
    }
  } else {
    r = vr; g = vg; b = vb; a = polyalpha;
  }
  if (blendmode == 2u && highlight) {
    uint tc = ps.toon[vr >> 1];
    r = min(r + c15_to_18(tc, 0u), 63u);
    g = min(g + c15_to_18(tc, 4u), 63u);
    b = min(b + c15_to_18(tc, 9u), 63u);
  }
  return r | (g << 8) | (b << 16) | (a << 24);
}

uint alpha_blend(uint dispcnt, uint src, uint dst, uint alpha) {
  uint dsta = dst >> 24;
  if (dsta == 0u) return src;
  uint r = src & 0x3Fu, g = (src >> 8) & 0x3Fu, b = (src >> 16) & 0x3Fu;
  if ((dispcnt & 8u) != 0u) {
    uint a1 = alpha + 1u;
    r = ((r * a1) + ((dst & 0x3Fu) * (32u - a1))) >> 5;
    g = ((g * a1) + (((dst >> 8) & 0x3Fu) * (32u - a1))) >> 5;
    b = ((b * a1) + (((dst >> 16) & 0x3Fu) * (32u - a1))) >> 5;
  }
  if (alpha > dsta) dsta = alpha;
  return r | (g << 8) | (b << 16) | (dsta << 24);
}


#endif // DS_SHADE_GLSL
