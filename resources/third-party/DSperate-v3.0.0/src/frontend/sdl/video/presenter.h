// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"
#include "frontend/video/layout.h"

#include <memory>
#include <string>

struct SDL_Window;

namespace ds::sdl {

class ScanoutOut;

// A presenter that takes the core's whole 256x192 frames and composes the
// output itself on the GPU: layout, scaling, rotation, LCD filters, the
// inset's alpha and the frontend's overlay. Display drives it in place of
// the CPU scanline scaler (the software presenter) when one opens.
class FramePresenter {
public:
  virtual ~FramePresenter() = default;

  struct View { int screen; frontend::Rect rect; bool shown; bool blends; bool grid = false; };   // grid: LCD grid applies to this view
  struct Params {
    int rot = 0;                 // presented = logical rotated by this
    int lw = 0, lh = 0;          // the logical (unrotated) frame the rects are laid out in
    u8 inset_alpha = 255;
    frontend::Rect drawn;        // where the frontend drew on overlay() this frame
    u32 grid = 256;              // LCD grid brightness on a seam, 0..256 (256 = off)
  };

  const std::string& name() const { return name_; }   // for the log: what presents, on which device
  // Which hardware presents: the menu greys out what a presenter cannot do
  // (the RGA scales bilinear only, and draws no grid, seams or chunky cells).
  enum class Kind : u8 { Vulkan, Rga };
  virtual Kind kind() const = 0;

  // The output is `w`x`h` presented pixels now. Same: nothing to do.
  // Lost: the presenter cannot continue and Display falls back to software.
  enum class Fit : u8 { Same, Changed, Lost };
  virtual Fit fit(SDL_Window* win, int w, int h) = 0;
  // One frame. False if nothing was shown.
  virtual bool present(const u32* const fb[2], const View* views, int nviews, const Params& p) = 0;
  // The frontend's overlay for the coming frame: `lw`x`lh`, pitch `lw`,
  // 0xAARRGGBB with alpha honoured. Null if unavailable.
  virtual u32* overlay(int lw, int lh) = 0;
  // Everything presented is on its way to the panel (a present with no frame following).
  virtual void flush() = 0;
  // The frame just presented goes to the sink now, once the GPU is done with
  // it, rather than at the start of the next present(). Unlike flush(), it
  // does not wait for the sink to put it on the panel.
  virtual void retire() {}

protected:
  std::string name_;
};

// The dma-buf import presenter (GpuPresent): Vulkan writes the scanout
// sink's own buffers (Wayland dma-buf or KMS), which the sink presents.
// `sink` stays owned by the caller and must outlive the presenter.
std::unique_ptr<FramePresenter> open_vk_import_presenter(ScanoutOut& sink, std::string* why);
// The RGA presenter (present_rga.cpp): the Rockchip 2D accelerator scales the
// frames into the sink's dma-buf through V4L2 mem2mem; no GPU involved.
std::unique_ptr<FramePresenter> open_rga_presenter(ScanoutOut& sink, std::string* why);

} // namespace ds::sdl
