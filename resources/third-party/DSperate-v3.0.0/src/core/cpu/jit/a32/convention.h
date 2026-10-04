// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// ARMv7 (A32) backend: host register convention shared by the stubs and the
// translator. Nothing outside a32/ sees this.
//
// Only 13 usable registers, so unlike a64 only guest r0-r3 and sp are pinned
// (in the free callee-saved regs); other guest registers live in memory
// (JitHot::regs) with a per-block RegCache (translate.cpp) in the scratch regs.
//
//   r0-r3, r12       scratch: cache slots, shifter/cost temporaries, C call
//                    args (r12 = helper address in the stubs)
//   r4-r7            guest r0-r3     (callee-saved: survive C calls)
//   r8               guest r13
//   r9               cycle budget minus one (bit 31 set => leave)
//   r10              page-table base for this CPU
//   r11              CpuContext*
//   r13              sp
//   r14              lr: stubs called with `bl` read their literal arguments
//                    (instruction, key, ...) through it
//
// Guest NZCV and Q live in the host APSR (bits 31:27); rest of CPSR is in
// memory. Q rides along since every APSR write masks nzcvq, making v5TE
// saturating instructions native with no separate sticky-flag merge.
#pragma once
#include "core/cpu/jit/jit_internal.h"

namespace ds::jit {

constexpr u32 R_BUDGET = 9, R_PT = 10, R_CTX = 11, R_FN = 12, R_SP = 13, R_LR = 14, R_PC = 15;
constexpr u32 SCRATCH0 = 0, SCRATCH1 = 1, SCRATCH2 = 2, SCRATCH3 = 3, SCRATCH4 = 12;

inline constexpr u32 pinned_host(u32 guest) {
  return guest < 4 ? 4 + guest : guest == 13 ? 8 : 0xFFu;
}
constexpr u32 PINNED_MASK = 0xFu | (1u << 13);

} // namespace ds::jit
