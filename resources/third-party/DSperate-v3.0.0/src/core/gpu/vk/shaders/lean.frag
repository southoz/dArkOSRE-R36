#version 450
// The lean path's one fragment stage: DS shading (ds_shade.glsl) once per
// pixel, the alpha test, fog by the pixel's own depth, and the colour out as
// raw 6-bit channels with the 5-bit alpha encoded so the pipeline's blend
// factors and MAX alpha reproduce the DS blend. Depth, stencil and blending
// are the pipeline's; nothing here reads the framebuffer.
#extension GL_GOOGLE_include_directive : require
#define DS_GLSL 1
#include "vk_layout.h"
layout(std430, binding = 0) readonly buffer Polys  { GpuPoly polys[]; };
#define DS_TEXEL_BUFFER 1
layout(binding = 2) uniform usamplerBuffer texels_tb;
layout(std430, binding = 3) readonly buffer Post   { GpuPost ps; };
layout(push_constant) uniform PC { GpuFrame f; } pc;
layout(location = 0) flat in uint v_poly;
layout(location = 1) in vec3 v_rgb;
layout(location = 2) in vec2 v_st;
layout(location = 3) noperspective in float v_z;
layout(location = 4) in float v_zp;
layout(location = 0) out vec4 o_col;
layout(location = 1) out float o_fog;   // the polygon's fog bit; blended with MIN so a pixel keeps fog only if every layer had it (the DS rule)
#ifdef DS_EARLY
// Polygons whose alpha cannot fail the test (no cut-out texture): no
// discard, so the depth test runs before shading and overdraw costs nothing.
layout(early_fragment_tests) in;
#endif
#include "ds_shade.glsl"

// The DS evaluates a pixel at its top-left corner; with vertices on pixel
// corners that is half a pixel before the centre, so attributes are read
// there. On an edge pixel the corner can lie outside the polygon and the
// value is an extrapolation, so texcoords are clamped to the polygon's own
// range (row_base / pad_ carry it: s min|max, t min|max as 16-bit pairs)
// and colours to their range, never a texel past the polygon.
vec2 st_bounds_lo(GpuPoly p) { return vec2(float(int(p.row_base & 0xFFFFu) - 32768), float(int(p.pad_ & 0xFFFFu) - 32768)); }
vec2 st_bounds_hi(GpuPoly p) { return vec2(float(int(p.row_base >> 16) - 32768), float(int(p.pad_ >> 16) - 32768)); }

void main() {
  GpuPoly p = polys[v_poly];
  vec3 rgb = clamp(interpolateAtOffset(v_rgb, vec2(-0.5)), vec3(0.0), vec3(511.0));
#ifdef DS_CENTROID
  vec2 st = clamp(interpolateAtCentroid(v_st), st_bounds_lo(p), st_bounds_hi(p));
#else
  vec2 st = clamp(interpolateAtOffset(v_st, vec2(-0.5)), st_bounds_lo(p), st_bounds_hi(p));
#endif
  uint blendmode = (p.attr >> 4) & 3u;
  uint polyalpha = (p.attr >> 16) & 0x1Fu;
  bool textured = (p.flags & DS_PF_TEXTURED) != 0u;
  // Texcoords truncate as the DS 12.4 >> 4 does; +0.01 absorbs float error.
  uint src = shade_pixel(p, blendmode, polyalpha, textured, int(round(rgb.r)), int(round(rgb.g)), int(round(rgb.b)), int(floor(st.x + 0.01)), int(floor(st.y + 0.01)));
  uint alpha = src >> 24;
#ifndef DS_EARLY
  if (alpha <= pc.f.alpha_ref) discard;
#endif
  // A translucent polygon's pixels are opaque where their final alpha is 31
  // (written with depth, no translucent flag, so a later polygon of the
  // same id draws over them) and translucent elsewhere. The run is drawn
  // twice: DS_ALPHA_SEL 1 keeps the opaque pixels, 2 the translucent ones.
#if DS_ALPHA_SEL == 1
  if (alpha != 31u) discard;
#elif DS_ALPHA_SEL == 2
  if (alpha == 31u) discard;
#endif
  uint r = src & 0x3Fu, g = (src >> 8) & 0x3Fu, b = (src >> 16) & 0x3Fu;
  // Fog is the resolve's (once per pixel by the pixel's depth, as the DS does
  // after blending); the polygon's fog bit goes to the fog attachment.
  uint fogbit = ((pc.f.dispcnt & 128u) != 0u && (p.attr & 0x8000u) != 0u) ? 1u : 0u;
  // Alpha byte: 0 stays 0 (undrawn), else (a + 1) * 8: the blend's SRC_ALPHA is (a + 1) / 32 to within 1/256.
  uint ab = alpha == 0u ? 0u : min(255u, (alpha + 1u) * 8u);
  o_col = vec4(float(r), float(g), float(b), float(ab)) / 255.0;
  o_fog = float(fogbit);
}
