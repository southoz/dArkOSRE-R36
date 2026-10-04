// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// AArch64 backend: the host register convention shared by the stubs and the
// translator. Nothing outside a64/ sees this.
#pragma once
#include "core/cpu/jit/jit_internal.h"

namespace ds::jit {

// ---- host register convention --------------------------------------------
// Guest registers are pinned; every block and stub agrees on this map, so
// linked blocks reconcile nothing at the edge.
//
//   x0-x7, x16, x17   scratch (also C call arguments)
//   w8                cycle budget minus one (bit 31 set => leave): one `tbnz`
//                     tests it; the stubs add/subtract the one at the boundary
//   x9-x13            guest r8-r12   (caller-saved: the call stubs spill them)
//   x14               page-table base for this CPU
//   x15               per-page timing table (timing9 or timing7)
//   x18               base of the code arena: the branch LUTs sit at its front
//                     and every block pointer is a 32-bit offset from it, so
//                     one register serves the dispatch probe and the jump.
//                     Linux leaves the platform register alone; C code may
//                     clobber it, so the call stubs reload it like x14/x15.
//   x19-x26           guest r0-r7    (callee-saved: survive C calls)
//   x27, x28          guest r13, r14
//   x29               CpuContext*
//   x30               link register: stubs called with `bl` read their literal
//                     arguments (instruction, key, ...) through it
//
// Guest NZCV live in the host NZCV; the rest of CPSR lives in memory.
constexpr u32 R_BUDGET = 8, R_PT = 14, R_TIM = 15, R_ARENA = 18, R_CTX = 29, R_LR = 30;
constexpr u32 SCRATCH0 = 0, SCRATCH1 = 1, SCRATCH2 = 2, SCRATCH3 = 3, SCRATCH4 = 4, SCRATCH5 = 5, SCRATCH6 = 6, SCRATCH7 = 7;
constexpr u32 R_FN = 16;        // function address for the call stubs

inline constexpr u32 host_reg(u32 guest) {
  return guest < 8 ? 19 + guest : guest < 13 ? 9 + (guest - 8) : guest == 13 ? 27 : 28;
}
inline constexpr bool host_reg_callee_saved(u32 guest) { return guest < 8 || guest >= 13; }

} // namespace ds::jit
