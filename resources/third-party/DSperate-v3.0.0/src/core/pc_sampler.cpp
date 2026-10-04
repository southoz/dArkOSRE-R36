// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/pc_sampler.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>

#if defined(__linux__)
#include <csignal>
#include <cstring>
#include <dirent.h>
#include <link.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <ucontext.h>
#include <unistd.h>
#endif

namespace ds::pcsample {

#if defined(__linux__)
namespace {

constexpr size_t kMaxSamples = 1 << 20;
uintptr_t g_pc[kMaxSamples];
uint32_t g_tid[kMaxSamples];
std::atomic<size_t> g_n{0};
std::atomic<bool> g_active{false};

// utime+stime per thread at first activation and at write, for absolute CPU.
struct TaskTime { long tid; unsigned long long ticks; };
constexpr size_t kMaxTasks = 64;
TaskTime g_t0[kMaxTasks];
size_t g_nt0 = 0;
std::chrono::steady_clock::time_point g_window0;
bool g_window_started = false;

// utime + stime in clock ticks, or false.
bool task_ticks(const char* tid, unsigned long long* ticks) {
  char p[64], buf[512];
  std::snprintf(p, sizeof p, "/proc/self/task/%s/stat", tid);
  FILE* f = std::fopen(p, "r");
  if (!f) return false;
  const size_t n = std::fread(buf, 1, sizeof buf - 1, f);
  std::fclose(f);
  buf[n] = 0;
  const char* q = std::strrchr(buf, ')');   // the name can hold spaces; fields resume after its ')'
  if (!q) return false;
  unsigned long ut = 0, st = 0;
  // fields 3..15 after the name: state ppid pgrp session tty tpgid flags minflt cminflt majflt cmajflt utime stime
  if (std::sscanf(q + 1, " %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &ut, &st) != 2) return false;
  *ticks = static_cast<unsigned long long>(ut) + st;
  return true;
}

void snapshot_tasks() {
  g_nt0 = 0;
  if (DIR* d = opendir("/proc/self/task")) {
    while (dirent* e = readdir(d)) {
      unsigned long long t;
      if (e->d_name[0] != '.' && g_nt0 < kMaxTasks && task_ticks(e->d_name, &t)) g_t0[g_nt0++] = TaskTime{std::atol(e->d_name), t};
    }
    closedir(d);
  }
}

void on_sigprof(int, siginfo_t*, void* ctx) {
  if (!g_active.load(std::memory_order_relaxed)) return;
  const ucontext_t* uc = static_cast<const ucontext_t*>(ctx);
  uintptr_t pc = 0;
#if defined(__arm__)
  pc = uc->uc_mcontext.arm_pc;
#elif defined(__aarch64__)
  pc = uc->uc_mcontext.pc;
#elif defined(__x86_64__)
  pc = static_cast<uintptr_t>(uc->uc_mcontext.gregs[REG_RIP]);
#endif
  // Two threads can take a tick at once on a multi-core host: claim the slot.
  size_t n = g_n.load(std::memory_order_relaxed);
  do {
    if (n >= kMaxSamples) return;
  } while (!g_n.compare_exchange_weak(n, n + 1, std::memory_order_relaxed));
  g_pc[n] = pc;
  g_tid[n] = static_cast<uint32_t>(syscall(SYS_gettid));
}

int each_object(struct dl_phdr_info* info, size_t, void* out) {
  FILE* f = static_cast<FILE*>(out);
  char exe[512] = "";
  const char* name = info->dlpi_name;
  if (!name || !*name) {   // the executable itself
    const ssize_t k = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (k > 0) exe[k] = 0;
    name = exe;
  }
  for (int i = 0; i < info->dlpi_phnum; ++i) {
    const auto& ph = info->dlpi_phdr[i];
    if (ph.p_type != PT_LOAD || !(ph.p_flags & PF_X)) continue;
    const uintptr_t lo = info->dlpi_addr + ph.p_vaddr;
    std::fprintf(f, "obj %#lx %#lx %s\n", static_cast<unsigned long>(lo), static_cast<unsigned long>(lo + ph.p_memsz), name);
  }
  return 0;
}

int first_object(struct dl_phdr_info* info, size_t, void* out) {
  *static_cast<uintptr_t*>(out) = info->dlpi_addr;   // the first object is the executable
  return 1;
}

} // namespace

bool start() {
  struct sigaction sa {};
  sa.sa_sigaction = on_sigprof;
  sa.sa_flags = SA_SIGINFO | SA_RESTART;
  sigemptyset(&sa.sa_mask);
  if (sigaction(SIGPROF, &sa, nullptr) != 0) return false;
  const itimerval tv{{0, 1000}, {0, 1000}};   // asks for 1 kHz; the kernel's tick decides
  return setitimer(ITIMER_PROF, &tv, nullptr) == 0;
}

void set_active(bool on) {
  if (on && !g_window_started) {   // arm the window once, on first activation
    g_window_started = true;
    snapshot_tasks();
    g_window0 = std::chrono::steady_clock::now();
  }
  g_active.store(on, std::memory_order_relaxed);
}

bool write(const char* path) {
  g_active.store(false, std::memory_order_relaxed);
  const itimerval off{};
  setitimer(ITIMER_PROF, &off, nullptr);
  FILE* f = std::fopen(path, "w");
  if (!f) return false;
  uintptr_t base = 0;
  dl_iterate_phdr(first_object, &base);
  std::fprintf(f, "base %#lx\npid %d\n", static_cast<unsigned long>(base), static_cast<int>(getpid()));
  if (g_window_started)
    std::fprintf(f, "window %.3f %ld\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - g_window0).count(), sysconf(_SC_CLK_TCK));
  dl_iterate_phdr(each_object, f);
  if (DIR* d = opendir("/proc/self/task")) {
    while (dirent* e = readdir(d)) {
      if (e->d_name[0] == '.') continue;
      char p[64], comm[32] = "?";
      std::snprintf(p, sizeof p, "/proc/self/task/%s/comm", e->d_name);
      if (FILE* c = std::fopen(p, "r")) {
        if (std::fgets(comm, sizeof comm, c)) comm[std::strcspn(comm, "\n")] = 0;
        std::fclose(c);
      }
      unsigned long long t = 0, t0 = 0;
      const bool have = task_ticks(e->d_name, &t);
      for (size_t k = 0; k < g_nt0; ++k) if (g_t0[k].tid == std::atol(e->d_name)) t0 = g_t0[k].ticks;
      std::fprintf(f, "thread %s %s %lld\n", e->d_name, comm, have ? static_cast<long long>(t - t0) : -1LL);
    }
    closedir(d);
  }
  const size_t n = g_n.load();
  for (size_t i = 0; i < n; ++i) std::fprintf(f, "%#lx %u\n", static_cast<unsigned long>(g_pc[i]), g_tid[i]);
  std::fclose(f);
  std::fprintf(stderr, "pc profile: %zu samples to %s\n", n, path);
  return true;
}

#else
bool start() { return false; }
void set_active(bool) {}
bool write(const char*) { return false; }
#endif

} // namespace ds::pcsample
