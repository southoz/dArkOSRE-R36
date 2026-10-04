// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "frontend/video/select.h"

#include <algorithm>

namespace ds::frontend {

namespace {
constexpr const char* kNames[] = {"disp", "fbdev", "kms", "dmabuf", "surface", "renderer"};
static_assert(sizeof(kNames) / sizeof(*kNames) == static_cast<size_t>(Sink::Count), "one name per sink");

bool on(const std::string& v) { return v == "true" || v == "on"; }
bool is_auto(const std::string& v) { return v.empty() || v == "auto"; }
} // namespace

const char* sink_name(Sink s) { return s < Sink::Count ? kNames[static_cast<size_t>(s)] : "auto"; }

bool parse_sink(const std::string& s, Sink& out) {
  if (is_auto(s)) { out = Sink::Count; return true; }
  for (size_t i = 0; i < static_cast<size_t>(Sink::Count); ++i)
    if (s == kNames[i]) { out = static_cast<Sink>(i); return true; }
  return false;
}

BootPlan plan_boot(const BootProbe& p) {
  BootPlan b;
  Sink want = Sink::Count;
  if (!parse_sink(p.sink, want)) b.notes.push_back("video.sink: '" + p.sink + "' is not a sink; auto");
  // The old per-tier switches still steer auto; an explicit sink overrides them.
  if (!p.disp.empty()) b.notes.push_back("video.disp is deprecated; use video.sink = disp");
  if (!p.fbdev.empty()) b.notes.push_back("video.fbdev is deprecated; use video.sink = fbdev");
  const bool explicit_sink = want != Sink::Count;
  const bool use_disp = explicit_sink ? want == Sink::Disp : on(p.disp);
  const bool disp_auto = !explicit_sink && is_auto(p.disp);
  const bool use_fbdev = explicit_sink ? want == Sink::Fbdev : on(p.fbdev);
  const bool fbdev_auto = !explicit_sink && is_auto(p.fbdev);
  if (want != Sink::Disp && want != Sink::Fbdev) b.want = want;

  if ((use_disp || use_fbdev) && p.dual_window)
    b.notes.push_back(std::string("video.sink: ") + (use_disp ? "disp" : "fbdev") + " drives one panel; not with dual_window");
  if (p.dual_window) return b;

  // Display engine: auto wherever /dev/disp answers.
  if (use_disp || disp_auto) {
    if (p.disp_ok) { b.panel = Sink::Disp; b.headless_driver = true; return b; }
    if (use_disp) b.notes.push_back("video.sink: /dev/disp not usable; using SDL");
  }

  // fbdev: auto only where this is an fbdev box -- the launcher named the
  // mali driver, this SDL2 was built with one, the launcher named a headless
  // driver outright, or nothing SDL could draw on exists (no X or Wayland
  // display, no DRM node), in which case SDL lands on its offscreen driver by
  // itself (seen on the RG35XX SP through spruce's launcher: a black panel
  // with sound). Any desktop with a console has an fb0 too, and its
  // compositor, not us, should have it.
  if (use_fbdev || fbdev_auto) {
    bool mali = false, headless = false;
    if (!p.sdl_videodriver.empty()) {
      mali = p.sdl_videodriver == "mali";
      headless = p.sdl_videodriver == "dummy" || p.sdl_videodriver == "offscreen";
    } else {
      mali = p.sdl_has_mali;
      headless = !p.display_env && !p.has_dri;
    }
    if ((use_fbdev || mali || headless) && p.fb0_ok) {
      b.panel = Sink::Fbdev;
      b.headless_driver = true;
      b.unset_videodriver = mali && !p.sdl_videodriver.empty();
      return b;
    }
    if (use_fbdev) b.notes.push_back("video.sink: /dev/fb0 not usable; using SDL");
  }
  return b;
}

WindowPlan plan_window(const WindowProbe& p) {
  WindowPlan w;
  const bool wayland = p.video_driver == "wayland";
  const bool kms = p.video_driver == "KMSDRM";
  const bool dm_forbidden = p.dmabuf_env == "0";
  w.required = p.dmabuf_env == "1";

  if (p.want != Sink::Count && p.want != Sink::Renderer) {
    // Explicit: that sink, and the renderer if it won't open.
    if (!(dm_forbidden && (p.want == Sink::Kms || p.want == Sink::Dmabuf))) w.order.push_back(p.want);
  } else if (p.want == Sink::Count) {
    // Per-scanline scaling renders straight into the presented buffer, so it
    // is on wherever a zero-copy destination exists: Wayland's shm surface
    // (or a dma-buf in its place), or KMSDRM's page-flip sink (even its
    // window-surface fallback wins, since that's secretly a hidden GLES
    // renderer there; see display_drm.h). DS_SCANLINE_SCALE=0/1 overrides.
    const bool scanline = !p.scanline_env.empty() ? p.scanline_env != "0" : (wayland || kms);
    if (scanline) {
      // KMSDRM first, before asking for a window surface: SDL_GetWindowSurface
      // there *succeeds* by quietly building a GLES renderer, the cost Kms avoids.
      if (kms && !dm_forbidden) w.order.push_back(Sink::Kms);
      if (wayland && !dm_forbidden) w.order.push_back(Sink::Dmabuf);
      w.order.push_back(Sink::Surface);
    }
  }
  w.order.push_back(Sink::Renderer);
  if (w.required) w.required = std::any_of(w.order.begin(), w.order.end(), [](Sink s) { return s == Sink::Kms || s == Sink::Dmabuf; });
  return w;
}

} // namespace ds::frontend
