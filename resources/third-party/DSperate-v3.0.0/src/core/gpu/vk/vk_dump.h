// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
// A recorded GPU frame: one frame's polygon list in the raster's own layout
// (vk_layout.h), written by the headless --dump-gpu-frame and read by the
// vk_probe tool, so the raster can be timed and eyeballed on a device without
// the emulator around it. All little-endian, host layout of the structs.
#include "core/gpu/vk/vk_layout.h"

namespace ds::gpu::vk {

struct GpuDumpHeader {
  static constexpr u32 kMagic = 0x44475046;   // "FPGD"
  u32 magic = kMagic, version = 1;
  u32 npoly = 0, nvert = 0, ntexels = 0, nrows = 0;   // array lengths that follow, in entries
  u64 frame = 0;                                        // NDS frame number
  GpuFrame f{};
  GpuPost post{};
};
// File body after the header: GpuPoly[npoly], GpuVert[nvert], u32 texels[ntexels],
// u32 shrun[npoly * DS_SHRUN_LINES], u32 rowpoly[nrows].

} // namespace ds::gpu::vk
