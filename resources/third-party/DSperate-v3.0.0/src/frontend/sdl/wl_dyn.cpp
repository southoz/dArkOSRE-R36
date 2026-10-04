// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "wl_dyn.h"

#include <dlfcn.h>

extern "C" {
struct wl_proxy* (*p_wl_proxy_marshal_flags)(struct wl_proxy*, uint32_t, const struct wl_interface*, uint32_t, uint32_t, ...);
struct wl_proxy* (*p_wl_proxy_marshal_array_flags)(struct wl_proxy*, uint32_t, const struct wl_interface*, uint32_t, uint32_t, union wl_argument*);
int              (*p_wl_proxy_add_listener)(struct wl_proxy*, void (**)(void), void*);
void             (*p_wl_proxy_destroy)(struct wl_proxy*);
uint32_t         (*p_wl_proxy_get_version)(struct wl_proxy*);
void             (*p_wl_proxy_set_queue)(struct wl_proxy*, struct wl_event_queue*);
struct wl_event_queue* (*p_wl_display_create_queue)(struct wl_display*);
void             (*p_wl_event_queue_destroy)(struct wl_event_queue*);
int              (*p_wl_display_roundtrip_queue)(struct wl_display*, struct wl_event_queue*);
int              (*p_wl_display_dispatch_queue)(struct wl_display*, struct wl_event_queue*);
int              (*p_wl_display_dispatch_queue_pending)(struct wl_display*, struct wl_event_queue*);
int              (*p_wl_display_flush)(struct wl_display*);
int              (*p_wl_display_get_error)(struct wl_display*);
}

namespace ds::sdl::wldyn {

namespace {
const char* err_ = nullptr;
enum class State { Untried, Ok, Failed } state_ = State::Untried;
}

const char* error() { return err_ ? err_ : "not attempted"; }

bool load() {
  if (state_ != State::Untried) return state_ == State::Ok;
  state_ = State::Failed;
  // The soname, not the dev symlink: devices ship the runtime only.
  void* h = dlopen("libwayland-client.so.0", RTLD_NOW | RTLD_GLOBAL);
  if (!h) { err_ = dlerror(); return false; }
  struct Sym { const char* name; void** slot; };
  const Sym syms[] = {
    {"wl_proxy_marshal_flags", reinterpret_cast<void**>(&p_wl_proxy_marshal_flags)},
    {"wl_proxy_marshal_array_flags", reinterpret_cast<void**>(&p_wl_proxy_marshal_array_flags)},
    {"wl_proxy_add_listener", reinterpret_cast<void**>(&p_wl_proxy_add_listener)},
    {"wl_proxy_destroy", reinterpret_cast<void**>(&p_wl_proxy_destroy)},
    {"wl_proxy_get_version", reinterpret_cast<void**>(&p_wl_proxy_get_version)},
    {"wl_proxy_set_queue", reinterpret_cast<void**>(&p_wl_proxy_set_queue)},
    {"wl_display_create_queue", reinterpret_cast<void**>(&p_wl_display_create_queue)},
    {"wl_event_queue_destroy", reinterpret_cast<void**>(&p_wl_event_queue_destroy)},
    {"wl_display_roundtrip_queue", reinterpret_cast<void**>(&p_wl_display_roundtrip_queue)},
    {"wl_display_dispatch_queue", reinterpret_cast<void**>(&p_wl_display_dispatch_queue)},
    {"wl_display_dispatch_queue_pending", reinterpret_cast<void**>(&p_wl_display_dispatch_queue_pending)},
    {"wl_display_flush", reinterpret_cast<void**>(&p_wl_display_flush)},
    {"wl_display_get_error", reinterpret_cast<void**>(&p_wl_display_get_error)},
  };
  for (const Sym& s : syms) {
    *s.slot = dlsym(h, s.name);
    if (!*s.slot) { err_ = s.name; return false; }   // an ancient libwayland; treat as absent
  }
  state_ = State::Ok;
  return true;
}

} // namespace ds::sdl::wldyn
