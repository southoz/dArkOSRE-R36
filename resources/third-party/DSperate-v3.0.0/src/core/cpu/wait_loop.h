// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// The ARM7 BIOS's WaitByLoop (SWI 3) countdown, `subs r0, #1; bgt` on
// itself, fast-forwarded: at the loop head the iterations the slice's budget
// covers are skipped in one step, leaving the last to run for real. An
// iteration touches only r0 and the flags and costs the same every time, so
// the state and the cycle count after the skip are exactly what running it
// would leave -- an IRQ, a slice end or a trace sees no difference. Spirit
// Tracks' ARM7 spends most of its time here.
#pragma once
#include "core/types.h"

namespace ds {
struct CpuContext;
}

namespace ds::cpu {

// r15 (loop pc + 4, Thumb) of the ARM7's loop head, the interpreter's
// per-instruction test; 0 once the bytes there turn out not to be the loop,
// or under DS_NO_WAIT_LOOP=1.
extern u32 g_wait_loop_r15_7;

// Whether a block starting at `pc` is the loop (the recompiler's hook).
bool wait_loop_hook_wanted(CpuContext& cpu, u32 pc, bool thumb);
// At the loop head: skip what the budget covers. True if anything was skipped.
bool wait_loop_run(CpuContext& cpu);

struct WaitLoopStats { u64 runs, iterations, cycles; };
const WaitLoopStats& wait_loop_stats();

} // namespace ds::cpu
