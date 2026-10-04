// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"

namespace ds::gpu {

// Everything a 2D engine's line renderer reads besides memory (palette, OAM,
// VRAM): the render-side registers after the line's journal writes and
// latches, taken by Engine2DRegs::capture(). Renderers draw from this alone,
// so a line can be drawn now (software) or recorded and drawn later.
struct LineRegs {
  u32 dispcnt;
  u16 bgcnt[4], bghofs[4], bgvofs[4];
  s32 ref_x[2], ref_y[2];            // BG2/BG3 reference point latched for this line
  s16 pa[2], pc[2];
  u16 bldcnt;
  u16 master_bright;
  u32 bg_mosaic_line, obj_mosaic_line;
  u8 win0[4], win1[4];               // x1, x2, y1, y2
  u8 wincnt[4];                      // WININ lo/hi, WINOUT lo/hi
  u8 win0_active, win1_active;       // bit0 inside the y range, bit1 x range carried in from the previous line
  u8 bg_mosaic_w, obj_mosaic_w;
  u8 eva, evb, evy;                  // clamped to 16
  u8 layer_enable, obj_enable;       // latched enables (DISPCNT bits 8-12)
  bool enabled, forced_blank;
  u8 screen;                         // 0 = top, 1 = bottom
  u32 pal_ver, oam_ver, oam_geom_ver;   // generations of the palette and OAM this line sees
};

} // namespace ds::gpu
