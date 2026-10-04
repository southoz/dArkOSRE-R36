// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// The GPU present off the emulation thread: Display hands a frame's
// presenter work (Vulkan upload, dispatch, submit, fence, the sink's commit)
// to one thread on the aux core, and the emulation thread goes on to the next
// frame. What the present costs in kernel work (the driver's job submit, the
// compositor wakeup, the per-CPU kworker they queue) then lands on the aux
// core instead of the emulation thread's.
#pragma once
#include "core/host_cores.h"
#include "core/types.h"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace ds::sdl {

class PresentThread {
public:
  // One per process, started by the first post().
  static PresentThread& get() {
    static PresentThread t;
    return t;
  }
  ~PresentThread() {
    if (!thread_.joinable()) return;
    { std::lock_guard<std::mutex> lk(m_); quit_ = true; }
    cv_.notify_all();
    thread_.join();
  }

  // Queues `job` behind the ones already posted; returns its ticket.
  u64 post(std::function<void()> job) {
    std::lock_guard<std::mutex> lk(m_);
    if (!thread_.joinable()) thread_ = std::thread([this] { loop(); });
    jobs_.push_back(std::move(job));
    const u64 t = ++posted_;
    cv_.notify_all();
    return t;
  }

  // Blocks until the job with `ticket` (and every one before it) has run.
  void wait(u64 ticket) {
    if (!ticket) return;
    std::unique_lock<std::mutex> lk(m_);
    if (done_ >= ticket) return;
    const auto t0 = std::chrono::steady_clock::now();
    done_cv_.wait(lk, [&] { return done_ >= ticket; });
    const u64 ns = static_cast<u64>((std::chrono::steady_clock::now() - t0).count());
    wait_ns_ += ns; ++waits_;
    if (ns > 500'000) ++long_waits_;
  }

  struct Stats { u64 jobs, job_ns, job_max_ns, waits, wait_ns, long_waits; };
  Stats stats() {
    std::lock_guard<std::mutex> lk(m_);
    return Stats{done_, job_ns_, job_max_ns_, waits_, wait_ns_, long_waits_};
  }

private:
  PresentThread() = default;

  void loop() {
    name_current_thread("present");
    place_current_thread(ThreadRole::Aux);
#if defined(__linux__)
    // A normal task by default: it is off the frame's critical path, and an
    // RT thread would keep the kernel work a present queues on its core
    // (the kworker) waiting behind it. DS_PRESENT_RT=1 keeps the inherited policy.
    if (!std::getenv("DS_PRESENT_RT")) { sched_param z{}; pthread_setschedparam(pthread_self(), SCHED_OTHER, &z); }
#endif
    std::unique_lock<std::mutex> lk(m_);
    for (;;) {
      cv_.wait(lk, [&] { return quit_ || !jobs_.empty(); });
      if (jobs_.empty()) return;   // quit, nothing left
      std::function<void()> job = std::move(jobs_.front());
      jobs_.pop_front();
      lk.unlock();
      const auto t0 = std::chrono::steady_clock::now();
      job();
      const u64 ns = static_cast<u64>((std::chrono::steady_clock::now() - t0).count());
      lk.lock();
      job_ns_ += ns; if (ns > job_max_ns_) job_max_ns_ = ns;
      ++done_;
      done_cv_.notify_all();
    }
  }

  std::thread thread_;
  std::mutex m_;
  std::condition_variable cv_, done_cv_;
  std::deque<std::function<void()>> jobs_;
  bool quit_ = false;
  u64 posted_ = 0, done_ = 0;
  u64 job_ns_ = 0, job_max_ns_ = 0, waits_ = 0, wait_ns_ = 0, long_waits_ = 0;
};

} // namespace ds::sdl
