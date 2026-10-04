// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Runtime-loads libwayland-client via dlsym so the binary has no Wayland link
// dependency. Macros below map each real wl_* name to its resolved pointer;
// must be included before any wayland header (CMake force-includes it into
// generated .c files). Interface data symbols are compiled in separately
// (wl/wayland-protocol.c) since libwayland matches those by content, not identity.
#pragma once

#include <stdint.h>

struct wl_proxy;
struct wl_display;
struct wl_event_queue;
struct wl_interface;

#ifdef __cplusplus
namespace ds::sdl::wldyn {

bool load();   // true once resolved; safe to call repeatedly, failure is sticky
const char* error();   // why load() failed

} // namespace ds::sdl::wldyn

// Outside the namespace so the macros below substitute into vendored C code.
extern "C" {
#endif
union wl_argument;   // forward decl
extern struct wl_proxy* (*p_wl_proxy_marshal_flags)(struct wl_proxy*, uint32_t opcode, const struct wl_interface*, uint32_t version, uint32_t flags, ...);
extern struct wl_proxy* (*p_wl_proxy_marshal_array_flags)(struct wl_proxy*, uint32_t opcode, const struct wl_interface*, uint32_t version, uint32_t flags, union wl_argument*);
extern int              (*p_wl_proxy_add_listener)(struct wl_proxy*, void (**implementation)(void), void* data);
extern void             (*p_wl_proxy_destroy)(struct wl_proxy*);
extern uint32_t         (*p_wl_proxy_get_version)(struct wl_proxy*);
extern void             (*p_wl_proxy_set_queue)(struct wl_proxy*, struct wl_event_queue*);
extern struct wl_event_queue* (*p_wl_display_create_queue)(struct wl_display*);
extern void             (*p_wl_event_queue_destroy)(struct wl_event_queue*);
extern int              (*p_wl_display_roundtrip_queue)(struct wl_display*, struct wl_event_queue*);
extern int              (*p_wl_display_dispatch_queue)(struct wl_display*, struct wl_event_queue*);
extern int              (*p_wl_display_dispatch_queue_pending)(struct wl_display*, struct wl_event_queue*);
extern int              (*p_wl_display_flush)(struct wl_display*);
extern int              (*p_wl_display_get_error)(struct wl_display*);
#ifdef __cplusplus
}
#endif

// Map real names onto the pointers for code compiled after this header.
#define wl_proxy_marshal_flags (*p_wl_proxy_marshal_flags)
#define wl_proxy_marshal_array_flags (*p_wl_proxy_marshal_array_flags)
#define wl_proxy_add_listener (*p_wl_proxy_add_listener)
#define wl_proxy_destroy (*p_wl_proxy_destroy)
#define wl_proxy_get_version (*p_wl_proxy_get_version)
#define wl_proxy_set_queue (*p_wl_proxy_set_queue)
#define wl_display_create_queue (*p_wl_display_create_queue)
#define wl_event_queue_destroy (*p_wl_event_queue_destroy)
#define wl_display_roundtrip_queue (*p_wl_display_roundtrip_queue)
#define wl_display_dispatch_queue (*p_wl_display_dispatch_queue)
#define wl_display_dispatch_queue_pending (*p_wl_display_dispatch_queue_pending)
#define wl_display_flush (*p_wl_display_flush)
#define wl_display_get_error (*p_wl_display_get_error)
