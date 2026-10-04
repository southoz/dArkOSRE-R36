// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"

#include <SDL2/SDL.h>

#if defined(__linux__)
#include <cerrno>
#include <ctime>
#include <pthread.h>
#include <sched.h>
#include <sys/prctl.h>
#endif

namespace ds::sdl {

// Frame limiter. Deadlines accumulate rather than resetting each frame, so a
// long frame is paid back by the next one; debt is capped at one frame so an
// alternating heavy/light pair stays on time on average.
class Pacer {
public:
  // period_ns: nominal frame period, e.g. CYCLES_PER_FRAME / ARM9_CLOCK_HZ (59.8261 Hz).
  explicit Pacer(double period_ns) {
#if defined(__linux__)
    prctl(PR_SET_TIMERSLACK, 1UL, 0, 0, 0);   // remove the kernel's default ~50us timer rounding
#endif
    set_period_ns(period_ns);
    reset();
  }

  void set_period_ns(double ns) { period_ = ns * ticks_per_ns(); }
  double period_ns() const { return period_ / ticks_per_ns(); }

  // Spin instead of sleeping between frames; keeps the core from reading as
  // idle to a polling cpufreq governor (see cpu_gov.h). Same frame rate either way.
  void set_busy_wait(bool on) { busy_ = on; }
  bool busy_wait() const { return busy_; }

  // Keep this thread under the kernel's RT bandwidth cap (sched_rt_runtime_us)
  // by sleeping off the surplus near the frame boundary, instead of being
  // throttled for up to 50ms when the budget runs out mid-frame.
  // `cap` is runtime/period; `period_ns` the kernel's RT accounting period.
  void set_rt_relief(double cap, double period_ns) {
    if (!(cap > 0.0 && cap < 1.0) || !(period_ns > 0.0)) return;
    allowed_ = cap - 0.01;   // small margin: throttling costs 50ms, being early costs us
    rt_period_ = period_ns * ticks_per_ns();
    relief_ = allowed_ > 0.0;
    window_ = SDL_GetPerformanceCounter();
    rt_busy_ = 0.0;
    frame_start_ = window_;
  }
  bool rt_relief() const { return relief_; }

  // Relief cost in us/frame since the last call, and frames it acted on.
  double relief_us(unsigned* frames = nullptr) {
    const double v = relief_n_ ? relief_ticks_ / ticks_per_ns() / 1e3 / relief_n_ : 0.0;
    if (frames) *frames = relief_n_;
    relief_ticks_ = 0; relief_n_ = 0;
    return v;
  }

  // Reset deadline to one period from now; call after unpause/load/speed change
  // so the gap isn't repaid as a burst.
  void reset() { next_ = SDL_GetPerformanceCounter(); }

  // This frame's deadline, in SDL performance-counter ticks (used by LAN slicing).
  Uint64 next() const { return next_; }

  // Advance to the next deadline and sleep until it. `scale`: 2.0 = double speed, 0.5 = half.
  void wait(double scale = 1.0) {
    next_ += static_cast<Uint64>(scale > 0.0 ? period_ / scale : period_);
    Uint64 now = SDL_GetPerformanceCounter();
    if (relief_) now = relieve(now);
    if (now >= next_) {
      const Uint64 budget = static_cast<Uint64>(period_);   // cap debt at one frame
      if (now - next_ > budget) next_ = now - budget;
    } else {
      sleep_until(next_, now);
    }
    if (relief_) frame_start_ = SDL_GetPerformanceCounter();   // RT run starts here, not before the wait
  }

  // Spin cost in us/frame, averaged since the last call. Informational only.
  double spin_us() {
    const double v = spin_n_ ? spin_ticks_ / ticks_per_ns() / 1e3 / spin_n_ : 0.0;
    spin_ticks_ = 0; spin_n_ = 0;
    return v;
  }

private:
  // Sleep wakeups run late by a variable amount, so the spin margin is learned
  // per-thread: jumps to any new lateness seen, decays slowly (1/512/frame,
  // ~2s to halve) otherwise. Asymmetric because being early costs microseconds
  // of spin, being late costs a missed frame.
  static constexpr double MARGIN_DECAY = 1.0 - 1.0 / 512.0;
  static constexpr double MARGIN_MAX_NS = 2e6;    // never hand more than 2 ms to the spin

  static double ticks_per_ns() {
    static const double v = static_cast<double>(SDL_GetPerformanceFrequency()) / 1e9;
    return v;
  }

  // Drops RT scheduling for the spin so it doesn't burn the thread's RT
  // bandwidth budget (sched_rt_runtime_us) and starve other threads on the core.
  void busy_until(Uint64 deadline, Uint64 now) {
#if defined(__linux__)
    int policy = 0;
    sched_param sp{};
    bool rt = pthread_getschedparam(pthread_self(), &policy, &sp) == 0 && (policy == SCHED_RR || policy == SCHED_FIFO);
    if (rt) {
      sched_param normal{};
      if (pthread_setschedparam(pthread_self(), SCHED_OTHER, &normal) != 0) rt = false;
    }
#endif
    while (SDL_GetPerformanceCounter() < deadline) {}
#if defined(__linux__)
    if (rt) pthread_setschedparam(pthread_self(), policy, &sp);
#endif
    spin_ticks_ += static_cast<double>(deadline - now);
    ++spin_n_;
  }

  // Charge the frame just run, then sleep off whatever exceeds the cap for the window.
  Uint64 relieve(Uint64 now) {
    rt_busy_ += static_cast<double>(now - frame_start_);
    const double elapsed = static_cast<double>(now - window_);
    if (elapsed >= rt_period_) {          // new accounting window
      window_ = now; rt_busy_ = 0.0;
      return now;
    }
    const double over = rt_busy_ - allowed_ * elapsed;
    if (over <= 0.0) return now;
    const double cap_ticks = period_ * 0.25;   // spread the cost; cap at a quarter frame per nap
    const double nap = over < cap_ticks ? over : cap_ticks;
#if defined(__linux__)
    const double nap_ns = nap / ticks_per_ns();
    timespec ts{0, static_cast<long>(nap_ns)};
    while (clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, &ts) == EINTR) {}
#endif
    const Uint64 woke = SDL_GetPerformanceCounter();
    relief_ticks_ += static_cast<double>(woke - now);
    ++relief_n_;
    return woke;
  }

  void sleep_until(Uint64 deadline, Uint64 now) {
    if (busy_) { busy_until(deadline, now); return; }
    const double margin = margin_ticks_;
    const double left = static_cast<double>(deadline - now);
    if (left > margin) {
      const double nap_ns = (left - margin) / ticks_per_ns();
      const Uint64 asked = deadline - static_cast<Uint64>(margin);
#if defined(__linux__)
      // Absolute deadline: a signal restart doesn't shorten/lengthen the frame.
      timespec mono{};
      clock_gettime(CLOCK_MONOTONIC, &mono);
      long long end = mono.tv_sec * 1'000'000'000LL + mono.tv_nsec + static_cast<long long>(nap_ns);
      timespec until{static_cast<time_t>(end / 1'000'000'000LL), static_cast<long>(end % 1'000'000'000LL)};
      while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &until, nullptr) == EINTR) {}
#else
      const double nap_ms = nap_ns / 1e6;
      if (nap_ms > 1.0) SDL_Delay(static_cast<Uint32>(nap_ms));
#endif
      const Uint64 woke = SDL_GetPerformanceCounter();
      const double late = static_cast<double>(woke) - static_cast<double>(asked);
      const double cap = MARGIN_MAX_NS * ticks_per_ns();
      margin_ticks_ = late > margin_ticks_ ? (late < cap ? late : cap) : margin_ticks_ * MARGIN_DECAY;
      if (woke >= deadline) return;      // already past deadline
      spin_ticks_ += static_cast<double>(deadline - woke);
    }
    ++spin_n_;
    while (SDL_GetPerformanceCounter() < deadline) {}
  }

  bool busy_ = false;        // spin the wait instead of sleeping it
  bool relief_ = false;      // keep this thread under the kernel's RT bandwidth cap
  double allowed_ = 0.0;     // fraction of wall time we may spend running at RT
  double rt_period_ = 0.0;   // the kernel's RT accounting period, in ticks
  Uint64 window_ = 0;        // start of the current accounting window
  double rt_busy_ = 0.0;     // ticks run at RT priority in this window
  Uint64 frame_start_ = 0;   // when this frame's real-time run began
  double relief_ticks_ = 0;  // what the relief has cost since the last relief_us()
  unsigned relief_n_ = 0;
  double period_ = 0;        // in performance-counter ticks
  Uint64 next_ = 0;
  double margin_ticks_ = 0;  // learned; starts at zero and grows into the first few frames
  double spin_ticks_ = 0;    // what the spin has cost since the last spin_us()
  unsigned spin_n_ = 0;
};

} // namespace ds::sdl
