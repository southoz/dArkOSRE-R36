// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/cpu/wait_loop.h"
#include "core/cpu/cpu.h"
#include "core/cpu/cpu_cycles.h"
#include "core/mem/bus.h"
#include "core/nds.h"

#include <cstdlib>
#include <cstring>

namespace ds::cpu {

// Where the retail ARM7 BIOS keeps it (the SWI table's entry 3 is 0x2f09).
// Verified against the bytes on every use, so another BIOS just never fires.
constexpr u32 LOOP_PC = 0x2f08;
constexpr u16 SUBS_R0_1 = 0x3801, BGT_SELF = 0xdcfd;
u32 g_wait_loop_r15_7 = std::getenv("DS_NO_WAIT_LOOP") ? 0 : LOOP_PC + 4;   // DS_NO_WAIT_LOOP=1 runs the loop as is

namespace {
WaitLoopStats g_stats;

u16 loop_half(CpuContext& cpu, u32 addr) {
  u16 v;
  if (const u8* p = cpu.page_table.read_ptr(addr)) { std::memcpy(&v, p, 2); return v; }
  return static_cast<u16>(cpu.nds->bus.fetch(Cpu::ARM7, addr, 16));
}

bool loop_at(CpuContext& cpu, u32 pc) {
  if (loop_half(cpu, pc) == SUBS_R0_1 && loop_half(cpu, pc + 2) == BGT_SELF) return true;
  g_wait_loop_r15_7 = 0;   // not this BIOS: the interpreter stops testing for it
  return false;
}
} // namespace

bool wait_loop_hook_wanted(CpuContext& cpu, u32 pc, bool thumb) {
  return thumb && cpu.which == Cpu::ARM7 && pc + 4 == g_wait_loop_r15_7 && loop_at(cpu, pc);
}

bool wait_loop_run(CpuContext& cpu) {
  const u32 pc = cpu.hot.regs[15] - 4;
  const s32 n = static_cast<s32>(cpu.hot.regs[0]);
  if (n <= 1 || cpu.halted) return false;
  // An IRQ due now is taken at the next instruction boundary; skipping ahead
  // of it would move it. Tracing and watchpoints see every instruction.
  if (cpu.hot.irq_pending && !(cpu.hot.cpsr & 0x80)) return false;
  if (cpu.nds->trace || cpu.step_limit || mem::Bus::watch_active()) return false;
  const s32 budget = cpu.hot.cycle_budget;
  if (budget <= 0 || !loop_at(cpu, pc)) return false;
  // One iteration as the model in force charges it: the subs's fetch, then
  // the taken branch's refill (a taken Thumb branch pays nothing else).
  cpu.code_cycles = pc >> 15; cpu.code_region = pc >> 24;
  cpu.hot.cycle_budget = 0;
  charge_C(cpu);
  cpu.hot.cycle_budget -= static_cast<s32>(refill_cycles(cpu, pc, true));
  const s32 cost = -cpu.hot.cycle_budget;
  cpu.hot.cycle_budget = budget;
  if (cost <= 0) return false;
  // Whole iterations the budget covers, the last one left to run: an
  // iteration that ends the budget exactly stops here, as the run loop would.
  u32 k = static_cast<u32>(budget / cost);
  if (k > static_cast<u32>(n - 1)) k = static_cast<u32>(n - 1);
  if (!k) return false;
  cpu.hot.regs[0] = static_cast<u32>(n) - k;
  cpu.hot.cpsr = (cpu.hot.cpsr & ~0xF0000000u) | 0x20000000u;   // the subs of a positive result: C set, N Z V clear
  cpu.hot.cycle_budget = budget - static_cast<s32>(k) * cost;
  ++g_stats.runs; g_stats.iterations += k; g_stats.cycles += static_cast<u64>(k) * static_cast<u64>(cost);
  return true;
}

const WaitLoopStats& wait_loop_stats() { return g_stats; }

} // namespace ds::cpu
