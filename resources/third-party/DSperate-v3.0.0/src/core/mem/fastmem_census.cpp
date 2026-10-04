// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/mem/fastmem_census.h"
#include "core/cpu/cpu.h"
#include "core/mem/page_table.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ds::mem::fmc {

bool g_on = false;

namespace {

// Why an access would leave the view.
enum Why : u32 { W_IO, W_VRAM, W_UNMAPPED, W_CODE_STORE, W_RO_STORE, W_BUDDY, WHY_COUNT };
const char* const kWhy[WHY_COUNT] = {"I/O or MMIO (no base)", "VRAM region (kept out of views)", "unmapped / read-trapped", "store to a code page",
                                     "store to a read-only / trapped page", "store to data sharing a 4 KB host page with code"};

struct Site {
  u64 direct = 0;        // served by the view
  u64 walked = 0;        // lost to the view after the rewrite
  u64 slow = 0;           // would leave the view
  bool rewritten = false;
  bool mixed = false;    // served directly before its first fault
  u8 first_why = 0;
  bool first_store = false;
  u32 first_addr = 0;
  u64 slow_after = 0;    // faults after the rewrite
};

struct State {
  bool counting = false;
  std::unordered_map<u64, Site> sites;
  // Approximated code tags for interpreter runs: a host page is code once
  // fetched from, until a store lands on it. 2 KB guest granularity, 4 KB
  // for host-page protection.
  std::unordered_set<uintptr_t> code2k;
  std::unordered_map<uintptr_t, u32> code4k;   // host 4 KB page -> tagged 2 KB halves in it
  u64 direct[2] = {}, walked[2] = {}, faults_first[2] = {}, slow_why[2][WHY_COUNT] = {};
  u64 rewrites_counted[2] = {}, rewrites_mixed[2] = {};
  u64 region_direct[2][64] = {};      // direct accesses by 64 MB region
  u64 view_map_changes = 0, view_prot_changes = 0, vram_changes = 0, code_tags_on = 0, code_tags_off = 0;
};
State* g_st = nullptr;

bool in_vram(u32 addr) { return (addr >> 24) == 0x06; }

} // namespace

void init() {
  g_on = prof::census_env("DS_FASTMEM_CENSUS") && std::atoi(std::getenv("DS_FASTMEM_CENSUS")) != 0;
  if (g_on && !g_st) g_st = new State;
}

void set_counting(bool c) { if (g_st) g_st->counting = c; }

void access(CpuContext& cpu, u32 addr, bool store) {
  State& s = *g_st;
  const int c = cpu.which == Cpu::ARM9 ? 0 : 1;
  const Entry e = cpu.page_table.entry(addr);
  const uintptr_t base = e << 2;
  int why = -1;
  if (in_vram(addr)) why = W_VRAM;
  else if (!base) why = (e & TAG_SPECIAL) ? W_IO : W_UNMAPPED;
  else if (store && (e & TAG_SPECIAL)) why = W_RO_STORE;
  else if (store) {
    const uintptr_t h = base + addr;
    const bool tagged = (e & TAG_CODE) || s.code2k.count(h >> PAGE_SHIFT);
    if (tagged) {
      why = W_CODE_STORE;
      if (s.code2k.erase(h >> PAGE_SHIFT)) { auto it = s.code4k.find(h >> 12); if (it != s.code4k.end() && --it->second == 0) s.code4k.erase(it); }
    } else if (s.code4k.count(h >> 12)) {
      why = W_BUDDY;   // shares a 4 KB host page with a 2 KB code guest page
    }
  }
  const u64 key = (static_cast<u64>(c) << 32) | cpu.hot.regs[15];
  Site& site = s.sites[key];
  if (why < 0) {
    if (site.rewritten) { ++site.walked; if (s.counting) ++s.walked[c]; }
    else { ++site.direct; if (s.counting) { ++s.direct[c]; ++s.region_direct[c][addr >> 26]; } }
    return;
  }
  ++site.slow;
  if (site.rewritten) ++site.slow_after;
  if (s.counting) ++s.slow_why[c][why];
  if (!site.rewritten) {
    site.first_why = static_cast<u8>(why); site.first_addr = addr; site.first_store = store;
    site.rewritten = true;
    site.mixed = site.direct != 0;
    if (s.counting) { ++s.rewrites_counted[c]; if (site.mixed) ++s.rewrites_mixed[c]; }
  }
}

void fetch(CpuContext& cpu, u32 addr) {
  State& s = *g_st;
  const Entry e = cpu.page_table.entry(addr);
  const uintptr_t base = e << 2;
  if (!base) return;
  const uintptr_t h = base + addr;
  if (s.code2k.insert(h >> PAGE_SHIFT).second) ++s.code4k[h >> 12];
}

void entry_changed(u32 guest_page, uintptr_t old_e, uintptr_t new_e) {
  State& s = *g_st;
  if (!s.counting || old_e == new_e) return;
  if ((guest_page << PAGE_SHIFT >> 24) == 0x06) { ++s.vram_changes; return; }
  const uintptr_t ob = old_e << 2, nb = new_e << 2;
  if (ob != nb) ++s.view_map_changes;                                   // backing moved: mmap
  else if ((old_e ^ new_e) & (TAG_CODE | TAG_SPECIAL)) ++s.view_prot_changes;   // protection: mprotect
}

void code_tag(bool on) {
  if (!g_st->counting) return;
  if (on) ++g_st->code_tags_on; else ++g_st->code_tags_off;
}

void report(u64 frames) {
  if (!g_st || !frames) return;
  State& s = *g_st;
  const double f = static_cast<double>(frames);
  for (int c = 0; c < 2; ++c) {
    const char* n = c ? "arm7" : "arm9";
    u64 slow = 0;
    for (u64 v : s.slow_why[c]) slow += v;
    const u64 total = s.direct[c] + s.walked[c] + slow;
    if (!total) continue;
    std::fprintf(stderr, "[fastmem] %s accesses/frame %10.0f: direct %5.1f %%, walked after rewrite %5.1f %%, leave the view %5.1f %%\n", n,
                 total / f, 100.0 * s.direct[c] / total, 100.0 * s.walked[c] / total, 100.0 * slow / total);
    for (u32 w = 0; w < WHY_COUNT; ++w)
      if (s.slow_why[c][w]) std::fprintf(stderr, "[fastmem] %s   leave: %-50s %10.1f/frame\n", n, kWhy[w], s.slow_why[c][w] / f);
    std::fprintf(stderr, "[fastmem] %s site rewrites in window %llu (%llu served directly before their first fault)\n", n,
                 static_cast<unsigned long long>(s.rewrites_counted[c]), static_cast<unsigned long long>(s.rewrites_mixed[c]));
    std::vector<std::pair<u64, int>> reg;
    for (int r = 0; r < 64; ++r) if (s.region_direct[c][r]) reg.emplace_back(s.region_direct[c][r], r);
    std::sort(reg.rbegin(), reg.rend());
    std::fprintf(stderr, "[fastmem] %s direct accesses by 64 MB region:", n);
    for (auto& [v, r] : reg) std::fprintf(stderr, " %08x %.1f%%", static_cast<u32>(r) << 26, 100.0 * v / s.direct[c]);
    std::fputc('\n', stderr);
  }
  std::vector<std::pair<u64, u64>> lost;
  for (auto& [k, st] : s.sites) if (st.walked) lost.emplace_back(st.walked, k);
  std::sort(lost.rbegin(), lost.rend());
  u64 lost_total = 0;
  for (auto& l : lost) lost_total += l.first;
  std::fprintf(stderr, "[fastmem] sites with lost direct accesses: %zu, %.0f accesses/frame (whole run incl. warm-up); worst:", lost.size(), lost_total / f);
  std::fputc('\n', stderr);
  for (size_t i = 0; i < lost.size() && i < 12; ++i) {
    const Site& st = s.sites[lost[i].second];
    std::fprintf(stderr, "[fastmem]   arm%d@%08x lost %.0f/f, still leaving %.0f/f; first fault: %s %08x (%s)\n", (lost[i].second >> 32) ? 7 : 9,
                 static_cast<u32>(lost[i].second), lost[i].first / f, st.slow_after / f, st.first_store ? "store" : "load", st.first_addr, kWhy[st.first_why]);
  }
  std::fprintf(stderr, "[fastmem] view churn/frame: remaps %.1f, protection changes %.1f, code tags on %.1f off %.1f; VRAM entry changes (free) %.1f\n",
               s.view_map_changes / f, s.view_prot_changes / f, s.code_tags_on / f, s.code_tags_off / f, s.vram_changes / f);
}

} // namespace ds::mem::fmc
