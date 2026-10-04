// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Presentation through the Allwinner display engine's scaler layer, for the
// Miyoo A30 (Allwinner A33, "disp 1.5" driver) and similar /dev/disp devices.
// Exists because the A30's only SDL2 driver is Mali EGL over fbdev (GLES
// upload+swap for every path) and CPU rotation for its portrait panel is
// costly; instead the DS framebuffers are NEON-rotated into one composite
// that the scaler layer scales+rotates onto the panel in hardware. The UI's
// own fb0 layer is disabled while this is open. One DE scaler layer only, so
// multi-view layouts (PiP, dominant) are all composited into it first.
// Chunky mode (set_divisor) downscales the composite before the DE enlarges
// it with the nearest table, so DS blocks land on whole panel cells for
// free. The LCD grid is a second DE layer blended per pixel over the scaled
// composite. Nearest-neighbour filtering is done by overwriting the
// scaler's coefficient RAM via /dev/mem right after each layer set (the
// driver's own write happens inside that ioctl, so ours wins from the next
// line); --linear leaves the RAM to the driver.
#pragma once

#include "core/types.h"

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <thread>
#include <vector>

namespace ds::sdl {

class DispOut {
public:
  static constexpr int BUFS = 4, VIEWS = 2;

  // True if /dev/disp and /dev/fb0 open and a layer query answers (disp-1.5
  // kernel). Cheap, cached; safe to call before SDL_Init.
  static bool available();

  // rot: 0/90/180/270, the rotation from DS layout to panel. False leaves
  // nothing changed on the device.
  bool open(int rot, bool vsync);
  void close();
  bool vsync() const { return vsync_; }

  // The canvas: Display::natural_size in DS pixels before rotation, what
  // layout() and map_point() work in.
  void set_canvas(int w, int h);
  int  logical_w() const { return canvas_w_; }
  int  logical_h() const { return canvas_h_; }

  // View i's rectangle on the canvas; later views draw over earlier ones.
  void set_view(int i, int x, int y, int w, int h, bool shown);
  // Chunky cell size drawn at source in view i, canvas pixels (1 = none).
  void set_view_cell(int i, int cell);
  // Hardware chunky: composite = canvas / d (1 = off); every view rect and
  // the canvas must divide by d. Insets stay opaque under a divisor.
  void set_divisor(int d);
  int  divisor() const { return div_; }
  double fit_scale() const;   // panel pixels per canvas pixel, independent of the divisor
  // Opacity of a view drawn over another (the PiP inset), 0..255; a 1:1 view is always opaque.
  void set_inset_alpha(u8 a) { inset_alpha_ = a; }
  // LCD grid as a second DE layer, blended per pixel over the scaled
  // composite at no per-frame cost; redrawn when the layout changes. Set
  // before open(); 0 = no grid.
  void set_grid(u8 alpha) { grid_alpha_ = alpha; }
  bool grid() const { return grid_layer_ >= 0 && grid_alpha_ != 0; }

  // The grid's layer doubles as a drawing surface for the frontend (pause
  // menu, OSD) at panel resolution, since the DE has only two pipes and no
  // spare memory for a second image. Recomposed only when the frontend
  // says it changed. Enable before open().
  void set_overlay(bool on) { overlay_wanted_ = on; }
  bool overlay_available() const { return grid_layer_ >= 0 && overlay_wanted_; }
  // The surface, in display orientation (panel turned by rot). Cleared to
  // transparent on the first call of a frame. False if there's no overlay.
  bool overlay(u32*& px, int& pitch, int& w, int& h);
  void overlay_changed(bool any);   // any=false takes the last overlay back off the panel
  void set_nearest(bool on) { nearest_wanted_ = on; }   // see header comment; set before open()
  // Display::IntScale (0 off, 1 under, 2 over): snaps the DE fit to whole panel pixels per DS pixel.
  void set_integer_scale(int m) { int_scale_ = m; }
  bool nearest() const { return fe_ != nullptr; }

  // Draws each view's framebuffer (null skips it) into a free composite and
  // flips the layer to it; never blocks emulation on the panel (vsync waits
  // happen on the presenter thread). Four buffers cover displayed + latched
  // + queued + drawing.
  void present(const u32* const fb[VIEWS]);

private:
  struct ViewRect { int x = 0, y = 0, w = 0, h = 0; bool shown = false; int cell = 1; };
  struct Dims { int w = 0, h = 0; };          // a composite's size (the canvas, rotated, over the divisor)
  Dims comp_dims() const;
  bool set_layer(u32 addr, Dims d);
  // Panel window the composite is fitted into (aspect kept, centred), and
  // the part of the composite shown (all, unless integer overscale crops it).
  struct Fit { int x = 0, y = 0; unsigned w = 0, h = 0; int sx = 0, sy = 0; unsigned sw = 0, sh = 0; };
  Fit fit(Dims d) const;
  double snap(double s) const;
  void comp_rect(const ViewRect& r, int& cx, int& cy, int& cw, int& ch) const;
  void draw_grid(Dims d);
  bool set_grid_layer();
  void flip(int buf);
  void wait_vsync();
  bool open_frontend();           // maps the DE front end's registers for write_coefs()
  void write_coefs();             // the nearest table into the scaler's coefficient RAM
  void draw_view(u32* comp, int comp_w, const ViewRect& r, const u32* fb, int index, const u32* const fbs[VIEWS]);
  void canvas_point(int compx, int compy, int& x, int& y) const;   // inverse of draw_view's rotation
  const u32* under_pixel(int x, int y, int index, const u32* const fbs[VIEWS]) const;
  void presenter();
  u32  buf_addr(int buf) const { return phys_ + static_cast<u32>(buf * buf_bytes_); }
  u32* buf_ptr(int buf) const { return reinterpret_cast<u32*>(map_ + buf * buf_bytes_); }

  int  disp_ = -1, fb_ = -1, mem_ = -1;
  volatile u32* fe_ = nullptr;      // DE front end registers (nearest only)
  bool nearest_wanted_ = true;
  int  int_scale_ = 0;
  u8*  map_ = nullptr;
  size_t map_len_ = 0;
  u32  phys_ = 0;
  int  panel_w_ = 0, panel_h_ = 0;
  int  rot_ = 0;
  int  canvas_w_ = 0, canvas_h_ = 0;
  int  div_ = 1;                    // set_divisor
  size_t buf_bytes_ = 0;            // one composite's allocation (the largest canvas)
  int  layer_ = -1, ui_layer_ = -1;
  u8   grid_alpha_ = 0;
  int  grid_layer_ = -1;            // the grid's layer; -1 = no grid
  bool grid_enabled_ = false;
  bool grid_dirty_ = false;         // redraw the grid image before the next flip
  size_t grid_off_ = 0;             // the grid image's offset in fb0 memory (after the composites)
  Dims grid_dims_;                  // the composite size the image was drawn for
  std::vector<u32> grid_stage_;     // the image is composed here, then copied to fb0 in bulk
  bool overlay_wanted_ = false;
  bool ov_active_ = false;          // the frontend drew something last frame
  bool ov_taken_ = false;           // overlay() was called since the last compose
  int  ov_w_ = 0, ov_h_ = 0;        // the surface's size, in display orientation
  std::vector<u32> ov_stage_;       // what the frontend draws into (display orientation)
  std::vector<u32> ov_sent_;        // the last image uploaded to fb0, to skip identical ones
  void compose_overlay();           // grid seams + ov_stage_, rotated, into fb0 if it changed
  bool ui_was_enabled_ = false;
  bool layer_enabled_ = false;      // our layer is on (enabled on the first flip)
  ViewRect views_[VIEWS];
  u8   inset_alpha_ = 255;
  Dims dims_[BUFS];                 // what each buffer holds, set by present, read by flip
  unsigned dirty_ = 0;              // buffers to black out before the next draw (canvas changed)
  std::vector<u32> tmp_, tmp2_, tmp3_, tmp4_;   // cached temporaries for the downscaled views (tmp3_: the row under a blended one; tmp4_: a halving pass)
  int  cur_ = 0;
  bool vsync_ = true;
  bool timing_ = false;             // DS_DISP_TIMING: log the flip's distance from the blank
  bool diag_ = false;               // DS_DISP_DIAG: stall and rate diagnostics from the presenter
  u64  diag_flips_ = 0, diag_posts_ = 0, diag_mark_ = 0;
  u64  flip_ns_sum_ = 0, flip_ns_max_ = 0, flip_n_ = 0, flip_miss_ = 0;
  bool pan_blocks_ = true;          // FBIOPAN_DISPLAY waits for the refresh (measured once)
  bool pan_measured_ = false;
  u64  next_ns_ = 0;                // fallback pacing when pan does not block

  // Presenter thread state (vsync only). Buffer indices; -1 = none.
  std::thread             thread_;
  std::mutex              mu_;
  std::condition_variable cv_;
  int  displayed_ = 0;              // on the panel now
  int  latched_ = -1;               // flipped to, waiting for the refresh
  int  queued_ = -1;                // taken by the presenter, waiting for the vsync to flip
  int  pending_ = -1;               // drawn into, not yet flipped
  bool stop_ = false;
};

} // namespace ds::sdl
