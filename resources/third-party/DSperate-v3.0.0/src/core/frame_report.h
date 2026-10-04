// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Per-frame host time report shared by both frontends so headless and SDL
// runs stay comparable. The input series must be emulation work only --
// frame pacing (vsync, audio queue) must be excluded or the tail measures
// display refresh instead of the emulator.
#pragma once

#include <algorithm>
#include <cstdio>
#include <vector>

namespace ds {

// DS frame period. Frames over budget are dropped/underrun frames; report
// both mean and tail since the mean can improve while the tail worsens.
inline constexpr double kFrameBudgetMs = 1000.0 / 59.8261;

// `label` names the series. SDL also prints a "work" series (emulation +
// present, pacing excluded), meaningful only with --no-vsync.
inline void frame_report(const std::vector<double>& frame_ms, const char* label = "frame") {
  if (frame_ms.empty()) return;
  std::vector<double> v = frame_ms;
  std::sort(v.begin(), v.end());
  double sum = 0; for (double x : v) sum += x;
  const size_t n = v.size();
  auto pct = [&](double p) { return v[std::min(n - 1, static_cast<size_t>(p * n))]; };
  size_t over = 0; for (double x : v) if (x > kFrameBudgetMs) ++over;
  // p95 is the gating tier; p99/max are reported but not gated on since
  // loading screens legitimately spend frame time there.
  std::fprintf(stderr, "%s ms: median %.3f mean %.3f p90 %.3f p95 %.3f p99 %.3f max %.3f min %.3f total %.1f\n",
               label, v[n / 2], sum / n, pct(0.90), pct(0.95), pct(0.99), v.back(), v.front(), sum);
  std::fprintf(stderr, "%s budget: %zu of %zu frames over %.3f ms (%.2f%%)\n",
               label, over, n, kFrameBudgetMs, 100.0 * static_cast<double>(over) / static_cast<double>(n));

  // Locate overruns for --dump-from/--dump-count; frame_ms is in run order.
  if (!over) return;
  size_t worst_i = 0, bursts = 0, longest = 0, longest_at = 0, cur = 0, cur_at = 0;
  for (size_t i = 0; i < frame_ms.size(); ++i) {
    if (frame_ms[i] > frame_ms[worst_i]) worst_i = i;
    if (frame_ms[i] > kFrameBudgetMs) {
      if (cur == 0) { ++bursts; cur_at = i; }
      if (++cur > longest) { longest = cur; longest_at = cur_at; }
    } else cur = 0;
  }
  std::fprintf(stderr, "  worst frame #%zu at %.3f ms; %zu bursts, longest %zu frames from #%zu\n",
               worst_i, frame_ms[worst_i], bursts, longest, longest_at);
  // Ten windows over the run; a spike marks a section worth dumping.
  constexpr size_t W = 10;
  const size_t span = (frame_ms.size() + W - 1) / W;
  std::fprintf(stderr, "  over-budget per %zu-frame window:", span);
  for (size_t w = 0; w < W; ++w) {
    size_t c = 0;
    for (size_t i = w * span; i < std::min(frame_ms.size(), (w + 1) * span); ++i)
      if (frame_ms[i] > kFrameBudgetMs) ++c;
    std::fprintf(stderr, " %zu", c);
  }
  std::fputc('\n', stderr);
}

}  // namespace ds
