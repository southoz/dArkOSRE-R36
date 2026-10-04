// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/cpu/interp/interp.h"
#include "core/cpu/wait_loop.h"
#include "core/cpu/interp/interp_internal.h"
#include "core/cpu/cpu_cycles.h"
#include "core/nds.h"
#include "core/profile.h"

#include "core/cpu/arm_decode.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <utility>
#include <vector>


namespace ds { void (*g_census_access)(bool, u32, bool) = nullptr; }

namespace ds::interp {

// DS_CENSUS=1 (census builds): count the executed guest instruction stream by shape.
namespace census {

struct Counts {
  u64 total = 0, arm = 0, thumb = 0;
  u64 cond = 0;                 // ARM, cond != AL
  u64 cond_csel = 0;            // csel candidate: dp, S=0, rd != 15
  u64 cond_dp_s = 0;            // dp that also writes flags
  u64 cond_mem = 0;
  u64 cond_branch = 0;
  u64 cond_other = 0;
  u64 access = 0;               // single data transfers (ARM + Thumb)
  u64 block = 0, block_regs = 0;
  // same_page: access in same 4 KB timing-table page as previous.
  u64 same_page = 0, pc_rel = 0, sp_rel = 0;
  u64 charged = 0, charged_seq = 0;
  u32 last_page = 0xFFFFFFFFu;
  u64 mmio = 0;
  std::map<u32, u64> mmio_hits;
  u64 indirect = 0;
  // Block transfer crossing a 2 KB page forces interpreter fallback.
  u64 straddle = 0;
  u32 burst_page = 0xFFFFFFFFu;
  bool burst_counted = false;
};
inline Counts& at(bool a9) { static Counts c[2]; return c[a9 ? 0 : 1]; }
const bool g_on = prof::census_env("DS_CENSUS");
inline bool on() { return prof::census && g_on; }

inline void note_access(bool a9, u32 addr, bool seq) {
  Counts& c = at(a9);
  if ((addr >> 24) == 0x04) { ++c.mmio; ++c.mmio_hits[addr & ~3u]; }
  if (!seq) { c.burst_page = addr >> 11; c.burst_counted = false; }
  else if (!c.burst_counted && (addr >> 11) != c.burst_page) { ++c.straddle; c.burst_counted = true; }
  ++c.charged;
  if (seq) ++c.charged_seq;
  const u32 page = addr >> 12;
  if (page == c.last_page) ++c.same_page;
  c.last_page = page;
}

inline void arm_instr(bool a9, u32 instr) {
  Counts& c = at(a9); ++c.total; ++c.arm;
  const arm::AOp op = arm::decode_arm(instr);
  switch (op) {
  case arm::AOp::LdrStrImm: case arm::AOp::LdrStrHImm: {
    const u32 rn = (instr >> 16) & 0xF;
    if (rn == 15) ++c.pc_rel; else if (rn == 13) ++c.sp_rel;
    break;
  }
  default: break;
  }
  switch (op) {
  case arm::AOp::LdrStrImm: case arm::AOp::LdrStrReg:
  case arm::AOp::LdrStrHImm: case arm::AOp::LdrStrHReg:
  case arm::AOp::Swp: case arm::AOp::Swpb: ++c.access; break;
  case arm::AOp::Ldm: case arm::AOp::Stm:
    ++c.block; c.block_regs += static_cast<u32>(__builtin_popcount(instr & 0xFFFF));
    if (op == arm::AOp::Ldm && (instr & 0x8000)) ++c.indirect;      // LDM ..., pc
    break;
  case arm::AOp::Bx: case arm::AOp::BlxReg: ++c.indirect; break;
  default: break;
  }
  if ((instr >> 28) == 0xE) return;          // AL: not conditional
  ++c.cond;
  switch (op) {
  case arm::AOp::DpImm: case arm::AOp::DpImmShift: case arm::AOp::DpRegShift: {
    const bool sets_flags = (instr & (1u << 20)) != 0;
    const u32 rd = (instr >> 12) & 0xF;
    const u32 dpop = (instr >> 21) & 0xF;
    const bool compare = dpop >= 0x8 && dpop <= 0xB;   // TST/TEQ/CMP/CMN
    if (sets_flags || compare) ++c.cond_dp_s; else if (rd != 15) ++c.cond_csel; else ++c.cond_other;
    break;
  }
  case arm::AOp::LdrStrImm: case arm::AOp::LdrStrReg: case arm::AOp::LdrStrHImm:
  case arm::AOp::LdrStrHReg: case arm::AOp::Ldm: case arm::AOp::Stm:
  case arm::AOp::Swp: case arm::AOp::Swpb: ++c.cond_mem; break;
  case arm::AOp::B: case arm::AOp::Bl: case arm::AOp::BlxImm: ++c.cond_branch; break;
  default: ++c.cond_other; break;
  }
}

inline void thumb_instr(bool a9, u16 instr) {
  Counts& c = at(a9); ++c.total; ++c.thumb;
  using T = arm::TOp;
  switch (arm::decode_thumb(instr)) {
  case T::LdrPcRel:    ++c.pc_rel; break;
  case T::LdrStrSpRel: ++c.sp_rel; break;
  default: break;
  }
  switch (arm::decode_thumb(instr)) {
  case T::LdrPcRel: case T::LdrStrReg: case T::LdrStrImm5:
  case T::LdrStrHImm5: case T::LdrStrSpRel: ++c.access; break;
  case T::PushPop:
    ++c.block; c.block_regs += static_cast<u32>(__builtin_popcount(instr & 0xFF)) + ((instr & 0x0100) ? 1 : 0);
    if ((instr & 0x0900) == 0x0900) ++c.indirect;                    // POP {..., pc}
    break;
  case T::BxBlx: ++c.indirect; break;
  case T::StmLdm:
    ++c.block; c.block_regs += static_cast<u32>(__builtin_popcount(instr & 0xFF)); break;
  default: break;
  }
}

inline void report(u64 frames) {
  if (!on()) return;
  const double f = frames ? static_cast<double>(frames) : 1.0;
  std::fprintf(stderr, "[census] frames %llu\n", (unsigned long long)frames);
  for (int i = 0; i < 2; ++i) {
    const Counts& c = at(i == 0);
    if (!c.total) continue;
    const char* n = i == 0 ? "arm9" : "arm7";
    std::fprintf(stderr, "[census] %s executed/frame %10.0f  (arm %.0f  thumb %.0f)\n", n, c.total/f, c.arm/f, c.thumb/f);
    std::fprintf(stderr, "[census] %s   data accesses/frame %8.0f  = %5.2f%% of executed\n", n, c.access/f, 100.0*c.access/c.total);
    std::fprintf(stderr, "[census] %s   block transfers/frame %6.0f  moving %.0f regs (%.1f/xfer)\n", n, c.block/f, c.block_regs/f, c.block ? (double)c.block_regs/c.block : 0.0);
    std::fprintf(stderr, "[census] %s   COST-MODEL RUNS/frame %8.0f  (of which burst-sequential %.0f)\n", n, c.charged/f, c.charged_seq/f);
    std::fprintf(stderr, "[census] %s   same 4K page as previous run %8.0f = %5.2f%% (lookup reusable)\n", n, c.same_page/f, c.charged ? 100.0*c.same_page/c.charged : 0.0);
    std::fprintf(stderr, "[census] %s   pc-relative (page known at translate) %5.0f   sp-relative %6.0f\n", n, c.pc_rel/f, c.sp_rel/f);
    std::fprintf(stderr, "[census] %s   MMIO accesses/frame %8.0f  over %zu distinct registers\n", n, c.mmio/f, c.mmio_hits.size());
    std::fprintf(stderr, "[census] %s   indirect branches/frame %6.0f\n", n, c.indirect/f);
    std::fprintf(stderr, "[census] %s   block transfers straddling 2 KB %5.0f = %4.1f%% of transfers\n", n, c.straddle/f, c.block ? 100.0*c.straddle/c.block : 0.0);
    std::vector<std::pair<u64,u32> > top;
    for (std::map<u32,u64>::const_iterator it = c.mmio_hits.begin(); it != c.mmio_hits.end(); ++it)
      top.push_back(std::make_pair(it->second, it->first));
    std::sort(top.rbegin(), top.rend());
    u64 cum = 0;
    for (size_t i = 0; i < top.size() && i < 12; ++i) {
      cum += top[i].first;
      std::fprintf(stderr, "[census] %s     %08x %8.0f/frame  %5.1f%%  (cum %4.1f%%)\n", n,
                   top[i].second, top[i].first/f, 100.0*top[i].first/c.mmio, 100.0*cum/c.mmio);
    }
    std::fprintf(stderr, "[census] %s   conditional (non-AL)/frame %5.0f = %5.2f%% of executed\n", n, c.cond/f, 100.0*c.cond/c.total);
    std::fprintf(stderr, "[census] %s     csel-able dp      %8.0f = %5.2f%% of executed\n", n, c.cond_csel/f, 100.0*c.cond_csel/c.total);
    std::fprintf(stderr, "[census] %s     dp writing flags  %8.0f\n", n, c.cond_dp_s/f);
    std::fprintf(stderr, "[census] %s     memory            %8.0f\n", n, c.cond_mem/f);
    std::fprintf(stderr, "[census] %s     branch            %8.0f\n", n, c.cond_branch/f);
    std::fprintf(stderr, "[census] %s     other             %8.0f\n", n, c.cond_other/f);
  }
}

} // namespace census


void run(CpuContext& cpu) {
  if (census::on() && !g_census_access) g_census_access = &census::note_access;
  const bool dsi = cpu.nds->dsi;
  {
    // DSi: an IRQ taken here (wake from halt, or off-slice) has its vector
    // refill charged after the first instruction, not now.
    const s32 b0 = cpu.hot.cycle_budget;
    cpu.check_irq();
    if (dsi && cpu.hot.cycle_budget != b0) { cpu.defer_cost += b0 - cpu.hot.cycle_budget; cpu.hot.cycle_budget = b0; }
  }
  if (cpu.halted) { cpu.hot.cycle_budget = -1; return; }
  const bool a9 = cpu.which == Cpu::ARM9;

  // DS_DEBUG_CYCLES=1: log budget before every instruction.
  static const bool debug_cycles = std::getenv("DS_DEBUG_CYCLES") != nullptr;
  while (cpu.hot.cycle_budget > 0) {
    const s32 b_before = cpu.hot.cycle_budget;
    cpu.data_cycles = 0;
    cpu.jumped = false;
    if (debug_cycles) std::fprintf(stderr, "[cyc%d] %08x %d\n", a9 ? 9 : 7, cpu.hot.regs[15], cpu.hot.cycle_budget);
    if (cpu.thumb()) {
      // The ARM7 BIOS WaitByLoop head: skip the iterations the budget covers.
      if (!a9 && cpu.hot.regs[15] == cpu::g_wait_loop_r15_7 && cpu::wait_loop_run(cpu)) continue;
      const u32 pc = cpu.hot.regs[15] - 4;
      const u16 instr = fetch16(cpu, pc);
      if (census::on()) census::thumb_instr(a9, instr);
      if (cpu.nds->trace) cpu.nds->trace(cpu, instr, cpu.nds->trace_user);
      if (a9) prefetch_cost9(cpu);
      exec_thumb(cpu, instr);
      if (!cpu.jumped) cpu.hot.regs[15] += 2;
    } else {
      const u32 pc = cpu.hot.regs[15] - 8;
      const u32 instr = fetch32(cpu, pc);
      if (census::on()) census::arm_instr(a9, instr);
      if (cpu.nds->trace) cpu.nds->trace(cpu, instr, cpu.nds->trace_user);
      if (a9) prefetch_cost9(cpu);
      exec_arm(cpu, instr);
      if (!cpu.jumped) cpu.hot.regs[15] += 4;
    }
    if (cpu.defer_cost) { cpu.hot.cycle_budget -= cpu.defer_cost; cpu.defer_cost = 0; }
    if (cpu.halted) {
      // DSi: the halting instruction's cost stays pending until wake.
      if (dsi) { cpu.defer_cost += b_before - cpu.hot.cycle_budget; cpu.hot.cycle_budget = b_before; }
      cpu.budget_at_halt = cpu.hot.cycle_budget; cpu.hot.cycle_budget = -1; return;
    }
    if (cpu.hot.irq_pending) cpu.check_irq();
    if (cpu.step_limit && ++cpu.steps >= cpu.step_limit) return;
  }
}

void census_report(u64 frames) { census::report(frames); }

} // namespace ds::interp
