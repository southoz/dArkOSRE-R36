// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// The CPU timing model (emu.timing / DS_TIMING), process-wide and fixed at
// boot, before the recompiler emits its stubs.
//   exact  melonDS's per-access model (cpu_cycles.h): per-page costs, the
//          CD/CDI overlap, the ARM7 main-RAM rules, per-target refills.
//   fast   (the default where the recompiler implements it) every data
//          access costs a constant, loads and stores pay numC + data with no
//          overlap and no region rules, and every jump's refill is a constant.
// What fast removes is the per-access timing code in translated code (a table
// load and the combine) and the refill lookup of every indirect branch; numC,
// resolved when a block is translated, stays exact. The cartridge keeps its
// exact timing under fast (games break without it: Io::cart_write_romctrl,
// Io::cart_word_delay); DS_CART_GAPS=0 / DS_CART_CLOCK=0 are the old shortcuts.
#pragma once
#include "core/cpu/cpu.h"

namespace ds {

// The ARM9's data cost depends on the base register, a translation-time fact:
// SP-based accesses (the stack, in DTCM in nearly every game) cost d9s, all
// others (main RAM, I/O) d9. One constant cannot fit both. The exact model
// overlaps a DTCM access with the fetch, so stack-heavy frame logic (ST, NSMB)
// pays almost nothing for data, while a main-RAM access costs ~5 over numC.
// A single constant low enough for the former made main-RAM-bound idle work
// (PW2's) iterate ~40% more per frame, and one at the whole-run average (4)
// pushed ST's and NSMB's frame logic past VBlank, halving their 3D rate.
struct FastTiming { u32 d9s = 0, d9 = 5, d7 = 2, r9 = 4, r7 = 2; };
extern bool g_fast_timing;
extern FastTiming g_fast;

// Whether a load/store addresses through SP. ARM: every load/store encoding
// has its base in bits 19-16 (and the cost is only read by those). Thumb: only
// SP-relative LDR/STR and PUSH/POP use SP; every other form has a low base.
inline bool fast_sp_based(u32 instr, bool thumb) {
  return thumb ? ((instr & 0xF000) == 0x9000 || (instr & 0xF600) == 0xB400) : ((instr >> 16) & 15) == 13;
}
inline u32 fast_data(const CpuContext& cpu, u32 instr, bool thumb) {
  if (cpu.which != Cpu::ARM9) return g_fast.d7;
  return fast_sp_based(instr, thumb) ? g_fast.d9s : g_fast.d9;
}

} // namespace ds
