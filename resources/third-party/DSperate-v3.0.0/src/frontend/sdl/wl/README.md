# Vendored Wayland protocol code

Generated and copied so the dmabuf presentation path builds with **no
build- or link-time Wayland dependency**: `libwayland-client.so.0` is
dlopen'd at runtime (`wl_dyn.h`), and a device without it (or without a
compositor) falls back to the KMSDRM scanout tier (`display_drm.h`) and
then to SDL's own paths.

- `wayland-client-protocol.h` / `wayland-protocol.c`: core protocol,
  `wayland-scanner {client-header,private-code} /usr/share/wayland/wayland.xml`.
  Compiling the core private-code in means the `wl_*_interface` descriptor
  *data* symbols are our own; only the dozen `wl_proxy_*` / `wl_display_*`
  *functions* come from the dlopen'd library (libwayland marshals by
  interface content, not identity).
- `linux-dmabuf-v1.{h,c}`: wayland-protocols stable/linux-dmabuf, same scanner.
- `xdg-shell.{h,c}`: wayland-protocols stable/xdg-shell, same scanner. Only
  used to ask for fullscreen on a *specific* output (SDL cannot on Wayland --
  window positions are a client illusion there); the xdg_toplevel itself is
  SDL's, fetched via SDL_SysWMinfo.
- `wayland-client{,-core}.h`, `wayland-util.h`, `wayland-version.h`: copied
  from libwayland 1.22 (MIT), with one local change: the prototypes of the
  thirteen functions `wl_dyn.h` maps to pointers are wrapped in
  `#ifndef <name>` so the macro expansion cannot turn them into variable
  definitions. Re-apply that if these files are ever refreshed.

Every file that touches these must include `wl_dyn.h` **first**: it defines
the `wl_proxy_*` names as macros over function pointers before the header
inlines are seen (the SDL dynamic-loading pattern). The generated `.c` files
get it force-included by CMake.
