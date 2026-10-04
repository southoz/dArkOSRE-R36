// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "display_fbdev.h"
#include "rt_thread.h"

#include <SDL2/SDL.h>

#include <cstdio>
#include <cstring>
#include <ctime>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace ds::sdl {

namespace {

// <linux/fb.h> in some sysroots lacks it; the value is stable across kernels.
#ifndef FBIO_WAITFORVSYNC
#define FBIO_WAITFORVSYNC _IOW('F', 0x20, u32)
#endif

constexpr const char* FB_PATH = "/dev/fb0";

u64 now_ns() { timespec t{}; clock_gettime(CLOCK_MONOTONIC, &t); return static_cast<u64>(t.tv_sec) * 1000000000ull + static_cast<u64>(t.tv_nsec); }

// 0xAARRGGBB in memory as the core writes it: blue at bit 0, red at bit 16.
bool is_argb8888(const fb_var_screeninfo& v) {
  return v.bits_per_pixel == 32 && v.red.offset == 16 && v.green.offset == 8 && v.blue.offset == 0
      && v.red.length == 8 && v.green.length == 8 && v.blue.length == 8;
}

} // namespace

bool FbdevOut::available() {
  static int cached = -1;
  if (cached >= 0) return cached != 0;
  cached = 0;
  const int fd = ::open(FB_PATH, O_RDWR | O_CLOEXEC);
  if (fd < 0) return false;
  fb_var_screeninfo var{};
  const bool ok = ioctl(fd, FBIOGET_VSCREENINFO, &var) == 0 && var.xres > 0 && var.yres > 0 && is_argb8888(var);
  ::close(fd);
  cached = ok ? 1 : 0;
  return ok;
}

void FbdevOut::fit_window(SDL_Window* win) const {
  if (!win) return;
  // A fullscreen window ignores SDL_SetWindowSize; the headless driver's
  // fullscreen is a made-up desktop mode, not the panel.
  if (SDL_GetWindowFlags(win) & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP)) SDL_SetWindowFullscreen(win, 0);
  int w = 0, h = 0;
  SDL_GetWindowSize(win, &w, &h);
  if (w != w_ || h != h_) SDL_SetWindowSize(win, w_, h_);
}

bool FbdevOut::open(SDL_Window* win, bool vsync) {
  if (!available()) return false;
  fd_ = ::open(FB_PATH, O_RDWR | O_CLOEXEC);
  if (fd_ < 0) { std::perror("fbdev: open"); return false; }
  if (ioctl(fd_, FBIOGET_VSCREENINFO, &var_) != 0) { std::perror("fbdev: FBIOGET_VSCREENINFO"); close(); return false; }
  if (!is_argb8888(var_)) { std::fprintf(stderr, "fbdev: fb0 is not ARGB8888\n"); close(); return false; }

  // Ask for three panels of virtual height; a driver that refuses keeps
  // whatever it had, and two is still a usable tier.
  if (var_.yres_virtual < var_.yres * MAX_BUFS) {
    fb_var_screeninfo want = var_;
    want.yres_virtual = var_.yres * MAX_BUFS;
    want.xres_virtual = var_.xres;
    want.yoffset = 0;
    want.activate = FB_ACTIVATE_NOW;
    if (ioctl(fd_, FBIOPUT_VSCREENINFO, &want) != 0) std::fprintf(stderr, "fbdev: cannot grow the virtual height to %u lines (%s)\n", want.yres_virtual, std::strerror(errno));
    if (ioctl(fd_, FBIOGET_VSCREENINFO, &var_) != 0) { std::perror("fbdev: FBIOGET_VSCREENINFO"); close(); return false; }
  }
  fb_fix_screeninfo fix{};
  if (ioctl(fd_, FBIOGET_FSCREENINFO, &fix) != 0) { std::perror("fbdev: FBIOGET_FSCREENINFO"); close(); return false; }
  if (fix.line_length % 4 != 0 || fix.line_length < var_.xres * 4) { std::fprintf(stderr, "fbdev: odd line length %u\n", fix.line_length); close(); return false; }

  w_ = static_cast<int>(var_.xres); h_ = static_cast<int>(var_.yres);
  stride_ = static_cast<int>(fix.line_length / 4);
  buf_bytes_ = static_cast<size_t>(fix.line_length) * var_.yres;
  bufs_ = static_cast<int>(var_.yres_virtual / var_.yres);
  if (bufs_ > MAX_BUFS) bufs_ = MAX_BUFS;
  while (bufs_ > 0 && buf_bytes_ * static_cast<size_t>(bufs_) > fix.smem_len) --bufs_;
  if (bufs_ < 2) { std::fprintf(stderr, "fbdev: fb0 holds %d panel buffer(s); two are needed\n", bufs_); close(); return false; }

  map_len_ = fix.smem_len;
  void* m = mmap(nullptr, map_len_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
  if (m == MAP_FAILED) { std::perror("fbdev: mmap"); map_ = nullptr; close(); return false; }
  map_ = static_cast<u8*>(m);
  std::memset(map_, 0, buf_bytes_ * static_cast<size_t>(bufs_));

  // Presenter thread always runs: FBIOPAN_DISPLAY blocks for the refresh, so
  // panning from end_frame() directly would stall emulation regardless.
  if (!vsync) std::fprintf(stderr, "fbdev: --no-vsync has no effect on this tier (the pan waits for the refresh; presenting from a thread)\n");
  vsync_ = true;
  cur_ = -1; dead_ = false;
  displayed_ = 0; latched_ = pending_ = -1; stop_ = false;
  next_ns_ = now_ns();
  var_.yoffset = 0; var_.xoffset = 0;
  fit_window(win);
  thread_ = std::thread([this] { presenter(); });
  std::fprintf(stderr, "fbdev: %dx%d, stride %d px, %d buffers, %u lines virtual\n", w_, h_, stride_, bufs_, var_.yres_virtual);
  return true;
}

bool FbdevOut::reopen(SDL_Window* win, int, int) {
  if (fd_ < 0 || dead_) return false;
  fit_window(win);
  return true;
}

void FbdevOut::close() {
  if (thread_.joinable()) {
    { std::lock_guard<std::mutex> g(mu_); stop_ = true; }
    cv_.notify_all();
    thread_.join();
  }
  if (map_) {
    // Leave the panel on buffer 0 so whatever draws fb0 next isn't stuck on
    // an unexpected pan offset.
    if (fd_ >= 0) { var_.yoffset = 0; ioctl(fd_, FBIOPAN_DISPLAY, &var_); }
    munmap(map_, map_len_);
    map_ = nullptr;
  }
  if (fd_ >= 0) ::close(fd_);
  fd_ = -1;
  cur_ = -1;
}

u32* FbdevOut::begin_frame() {
  if (fd_ < 0 || dead_ || cur_ >= 0) return nullptr;
  int buf = 0;
  if (thread_.joinable()) {
    // Any buffer that's neither shown, latched, nor drawn-and-waiting.
    std::unique_lock<std::mutex> lk(mu_);
    cv_.wait(lk, [this] { if (dead_) return true; for (int b = 0; b < bufs_; ++b) if (b != displayed_ && b != latched_ && b != pending_) return true; return false; });
    if (dead_) return nullptr;
    while (buf == displayed_ || buf == latched_ || buf == pending_) ++buf;
  } else buf = (displayed_ + 1) % bufs_;
  cur_ = buf;
  return buf_ptr(buf);
}

void FbdevOut::end_frame() {
  if (cur_ < 0) return;
  const int buf = cur_;
  cur_ = -1;
  if (thread_.joinable()) {
    { std::lock_guard<std::mutex> g(mu_); pending_ = buf; }
    cv_.notify_all();
  } else {
    pan(buf);
    displayed_ = buf;
  }
}

void FbdevOut::pan(int buf) {
  fb_var_screeninfo var = var_;
  var.yoffset = static_cast<u32>(buf) * var_.yres;
  var.activate = FB_ACTIVATE_VBL;
  const u64 t0 = now_ns();
  if (ioctl(fd_, FBIOPAN_DISPLAY, &var) != 0) {
    std::perror("fbdev: FBIOPAN_DISPLAY");
    dead_ = true;
    return;
  }
  if (!pan_measured_) {
    pan_measured_ = true;
    if (now_ns() - t0 < 1000000ull) {
      pan_blocks_ = false;
      std::fprintf(stderr, "fbdev: FBIOPAN_DISPLAY does not wait for vsync; %s\n", waitforvsync_ ? "trying FBIO_WAITFORVSYNC" : "pacing by the clock");
    }
  }
}

void FbdevOut::wait_vsync() {
  if (pan_blocks_) return;      // the pan itself was the wait
  if (waitforvsync_) {
    u32 zero = 0;
    if (ioctl(fd_, FBIO_WAITFORVSYNC, &zero) == 0) return;
    waitforvsync_ = false;
    std::fprintf(stderr, "fbdev: FBIO_WAITFORVSYNC unsupported; pacing by the clock\n");
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
}

void FbdevOut::presenter() {
  // One pan per refresh; ioctls run outside the lock so a post never waits.
  raise_presenter_priority("fbdev: presenter priority");
  std::unique_lock<std::mutex> lk(mu_);
  for (;;) {
    cv_.wait(lk, [this] { return stop_ || pending_ >= 0; });
    if (stop_) return;
    latched_ = pending_; pending_ = -1;
    lk.unlock();
    pan(latched_);
    if (!dead_) wait_vsync();
    lk.lock();
    displayed_ = latched_; latched_ = -1;
    cv_.notify_all();
    if (dead_) return;
  }
}

} // namespace ds::sdl
