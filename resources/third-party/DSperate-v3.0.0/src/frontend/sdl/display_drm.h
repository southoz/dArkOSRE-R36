// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Direct KMS scanout: the KMSDRM counterpart of the Wayland dmabuf tier.
// SDL2's KMSDRM backend has no window framebuffer -- every SDL presentation
// path there secretly routes through a hidden GLES renderer and a blocking
// eglSwapBuffers. Instead we write scanline frames straight into CMA
// dma-heap buffers, import them as DRM framebuffers, and page-flip onto the
// CRTC directly (SDL keeps the window/input/DRM master; we borrow its fd via
// SDL_SysWMinfo). No libdrm/kernel DRM headers needed: ioctls go through
// drm_uapi.h. open() fails cleanly if unsupported, and Display falls back to
// SDL's window surface.
#pragma once

#include "core/types.h"
#include "scanout.h"

#include <cstddef>

namespace ds::sdl {

class DrmOut : public ScanoutOut {
public:
  // One on screen, one flip pending (only one outstanding per CRTC), one
  // queued behind it. begin_frame() blocks only when all three are taken.
  static constexpr int BUFS = 4;   // array size; nbufs_ is the count in use (default 3, 4 under GPU present)
  static constexpr int DEFAULT_BUFS = 3;

  // False if any precondition is missing (wrong video driver, no usable
  // connector, window size doesn't match a panel mode, allocation failed).
  // `display_index` is SDL's: KMSDRM enumerates one display per connected
  // connector in DRM resource order.
  bool open(SDL_Window* win, int w, int h, int display_index);

  bool reopen(SDL_Window* win, int w, int h) override { const int d = display_; close(); return open(win, w, h, d); }
  void close() override;

  int width() const override { return w_; }
  int height() const override { return h_; }
  int bufs() const override { return nbufs_; }
  void set_bufs(int n) { nbufs_ = n < 2 ? 2 : n > BUFS ? BUFS : n; }
  int current() const override { return cur_; }

  // Waits for a flip to retire only when no buffer is free; taken here
  // rather than end_frame() so the wait overlaps emulation, not the present.
  u32* begin_frame() override;
  bool dmabuf_plane(int buf, DmabufPlane& out) const override;
  void set_gpu_writes(bool on) override { gpu_writes_ = on; }
  void end_frame() override;      // flip now, or queue behind the pending flip; does not wait
  void flush() override;          // wait until no flip is queued behind a pending one

private:
  struct Buf {
    int fd = -1;                  // dma-heap buffer
    u32* px = nullptr;
    size_t bytes = 0;
    u32 handle = 0;               // GEM handle from the PRIME import
    u32 fb = 0;                   // DRM framebuffer id
    bool busy = false;            // on screen, flip pending, or queued
  };
  bool flip(int i);               // issue the page flip for bufs_[i]; false = driver error
  bool alloc_buf(Buf& b);
  void drop_buf(Buf& b);
  // Reads events on the shared DRM fd and routes each to its owning DrmOut
  // (a dual-window layout has one instance per panel). `block` waits for one.
  static bool pump(int fd, bool block);
  void retire();                  // our flip completed

  int fd_ = -1;                   // SDL's DRM fd; not ours to close
  u32 crtc_ = 0, conn_ = 0;
  int display_ = 0;
  int w_ = 0, h_ = 0;
  Buf bufs_[BUFS];
  int nbufs_ = DEFAULT_BUFS;
  bool gpu_writes_ = false;
  int cur_ = -1;                  // buffer handed out by begin_frame()
  int on_screen_ = -1;            // buffer the CRTC is scanning out
  int pending_ = -1;              // buffer whose flip has not completed
  int queued_ = -1;               // buffer drawn while a flip was pending; flipped on its retire
  bool dead_ = false;             // driver error; stop submitting
};

} // namespace ds::sdl
