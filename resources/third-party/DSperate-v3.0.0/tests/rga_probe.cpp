// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// RGA probe (Rockchip 2D accelerator as a V4L2 mem2mem device): can it scale
// a 1x DS frame in a CMA dma-buf into a panel-sized scanout dma-buf, with
// crop/compose rectangles for the two-screen layout, and how long does a
// frame take? Device-only; not a test.
//
//   rga_probe [/dev/videoN] [iters]      (DS_RGA_SW=256x192 DS_RGA_DW=1024x768)
//
// It fills the source with a pattern, runs the scale `iters` times and
// checks a few destination pixels, then times a two-screen frame: screen 0
// into the top half, screen 1 into the bottom half (compose rectangles on
// the capture queue, crop on the output queue).
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>
#include <algorithm>

// Needs a V4L2 with multi-planar mem2mem and dma-buf buffers (older kernel headers lack them).
#if defined(V4L2_PIX_FMT_XBGR32) && defined(V4L2_CAP_VIDEO_M2M_MPLANE)   // (V4L2_MEMORY_DMABUF is an enumerator, of the same vintage)

namespace {
struct dma_heap_allocation_data { uint64_t len; uint32_t fd; uint32_t fd_flags; uint64_t heap_flags; };
#define DMA_HEAP_IOCTL_ALLOC _IOWR('H', 0x0, struct dma_heap_allocation_data)

int heap_alloc(size_t len) {
  const char* heaps[] = {"/dev/dma_heap/linux,cma", "/dev/dma_heap/system", nullptr};
  for (int i = 0; heaps[i]; ++i) {
    int h = open(heaps[i], O_RDWR | O_CLOEXEC);
    if (h < 0) continue;
    dma_heap_allocation_data a{}; a.len = len; a.fd_flags = O_RDWR | O_CLOEXEC;
    int r = ioctl(h, DMA_HEAP_IOCTL_ALLOC, &a);
    close(h);
    if (r == 0) { std::printf("dma-buf %zu bytes from %s\n", len, heaps[i]); return static_cast<int>(a.fd); }
  }
  return -1;
}

int xioctl(int fd, unsigned long req, void* arg) { int r; do r = ioctl(fd, req, arg); while (r < 0 && errno == EINTR); return r; }

bool set_fmt(int fd, uint32_t type, uint32_t w, uint32_t h, uint32_t pixfmt, uint32_t* stride, uint32_t* size) {
  v4l2_format f{}; f.type = static_cast<v4l2_buf_type>(type);   // the RGA is a multi-planar mem2mem device (one plane for RGB)
  f.fmt.pix_mp.width = w; f.fmt.pix_mp.height = h; f.fmt.pix_mp.pixelformat = pixfmt; f.fmt.pix_mp.field = V4L2_FIELD_NONE; f.fmt.pix_mp.num_planes = 1;
  if (xioctl(fd, VIDIOC_S_FMT, &f) < 0) { std::perror("S_FMT"); return false; }
  if (f.fmt.pix_mp.pixelformat != pixfmt) { std::printf("driver changed the format to %.4s\n", reinterpret_cast<char*>(&f.fmt.pix_mp.pixelformat)); }
  *stride = f.fmt.pix_mp.plane_fmt[0].bytesperline; *size = f.fmt.pix_mp.plane_fmt[0].sizeimage;
  std::printf("%s: %ux%u %.4s stride %u size %u planes %u\n", type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE ? "output " : "capture", f.fmt.pix_mp.width, f.fmt.pix_mp.height,
              reinterpret_cast<char*>(&f.fmt.pix_mp.pixelformat), *stride, *size, f.fmt.pix_mp.num_planes);
  return f.fmt.pix_mp.width == w && f.fmt.pix_mp.height == h;
}

bool set_sel(int fd, uint32_t type, uint32_t target, int x, int y, int w, int h) {
  v4l2_selection s{}; s.type = type; s.target = target; s.r.left = x; s.r.top = y; s.r.width = static_cast<uint32_t>(w); s.r.height = static_cast<uint32_t>(h);
  if (xioctl(fd, VIDIOC_S_SELECTION, &s) < 0) { std::perror(target == V4L2_SEL_TGT_CROP ? "S_SELECTION crop" : "S_SELECTION compose"); return false; }
  if (s.r.left != x || s.r.top != y || static_cast<int>(s.r.width) != w || static_cast<int>(s.r.height) != h)
    std::printf("selection adjusted to %d,%d %ux%u\n", s.r.left, s.r.top, s.r.width, s.r.height);
  return true;
}

bool reqbufs(int fd, uint32_t type, uint32_t n) {
  v4l2_requestbuffers rb{}; rb.type = static_cast<v4l2_buf_type>(type); rb.memory = V4L2_MEMORY_DMABUF; rb.count = n;
  if (xioctl(fd, VIDIOC_REQBUFS, &rb) < 0) { std::perror("REQBUFS dmabuf"); return false; }
  return rb.count >= n;
}

bool qbuf(int fd, uint32_t type, uint32_t index, int dmafd, uint32_t bytes) {
  v4l2_plane pl{}; pl.m.fd = dmafd; pl.length = bytes; if (type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) pl.bytesused = bytes;
  v4l2_buffer b{}; b.type = static_cast<v4l2_buf_type>(type); b.memory = V4L2_MEMORY_DMABUF; b.index = index; b.m.planes = &pl; b.length = 1;
  if (xioctl(fd, VIDIOC_QBUF, &b) < 0) { std::perror("QBUF"); return false; }
  return true;
}
bool dqbuf(int fd, uint32_t type) {
  v4l2_plane pl{};
  v4l2_buffer b{}; b.type = static_cast<v4l2_buf_type>(type); b.memory = V4L2_MEMORY_DMABUF; b.m.planes = &pl; b.length = 1;
  if (xioctl(fd, VIDIOC_DQBUF, &b) < 0) { std::perror("DQBUF"); return false; }
  return true;
}
bool stream(int fd, uint32_t type, bool on) {
  int t = static_cast<int>(type);
  if (xioctl(fd, on ? VIDIOC_STREAMON : VIDIOC_STREAMOFF, &t) < 0) { std::perror(on ? "STREAMON" : "STREAMOFF"); return false; }
  return true;
}

double now_ms() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// One job: queue both, wait, dequeue both.
bool run_job(int fd, int sfd, uint32_t sbytes, int dfd, uint32_t dbytes) {
  if (!qbuf(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, 0, sfd, sbytes)) return false;
  if (!qbuf(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, 0, dfd, dbytes)) return false;
  pollfd p{fd, POLLIN | POLLOUT, 0};
  if (poll(&p, 1, 1000) <= 0) { std::printf("poll timed out\n"); return false; }
  return dqbuf(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) && dqbuf(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
}

void parse_wh(const char* env, uint32_t* w, uint32_t* h) { if (const char* s = std::getenv(env)) std::sscanf(s, "%ux%u", w, h); }
}

int main(int argc, char** argv) {
  const char* dev = argc > 1 ? argv[1] : "/dev/video2";
  const int iters = argc > 2 ? std::atoi(argv[2]) : 300;
  uint32_t sw = 256, sh = 192, dw = 1024, dh = 768;
  parse_wh("DS_RGA_SW", &sw, &sh); parse_wh("DS_RGA_DW", &dw, &dh);

  int fd = open(dev, O_RDWR | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) { std::perror(dev); return 1; }
  v4l2_capability cap{};
  if (xioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) { std::perror("QUERYCAP"); return 1; }
  std::printf("%s: driver %s card %s caps 0x%08x device caps 0x%08x\n", dev, cap.driver, cap.card, cap.capabilities, cap.device_caps);

  // Formats on offer.
  for (uint32_t type : {V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE}) {
    std::printf("%s formats:", type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE ? "output " : "capture");
    for (uint32_t i = 0;; ++i) {
      v4l2_fmtdesc d{}; d.type = type; d.index = i;
      if (xioctl(fd, VIDIOC_ENUM_FMT, &d) < 0) break;
      std::printf(" %.4s", reinterpret_cast<char*>(&d.pixelformat));
    }
    std::printf("\n");
  }

  // XRGB8888 little-endian in memory (B G R X) is V4L2_PIX_FMT_XBGR32 in V4L2's naming (byte order reversed).
  const uint32_t pix = std::getenv("DS_RGA_FMT") ? v4l2_fourcc(std::getenv("DS_RGA_FMT")[0], std::getenv("DS_RGA_FMT")[1], std::getenv("DS_RGA_FMT")[2], std::getenv("DS_RGA_FMT")[3]) : V4L2_PIX_FMT_XBGR32;
  uint32_t sstride, ssize, dstride, dsize;
  if (!set_fmt(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, sw, sh, pix, &sstride, &ssize)) return 1;
  if (!set_fmt(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, dw, dh, pix, &dstride, &dsize)) return 1;

  const int sfd = heap_alloc(ssize), dfd = heap_alloc(dsize);
  if (sfd < 0 || dfd < 0) { std::printf("no dma-buf allocator\n"); return 1; }
  auto* src = static_cast<uint32_t*>(mmap(nullptr, ssize, PROT_READ | PROT_WRITE, MAP_SHARED, sfd, 0));
  auto* dst = static_cast<uint32_t*>(mmap(nullptr, dsize, PROT_READ | PROT_WRITE, MAP_SHARED, dfd, 0));
  if (src == MAP_FAILED || dst == MAP_FAILED) { std::perror("mmap"); return 1; }
  // Pattern: red/green checker of 8x8 source pixels, blue ramp along y; the top-left pixel white.
  for (uint32_t y = 0; y < sh; ++y) for (uint32_t x = 0; x < sw; ++x) {
    const bool c = ((x >> 3) + (y >> 3)) & 1;
    src[y * (sstride / 4) + x] = 0xFF000000u | (c ? 0xFF0000u : 0x00FF00u) | (y * 255 / sh);
  }
  src[0] = 0xFFFFFFFFu;
  std::memset(dst, 0, dsize);

  if (!reqbufs(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, 1) || !reqbufs(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, 1)) return 1;
  if (!stream(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, true) || !stream(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, true)) return 1;

  // 1. Full-frame scale, timed.
  {
    std::vector<double> ms; ms.reserve(iters);
    if (!run_job(fd, sfd, ssize, dfd, dsize)) return 1;   // warm-up
    for (int i = 0; i < iters; ++i) {
      const double t0 = now_ms();
      if (!run_job(fd, sfd, ssize, dfd, dsize)) return 1;
      ms.push_back(now_ms() - t0);
    }
    std::sort(ms.begin(), ms.end());
    std::printf("scale %ux%u -> %ux%u: median %.3f ms, p90 %.3f, max %.3f (%d jobs)\n", sw, sh, dw, dh, ms[ms.size() / 2], ms[ms.size() * 9 / 10], ms.back(), iters);
    // Check: top-left white, a checker cell boundary at 8 source px = 8*(dw/sw) panel px.
    const uint32_t sx = dw / sw;
    const uint32_t p0 = dst[0] & 0xFFFFFF, p1 = dst[4 * sx] & 0xFFFFFF, p2 = dst[12 * sx] & 0xFFFFFF, pm = dst[(dh / 2) * (dstride / 4) + dw / 2] & 0xFFFFFF;
    std::printf("dst[0,0]=%06x [%u,0]=%06x [%u,0]=%06x centre=%06x (expect white, green-ish, red-ish, blue ramp ~0x7f)\n", p0, 4 * sx, p1, 12 * sx, p2, pm);
    // Filter: a 4x nearest scale leaves the pixels 0..3 of a cell identical; bilinear blends at the cell edge.
    const uint32_t e0 = dst[(8 * sx - 1)] & 0xFFFFFF, e1 = dst[8 * sx] & 0xFFFFFF;
    std::printf("cell edge %06x | %06x (%s)\n", e0, e1, (e0 == (dst[8 * sx - 2] & 0xFFFFFF) && e1 == (dst[8 * sx + 1] & 0xFFFFFF)) ? "nearest-like" : "blended");
  }

  // 1b. DS_RGA_BLEND=1: does the driver blend an ARGB source over the destination
  // (per-pixel alpha)? Source AR24 at alpha 0x80 (red) over a white destination:
  // blended gives ~(255,128,128), replaced gives (255,0,0) or the alpha byte ignored.
  if (std::getenv("DS_RGA_BLEND")) {
    stream(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, false); stream(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, false);
    v4l2_requestbuffers rb{}; rb.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE; rb.memory = V4L2_MEMORY_DMABUF; rb.count = 0; xioctl(fd, VIDIOC_REQBUFS, &rb);
    uint32_t s2, sz2;
    const uint32_t argb = v4l2_fourcc('A', 'R', '2', '4');
    if (!set_fmt(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, sw, sh, argb, &s2, &sz2)) return 1;
    reqbufs(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, 1);
    stream(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, true); stream(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, true);
    for (uint32_t i = 0; i < sw * sh; ++i) src[i] = 0x80FF0000u;   // A=0x80 R=0xFF
    for (uint32_t i = 0; i < dw * dh; ++i) dst[i] = 0xFFFFFFFFu;
    set_sel(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, V4L2_SEL_TGT_CROP, 0, 0, static_cast<int>(sw), static_cast<int>(sh));
    set_sel(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, V4L2_SEL_TGT_COMPOSE, 0, 0, static_cast<int>(dw), static_cast<int>(dh));
    if (!run_job(fd, sfd, sz2, dfd, dsize)) return 1;
    const uint32_t p = dst[(dh / 2) * (dstride / 4) + dw / 2];
    std::printf("ARGB source alpha 0x80 over white: dst = %08x (%s)\n", p, ((p >> 8) & 0xFF) > 0x40 && ((p >> 8) & 0xFF) < 0xC0 ? "BLENDED" : "not blended");
    return 0;
  }

  // 2. Two-screen frame: source crop rectangles into compose rectangles (top / bottom halves).
  {
    const int hh = static_cast<int>(dh) / 2;
    bool sel_ok = set_sel(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, V4L2_SEL_TGT_CROP, 0, 0, static_cast<int>(sw), static_cast<int>(sh) / 2)
               && set_sel(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, V4L2_SEL_TGT_COMPOSE, 0, 0, static_cast<int>(dw), hh);
    if (sel_ok) {
      std::vector<double> ms; ms.reserve(iters);
      for (int i = 0; i < iters; ++i) {
        const double t0 = now_ms();
        set_sel(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, V4L2_SEL_TGT_CROP, 0, 0, static_cast<int>(sw), static_cast<int>(sh) / 2);
        set_sel(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, V4L2_SEL_TGT_COMPOSE, 0, 0, static_cast<int>(dw), hh);
        if (!run_job(fd, sfd, ssize, dfd, dsize)) return 1;
        set_sel(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, V4L2_SEL_TGT_CROP, 0, static_cast<int>(sh) / 2, static_cast<int>(sw), static_cast<int>(sh) / 2);
        set_sel(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, V4L2_SEL_TGT_COMPOSE, 0, hh, static_cast<int>(dw), hh);
        if (!run_job(fd, sfd, ssize, dfd, dsize)) return 1;
        ms.push_back(now_ms() - t0);
      }
      std::sort(ms.begin(), ms.end());
      std::printf("two half-screen jobs (crop + compose) per frame: median %.3f ms, p90 %.3f, max %.3f\n", ms[ms.size() / 2], ms[ms.size() * 9 / 10], ms.back());
      const uint32_t top = dst[(hh / 2) * (dstride / 4) + dw / 2] & 0xFF, bot = dst[(hh + hh / 2) * (dstride / 4) + dw / 2] & 0xFF;
      std::printf("blue ramp: top half centre %u, bottom half centre %u (expect ~64 and ~191)\n", top, bot);
    } else std::printf("crop/compose selection not supported: the layout needs a job per screen into separate buffers, or the CPU\n");
  }

  stream(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, false); stream(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, false);
  // Optional dump for eyeballing: DS_RGA_DUMP=file.ppm
  if (const char* o = std::getenv("DS_RGA_DUMP")) {
    FILE* f = std::fopen(o, "wb");
    if (f) { std::fprintf(f, "P6\n%u %u\n255\n", dw, dh); for (uint32_t y = 0; y < dh; ++y) for (uint32_t x = 0; x < dw; ++x) { const uint32_t p = dst[y * (dstride / 4) + x]; const unsigned char rgb[3] = {static_cast<unsigned char>(p >> 16), static_cast<unsigned char>(p >> 8), static_cast<unsigned char>(p)}; std::fwrite(rgb, 1, 3, f); } std::fclose(f); }
  }
  return 0;
}

#else
int main() { std::fprintf(stderr, "rga_probe: this V4L2 has no multi-planar dma-buf mem2mem support\n"); return 2; }
#endif
