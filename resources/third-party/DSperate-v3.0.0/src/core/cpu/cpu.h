// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"
#include "core/mem/page_table.h"

namespace ds {

struct NDS;

// Per-CPU state, shared by the interpreter and (on AArch64) the recompiler.
// Layout is a contract with the JIT: standard-layout, offsets static_asserted,
// JIT-touched fields grouped into `JitHot` to fit one base-register-relative
// immediate range. `page_table` points to a separate 16 MiB allocation the
// JIT keeps pinned in a register (jit_internal.h).

enum class Mode : u8 {
  USR = 0x10, FIQ = 0x11, IRQ = 0x12, SVC = 0x13, ABT = 0x17, UND = 0x1B, SYS = 0x1F
};

struct JitHot {
  u32 regs[16];        // r0-r15 home locations; r15 is only valid at sync points
  u32 cpsr;
  u32 spsr;            // SPSR of the current mode (banked copies live below)
  s32 cycle_budget;    // counts DOWN; sign bit => leave translated code
  u32 irq_pending;     // non-zero => an IRQ is waiting to be taken
  u32 alerts;          // scheduler/DMA/SMC "look at me" word polled after stores
  u32 _pad;
  u64 exit_native;     // native resume/exit address
  u64 other_cpu;       // sibling CpuContext*
};

struct CpuContext {
  Cpu  which;
  bool halted;
  s32  preempt_residual = 0;  // budget handed back when a DMA cuts this CPU's slice short
  bool yielded = false;       // Scheduler::yield: residual not resumed; other CPU runs first
  bool jumped;       // set by jump(); cleared by the interpreter before each instruction
  u8   _pad0[5];

  JitHot hot;

  // Banked registers, indexed by bank {USR/SYS, FIQ, IRQ, SVC, ABT, UND}.
  u32 bank_r8_r12[2][5];   // USR and FIQ only
  u32 bank_r13[6];
  u32 bank_r14[6];
  u32 bank_spsr[6];

  // CP15 (ARM9 only).
  u32 cp15_control;
  u32 cp15_dtcm;           // DTCM base/size register value
  u32 cp15_itcm;           // ITCM size register value
  u32 pu_region[8];        // c6,cN,0: base/size/enable
  u32 pu_code_cacheable;   // c2,c0,1 (bit per region)
  u32 pu_data_cacheable;   // c2,c0,0
  u32 pu_data_bufferable;  // c3,c0,0
  u32 pu_code_perm, pu_data_perm;   // c5,c0,{3,2}
  u32 itcm_size;           // effective ITCM window (bytes), 0 when disabled
  u32 dtcm_base, dtcm_mask;// DTCM window: (addr & dtcm_mask) == dtcm_base

  // ---- cycle accounting ----
  // Per-4 KB timing (ARM9): [0] code cost or 0xFF if instruction-cacheable;
  // [1] load N16, [2] load N32, [3] load S32, [5..7] same for stores ([4]
  // unused). Per-32 KB (ARM7): N16, S16, N32, S32.
  const u8 (*timing9)[8];
  const u8 (*timing7)[4];
  const u8* cost7;   // ARM7 precomputed data cost (mem::Timing::cost7)
  u32 code_cycles;         // ARM9: cost of the most recent prefetch. ARM7: code-region table index.
  u32 data_cycles;         // accumulated data-access cost of the current instruction
  u32 code_region, data_region;   // high byte of the address (ARM7 main-RAM overlap rules)
  u8  fast_d;              // fast timing: the current instruction's data cost per access (timing_mode.h)
  bool branch_fetch;       // ARM9: the next prefetch is the first after a branch

  // Cycles owed before executing anything: reset/direct-boot pipeline fills,
  // and a DSiWare loader measuring ARM7 against ARM9 at boot. Zero on a DS.
  s32  boot_stall = 0;
  // DSi: ARM9 sequential code-fetch cost is latched at each jump, not re-read
  // per instruction, so it can go stale after a CP15 write. 0 = read the table (DS).
  u8   code_latch = 0;
  // DSi: a pending IRQ is only taken after the CPU's next instruction. Set by
  // Io::update_irq when off-slice; scheduler turns it into irq_skip_once.
  bool irq_offline = false, irq_skip_once = false;
  // DSi: cost of an instruction that started an immediate DMA is charged
  // after the next instruction, not before the DMA.
  s32  defer_cost = 0;
  // Debug single-stepping: interpreter stops after step_limit instructions if nonzero.
  u32 step_limit, steps;
  // Budget when the CPU halted, for comparing consumed cycles across engines.
  s32 budget_at_halt;

  mem::PageTable page_table;

  NDS* nds;

  // Recompiler hooks (null on the interpreter). jit_timing_changed fires when
  // the per-page cost table or TCM windows change.
  void (*jit_timing_changed)(CpuContext&) = nullptr;
  void* jit = nullptr;

  void reset(Cpu which, NDS* nds);
  template <class S> void sync_state(S& s);   // page table/timing pointers rebuilt by Bus::relink

  void switch_mode(u32 new_mode);    // banks r8-r14 and SPSR as needed
  void set_cpsr(u32 value);          // full write incl. mode switch
  void restore_cpsr();               // CPSR <- SPSR (exception return)

  enum class Exception : u8 { Reset, Undefined, Swi, PrefetchAbort, DataAbort, Irq, Fiq };
  void raise_exception(Exception e);
  void update_tcm_windows();         // recompute itcm_size/dtcm_base/dtcm_mask from CP15
  void check_irq();                  // take a pending IRQ if unmasked

  void jump(u32 addr, bool interwork);   // `addr` bit 0 selects Thumb when `interwork`
  bool thumb() const { return hot.cpsr & 0x20; }
  u32  exception_base() const;
};

static_assert(offsetof(JitHot, cycle_budget) == 72, "JitHot layout changed; update the JIT");
static_assert(sizeof(JitHot) == 104, "JitHot layout changed; update the JIT");

// Pluggable execution engine: interpreter always; JIT on AArch64.
using RunFn = void (*)(CpuContext& cpu);

} // namespace ds
