// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/handoff_stats.h"

#include <chrono>
#include <cstdlib>

namespace ds::handoff {

const bool enabled = std::getenv("DS_HANDOFF_STATS") != nullptr;

u64 now_ns() {
  return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

void Hist::add(u64 ns) {
  n.fetch_add(1, std::memory_order_relaxed);
  sum.fetch_add(ns, std::memory_order_relaxed);
  u64 m = max.load(std::memory_order_relaxed);
  while (ns > m && !max.compare_exchange_weak(m, ns, std::memory_order_relaxed)) {}
  const int i = ns < 10'000 ? 0 : ns < 50'000 ? 1 : ns < 200'000 ? 2 : ns < 1'000'000 ? 3 : ns < 5'000'000 ? 4 : 5;
  b[i].fetch_add(1, std::memory_order_relaxed);
}

Stats& stats() {
  static Stats s;
  return s;
}

namespace {
void line(std::FILE* f, const char* what, const Hist& h) {
  const u64 n = h.n.load(std::memory_order_relaxed);
  if (!n) return;
  std::fprintf(f, "    %-12s %9llu  mean %7.1f us  max %8.1f us  total %8.1f ms  | <10us %llu, <50 %llu, <200 %llu, <1ms %llu, <5ms %llu, more %llu\n", what,
               static_cast<unsigned long long>(n), static_cast<double>(h.sum.load()) / 1e3 / static_cast<double>(n),
               static_cast<double>(h.max.load()) / 1e3, static_cast<double>(h.sum.load()) / 1e6,
               static_cast<unsigned long long>(h.b[0].load()), static_cast<unsigned long long>(h.b[1].load()),
               static_cast<unsigned long long>(h.b[2].load()), static_cast<unsigned long long>(h.b[3].load()),
               static_cast<unsigned long long>(h.b[4].load()), static_cast<unsigned long long>(h.b[5].load()));
}
} // namespace

void report(std::FILE* f) {
  if (!enabled) return;
  static const char* const kName[KINDS] = {"2D engine A worker", "2D engine B worker", "3D bands"};
  Stats& s = stats();
  std::fprintf(f, "hand-offs (DS_HANDOFF_STATS):\n");
  for (int k = 0; k < KINDS; ++k) {
    if (!s.wake[k].n.load() && !s.wake_parked[k].n.load() && !s.wait[k].n.load()) continue;
    std::fprintf(f, "  %s:\n", kName[k]);
    line(f, "wake spin", s.wake[k]);
    line(f, "wake parked", s.wake_parked[k]);
    line(f, "job", s.job[k]);
    line(f, "wait", s.wait[k]);
    line(f, "back", s.back[k]);
    std::fprintf(f, "    waits by join site (catch_up, trap, journal, line0, remap, ranges_pre, ranges_post, other):");
    for (int i = 0; i < 8; ++i) std::fprintf(f, " %llu", static_cast<unsigned long long>(s.wait_site[k][i].load()));
    std::fputc('\n', f);
  }
}

} // namespace ds::handoff
