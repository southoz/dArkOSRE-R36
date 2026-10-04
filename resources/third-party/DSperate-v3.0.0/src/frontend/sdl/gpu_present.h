// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// GPU present stage: the scanout tier's CMA buffers are imported into Vulkan
// as LINEAR images, and one compute dispatch per frame lays out, scales,
// rotates and blends the core's two 256x192 frames straight into the buffer
// the tier presents, replacing the CPU scanline scaler.
//
// Frames are pipelined one deep: present() submits frame N and returns; the
// fence wait and tier end_frame() happen at the start of frame N+1 (or in
// flush()), so the emulation thread stalls only if more than a frame behind.
#pragma once

#include "core/types.h"
#include "frontend/sdl/scanout.h"

#include <SDL.h>
#include <memory>
#include <string>

namespace ds::sdl {

class GpuPresent {
public:
  // Imports every buffer of `out`; null (with a reason) if unavailable.
  static std::unique_ptr<GpuPresent> open(ScanoutOut& out, std::string* why);
  ~GpuPresent();
  GpuPresent(const GpuPresent&) = delete;
  GpuPresent& operator=(const GpuPresent&) = delete;

  // Tier reopened at a new size: drop and re-import its buffers.
  bool reimport(ScanoutOut& out);

  struct View { int screen; SDL_Rect rect; bool shown; bool blends; bool grid = false; };   // grid: LCD grid applies to this view
  // One frame: take a buffer from the tier, upload fb[0..1], dispatch,
  // submit. `lw`x`lh` is the logical (unrotated) frame the rects are laid
  // out in; `rot` maps it onto the presented buffer. False if nothing shown.
  // `drawn`: rectangle the frontend's overlay drew this frame (see overlay()).
  // `grid`: LCD grid brightness on a seam, 0..256 (256 = off).
  bool present(ScanoutOut& out, const u32* const fb[2], const View* views, int nviews, int rot, int lw, int lh, u8 inset_alpha,
               SDL_Rect drawn = SDL_Rect{0, 0, 0, 0}, u32 grid = 256);
  // Canvas for the frontend's overlays (OSD, pause menu, notices) for the
  // coming frame: `lw`x`lh`, pitch `lw`, 0xAARRGGBB, alpha honoured,
  // host-cached. Pre-cleared where the frame before last drew.
  u32* overlay(int lw, int lh);
  // Wait for the frame in flight and hand its buffer to the tier.
  void flush(ScanoutOut& out);

  const std::string& device_name() const;

private:
  GpuPresent() = default;
  struct Impl;
  std::unique_ptr<Impl> d_;
};

} // namespace ds::sdl
