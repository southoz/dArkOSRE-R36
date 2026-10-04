// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "frontend/video/layout.h"

#include <algorithm>
#include <cmath>

namespace ds::frontend {

namespace {
const char* const kModeNames[] = {"vertical", "horizontal", "single", "pip", "dominant_v", "dominant_h"};
const char* const kCornerNames[] = {"tl", "tr", "bl", "br"};
}

const char* mode_name(Mode m) { return kModeNames[static_cast<int>(m)]; }
const char* corner_name(Corner c) { return kCornerNames[static_cast<int>(c)]; }
bool parse_mode(const std::string& s, Mode& m) {
  for (int i = 0; i < static_cast<int>(Mode::Count); ++i) if (s == kModeNames[i]) { m = static_cast<Mode>(i); return true; }
  return false;
}
bool parse_corner(const std::string& s, Corner& c) {
  for (int i = 0; i < static_cast<int>(Corner::Count); ++i) if (s == kCornerNames[i]) { c = static_cast<Corner>(i); return true; }
  return false;
}

const char* int_scale_name(IntScale m) {
  switch (m) { case IntScale::Under: return "under"; case IntScale::Over: return "over"; default: return "off"; }
}
bool parse_int_scale(const std::string& s, IntScale& m) {
  if (s == "off" || s == "false" || s == "0") { m = IntScale::Off; return true; }
  if (s == "under" || s == "true" || s == "1") { m = IntScale::Under; return true; }
  if (s == "over") { m = IntScale::Over; return true; }
  return false;
}
double snap_scale(double s, IntScale m) {
  if (m == IntScale::Off || s <= 0.0) return s;
  const double f = std::floor(s + 1e-9);          // handle 2.9999 from a division
  if (f == s || (s - f) < 1e-9) return f;
  return m == IntScale::Under ? std::max(1.0, f) : f + 1.0;
}

void natural_size(const Layout& l, double scale, int& w, int& h) {
  const double sw = SCREEN_W * scale, sh = SCREEN_H * scale;
  double fw = sw, fh = sh;
  switch (l.mode) {
    case Mode::Vertical:   fh = sh * 2; break;
    case Mode::Horizontal: fw = sw * 2; break;
    case Mode::Single: case Mode::Pip: break;
    case Mode::DominantV:  fh = sh * (1 + (l.dominant_auto ? l.dominant_min : l.dominant)); break;
    case Mode::DominantH:  fw = sw * (1 + (l.dominant_auto ? l.dominant_min : l.dominant)); break;
    case Mode::Count: break;
  }
  // The gap is not scaled: a distance on the glass, not part of the DS. An auto gap is spare room, none here.
  if (!l.gap_auto && (l.mode == Mode::Vertical || l.mode == Mode::DominantV)) fh += l.gap;
  if (!l.gap_auto && (l.mode == Mode::Horizontal || l.mode == Mode::DominantH)) fw += l.gap;
  w = std::max(1, static_cast<int>(fw)); h = std::max(1, static_cast<int>(fh));
}

// Auto dominant: the primary's scale `s` is the largest whole number that
// leaves the secondary at least `dominant_min` of it; the secondary `s2`
// is then the largest that fits the room left beside it (the whole
// width/height, not a ratio of the primary -- the point is a crisp primary,
// the secondary takes what remains), capped at the primary's size. Under
// an integer-scale setting the secondary is floored too, when that leaves
// it at least one panel pixel per DS pixel. When no whole scale leaves
// room enough the fractional fit at `dominant_min` is what is left.
void dominant_auto(const Layout& l, int w, int h, bool across, IntScale snap, double& s, double& s2) {
  const double sw = SCREEN_W, sh = SCREEN_H;
  const double along = across ? w / sw : h / sh;      // room along the pair, in screens
  const double side  = across ? h / sh : w / sw;      // room across it
  for (int k = static_cast<int>(std::floor(std::min(along, side) + 1e-9)); k >= 1; --k) {
    double rest = std::min(along - k, side);          // the secondary's fit in the leftover
    rest = std::min(rest, static_cast<double>(k));
    // The integer-scale setting applies to the secondary too: under floors
    // it, over takes the next whole scale (cropped at the edge, as over does).
    if (snap != IntScale::Off && rest >= 1.0) rest = std::min(snap_scale(rest, snap), static_cast<double>(k));
    if (rest + 1e-9 >= k * l.dominant_min) { s = k; s2 = rest; return; }
  }
  const double r = l.dominant_min;
  s = snap_scale(std::min(across ? w / (sw * (1 + r)) : w / sw, across ? h / sh : h / (sh * (1 + r))), snap);
  s2 = s * r;
}

void place(const Layout& l, int w, int h, View out[SCREENS], IntScale snap) {
  const double sw = SCREEN_W, sh = SCREEN_H;
  // The fit is snapped whole here, so every rect below -- and the scale
  // tables, touch map and margins built from them -- follows. The pair and
  // dominant layouts stay centred as a whole, so an overscale crop takes
  // equally from the outer edges and the edge between the screens is kept.
  // video.screen_gap: the pair is fitted into the room the gap leaves along
  // it, and the second screen starts that much further on. Single and PiP
  // have no pair to part.
  const bool pair_across = l.mode == Mode::Horizontal || l.mode == Mode::DominantH;
  const bool pair = pair_across || l.mode == Mode::Vertical || l.mode == Mode::DominantV;
  // An auto gap: the pair is fitted with none, and whatever room is left
  // along it goes between the screens, which puts them at the edges.
  int gap = pair && !l.gap_auto ? l.gap : 0;
  const int fw = pair_across ? std::max(1, w - gap) : w, fh = pair && !pair_across ? std::max(1, h - gap) : h;
  auto fit = [&](double cols, double rows) { return snap_scale(std::min(fw / (sw * cols), fh / (sh * rows)), snap); };
  auto auto_gap = [&](double along_px) { if (pair && l.gap_auto) gap = std::max(0, static_cast<int>((pair_across ? w : h) - along_px)); };
  auto rect = [&](double x, double y, double s) { return Rect{static_cast<int>(x), static_cast<int>(y), static_cast<int>(sw * s), static_cast<int>(sh * s)}; };
  const int p = l.primary, q = 1 - p;
  switch (l.mode) {
    case Mode::Vertical: case Mode::Horizontal: {
      const bool across = l.mode == Mode::Horizontal;
      const double s = across ? fit(2, 1) : fit(1, 2);
      const double dw = sw * s, dh = sh * s;
      auto_gap(across ? 2 * dw : 2 * dh);
      const double x = (w - dw * (across ? 2 : 1) - (across ? gap : 0)) / 2, y = (h - dh * (across ? 1 : 2) - (across ? 0 : gap)) / 2;
      for (int i = 0; i < SCREENS; ++i) {
        const int screen = i == 0 ? p : q;
        out[i] = View{screen, across ? rect(x + (dw + gap) * i, y, s) : rect(x, y + (dh + gap) * i, s), true, true};
      }
      break;
    }
    case Mode::Single: case Mode::Pip: {
      const double s = fit(1, 1);
      const Rect big = rect((w - sw * s) / 2, (h - sh * s) / 2, s);
      out[0] = View{p, big, true, true};
      if (l.mode == Mode::Single) {
        out[1] = View{q, Rect{0, 0, static_cast<int>(SCREEN_W), static_cast<int>(SCREEN_H)}, false, false};
      } else {
        const double s2 = s * l.pip;
        const int iw = static_cast<int>(sw * s2), ih = static_cast<int>(sh * s2);
        const bool right = l.corner == Corner::TopRight || l.corner == Corner::BottomRight;
        const bool bottom = l.corner == Corner::BottomLeft || l.corner == Corner::BottomRight;
        out[1] = View{q, Rect{right ? big.x + big.w - iw : big.x, bottom ? big.y + big.h - ih : big.y, iw, ih}, false, true};
      }
      break;
    }
    case Mode::DominantV: case Mode::DominantH: {
      const bool across = l.mode == Mode::DominantH;
      double s = 0, s2 = 0;    // the primary's and the secondary's scale
      if (!l.dominant_auto) { s = across ? fit(1 + l.dominant, 1) : fit(1, 1 + l.dominant); s2 = s * l.dominant; }
      else dominant_auto(l, fw, fh, across, snap, s, s2);
      auto_gap(across ? sw * (s + s2) : sh * (s + s2));
      const double sc[2] = {p == 0 ? s : s2, p == 1 ? s : s2};
      if (!across) {
        double y = (h - sh * (s + s2) - gap) / 2;
        for (int i = 0; i < SCREENS; ++i) { out[i] = View{i, rect((w - sw * sc[i]) / 2, y, sc[i]), true, true}; y += sh * sc[i] + gap; }
      } else {
        double x = (w - sw * (s + s2) - gap) / 2;
        const double bottom = (h - sh * s) / 2 + sh * s;
        for (int i = 0; i < SCREENS; ++i) { out[i] = View{i, rect(x, bottom - sh * sc[i], sc[i]), true, true}; x += sw * sc[i] + gap; }
      }
      break;
    }
    case Mode::Count: break;
  }
}

View place_single(int screen, int w, int h, IntScale snap, bool upper_panel) {
  const double sw = SCREEN_W, sh = SCREEN_H, s = snap_scale(std::min(w / sw, h / sh), snap);
  const int dw = static_cast<int>(sw * s), dh = static_cast<int>(sh * s);
  int y = (h - dh) / 2;
  if (dh > h) y = upper_panel ? h - dh : 0;
  return View{screen, Rect{(w - dw) / 2, y, dw, dh}, true, true};
}

} // namespace ds::frontend
