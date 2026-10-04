// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// One worker thread that runs a fixed job on request, for engine B's share of
// display lines while the calling thread renders engine A's. Spins before
// parking, since per-line dispatch (up to 192/frame) is cheaper than waking
// a condition variable.
#pragma once

#include "core/handoff_stats.h"
#include "core/host_cores.h"
#include "core/types.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace ds::gpu {

class LineWorker {
public:
  LineWorker() = default;
  ~LineWorker() { stop(); }
  LineWorker(const LineWorker&) = delete;
  LineWorker& operator=(const LineWorker&) = delete;

  // `fn`/`arg` is the job; it runs on the worker for every dispatch().
  // `role`/`name`: the thread's placement (host_cores.h) and name.
  void start(void (*fn)(void*), void* arg, ThreadRole role = ThreadRole::Line, const char* name = "line-worker") {
    if (thread_.joinable()) return;
    fn_ = fn; arg_ = arg; role_ = role; name_ = name;
    kind_ = role == ThreadRole::EngineA ? handoff::EngineA : handoff::EngineB;
    quit_.store(false, std::memory_order_relaxed);
    req_.store(0, std::memory_order_relaxed);
    ack_.store(0, std::memory_order_relaxed);
    thread_ = std::thread([this] { loop(); });
  }

  void stop() {
    if (!thread_.joinable()) return;
    quit_.store(true, std::memory_order_relaxed);
    { std::lock_guard<std::mutex> lk(m_); }
    cv_.notify_all();
    thread_.join();
  }

  bool running() const { return thread_.joinable(); }
  bool parked() const { return parked_.load(std::memory_order_relaxed); }
  // For a stalled-frame watchdog (DS_WATCHDOG).
  void debug_dump(FILE* f) const {
    std::fprintf(f, "  line worker: running %d req %u ack %u parked %d\n", running() ? 1 : 0,
                 req_.load(std::memory_order_relaxed), ack_.load(std::memory_order_relaxed), parked_.load(std::memory_order_relaxed) ? 1 : 0);
  }

  // Must be paired with wait() before reading the job's output or dispatching again.
  void dispatch() {
    const u32 n = req_.load(std::memory_order_relaxed) + 1;
    if (handoff::enabled) disp_ns_.store(handoff::now_ns(), std::memory_order_relaxed);
    // seq_cst: req_/parked_ form a Dekker pair with the worker's park; weaker
    // ordering lets both sides read stale and wait() spins forever.
    req_.store(n, std::memory_order_seq_cst);
    // qemu-user models seq_cst stlr/ldar as plain release/acquire, which
    // could reorder this store past the load below; real hardware doesn't need it.
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (parked_.load(std::memory_order_seq_cst)) {
      std::lock_guard<std::mutex> lk(m_);
      cv_.notify_one();
    }
  }

  void wait(int site = 7) {
    const u32 n = req_.load(std::memory_order_relaxed);
    if (ack_.load(std::memory_order_acquire) == n) return;
    if (handoff::enabled) handoff::stats().wait_site[kind_][site & 7].fetch_add(1, std::memory_order_relaxed);
    const u64 t0 = handoff::enabled ? handoff::now_ns() : 0;
    int spins = 4000;
    while (ack_.load(std::memory_order_acquire) != n) {
      if (--spins > 0) cpu_relax();
      else std::this_thread::yield();
    }
    if (handoff::enabled) {
      const u64 t1 = handoff::now_ns(), done = done_ns_.load(std::memory_order_relaxed);
      handoff::stats().wait[kind_].add(t1 - t0);
      if (done > t0 && done <= t1) handoff::stats().back[kind_].add(t1 - done);
    }
  }

private:
  static void cpu_relax() {
#if defined(__aarch64__) || defined(__arm__)
    asm volatile("yield" ::: "memory");
#elif defined(__x86_64__) || defined(__i386__)
    asm volatile("pause" ::: "memory");
#else
    std::this_thread::yield();
#endif
  }

  void loop() {
    name_current_thread(name_);
    place_current_thread(role_);
    u32 last = 0;
    for (;;) {
      // Spin budget before parking (~line gap). DS_LINE_SPIN overrides.
      static const int kSpin = std::getenv("DS_LINE_SPIN") ? std::atoi(std::getenv("DS_LINE_SPIN")) : 20000;
      int spins = kSpin;
      bool slept = false;
      u32 r = req_.load(std::memory_order_acquire);
      while (r == last) {
        if (quit_.load(std::memory_order_relaxed)) return;
        if (--spins > 0) { cpu_relax(); r = req_.load(std::memory_order_acquire); continue; }
        std::unique_lock<std::mutex> lk(m_);
        parked_.store(true, std::memory_order_seq_cst);
        std::atomic_thread_fence(std::memory_order_seq_cst);   // see dispatch()
        // Re-check: catches a dispatch that raced the park and skipped the notify.
        r = req_.load(std::memory_order_seq_cst);
        if (r == last && !quit_.load(std::memory_order_relaxed))
          cv_.wait(lk, [&] {
            r = req_.load(std::memory_order_acquire);
            return r != last || quit_.load(std::memory_order_relaxed);
          });
        parked_.store(false, std::memory_order_seq_cst);
        if (quit_.load(std::memory_order_relaxed)) return;
        spins = kSpin;
        slept = true;
      }
      last = r;
      if (handoff::enabled) {
        const u64 d = handoff::now_ns() - disp_ns_.load(std::memory_order_relaxed);
        (slept ? handoff::stats().wake_parked[kind_] : handoff::stats().wake[kind_]).add(d);
      }
      const u64 j0 = handoff::enabled ? handoff::now_ns() : 0;
      fn_(arg_);
      if (handoff::enabled) { const u64 j1 = handoff::now_ns(); handoff::stats().job[kind_].add(j1 - j0); done_ns_.store(j1, std::memory_order_relaxed); }
      ack_.store(r, std::memory_order_release);
    }
  }

  std::thread thread_;
  ThreadRole role_ = ThreadRole::Line;
  const char* name_ = "line-worker";
  void (*fn_)(void*) = nullptr;
  void* arg_ = nullptr;
  std::atomic<u32> req_{0}, ack_{0};
  std::atomic<bool> quit_{false}, parked_{false};
  handoff::Kind kind_ = handoff::EngineB;
  std::atomic<u64> disp_ns_{0}, done_ns_{0};   // DS_HANDOFF_STATS: last dispatch, last job end
  std::mutex m_;
  std::condition_variable cv_;
};

} // namespace ds::gpu
