// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Hand-off latency between the emulation thread and the video workers
// (DS_HANDOFF_STATS=1), for judging scheduling policy and placement:
//   wake  -- dispatch until the worker starts the job (split by whether the
//            worker had parked on its condition variable or was still spinning)
//   wait  -- how long a waiter blocked for the job's result
//   back  -- the job done until a blocked waiter runs again
// Cheap enough to leave in: one relaxed flag test per hand-off when off.
#pragma once
#include "core/types.h"

#include <atomic>
#include <cstdio>

namespace ds::handoff {

enum Kind : u8 { EngineA, EngineB, Band, KINDS };

extern const bool enabled;

u64 now_ns();

// Log2-ish buckets: <10us, <50us, <200us, <1ms, <5ms, more.
struct Hist {
  static constexpr int BUCKETS = 6;
  std::atomic<u64> n{0}, sum{0}, max{0}, b[BUCKETS]{};
  void add(u64 ns);
};

struct Stats {
  Hist wake[KINDS], wake_parked[KINDS], wait[KINDS], back[KINDS], job[KINDS];   // job: the worker's own run time per dispatch
  std::atomic<u64> wait_site[KINDS][8]{};   // waits by the caller's join site (gpu.cpp JoinSite order)
};
Stats& stats();

// One block per kind that saw any traffic; nothing when disabled.
void report(std::FILE* f);

} // namespace ds::handoff
