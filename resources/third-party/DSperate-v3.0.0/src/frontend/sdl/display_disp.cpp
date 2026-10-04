// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "display_disp.h"
#include "rt_thread.h"
#include "display.h"

#include "core/gpu/gpu.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <ctime>

#if defined(__linux__)
#include <fcntl.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define DS_DISP_NEON 1
#endif

namespace ds::sdl {

namespace {

// ---- disp 1.5 ABI (kernel-headers/drv_display.h) -------------------------------
constexpr unsigned CMD_LAYER_ENABLE = 0x40, CMD_LAYER_DISABLE = 0x41, CMD_LAYER_SET_INFO = 0x42, CMD_LAYER_GET_INFO = 0x43;
constexpr unsigned CMD_GET_SCN_WIDTH = 0x07, CMD_GET_SCN_HEIGHT = 0x08;
constexpr unsigned FBIOGET_LAYER_HDL_0 = 0x4700;
constexpr unsigned LAYER_MODE_SCALER = 4;
constexpr unsigned FORMAT_ARGB_8888 = 0;

// ---- DE front end (the scaler) ---------------------------------------------------
// Register layout per the sun4i/sun8i DEFE (linux: drivers/gpu/drm/sun4i/
// sun4i_frontend.h; the A33 is "allwinner,sun8i-a33-display-frontend").
// The base is the A33's DEFE0; another chip on this driver would need its own.
constexpr off_t    FE_BASE = 0x01e00000;
constexpr size_t   FE_MAP = 0x1000;
constexpr unsigned FE_FRM_CTRL = 0x004 / 4, FE_STATUS = 0x068 / 4;
constexpr u32      FE_COEF_ACCESS = 1u << 23, FE_COEF_ACCESS_OK = 1u << 11;
constexpr unsigned FE_CH0_HORZCOEF0 = 0x400 / 4, FE_CH0_HORZCOEF1 = 0x480 / 4, FE_CH0_VERTCOEF = 0x500 / 4;
constexpr unsigned FE_CH1_HORZCOEF0 = 0x600 / 4, FE_CH1_HORZCOEF1 = 0x680 / 4, FE_CH1_VERTCOEF = 0x700 / 4;

struct DispWindow { int x, y; unsigned width, height; };
struct DispSize { unsigned width, height; };
struct DispFbInfo {
  unsigned addr[3];
  DispSize size;
  unsigned format;
  unsigned cs_mode;
  int      b_trd_src;
  unsigned trd_mode;
  unsigned trd_right_addr[3];
  int      pre_multiply;
  DispWindow src_win;
};
struct DispLayerInfo {
  unsigned mode;
  unsigned char pipe, zorder, alpha_mode, alpha_value;
  int      ck_enable;
  DispWindow screen_win;
  DispFbInfo fb;
  int      b_trd_out;
  unsigned out_trd_mode;
  unsigned id;
};
static_assert(sizeof(DispLayerInfo) == 108, "disp_layer_info layout (verified against the A30 driver)");

#if defined(__linux__)
long layer_ioctl(int fd, unsigned cmd, unsigned layer, void* info) {
  unsigned long args[4] = {0, layer, reinterpret_cast<unsigned long>(info), 0};
  return ioctl(fd, cmd, args);
}
u64 now_ns() { timespec t{}; clock_gettime(CLOCK_MONOTONIC, &t); return static_cast<u64>(t.tv_sec) * 1000000000ull + static_cast<u64>(t.tv_nsec); }
#endif

// ---- rotation kernels ----------------------------------------------------------
// 270: dst[py][px] = src[px][W-1-py]. 90: dst[py][px] = src[H-1-px][py].
// Panel memory is uncached, so stores go a full line at a time via NEON transposes.
constexpr int W = static_cast<int>(SCREEN_W), H = static_cast<int>(SCREEN_H);

#if DS_DISP_NEON
inline void tr4(uint32x4_t r0, uint32x4_t r1, uint32x4_t r2, uint32x4_t r3, uint32x4_t& c0, uint32x4_t& c1, uint32x4_t& c2, uint32x4_t& c3) {
  const uint32x4x2_t a = vtrnq_u32(r0, r1), b = vtrnq_u32(r2, r3);
  c0 = vcombine_u32(vget_low_u32(a.val[0]), vget_low_u32(b.val[0]));
  c1 = vcombine_u32(vget_low_u32(a.val[1]), vget_low_u32(b.val[1]));
  c2 = vcombine_u32(vget_high_u32(a.val[0]), vget_high_u32(b.val[0]));
  c3 = vcombine_u32(vget_high_u32(a.val[1]), vget_high_u32(b.val[1]));
}
void rot270(const u32* src, u32* dst, int stride) {
  for (int sy = 0; sy < H; sy += 16) {
    const u32* r = src + sy * W;
    for (int sx = 0; sx < W; sx += 4) {
      uint32x4_t c[4][4];
      for (int g = 0; g < 4; ++g) {
        const u32* p = r + g * 4 * W + sx;
        tr4(vld1q_u32(p), vld1q_u32(p + W), vld1q_u32(p + 2 * W), vld1q_u32(p + 3 * W), c[0][g], c[1][g], c[2][g], c[3][g]);
      }
      for (int j = 0; j < 4; ++j) {
        u32* o = dst + (W - 1 - (sx + j)) * stride + sy;
        vst1q_u32(o, c[j][0]); vst1q_u32(o + 4, c[j][1]); vst1q_u32(o + 8, c[j][2]); vst1q_u32(o + 12, c[j][3]);
      }
    }
  }
}
void rot90(const u32* src, u32* dst, int stride) {
  for (int sy = 0; sy < H; sy += 16) {
    const u32* r = src + sy * W;
    for (int sx = 0; sx < W; sx += 4) {
      uint32x4_t c[4][4];
      for (int g = 0; g < 4; ++g) {
        const u32* p = r + g * 4 * W + sx;
        tr4(vld1q_u32(p + 3 * W), vld1q_u32(p + 2 * W), vld1q_u32(p + W), vld1q_u32(p), c[0][g], c[1][g], c[2][g], c[3][g]);
      }
      for (int j = 0; j < 4; ++j) {
        u32* o = dst + (sx + j) * stride + (H - 16 - sy);
        vst1q_u32(o, c[j][3]); vst1q_u32(o + 4, c[j][2]); vst1q_u32(o + 8, c[j][1]); vst1q_u32(o + 12, c[j][0]);
      }
    }
  }
}
#else
void rot270(const u32* src, u32* dst, int stride) {
  for (int py = 0; py < W; ++py) for (int px = 0; px < H; ++px) dst[py * stride + px] = src[px * W + (W - 1 - py)];
}
void rot90(const u32* src, u32* dst, int stride) {
  for (int py = 0; py < W; ++py) for (int px = 0; px < H; ++px) dst[py * stride + px] = src[(H - 1 - px) * W + py];
}
#endif
// 0 and 180 keep the DS orientation: whole-row copies.
void rot0(const u32* src, u32* dst, int stride) {
  for (int y = 0; y < H; ++y) std::memcpy(dst + y * stride, src + y * W, W * sizeof(u32));
}
void rot180(const u32* src, u32* dst, int stride) {
  for (int y = 0; y < H; ++y) {
    const u32* s = src + (H - 1 - y) * W;
    u32* d = dst + y * stride;
    for (int x = 0; x < W; ++x) d[x] = s[W - 1 - x];
  }
}

} // namespace

bool DispOut::available() {
#if defined(__linux__)
  static int cached = -1;
  if (cached >= 0) return cached != 0;
  cached = 0;
  const int d = ::open("/dev/disp", O_RDWR | O_CLOEXEC);
  if (d < 0) return false;
  DispLayerInfo info{};
  const bool ok = layer_ioctl(d, CMD_LAYER_GET_INFO, 0, &info) == 0 && ::access("/dev/fb0", R_OK | W_OK) == 0;
  ::close(d);
  cached = ok ? 1 : 0;
  return ok;
#else
  return false;
#endif
}

bool DispOut::open(int rot, bool vsync) {
#if defined(__linux__)
  if (!available()) return false;
  if (rot != 0 && rot != 90 && rot != 180 && rot != 270) { std::fprintf(stderr, "disp: rotation %d not supported\n", rot); return false; }
  disp_ = ::open("/dev/disp", O_RDWR | O_CLOEXEC);
  fb_ = ::open("/dev/fb0", O_RDWR | O_CLOEXEC);
  if (disp_ < 0 || fb_ < 0) { std::perror("disp: open"); close(); return false; }
  fb_fix_screeninfo fix{};
  if (ioctl(fb_, FBIOGET_FSCREENINFO, &fix) != 0) { std::perror("disp: FBIOGET_FSCREENINFO"); close(); return false; }
  unsigned long a[4] = {0, 0, 0, 0};
  panel_w_ = static_cast<int>(ioctl(disp_, CMD_GET_SCN_WIDTH, a));
  panel_h_ = static_cast<int>(ioctl(disp_, CMD_GET_SCN_HEIGHT, a));
  if (panel_w_ <= 0 || panel_h_ <= 0) { std::fprintf(stderr, "disp: no screen size\n"); close(); return false; }
  rot_ = rot; vsync_ = vsync;
  timing_ = std::getenv("DS_DISP_TIMING") != nullptr;
  diag_ = std::getenv("DS_DISP_DIAG") != nullptr; diag_mark_ = now_ns();
  buf_bytes_ = static_cast<size_t>(W) * H * VIEWS * sizeof(u32);   // the largest canvas: two screens
  if (buf_bytes_ * BUFS > fix.smem_len) { std::fprintf(stderr, "disp: fb0 too small for %d composites\n", BUFS); close(); return false; }
  map_len_ = fix.smem_len;
  void* m = mmap(nullptr, map_len_, PROT_READ | PROT_WRITE, MAP_SHARED, fb_, 0);
  if (m == MAP_FAILED) { std::perror("disp: mmap fb0"); map_ = nullptr; close(); return false; }
  map_ = static_cast<u8*>(m);
  phys_ = static_cast<u32>(fix.smem_start);

  // The UI's layer (the one fb0 drives) goes off while ours carries the
  // composite; both come back on close().
  unsigned long hdl = 0;
  if (ioctl(fb_, FBIOGET_LAYER_HDL_0, &hdl) == 0) ui_layer_ = static_cast<int>(hdl);
  for (int l = 0; l < 4; ++l) if (l != ui_layer_) { layer_ = l; break; }
  grid_layer_ = -1; grid_enabled_ = false; grid_dirty_ = false; grid_dims_ = Dims{};
  ov_active_ = false; ov_taken_ = false; ov_stage_.clear(); ov_sent_.clear();
  if (grid_alpha_ || overlay_wanted_) {
    grid_off_ = buf_bytes_ * BUFS;
    const size_t need = static_cast<size_t>(panel_w_) * panel_h_ * sizeof(u32);
    if (grid_off_ + need <= fix.smem_len) {
      for (int l = 0; l < 4; ++l) if (l != ui_layer_ && l != layer_) { grid_layer_ = l; break; }
      grid_dirty_ = true;
    }
    if (grid_layer_ < 0) std::fprintf(stderr, "disp: no room or layer for the overlay; grid and menu layer off\n");
  }
  {
    const bool turned = rot_ == 90 || rot_ == 270;
    ov_w_ = turned ? panel_h_ : panel_w_;
    ov_h_ = turned ? panel_w_ : panel_h_;
  }
  for (auto& v : views_) v = ViewRect{};
  for (auto& d : dims_) d = Dims{};
  canvas_w_ = W; canvas_h_ = H;
  dirty_ = (1u << BUFS) - 1;
  const size_t n = buf_bytes_ / sizeof(u32);
  for (int b = 0; b < BUFS; ++b) { u32* p = buf_ptr(b); for (size_t i = 0; i < n; ++i) p[i] = 0xFF000000u; }

  if (nearest_wanted_ && !open_frontend()) std::fprintf(stderr, "disp: cannot reach the scaler's coefficient RAM; the driver's filter shows\n");

  if (ui_layer_ >= 0) ui_was_enabled_ = layer_ioctl(disp_, CMD_LAYER_DISABLE, static_cast<unsigned>(ui_layer_), nullptr) == 0;
  cur_ = 0;
  next_ns_ = now_ns();
  displayed_ = 0; latched_ = queued_ = pending_ = -1; stop_ = false;
  if (vsync_) thread_ = std::thread([this] { presenter(); });
  return true;
#else
  (void)rot; (void)vsync;
  return false;
#endif
}

void DispOut::close() {
#if defined(__linux__)
  if (thread_.joinable()) {
    { std::lock_guard<std::mutex> g(mu_); stop_ = true; }
    cv_.notify_all();
    thread_.join();
  }
  if (disp_ >= 0) {
    if (layer_ >= 0) layer_ioctl(disp_, CMD_LAYER_DISABLE, static_cast<unsigned>(layer_), nullptr);
    if (grid_layer_ >= 0 && grid_enabled_) layer_ioctl(disp_, CMD_LAYER_DISABLE, static_cast<unsigned>(grid_layer_), nullptr);
    if (ui_layer_ >= 0 && ui_was_enabled_) layer_ioctl(disp_, CMD_LAYER_ENABLE, static_cast<unsigned>(ui_layer_), nullptr);
    ::close(disp_);
  }
  if (map_) munmap(map_, map_len_);
  if (fb_ >= 0) ::close(fb_);
  if (fe_) munmap(const_cast<u32*>(fe_), FE_MAP);
  if (mem_ >= 0) ::close(mem_);
#endif
  disp_ = fb_ = mem_ = -1; map_ = nullptr; fe_ = nullptr; map_len_ = 0; layer_ = ui_layer_ = -1; ui_was_enabled_ = false; layer_enabled_ = false;
  grid_layer_ = -1; grid_enabled_ = false;
}

void DispOut::set_canvas(int w, int h) {
  if (w <= 0 || h <= 0 || static_cast<size_t>(w) * h > static_cast<size_t>(W) * H * VIEWS) return;
  if (w == canvas_w_ && h == canvas_h_) return;
  canvas_w_ = w; canvas_h_ = h;
  dirty_ = (1u << BUFS) - 1;   // gaps between views must be black again
  grid_dirty_ = true;
}

void DispOut::set_view(int i, int x, int y, int w, int h, bool shown) {
  if (i < 0 || i >= VIEWS) return;
  const ViewRect& o = views_[i];
  // A moved/resized/hidden view leaves old pixels behind in every buffer
  // (set_canvas() alone wouldn't black them out on a same-size layout change).
  if (o.x != x || o.y != y || o.w != w || o.h != h || o.shown != shown) { dirty_ = (1u << BUFS) - 1; grid_dirty_ = true; }
  views_[i] = ViewRect{x, y, w, h, shown, o.cell};
}

void DispOut::set_divisor(int d) {
  if (d < 1) d = 1;
  if (d == div_) return;
  div_ = d;
  dirty_ = (1u << BUFS) - 1;
  grid_dirty_ = true;
}

DispOut::Dims DispOut::comp_dims() const {
  const bool turned = rot_ == 90 || rot_ == 270;
  return Dims{(turned ? canvas_h_ : canvas_w_) / div_, (turned ? canvas_w_ : canvas_h_) / div_};
}

void DispOut::set_view_cell(int i, int cell) {
  if (i < 0 || i >= VIEWS || cell < 1) return;
  if (views_[i].cell != cell) grid_dirty_ = true;
  views_[i].cell = cell;
}

double DispOut::fit_scale() const {
  const bool turned = rot_ == 90 || rot_ == 270;
  const Dims d{turned ? canvas_h_ : canvas_w_, turned ? canvas_w_ : canvas_h_};   // in DS pixels, whatever the divisor
  if (d.w <= 0 || d.h <= 0) return 0.0;
  return snap(std::min(static_cast<double>(panel_w_) / d.w, static_cast<double>(panel_h_) / d.h));
}

// Snaps to whole panel pixels per DS pixel (Display::IntScale). `s` is a
// DS-pixel factor; the composite's own factor is s times the divisor.
double DispOut::snap(double s) const {
  if (int_scale_ == 0 || s <= 0.0) return s;
  const double f = std::floor(s + 1e-9);
  if (s - f < 1e-9) return f;
  return int_scale_ == 1 ? std::max(1.0, f) : f + 1.0;
}

DispOut::Fit DispOut::fit(Dims d) const {
  // `d` is in composite pixels; the snap is taken on the DS factor and put back.
  Fit f;
  const double s = snap(std::min(static_cast<double>(panel_w_) / d.w, static_cast<double>(panel_h_) / d.h) / div_) * div_;
  f.sx = 0; f.sy = 0; f.sw = static_cast<unsigned>(d.w); f.sh = static_cast<unsigned>(d.h);
  f.w = static_cast<unsigned>(d.w * s); f.h = static_cast<unsigned>(d.h * s);
  // Overscale: layer shows a centred window of whole composite pixels (keeps scale exactly s).
  if (f.w > static_cast<unsigned>(panel_w_)) { f.sw = static_cast<unsigned>(panel_w_ / s); f.w = static_cast<unsigned>(f.sw * s); f.sx = static_cast<int>((d.w - static_cast<int>(f.sw)) / 2); }
  if (f.h > static_cast<unsigned>(panel_h_)) { f.sh = static_cast<unsigned>(panel_h_ / s); f.h = static_cast<unsigned>(f.sh * s); f.sy = static_cast<int>((d.h - static_cast<int>(f.sh)) / 2); }
  f.x = static_cast<int>((panel_w_ - static_cast<int>(f.w)) / 2); f.y = static_cast<int>((panel_h_ - static_cast<int>(f.h)) / 2);
  return f;
}

void DispOut::comp_rect(const ViewRect& r0, int& cx, int& cy, int& cw, int& ch) const {
  const bool turned = rot_ == 90 || rot_ == 270;
  const ViewRect r{r0.x / div_, r0.y / div_, r0.w / div_, r0.h / div_, r0.shown, r0.cell};
  const int cw_ = canvas_w_ / div_, ch_ = canvas_h_ / div_;
  switch (rot_) {
    case 270: cx = r.y; cy = cw_ - (r.x + r.w); break;
    case 90:  cx = ch_ - (r.y + r.h); cy = r.x; break;
    case 180: cx = cw_ - (r.x + r.w); cy = ch_ - (r.y + r.h); break;
    default:  cx = r.x; cy = r.y; break;
  }
  cw = turned ? r.h : r.w; ch = turned ? r.w : r.h;
}

// Composite pixel c covers panel run [ceil(c*pw/n), ceil((c+1)*pw/n)); the
// run's first pixel is a seam when the run is >= ceil(pw/n) long
// (kern::scale_row_grid's rule), matching the scanline tiers. Composed in
// cached memory and copied to fb0 in bulk (uncached stores are slow).
void DispOut::draw_grid(Dims d) {
  if (grid_layer_ < 0 || d.w <= 0 || d.h <= 0) return;
  const size_t n = static_cast<size_t>(panel_w_) * panel_h_;
  grid_stage_.assign(n, 0u);
  if (!grid_alpha_) { grid_dims_ = d; grid_dirty_ = false; return; }
  const Fit f = fit(d);
  const u32 seam = static_cast<u32>(grid_alpha_) << 24;
  std::vector<u8> col_seam(static_cast<size_t>(panel_w_)), row_seam(static_cast<size_t>(panel_h_));
  auto seams = [&](int p0, int p1, int cells, u8* out) {
    // Panel range [p0, p1) shows `cells` composite pixels (or pairs).
    const int pw = p1 - p0;
    if (pw <= 0 || cells <= 0) return;
    const int min_run = (pw + cells - 1) / cells;
    // At exactly 2x, every other cell gets a seam (Gpu::scale_screen_line matches).
    const int pitch = pw == 2 * cells ? 2 : 1;
    for (int c = 0; c < cells; c += pitch) {
      const int a = (c * pw + cells - 1) / cells, b = ((c + 1) * pw + cells - 1) / cells;
      if (b - a >= min_run && p0 + a < p1) out[p0 + a] = 1;
    }
  };
  for (const ViewRect& r : views_) {
    if (!r.shown || r.w <= 0 || r.h <= 0) continue;
    int cx, cy, cw, ch;
    comp_rect(r, cx, cy, cw, ch);
    // Composite x lands on the panel at f.x + (x - f.sx) * f.w / f.sw.
    auto pxof = [&](int x) { return f.x + static_cast<int>(static_cast<s64>(x - f.sx) * static_cast<s64>(f.w) / static_cast<s64>(f.sw)); };
    auto pyof = [&](int y) { return f.y + static_cast<int>(static_cast<s64>(y - f.sy) * static_cast<s64>(f.h) / static_cast<s64>(f.sh)); };
    const int px0 = pxof(cx), px1 = pxof(cx + cw);
    const int py0 = pyof(cy), py1 = pyof(cy + ch);
    const int cell = (W % r.cell == 0 && H % r.cell == 0) ? r.cell : 1;
    for (int y = std::max(0, py0); y < std::min(panel_h_, py1); ++y)
      std::fill(grid_stage_.data() + static_cast<size_t>(y) * panel_w_ + std::max(0, px0), grid_stage_.data() + static_cast<size_t>(y) * panel_w_ + std::min(panel_w_, px1), 0u);
    const bool turned = rot_ == 90 || rot_ == 270;
    if (px1 - px0 < (turned ? H : W) || py1 - py0 < (turned ? W : H)) continue;
    const int nx = cw / cell, ny = ch / cell;
    std::fill(col_seam.begin(), col_seam.end(), 0); std::fill(row_seam.begin(), row_seam.end(), 0);
    seams(std::max(0, px0), std::min(panel_w_, px1), nx, col_seam.data());
    seams(std::max(0, py0), std::min(panel_h_, py1), ny, row_seam.data());
    for (int y = std::max(0, py0); y < std::min(panel_h_, py1); ++y) {
      u32* row = grid_stage_.data() + static_cast<size_t>(y) * panel_w_;
      if (row_seam[y]) { for (int x = std::max(0, px0); x < std::min(panel_w_, px1); ++x) row[x] = seam; continue; }
      for (int x = std::max(0, px0); x < std::min(panel_w_, px1); ++x) if (col_seam[x]) row[x] = seam;
    }
  }
  grid_dims_ = d;
  grid_dirty_ = false;
}

bool DispOut::overlay(u32*& px, int& pitch, int& w, int& h) {
  if (!overlay_available() || ov_w_ <= 0 || ov_h_ <= 0) return false;
  const size_t n = static_cast<size_t>(ov_w_) * ov_h_;
  if (ov_stage_.size() != n) ov_stage_.assign(n, 0u);
  else if (!ov_taken_) std::fill(ov_stage_.begin(), ov_stage_.end(), 0u);
  ov_taken_ = true;
  px = ov_stage_.data(); pitch = ov_w_; w = ov_w_; h = ov_h_;
  return true;
}

void DispOut::overlay_changed(bool any) {
  if (!overlay_available()) return;
  if (!any && !ov_taken_ && !ov_active_) return;   // nothing drawn, nothing showing
  if (!any && !ov_stage_.empty()) std::fill(ov_stage_.begin(), ov_stage_.end(), 0u);
  ov_active_ = any;
  ov_taken_ = false;
  grid_dirty_ = true;   // recompose before the next flip
}

// Skips the copy to fb0 (uncached) when unchanged, checked against a cached copy.
void DispOut::compose_overlay() {
#if defined(__linux__)
  if (grid_layer_ < 0) return;
  const size_t n = static_cast<size_t>(panel_w_) * panel_h_;
  if (grid_stage_.size() != n) return;
  if (ov_active_ && ov_stage_.size() == static_cast<size_t>(ov_w_) * ov_h_) {
    // (x, y) on the surface -> the panel, the inverse of canvas_point's turn.
    for (int y = 0; y < ov_h_; ++y) {
      const u32* src = ov_stage_.data() + static_cast<size_t>(y) * ov_w_;
      for (int x = 0; x < ov_w_; ++x) {
        const u32 c = src[x];
        if (!(c >> 24)) continue;                 // transparent: the picture shows
        int px, py;
        switch (rot_) {
          case 270: px = y;             py = ov_w_ - 1 - x; break;
          case 90:  px = ov_h_ - 1 - y; py = x;             break;
          case 180: px = ov_w_ - 1 - x; py = ov_h_ - 1 - y; break;
          default:  px = x;             py = y;             break;
        }
        if (px >= 0 && px < panel_w_ && py >= 0 && py < panel_h_)
          grid_stage_[static_cast<size_t>(py) * panel_w_ + px] = c;
      }
    }
  }
  if (ov_sent_.size() == n && std::memcmp(ov_sent_.data(), grid_stage_.data(), n * sizeof(u32)) == 0) return;
  std::memcpy(map_ + grid_off_, grid_stage_.data(), n * sizeof(u32));
  ov_sent_.assign(grid_stage_.begin(), grid_stage_.end());
#endif
}

bool DispOut::set_grid_layer() {
#if defined(__linux__)
  if (grid_layer_ < 0) return false;
  DispLayerInfo info{};
  info.mode = 0;   // normal: 1:1, the panel's own pixels
  info.pipe = 0; info.zorder = 1; info.alpha_mode = 0; info.alpha_value = 255; info.ck_enable = 0;
  info.screen_win = {0, 0, static_cast<unsigned>(panel_w_), static_cast<unsigned>(panel_h_)};
  info.fb.addr[0] = phys_ + static_cast<u32>(grid_off_);
  info.fb.size = {static_cast<unsigned>(panel_w_), static_cast<unsigned>(panel_h_)};
  info.fb.format = FORMAT_ARGB_8888;
  info.fb.src_win = {0, 0, static_cast<unsigned>(panel_w_), static_cast<unsigned>(panel_h_)};
  if (layer_ioctl(disp_, CMD_LAYER_SET_INFO, static_cast<unsigned>(grid_layer_), &info) != 0) { std::perror("disp: grid LAYER_SET_INFO"); grid_layer_ = -1; return false; }
  if (layer_ioctl(disp_, CMD_LAYER_ENABLE, static_cast<unsigned>(grid_layer_), nullptr) != 0) { std::perror("disp: grid LAYER_ENABLE"); grid_layer_ = -1; return false; }
  grid_enabled_ = true;
  return true;
#else
  return false;
#endif
}

bool DispOut::set_layer(u32 addr, Dims d) {
#if defined(__linux__)
  if (d.w <= 0 || d.h <= 0) return false;
  const Fit f = fit(d);
  DispLayerInfo info{};
  info.mode = LAYER_MODE_SCALER;
  info.pipe = 1; info.zorder = 0; info.alpha_mode = 1; info.alpha_value = 255; info.ck_enable = 0;
  info.screen_win = {f.x, f.y, f.w, f.h};
  info.fb.addr[0] = addr;
  info.fb.size = {static_cast<unsigned>(d.w), static_cast<unsigned>(d.h)};
  info.fb.format = FORMAT_ARGB_8888;
  info.fb.src_win = {f.sx, f.sy, f.sw, f.sh};   // a window of the composite under overscale, else all of it
  if (layer_ioctl(disp_, CMD_LAYER_SET_INFO, static_cast<unsigned>(layer_), &info) != 0) { std::perror("disp: LAYER_SET_INFO"); return false; }
  if (fe_) write_coefs();   // over the driver's own table
  if (!layer_enabled_) {
    if (layer_ioctl(disp_, CMD_LAYER_ENABLE, static_cast<unsigned>(layer_), nullptr) != 0) { std::perror("disp: LAYER_ENABLE"); return false; }
    layer_enabled_ = true;
    if (grid_layer_ >= 0 && !grid_enabled_) set_grid_layer();   // over the composite, once it shows
  }
  return true;
#else
  (void)addr; (void)d; return false;
#endif
}

void DispOut::flip(int buf) { set_layer(buf_addr(buf), dims_[buf]); }

bool DispOut::open_frontend() {
#if defined(__linux__)
  mem_ = ::open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC);
  if (mem_ < 0) return false;
  void* m = mmap(nullptr, FE_MAP, PROT_READ | PROT_WRITE, MAP_SHARED, mem_, FE_BASE);
  if (m == MAP_FAILED) { ::close(mem_); mem_ = -1; return false; }
  fe_ = static_cast<volatile u32*>(m);
  return true;
#else
  return false;
#endif
}


// Nearest neighbour as a polyphase table: full weight (64) on the centre
// sample in every phase. Tap positions probed on the A30 (RAM is
// write-only): horizontally the centre is tap 4, vertically byte 1 of the
// one register. RAM is behind an access control (frm_ctrl bit 23, status
// bit 11 grants it) that locks the scaler out while raised, so raise/write/
// lower as the driver does; presenter() times the swap right after the
// vsync return to keep the lockout confined near the frame's top. The
// ready bit is a sun4i thing the A33 lacks.
void DispOut::write_coefs() {
#if defined(__linux__)
  fe_[FE_FRM_CTRL] = fe_[FE_FRM_CTRL] | FE_COEF_ACCESS;
  int spin = 0;
  for (; spin < 100000 && !(fe_[FE_STATUS] & FE_COEF_ACCESS_OK); ++spin) {}
  if (diag_ && spin == 100000) std::fprintf(stderr, "disp: coef access never granted (frm_ctrl %08x status %08x)\n", fe_[FE_FRM_CTRL], fe_[FE_STATUS]);
  for (int i = 0; i < 32; ++i) {
    fe_[FE_CH0_HORZCOEF0 + i] = 0; fe_[FE_CH0_HORZCOEF1 + i] = 64u; fe_[FE_CH0_VERTCOEF + i] = 64u << 8;
    fe_[FE_CH1_HORZCOEF0 + i] = 0; fe_[FE_CH1_HORZCOEF1 + i] = 64u; fe_[FE_CH1_VERTCOEF + i] = 64u << 8;
  }
  fe_[FE_FRM_CTRL] = fe_[FE_FRM_CTRL] & ~FE_COEF_ACCESS;
#endif
}

void DispOut::canvas_point(int compx, int compy, int& x, int& y) const {
  switch (rot_) {
    case 270: x = canvas_w_ - 1 - compy; y = compx; break;
    case 90:  x = compy; y = canvas_h_ - 1 - compx; break;
    case 180: x = canvas_w_ - 1 - compx; y = canvas_h_ - 1 - compy; break;
    default:  x = compx; y = compy; break;
  }
}

const u32* DispOut::under_pixel(int x, int y, int index, const u32* const fbs[VIEWS]) const {
  for (int v = index - 1; v >= 0; --v) {
    const ViewRect& u = views_[v];
    if (!fbs[v] || !u.shown || u.w != W || u.h != H) continue;
    if (x < u.x || x >= u.x + u.w || y < u.y || y >= u.y + u.h) continue;
    return fbs[v] + static_cast<size_t>(y - u.y) * W + (x - u.x);
  }
  return nullptr;
}

void DispOut::draw_view(u32* comp, int comp_w, const ViewRect& r0, const u32* fb, int index, const u32* const fbs[VIEWS]) {
  const bool turned = rot_ == 90 || rot_ == 270;
  int cx, cy, cw, ch;
  comp_rect(r0, cx, cy, cw, ch);
  u32* dst = comp + static_cast<size_t>(cy) * comp_w + cx;
  // Under a divisor a 1:1 view takes the downscale below (box average = chunky mean).
  const ViewRect r{r0.x / div_, r0.y / div_, r0.w / div_, r0.h / div_, r0.shown, r0.cell};
  if (r.w == W && r.h == H) {
    switch (rot_) {
      case 270: rot270(fb, dst, comp_w); break;
      case 90:  rot90(fb, dst, comp_w); break;
      case 180: rot180(fb, dst, comp_w); break;
      default:  rot0(fb, dst, comp_w); break;
    }
    return;
  }
  if (r.w <= 0 || r.h <= 0 || r.w > W || r.h > H) return;
  // Downscale, unrotated, into tmp_ (r.w x r.h). Power-of-two factors get a
  // NEON path: rounding halving adds per pass (a later pass reads tmp4_).
  tmp_.resize(static_cast<size_t>(r.w) * r.h);
#if DS_DISP_NEON
  int halvings = 0;
  for (int w = W, h = H; w > r.w && h > r.h && w == 2 * (w / 2) && h == 2 * (h / 2); w /= 2, h /= 2) ++halvings;
  if (halvings > 0 && (W >> halvings) == r.w && (H >> halvings) == r.h) {
    const u32* src = fb;
    int sw = W, sh = H;
    for (int pass = 0; pass < halvings; ++pass) {
      const int ow = sw / 2, oh = sh / 2;
      u32* out = pass == halvings - 1 ? tmp_.data() : (tmp4_.resize(static_cast<size_t>(ow) * oh), tmp4_.data());
      for (int dy = 0; dy < oh; ++dy) {
        const u32* s0 = src + static_cast<size_t>(dy * 2) * sw;
        const u32* s1 = s0 + sw;
        u32* o = out + static_cast<size_t>(dy) * ow;
        int dx = 0;
        for (; dx + 4 <= ow; dx += 4) {
          const uint32x4x2_t a = vld2q_u32(s0 + dx * 2), b = vld2q_u32(s1 + dx * 2);
          const uint8x16_t ha = vrhaddq_u8(vreinterpretq_u8_u32(a.val[0]), vreinterpretq_u8_u32(a.val[1]));
          const uint8x16_t hb = vrhaddq_u8(vreinterpretq_u8_u32(b.val[0]), vreinterpretq_u8_u32(b.val[1]));
          vst1q_u32(o + dx, vorrq_u32(vreinterpretq_u32_u8(vrhaddq_u8(ha, hb)), vdupq_n_u32(0xFF000000u)));
        }
        for (; dx < ow; ++dx) {
          const u32 p00 = s0[dx * 2], p01 = s0[dx * 2 + 1], p10 = s1[dx * 2], p11 = s1[dx * 2 + 1];
          u32 v = 0xFF000000u;
          for (int sh8 = 0; sh8 < 24; sh8 += 8) v |= ((((p00 >> sh8) & 0xFF) + ((p01 >> sh8) & 0xFF) + ((p10 >> sh8) & 0xFF) + ((p11 >> sh8) & 0xFF) + 2) / 4) << sh8;
          o[dx] = v;
        }
      }
      src = out; sw = ow; sh = oh;
    }
  } else
#endif
  for (int dy = 0; dy < r.h; ++dy) {
    const int sy0 = dy * H / r.h, sy1 = std::max(sy0 + 1, (dy + 1) * H / r.h);
    for (int dx = 0; dx < r.w; ++dx) {
      const int sx0 = dx * W / r.w, sx1 = std::max(sx0 + 1, (dx + 1) * W / r.w);
      unsigned rs = 0, gs = 0, bs = 0, n = 0;
      for (int sy = sy0; sy < sy1; ++sy) for (int sx = sx0; sx < sx1; ++sx) {
        const u32 p = fb[sy * W + sx];
        rs += (p >> 16) & 0xFF; gs += (p >> 8) & 0xFF; bs += p & 0xFF; ++n;
      }
      tmp_[static_cast<size_t>(dy) * r.w + dx] = 0xFF000000u | ((rs / n) << 16) | ((gs / n) << 8) | (bs / n);
    }
  }
  const int tw = turned ? r.h : r.w, th = turned ? r.w : r.h;
  tmp2_.resize(static_cast<size_t>(tw) * th);
  for (int py = 0; py < th; ++py) for (int px = 0; px < tw; ++px) {
    int vx, vy;
    switch (rot_) {
      case 270: vx = r.w - 1 - py; vy = px; break;
      case 90:  vx = py; vy = r.h - 1 - px; break;
      case 180: vx = r.w - 1 - px; vy = r.h - 1 - py; break;
      default:  vx = px; vy = py; break;
    }
    tmp2_[static_cast<size_t>(py) * tw + px] = tmp_[static_cast<size_t>(vy) * r.w + vx];
  }
  // Blended over the view under it (drawn earlier) when the inset is
  // translucent, reading that view's own framebuffer rather than the
  // (uncached) composite; falls back to the composite where nothing is under.
  if (inset_alpha_ == 255 || div_ > 1) {
    for (int py = 0; py < th; ++py) std::memcpy(dst + static_cast<size_t>(py) * comp_w, tmp2_.data() + static_cast<size_t>(py) * tw, static_cast<size_t>(tw) * sizeof(u32));
    return;
  }
  tmp3_.resize(static_cast<size_t>(tw));
  const int sx = rot_ == 0 ? 1 : rot_ == 180 ? -1 : 0, sy = rot_ == 270 ? 1 : rot_ == 90 ? -1 : 0;
  for (int py = 0; py < th; ++py) {
    u32* row = dst + static_cast<size_t>(py) * comp_w;
    int x0, y0;
    canvas_point(cx, cy + py, x0, y0);
    // Usual case: whole row in one 1:1 view, a strided gather. Else pixel by pixel.
    const u32* u0 = under_pixel(x0, y0, index, fbs);
    const u32* u1 = under_pixel(x0 + sx * (tw - 1), y0 + sy * (tw - 1), index, fbs);
    if (u0 && u1 && (u1 - u0) == static_cast<std::ptrdiff_t>(sx + sy * static_cast<int>(W)) * (tw - 1)) {
      const std::ptrdiff_t step = sx + sy * static_cast<int>(W);
      for (int px = 0; px < tw; ++px) tmp3_[static_cast<size_t>(px)] = u0[step * px];
    } else {
      for (int px = 0; px < tw; ++px) {
        const u32* u = under_pixel(x0 + sx * px, y0 + sy * px, index, fbs);
        tmp3_[static_cast<size_t>(px)] = u ? *u : row[px];
      }
    }
    Display::blend_row(tmp3_.data(), tmp2_.data() + static_cast<size_t>(py) * tw, static_cast<size_t>(tw), inset_alpha_);
    std::memcpy(row, tmp3_.data(), static_cast<size_t>(tw) * sizeof(u32));
  }
}

void DispOut::present(const u32* const fb[VIEWS]) {
#if defined(__linux__)
  if (!map_) return;
  int buf;
  if (thread_.joinable()) {
    std::lock_guard<std::mutex> g(mu_);
    if (pending_ >= 0) { buf = pending_; pending_ = -1; }   // not yet flipped: take it back and overwrite it (the panel skips that frame)
    else { buf = 0; while (buf == displayed_ || buf == latched_ || buf == queued_) ++buf; }
  } else buf = cur_ = (cur_ + 1) % BUFS;
  const Dims d = comp_dims();
  u32* comp = buf_ptr(buf);
  if (dirty_ & (1u << buf)) {
    const size_t n = static_cast<size_t>(d.w) * d.h;
    for (size_t i = 0; i < n; ++i) comp[i] = 0xFF000000u;
    dirty_ &= ~(1u << buf);
  }
  for (int v = 0; v < VIEWS; ++v) if (fb[v] && views_[v].shown) draw_view(comp, d.w, views_[v], fb[v], v, fb);
  dims_[buf] = d;
  // Redrawn in place; scanned out live, so a layout change may show one torn refresh.
  if (grid_layer_ >= 0 && (grid_dirty_ || d.w != grid_dims_.w || d.h != grid_dims_.h)) {
    draw_grid(d);
    compose_overlay();
  }
  if (diag_) ++diag_posts_;
  if (thread_.joinable()) {
    { std::lock_guard<std::mutex> g(mu_); pending_ = buf; }
    cv_.notify_one();
  } else flip(buf);
#else
  (void)fb;
#endif
}

void DispOut::presenter() {
#if defined(__linux__)
  // One flip per refresh, timed right after the vsync return so the
  // driver's own coefficient table (reasserted on every layer set) is
  // confined to a few lines rather than a mid-screen band before
  // write_coefs() puts ours back. Ioctls run outside the lock.
  raise_presenter_priority("disp: presenter priority");
  std::unique_lock<std::mutex> lk(mu_);
  for (;;) {
    cv_.wait(lk, [this] { return stop_ || pending_ >= 0; });
    if (stop_) return;
    queued_ = pending_; pending_ = -1;
    lk.unlock();
    const u64 tw = now_ns();
    wait_vsync();
    const u64 t0 = now_ns();
    flip(queued_);
    if (diag_) {                // DS_DISP_DIAG: log stalls and frame rate
      const u64 t1 = now_ns();
      ++diag_flips_;
      if (t0 - tw > 40000000ull || t1 - t0 > 8000000ull)
        std::fprintf(stderr, "disp: STALL vsync wait %.1f ms, flip %.1f ms (flip #%llu, posts %llu)\n", (t0 - tw) / 1e6, (t1 - t0) / 1e6, (unsigned long long)diag_flips_, (unsigned long long)diag_posts_);
      if (t1 - diag_mark_ > 5000000000ull) {
        std::fprintf(stderr, "disp: %llu flips, %llu posts in %.1f s; frm_ctrl %08x status %08x\n", (unsigned long long)diag_flips_, (unsigned long long)diag_posts_, (t1 - diag_mark_) / 1e9,
                     fe_ ? fe_[FE_FRM_CTRL] : 0u, fe_ ? fe_[FE_STATUS] : 0u);
        diag_mark_ = t1; diag_flips_ = diag_posts_ = 0;
      }
    }
    if (timing_) {              // DS_DISP_TIMING: swap time past the vsync return
      const u64 dt = now_ns() - t0;
      flip_ns_sum_ += dt; if (dt > flip_ns_max_) flip_ns_max_ = dt; ++flip_n_;
      if ((flip_n_ & 255) == 0) std::fprintf(stderr, "disp: flip after vsync mean %.0f us, max %.0f us (%llu)\n", flip_ns_sum_ / 1000.0 / flip_n_, flip_ns_max_ / 1000.0, static_cast<unsigned long long>(flip_n_));
    }
    lk.lock();
    if (latched_ >= 0) displayed_ = latched_;
    latched_ = queued_; queued_ = -1;
  }
#endif
}

void DispOut::wait_vsync() {
#if defined(__linux__)
  // FBIOPAN_DISPLAY on fb0 blocks for the next refresh on this driver, even
  // with the UI layer off; checked once, else pace by the clock.
  if (pan_blocks_) {
    fb_var_screeninfo var{};
    if (ioctl(fb_, FBIOGET_VSCREENINFO, &var) == 0) {
      const u64 t0 = now_ns();
      ioctl(fb_, FBIOPAN_DISPLAY, &var);
      if (!pan_measured_) {
        pan_measured_ = true;
        const u64 dt = now_ns() - t0;
        if (dt < 1000000ull) {
          pan_blocks_ = false;
          std::fprintf(stderr, "disp: FBIOPAN_DISPLAY does not wait for vsync; pacing by the clock\n");
        }
      }
      if (pan_blocks_) return;
    } else pan_blocks_ = false;
  }
  constexpr u64 PERIOD = 16666667ull;
  const u64 t = now_ns();
  if (next_ns_ < t - PERIOD) next_ns_ = t;
  next_ns_ += PERIOD;
  if (next_ns_ > t) {
    const u64 d = next_ns_ - t;
    timespec ts{static_cast<time_t>(d / 1000000000ull), static_cast<long>(d % 1000000000ull)};
    nanosleep(&ts, nullptr);
  }
#endif
}

} // namespace ds::sdl
