// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Dmabuf presentation onto SDL's own Wayland window: frames render straight
// into CMA dma-heap buffers submitted via zwp_linux_dmabuf_v1, avoiding the
// shm copy and texture upload of SDL's window surface path. The compositor
// samples zero-copy, or (fullscreen/opaque/untransformed) scans out directly.
//
// SDL keeps the window, xdg-shell, fullscreen and input; we take wl_display/
// wl_surface via SDL_SysWMinfo and use a private event queue so our
// dispatching and SDL's event pump never touch each other's handlers.
//
// libwayland is dlopen'd (wl_dyn.h); open() fails cleanly if unavailable.
#pragma once

#include "core/types.h"
#include "scanout.h"

#include <cstddef>

struct SDL_Window;
struct wl_display;
struct wl_surface;
struct wl_event_queue;
struct wl_registry;
struct wl_compositor;
struct wl_buffer;
struct wl_output;
struct zwp_linux_dmabuf_v1;
struct xdg_toplevel;

namespace ds::sdl {

class DmabufOut : public ScanoutOut {
public:
  static constexpr int BUFS = 5;           // nbufs_ is the count in use; 5 needed under GPU present
  static constexpr int DEFAULT_BUFS = 4;   // two frames of run-ahead

  // False if any precondition is missing; caller falls back to another path.
  // w/h is the buffer size in pixels.
  //
  // `output_index` >= 0 asks the compositor to fullscreen on that output
  // (registry order): Wayland has no client-side window positions, so this
  // must go through the window's xdg_toplevel rather than SDL.
  bool open(SDL_Window* win, int w, int h, int output_index = -1);

  bool reopen(SDL_Window* win, int w, int h) override { const int o = output_index_; close(); return open(win, w, h, o); }
  void close() override;

  int width() const override { return w_; }
  int height() const override { return h_; }
  int bufs() const override { return nbufs_; }
  // Before open(): how many buffers to rotate, up to BUFS.
  void set_bufs(int n) { nbufs_ = n < 2 ? 2 : n > BUFS ? BUFS : n; }
  int current() const override { return cur_; }

  // Pixels of a free buffer (blocks on the compositor if all pending: vsync).
  // Null on protocol error; caller falls back.
  u32* begin_frame() override;
  bool dmabuf_plane(int buf, DmabufPlane& out) const override;
  void set_gpu_writes(bool on) override { gpu_writes_ = on; }
  void end_frame() override;       // attach + damage + commit + flush

  // Public for the C listener table; not part of the interface.
  static void on_release(void* data, struct wl_buffer* wb);

private:
  struct Buf {
    int fd = -1;
    u32* px = nullptr;
    size_t bytes = 0;
    struct wl_buffer* wb = nullptr;
    bool busy = false;
  };
  bool alloc_buf(Buf& b);
  void drop_buf(Buf& b);

  struct wl_display* dpy_ = nullptr;      // SDL's; not ours to destroy
  struct wl_surface* surf_ = nullptr;     // SDL's; not ours to destroy
  struct wl_event_queue* q_ = nullptr;
  Buf bufs_[BUFS];
  int nbufs_ = DEFAULT_BUFS;
  bool gpu_writes_ = false;
  int cur_ = -1;
  int w_ = 0, h_ = 0;
  int output_index_ = -1;                 // saved open() arg, for reopen()
  bool dead_ = false;                     // protocol error; stop submitting
};

} // namespace ds::sdl
