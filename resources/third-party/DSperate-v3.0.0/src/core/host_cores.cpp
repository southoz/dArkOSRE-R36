// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/host_cores.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>

#include <mutex>
#include <vector>

#if defined(__linux__)
#include <dirent.h>
#include <pthread.h>
#include <sched.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace ds {

namespace {

constexpr u32 MAX_CPUS = 64;

int forced_cores() {
  static const int n = [] { const char* e = std::getenv("DS_HOST_CORES"); return e ? std::atoi(e) : 0; }();
  return n;
}
std::atomic<u32> g_configured_cores{0};

#if defined(__linux__)
// "0,3" or "0-3,6" -> mask. 0 when unreadable.
u64 read_online_mask() {
  FILE* f = std::fopen("/sys/devices/system/cpu/online", "r");
  if (!f) return 0;
  char buf[256];
  const size_t n = std::fread(buf, 1, sizeof buf - 1, f);
  std::fclose(f);
  buf[n] = 0;
  u64 mask = 0;
  for (char* p = buf; *p;) {
    char* end;
    const unsigned long lo = std::strtoul(p, &end, 10);
    if (end == p) break;
    unsigned long hi = lo;
    if (*end == '-') hi = std::strtoul(end + 1, &end, 10);
    for (unsigned long c = lo; c <= hi && c < MAX_CPUS; ++c) mask |= u64{1} << c;
    p = *end == ',' ? end + 1 : end;
    if (*p == '\n') break;
  }
  return mask;
}

// Process affinity as first read.
u64 read_affinity() {
  cpu_set_t set;
  CPU_ZERO(&set);
  if (sched_getaffinity(0, sizeof set, &set) != 0) return ~u64{0};
  u64 m = 0;
  for (u32 c = 0; c < MAX_CPUS; ++c) if (CPU_ISSET(c, &set)) m |= u64{1} << c;
  return m ? m : ~u64{0};
}
std::atomic<u64> g_startup_affinity{0};
u64 startup_affinity() {
  u64 m = g_startup_affinity.load(std::memory_order_relaxed);
  if (!m) { m = read_affinity(); g_startup_affinity.store(m, std::memory_order_relaxed); }
  return m;
}

// The usable set, re-read at most once a second.
u64 usable_mask() {
  static std::atomic<u64> cached{0};
  static std::atomic<s64> read_at{0};
  const s64 now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  u64 m = cached.load(std::memory_order_relaxed);
  if (!m || now - read_at.load(std::memory_order_relaxed) >= 1000) {
    const u64 aff = startup_affinity();
    const u64 online = read_online_mask();
    m = online ? (online & aff) : aff;
    if (!m) m = online ? online : 1;
    cached.store(m, std::memory_order_relaxed);
    read_at.store(now, std::memory_order_relaxed);
  }
  return m;
}
#endif

} // namespace

void set_host_cores(u32 n) { g_configured_cores.store(n, std::memory_order_relaxed); }

u32 host_cores() {
  if (forced_cores() > 0) return static_cast<u32>(forced_cores());
  if (const u32 n = g_configured_cores.load(std::memory_order_relaxed)) return n;
#if defined(__linux__)
  const u64 m = usable_mask();
  if (m != ~u64{0}) return static_cast<u32>(__builtin_popcountll(m));
#endif
  const u32 hc = std::thread::hardware_concurrency();
  return hc ? hc : 4u;
}

void name_current_thread(const char* name) {
#if defined(__linux__)
  prctl(PR_SET_NAME, name, 0, 0, 0);
#else
  (void)name;
#endif
}

// ---- thread layout -----------------------------------------------------------------------------
namespace {
std::atomic<bool> g_layout{false};
std::mutex g_placed_m;
std::vector<long> g_placed;   // tids placed by role

#if defined(__linux__)
long current_tid() { return static_cast<long>(syscall(SYS_gettid)); }

// The four cores the layout uses, in role order {aux, engine A pair, engine
// B pair, emu}: the startup set's first four, lowest first (CPU0 takes the
// interrupts on the RK3566, so it is aux). 0 cores: layout unavailable.
u32 layout_cores(u32 out[4]) {
  const u64 m = startup_affinity();
  u32 n = 0;
  for (u32 c = 0; c < MAX_CPUS && n < 4; ++c) if (m & (u64{1} << c)) out[n++] = c;
  return n;
}

u64 role_mask_default(ThreadRole role, u32 index) {
  u32 c[4];
  if (layout_cores(c) < 4) return 0;
  switch (role) {
    case ThreadRole::Emu:     return u64{1} << c[3];
    case ThreadRole::EngineA: return u64{1} << c[1];
    case ThreadRole::Line:    return u64{1} << c[2];
    case ThreadRole::Band:    return u64{1} << (index == 0 ? c[1] : index == 1 ? c[2] : c[0]);
    case ThreadRole::Aux:     return u64{1} << c[0];
  }
  return 0;
}

// DS_PIN="emu=3,band0=1,band1=2,line=2,enginea=1,aux=0": a role's CPU list
// ("0-1" for a range). "band" names every band. 0 if the role is not named.
u64 role_mask_env(const char* key) {
  const char* e = std::getenv("DS_PIN");
  if (!e) return 0;
  const size_t kl = std::strlen(key);
  for (const char* p = e; *p;) {
    const char* end = std::strchr(p, ',');
    const char* eq = std::strchr(p, '=');
    if (!eq || (end && eq > end)) break;
    if (static_cast<size_t>(eq - p) == kl && !std::strncmp(p, key, kl)) {
      u64 m = 0;
      const char* q = eq + 1;
      while (*q && *q != ',') {
        char* x; const long a = std::strtol(q, &x, 10); long b = a;
        if (x == q) break;
        if (*x == '-') b = std::strtol(x + 1, &x, 10);
        for (long c = a; c <= b && c < static_cast<long>(MAX_CPUS); ++c) if (c >= 0) m |= u64{1} << c;
        q = x;
      }
      return m;
    }
    if (!end) break;
    p = end + 1;
  }
  return 0;
}

u64 role_mask(ThreadRole role, u32 index) {
  char key[16];
  switch (role) {
    case ThreadRole::Emu:     std::snprintf(key, sizeof key, "emu"); break;
    case ThreadRole::EngineA: std::snprintf(key, sizeof key, "enginea"); break;
    case ThreadRole::Line:    std::snprintf(key, sizeof key, "line"); break;
    case ThreadRole::Band:    std::snprintf(key, sizeof key, "band%u", index); break;
    case ThreadRole::Aux:     std::snprintf(key, sizeof key, "aux"); break;
  }
  if (const u64 m = role_mask_env(key)) return m;
  if (role == ThreadRole::Band) if (const u64 m = role_mask_env("band")) return m;
  if (std::getenv("DS_PIN")) return 0;   // an explicit layout names what it pins
  return g_layout.load(std::memory_order_relaxed) ? role_mask_default(role, index) : 0;
}

bool set_tid_affinity(long tid, u64 m) {
  cpu_set_t set; CPU_ZERO(&set);
  for (u32 c = 0; c < MAX_CPUS; ++c) if (m & (u64{1} << c)) CPU_SET(c, &set);
  return sched_setaffinity(static_cast<pid_t>(tid), sizeof set, &set) == 0;
}
#endif
} // namespace

void set_thread_layout(bool on) {
#if defined(__linux__)
  startup_affinity();   // latch the unpinned set before anything is pinned
  host_cores();
#endif
  g_layout.store(on, std::memory_order_relaxed);
}
bool thread_layout_on() { return g_layout.load(std::memory_order_relaxed) || std::getenv("DS_PIN"); }

#if defined(__linux__)
namespace {
std::atomic<int> g_worker_policy{-1}, g_worker_prio{0};   // -1: not latched
}
#endif

void latch_worker_sched() {
#if defined(__linux__)
  sched_param sp{};
  const int pol = sched_getscheduler(0);
  if (pol < 0 || sched_getparam(0, &sp) != 0) return;
  g_worker_prio.store(sp.sched_priority, std::memory_order_relaxed);
  g_worker_policy.store(pol, std::memory_order_relaxed);
#endif
}

void place_current_thread(ThreadRole role, u32 index) {
#if defined(__linux__)
  const long tid = current_tid();
  { std::lock_guard<std::mutex> lk(g_placed_m); g_placed.push_back(tid); }
  if (const u64 m = role_mask(role, index)) set_tid_affinity(tid, m);
  const int pol = g_worker_policy.load(std::memory_order_relaxed);
  if (pol == SCHED_RR || pol == SCHED_FIFO) {
    static const bool emu_other = std::getenv("DS_EMU_OTHER") != nullptr;
    sched_param sp{};
    int want = pol;
    sp.sched_priority = g_worker_prio.load(std::memory_order_relaxed);
    if (role == ThreadRole::Emu && emu_other) { want = SCHED_OTHER; sp.sched_priority = 0; }
    // Through pthread, not sched_setscheduler: glibc caches what
    // pthread_setschedparam last set, and pthread_getschedparam (the pacer's
    // spin saves and restores the policy with it) answers from that cache.
    if (role != ThreadRole::Aux)
      if (const int e = pthread_setschedparam(pthread_self(), want, &sp))
        std::fprintf(stderr, "thread layout: scheduling policy %d/%d for role %d not set: %s\n", want, sp.sched_priority, static_cast<int>(role), std::strerror(e));
  }
#else
  (void)role; (void)index;
#endif
}

void place_foreign_threads() {
#if defined(__linux__)
  const u64 m = role_mask(ThreadRole::Aux, 0);
  if (!m) return;
  DIR* d = opendir("/proc/self/task");
  if (!d) return;
  std::vector<long> placed;
  { std::lock_guard<std::mutex> lk(g_placed_m); placed = g_placed; }
  while (dirent* e = readdir(d)) {
    if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
    const long tid = std::atol(e->d_name);
    if (std::find(placed.begin(), placed.end(), tid) != placed.end()) continue;
    set_tid_affinity(tid, m);
  }
  closedir(d);
#endif
}

} // namespace ds
