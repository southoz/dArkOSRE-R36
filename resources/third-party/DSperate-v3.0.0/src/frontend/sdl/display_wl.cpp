// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "wl_dyn.h"             // must precede every wayland header
#include "display_wl.h"
#include "dmaheap.h"

#include "wl/wayland-client.h"
#include "wl/linux-dmabuf-v1.h"
#include "wl/xdg-shell.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace ds::sdl {

namespace {

constexpr u32 FMT_XRGB8888 = 0x34325258;   // 'XR24'; core writes 0xAARRGGBB, opaque

// LINEAR: plain row-major (what the CMA heap gives us). INVALID: "derive
// layout from the dmabuf". A compositor may accept one and not the other.
constexpr u64 MOD_LINEAR  = 0;
constexpr u64 MOD_INVALID = 0x00ffffffffffffffULL;

// Bound once per process, never destroyed: proxies we create can be
// referenced by events queued for other dispatchers (compositor sends
// wl_surface.enter to SDL's queue naming our wl_output), so tearing them
// down on close/reopen leaves a dangling proxy in a queue we don't control.
struct Globals {
  wl_display* dpy = nullptr;
  wl_event_queue* q = nullptr;
  wl_registry* reg = nullptr;
  zwp_linux_dmabuf_v1* dmabuf = nullptr;
  wl_compositor* comp = nullptr;
  wl_output* outputs[4] = {};
  int n_outputs = 0;
  bool tried = false;

  // What the compositor can import, gathered during the bind roundtrip.
  // v3 bind gets modifier events; older only format events (no layout info).
  bool any_format = false;        // at least one format event arrived
  bool any_modifier = false;      // at least one modifier event arrived
  bool xr24 = false;              // XR24 in a format event
  bool xr24_linear = false;       // XR24 + LINEAR in a modifier event
  bool xr24_implicit = false;     // XR24 + INVALID in a modifier event
  u64 mod = MOD_LINEAR;           // what alloc_buf() declares
  bool usable = false;            // the tier may run at all
};
Globals g_;

void on_global(void* data, wl_registry* reg, u32 name, const char* iface, u32 ver) {
  Globals* g = static_cast<Globals*>(data);
  if (!std::strcmp(iface, wl_output_interface.name)) {
    if (g->n_outputs < 4) {
      wl_output* o = static_cast<wl_output*>(wl_registry_bind(reg, name, &wl_output_interface, 1));
      wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(o), g->q);
      g->outputs[g->n_outputs++] = o;
    }
  } else if (!std::strcmp(iface, zwp_linux_dmabuf_v1_interface.name)) {
    // v3 create_params semantics are all we use; they are unchanged in v4.
    g->dmabuf = static_cast<zwp_linux_dmabuf_v1*>(
        wl_registry_bind(reg, name, &zwp_linux_dmabuf_v1_interface, ver < 3 ? ver : 3));
    wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(g->dmabuf), g->q);
  } else if (!std::strcmp(iface, wl_compositor_interface.name)) {
    g->comp = static_cast<wl_compositor*>(wl_registry_bind(reg, name, &wl_compositor_interface, 1));
    wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(g->comp), g->q);
  }
}
void on_global_remove(void*, wl_registry*, u32) {}
const wl_registry_listener reg_listener = { on_global, on_global_remove };

void on_dmabuf_format(void* data, zwp_linux_dmabuf_v1*, u32 format) {
  Globals* g = static_cast<Globals*>(data);
  g->any_format = true;
  if (format == FMT_XRGB8888) g->xr24 = true;
}

void on_dmabuf_modifier(void* data, zwp_linux_dmabuf_v1*, u32 format, u32 hi, u32 lo) {
  Globals* g = static_cast<Globals*>(data);
  g->any_modifier = true;
  if (format != FMT_XRGB8888) return;
  g->xr24 = true;
  const u64 m = (static_cast<u64>(hi) << 32) | lo;
  if (m == MOD_LINEAR) g->xr24_linear = true;
  else if (m == MOD_INVALID) g->xr24_implicit = true;
}

const zwp_linux_dmabuf_v1_listener dmabuf_listener = { on_dmabuf_format, on_dmabuf_modifier };

// Decide what to declare in the add request, or refuse the tier. `created`
// is not proof the buffer displays: wlroots validates params up front but
// imports at composite time, silently, so this checks the advertised table
// instead of trusting `created` (a late failure is a black screen).
bool pick_modifier(Globals* g) {
  static const bool verbose = std::getenv("DS_VERBOSE") != nullptr;
  if (g->any_modifier) {
    // v3 table is authoritative: what is not in it will not import.
    if (g->xr24_linear) {
      g->mod = MOD_LINEAR;
      if (verbose) std::fprintf(stderr, "dmabuf: XR24 linear\n");
      return true;
    }
    if (g->xr24_implicit) {
      g->mod = MOD_INVALID;
      if (verbose) std::fprintf(stderr, "dmabuf: XR24 implicit layout (compositor offers no linear)\n");
      return true;
    }
    std::fprintf(stderr, "dmabuf: compositor imports no XR24 layout we can produce%s\n",
                 g->xr24 ? " (XR24 offered, but neither LINEAR nor implicit)" : " (no XR24 at all)");
    return false;
  }
  if (g->any_format && !g->xr24) {
    std::fprintf(stderr, "dmabuf: compositor does not import XR24\n");
    return false;
  }
  // Pre-v3 or nothing advertised: no table to consult; declare LINEAR.
  g->mod = MOD_LINEAR;
  if (verbose) std::fprintf(stderr, "dmabuf: no modifier table; assuming XR24 linear\n");
  return true;
}

// Bind the globals on first use; idempotent, failure sticky for the session.
bool globals_init(wl_display* dpy) {
  if (g_.tried) return g_.dpy == dpy && g_.dmabuf && g_.usable;
  g_.tried = true;
  g_.dpy = dpy;
  g_.q = wl_display_create_queue(dpy);
  if (!g_.q) return false;
  g_.reg = wl_display_get_registry(dpy);
  // Move off SDL's default queue before any dispatch reaches SDL's handlers.
  wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(g_.reg), g_.q);
  wl_registry_add_listener(g_.reg, &reg_listener, &g_);
  wl_display_roundtrip_queue(dpy, g_.q);
  if (!g_.dmabuf) {
    std::fprintf(stderr, "dmabuf: compositor lacks zwp_linux_dmabuf_v1\n");
    return false;
  }
  // format/modifier events fire once on bind; second roundtrip collects them.
  zwp_linux_dmabuf_v1_add_listener(g_.dmabuf, &dmabuf_listener, &g_);
  wl_display_roundtrip_queue(dpy, g_.q);
  g_.usable = pick_modifier(&g_);
  return g_.usable;
}

} // namespace

void DmabufOut::on_release(void* data, wl_buffer*) { static_cast<Buf*>(data)->busy = false; }

namespace { const wl_buffer_listener buf_listener = { DmabufOut::on_release }; }

namespace {
// Non-immediate create: create_immed would turn a rejected buffer into a
// fatal protocol error, which is no way to probe heaps.
struct Created { wl_buffer* wb = nullptr; bool done = false; };
void on_created(void* d, zwp_linux_buffer_params_v1*, wl_buffer* wb) { auto* c = static_cast<Created*>(d); c->wb = wb; c->done = true; }
void on_failed(void* d, zwp_linux_buffer_params_v1*) { static_cast<Created*>(d)->done = true; }
const zwp_linux_buffer_params_v1_listener params_listener = { on_created, on_failed };
} // namespace

bool DmabufOut::alloc_buf(Buf& b) {
  const size_t bytes = static_cast<size_t>(w_) * h_ * 4;
  b.fd = dmaheap::alloc(bytes, [&](int fd) {
    zwp_linux_buffer_params_v1* p = zwp_linux_dmabuf_v1_create_params(g_.dmabuf);
    Created c;
    zwp_linux_buffer_params_v1_add_listener(p, &params_listener, &c);
    // plane 0, layout settled by pick_modifier() at bind time
    zwp_linux_buffer_params_v1_add(p, fd, 0, 0, w_ * 4,
                                   static_cast<u32>(g_.mod >> 32), static_cast<u32>(g_.mod));
    zwp_linux_buffer_params_v1_create(p, w_, h_, FMT_XRGB8888, 0);
    while (!c.done && wl_display_roundtrip_queue(dpy_, g_.q) >= 0) {}
    zwp_linux_buffer_params_v1_destroy(p);
    if (!c.wb) { std::fprintf(stderr, "dmabuf: compositor refused the buffer\n"); return false; }
    b.wb = c.wb;
    return true;
  }, "dmabuf");
  if (b.fd < 0) return false;
  b.bytes = bytes;
  void* m = mmap(nullptr, b.bytes, PROT_READ | PROT_WRITE, MAP_SHARED, b.fd, 0);
  if (m == MAP_FAILED) { std::perror("dmabuf: mmap"); return false; }
  b.px = static_cast<u32*>(m);
  std::memset(b.px, 0, b.bytes);
  wl_buffer_add_listener(b.wb, &buf_listener, &b);
  return true;
}

void DmabufOut::drop_buf(Buf& b) {
  if (b.wb) wl_buffer_destroy(b.wb);
  if (b.px) munmap(b.px, b.bytes);
  if (b.fd >= 0) ::close(b.fd);
  b = Buf{};
}

bool DmabufOut::open(SDL_Window* win, int w, int h, int output_index) {
  if (!wldyn::load()) { std::fprintf(stderr, "dmabuf: no libwayland (%s)\n", wldyn::error()); return false; }

  SDL_SysWMinfo wm;
  SDL_VERSION(&wm.version);
  if (!SDL_GetWindowWMInfo(win, &wm) || wm.subsystem != SDL_SYSWM_WAYLAND) {
    std::fprintf(stderr, "dmabuf: not a wayland window\n");
    return false;
  }
  output_index_ = output_index;
  dpy_ = static_cast<wl_display*>(wm.info.wl.display);
  surf_ = static_cast<wl_surface*>(wm.info.wl.surface);
  w_ = w; h_ = h;
  if (!globals_init(dpy_)) { dpy_ = nullptr; surf_ = nullptr; return false; }
  q_ = g_.q;

#if SDL_VERSION_ATLEAST(2, 0, 18)
  if (output_index >= 0) {
    if (output_index >= g_.n_outputs) {
      std::fprintf(stderr, "dmabuf: output %d of %d not present\n", output_index, g_.n_outputs);
      close();
      return false;
    }
    // Compositor answers with a configure carrying the output's size; the
    // caller's per-frame size check follows it.
    if (auto* tl = static_cast<xdg_toplevel*>(wm.info.wl.xdg_toplevel))
      xdg_toplevel_set_fullscreen(tl, g_.outputs[output_index]);
    else
      std::fprintf(stderr, "dmabuf: SDL exposes no xdg_toplevel; cannot target output %d\n", output_index);
  }
#else
  (void)output_index;
#endif

  for (int i = 0; i < nbufs_; ++i)
    if (!alloc_buf(bufs_[i])) { close(); return false; }
  wl_display_roundtrip_queue(dpy_, q_);   // surface any create_immed error now, not mid-game

  // Fullscreen+opaque are two of the three scanout conditions (third is the
  // compositor's untransformed output). SDL's format has alpha, so declare opacity ourselves.
  if (g_.comp) {
    wl_region* r = wl_compositor_create_region(g_.comp);
    wl_region_add(r, 0, 0, w_, h_);
    wl_surface_set_opaque_region(surf_, r);
    wl_region_destroy(r);
  }
  return true;
}

void DmabufOut::close() {
  // Only per-instance objects die here; globals live for the connection.
  for (Buf& b : bufs_) drop_buf(b);
  // Size 0: a sink whose reopen failed must not look usable at its new size.
  dpy_ = nullptr; surf_ = nullptr; q_ = nullptr; cur_ = -1; dead_ = false; w_ = h_ = 0;
}

bool DmabufOut::dmabuf_plane(int buf, DmabufPlane& out) const {
  if (buf < 0 || buf >= nbufs_ || bufs_[buf].fd < 0) return false;
  out.fd = bufs_[buf].fd; out.offset = 0; out.stride_bytes = static_cast<u32>(w_) * 4;
  out.width = static_cast<u32>(w_); out.height = static_cast<u32>(h_); out.fourcc = FMT_XRGB8888;
  return true;
}

u32* DmabufOut::begin_frame() {
  if (dead_ || !dpy_) return nullptr;
  for (;;) {
    wl_display_dispatch_queue_pending(dpy_, q_);
    for (int i = 0; i < nbufs_; ++i)
      if (!bufs_[i].busy) {
        cur_ = i;
        if (!gpu_writes_) dmaheap::sync_begin_write(bufs_[i].fd);
        return bufs_[i].px;
      }
    // All buffers pending: wait for a release (vsync pacing).
    wl_display_flush(dpy_);
    if (wl_display_dispatch_queue(dpy_, q_) < 0) {
      std::fprintf(stderr, "dmabuf: display error %d; falling back\n", wl_display_get_error(dpy_));
      dead_ = true;
      return nullptr;
    }
  }
}

void DmabufOut::end_frame() {
  if (dead_ || cur_ < 0) return;
  Buf& b = bufs_[cur_];
  if (!gpu_writes_) dmaheap::sync_end_write(b.fd);
  b.busy = true;
  wl_surface_attach(surf_, b.wb, 0, 0);
  wl_surface_damage(surf_, 0, 0, w_, h_);
  wl_surface_commit(surf_);
  wl_display_flush(dpy_);
  cur_ = -1;
}

} // namespace ds::sdl
