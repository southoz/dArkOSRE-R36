// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Screen layout (frontend/video/layout.h): hand-checked rects for the common
// cases, and one hash over a sweep of modes x options x output sizes x
// integer-scale settings so any change in placement shows. `test_layout
// --print` lists the sweep.
#include "frontend/video/layout.h"

#include <cstdio>
#include <cstring>
#include <string>

using namespace ds;
using namespace ds::frontend;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

static bool eq(const Rect& r, int x, int y, int w, int h) {
  if (r.x == x && r.y == y && r.w == w && r.h == h) return true;
  std::fprintf(stderr, "  rect %d,%d %dx%d, expected %d,%d %dx%d\n", r.x, r.y, r.w, r.h, x, y, w, h);
  return false;
}

static void spot_checks() {
  View v[SCREENS];
  Layout l;
  // 640x480 panel, stacked: 1.25x each, centred.
  place(l, 640, 480, v); CHECK(eq(v[0].rect, 160, 0, 320, 240)); CHECK(eq(v[1].rect, 160, 240, 320, 240));
  // ... integer under: 1x, letterboxed.
  place(l, 640, 480, v, IntScale::Under); CHECK(eq(v[0].rect, 192, 48, 256, 192)); CHECK(eq(v[1].rect, 192, 240, 256, 192));
  // ... integer over: 2x, cropped equally top and bottom, the shared edge kept at the centre.
  place(l, 640, 480, v, IntScale::Over); CHECK(eq(v[0].rect, 64, -144, 512, 384)); CHECK(eq(v[1].rect, 64, 240, 512, 384));
  // Side by side at 1920x1080, bottom screen first.
  l.mode = Mode::Horizontal; l.primary = 1;
  place(l, 1920, 1080, v); CHECK(v[0].screen == 1); CHECK(eq(v[0].rect, 0, 180, 960, 720)); CHECK(eq(v[1].rect, 960, 180, 960, 720));
  // PiP: inset a third of the large screen, bottom right.
  l = Layout{}; l.mode = Mode::Pip;
  place(l, 640, 480, v); CHECK(eq(v[0].rect, 0, 0, 640, 480)); CHECK(!v[1].direct && v[1].shown); CHECK(eq(v[1].rect, 427, 320, 213, 160));
  // Single: the other screen hidden.
  l.mode = Mode::Single; place(l, 640, 480, v); CHECK(!v[1].shown);
  // Gap: the pair fitted into the room left, the second screen further on.
  l = Layout{}; l.gap = 20;
  place(l, 640, 480, v); CHECK(eq(v[0].rect, 166, 0, 306, 230)); CHECK(eq(v[1].rect, 166, 250, 306, 230));
  // Dominant auto at 1280x800: the primary at the largest whole scale (4x)
  // that leaves the secondary a quarter of it (1x), bottoms aligned.
  l = Layout{}; l.mode = Mode::DominantH;
  place(l, 1280, 800, v); CHECK(eq(v[0].rect, 0, 16, 1024, 768)); CHECK(eq(v[1].rect, 1024, 592, 256, 192));
  // Natural sizes.
  int w = 0, h = 0;
  natural_size(Layout{}, 2.0, w, h); CHECK(w == 512 && h == 768);
  l = Layout{}; l.mode = Mode::DominantV; l.dominant_auto = false; l.dominant = 0.5;
  natural_size(l, 1.0, w, h); CHECK(w == 256 && h == 288);
  // A dual-window panel overscaled keeps the edge by the hinge.
  CHECK(eq(place_single(0, 640, 480, IntScale::Over, true).rect, -64, -96, 768, 576));   // 2.5x over: 3x
  CHECK(eq(place_single(1, 640, 480, IntScale::Over, false).rect, -64, 0, 768, 576));
  // Names round-trip.
  for (int i = 0; i < static_cast<int>(Mode::Count); ++i) { Mode m; CHECK(parse_mode(mode_name(static_cast<Mode>(i)), m) && m == static_cast<Mode>(i)); }
  IntScale s; CHECK(parse_int_scale("true", s) && s == IntScale::Under); CHECK(!parse_int_scale("sideways", s));
  CHECK(snap_scale(2.9999999999, IntScale::Under) == 3.0); CHECK(snap_scale(1.25, IntScale::Over) == 2.0); CHECK(snap_scale(0.5, IntScale::Under) == 1.0);
}

static std::string sweep() {
  static const int sizes[][2] = {{640, 480}, {480, 640}, {1920, 1080}, {854, 480}, {1024, 768}, {1280, 960}, {720, 720}, {320, 240}, {1280, 800}};
  std::string out;
  char line[256];
  for (int m = 0; m < static_cast<int>(Mode::Count); ++m)
    for (int primary = 0; primary < 2; ++primary)
      for (int corner = 0; corner < (m == static_cast<int>(Mode::Pip) ? 4 : 1); ++corner)
        for (int gap : {0, 16, -8, 1000})   // 1000: the auto gap (the pair pushed to the edges)
          for (int autod = 0; autod < 2; ++autod)
            for (int snap = 0; snap < 3; ++snap)
              for (const auto& sz : sizes) {
                Layout l; l.mode = static_cast<Mode>(m); l.primary = primary; l.corner = static_cast<Corner>(corner);
                l.gap = gap == 1000 ? 0 : gap; l.gap_auto = gap == 1000; l.dominant_auto = autod != 0; l.dominant = 0.6; l.pip = 0.3;
                View v[SCREENS];
                place(l, sz[0], sz[1], v, static_cast<IntScale>(snap));
                int nw = 0, nh = 0; natural_size(l, 1.5, nw, nh);
                std::snprintf(line, sizeof line, "%s p%d c%d g%d a%d s%d %dx%d nat %dx%d:", mode_name(l.mode), primary, corner, gap, autod, snap, sz[0], sz[1], nw, nh);
                out += line;
                for (const View& x : v) { std::snprintf(line, sizeof line, " [%d %d,%d %dx%d %d%d]", x.screen, x.rect.x, x.rect.y, x.rect.w, x.rect.h, x.direct, x.shown); out += line; }
                out += '\n';
              }
  for (const auto& sz : sizes)
    for (int snap = 0; snap < 3; ++snap)
      for (int upper = 0; upper < 2; ++upper) {
        const View x = place_single(upper ? 0 : 1, sz[0], sz[1], static_cast<IntScale>(snap), upper != 0);
        std::snprintf(line, sizeof line, "single %dx%d s%d u%d: %d,%d %dx%d\n", sz[0], sz[1], snap, upper, x.rect.x, x.rect.y, x.rect.w, x.rect.h);
        out += line;
      }
  return out;
}

int main(int argc, char** argv) {
  const std::string s = sweep();
  if (argc > 1 && !std::strcmp(argv[1], "--print")) { std::fputs(s.c_str(), stdout); return 0; }
  spot_checks();
  u64 h = 1469598103934665603ull;
  for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
  // The sweep as placement stood when layout moved out of Display (video overhaul P3).
  constexpr u64 kSweep = 0xea2a86071452114aull;
  if (h != kSweep) { std::fprintf(stderr, "FAIL layout sweep hash %016llx, expected %016llx (test_layout --print to compare)\n", (unsigned long long)h, (unsigned long long)kSweep); ++failures; }
  std::printf("layout: %s\n", failures ? "FAIL" : "ok");
  return failures ? 1 : 0;
}
