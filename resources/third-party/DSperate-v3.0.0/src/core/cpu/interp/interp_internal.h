// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/cpu/cpu.h"
#include "core/cpu/cpu_mem.h"
#include "core/cpu/arm_decode.h"
#include "core/cpu/cpu_cycles.h"

namespace ds::interp {

// r15 holds instruction address + 8 (ARM) / + 4 (Thumb) on entry; handlers
// that branch call cpu.jump(). Every handler charges its cycles through
// cpu_cycles.h exactly once.
void exec_arm(CpuContext& cpu, u32 instr);
void exec_thumb(CpuContext& cpu, u16 instr);

// ---- flag helpers -------------------------------------------------------
constexpr u32 FLAG_N = 0x80000000, FLAG_Z = 0x40000000, FLAG_C = 0x20000000, FLAG_V = 0x10000000;

inline void set_nz(CpuContext& cpu, u32 r) {
  cpu.hot.cpsr = (cpu.hot.cpsr & ~(FLAG_N | FLAG_Z)) | (r & FLAG_N) | (r == 0 ? FLAG_Z : 0);
}
inline void set_nzc(CpuContext& cpu, u32 r, bool c) {
  cpu.hot.cpsr = (cpu.hot.cpsr & ~(FLAG_N | FLAG_Z | FLAG_C)) | (r & FLAG_N) | (r == 0 ? FLAG_Z : 0) | (c ? FLAG_C : 0);
}
inline void set_nzcv(CpuContext& cpu, u32 r, bool c, bool v) {
  cpu.hot.cpsr = (cpu.hot.cpsr & ~(FLAG_N | FLAG_Z | FLAG_C | FLAG_V)) | (r & FLAG_N) |
                 (r == 0 ? FLAG_Z : 0) | (c ? FLAG_C : 0) | (v ? FLAG_V : 0);
}
inline bool carry(const CpuContext& cpu) { return cpu.hot.cpsr & FLAG_C; }

inline u32 add_flags(CpuContext& cpu, u32 a, u32 b, u32 cin) {
  u64 wide = u64{a} + b + cin;
  u32 r = static_cast<u32>(wide);
  bool v = (~(a ^ b) & (a ^ r)) >> 31;
  set_nzcv(cpu, r, wide >> 32, v);
  return r;
}
inline u32 sub_flags(CpuContext& cpu, u32 a, u32 b, u32 cin /* cin=1: no borrow in */) {
  u64 wide = u64{a} + static_cast<u32>(~b) + cin;
  u32 r = static_cast<u32>(wide);
  bool v = ((a ^ b) & (a ^ r)) >> 31;
  set_nzcv(cpu, r, wide >> 32, v);
  return r;
}

// ---- shifter ------------------------------------------------------------
// Immediate-amount shifts (ARM DP imm-shift forms and Thumb shift-imm).
inline u32 shift_imm(u32 type, u32 amt, u32 v, bool& c) {
  switch (type) {
  case 0:                        // LSL
    if (amt == 0) return v;
    c = (v >> (32 - amt)) & 1; return v << amt;
  case 1:                        // LSR (#0 means #32)
    if (amt == 0) { c = v >> 31; return 0; }
    c = (v >> (amt - 1)) & 1; return v >> amt;
  case 2:                        // ASR (#0 means #32)
    if (amt == 0) { c = v >> 31; return static_cast<u32>(static_cast<s32>(v) >> 31); }
    c = (v >> (amt - 1)) & 1; return static_cast<u32>(static_cast<s32>(v) >> amt);
  default:                       // ROR (#0 means RRX)
    if (amt == 0) { bool cin = c; c = v & 1; return (v >> 1) | (cin ? 0x80000000u : 0); }
    c = (v >> (amt - 1)) & 1; return rotr32(v, amt);
  }
}
// Register-amount shifts (ARM DP reg-shift forms and Thumb ALU shifts).
inline u32 shift_reg(u32 type, u32 amt, u32 v, bool& c) {
  amt &= 0xFF;
  if (amt == 0) return v;
  switch (type) {
  case 0:
    if (amt < 32) { c = (v >> (32 - amt)) & 1; return v << amt; }
    c = (amt == 32) ? (v & 1) : false; return 0;
  case 1:
    if (amt < 32) { c = (v >> (amt - 1)) & 1; return v >> amt; }
    c = (amt == 32) ? (v >> 31) : false; return 0;
  case 2:
    if (amt < 32) { c = (v >> (amt - 1)) & 1; return static_cast<u32>(static_cast<s32>(v) >> amt); }
    c = v >> 31; return static_cast<u32>(static_cast<s32>(v) >> 31);
  default:
    amt &= 31;
    if (amt == 0) { c = v >> 31; return v; }
    c = (v >> (amt - 1)) & 1; return rotr32(v, amt);
  }
}

// Saturating helpers for the v5 DSP ops.
inline s32 sat_add(CpuContext& cpu, s32 a, s32 b) {
  s64 r = s64{a} + b;
  if (r > 0x7FFFFFFF) { cpu.hot.cpsr |= 0x08000000; return 0x7FFFFFFF; }
  if (r < -0x80000000LL) { cpu.hot.cpsr |= 0x08000000; return static_cast<s32>(0x80000000u); }
  return static_cast<s32>(r);
}
inline s32 sat_sub(CpuContext& cpu, s32 a, s32 b) {
  s64 r = s64{a} - b;
  if (r > 0x7FFFFFFF) { cpu.hot.cpsr |= 0x08000000; return 0x7FFFFFFF; }
  if (r < -0x80000000LL) { cpu.hot.cpsr |= 0x08000000; return static_cast<s32>(0x80000000u); }
  return static_cast<s32>(r);
}

inline u32 popcount16(u32 v) { return static_cast<u32>(__builtin_popcount(v & 0xFFFF)); }

} // namespace ds::interp
