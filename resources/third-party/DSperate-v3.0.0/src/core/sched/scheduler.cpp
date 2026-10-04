// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/sched/scheduler.h"
#include "core/state/state.h"
#include "core/nds.h"
#include "core/profile.h"
#include "core/cpu/interp/interp.h"
#include "core/cpu/idle_loop.h"
#if DSPERATE_JIT
#include "core/cpu/jit/jit.h"
#endif

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <map>
#include <vector>
#include <cstdlib>
#include <limits>
#include <algorithm>

namespace ds {

const char* event_name(u32 id) {
  static const char* const names[static_cast<u32>(EventId::Count)] = {
    "hblank", "scanline", "timer0", "timer1", "timer2", "timer3", "timer7_0", "timer7_1", "timer7_2", "timer7_3",
    "dma", "spu", "spi", "rtc", "cart", "gx3d", "display fifo", "div", "sqrt", "lcd irq", "wifi",
  };
  return id < static_cast<u32>(EventId::Count) && names[id] ? names[id] : "?";
}

Scheduler::~Scheduler() {
  if (!prof::enabled) return;
  u64 total = 0;
  for (u32 i = 0; i < EVENT_COUNT; ++i) total += ev_n_[i];
  if (!total) return;
  std::fprintf(stderr, "[events] handler time by event (the EVENTS stage split):\n");
  for (u32 i = 0; i < EVENT_COUNT; ++i)
    if (ev_n_[i]) std::fprintf(stderr, "[events]   %-14s %10llu fired  %9.3f ms total  %6.0f ns each\n", event_name(i),
                                static_cast<unsigned long long>(ev_n_[i]), static_cast<double>(ev_ns_[i]) / 1e6, static_cast<double>(ev_ns_[i]) / static_cast<double>(ev_n_[i]));
}

Scheduler::Scheduler(NDS& nds) : nds_(nds), now_(0) {
  debug_slices_ = std::getenv("DS_DEBUG_SLICES") != nullptr;
  idle_survey_ = std::getenv("DS_IDLE_SURVEY") != nullptr;
  if (const char* e = std::getenv("DS_IDLE_SKIP")) set_idle_skip(e);
  if (const char* e = std::getenv("DS_IDLE_CUT")) idle_cut_ = e[0] != '0';
  reset();
}

// DSi: ARM9 clock can switch 134/67 MHz mid-slice. Rescales ARM9Timestamp at
// the write (sub-system-cycle part dropped) and re-expresses the slice in the new core cycles.
void Scheduler::set_clock9_shift(u32 timing_shift) {
  const u32 ns = timing_shift - 1;
  CpuContext& a9 = nds_.cpu(Cpu::ARM9);
  if (dsi_ && running_ == &a9 && ns != shift9_) {
    const s64 consumed = static_cast<s64>(running_start_budget_) - a9.hot.cycle_budget - a9.preempt_residual;
    const s64 sys = (consumed + arm9_carry_) >> (shift9_ + 1);
    const s64 slice = (static_cast<s64>(budget9_) + arm9_carry_) >> shift9_;   // ticks
    budget9_ = static_cast<s32>(slice << ns);
    running_start_budget_ = budget9_;
    a9.hot.cycle_budget = static_cast<s32>(budget9_ - (sys << (ns + 1)) - a9.preempt_residual);
    running_rshift_ = ns + 1;
    running_carry_ = 0;
  }
  shift9_ = ns;
  arm9_carry_ = 0;
}

void Scheduler::set_idle_skip(const char* mode) {
  idle_skip_ = (mode[0] == '0') ? 0 : (std::strcmp(mode, "all") == 0 || mode[0] == '2') ? 2 : 1;
}

void Scheduler::reset() {
  now_ = 0;
  arm7_debt_ = 0; arm9_carry_ = 0;
  armed_ = 0;
  at_.fill(0); fn_.fill(nullptr); param_.fill(0);
  next_ = std::numeric_limits<u64>::max();
  next_id_ = EVENT_COUNT;
}

// `next_` caches the earliest armed deadline so the slice loop scans the
// table only when an event is actually due (or after a cancel).
void Scheduler::schedule(EventId id, u64 at, EventFn fn, u32 param) {
  const u32 i = static_cast<u32>(id);
  const bool was_next = (armed_ & (1u << i)) && next_id_ == i;   // only a live next event can be pushed later and leave next_ stale
  at_[i] = at; fn_[i] = fn; param_[i] = param;
  armed_ |= 1u << i;
  if (at < next_) { next_ = at; next_id_ = i; }
  else if (was_next && at > next_) rescan();
  if (cut_on_schedule_) cut_arm9_at(at);
}

void Scheduler::cut_arm9_at(u64 at) {
  CpuContext& a9 = nds_.cpu(Cpu::ARM9);
  if (running_ != &a9 || at >= slice_end_ || at <= now_) return;
  const s32 nb = budget9_for(static_cast<s64>(at - now_));
  if (nb >= budget9_) return;
  // An idle-cut probe in progress is abandoned: the held budget back first, so
  // what the ARM9 has used is counted from the whole slice.
  if (cut_held_[0]) { a9.hot.cycle_budget += cut_held_[0]; running_start_budget_ += cut_held_[0]; cut_held_[0] = 0; cut_idle_[0] = false; }
  const s32 consumed = budget9_ - a9.hot.cycle_budget;
  budget9_ = nb; running_start_budget_ = nb; slice_end_ = at;
  a9.hot.cycle_budget = nb - consumed;   // <= 0: the ARM9 stops after this instruction, as melonDS's loop does
}

void Scheduler::cancel(EventId id) {
  const u32 i = static_cast<u32>(id);
  if (!(armed_ & (1u << i))) return;
  armed_ &= ~(1u << i);
  if (next_id_ == i) rescan();
}

// Earliest armed deadline and which event owns it.
void Scheduler::rescan() {
  u64 best = std::numeric_limits<u64>::max();
  u32 best_id = EVENT_COUNT;
  for (u32 m = armed_; m; m &= m - 1) {
    const u32 i = static_cast<u32>(__builtin_ctz(m));
    if (at_[i] < best) { best = at_[i]; best_id = i; }
  }
  next_ = best;
  next_id_ = best_id;
}

// ---- idle cut ----
// A CPU that ends slices in states it has already been in is re-reading
// memory and I/O without progress: a poll. Instead of its whole next slice it
// gets a probe of a few hundred cycles; if that ends in a state already seen
// too, the rest of the slice counts as run without being emulated. Anything
// the poll waits on -- a value that lands in a register -- makes the probe end
// somewhere new and the CPU takes its whole slice, so it notices within one
// slice. A trade against exact timing: the skipped iterations cost the host
// nothing, and a store-only loop with frozen registers would be cut too.
// A running DMA doesn't stop it: the skipped cycles go to the DMA instead
// (cut_resolve), as a skipped CPU's do in advance_dma_only.
bool Scheduler::cut_ok(const CpuContext& c) const {
  return idle_cut_ && !dsi_ && !c.halted && !(c.hot.irq_pending && !(c.hot.cpsr & 0x80));
}
namespace {
u64 reg_hash(const CpuContext& c) {
  u64 h = 1469598103934665603ull;
  for (u32 r = 0; r < 15; ++r) h = (h ^ c.hot.regs[r]) * 1099511628211ull;
  return (h ^ c.hot.cpsr) * 1099511628211ull;
}
} // namespace
// The state is one the CPU ended a recent slice in: same registers, and the
// same PC or one nearby (a poll of several addresses stops anywhere in its loop).
bool Scheduler::cut_seen(int i, const CpuContext& c) const {
  const u32 pc = c.hot.regs[15];
  const u64 h = reg_hash(c);
  for (u32 k = 0; k < 8; ++k) if (cut_h_[i][k] == h && cut_pc_[i][k] - pc + 256u < 512u) return true;
  return false;
}
// At a slice end: is the CPU idle (for the next slice's probe), then remember the state.
void Scheduler::cut_note(int i, const CpuContext& c) {
  cut_idle_[i] = cut_ok(c) && cut_seen(i, c);
  cut_h_[i][cut_pos_[i] & 7] = reg_hash(c);
  cut_pc_[i][cut_pos_[i]++ & 7] = c.hot.regs[15];
}
// Slice start: an idle CPU runs a probe, the rest of its budget held back.
void Scheduler::cut_arm(int i, CpuContext& c, s32 probe) {
  cut_held_[i] = 0;
  if (!cut_idle_[i] || !cut_ok(c) || c.hot.cycle_budget <= 2 * probe) return;
  cut_held_[i] = c.hot.cycle_budget - probe;
  c.hot.cycle_budget = probe;
  // The clock inside the slice (now()) counts from the budget the CPU was
  // handed: the probe's, so it runs from the slice start, not its end.
  if (running_ == &c) running_start_budget_ -= cut_held_[i];
  prof::add(prof::C_CUT_PROBES, 1);
}
// After the probe: still idle, and the held budget counts as spent (true);
// or the CPU gets it back to run the rest of its slice (false).
bool Scheduler::cut_resolve(int i, CpuContext& c) {
  const s32 held = cut_held_[i];
  cut_held_[i] = 0;
  if (c.halted || (cut_ok(c) && cut_seen(i, c))) {
    if (!c.halted) {
      prof::add(prof::C_CUT_HITS, 1);
      prof::add(i == 0 ? prof::C_CYC_CUT_A9 : prof::C_CYC_CUT_A7, static_cast<u64>(held));
      // A DMA on this CPU still gets the whole slice; what it leaves is idle.
      if (nds_.dma.any_running(c.which)) {
        c.hot.cycle_budget += held;
        if (running_ == &c) running_start_budget_ += held;
        advance_dma_only(c);
        if (c.hot.cycle_budget > 0) c.hot.cycle_budget = 0;
      }
    }
    return true;   // a CPU that halted in its probe sleeps out the slice as usual
  }
  // Where the probe stopped goes into the ring as well: a loop that spans a
  // call (a game's wait around a BIOS routine) stops in the other half than
  // whole slices do, and is recognised there from the second probe on.
  if (!c.halted) {
    cut_h_[i][cut_pos_[i] & 7] = reg_hash(c);
    cut_pc_[i][cut_pos_[i]++ & 7] = c.hot.regs[15];
  }
  cut_idle_[i] = false;
  c.hot.cycle_budget += held;
  if (running_ == &c) running_start_budget_ += held;
  return false;
}

// A CPU counts as idle when halted, or awake but provably going nowhere.
// Requires the sibling CPU idle too, no DMA running, and GX quiet; the slice
// still ends at the next scheduled event.
bool Scheduler::machine_idle(bool& skip9, bool& skip7) const {
  skip9 = skip7 = false;
  if (!idle_skip_) return both_idle();
  // Swap-wait mode: skip the PC ring/body walk/DMA probes unless a swap is pending.
  const bool gx_only = idle_skip_ == 1;
  if (gx_only && !nds_.gpu3d.swap_pending()) return both_idle();
  // A pending GX command doesn't veto: logged commands cost no time. DMA vetoes
  // on DSi only (a9_dma_iter_ needs a CPU to have run); elsewhere advance_dma_only
  // keeps a skipped channel moving.
  bool vetoed = false;
  if (dsi_ && (nds_.dma.any_running(Cpu::ARM9) || nds_.dma.any_running(Cpu::ARM7))) { prof::add(prof::C_IDLE_NO_DMA, 1); vetoed = true; }
  else if (prof::enabled && !nds_.gpu3d.idle()) prof::add(prof::C_IDLE_NO_GX, 1);   // counted, not vetoed
  if (vetoed) {
    // DS_IDLE_SURVEY: run the analysis anyway to count what the veto turns
    // away, restoring the PC ring after so the survey doesn't affect real decisions.
    if (!idle_survey_) return false;
    prof::add(prof::C_IDLE_SURVEY_SEEN, 1);
    u32 ring[2][8]; std::memcpy(ring, idle_pc_ring_, sizeof ring);
    const u32 p0 = idle_pc_pos_[0], p1 = idle_pc_pos_[1];
    bool s9 = false, s7 = false;
    if (idle_analyse(s9, s7, true)) prof::add(prof::C_IDLE_SURVEY_WOULD_SKIP, 1);
    std::memcpy(idle_pc_ring_, ring, sizeof ring);
    idle_pc_pos_[0] = p0; idle_pc_pos_[1] = p1;
    return false;
  }
  return idle_analyse(skip9, skip7, false);
}

// PC pre-filter plus the loop analysis. `survey` sends rejections to shadow
// counters so a survey pass cannot pollute the real ones.
bool Scheduler::idle_analyse(bool& skip9, bool& skip7, bool survey) const {
  const bool gx_only = idle_skip_ == 1;
  CpuContext& a9 = const_cast<CpuContext&>(nds_.cpu(Cpu::ARM9));
  CpuContext& a7 = const_cast<CpuContext&>(nds_.cpu(Cpu::ARM7));
  CpuContext* cpus[2] = {&a9, &a7};
  // Pre-filter: a spinning CPU comes back to the same PC it left on. Both
  // slots refresh before any early return, so one CPU's rejection can't leave the other stale.
  bool repeated[2];
  for (int i = 0; i < 2; ++i) {
    const u32 pc = cpus[i]->hot.regs[15];
    repeated[i] = false;
    for (u32 k = 0; k < 8; ++k) if (idle_pc_ring_[i][k] == pc) { repeated[i] = true; break; }
    idle_pc_ring_[i][idle_pc_pos_[i]++ & 7] = pc;
  }
  bool skip[2] = {false, false};
  for (int i = 0; i < 2; ++i) {
    CpuContext& c = *cpus[i];
    // An unmasked pending IRQ means the CPU is about to leave, halted or not.
    if (c.hot.irq_pending && !(c.hot.cpsr & 0x80)) { prof::add(survey ? prof::C_IDLE_SURVEY_NO_IRQ : prof::C_IDLE_NO_IRQ, 1); return false; }
    if (c.halted) continue;
    if (!repeated[i]) { prof::add(survey ? prof::C_IDLE_SURVEY_NO_FILTER : prof::C_IDLE_NO_FILTER, 1); return false; }
    // Swap-wait mode: the ARM9 loop must read GXSTAT and no other device; the
    // ARM7 (which cannot see GXSTAT) may only spin on RAM.
    const cpu::IdlePorts ports = !gx_only ? cpu::IdlePorts::All : i == 0 ? cpu::IdlePorts::GxstatOnly : cpu::IdlePorts::RamOnly;
    if (!cpu::in_idle_loop(c, ports)) { prof::add(survey ? prof::C_IDLE_SURVEY_NO_LOOP : (i ? prof::C_IDLE_NO_LOOP7 : prof::C_IDLE_NO_LOOP9), 1); return false; }
    skip[i] = true;
  }
  if (!survey) prof::add(prof::C_IDLE_OK, 1);
  skip9 = skip[0];
  skip7 = skip[1];
  return true;
}

bool Scheduler::arm7_spi_poll(u64& wake) const {
  if (!idle_skip_) return false;
  const CpuContext& a7 = nds_.cpu(Cpu::ARM7);
  if (a7.halted || (a7.hot.irq_pending && !(a7.hot.cpsr & 0x80))) return false;
  if (!nds_.io.spi_busy()) return false;
  if (nds_.dma.any_running(Cpu::ARM7)) return false;
  const u32 pc = a7.hot.regs[15];
  bool repeated = false;
  for (u32 k = 0; k < 8; ++k) if (spi_pc_ring_[k] == pc) { repeated = true; break; }
  spi_pc_ring_[spi_pc_pos_++ & 7] = pc;
  if (!repeated) return false;
  if (!cpu::in_idle_loop(const_cast<CpuContext&>(a7), cpu::IdlePorts::SpicntOnly)) return false;
  wake = nds_.io.spi_ready_at;
  return true;
}

bool Scheduler::both_idle() const {
  const CpuContext& a9 = nds_.cpu(Cpu::ARM9);
  const CpuContext& a7 = nds_.cpu(Cpu::ARM7);
  auto asleep = [](const CpuContext& c) { return c.halted && !(c.hot.irq_pending && !(c.hot.cpsr & 0x80)); };
  return asleep(a9) && asleep(a7) && !nds_.dma.any_running(Cpu::ARM9) && !nds_.dma.any_running(Cpu::ARM7) && nds_.gpu3d.idle();
}

// DS_PROFILE=1: what the slices are made of.
void Scheduler::count_slice(bool skipped, s64 slice) const {
  const CpuContext& a9 = nds_.cpu(Cpu::ARM9);
  const CpuContext& a7 = nds_.cpu(Cpu::ARM7);
  if (!prof::heavy) {   // light profiling: only what the frame series annotates
    prof::Accum* a = prof::detail::get();
    a->count[prof::C_SLICES] += 1; a->count[prof::C_CYC_TOTAL] += static_cast<u64>(slice);
    return;
  }
  prof::add(prof::C_SLICES, 1);
  if (a9.halted) prof::add(prof::C_SLICES_A9_HALTED, 1);
  if (a7.halted) prof::add(prof::C_SLICES_A7_HALTED, 1);
  if (a9.halted && a7.halted) prof::add(prof::C_SLICES_BOTH_HALTED, 1);
  if (nds_.dma.any_running(Cpu::ARM9) || nds_.dma.any_running(Cpu::ARM7)) prof::add(prof::C_SLICES_DMA, 1);
  if (skipped) prof::add(prof::C_SLICES_SKIPPED, 1);

  // Cycle-weighted halt state: plain slice counts hide it since awake-CPU
  // slices are the ones SLICE_QUANTUM keeps short.
  const u64 cyc = static_cast<u64>(slice);
  prof::add(prof::C_CYC_TOTAL, cyc);
  if (a9.halted && a7.halted) prof::add(prof::C_CYC_BOTH_HALTED, cyc);
  else if (a9.halted) prof::add(prof::C_CYC_A9_ONLY_HALTED, cyc);
  else if (a7.halted) prof::add(prof::C_CYC_A7_ONLY_HALTED, cyc);
  else prof::add(prof::C_CYC_NEITHER_HALTED, cyc);

  // Spin proxy for the awake CPUs.
  bool spin[2] = {false, false}, exact[2] = {false, false}, drift[2] = {false, false};
  const CpuContext* cpus[2] = {&a9, &a7};
  for (int i = 0; i < 2; ++i) {
    if (cpus[i]->halted) continue;
    const u32 pc = cpus[i]->hot.regs[15];
    u64 h = 1469598103934665603ull;
    for (u32 r = 0; r < 15; ++r) h = (h ^ cpus[i]->hot.regs[r]) * 1099511628211ull;
    h = (h ^ cpus[i]->hot.cpsr) * 1099511628211ull;
    for (u32 k = 0; k < 8; ++k) {
      if (spin_ring_[i][k] == pc) { spin[i] = true; if (spin_hash_[i][k] == h) exact[i] = true; }
      else if (spin_hash_[i][k] == h && spin_ring_[i][k] - pc + 256u < 512u) drift[i] = true;
    }
    if (exact[i]) drift[i] = false;
    spin_hash_[i][spin_pos_[i] & 7] = h;
    spin_ring_[i][spin_pos_[i]++ & 7] = pc;
  }
  spin_now_[0] = spin[0]; spin_now_[1] = spin[1];
  idle_now_[0] = exact[0]; idle_now_[1] = exact[1];
  drift_now_[0] = drift[0]; drift_now_[1] = drift[1];
  if (drift[0]) prof::add(prof::C_CYC_A9_IDLE_DRIFT, cyc);
  if (drift[1]) prof::add(prof::C_CYC_A7_IDLE_DRIFT, cyc);
  if (exact[0]) prof::add(prof::C_CYC_A9_IDLE_EXACT, cyc);
  if (exact[1]) prof::add(prof::C_CYC_A7_IDLE_EXACT, cyc);
  // DS_DUMP_CODE=<hex addr>: one-shot dump of 16 guest words, for inspecting
  // a loop body the analyser rejected.
  static const char* dump_env = std::getenv("DS_DUMP_CODE");
  if (dump_env) {
    static bool done = false;
    if (!done) {
      const u32 base = static_cast<u32>(std::strtoul(dump_env, nullptr, 16));
      CpuContext& c = const_cast<CpuContext&>(nds_.cpu(Cpu::ARM9));
      const u32 at = c.hot.regs[15] - 8;   // wait until the CPU is actually in there
      if (at >= base && at < base + 64 && c.page_table.read_ptr(base)) {
        done = true;
        for (u32 k = 0; k < 16; ++k) {
          u32 w = 0;
          if (const u8* hp = c.page_table.read_ptr(base + k * 4)) std::memcpy(&w, hp, 4);
          std::fprintf(stderr, "[code] %08x  %08x\n", base + k * 4, w);
        }
      }
    }
  }
  // DS_SPIN_PCS=1: where the spin slices actually sit, to tell an idle poll
  // loop (a few addresses) from a merely hot inner loop (many).
  static const bool spin_pcs = std::getenv("DS_SPIN_PCS") != nullptr;
  if (spin_pcs) {
    static std::map<u32, u64> hist[2], ihist[2];
    static std::map<u32, u32> opc[2];
    static std::map<u32, const char*> why[2];
    spin_opcodes_ = &opc[0];
    spin_reject_ = &why[0];
    static bool reg = false;
    if (!reg) {
      reg = true;
      std::atexit([] {
        for (int i = 0; i < 2; ++i) {
          std::vector<std::pair<u64, u32>> iv;
          u64 itot = 0;
          for (auto& kv : ihist[i]) { iv.push_back({kv.second, kv.first}); itot += kv.second; }
          std::sort(iv.rbegin(), iv.rend());
          if (itot) std::fprintf(stderr, "[idle] %s: exact-idle, %zu distinct pcs, %llu cycles\n", i ? "arm7" : "arm9", iv.size(), (unsigned long long)itot);
          for (size_t k = 0; k < iv.size() && k < 8; ++k)
            std::fprintf(stderr, "[idle]   %08x  %12llu %5.1f%%\n", iv[k].second, (unsigned long long)iv[k].first, 100.0 * static_cast<double>(iv[k].first) / static_cast<double>(itot));
          std::vector<std::pair<u64, u32>> v;
          u64 tot = 0;
          for (auto& kv : hist[i]) { v.push_back({kv.second, kv.first}); tot += kv.second; }
          if (!tot) continue;
          std::sort(v.rbegin(), v.rend());
          std::fprintf(stderr, "[spin] %s: %zu distinct pcs, %llu cycles\n", i ? "arm7" : "arm9",
                       v.size(), (unsigned long long)tot);
          for (size_t k = 0; k < v.size() && k < 12; ++k)
            std::fprintf(stderr, "[spin]   %08x  op %08x  %-12s %12llu %5.1f%%\n", v[k].second,
                         spin_opcodes_ ? spin_opcodes_[i][v[k].second] : 0u,
                         spin_reject_ ? spin_reject_[i][v[k].second] : "?",
                         (unsigned long long)v[k].first, 100.0 * static_cast<double>(v[k].first) / static_cast<double>(tot));
        }
      });
    }
    for (int i = 0; i < 2; ++i) if (spin[i]) {
      const u32 pc = cpus[i]->hot.regs[15];
      hist[i][pc] += cyc;
      if (exact[i]) ihist[i][pc] += cyc;
      const bool th = cpus[i]->thumb();
      const u32 at = pc - (th ? 4 : 8);   // regs[15] runs ahead of the executing instruction
      u32 w = 0;
      if (const u8* hp = cpus[i]->page_table.read_ptr(at)) std::memcpy(&w, hp, th ? 2 : 4);
      opc[i][pc] = w;
      CpuContext& dc = const_cast<CpuContext&>(*cpus[i]);
      why[i][pc] = cpu::in_idle_loop(dc) ? "ok" : cpu::idle_reject_name(cpu::idle_loop_last_reject());
    }
  }
  if (spin[0]) prof::add(prof::C_CYC_A9_SPIN, cyc);
  if (spin[1]) prof::add(prof::C_CYC_A7_SPIN, cyc);
  const bool idle9 = a9.halted || spin[0];
  const bool idle7 = a7.halted || spin[1];
  if (idle9 && idle7) {
    prof::add(prof::C_CYC_BOTH_SPIN_OR_HALTED, cyc);
    if (!(a9.halted && a7.halted)) prof::add(prof::C_CYC_ONE_SPIN_ONE_HALTED, cyc);
  }
}

// Fires every armed event at or before now_, repeated until none left: a
// handler may reschedule an event already overshot (timer period shorter
// than a slice), which one pass would leave armed in the past.
void Scheduler::fire_due() {
  while (now_ >= next_) {
    next_ = std::numeric_limits<u64>::max();
    next_id_ = EVENT_COUNT;
    // Ascending id order; `armed_` re-read after every handler so an event
    // armed at a higher id fires this pass, a lower one waits for the next.
    for (u32 m = armed_; m; ) {
      const u32 i = static_cast<u32>(__builtin_ctz(m));
      const u32 bit = 1u << i;
      if (at_[i] <= now_) {
        armed_ &= ~bit;
        firing_at_ = at_[i];
        if (debug_slices_) std::fprintf(stderr, "[fire] t %llu event %u at %llu\n", (unsigned long long)now_, i, (unsigned long long)at_[i]);
        // The two scanline handlers and the SPU account for themselves.
        if (i == static_cast<u32>(EventId::HBlank) || i == static_cast<u32>(EventId::VBlank_Scanline) || i == static_cast<u32>(EventId::Spu)) fn_[i](nds_, param_[i]);
        else if (!prof::enabled) fn_[i](nds_, param_[i]);   // may schedule: next_ is kept current by schedule()
        else {
          // Per event id as well as in the EVENTS stage (destructor prints it).
          const u64 t0 = prof::now_ns();
          fn_[i](nds_, param_[i]);
          const u64 el = prof::now_ns() - t0;
          prof::add_timed(prof::EVENTS, el);
          ev_ns_[i] += el; ++ev_n_[i];
        }
        m = armed_ & ~((bit << 1) - 1);
      } else {
        if (at_[i] < next_) { next_ = at_[i]; next_id_ = i; }
        m &= ~bit;
      }
    }
  }
}

// DSi: the instruction that started a DMA drove the budget below the zero
// preempt() set; hand that cost back so the DMA starts at the instruction's
// start time, charged after the CPU's next instruction (CpuContext::defer_cost).
void Scheduler::defer_preempt_cost(CpuContext& cpu) {
  if (!dsi_ || cpu.yielded || cpu.hot.cycle_budget >= 0) return;
  const s32 over = -cpu.hot.cycle_budget;
  cpu.hot.cycle_budget = 0;
  cpu.defer_cost += over;
}

// A skipped CPU still lets its DMA advance (DMA is driven from inside
// run_cpu). DSi instead keeps the machine_idle DMA veto, since a9_dma_iter_
// has no meaning when no CPU ran.
void Scheduler::advance_dma_only(CpuContext& cpu) {
  const Cpu which = cpu.which;
  if (!nds_.dma.any_running(which)) return;
  DS_PROF(DMA);
  in_dma_ = true; dma_used_ = 0;
  cpu.hot.cycle_budget -= static_cast<s32>(nds_.dma.run(which, static_cast<u32>(cpu.hot.cycle_budget)));
  in_dma_ = false; dma_used_ = 0;
}

void Scheduler::run_cpu(CpuContext& cpu, RunFn run) {
  const Cpu which = cpu.which;
  for (;;) {
    if (nds_.dma.any_running(which)) {
      const s32 b0 = cpu.hot.cycle_budget;
      { DS_PROF(DMA); in_dma_ = true; dma_used_ = 0; cpu.hot.cycle_budget -= static_cast<s32>(nds_.dma.run(which, static_cast<u32>(cpu.hot.cycle_budget))); in_dma_ = false; dma_used_ = 0; }
      if (dsi_ && which == Cpu::ARM9 && cpu.hot.cycle_budget != b0) { a9_dma_iter_ = true; return; }   // see a9_dma_iter_ (a DMA that could not move is not an iteration)
      // DSi ARM7: re-enter until the DMA reaches its target (multi-channel
      // hand-off, e.g. AES NDMA ping-pong); else it falls behind for the whole transfer.
      if (dsi_ && cpu.hot.cycle_budget > 0 && cpu.hot.cycle_budget != b0 && nds_.dma.any_running(which)) continue;
      if (cpu.hot.cycle_budget <= 0 || nds_.dma.any_running(which)) return;
      // DMA ended mid-phase: an IRQ it raised is taken after this CPU's next instruction, not immediately.
      if (cpu.irq_offline) { cpu.irq_skip_once = !cpu.halted; cpu.irq_offline = false; }
    }
    {
      u64 t0 = 0;
      if (prof::enabled) t0 = prof::now_ns();
      run(cpu);
      if (prof::enabled) {
        const int ci = which == Cpu::ARM9 ? 0 : 1;
        const u64 el = prof::now_ns() - t0;
        prof::add_timed(ci == 0 ? prof::CPU9 : prof::CPU7, el);
        prof::add(spin_now_[ci] ? (ci == 0 ? prof::C_NS_A9_SPIN : prof::C_NS_A7_SPIN)
                                : (ci == 0 ? prof::C_NS_A9_WORK : prof::C_NS_A7_WORK), el);
        if (idle_now_[ci]) prof::add(ci == 0 ? prof::C_NS_A9_IDLE_EXACT : prof::C_NS_A7_IDLE_EXACT, el); else if (drift_now_[ci]) prof::add(ci == 0 ? prof::C_NS_A9_IDLE_DRIFT : prof::C_NS_A7_IDLE_DRIFT, el);
      }
    }
    if (!cpu.preempt_residual) return;
    defer_preempt_cost(cpu);
    cpu.hot.cycle_budget += cpu.preempt_residual;   // overshoot of the preempted instruction comes off the residual
    cpu.preempt_residual = 0;
    if (cpu.yielded) { cpu.yielded = false; return; }   // yield(): the rest of the slice goes to the other CPU
    if (dsi_ && which == Cpu::ARM9) { a9_dma_iter_ = true; return; }   // DSi: the DMA runs in the next iteration (see a9_dma_iter_)
    if (cpu.hot.cycle_budget <= 0 || cpu.halted) return;
  }
}

#if DSPERATE_JIT
// ---- native slice loop ----
//
// run_until/run_cpu/jit::run flattened into one straight-line sequence per
// slice, cut where translated code is entered, with two resume points at the
// top. Order of operations matches run_until below exactly.

namespace {
enum { SL_BEGIN, SL_A9, SL_A7 };
}

SliceNext Scheduler::slice_next() {
  CpuContext& a9 = nds_.cpu(Cpu::ARM9);
  CpuContext& a7 = nds_.cpu(Cpu::ARM7);
  CpuContext* cpu;
  RunFn run;
  if (sl_.phase == SL_A9) { cpu = &a9; run = nds_.run_arm9; goto resume; }
  if (sl_.phase == SL_A7) { cpu = &a7; run = nds_.run_arm7; goto resume; }

begin:
  {
    prof::Scope sched_scope(prof::SCHED);
    if ((sl_.until_frame && nds_.frame_ready) || now_ >= sl_.until) { sl_.phase = SL_BEGIN; return {nullptr, nullptr}; }
    u64 deadline = next_;
    if (deadline > sl_.until) deadline = sl_.until;
    s64 slice = static_cast<s64>(deadline - now_);
    if (slice <= 0) slice = 1;
    // With both CPUs asleep SLICE_QUANTUM only paces the clock: run to the deadline.
    const bool all_idle = machine_idle(sl_.skip9, sl_.skip7);
    const bool idle = slice > SLICE_QUANTUM && all_idle && !dsi_;   // DSi: melonDS steps 64 cycles even with both CPUs asleep (its timers are checked per step)
    if (slice >= SLICE_QUANTUM + slice_margin_ && !idle) slice = SLICE_QUANTUM;   // melonDS: minEvent < max + margin extends, equal does not
    u64 wake = 0;
    if (!all_idle && !sl_.skip7 && arm7_spi_poll(wake)) {
      sl_.skip7 = true;
      if (wake > now_ && static_cast<s64>(wake - now_) < slice) slice = static_cast<s64>(wake - now_);
      if (prof::enabled) { prof::add(prof::C_A7_SPI_SLEEP, 1); prof::add(prof::C_CYC_A7_SPI_SLEPT, static_cast<u64>(slice)); }
    }
    sl_.slice = slice;
    if (prof::enabled) count_slice(idle, slice);
    if (prof::enabled && (sl_.skip9 || sl_.skip7)) prof::add(prof::C_CYC_IDLE_SKIPPED, static_cast<u64>(slice));
    budget9_ = budget9_for(slice); slice_end_ = now_ + static_cast<u64>(slice);
    a9.hot.cycle_budget = budget9_;
    if (a9.boot_stall) take_stall(a9);
    if (a9.irq_offline) { a9.irq_skip_once = !a9.halted; a9.irq_offline = false; }
    running_ = &a9; running_start_budget_ = budget9_; running_shift_ = 0; running_rshift_ = dsi_ ? shift9_ + 1 : 0; running_base_ = now_; running_carry_ = static_cast<u64>(arm9_carry_);
    sl_.phase = SL_A9; cpu = &a9; run = nds_.run_arm9;
    if (sl_.skip9) { advance_dma_only(a9); goto a9_done; }
    cut_arm(0, a9, CUT_PROBE9);
  }
cpu_begin:   // run_cpu loop head
  {
    if (nds_.dma.any_running(cpu->which)) {
      const s32 b0 = cpu->hot.cycle_budget;
      { DS_PROF(DMA); in_dma_ = true; dma_used_ = 0; cpu->hot.cycle_budget -= static_cast<s32>(nds_.dma.run(cpu->which, static_cast<u32>(cpu->hot.cycle_budget))); in_dma_ = false; dma_used_ = 0; }
      if (dsi_ && cpu == &a9 && cpu->hot.cycle_budget != b0) { a9_dma_iter_ = true; goto cpu_done; }   // see a9_dma_iter_
      if (dsi_ && cpu->hot.cycle_budget > 0 && cpu->hot.cycle_budget != b0 && nds_.dma.any_running(cpu->which)) goto cpu_begin;   // see run_cpu
      if (cpu->hot.cycle_budget <= 0 || nds_.dma.any_running(cpu->which)) goto cpu_done;
      if (cpu->irq_offline) { cpu->irq_skip_once = !cpu->halted; cpu->irq_offline = false; }   // see run_cpu
    }
    if (prof::enabled) sl_.t0 = prof::now_ns();
    if (!cpu->jit) { run(*cpu); goto run_returned; }
    // jit::run up to the first entry (check_irq also retires irq_skip_once)
    if (cpu->hot.irq_pending || cpu->irq_skip_once) cpu->check_irq();
    if (cpu->halted) { cpu->hot.cycle_budget = -1; goto run_returned; }
    if (cpu->step_limit) { interp::run(*cpu); goto run_returned; }
    if (cpu->hot.cycle_budget > 0) return {cpu, jit::lookup(*cpu)};
    goto run_returned;
  }
resume:      // translated code left
  {
    if (cpu->halted) { cpu->budget_at_halt = cpu->hot.cycle_budget; cpu->hot.cycle_budget = -1; goto run_returned; }
    if (cpu->hot.irq_pending) cpu->check_irq();
    if (cpu->hot.cycle_budget > 0) return {cpu, jit::lookup(*cpu)};
  }
run_returned:
  {
    if (prof::enabled) {
      const int ci = cpu->which == Cpu::ARM9 ? 0 : 1;
      const u64 el = prof::now_ns() - sl_.t0;
      prof::add_timed(ci == 0 ? prof::CPU9 : prof::CPU7, el);
      prof::add(spin_now_[ci] ? (ci == 0 ? prof::C_NS_A9_SPIN : prof::C_NS_A7_SPIN)
                              : (ci == 0 ? prof::C_NS_A9_WORK : prof::C_NS_A7_WORK), el);
      if (idle_now_[ci]) prof::add(ci == 0 ? prof::C_NS_A9_IDLE_EXACT : prof::C_NS_A7_IDLE_EXACT, el); else if (drift_now_[ci]) prof::add(ci == 0 ? prof::C_NS_A9_IDLE_DRIFT : prof::C_NS_A7_IDLE_DRIFT, el);
    }
    if (cpu->preempt_residual) {
      defer_preempt_cost(*cpu);
      cpu->hot.cycle_budget += cpu->preempt_residual;
      cpu->preempt_residual = 0;
      if (cpu->yielded) cpu->yielded = false;   // yield(): the rest of the slice goes to the other CPU
      else if (dsi_ && cpu == &a9) a9_dma_iter_ = true;   // DSi: the DMA runs in the next iteration (see a9_dma_iter_)
      else if (cpu->hot.cycle_budget > 0 && !cpu->halted) goto cpu_begin;
    }
  }
cpu_done:
  {
    const int ci = cpu == &a9 ? 0 : 1;
    if (cut_held_[ci] && !cut_resolve(ci, *cpu) && cpu->hot.cycle_budget > 0 && !cpu->halted) goto cpu_begin;
  }
  if (cpu == &a7) goto a7_done;
a9_done:
  {
    const bool full9 = (a9.halted || sl_.skip9) && !a9_dma_iter_;
    const bool dma_iter = a9_dma_iter_; a9_dma_iter_ = false;
    s64 ran9 = full9 ? sl_.slice : ticks9(budget9_ - a9.hot.cycle_budget);
    if (full9) arm9_carry_ = 0;
    if (ran9 <= 0) ran9 = dma_iter ? 0 : 1;   // a DMA hand-off phase may be empty (melonDS's zero-length iteration); the DMA runs next
    sl_.ran9 = ran9;
    running_ = nullptr; running_rshift_ = 0;
    arm7_debt_ += ran9;
    sl_.budget7 = static_cast<s32>(arm7_debt_ / 2);
    if (sl_.budget7 <= 0) goto slice_end;
    a7.hot.cycle_budget = sl_.budget7;
    if (a7.boot_stall) take_stall(a7);
    if (a7.irq_offline) { a7.irq_skip_once = !a7.halted; a7.irq_offline = false; }
    running_ = &a7; running_start_budget_ = sl_.budget7; running_shift_ = 1; running_rshift_ = 0;
    running_base_ = dsi_ ? static_cast<u64>(static_cast<s64>(now_) + sl_.ran9 - arm7_debt_) : now_;
    sl_.phase = SL_A7; cpu = &a7; run = nds_.run_arm7;
    if (sl_.skip7) { advance_dma_only(a7); goto a7_done; }
    cut_arm(1, a7, CUT_PROBE7);
    goto cpu_begin;
  }
a7_done:
  {
    if (nds_.dsi_soft_reset_pending) nds_.dsi_soft_reset();   // BPTWL soft reset mid-slice
    const s64 consumed7 = (a7.halted || sl_.skip7) ? sl_.budget7 : (sl_.budget7 - a7.hot.cycle_budget);
    arm7_debt_ -= consumed7 * 2;
  }
slice_end:
  {
    running_ = nullptr;
    if (idle_cut_) { cut_note(0, a9); cut_note(1, a7); }
    now_ += static_cast<u64>(sl_.ran9);
    if (debug_slices_) std::fprintf(stderr, "[slice] now %llu ran9 %lld a9pc %08x b7 %d a7left %d a7pc %08x\n", (unsigned long long)now_, (long long)sl_.ran9, a9.hot.regs[15], sl_.budget7, a7.hot.cycle_budget, a7.hot.regs[15]);
    { prof::Scope sched_scope(prof::SCHED); }   // the walk itself is inside fire_due's per-handler accounting
    fire_due();
    goto begin;
  }
}

extern "C" SliceNext ds_slice_next(void* scheduler) { return static_cast<Scheduler*>(scheduler)->slice_next(); }

u64 Scheduler::run_until_native(u64 until, bool until_frame) {
  const u64 start = now_;
  fire_due();   // anything already due (see fire_due): the loop only fires at slice ends
  sl_.until = until;
  sl_.until_frame = until_frame;
  sl_.phase = SL_BEGIN;
  jit::run_loop(this);
  return now_ - start;
}

#endif // DSPERATE_JIT

// The frame flag is set by the line-0 scanline handler, i.e. from the fire_due
// at a slice end, so testing it at the next slice start stops at exactly the
// point `while (!frame_ready) run_until(next_deadline())` stopped at.
bool Scheduler::done(u64 until, bool until_frame) const {
  return (until_frame && nds_.frame_ready) || now_ >= until;
}

u64 Scheduler::run_until(u64 until) { return run_until_impl(until, false); }

u64 Scheduler::run_until_frame() { return run_until_impl(~u64{0}, true); }

u64 Scheduler::run_until_or_frame(u64 until) { return run_until_impl(until, true); }

u64 Scheduler::run_until_impl(u64 until, bool until_frame) {
#if DSPERATE_JIT
  if (jit::has_runtime()) return run_until_native(until, until_frame);
#endif
  const u64 start = now_;
  fire_due();   // anything already due (see fire_due): the loop only fires at slice ends
  while (!done(until, until_frame)) {
    u64 deadline = next_deadline();
    if (deadline > until) deadline = until;
    s64 slice = static_cast<s64>(deadline - now_);
    if (slice <= 0) slice = 1;
    bool skip9 = false, skip7 = false;
    const bool all_idle = machine_idle(skip9, skip7);
    const bool idle = slice > SLICE_QUANTUM && all_idle && !dsi_;   // DSi: melonDS steps 64 cycles even with both CPUs asleep (its timers are checked per step)
    if (slice >= SLICE_QUANTUM + slice_margin_ && !idle) slice = SLICE_QUANTUM;   // melonDS: minEvent < max + margin extends, equal does not
    u64 wake = 0;
    if (!all_idle && !skip7 && arm7_spi_poll(wake)) {
      skip7 = true;
      if (wake > now_ && static_cast<s64>(wake - now_) < slice) slice = static_cast<s64>(wake - now_);
      if (prof::enabled) { prof::add(prof::C_A7_SPI_SLEEP, 1); prof::add(prof::C_CYC_A7_SPI_SLEPT, static_cast<u64>(slice)); }
    }
    if (prof::enabled) count_slice(idle, slice);
    if (prof::enabled && (skip9 || skip7)) prof::add(prof::C_CYC_IDLE_SKIPPED, static_cast<u64>(slice));

    // ARM9 gets the whole slice; ARM7 then catches up at half clock.
    CpuContext& a9 = nds_.cpu(Cpu::ARM9);
    CpuContext& a7 = nds_.cpu(Cpu::ARM7);
    budget9_ = budget9_for(slice); slice_end_ = now_ + static_cast<u64>(slice);
    a9.hot.cycle_budget = budget9_;
    if (a9.boot_stall) take_stall(a9);
    if (a9.irq_offline) { a9.irq_skip_once = !a9.halted; a9.irq_offline = false; }
    running_ = &a9; running_start_budget_ = budget9_; running_shift_ = 0; running_rshift_ = dsi_ ? shift9_ + 1 : 0; running_base_ = now_; running_carry_ = static_cast<u64>(arm9_carry_);
    if (skip9) advance_dma_only(a9);
    else {
      cut_arm(0, a9, CUT_PROBE9);
      run_cpu(a9, nds_.run_arm9);
      if (cut_held_[0] && !cut_resolve(0, a9) && a9.hot.cycle_budget > 0 && !a9.halted) run_cpu(a9, nds_.run_arm9);
    }
    // A halted CPU consumes exactly the slice; a running one may overshoot,
    // which carries into the next slice.
    const bool full9 = (a9.halted || skip9) && !a9_dma_iter_;
    const bool dma_iter = a9_dma_iter_; a9_dma_iter_ = false;
    s64 ran9 = full9 ? slice : ticks9(budget9_ - a9.hot.cycle_budget);
    if (full9) arm9_carry_ = 0;   // melonDS: a halted ARM9 is set to the target exactly
    if (ran9 <= 0) ran9 = dma_iter ? 0 : 1;   // see above
    running_ = nullptr; running_rshift_ = 0;

    // The ARM7 runs at half clock and must cover the same span of time; its
    // overshoot and the odd ARM9 cycle are carried in arm7_debt_.
    arm7_debt_ += ran9;
    const s32 budget7 = static_cast<s32>(arm7_debt_ / 2);
    if (budget7 > 0) {
      a7.hot.cycle_budget = budget7;
      if (a7.boot_stall) take_stall(a7);
      if (a7.irq_offline) { a7.irq_skip_once = !a7.halted; a7.irq_offline = false; }
      running_ = &a7; running_start_budget_ = budget7; running_shift_ = 1; running_rshift_ = 0;
      running_base_ = dsi_ ? static_cast<u64>(static_cast<s64>(now_) + ran9 - arm7_debt_) : now_;
      if (skip7) advance_dma_only(a7);
      else {
        cut_arm(1, a7, CUT_PROBE7);
        run_cpu(a7, nds_.run_arm7);
        if (cut_held_[1] && !cut_resolve(1, a7) && a7.hot.cycle_budget > 0 && !a7.halted) run_cpu(a7, nds_.run_arm7);
      }
      // A BPTWL soft reset halted the ARM7 mid-slice; handled as its run
      // returns, un-halted with a zero budget so the slice counts as run.
      if (nds_.dsi_soft_reset_pending) nds_.dsi_soft_reset();
      const s64 consumed7 = (a7.halted || skip7) ? budget7 : (budget7 - a7.hot.cycle_budget);
      arm7_debt_ -= consumed7 * 2;
    }
    running_ = nullptr;
    if (idle_cut_) { cut_note(0, a9); cut_note(1, a7); }

    now_ += static_cast<u64>(ran9);
    // DS_DEBUG_SLICES=1: one line per slice.
    if (debug_slices_) std::fprintf(stderr, "[slice] now %llu ran9 %lld a9pc %08x b7 %d a7left %d a7pc %08x\n", (unsigned long long)now_, (long long)ran9, a9.hot.regs[15], budget7, a7.hot.cycle_budget, a7.hot.regs[15]);
    fire_due();
  }
  return now_ - start;
}


template <class S> void Scheduler::sync_state(S& s) {
  s.begin("SCHD");
  static_assert(EVENT_COUNT == 29, "EVENT_COUNT changed: add a save-state version");
  s.fields(now_, arm7_debt_, armed_);
  s.fields(at_, param_);
  s.fields(idle_pc_ring_, idle_pc_pos_);   // idle-skip pre-filter is part of the timing
  s.fields(arm9_carry_);   // appended: DS states leave it 0
  s.end();
  if constexpr (S::reading) {
    fn_.fill(nullptr); in_dma_ = false; running_ = nullptr;
    static const bool dbg = std::getenv("DS_DEBUG_STATE") != nullptr;   // the armed events as loaded, against now_
    if (dbg) for (u32 i = 0; i < EVENT_COUNT; ++i)
      if (armed_ & (1u << i)) std::fprintf(stderr, "[state] event %-13s at %llu (now %llu, in %lld) param %u\n", event_name(i),
                                           (unsigned long long)at_[i], (unsigned long long)now_, (long long)(at_[i] - now_), param_[i]);
  }
}
template void Scheduler::sync_state<state::Writer>(state::Writer&);
template void Scheduler::sync_state<state::Reader>(state::Reader&);

bool Scheduler::after_load() {
  rescan();
  for (u32 i = 0; i < EVENT_COUNT; ++i)
    if ((armed_ & (1u << i)) && !fn_[i]) { std::fprintf(stderr, "[state] event %u armed without a handler\n", i); return false; }
  return true;
}

} // namespace ds
