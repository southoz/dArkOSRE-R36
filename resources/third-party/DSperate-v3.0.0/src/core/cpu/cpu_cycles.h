// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Per-instruction cycle model, shared by the interpreter and the recompiler.
// Mirrors melonDS: every instruction pays a code-fetch cost numC (ARM9: the
// prefetch two ahead, `code_cycles`; ARM7: S for plain instructions, N for
// ones with an internal cycle or data access). CI adds `internal`; CD/CDI
// combine numC with data cost numD, overlapping where hardware does.
// Branches pay the pipeline refill instead (`refill_cycles`, via jump()).
//
// Charge timing matters: charge_* runs before a jump (old pc/state) for ALU/
// LDR/hi-reg ops, or after (new pc/state/code region) for LDM/POP.
#pragma once
#include "core/cpu/cpu.h"
#include "core/cpu/cpu_mem.h"
#include "core/cpu/timing_mode.h"

#include <algorithm>

namespace ds {


// ARM9 numC at the moment of charging: the odd halfword of a Thumb pair is
// already in the prefetch buffer.
inline u32 num_c9(const CpuContext& cpu) {
  return (cpu.thumb() && (cpu.hot.regs[15] & 2)) ? 0 : cpu.code_cycles;
}

inline void charge_C(CpuContext& cpu) {
  if (cpu.which == Cpu::ARM9) { cpu.hot.cycle_budget -= static_cast<s32>(num_c9(cpu)); return; }
  cpu.hot.cycle_budget -= cpu.timing7[cpu.code_cycles][cpu.thumb() ? 1 : 3];
}

inline void charge_CI(CpuContext& cpu, u32 internal) {
  if (cpu.which == Cpu::ARM9) { cpu.hot.cycle_budget -= static_cast<s32>(num_c9(cpu) + internal); return; }
  cpu.hot.cycle_budget -= static_cast<s32>(cpu.timing7[cpu.code_cycles][cpu.thumb() ? 0 : 2] + internal);
}

inline u32 max3(s32 a, s32 b, s32 c) { return static_cast<u32>(std::max(a, std::max(b, c))); }

// Fast model: numC (the ARM7's non-sequential one) plus the data cost, no overlap.
inline void charge_fast_CD(CpuContext& cpu) {
  const u32 numC = cpu.which == Cpu::ARM9 ? num_c9(cpu) : cpu.timing7[cpu.code_cycles][cpu.thumb() ? 0 : 2];
  cpu.hot.cycle_budget -= static_cast<s32>(numC + cpu.data_cycles);
}

// Loads (the ARM9 skips the internal cycle; the ARM7 pays it unless main RAM absorbs it).
inline void charge_CDI(CpuContext& cpu) {
  if (g_fast_timing) { charge_fast_CD(cpu); return; }
  const s32 numD = static_cast<s32>(cpu.data_cycles);
  if (cpu.which == Cpu::ARM9) {
    const s32 numC = static_cast<s32>(num_c9(cpu));
    cpu.hot.cycle_budget -= static_cast<s32>(max3(numC + numD - 6, numC, numD));
    return;
  }
  s32 numC = cpu.timing7[cpu.code_cycles][cpu.thumb() ? 0 : 2];
  s32 d = numD;
  u32 cost;
  if (cpu.data_region == 0x02) {
    if (cpu.code_region == 0x02) cost = static_cast<u32>(numC + d);
    else { numC++; cost = max3(numC + d - 3, numC, d); }
  } else {
    if (cpu.code_region == 0x02) { d++; cost = max3(numC + d - 3, numC, d); }
    else cost = static_cast<u32>(numC + d + 1);
  }
  cpu.hot.cycle_budget -= static_cast<s32>(cost);
}

// A load that just jumped (LDM pc, POP pc). melonDS charges with R[15] at
// target+2 in Thumb where jump() leaves target+4, so `& 2` here is inverted.
inline void charge_CDI_after_jump(CpuContext& cpu) {
  if (g_fast_timing) { cpu.hot.cycle_budget -= static_cast<s32>(cpu.data_cycles); return; }   // the constant refill stands for numC
  if (cpu.which == Cpu::ARM9) {
    const s32 numC = (cpu.thumb() && !(cpu.hot.regs[15] & 2)) ? 0 : static_cast<s32>(cpu.code_cycles);
    const s32 numD = static_cast<s32>(cpu.data_cycles);
    cpu.hot.cycle_budget -= static_cast<s32>(max3(numC + numD - 6, numC, numD));
    return;
  }
  charge_CDI(cpu);
}

// Stores.
inline void charge_CD(CpuContext& cpu) {
  if (g_fast_timing) { charge_fast_CD(cpu); return; }
  const s32 numD = static_cast<s32>(cpu.data_cycles);
  if (cpu.which == Cpu::ARM9) {
    const s32 numC = static_cast<s32>(num_c9(cpu));
    cpu.hot.cycle_budget -= static_cast<s32>(max3(numC + numD - 6, numC, numD));
    return;
  }
  const s32 numC = cpu.timing7[cpu.code_cycles][cpu.thumb() ? 0 : 2];
  u32 cost;
  if ((cpu.data_region == 0x02) == (cpu.code_region == 0x02)) cost = static_cast<u32>(numC + numD);
  else cost = max3(numC + numD - 3, numC, numD);
  cpu.hot.cycle_budget -= static_cast<s32>(cost);
}

// ARM9 prefetch cost for the instruction about to execute (r15 = two ahead).
inline void prefetch_cost9(CpuContext& cpu) {
  const u32 pc = cpu.hot.regs[15];
  if (cpu.thumb() && (pc & 2)) { cpu.code_cycles = 0; return; }
  u8 c = cpu.code_latch ? cpu.code_latch : cpu.timing9[pc >> 12][0];
  if (cpu.code_latch && pc < cpu.itcm_size) c = 1;   // ITCM fetches always cost 1
  cpu.code_cycles = (c == 0xFF) ? (!(pc & 0x1F) ? 3 : 1) : c;
}

// Cost of one 32-bit instruction fetch at `addr`, in ARM9 cycles. `branch`
// marks the first fetch after a branch (a cache line fill on cacheable code).
inline u32 fetch_cost9(const CpuContext& cpu, u32 addr, bool branch) {
  const u8 c = cpu.timing9[addr >> 12][0];
  if (c == 0xFF) return (branch || !(addr & 0x1F)) ? 3 : 1;   // cache line fill vs hit (approximation)
  return c;
}

// Pipeline refill for a jump to `addr`. `code_after` receives the ARM9
// prefetch cost of the refill's last fetch, used as numC by a post-jump charge.
inline u32 refill_cycles(const CpuContext& cpu, u32 addr, bool thumb, u32* code_after = nullptr) {
  if (cpu.which == Cpu::ARM9) {
    u32 c, last;
    if (thumb) {
      if (addr & 2) { last = fetch_cost9(cpu, addr + 2, false); c = fetch_cost9(cpu, addr - 2, true) + last; }
      else { last = fetch_cost9(cpu, addr, true); c = last; }
    } else {
      last = fetch_cost9(cpu, addr + 4, false);
      c = fetch_cost9(cpu, addr, true) + last;
    }
    if (code_after) *code_after = last;
    return g_fast_timing ? g_fast.r9 : c;
  }
  if (g_fast_timing) return g_fast.r7;
  const u8* t = cpu.timing7[addr >> 15];
  return thumb ? (t[0] + t[1]) : (t[2] + t[3]);
}

} // namespace ds
