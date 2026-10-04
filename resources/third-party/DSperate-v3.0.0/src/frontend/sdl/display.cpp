// SPDX-License-Identifier: GPL-3.0-or-later
#include "frontend/sdl/gpu_present.h"
#include "display.h"
#include "display_wl.h"
#include "present_thread.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <cstring>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define DS_ROT_NEON 1
#else
#define DS_ROT_NEON 0
#endif

namespace ds::sdl {

// View rects go to SDL as SDL_Rect.
static_assert(sizeof(frontend::Rect) == sizeof(SDL_Rect) && offsetof(frontend::Rect, w) == offsetof(SDL_Rect, w) && offsetof(frontend::Rect, h) == offsetof(SDL_Rect, h),
              "frontend::Rect must match SDL_Rect");

// ---- rotation ----------------------------------------------------------------
// Same mapping the display-engine tier uses (display_disp.cpp), so DS_ROTATE
// means the same on both: 270: dst[py][px] = src[px][lw-1-py]; 90:
// dst[py][px] = src[lh-1-px][py]; 180: dst[py][px] = src[lh-1-py][lw-1-px].
// Presented memory is uncached on the scanout tiers, so 90/270 store whole
// 64-byte lines via 4x4 NEON transposes; edges go scalar.
namespace {

#if DS_ROT_NEON
inline void tr4(uint32x4_t r0, uint32x4_t r1, uint32x4_t r2, uint32x4_t r3, uint32x4_t& c0, uint32x4_t& c1, uint32x4_t& c2, uint32x4_t& c3) {
  const uint32x4x2_t a = vtrnq_u32(r0, r1), b = vtrnq_u32(r2, r3);
  c0 = vcombine_u32(vget_low_u32(a.val[0]), vget_low_u32(b.val[0]));
  c1 = vcombine_u32(vget_low_u32(a.val[1]), vget_low_u32(b.val[1]));
  c2 = vcombine_u32(vget_high_u32(a.val[0]), vget_high_u32(b.val[0]));
  c3 = vcombine_u32(vget_high_u32(a.val[1]), vget_high_u32(b.val[1]));
}
#endif

void rot270(const u32* src, int lw, int lh, u32* dst, int dp) {
  int sy = 0, sxb = 0;
#if DS_ROT_NEON
  const int lh16 = lh & ~15, lw4 = lw & ~3;
  for (; sy < lh16; sy += 16) {
    const u32* r = src + static_cast<size_t>(sy) * lw;
    for (int sx = 0; sx < lw4; sx += 4) {
      uint32x4_t c[4][4];
      for (int g = 0; g < 4; ++g) {
        const u32* p = r + g * 4 * lw + sx;
        tr4(vld1q_u32(p), vld1q_u32(p + lw), vld1q_u32(p + 2 * lw), vld1q_u32(p + 3 * lw), c[0][g], c[1][g], c[2][g], c[3][g]);
      }
      for (int j = 0; j < 4; ++j) {
        u32* o = dst + static_cast<size_t>(lw - 1 - (sx + j)) * dp + sy;
        vst1q_u32(o, c[j][0]); vst1q_u32(o + 4, c[j][1]); vst1q_u32(o + 8, c[j][2]); vst1q_u32(o + 12, c[j][3]);
      }
    }
  }
  sxb = lw4;
#endif
  for (int y = sy; y < lh; ++y) for (int x = 0; x < lw; ++x) dst[static_cast<size_t>(lw - 1 - x) * dp + y] = src[static_cast<size_t>(y) * lw + x];
  for (int y = 0; y < sy; ++y) for (int x = sxb; x < lw; ++x) dst[static_cast<size_t>(lw - 1 - x) * dp + y] = src[static_cast<size_t>(y) * lw + x];
}

void rot90(const u32* src, int lw, int lh, u32* dst, int dp) {
  int sy = 0, sxb = 0;
#if DS_ROT_NEON
  const int lh16 = lh & ~15, lw4 = lw & ~3;
  for (; sy < lh16; sy += 16) {
    const u32* r = src + static_cast<size_t>(sy) * lw;
    for (int sx = 0; sx < lw4; sx += 4) {
      uint32x4_t c[4][4];
      for (int g = 0; g < 4; ++g) {
        const u32* p = r + g * 4 * lw + sx;
        tr4(vld1q_u32(p + 3 * lw), vld1q_u32(p + 2 * lw), vld1q_u32(p + lw), vld1q_u32(p), c[0][g], c[1][g], c[2][g], c[3][g]);
      }
      for (int j = 0; j < 4; ++j) {
        u32* o = dst + static_cast<size_t>(sx + j) * dp + (lh - 16 - sy);
        vst1q_u32(o, c[j][3]); vst1q_u32(o + 4, c[j][2]); vst1q_u32(o + 8, c[j][1]); vst1q_u32(o + 12, c[j][0]);
      }
    }
  }
  sxb = lw4;
#endif
  for (int y = sy; y < lh; ++y) for (int x = 0; x < lw; ++x) dst[static_cast<size_t>(x) * dp + (lh - 1 - y)] = src[static_cast<size_t>(y) * lw + x];
  for (int y = 0; y < sy; ++y) for (int x = sxb; x < lw; ++x) dst[static_cast<size_t>(x) * dp + (lh - 1 - y)] = src[static_cast<size_t>(y) * lw + x];
}

void rot180(const u32* src, int lw, int lh, u32* dst, int dp) {
  for (int y = 0; y < lh; ++y) {
    const u32* r = src + static_cast<size_t>(lh - 1 - y) * lw;
    u32* o = dst + static_cast<size_t>(y) * dp;
    for (int x = 0; x < lw; ++x) o[x] = r[lw - 1 - x];
  }
}

} // namespace

void Display::rotate_out(u32* dst, u32 dst_pitch) const {
  const u32* src = stage_.data();
  const int lw = scaled_w_, lh = scaled_h_, dp = static_cast<int>(dst_pitch);
  switch (rot_) {
    case 90:  rot90(src, lw, lh, dst, dp); break;
    case 180: rot180(src, lw, lh, dst, dp); break;
    case 270: rot270(src, lw, lh, dst, dp); break;
    default:  for (int y = 0; y < lh; ++y) std::memcpy(dst + static_cast<size_t>(y) * dp, src + static_cast<size_t>(y) * lw, static_cast<size_t>(lw) * sizeof(u32));
  }
}

void Display::output_size(int& w, int& h) const {
  out_size(w, h);
  if (rotated()) std::swap(w, h);
}

namespace {
// The same DS_VERBOSE gate as main.cpp's VLOG.
bool verbose() { static const bool v = std::getenv("DS_VERBOSE") != nullptr; return v; }
} // namespace

double Display::dominant_ratio() const {
  if (layout_.mode != Mode::DominantV && layout_.mode != Mode::DominantH) return 1.0;
  if (!layout_.dominant_auto) return layout_.dominant;
  const int p = layout_.primary, q = 1 - p;
  return views_[p].rect.w > 0 ? static_cast<double>(views_[q].rect.w) / views_[p].rect.w : layout_.dominant_min;
}

bool Display::open(const char* title, int scale, bool fullscreen, bool linear, bool vsync, const Layout& layout_mode, int only_screen, int display_index) {
  sync();
  layout_ = layout_mode;
  only_screen_ = only_screen;
  upper_panel_ = only_screen == 0;
  display_index_ = display_index;
  nviews_ = only_screen_ >= 0 ? 1 : SCREENS;
  sink_ = Sink::Count;
  const bool panel = only_screen_ < 0 && (panel_ == Sink::Disp || panel_ == Sink::Fbdev);
  int w = 0, h = 0;
  if (only_screen_ >= 0) { w = static_cast<int>(SCREEN_W) * scale; h = static_cast<int>(SCREEN_H) * scale; }
  else natural_size(layout_, scale, w, h);
  int rot = rot_wanted_;
  if (!rot) if (const char* r = std::getenv("DS_ROTATE")) rot = std::atoi(r);
  rot = ((rot % 360) + 360) % 360;
  if (rot != 0 && rot != 90 && rot != 180 && rot != 270) { std::fprintf(stderr, "video: rotation %d not supported; 0\n", rot); rot = 0; }
  if ((rot == 90 || rot == 270) && !panel) std::swap(w, h);
  const u32 flags = static_cast<u32>(SDL_WINDOW_RESIZABLE) | (fullscreen ? static_cast<u32>(SDL_WINDOW_FULLSCREEN_DESKTOP) : 0u);
  win_ = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED_DISPLAY(display_index), SDL_WINDOWPOS_CENTERED_DISPLAY(display_index), w, h, flags);
  if (!win_) { std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return false; }
  fullscreen_ = fullscreen;
  rot_ = 0;

  if (panel && panel_ == Sink::Disp && open_disp(linear, vsync, rot)) { sink_ = Sink::Disp; return true; }
  if (panel && panel_ == Sink::Fbdev) {
    if (open_fbdev(vsync, rot)) { sink_ = Sink::Fbdev; return true; }
    std::fprintf(stderr, "video.sink: /dev/fb0 not usable; using SDL\n");
  }

  const char* vd = SDL_GetCurrentVideoDriver();
  auto env = [](const char* k) { const char* e = std::getenv(k); return std::string(e ? e : ""); };
  const frontend::WindowPlan plan = frontend::plan_window({want_, vd ? vd : "", env("DS_DMABUF"), env("DS_SCANLINE_SCALE")});
  for (const Sink s : plan.order) {
    bool ok = false;
    switch (s) {
      case Sink::Kms:      ok = open_kms(rot); break;
      case Sink::Dmabuf:   ok = open_dmabuf(rot); break;
      case Sink::Surface:  ok = open_surface(rot); break;
      case Sink::Renderer: ok = open_renderer(linear, vsync, rot); break;
      default: break;
    }
    if (ok) { sink_ = s; return true; }
    if (plan.required && (s == Sink::Kms || s == Sink::Dmabuf)) {
      std::fprintf(stderr, "DS_DMABUF=1 but the %s sink failed\n", frontend::sink_name(s));
      return false;
    }
    if (s == Sink::Renderer) return false;
  }
  return false;
}

// Display engine: hardware scales a DS-resolution canvas, so there is no
// renderer/scaling here; views are laid out on the canvas and draw() draws
// into the layer's source.
bool Display::open_disp(bool linear, bool vsync, int rot) {
  if (!DispOut::available()) return false;
  auto d = std::make_unique<DispOut>();
  d->set_grid(disp_grid_);
  d->set_overlay(true);   // menu/OSD panel pixels: the only place on this chip to get them
  d->set_nearest(!linear);
  d->set_integer_scale(static_cast<int>(int_scale_));
  if (!d->open(rot, vsync)) return false;
  disp_ = std::move(d);
  layout();
  if (chunky_) build_source_scale();
  std::fprintf(stderr, "video: display-engine scaler (%s), rot %d, layout %s, %s driver, vsync %s%s%s\n",
               disp_->nearest() ? "nearest" : "driver filter", rot, mode_name(layout_.mode), SDL_GetCurrentVideoDriver(), vsync ? "on" : "off",
               !chunky_ ? "" : disp_->divisor() > 1 ? ", chunky in the scaler" : ", chunky at source",
               disp_->grid() ? ", grid layer" : disp_->overlay_available() ? ", overlay layer" : "");
  return true;
}

// fbdev: scanline path straight into fb0's buffers, for SDL2 builds whose
// only driver is Mali EGL over fbdev (H700 handhelds under BaseOS).
bool Display::open_fbdev(bool vsync, int rot) {
  auto fo = std::make_unique<FbdevOut>();
  if (!fo->open(win_, vsync)) return false;
  out_ = std::move(fo);
  scaled_ = true;
  rot_ = rot;
  layout();
  build_scale();
  std::fprintf(stderr, "video: fbdev scanout %dx%d, %s driver, scanline scaling, rot %d, vsync %s\n",
               out_->width(), out_->height(), SDL_GetCurrentVideoDriver(), rot_, vsync ? "on" : "off");
  return true;
}

// KMS page flips. Tried before any window surface is asked for: on KMSDRM
// SDL_GetWindowSurface *succeeds* by quietly building a GLES renderer.
bool Display::open_kms(int rot) {
  scaled_ = true;
  rot_ = rot;
  int ow = 0, oh = 0;
  SDL_GetWindowSize(win_, &ow, &oh);
  auto dr = std::make_unique<DrmOut>();
  if (gpu_wanted_) dr->set_bufs(DrmOut::DEFAULT_BUFS + 1);
  if (!dr->open(win_, ow, oh, display_index_)) { scaled_ = false; rot_ = 0; return false; }
  out_ = std::move(dr);
  layout();
  build_scale();
  if (try_gpu_present()) return true;
  std::fprintf(stderr, "video: kms scanout, %s driver, scanline scaling, rot %d\n", SDL_GetCurrentVideoDriver(), rot_);
  return true;
}

// The scanline path's pixels in a CMA dma-buf instead of the shm surface.
// Needs the window surface first (the size it's configured at), and falls
// back to it if the dma-buf is lost later.
bool Display::open_dmabuf(int rot) {
  if (!open_surface_scaling(rot)) return false;
  int ow = 0, oh = 0;
  output_size(ow, oh);   // the presented size, whatever the rotation
  auto dm = std::make_unique<DmabufOut>();
  if (gpu_wanted_) dm->set_bufs(DmabufOut::DEFAULT_BUFS + 1);
  if (!dm->open(win_, ow, oh, only_screen_ >= 0 ? display_index_ : -1)) return false;
  out_ = std::move(dm);
  SDL_GetWindowSize(win_, &open_w_, &open_h_);
  if (try_gpu_present()) return true;
  std::fprintf(stderr, "video: dmabuf, %s driver, scanline scaling, rot %d\n", SDL_GetCurrentVideoDriver(), rot_);
  return true;
}

bool Display::open_surface(int rot) {
  if (!open_surface_scaling(rot)) return false;
  std::fprintf(stderr, "video: window surface, %s driver, scanline scaling, rot %d\n", SDL_GetCurrentVideoDriver(), rot_);
  return true;
}

// Per-scanline scaling into SDL's window surface: the scaling tables built
// for it, or false (and nothing kept) where it has no usable surface.
bool Display::open_surface_scaling(int rot) {
  scaled_ = true;
  rot_ = rot;
  if (!SDL_GetWindowSurface(win_)) {
    std::fprintf(stderr, "window surface unavailable (%s)\n", SDL_GetError());
    scaled_ = false; rot_ = 0;
    return false;
  }
  layout();
  build_scale();
  if (!scaled_) { rot_ = 0; return false; }
  return true;
}

// SDL_Renderer: reached where no sink above applies (X11,
// DS_SCANLINE_SCALE=0, video.sink = renderer). GPU renderer tried first,
// software if it can't be created.
bool Display::open_renderer(bool linear, bool vsync, int rot) {
  scaled_ = false;
  rot_ = 0;
  if (rot) std::fprintf(stderr, "video: rotation %d is not applied on the renderer path\n", rot);
  const u32 vflag = vsync ? static_cast<u32>(SDL_RENDERER_PRESENTVSYNC) : 0u;
  ren_ = SDL_CreateRenderer(win_, -1, static_cast<u32>(SDL_RENDERER_ACCELERATED) | vflag);
  if (!ren_) {
    std::fprintf(stderr, "accelerated renderer unavailable (%s); falling back to software\n", SDL_GetError());
    ren_ = SDL_CreateRenderer(win_, -1, static_cast<u32>(SDL_RENDERER_SOFTWARE) | vflag);
  }
  if (!ren_) { std::fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError()); return false; }

  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, linear ? "linear" : "nearest");
  for (int i = 0; i < SCREENS; ++i) {
    // The core's framebuffers are 0xAARRGGBB words: ARGB8888.
    tex_[i] = SDL_CreateTexture(ren_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, static_cast<int>(SCREEN_W), static_cast<int>(SCREEN_H));
    if (!tex_[i]) { std::fprintf(stderr, "SDL_CreateTexture: %s\n", SDL_GetError()); return false; }
  }
  layout();

  SDL_RendererInfo info;
  if (SDL_GetRendererInfo(ren_, &info) == 0)
    std::fprintf(stderr, "video: %s renderer, %s driver, vsync %s\n", info.name, SDL_GetCurrentVideoDriver(), (info.flags & SDL_RENDERER_PRESENTVSYNC) ? "on" : "off");

  // Which GL stack actually drives the window (Mesa/panfrost vs a vendor blob) varies; name it.
  using GetString = const unsigned char* (*)(unsigned);
  if (auto gl_get_string = reinterpret_cast<GetString>(SDL_GL_GetProcAddress("glGetString"))) {
    const unsigned char* rend = gl_get_string(0x1F01);      // GL_RENDERER
    const unsigned char* ver  = gl_get_string(0x1F02);      // GL_VERSION
    if (rend) std::fprintf(stderr, "gl: %s | %s\n", rend, ver ? reinterpret_cast<const char*>(ver) : "?");
  }
  return true;
}

void Display::close() {
  sync();
  if (disp_) { disp_->close(); disp_.reset(); }
  gpu_.reset();   // flushes what it has in flight
  if (out_) { out_->close(); out_.reset(); }
  frame_px_ = nullptr;
  surf_ = nullptr;   // owned by SDL, freed with the window
  for (auto*& t : tex_) { if (t) SDL_DestroyTexture(t); t = nullptr; }
  if (ren_) { SDL_DestroyRenderer(ren_); ren_ = nullptr; }
  if (win_) { SDL_DestroyWindow(win_); win_ = nullptr; }
}

// Screen rects for the current mode: aspect preserved, centred.
void Display::layout() {
  if (disp_) { int cw = 0, ch = 0; natural_size(layout_, 1.0, cw, ch); disp_->set_canvas(cw, ch); }
  int w = 0, h = 0;
  if (!out_size(w, h)) return;
  if (only_screen_ >= 0) {
    views_[0] = frontend::place_single(only_screen_, w, h, int_scale_, upper_panel_);
    return;
  }
  place(layout_, w, h, views_, int_scale_);
  if (verbose()) for (int i = 0; i < nviews_; ++i)
    std::fprintf(stderr, "video: view %d screen %d at %d,%d %dx%d%s (%dx%d, integer %s%s)\n", i, views_[i].screen, views_[i].rect.x, views_[i].rect.y, views_[i].rect.w, views_[i].rect.h,
                 views_[i].shown ? "" : " hidden", w, h, int_scale_name(int_scale_), layout_.dominant_auto && (layout_.mode == Mode::DominantV || layout_.mode == Mode::DominantH) ? ", dominant auto" : "");
  if (disp_) for (int i = 0; i < nviews_; ++i) disp_->set_view(i, views_[i].rect.x, views_[i].rect.y, views_[i].rect.w, views_[i].rect.h, views_[i].shown);
}

bool Display::try_gpu_present() {
  if (!gpu_wanted_ || !out_) return false;
  std::string why;
  if (gpu_mode_ != GpuPresentMode::Vulkan) gpu_ = open_rga_presenter(*out_, &why);
  if (!gpu_ && gpu_mode_ != GpuPresentMode::Rga) {
    std::string why2;
    gpu_ = open_vk_import_presenter(*out_, &why2);
    if (!gpu_) why = gpu_mode_ == GpuPresentMode::Vulkan ? why2 : why + "; " + why2;
  }
  if (!gpu_) { std::fprintf(stderr, "video.gpu_present: %s; scanline scaling instead\n", why.c_str()); return false; }
  scaled_ = false;
  layout();
  std::fprintf(stderr, "video: %s, %s driver, rot %d\n", gpu_->name().c_str(), SDL_GetCurrentVideoDriver(), rot_);
  return true;
}

bool Display::settle_step(const u32* const blank[SCREENS]) {
  sync();
  if (!out_ || !fullscreen_) return true;
  // Target: the output's size. Unknown -> nothing to wait for.
  SDL_DisplayMode m{};
  const int di = only_screen_ >= 0 ? display_index_ : SDL_GetWindowDisplayIndex(win_);
  if (di < 0 || SDL_GetDesktopDisplayMode(di, &m) != 0 || m.w <= 0 || m.h <= 0) return true;
  SDL_PumpEvents();
  int w = 0, h = 0;
  SDL_GetWindowSize(win_, &w, &h);
  // Commit a frame: the surface maps on its first buffer, and the compositor
  // sends the fullscreen configure after that. Drawing at the new size is
  // what makes the buffers follow it.
  if (gpu_) draw_gpu(blank);
  else {
    Target t[SCREENS] = {};
    if (begin_frame(t)) present();
  }
  const bool done = (w == m.w && h == m.h) || (w == m.h && h == m.w);   // a rotated output reports either way
  if (done && verbose() && (w != open_w_ || h != open_h_)) std::fprintf(stderr, "video: settled at %dx%d (opened %dx%d) at %u ms\n", w, h, open_w_, open_h_, SDL_GetTicks());
  return done;
}

bool Display::drop_gpu_present(const char* why) {
  sync();
  if (!gpu_) return false;
  std::fprintf(stderr, "video: GPU present %s; scanline scaling from here\n", why);
  gpu_.reset();
  scaled_ = true;
  margins_dirty_ = true;
  layout();
  build_scale();
  return true;
}

void Display::sync() const {
  if (!job_) return;
  PresentThread::get().wait(job_);
  job_ = 0;
}

bool Display::prepare_gpu(GpuFrame& f) {
  int w = 0, h = 0;
  SDL_GetWindowSize(win_, &w, &h);
  switch (gpu_->fit(win_, w, h)) {
    case FramePresenter::Fit::Lost:
      // The sink may be closed now; begin_frame() reopens it or falls back
      // to the window surface.
      drop_gpu_present("lost on resize");
      return false;   // this frame is dropped; the next takes begin_frame()
    case FramePresenter::Fit::Changed: layout(); break;
    case FramePresenter::Fit::Same: break;
  }
  FramePresenter::Params& p = f.params;
  out_size(p.lw, p.lh);
  for (int i = 0; i < nviews_; ++i) f.v[i] = FramePresenter::View{views_[i].screen, views_[i].rect, views_[i].shown, !views_[i].direct, grid_on(views_[i].screen)};
  f.n = nviews_;
  p.rot = rot_; p.inset_alpha = inset_alpha_;
  p.drawn = frontend::Rect{canvas_drawn_.x, canvas_drawn_.y, canvas_drawn_.w, canvas_drawn_.h};
  p.grid = grid_strength_ > 0.0 ? static_cast<u32>(std::lround((1.0 - grid_strength_) * 256.0)) : 256u;
  canvas_drawn_ = SDL_Rect{0, 0, 0, 0};
  f.p = gpu_.get();
  return true;
}

void Display::draw_gpu(const u32* const fb[SCREENS]) {
  GpuFrame f;
  if (prepare_gpu(f)) f.p->present(fb, f.v, f.n, f.params);
}

void Display::draw_async(Display* const ds[], int n, const u32* const fb[SCREENS]) {
  // One copy of the frame for every window: the core writes the next frame
  // into fb while the present thread reads this one. Reused once the job
  // that read it is done.
  static std::vector<u32> stage[SCREENS];
  static u64 stage_job = 0;
  // A frame reaches the sink at the next present, when its fence has long
  // signalled: on the Mali-G52 a present's compute takes ~5 ms from submit
  // to fence, so retiring it straight away holds the job that long per
  // window. DS_PRESENT_RETIRE=1 does so anyway (a frame less latency).
  static const bool retire = std::getenv("DS_PRESENT_RETIRE") != nullptr;
  GpuFrame frames[2];
  Display* owners[2] = {};
  int nf = 0;
  for (int i = 0; i < n && i < 2; ++i) {
    Display& d = *ds[i];
    d.sync();
    if (!d.gpu_) { d.draw(fb); continue; }
    if (d.prepare_gpu(frames[nf])) owners[nf++] = &d;
  }
  if (!nf) return;
  PresentThread& pt = PresentThread::get();
  pt.wait(stage_job);
  for (int s = 0; s < SCREENS; ++s) {
    stage[s].resize(static_cast<size_t>(SCREEN_W) * SCREEN_H);
    std::memcpy(stage[s].data(), fb[s], stage[s].size() * sizeof(u32));
  }
  stage_job = pt.post([frames, nf] {
    const u32* const sfb[SCREENS] = {stage[0].data(), stage[1].data()};
    for (int k = 0; k < nf; ++k) {
      const GpuFrame& f = frames[k];
      f.p->present(sfb, f.v, f.n, f.params);
      if (retire) f.p->retire();
    }
  });
  for (int k = 0; k < nf; ++k) owners[k]->job_ = stage_job;
}

void Display::draw(const u32* const fb[SCREENS]) {
  sync();
  if (gpu_) { draw_gpu(fb); return; }
  if (disp_) {
    if (disp_->overlay_available()) { disp_->overlay_changed(canvas_taken_); canvas_taken_ = false; }
    const u32* slots[DispOut::VIEWS] = {nullptr, nullptr};
    for (int i = 0; i < nviews_ && i < DispOut::VIEWS; ++i) slots[i] = views_[i].shown ? fb[views_[i].screen] : nullptr;
    disp_->set_inset_alpha(inset_alpha_);
    disp_->present(slots);
    return;
  }
  if (!ren_) return;   // renderer tier only; scanline tiers use begin_frame/end_frame
  SDL_SetRenderDrawColor(ren_, 0, 0, 0, 255);
  SDL_RenderClear(ren_);
  for (int i = 0; i < nviews_; ++i) {
    const View& v = views_[i];
    if (!v.shown) continue;
    SDL_UpdateTexture(tex_[v.screen], nullptr, fb[v.screen], static_cast<int>(SCREEN_W) * 4);
    // Textures are per screen, not per view, so the alpha mod is scoped to
    // the inset's copy or a screen swap would carry it to the large view.
    const bool translucent = !v.direct && inset_alpha_ < 255;
    if (translucent) { SDL_SetTextureBlendMode(tex_[v.screen], SDL_BLENDMODE_BLEND); SDL_SetTextureAlphaMod(tex_[v.screen], inset_alpha_); }
    SDL_RenderCopy(ren_, tex_[v.screen], nullptr, reinterpret_cast<const SDL_Rect*>(&v.rect));
    if (translucent) { SDL_SetTextureAlphaMod(tex_[v.screen], 255); SDL_SetTextureBlendMode(tex_[v.screen], SDL_BLENDMODE_NONE); }
  }
  SDL_RenderPresent(ren_);
}

void Display::set_page(bool on) {
  sync();
  page_ = on;
  if (disp_) disp_->set_divisor(on ? 1 : disp_divisor_);
}

void Display::toggle_fullscreen() {
  sync();
  if (disp_) return;   // the panel is the window
  fullscreen_ = !fullscreen_;
  SDL_SetWindowFullscreen(win_, fullscreen_ ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
  layout();
  build_scale();
  margins_dirty_ = true;
}

void Display::set_layout(const Layout& l) {
  sync();
  if (only_screen_ >= 0) return;
  if (disp_) { layout_ = l; layout(); if (chunky_) build_source_scale(); return; }
  const Mode was = layout_.mode;
  layout_ = l;
  if (!fullscreen_ && was != l.mode) {
    int pw = 0;
    for (int i = 0; i < nviews_; ++i) if (views_[i].shown) pw = std::max(pw, views_[i].rect.w);
    int nw = 0, nh = 0;
    natural_size(l, std::max(1.0, static_cast<double>(pw) / SCREEN_W), nw, nh);
    if (rotated()) std::swap(nw, nh);   // the window is the presented side
    SDL_SetWindowSize(win_, nw, nh);
  }
  layout();
  build_scale();
  margins_dirty_ = true;
}

bool Display::map_point(int wx, int wy, int& screen, int& sx, int& sy) const {
  // Pointer events arrive in presented pixels; inverse of rotate_out()'s mapping.
  if (rot_ && scaled_) {
    const int lw = scaled_w_, lh = scaled_h_;
    int lx = wx, ly = wy;
    switch (rot_) {
      case 270: lx = lw - 1 - wy; ly = wx; break;
      case 90:  lx = wy; ly = lh - 1 - wx; break;
      case 180: lx = lw - 1 - wx; ly = lh - 1 - wy; break;
      default: break;
    }
    wx = lx; wy = ly;
  }
  for (int i = nviews_ - 1; i >= 0; --i) {
    const View& v = views_[i];
    if (!v.shown) continue;
    if (wx < v.rect.x || wx >= v.rect.x + v.rect.w || wy < v.rect.y || wy >= v.rect.y + v.rect.h) continue;
    screen = v.screen;
    sx = (wx - v.rect.x) * static_cast<int>(SCREEN_W) / v.rect.w;
    sy = (wy - v.rect.y) * static_cast<int>(SCREEN_H) / v.rect.h;
    return true;
  }
  return false;
}

// ---- per-scanline scaling ---------------------------------------------------

bool Display::out_size(int& w, int& h) const {
  if (disp_) { w = disp_->logical_w(); h = disp_->logical_h(); return true; }
  if (ren_) return SDL_GetRendererOutputSize(ren_, &w, &h) == 0;
  if (!win_) return false;
  // Window size is the truth on a scanout tier: the shm surface can lag a
  // configure by a frame, and KMSDRM has no window surface to ask at all.
  if (out_) { SDL_GetWindowSize(win_, &w, &h); if (rotated()) std::swap(w, h); return w > 0 && h > 0; }
  SDL_Surface* s = SDL_GetWindowSurface(win_);
  if (!s) return false;
  w = s->w; h = s->h;
  if (rotated()) std::swap(w, h);
  return true;
}

// Chunky on the display-engine tier. Panel cell P must be a whole number D
// of DS pixels under the DE's fit (P dividing the view's panel rect into a
// count that divides 256x192). Auto picks the smallest such P in 4..16.
// Two ways to draw it: in the scaler (DispOut::set_divisor) when every
// shown view and the canvas divide by D -- box-downscale each view by D
// (cells' mean), DE's nearest table enlarges them whole, no DS-resolution
// work. Otherwise at source: the scanline scaler flattens cells/pairs at
// 1:1 into src_side_, which end_frame hands to the layer instead of the
// core's framebuffer.
void Display::build_source_scale() {
  const double s = disp_ ? disp_->fit_scale() : 0.0;
  // First the cell each view would get, and whether the scaler can draw them.
  u32 D_hw = 0;
  bool hw = disp_ != nullptr && chunky_cell_ != 0 && s > 0.0;
  int cw = 0, chh = 0;
  natural_size(layout_, 1.0, cw, chh);
  for (int i = 0; hw && i < nviews_; ++i) {
    const View& v = views_[i];
    if (!v.shown) continue;
    const u32 pw = static_cast<u32>(std::lround(v.rect.w * s)), ph = static_cast<u32>(std::lround(v.rect.h * s));
    u32 D = 0;
    if (pw >= SCREEN_W && ph >= SCREEN_H) {
      auto fits = [&](u32 p) {
        if (p < 2 || pw % p || ph % p) return false;
        const u32 cx = pw / p, cy = ph / p;
        return SCREEN_W % cx == 0 && SCREEN_H % cy == 0 && SCREEN_W / cx == SCREEN_H / cy && SCREEN_W / cx <= ds::gpu::Gpu::CELL_TAPS;
      };
      u32 P = 0;
      if (chunky_cell_ > 0) { for (u32 p = static_cast<u32>(chunky_cell_); p >= 2 && !P; --p) if (fits(p)) P = p; }
      else { for (u32 p = 4; p <= 16 && !P; ++p) if (fits(p)) P = p; }
      D = P ? SCREEN_W / (pw / P) : 0;
      if (D < 2) hw = false;                       // no whole cell: pairs at source
      else if (D_hw && D != D_hw) hw = false;      // views disagree
      else D_hw = D;
    }
    if (D_hw && (v.rect.w % D_hw || v.rect.h % D_hw || v.rect.x % D_hw || v.rect.y % D_hw)) hw = false;
  }
  if (hw && (!D_hw || cw % D_hw || chh % D_hw)) hw = false;
  disp_divisor_ = hw ? static_cast<int>(D_hw) : 1;
  if (disp_) disp_->set_divisor(page_ ? 1 : disp_divisor_);
  if (hw) {
    scaled_ = false;   // the core's framebuffers go to the layer as they are
    for (int i = 0; i < nviews_; ++i) { disp_->set_view_cell(i, 1); src_chunky_[views_[i].screen] = false; }
    if (verbose()) std::fprintf(stderr, "video: chunky cells of %u DS pixels drawn by the scaler\n", D_hw);
    return;
  }
  scaled_ = true;
  for (int i = 0; i < nviews_; ++i) {
    const View& v = views_[i];
    std::vector<u16>& xr = xrun_[v.screen];
    xr.resize(static_cast<size_t>(SCREEN_W) + 1);
    for (u32 x = 0; x <= SCREEN_W; ++x) xr[x] = static_cast<u16>(x);
    xrun_plain_[v.screen] = xr;
    seam_w_[v.screen].assign(SCREEN_W, 0);
    cells_[v.screen] = {};
    src_side_[v.screen].assign(static_cast<size_t>(SCREEN_W) * SCREEN_H, 0xFF000000u);
    int cell = 2;
    const u32 pw = static_cast<u32>(std::lround(v.rect.w * s)), ph = static_cast<u32>(std::lround(v.rect.h * s));
    // Shown smaller than the screen: flattening before a downscale is only blur.
    src_chunky_[v.screen] = s > 0.0 && pw >= SCREEN_W && ph >= SCREEN_H;
    if (!src_chunky_[v.screen]) { if (disp_) disp_->set_view_cell(i, 1); continue; }
    if (chunky_cell_ != 0) {
      u32 P = 0;
      auto fits = [&](u32 p) {
        if (p < 2 || pw % p || ph % p) return false;
        const u32 cx = pw / p, cy = ph / p;
        return SCREEN_W % cx == 0 && SCREEN_H % cy == 0 && SCREEN_W / cx == SCREEN_H / cy && SCREEN_W / cx <= ds::gpu::Gpu::CELL_TAPS;
      };
      if (chunky_cell_ > 0) { for (u32 p = static_cast<u32>(chunky_cell_); p >= 2 && !P; --p) if (fits(p)) P = p; }
      else { for (u32 p = 4; p <= 16 && !P; ++p) if (fits(p)) P = p; }
      ds::gpu::Gpu::CellMap m;
      const u32 D = P ? SCREEN_W / (pw / P) : 0;
      if (P && D > 1 && ds::gpu::Gpu::build_cell_axis(SCREEN_W, SCREEN_W / D, D, m.x) && ds::gpu::Gpu::build_cell_axis(SCREEN_H, SCREEN_H / D, D, m.y)) {
        cells_[v.screen] = std::move(m);
        cell = static_cast<int>(D);
        for (u32 x = 0; x <= SCREEN_W; ++x) xr[x] = static_cast<u16>(std::min(x, SCREEN_W / D) * D);
        if (verbose() || (chunky_cell_ > 0 && P != static_cast<u32>(chunky_cell_)))
          std::fprintf(stderr, "video: chunky cells of %u px on %ux%u = %u DS pixels at source\n", P, pw, ph, D);
      } else {
        std::fprintf(stderr, "video: no %s cell is whole DS pixels on %ux%u; 2x2 pairs at source\n", chunky_cell_ > 0 ? "such" : "auto", pw, ph);
      }
    }
    if (cell == 2) for (u32 x = 1; x < SCREEN_W; x += 2) xr[x] = xr[x + 1];
    if (disp_) disp_->set_view_cell(i, cell);
  }
}

void Display::build_scale() {
  if (!scaled_ || !win_ || disp_) return;   // the display-engine tier's tables are fixed at open()
  if (out_) {
    // No window surface to fetch on KMSDRM, and asking would build a renderer.
    surf_ = nullptr;
    phys_w_ = out_->width(); phys_h_ = out_->height();
  } else {
    surf_ = SDL_GetWindowSurface(win_);      // recreated by SDL on resize
    if (!surf_) { scaled_ = false; return; }
    if (surf_->format->BytesPerPixel != 4) {
      std::fprintf(stderr, "window surface is %d bpp, not 32; using the framebuffer path\n",
                   surf_->format->BytesPerPixel);
      scaled_ = false;
      return;
    }
    phys_w_ = surf_->w; phys_h_ = surf_->h;
  }
  // Views/tables are in the logical frame; under rotation that's the staging
  // buffer, the presented buffer's size turned back.
  scaled_w_ = phys_w_; scaled_h_ = phys_h_;
  if (rotated()) std::swap(scaled_w_, scaled_h_);
  if (rot_) stage_.assign(static_cast<size_t>(scaled_w_) * scaled_h_, 0);
  else { stage_.clear(); stage_.shrink_to_fit(); }

  // Inverse of the dst_x -> src_x = dst_x * SCREEN_W / rect.w map used by
  // draw()/map_point(): source pixel s covers [xrun[s], xrun[s+1]). One
  // table per view. The largest shown view sets the chunky cell the others follow.
  int ref_view = 0;
  for (int i = 1; i < nviews_; ++i) if (views_[i].shown && (!views_[ref_view].shown || views_[i].rect.w > views_[ref_view].rect.w)) ref_view = i;
  const int order[SCREENS] = {ref_view, 1 - ref_view};   // the reference first, so ref_D is set
  double ref_D = 2.0;
  for (int oi = 0; oi < nviews_; ++oi) {
    const int i = order[oi];
    const View& v = views_[i];
    std::vector<u16>& xr = xrun_[v.screen];
    xr.resize(static_cast<size_t>(SCREEN_W) + 1);
    // A direct view scaled past the buffer (integer overscale) is cropped:
    // runs outside [x0, x1) collapse to empty (scale_row skips them), rest
    // relative to x0. Rows are cropped the same way by targets(). An inset
    // is never cropped here; blit_insets clips it at the edge.
    const u32 x0 = v.direct ? static_cast<u32>(std::max(0, -v.rect.x)) : 0u;
    const u32 x1 = v.direct ? static_cast<u32>(std::clamp(scaled_w_ - v.rect.x, 0, v.rect.w)) : static_cast<u32>(v.rect.w);
    const bool cropped = v.direct && (x0 > 0 || x1 < static_cast<u32>(v.rect.w) || v.rect.y < 0 || v.rect.y + v.rect.h > scaled_h_);
    for (u32 x = 0; x <= SCREEN_W; ++x) {
      const u32 raw = (x * static_cast<u32>(v.rect.w) + SCREEN_W - 1) / SCREEN_W;
      xr[x] = static_cast<u16>(std::clamp(raw, x0, x1) - x0);
    }
    xrun_plain_[v.screen] = xr;
    // Chunky: the even pixel's run widens over the odd one's (left as a
    // zero-length run scale_row skips); destination coverage is unchanged.
    cells_[v.screen] = {};
    src_chunky_[v.screen] = chunky_;
    bool pair = chunky_;
    if (chunky_) {
      // Cell of P panel pixels needs P to divide both dimensions, <=256 cells across.
      const u32 w = static_cast<u32>(v.rect.w), h = static_cast<u32>(v.rect.h);
      u32 P = 0;
      // A cropped view takes the pairs instead (exact cells at a whole scale anyway).
      auto fits = [&](u32 p) { return !cropped && p >= 2 && w % p == 0 && h % p == 0 && w / p <= SCREEN_W; };
      // Largest view chooses the cell (steps down to the nearest divisor,
      // never below auto's floor of 4). Other views match its DS-pixel cell
      // size rather than its panel-pixel one, so the smaller screen isn't
      // the coarsest; a view shown smaller than the screen goes plain.
      if (i == ref_view) {
        if (chunky_cell_ > 0) { for (u32 p = static_cast<u32>(chunky_cell_); p >= 4 && !P; --p) if (fits(p)) P = p; }
        else if (chunky_cell_ < 0) { for (u32 p = 4; p <= 16 && !P; ++p) if (fits(p)) P = p; }
        ref_D = P ? static_cast<double>(SCREEN_W) * P / w : 2.0;
      } else if (w < SCREEN_W || h < SCREEN_H) {
        src_chunky_[v.screen] = false; pair = false;
        if (verbose()) std::fprintf(stderr, "video: view %ux%u is below the screen; no chunky\n", w, h);
      } else if (chunky_cell_ != 0) {
        double best = 1e9;
        for (u32 p = 2; p <= 64; ++p) if (fits(p)) {
          const double d = std::fabs(static_cast<double>(SCREEN_W) * p / w - ref_D);
          if (d < best - 1e-9) { best = d; P = p; }
        }
      }
      ds::gpu::Gpu::CellMap m;
      if (P && ds::gpu::Gpu::build_cell_axis(SCREEN_W, w / P, P, m.x) && ds::gpu::Gpu::build_cell_axis(SCREEN_H, h / P, P, m.y)) {
        cells_[v.screen] = std::move(m);
        pair = false;
        // The cells' xrun: cell i covers [i*P, (i+1)*P); entries past the
        // last cell are empty runs.
        for (u32 x = 0; x <= SCREEN_W; ++x) xr[x] = static_cast<u16>(std::min(x, w / P) * P);
        // The chosen cell is chatter unless it is not the one asked for;
        // that is said once, with what was used instead.
        static bool told = false;
        if (i == ref_view && chunky_cell_ > 0 && P != static_cast<u32>(chunky_cell_) && !told) {
          told = true;
          std::fprintf(stderr, "video: no %d px cell divides %ux%u; using %u\n", chunky_cell_, w, h, P);
        } else if (verbose()) std::fprintf(stderr, "video: chunky cells %ux%u of %u px (%.2f DS pixels)\n", w / P, h / P, P, static_cast<double>(SCREEN_W) * P / w);
      } else if (!src_chunky_[v.screen]) {}
      else if (chunky_cell_ && cropped) { if (verbose()) std::fprintf(stderr, "video: view cropped by the overscale; 2x2 pairs\n"); }
      else if (chunky_cell_) std::fprintf(stderr, "video: no %s cell divides %ux%u; 2x2 pairs\n", chunky_cell_ > 0 ? "such" : "auto", w, h);
    }
    if (pair)
      for (u32 x = 1; x < SCREEN_W; x += 2) xr[x] = xr[x + 1];
    // Box-filter weights: the boundary between source pixels s and s+1 lies
    // at b = (s+1) * w / 256; when that is fractional the panel pixel it
    // falls in (the last of run s, index floor(b)) covers pixel s from its
    // left edge up to the boundary -- frac(b) of it -- and pixel s+1 for the
    // rest, 1 - frac(b). The weight stored is s+1's. Chunky pairs share a
    // boundary at s+2's, so the odd boundaries are not seams.
    std::vector<u8>& sw = seam_w_[v.screen];
    sw.assign(SCREEN_W, 0);
    for (u32 s = 0; s + 1 < SCREEN_W; ++s) {
      if (pair && !(s & 1)) continue;
      const u32 b = (s + 1) * static_cast<u32>(v.rect.w);
      const u32 frac = b % SCREEN_W;
      if (frac && xr[s + 1] > xr[s]) sw[s] = static_cast<u8>(256 - (frac * 256) / SCREEN_W);
    }
    // Bilinear: dst column x samples source u = (x+0.5)*256/w - 0.5, between
    // floor(u) and floor(u)+1. Right edge leans on pixel 254 at weight 255
    // so the kernel's pair load never reads past the row.
    std::vector<u16>& lsx = lin_sx_[v.screen];
    std::vector<u8>& lwx = lin_wx_[v.screen];
    lsx.assign(static_cast<size_t>(x1 - x0), 0); lwx.assign(static_cast<size_t>(x1 - x0), 0);
    for (u32 x = x0; x < x1; ++x) {
      const s32 u = static_cast<s32>(((2 * x + 1) * SCREEN_W * 128) / static_cast<u32>(v.rect.w)) - 128;  // u * 256
      if (u <= 0) continue;
      u32 s = static_cast<u32>(u) >> 8, f = static_cast<u32>(u) & 255;
      if (s >= SCREEN_W - 1) { s = SCREEN_W - 2; f = 255; }
      lsx[x - x0] = static_cast<u16>(s); lwx[x - x0] = static_cast<u8>(f);
    }
    if (!v.direct) side_[v.screen].assign(static_cast<size_t>(v.rect.w) * v.rect.h, 0);
  }
}

// Screen rects are overwritten in full every frame; the rest of the surface
// (dominant modes leave gaps) is cleared row by row outside the rects only
// when the layout changed under it. `w`/`h` are the buffer's own size: on
// the dmabuf tier the shm surface can lag a configure behind it.
void Display::clear_margins(u32* px, u32 pitch, int w, int h) const {
  for (int y = 0; y < h; ++y) {
    u32* row = px + static_cast<size_t>(y) * pitch;
    int x = 0;
    int xs[SCREENS], xe[SCREENS], n = 0;   // direct views in x order on this row (at most two)
    for (int i = 0; i < nviews_; ++i) {
      const View& v = views_[i];
      if (!v.direct || y < v.rect.y || y >= v.rect.y + v.rect.h) continue;
      xs[n] = std::min(v.rect.x, w); xe[n] = std::min(v.rect.x + v.rect.w, w); ++n;
    }
    if (n == 2 && xs[1] < xs[0]) { std::swap(xs[0], xs[1]); std::swap(xe[0], xe[1]); }
    for (int i = 0; i < n; ++i) {
      if (xs[i] > x) std::memset(row + x, 0, static_cast<size_t>(xs[i] - x) * sizeof(u32));
      x = std::max(x, xe[i]);
    }
    if (x < w) std::memset(row + x, 0, static_cast<size_t>(w - x) * sizeof(u32));
  }
}

// Accumulated into one rect for the frame (a modal page and a corner label
// are the two cases; a union costs a few cleared pixels, saves a list).
void Display::note_canvas_draw(int x, int y, int w, int h) {
  if (w <= 0 || h <= 0) return;
  if (canvas_drawn_.w == 0 || canvas_drawn_.h == 0) { canvas_drawn_ = SDL_Rect{x, y, w, h}; return; }
  const int x0 = std::min(canvas_drawn_.x, x), y0 = std::min(canvas_drawn_.y, y);
  const int x1 = std::max(canvas_drawn_.x + canvas_drawn_.w, x + w);
  const int y1 = std::max(canvas_drawn_.y + canvas_drawn_.h, y + h);
  canvas_drawn_ = SDL_Rect{x0, y0, x1 - x0, y1 - y0};
}

void Display::clear_rect(u32* px, u32 pitch, int w, int h, const SDL_Rect& r) const {
  const int x0 = std::max(0, r.x), y0 = std::max(0, r.y);
  const int x1 = std::min(w, r.x + r.w), y1 = std::min(h, r.y + r.h);
  if (x1 <= x0 || y1 <= y0) return;
  for (int y = y0; y < y1; ++y)
    std::memset(px + static_cast<size_t>(y) * pitch + x0, 0, static_cast<size_t>(x1 - x0) * sizeof(u32));
}

void Display::targets(u32* px, u32 stride, int w, int h, Target out[SCREENS]) {
  frame_px_ = px; frame_pitch_ = stride; frame_w_ = w; frame_h_ = h;
  insets_done_ = false;
  for (int i = 0; i < nviews_; ++i) {
    const View& v = views_[i];
    if (v.direct) {
      // px at the rect's first kept row/column; runs are relative to it (see build_scale).
      const int x0 = std::max(0, -v.rect.x), y0 = std::max(0, -v.rect.y);
      const int y1 = std::clamp(h - v.rect.y, 0, v.rect.h);
      out[v.screen] = Target{px + static_cast<size_t>(v.rect.y + y0) * stride + v.rect.x + x0, stride, static_cast<u32>(v.rect.h), xrun_[v.screen].data(), seam_w_[v.screen].data(), lin_sx_[v.screen].data(), lin_wx_[v.screen].data(), grid_on(v.screen), xrun_plain_[v.screen].data(),
                             static_cast<u32>(y0), static_cast<u32>(std::max(y0, y1))};
    } else
      out[v.screen] = Target{side_[v.screen].data(), static_cast<u32>(v.rect.w), static_cast<u32>(v.rect.h), xrun_[v.screen].data(), seam_w_[v.screen].data(), lin_sx_[v.screen].data(), lin_wx_[v.screen].data(), grid_on(v.screen), xrun_plain_[v.screen].data()};
  }
}

void Display::blend_row(u32* dst, const u32* src, size_t n, u32 alpha) {
  // Per-channel lerp, channel pairs masked apart. 0..255 alpha scaled to 0..256 so 255 is exact.
  const u32 a = alpha + (alpha >> 7), b = 256 - a;
  for (size_t i = 0; i < n; ++i) {
    const u32 d = dst[i], s = src[i];
    // Each channel product is under 2^16, so a packed pair never carries into the other.
    const u32 rb = (((s & 0x00FF00FFu) * a + (d & 0x00FF00FFu) * b) >> 8) & 0x00FF00FFu;
    const u32 g = (((s & 0x0000FF00u) * a + (d & 0x0000FF00u) * b) >> 8) & 0x0000FF00u;
    dst[i] = 0xFF000000u | rb | g;
  }
}

// Copies (or blends, if translucent) the shown side-buffer views into the
// frame. The blend reads the frame back, uncached on the scanout tiers, but
// the inset is small and sequential so it stays cheap.
void Display::blit_insets() {
  if (!frame_px_ || insets_done_) return;
  insets_done_ = true;
  for (int i = 0; i < nviews_; ++i) {
    const View& v = views_[i];
    if (v.direct || !v.shown) continue;
    // Clipped to the buffer: the large screen overscaled can push the inset's corner past the panel.
    const int x0 = std::max(0, -v.rect.x), x1 = std::min(v.rect.w, frame_w_ - v.rect.x);
    if (x1 <= x0) continue;
    for (int y = std::max(0, -v.rect.y); y < std::min(v.rect.h, frame_h_ - v.rect.y); ++y) {
      u32* dst = frame_px_ + static_cast<size_t>(v.rect.y + y) * frame_pitch_ + v.rect.x + x0;
      const u32* src = side_[v.screen].data() + static_cast<size_t>(y) * v.rect.w + x0;
      if (inset_alpha_ == 255) std::memcpy(dst, src, static_cast<size_t>(x1 - x0) * sizeof(u32));
      else blend_row(dst, src, static_cast<size_t>(x1 - x0), inset_alpha_);
    }
  }
}

bool Display::begin_frame(Target out[SCREENS]) {
  if (!scaled_) return false;   // GPU present or the renderer: nothing here touches the present thread's side
  sync();
  if (disp_) {
    for (int i = 0; i < SCREENS; ++i)
      out[i] = Target{src_side_[i].data(), SCREEN_W, SCREEN_H, xrun_[i].data(), seam_w_[i].data(), nullptr, nullptr, false, xrun_plain_[i].data()};
    return true;
  }
  if (out_) {
    // A configure resizes the window under us; buffers must follow before
    // anything writes at the new geometry.
    int w = 0, h = 0;
    SDL_GetWindowSize(win_, &w, &h);
    if (w != out_->width() || h != out_->height()) {
      if (!out_->reopen(win_, w, h)) {
        std::fprintf(stderr, "video: scanout resize failed; window surface from here\n");
        out_.reset();
        margins_dirty_ = true;
        layout();
        build_scale();
        // fall through to the shm path below
      } else {
        layout();
        build_scale();
        out_clean_ = 0;
      }
    }
  }
  if (out_) {
    if (u32* px = out_->begin_frame()) {
      take_frame(px, static_cast<u32>(out_->stride()), out_->width(), out_->height(), out_->current(), out);
      out_frame_ = true;
      return true;
    }
    std::fprintf(stderr, "video: scanout path lost; window surface from here\n");
    out_->close();
    out_.reset();
    margins_dirty_ = true;
    build_scale();
    if (!scaled_) return false;
  }
  // Fetched every frame, not cached: SDL hands back a new surface after a resize.
  SDL_Surface* s = SDL_GetWindowSurface(win_);
  if (!s) return false;
  if (s != surf_ || s->w != phys_w_ || s->h != phys_h_) {
    layout();
    build_scale();
    if (!scaled_) return false;
    margins_dirty_ = true;
  }
  if (SDL_MUSTLOCK(s) && SDL_LockSurface(s) != 0) {
    std::fprintf(stderr, "SDL_LockSurface: %s\n", SDL_GetError());
    return false;
  }
  take_frame(static_cast<u32*>(s->pixels), static_cast<u32>(s->pitch) / sizeof(u32), s->w, s->h, 0, out);
  return true;
}

void Display::take_frame(u32* px, u32 stride, int w, int h, int idx, Target out[SCREENS]) {
  if (rot_) {
    // Rotation rewrites the presented buffer in full every frame, so
    // per-buffer bookkeeping collapses to buffer 0 of the staging frame.
    phys_px_ = px; phys_pitch_ = stride;
    px = stage_.data(); stride = static_cast<u32>(scaled_w_); w = scaled_w_; h = scaled_h_; idx = 0;
  }
  // Every buffer's margins are cleared once (tracked per index, not a
  // frame count): a buffer can come back several frames running while
  // another still holds the old layout until it's cycled back to.
  if (margins_dirty_) { out_clean_ = 0; margins_dirty_ = false; }
  const u32 bit = idx >= 0 && idx < 32 ? 1u << idx : 0u;
  if (!(out_clean_ & bit)) { clear_margins(px, stride, w, h); out_clean_ |= bit; }
  if (idx >= 0 && idx < kMaxBufs) {
    clear_rect(px, stride, w, h, canvas_prev_[idx]);
    canvas_prev_[idx] = SDL_Rect{0, 0, 0, 0};
  }
  canvas_drawn_ = SDL_Rect{0, 0, 0, 0};
  targets(px, stride, w, h, out);
}

// Insets go down before anything the frontend draws, so an overlay can't be
// buried by the PiP inset.
void Display::finish_views() {
  sync();
  if (disp_) return;
  blit_insets();
}

bool Display::canvas_capable() const {
  // Display-engine tier has no panel-resolution frame buffer, but its
  // overlay layer serves the same purpose regardless of the scaling path.
  if (disp_) return disp_->overlay_available();
  if (gpu_) return true;   // GPU present stage blends its own overlay plane
  return scaled_;   // else the canvas is the frame itself; SDL_Renderer has none
}

bool Display::canvas(CanvasView& out) const {
  sync();
  if (gpu_) {
    int lw = 0, lh = 0;
    if (!out_size(lw, lh)) return false;
    u32* px = gpu_->overlay(lw, lh);
    if (!px) return false;
    const_cast<Display*>(this)->frame_w_ = lw;
    const_cast<Display*>(this)->frame_h_ = lh;
    out = CanvasView{px, static_cast<u32>(lw), lw, lh};
    return true;
  }
  if (disp_) {
    u32* px = nullptr; int pitch = 0, w = 0, h = 0;
    if (!disp_->overlay(px, pitch, w, h)) return false;
    canvas_taken_ = true;
    out = CanvasView{px, static_cast<u32>(pitch), w, h};
    return true;
  }
  if (!frame_px_) return false;
  out = CanvasView{frame_px_, frame_pitch_, frame_w_, frame_h_};
  return true;
}

void Display::present() {
  sync();
  if (disp_) {
    const u32* fb[SCREENS];
    for (int i = 0; i < SCREENS; ++i) fb[i] = src_side_[i].data();
    draw(fb);
    return;
  }
  // Remember what was drawn to this buffer, so its next use clears it back out.
  const int idx = rot_ ? 0 : out_frame_ && out_ ? out_->current() : 0;
  if (idx >= 0 && idx < kMaxBufs) canvas_prev_[idx] = canvas_drawn_;
  canvas_drawn_ = SDL_Rect{0, 0, 0, 0};
  frame_px_ = nullptr;
  if (rot_ && phys_px_) { rotate_out(phys_px_, phys_pitch_); phys_px_ = nullptr; }
  if (out_frame_) { out_frame_ = false; out_->end_frame(); return; }
  if (SDL_MUSTLOCK(surf_)) SDL_UnlockSurface(surf_);
  SDL_UpdateWindowSurface(win_);
}

void Display::end_frame() {
  sync();
  finish_views();
  present();
}

} // namespace ds::sdl
