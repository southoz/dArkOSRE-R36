// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"

#include <string>
#include <vector>

namespace ds::frontend {

// Which output ("sink") the software renderer's frames go to, decided in one
// place from what the device offers. No SDL: callers fill the probes, so the
// choices are pinned by test_select for every device class.
//
//   Disp      the Allwinner display engine's scaler layer (/dev/disp; A30 class)
//   Fbdev     straight into /dev/fb0 (mali-fbdev SDL2; H700 handhelds)
//   Kms       KMS page flips of dma-heap buffers (SDL's KMSDRM driver)
//   Dmabuf    zero-copy dma-buf wl_buffers (Wayland)
//   Surface   SDL's window surface (shm under a compositor)
//   Renderer  SDL_Renderer textures, the fallback that always opens
//
// Disp and Fbdev own the panel, so they are decided before SDL_Init (SDL is
// then pointed at a headless driver); the rest are tried in order once the
// window exists, under whatever driver SDL chose.
enum class Sink : u8 { Disp, Fbdev, Kms, Dmabuf, Surface, Renderer, Count };
const char* sink_name(Sink s);
// video.sink: "auto" (Sink::Count) or one of the names above.
bool parse_sink(const std::string& s, Sink& out);

// ---- before SDL_Init ----
struct BootProbe {
  std::string sink;                 // video.sink ("" = auto)
  std::string disp, fbdev;          // deprecated video.disp / video.fbdev: "" | auto | true | on | false | off
  bool dual_window = false;         // the panel-owning sinks drive one panel only
  std::string sdl_videodriver;      // $SDL_VIDEODRIVER, "" if unset
  bool sdl_has_mali = false;        // this SDL2 has a "mali" video driver
  bool display_env = false;         // $DISPLAY or $WAYLAND_DISPLAY set
  bool has_dri = false;             // /dev/dri exists
  bool disp_ok = false;             // DispOut::available()
  bool fb0_ok = false;              // FbdevOut::available()
};
struct BootPlan {
  Sink panel = Sink::Count;         // Disp or Fbdev when one owns the panel; Count: SDL's window decides
  Sink want = Sink::Count;          // explicit video.sink among the window sinks (Count = auto)
  bool headless_driver = false;     // point SDL at its dummy/offscreen driver
  bool unset_videodriver = false;   // drop SDL_VIDEODRIVER=mali first (it would put EGL on fb0 under us)
  std::vector<std::string> notes;   // what to tell stderr
};
BootPlan plan_boot(const BootProbe& p);

// ---- with a window ----
struct WindowProbe {
  Sink want = Sink::Count;          // BootPlan::want
  std::string video_driver;         // SDL_GetCurrentVideoDriver()
  std::string dmabuf_env;           // $DS_DMABUF: "0" forbids Kms/Dmabuf, "1" makes their failure fatal
  std::string scanline_env;         // $DS_SCANLINE_SCALE: "0" forces Renderer, anything else the scanline sinks
};
struct WindowPlan {
  std::vector<Sink> order;          // tried first to last; Renderer always ends it
  bool required = false;            // DS_DMABUF=1: a failed Kms/Dmabuf fails the open
};
WindowPlan plan_window(const WindowProbe& p);

} // namespace ds::frontend
