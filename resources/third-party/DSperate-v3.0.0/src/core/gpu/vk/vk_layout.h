// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#ifndef DS_VK_LAYOUT_H
#define DS_VK_LAYOUT_H
// Host/shader interface for the GPU raster. Included by C++ and, with
// DS_GLSL, textually by the shaders, so a field can't drift between the two.
// std430 layout: every member 4 bytes, every struct a multiple of 16.

#ifndef DS_GLSL
#include "core/types.h"
namespace ds::gpu::vk {
using uint = u32;
#define DS_INT s32
#else
#define DS_INT int
#endif

// Tiles. One workgroup per tile, one invocation per pixel: W*H must stay
// inside the GPU's invocation limit; narrow tiles bin fewer polygons per pixel loop.
#define DS_TILE_W 8
#define DS_TILE_H (256 / DS_TILE_W)
#define DS_TILES_X (256 / DS_TILE_W)
#define DS_TILES_Y (192 / DS_TILE_H)
#define DS_TILE_COUNT (DS_TILES_X * DS_TILES_Y)
// Overflow is recorded, not dropped: binning sets it and the frame falls back to the CPU raster.
#define DS_TILE_POLYS 512

// Hardware limits, so buffers size once at startup. Clipped polygon: max 10 vertices.
#define DS_MAX_POLYS 2048
#define DS_MAX_VERTS (DS_MAX_POLYS * 10)

// Polygon flags.
#define DS_PF_TRANSLUCENT 0x0001u
#define DS_PF_WBUFFER     0x0002u
#define DS_PF_FRONTFACING 0x0004u
#define DS_PF_TEXTURED    0x0008u
#define DS_PF_SHADOW_MASK 0x0010u
#define DS_PF_SHADOW      0x0020u
// Texture can produce a 0-alpha texel (colour-0-transparent, compressed
// transparent index, or direct-colour alpha bit); without it every texel is
// alpha 31 and needs no sample for the alpha test.
#define DS_PF_TEX_ALPHA   0x0040u

// Shadow mask RUN INDEX, per polygon per scanline. The DS clears the stencil
// per-scanline at the start of a mask run, but a tiled raster can't see that
// (the polygon ending a run may never reach this tile). Numbering runs
// per-scanline is tile-independent: a pixel clears when the run it sees
// differs from the one it saw last.
#define DS_SHRUN_LINES 192

// One screen-space vertex, after viewport transform and clipping. Flattened
// per polygon, not shared: the raster wants position/depth/attrs together. 48 bytes.
struct GpuVert {
  DS_INT sx, sy;        // screen position
  DS_INT z, w;          // depth and normalised W for this polygon's slot
  DS_INT r, g, b;       // 9-bit vertex colour; the raster narrows to 6 with >> 3
  DS_INT s, t;          // 12.4 texture coordinates
  uint   pad_[3];
};

// One polygon. 64 bytes.
struct GpuPoly {
  uint   first_vert;    // index into the vertex buffer; slot i is first_vert + i
  uint   nverts;
  uint   attr;          // POLYGON_ATTR as the hardware holds it
  uint   texparam;      // TEXIMAGE_PARAM: size, repeat and flip bits
  uint   tex_offset;    // word offset of the decoded texture in the arena
  uint   tex_w, tex_h;  // decoded texture dimensions, texels
  uint   flags;         // DS_PF_*
  DS_INT ytop, ybot;    // screen scanline range, inclusive
  uint   vtop, vbot;    // slots of the top and bottom vertices: where the edge chains start and end
  DS_INT xmin, xmax;    // screen x bounds, for the binning pass
  uint   row_base;      // this polygon's first row in the span table
  uint   pad_;
};

// One scanline of one polygon (the Y stage's output), computed once per
// (polygon, scanline) by the span pass. 16 words; endpoint attributes pack
// two apiece (3x9-bit colour into 10:10:10, two 12.4 texcoords into 16:16).
struct GpuRow {
  DS_INT xstart, xend, lim0, lim1;
  DS_INT wl, wr, zl, zr;
  uint   lrgb, lst, rrgb, rst;
  DS_INT xrz;           // the X stage's (1<<22)/xdiff
  uint   rcp;           // and its reciprocal-of-xdiff
  uint   fl;            // 1 valid, 2 l_fill, 4 r_fill, 8 w-buffer, 16 linear, 5-8 yedge, 512 mask, 1024 shadow, 2048/4096 left/right edge runs down-left (smooth filter)
  uint   poly;          // the polygon this row belongs to, so a pass driven by rows can find it
  uint   lcov, rcov;    // AA coverage of the left/right edge runs (bit 31: X-major, start << 12 | increment)
};

// Visibility pass workgroup: DS_VIS_ROWS rows, a DS_VIS_LANES-lane subgroup
// per row. 8 lanes matches a typical GPU subgroup so rows never diverge.
#define DS_VIS_ROWS  8
#define DS_VIS_LANES 8

// Span table rows a frame needs (sum of polygons' clamped heights); refused,
// not truncated, if it wants more. Worst case 2048*192 falls back to the CPU.
#define DS_MAX_SPAN_ROWS 49152

// Per-frame constants; pushed, not a buffer (changes every frame, small).
struct GpuFrame {
  uint   npoly;
  uint   scale;         // internal resolution multiplier (1 for P1)
  uint   dispcnt;       // DISP3DCNT: bit 3 is the blend enable the alpha blend reads
  uint   alpha_ref;     // already zeroed by the engine when DISP3DCNT bit 2 is clear
  uint   clear_color;   // the record clear_line() fills: RGB666 + alpha << 24
  uint   clear_depth;
  uint   clear_attr;    // polygon id in bits 24-29, fog bit 15
  uint   tile_y0;       // first tile row of this dispatch (banded dispatch)
  // ORDER-FREE PREFIX: [0, first_ordered) are opaque polygons, whose pixel
  // owner is a (z, back-facing, list index) minimum so order doesn't matter;
  // [first_ordered, npoly) is the ordered tail. opaque_rows is the prefix's span-table size.
  uint   first_ordered;
  uint   opaque_rows;
  uint   nrows;         // span rows this frame, all polygons: the span pass is one lane per row
  uint   flags;         // DS_FF_*
  uint   flags2;        // DS_FF2_* (flags is full: bits 16-31 are the shadow run)
};

#define DS_FF_WBUFFER 0x1u   // frame depth-tests on W (SWAP_BUFFERS bit 1); triangle path picks pipelines by it
#define DS_FF_FACE_BACK  0x4u  // triangle path opaque draw, back-facing only
#define DS_FF_FACE_FRONT 0x8u  // ... front-facing only, drawn second, LESS_OR_EQUAL (wins ties over opaque back-facing)
#define DS_FF_RUN_SHIFT 16u  // bits 16-31: shadow-mask run this draw belongs to
#define DS_FF_ROWS     0x2000u // span table holds this frame's rows (edge flags/coverage for edge marking, fast AA)
#define DS_FF_AAFAST   0x4000u // post.comp: fast AA, edge pixels blended with outside neighbour, two stages
#define DS_FF_POST2    0x8000u // post.comp: stage 2 of fast AA (stage 1 wrote fogged/marked pixels to the scratch plane)
// Smooth-3D present filter: fast AA's stage 2 writes each pixel's edge
// record to the edge plane instead of blending; present stage splits it.
#define DS_FF2_SMOOTH  0x1u
#define DS_FF2_MSAA    0x2u   // 4x MSAA: vertices on pixel corners, so coverage is the polygon's true area

// Render state too large for push constants (fog table, edge colours, toon
// table). Uploaded every frame, 320 bytes, cheaper than tracking changes.
struct GpuPost {
  uint fog_color;       // RenderState::fog_color: 15-bit colour in 0-14, 5-bit alpha in 16-20
  uint fog_offset;
  uint fog_shift;
  uint pad0_;
  uint density[34];     // RenderState::fog_density, 34 entries: index 32 is the last, 33 its repeat
  uint pad1_[2];
  uint edge[8];         // RenderState::edge, 15-bit colours indexed by polygon id >> 3
  uint toon[32];        // RenderState::toon, 15-bit colours indexed by the vertex red >> 1
};

// The binning pass's output: a polygon list per tile, plus a counter.
// `overflow` is set when any tile exceeded DS_TILE_POLYS.
struct GpuTiles {
  uint count[DS_TILE_COUNT];
  uint overflow;
  uint pad_[3];
  uint list[DS_TILE_COUNT * DS_TILE_POLYS];
  uint bias[DS_TILE_COUNT * DS_TILE_POLYS];
  uint yrng[DS_TILE_COUNT * DS_TILE_POLYS];
};

#ifndef DS_GLSL
#undef DS_INT
} // namespace ds::gpu::vk
#else
#undef DS_INT
#endif

#endif // DS_VK_LAYOUT_H
