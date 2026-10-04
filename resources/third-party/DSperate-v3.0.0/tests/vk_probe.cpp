// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// vk_probe DUMP [iterations] [out.ppm|out.raw]: draws a recorded frame
// (headless --dump-gpu-frame, vk_dump.h) on the GPU raster (vk_lean.h) over
// and over and reports the cost, so a device can be measured without the
// emulator around it. DS_VK_MSAA=1 draws with 4x MSAA, DS_VK_TIMING=1 adds
// the GPU's own time. out.raw writes the 3D layer record itself.
#include "core/gpu/vk/vk_device.h"
#include "core/gpu/vk/vk_dump.h"
#include "core/gpu/vk/vk_lean.h"

#include <chrono>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

using namespace ds;
using namespace ds::gpu::vk;

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "usage: vk_probe DUMP [iterations] [out.ppm]\n"); return 2; }
  const int iters = argc > 2 ? std::atoi(argv[2]) : 200;
  const char* ppm = argc > 3 ? argv[3] : nullptr;

  std::FILE* f = std::fopen(argv[1], "rb");
  if (!f) { std::fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
  GpuDumpHeader h;
  if (std::fread(&h, sizeof h, 1, f) != 1 || h.magic != GpuDumpHeader::kMagic) { std::fprintf(stderr, "not a gpu frame dump\n"); return 1; }
  std::vector<GpuPoly> gp(h.npoly); std::vector<GpuVert> gv(h.nvert); std::vector<u32> tex(h.ntexels), shrun(static_cast<size_t>(h.npoly) * DS_SHRUN_LINES), rowpoly(h.nrows);
  auto rd = [&](void* p, size_t n) { return n == 0 || std::fread(p, n, 1, f) == 1; };
  if (!rd(gp.data(), sizeof(GpuPoly) * gp.size()) || !rd(gv.data(), sizeof(GpuVert) * gv.size()) || !rd(tex.data(), sizeof(u32) * tex.size()) ||
      !rd(shrun.data(), sizeof(u32) * shrun.size()) || !rd(rowpoly.data(), sizeof(u32) * rowpoly.size())) { std::fprintf(stderr, "short dump\n"); return 1; }
  std::fclose(f);
  // Experiment switches: DS_PROBE_DISPCNT=hex replaces DISP3DCNT (bit 4 AA, bit 5 edge
  // marking, bit 7 fog); DS_PROBE_NOTEX=1 draws every polygon untextured.
  if (const char* e = std::getenv("DS_PROBE_DISPCNT")) h.f.dispcnt = static_cast<u32>(std::strtoul(e, nullptr, 16));
  if (const char* e = std::getenv("DS_PROBE_NOTEX"); e && std::atoi(e)) for (auto& p : gp) p.flags &= ~(DS_PF_TEXTURED | DS_PF_TEX_ALPHA);
  std::printf("frame %llu: %u polygons (%u opaque prefix), %u vertices, %u texel words, %u rows, dispcnt %04x%s\n",
              (unsigned long long)h.frame, h.npoly, h.f.first_ordered, h.nvert, h.ntexels, h.nrows, h.f.dispcnt, (h.f.flags & DS_FF_WBUFFER) ? ", W-buffer" : "");

  std::string why;
  std::shared_ptr<Device> dev = Device::shared(&why);
  if (!dev) { std::fprintf(stderr, "no Vulkan device: %s\n", why.c_str()); return 1; }
  std::printf("device: %s (graphics %d, msaa4 %d, int64 atomics %d, timestamps %s)\n", dev->name().c_str(), (int)dev->limits().graphics, (int)dev->limits().msaa4,
              (int)dev->limits().int64_atomics, dev->limits().timestamp_period_ns > 0 ? "yes" : "no");
  std::unique_ptr<Lean> L = Lean::create(*dev, std::getenv("DS_VK_MSAA") && std::atoi(std::getenv("DS_VK_MSAA")) != 0, &why);
  if (!L || !L->ready()) { std::fprintf(stderr, "GPU raster unavailable: %s\n", why.c_str()); return 1; }

  auto upload = [&]() {
    u32 cap = 0; u32* t = L->texel_buffer(&cap);
    std::memcpy(L->poly_buffer(), gp.data(), sizeof(GpuPoly) * gp.size());
    std::memcpy(L->vert_buffer(), gv.data(), sizeof(GpuVert) * gv.size());
    if (tex.size() > cap) { std::fprintf(stderr, "texel arena too small (%zu > %u)\n", tex.size(), cap); std::exit(1); }
    std::memcpy(t, tex.data(), sizeof(u32) * tex.size());
    *L->post_buffer() = h.post;
  };
  auto submit = [&]() { return L->submit(h.npoly, h.nvert, h.ntexels, h.f); };
  auto waitf = [&]() { L->wait(); };

  // Warm-up: pipelines, first-use allocations.
  upload();
  if (!submit()) { std::fprintf(stderr, "submit refused\n"); return 1; }
  waitf();

  std::vector<double> ms; ms.reserve(iters);
  std::vector<double> wait_ms; wait_ms.reserve(iters);
  std::vector<double> gms; gms.reserve(iters);
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < iters; ++i) {
    const auto a = std::chrono::steady_clock::now();
    upload();
    if (!submit()) { std::fprintf(stderr, "submit refused at %d\n", i); return 1; }
    // DS_PROBE_SLEEP=ms: sleep after the submit, then time the wait alone: a
    // wait that still takes the frame's GPU time means the job did not run
    // until the wait (driver deferral).
    static const int sleep_ms = std::getenv("DS_PROBE_SLEEP") ? std::atoi(std::getenv("DS_PROBE_SLEEP")) : 0;
    if (sleep_ms) {
      std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
      const auto w0 = std::chrono::steady_clock::now();
      waitf();
      wait_ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - w0).count());
    } else waitf();
    ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count());
    if (L->gpu_ns()) gms.push_back(static_cast<double>(L->gpu_ns()) / 1e6);
  }
  if (!wait_ms.empty()) {
    std::sort(wait_ms.begin(), wait_ms.end());
    std::printf("wait after sleep: median %.2f ms, p90 %.2f, max %.2f\n", wait_ms[wait_ms.size() / 2], wait_ms[std::min(wait_ms.size() - 1, static_cast<size_t>(0.9 * wait_ms.size()))], wait_ms.back());
  }
  if (!gms.empty()) {
    std::sort(gms.begin(), gms.end());
    std::printf("GPU time per frame median %.2f ms, p90 %.2f, max %.2f; %u draws\n", gms[gms.size() / 2], gms[std::min(gms.size() - 1, static_cast<size_t>(0.9 * gms.size()))], gms.back(), L->draws());
  }
  const double total = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  std::sort(ms.begin(), ms.end());
  auto q = [&](double p) { return ms[std::min(ms.size() - 1, static_cast<size_t>(p * ms.size()))]; };
  std::printf("submit+wait per frame: median %.2f ms, p90 %.2f, p99 %.2f, max %.2f (%d frames, %.1f ms total)\n", q(0.5), q(0.9), q(0.99), ms.back(), iters, total);
  if (ppm) {
    const u32 S = 1u;
    const u32* out = L->output();
    std::FILE* o = std::fopen(ppm, "wb");
    if (!o) { std::fprintf(stderr, "cannot write %s\n", ppm); return 1; }
    if (std::strlen(ppm) > 4 && !std::strcmp(ppm + std::strlen(ppm) - 4, ".raw")) {   // the record itself: RGB666 + 5-bit alpha words
      std::fwrite(out, sizeof(u32), 256 * 192 * S * S, o); std::fclose(o); return 0;
    }
    std::fprintf(o, "P6\n%u %u\n255\n", 256 * S, 192 * S);
    for (u32 i = 0; i < 256 * 192 * S * S; ++i) {
      const u32 c = out[i];
      const unsigned char px[3] = {static_cast<unsigned char>((c & 0x3F) * 255 / 63), static_cast<unsigned char>(((c >> 8) & 0x3F) * 255 / 63), static_cast<unsigned char>(((c >> 16) & 0x3F) * 255 / 63)};
      std::fwrite(px, 1, 3, o);
    }
    std::fclose(o);
    std::printf("wrote %s\n", ppm);
  }
  return 0;
}
