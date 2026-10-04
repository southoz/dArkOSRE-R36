// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Interface the scanline path presents through when not writing into SDL's
// window surface (DmabufOut/display_wl.h, DrmOut/display_drm.h): both hand
// out a CMA dma-heap buffer per frame and present it without a copy.
// begin_frame() blocks (buffer release / previous flip); end_frame() does not
// -- so display pacing is felt before emulation, where the loop has slack.
#pragma once

#include "core/types.h"

struct SDL_Window;

namespace ds::sdl {

class ScanoutOut {
public:
  virtual ~ScanoutOut() = default;

  // Re-establish at a new size after a configure/mode change. False: object
  // stays closed and the caller drops the tier.
  virtual bool reopen(SDL_Window* win, int w, int h) = 0;
  virtual void close() = 0;

  virtual int width() const = 0;
  virtual int height() const = 0;
  virtual int stride() const { return width(); }   // pixels; > width if padded (fbdev)
  // Buffer count, and index begin_frame() last handed out (0..bufs()-1, -1
  // outside a frame). Buffers are not round-robin -- lowest free is taken --
  // so a caller touching every buffer once keys off the index, not a frame count.
  virtual int bufs() const = 0;
  virtual int current() const = 0;

  virtual u32* begin_frame() = 0;   // free buffer's pixels, or null on protocol/driver error (caller falls back)
  virtual void end_frame() = 0;
  // Block until the last end_frame() is on its way to the panel. Needed by a
  // caller that presents once and stops (pause menu), since a tier may defer
  // its work to the next begin_frame() (DrmOut queues a flip). No-op elsewhere.
  virtual void flush() {}

  // dma-buf behind buffer `buf`, for a GPU present stage that imports and
  // writes the tier's buffers directly (no sync ioctl needed on read). Fd
  // stays owned by the tier; importer dup()s it. False if the tier has no dma-buf (fbdev).
  struct DmabufPlane { int fd = -1; u32 offset = 0, stride_bytes = 0, width = 0, height = 0; u32 fourcc = 0; };
  virtual bool dmabuf_plane(int buf, DmabufPlane& out) const { (void)buf; (void)out; return false; }
  virtual void set_gpu_writes(bool on) { (void)on; }   // skip the CPU-write cache sync once the GPU owns writes
};

} // namespace ds::sdl
