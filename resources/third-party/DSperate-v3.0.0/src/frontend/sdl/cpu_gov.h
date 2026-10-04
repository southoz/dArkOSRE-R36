// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once

#include <cstdio>
#include <string>

namespace ds::sdl {

// Detects CPU governors that poll busy%: a duty-cycled emulator load (work
// then sleep-to-deadline) can read as idle to a sampling window and get its
// clock stepped down, causing an audible frame stall. `performance` and
// `schedutil` don't have this problem.
inline std::string read_policy_attr(unsigned policy, const char* attr) {
  char path[128];
  std::snprintf(path, sizeof path, "/sys/devices/system/cpu/cpufreq/policy%u/%s", policy, attr);
  std::FILE* f = std::fopen(path, "r");
  if (!f) return {};
  char v[64] = {};
  const bool ok = std::fgets(v, sizeof v, f) != nullptr;
  std::fclose(f);
  if (!ok) return {};
  std::string s(v);
  while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
  return s;
}

// Governor of one cpufreq policy, or "" if unreadable.
inline std::string cpu_governor(unsigned policy = 0) { return read_policy_attr(policy, "scaling_governor"); }

// True for governors that decide clock by sampling recent busy%. `powersave`
// counts only under non-pstate drivers; under intel_pstate/amd-pstate it
// means an energy preference, not a polled floor.
inline bool governor_is_polling(const std::string& g, const std::string& driver = {}) {
  if (g == "ondemand" || g == "conservative" || g == "interactive") return true;
  return g == "powersave" && driver.find("pstate") == std::string::npos;
}

// True if any cpufreq policy on the machine polls; one is enough to stall a
// frame since emulator threads spread across cores. `which` gets its name.
inline bool host_governor_polls(std::string* which = nullptr) {
  for (unsigned p = 0; p < 16; ++p) {
    const std::string g = cpu_governor(p);
    if (g.empty()) continue;
    if (governor_is_polling(g, read_policy_attr(p, "scaling_driver"))) { if (which) *which = g; return true; }
  }
  return false;
}

} // namespace ds::sdl
