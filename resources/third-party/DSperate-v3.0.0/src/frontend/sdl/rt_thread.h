// Per-thread scheduling helper for the presenter threads: all threads inherit
// the same SCHED_RR priority from main.cpp, which can leave a latency-bound
// presenter queued behind the emulation thread for a full RR slice on
// oversubscribed cores. Give it one step above.
#pragma once

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#include <cstdio>
#endif

namespace ds::sdl {

// Raise the calling thread one RT priority step, if it has an RT policy. No-op otherwise.
inline void raise_presenter_priority(const char* who) {
#if defined(__linux__)
  int policy = 0;
  sched_param sp{};
  if (pthread_getschedparam(pthread_self(), &policy, &sp) != 0) return;
  if (policy != SCHED_RR && policy != SCHED_FIFO) return;
  const int max = sched_get_priority_max(policy);
  if (sp.sched_priority >= max) return;
  sp.sched_priority += 1;
  if (pthread_setschedparam(pthread_self(), policy, &sp) != 0) std::perror(who);
#else
  (void)who;
#endif
}

} // namespace ds::sdl
