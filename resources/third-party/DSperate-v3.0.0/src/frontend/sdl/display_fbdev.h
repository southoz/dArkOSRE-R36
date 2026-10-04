// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Direct fbdev scanout through /dev/fb0, for devices whose SDL2 has no
// KMSDRM or Wayland driver (e.g. spruceOS mali-fbdev on Anbernic H700): its
// only path is Mali EGL over fbdev, which forces a blocking eglSwapBuffers
// even through SDL_GetWindowSurface or the software renderer. We write
// scanline frames straight into fb0 (triple-buffered) and present with
// FBIOPAN_DISPLAY; SDL is kept only for the window/events/pad. The pan
// itself blocks for the refresh on these drivers, so --no-vsync is a no-op.
// open() fails cleanly if fb0 isn't 32bpp ARGB8888 or can't hold two panel
// buffers, and Display falls back to SDL's own paths.
#pragma once

#include "core/types.h"
#include "scanout.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <thread>

#include <linux/fb.h>

namespace ds::sdl {

class FbdevOut : public ScanoutOut {
public:
  static constexpr int MAX_BUFS = 3;

  // True if /dev/fb0 opens read-write and describes a 32bpp panel. Cheap,
  // cached; safe to call before SDL_Init.
  static bool available();

  // Resizes `win` to fb0's panel size (window has no size of its own on the
  // headless driver). False leaves nothing changed.
  bool open(SDL_Window* win, bool vsync);

  bool reopen(SDL_Window* win, int w, int h) override;
  void close() override;

  int width() const override { return w_; }
  int height() const override { return h_; }
  int stride() const override { return stride_; }
  int bufs() const override { return bufs_; }
  int current() const override { return cur_; }

  u32* begin_frame() override;
  void end_frame() override;

private:
  void pan(int buf);
  void wait_vsync();
  void presenter();
  void fit_window(SDL_Window* win) const;
  u32* buf_ptr(int buf) const { return reinterpret_cast<u32*>(map_ + static_cast<size_t>(buf) * buf_bytes_); }

  int  fd_ = -1;
  u8*  map_ = nullptr;
  size_t map_len_ = 0;
  size_t buf_bytes_ = 0;            // one buffer: line_length * yres
  int  w_ = 0, h_ = 0, stride_ = 0; // stride in pixels
  int  bufs_ = 0;
  fb_var_screeninfo var_{};
  bool vsync_ = true;
  bool pan_blocks_ = true;          // FBIOPAN_DISPLAY waits for the refresh (measured once)
  bool pan_measured_ = false;
  bool waitforvsync_ = true;        // FBIO_WAITFORVSYNC works (until it fails once)
  u64  next_ns_ = 0;                // clock pacing when neither waits
  int  cur_ = -1;                   // buffer handed out by begin_frame()
  std::atomic<bool> dead_{false};

  // Presenter thread state (vsync only). Buffer indices; -1 = none.
  std::thread             thread_;
  std::mutex              mu_;
  std::condition_variable cv_;
  int  displayed_ = 0;              // on the panel now
  int  latched_ = -1;               // panned to, waiting for the refresh
  int  pending_ = -1;               // drawn, not yet panned
  bool stop_ = false;
};

} // namespace ds::sdl
