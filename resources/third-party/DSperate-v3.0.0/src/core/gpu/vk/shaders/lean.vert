#version 450
// The lean path: polygons as fans through the hardware rasteriser, one
// instance per polygon, at 1x with the pipeline's MSAA. Vertices sit on
// pixel corners so the samples measure the polygon's true area: the DS
// span covers columns [xstart, xend] inclusive and rows [ytop, ybot)
// exclusive, so a left vertex is on its pixel's left edge and a right one on
// its pixel's right edge.
#extension GL_GOOGLE_include_directive : require
#define DS_GLSL 1
#include "vk_layout.h"
layout(std430, binding = 0) readonly buffer Polys { GpuPoly polys[]; };
layout(std430, binding = 1) readonly buffer Verts { GpuVert verts[]; };
layout(push_constant) uniform PC { GpuFrame f; } pc;
layout(location = 0) flat out uint v_poly;
layout(location = 1) out vec3 v_rgb;                 // 9-bit vertex colour, perspective-correct
layout(location = 2) out vec2 v_st;                  // 12.4 texture coordinates, perspective-correct
layout(location = 3) noperspective out float v_z;    // DS z, linear across the screen (Z-buffer mode)
layout(location = 4) out float v_zp;                 // DS depth, perspective-correct (W-buffer mode)
void main() {
  uint pi = uint(gl_InstanceIndex);
  GpuPoly p = polys[pi];
  uint tri = uint(gl_VertexIndex) / 3u, k = uint(gl_VertexIndex) % 3u;
  if (tri + 2u >= p.nverts) { gl_Position = vec4(2.0, 2.0, 0.0, 1.0); v_poly = 0u; v_rgb = vec3(0); v_st = vec2(0); v_z = 0.0; v_zp = 0.0; return; }
  uint vi = k == 0u ? 0u : tri + k;
  GpuVert vt = verts[p.first_vert + vi];
  float w = max(float(vt.w), 1.0);
  float cx = 0.5 * float(p.xmin + p.xmax);
  float sxf = float(vt.sx), syf = float(vt.sy);
  float ox = sxf > cx ? 1.0 : 0.0, oy = 0.0;
  // A zero-width/height polygon is a line to the DS but zero area here:
  // give it one pixel via the second/third vertex.
  if (p.xmax == p.xmin) ox = (vi == 1u || vi == 2u) ? 1.0 : 0.0;
  if (p.ybot == p.ytop) oy = (vi == 1u || vi == 2u) ? 1.0 : 0.0;
  float x = (sxf + ox) / 256.0 * 2.0 - 1.0, y = (syf + oy) / 192.0 * 2.0 - 1.0;
  float z = clamp(float(vt.z), 0.0, 16777215.0);
  // Z-buffer: z/2^24 linear in screen space. W-buffer: vt.z is already the
  // per-polygon normalised depth the DS compares; 1/z interpolated
  // perspective-correct reproduces it, and the test runs GREATER.
  bool wbuf = (pc.f.flags & DS_FF_WBUFFER) != 0u;
  // Depth-equal polygons (POLYGON_ATTR bit 14: decals on a surface) pass
  // within a tolerance of the pixel's depth, +-0x200 Z-buffered, +-0xFF
  // W-buffered. Here: the polygon sits that much nearer and its pipeline
  // tests less-or-equal, so a decal at, or slightly behind, its surface
  // draws. (A decal well in front would pass too; the DS fails it. Rare.)
  float zd = z;
  if ((p.attr & 0x4000u) != 0u) zd = max(z - (wbuf ? 255.0 : 512.0), 0.0);
  float zc = wbuf ? (1.0 / max(zd, 1.0)) * w : (zd / 16777215.0) * w;
  gl_Position = vec4(x * w, y * w, zc, w);
  v_poly = pi;
  v_rgb = vec3(float(vt.r), float(vt.g), float(vt.b));
  v_st = vec2(float(vt.s), float(vt.t));
  v_z = z;
  v_zp = z;
}
