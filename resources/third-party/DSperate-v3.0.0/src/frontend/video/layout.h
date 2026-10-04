// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"

#include <string>

namespace ds::frontend {

// Where the DS screens go in an output of a given size: the arithmetic every
// presenter (software sinks, GPU) and the screenshot writer share. No SDL.

struct Rect { int x = 0, y = 0, w = 0, h = 0; };   // same layout as SDL_Rect

// How the two screens share the output; `primary` is the screen shown alone
// (Single), large (Pip) or dominant (DominantV/H), else first in the
// stack/row. DominantV/H: primary fitted to width/height, secondary
// `dominant` times its size. With `dominant_auto`, the primary instead takes
// the largest whole scale leaving the secondary >= `dominant_min`.
enum class Mode : u8 { Vertical, Horizontal, Single, Pip, DominantV, DominantH, Count };
enum class Corner : u8 { TopLeft, TopRight, BottomLeft, BottomRight, Count };
struct Layout {
  Mode   mode = Mode::Vertical;
  int    primary = 0;
  Corner corner = Corner::BottomRight;
  double pip = 1.0 / 3.0;      // inset size relative to the large screen
  double dominant = 0.5;       // secondary size relative to the dominant screen
  bool   dominant_auto = true; // pick the primary's whole scale instead (see above); a ratio when false
  double dominant_min = 0.25;  // auto: the smallest secondary the primary may leave
  double pip_alpha = 1.0;      // inset opacity at rest, 0..1
  int    gap = 0;              // video.screen_gap: pixels between the two screens of a pair (negative overlaps)
  bool   gap_auto = false;     // video.screen_gap = auto: the pair is fitted with no gap, then the screens
                               // are pushed to the output's edges along the pair, the spare room between them
};
// Forced integer scaling of full-size views (PiP inset / dominant secondary
// keep their own ratio). Under: largest whole scale that fits, letterboxed.
// Over: smallest that covers, cropped and centred (a stacked pair keeps its
// shared edge).
enum class IntScale : u8 { Off, Under, Over };

// `direct`: drawn straight into the output at `rect`. Otherwise into a side
// buffer (an inset, or a hidden screen), copied into place if `shown`.
struct View { int screen; Rect rect; bool direct; bool shown; };
constexpr int SCREENS = 2;

const char* mode_name(Mode m);      // "vertical" ... "dominant_h"
bool parse_mode(const std::string& s, Mode& m);
const char* corner_name(Corner c);  // "tl" "tr" "bl" "br"
bool parse_corner(const std::string& s, Corner& c);
const char* int_scale_name(IntScale m);
bool parse_int_scale(const std::string& s, IntScale& m);
double snap_scale(double s, IntScale m);

// The output size that shows the layout at `scale` output pixels per DS pixel.
void natural_size(const Layout& l, double scale, int& w, int& h);
// The two screens in a w x h output under `l`, in draw order (later on top).
void place(const Layout& l, int w, int h, View out[SCREENS], IntScale snap = IntScale::Off);
// One screen alone (a dual-window panel). Overscale crops away from the
// edge shared with the other panel: the upper panel keeps its bottom row.
View place_single(int screen, int w, int h, IntScale snap, bool upper_panel);
void dominant_auto(const Layout& l, int w, int h, bool across, IntScale snap, double& s, double& s2);

} // namespace ds::frontend
