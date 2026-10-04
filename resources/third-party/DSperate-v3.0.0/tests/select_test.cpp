// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Output selection (frontend/video/select.h): the sink each device class
// gets, as the tier code in main.cpp and Display::open chose it before the
// table existed, plus the explicit video.sink values and the env overrides.
#include "frontend/video/select.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace ds::frontend;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

static std::string order(const WindowPlan& w) {
  std::string s;
  for (Sink k : w.order) { if (!s.empty()) s += ','; s += sink_name(k); }
  return s;
}
static bool eq(const WindowPlan& w, const char* want) {
  if (order(w) == want) return true;
  std::fprintf(stderr, "  order %s, expected %s\n", order(w).c_str(), want);
  return false;
}

static void devices() {
  // RG DS Plus (ROCKNIX, Sway, dual window): no panel sink; dma-buf first.
  {
    BootProbe p; p.dual_window = true; p.display_env = true; p.has_dri = true; p.fb0_ok = true;
    const BootPlan b = plan_boot(p);
    CHECK(b.panel == Sink::Count && !b.headless_driver && b.notes.empty());
    CHECK(eq(plan_window({b.want, "wayland", "", ""}), "dmabuf,surface,renderer"));
  }
  // Same device single-window: a Wayland desktop with an fb0 never takes fbdev.
  {
    BootProbe p; p.display_env = true; p.has_dri = true; p.fb0_ok = true;
    CHECK(plan_boot(p).panel == Sink::Count);
  }
  // Miyoo A30: /dev/disp answers.
  {
    BootProbe p; p.disp_ok = true; p.fb0_ok = true; p.has_dri = false;
    const BootPlan b = plan_boot(p);
    CHECK(b.panel == Sink::Disp && b.headless_driver && !b.unset_videodriver);
  }
  // RG35XX SP under spruce: no display, no DRM, fb0 -> fbdev.
  {
    BootProbe p; p.fb0_ok = true;
    const BootPlan b = plan_boot(p);
    CHECK(b.panel == Sink::Fbdev && b.headless_driver && !b.unset_videodriver);
  }
  // H700 BaseOS: the launcher names the mali driver; it is dropped so EGL stays off fb0.
  {
    BootProbe p; p.fb0_ok = true; p.sdl_videodriver = "mali"; p.has_dri = true; p.display_env = true;
    const BootPlan b = plan_boot(p);
    CHECK(b.panel == Sink::Fbdev && b.unset_videodriver);
  }
  // An SDL2 built with the mali driver, nothing named.
  {
    BootProbe p; p.fb0_ok = true; p.sdl_has_mali = true; p.has_dri = true; p.display_env = true;
    CHECK(plan_boot(p).panel == Sink::Fbdev);
  }
  // KMSDRM console (no compositor): page flips first.
  {
    BootProbe p; p.has_dri = true; p.fb0_ok = true;
    const BootPlan b = plan_boot(p);
    CHECK(b.panel == Sink::Count);
    CHECK(eq(plan_window({b.want, "KMSDRM", "", ""}), "kms,surface,renderer"));
  }
  // X11 desktop: the renderer only.
  CHECK(eq(plan_window({Sink::Count, "x11", "", ""}), "renderer"));
}

static void overrides() {
  // Deprecated switches still steer auto, with a note.
  {
    BootProbe p; p.disp_ok = true; p.fb0_ok = true; p.disp = "false";
    const BootPlan b = plan_boot(p);
    CHECK(b.panel == Sink::Fbdev);   // headless box: fbdev's auto takes over
    CHECK(b.notes.size() == 1);
  }
  {
    BootProbe p; p.fb0_ok = true; p.display_env = true; p.has_dri = true; p.fbdev = "on";
    CHECK(plan_boot(p).panel == Sink::Fbdev);
  }
  // Asked for but missing: said, and SDL decides.
  {
    BootProbe p; p.sink = "disp";
    const BootPlan b = plan_boot(p);
    CHECK(b.panel == Sink::Count && !b.headless_driver && b.notes.size() == 1);
  }
  // An explicit window sink skips the panel sinks' auto.
  {
    BootProbe p; p.sink = "surface"; p.disp_ok = true; p.fb0_ok = true;
    const BootPlan b = plan_boot(p);
    CHECK(b.panel == Sink::Count && b.want == Sink::Surface);
    CHECK(eq(plan_window({b.want, "wayland", "", ""}), "surface,renderer"));
  }
  {
    BootProbe p; p.sink = "renderer";
    CHECK(eq(plan_window({plan_boot(p).want, "wayland", "", ""}), "renderer"));
  }
  // Panel sinks are one-panel only.
  {
    BootProbe p; p.sink = "fbdev"; p.fb0_ok = true; p.dual_window = true;
    const BootPlan b = plan_boot(p);
    CHECK(b.panel == Sink::Count && b.notes.size() == 1);
  }
  // Unknown value: auto, with a note.
  {
    BootProbe p; p.sink = "vulkan"; p.disp_ok = true;
    const BootPlan b = plan_boot(p);
    CHECK(b.panel == Sink::Disp && b.notes.size() == 1);
  }
  // DS_DMABUF / DS_SCANLINE_SCALE.
  CHECK(eq(plan_window({Sink::Count, "wayland", "0", ""}), "surface,renderer"));
  CHECK(eq(plan_window({Sink::Count, "KMSDRM", "0", ""}), "surface,renderer"));
  CHECK(plan_window({Sink::Count, "wayland", "1", ""}).required);
  CHECK(!plan_window({Sink::Count, "x11", "1", ""}).required);
  CHECK(eq(plan_window({Sink::Count, "wayland", "", "0"}), "renderer"));
  CHECK(eq(plan_window({Sink::Count, "x11", "", "1"}), "surface,renderer"));
  CHECK(eq(plan_window({Sink::Dmabuf, "wayland", "0", ""}), "renderer"));
  // Names round-trip.
  for (int i = 0; i <= static_cast<int>(Sink::Count); ++i) {
    Sink s{};
    CHECK(parse_sink(sink_name(static_cast<Sink>(i)), s) && s == static_cast<Sink>(i));
  }
}

int main() {
  devices();
  overrides();
  if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
  std::puts("select: ok");
  return 0;
}
