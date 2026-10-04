// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Instruction classification shared by the interpreter and the recompiler.
// ARM:   index = ((instr >> 16) & 0xFF0) | ((instr >> 4) & 0xF)   (bits 27:20, 7:4)
// Thumb: index = instr >> 6                                        (bits 15:6)
// Tables are built at compile time from the classifiers below.
#pragma once
#include "core/types.h"

#include <array>

namespace ds::arm {

enum class AOp : u8 {
  Undefined,
  // data processing; shifter form in the low bits of the instruction
  DpImm, DpImmShift, DpRegShift,
  // PSR
  Mrs, MsrReg, MsrImm,
  // branches
  B, Bl, BlxImm, Bx, BlxReg,
  // multiply
  Mul, Mla, Umull, Umlal, Smull, Smlal,
  // v5 DSP
  Clz, QAdd, QSub, QDAdd, QDSub, SmlaXY, SmlawY, SmulwY, SmlalXY, SmulXY,
  // single data transfer (LDR/STR word/byte)
  LdrStrImm, LdrStrReg,
  // halfword / signed / doubleword
  LdrStrHImm, LdrStrHReg,     // H, SB, SH (LDRD/STRD folded in; see op2 bits)
  Swp, Swpb,
  Ldm, Stm,
  Swi, Bkpt,
  Cdp, Mcr, Mrc, Ldc, Stc,
  Pld,
};

enum class TOp : u8 {
  Undefined,
  ShiftImm,        // LSL/LSR/ASR rd, rs, #imm5
  AddSubReg,       // ADD/SUB rd, rs, rn
  AddSubImm3,      // ADD/SUB rd, rs, #imm3
  MovCmpAddSubImm8,
  Alu,             // 16 register ALU ops
  HiRegOp,         // ADD/CMP/MOV with high registers
  BxBlx,
  LdrPcRel,
  LdrStrReg,       // STR STRH STRB LDRSB LDR LDRH LDRB LDRSH
  LdrStrImm5,      // word/byte
  LdrStrHImm5,
  LdrStrSpRel,
  AddPcSp,
  AdjustSp,
  PushPop,
  StmLdm,
  BCond,
  Swi,
  B,
  BlxSuffix,
  BlPrefix,
  BlSuffix,
  Bkpt,
};

constexpr inline u32 arm_index(u32 instr) { return ((instr >> 16) & 0xFF0) | ((instr >> 4) & 0xF); }
constexpr inline u32 thumb_index(u16 instr) { return instr >> 6; }

constexpr AOp classify_arm(u32 idx) {
  const u32 hi = idx >> 4;        // bits 27:20
  const u32 lo = idx & 0xF;       // bits 7:4
  const u32 b27_25 = hi >> 5;
  const bool bit20 = hi & 1;

  switch (b27_25) {
  case 0: {
    if ((lo & 9) == 9) {                         // bit7 & bit4: mul / extra ls
      if (lo == 9) {
        if (!(hi & 0x10)) {                      // bit24 == 0: multiply family
          switch ((hi >> 1) & 7) {               // bits 23:21
          case 0: return AOp::Mul;
          case 1: return AOp::Mla;
          case 4: return AOp::Umull;
          case 5: return AOp::Umlal;
          case 6: return AOp::Smull;
          case 7: return AOp::Smlal;
          default: return AOp::Undefined;
          }
        }
        if ((hi & 0x0B) == 0)                    // bit24=1, bits 23,21,20 = 0 -> SWP; bit22 = byte
          return (hi & 4) ? AOp::Swpb : AOp::Swp;
        return AOp::Undefined;
      }
      // lo = 0xB, 0xD, 0xF : halfword/signed/double
      return (hi & 0x04) ? AOp::LdrStrHImm : AOp::LdrStrHReg;   // bit22: imm form
    }
    if (((hi >> 3) & 3) == 2 && !bit20) {        // bits 24:23 = 10, S=0: misc
      const u32 op = (hi >> 1) & 3;              // bits 22:21
      switch (lo) {
      case 0x0: return (op & 1) ? AOp::MsrReg : AOp::Mrs;
      case 0x1: if (op == 1) return AOp::Bx; if (op == 3) return AOp::Clz; return AOp::Undefined;
      case 0x3: return (op == 1) ? AOp::BlxReg : AOp::Undefined;
      case 0x5: switch (op) { case 0: return AOp::QAdd; case 1: return AOp::QSub;
                               case 2: return AOp::QDAdd; default: return AOp::QDSub; }
      case 0x7: return (op == 1) ? AOp::Bkpt : AOp::Undefined;
      case 0x8: case 0xA: case 0xC: case 0xE:    // 1xy0
        switch (op) { case 0: return AOp::SmlaXY;
                      case 1: return (lo & 2) ? AOp::SmulwY : AOp::SmlawY;
                      case 2: return AOp::SmlalXY; default: return AOp::SmulXY; }
      default: return AOp::Undefined;
      }
    }
    return (lo & 1) ? AOp::DpRegShift : AOp::DpImmShift;
  }
  case 1:
    if (((hi >> 3) & 3) == 2 && !bit20)
      return ((hi >> 1) & 1) ? AOp::MsrImm : AOp::Undefined;
    return AOp::DpImm;
  case 2: return AOp::LdrStrImm;
  case 3: return (lo & 1) ? AOp::Undefined : AOp::LdrStrReg;
  case 4: return bit20 ? AOp::Ldm : AOp::Stm;
  case 5: return (hi & 0x10) ? AOp::Bl : AOp::B;   // cond==0xF handled at execute (BLX)
  case 6: return bit20 ? AOp::Ldc : AOp::Stc;
  default:
    if (hi & 0x10) return AOp::Swi;
    if (!(lo & 1)) return AOp::Cdp;
    return bit20 ? AOp::Mrc : AOp::Mcr;
  }
}

constexpr TOp classify_thumb(u32 idx) {
  const u32 i = idx << 6;                       // reconstruct bits 15:6
  switch (i >> 13) {
  case 0:
    if (((i >> 11) & 3) == 3) return (i & (1 << 10)) ? TOp::AddSubImm3 : TOp::AddSubReg;
    return TOp::ShiftImm;
  case 1: return TOp::MovCmpAddSubImm8;
  case 2:
    switch ((i >> 10) & 7) {
    case 0: return TOp::Alu;
    case 1: return (((i >> 8) & 3) == 3) ? TOp::BxBlx : TOp::HiRegOp;
    case 2: case 3: return TOp::LdrPcRel;
    default: return TOp::LdrStrReg;
    }
  case 3: return TOp::LdrStrImm5;
  case 4: return (i & (1 << 12)) ? TOp::LdrStrSpRel : TOp::LdrStrHImm5;
  case 5:
    if (!(i & (1 << 12))) return TOp::AddPcSp;
    switch ((i >> 8) & 0xF) {
    case 0x0: return TOp::AdjustSp;
    case 0x4: case 0x5: case 0xC: case 0xD: return TOp::PushPop;
    case 0xE: return TOp::Bkpt;
    default: return TOp::Undefined;
    }
  case 6:
    if (!(i & (1 << 12))) return TOp::StmLdm;
    switch ((i >> 8) & 0xF) { case 0xF: return TOp::Swi; case 0xE: return TOp::Undefined; default: return TOp::BCond; }
  default:
    switch ((i >> 11) & 3) {
    case 0: return TOp::B;
    case 1: return TOp::BlxSuffix;
    case 2: return TOp::BlPrefix;
    default: return TOp::BlSuffix;
    }
  }
}

namespace detail {
constexpr std::array<AOp, 4096> build_arm() {
  std::array<AOp, 4096> t{};
  for (u32 i = 0; i < 4096; ++i) t[i] = classify_arm(i);
  return t;
}
constexpr std::array<TOp, 1024> build_thumb() {
  std::array<TOp, 1024> t{};
  for (u32 i = 0; i < 1024; ++i) t[i] = classify_thumb(i);
  return t;
}
} // namespace detail

inline constexpr std::array<AOp, 4096> ARM_TABLE   = detail::build_arm();
inline constexpr std::array<TOp, 1024> THUMB_TABLE = detail::build_thumb();

inline AOp decode_arm(u32 instr)   { return ARM_TABLE[arm_index(instr)]; }
inline TOp decode_thumb(u16 instr) { return THUMB_TABLE[thumb_index(instr)]; }

constexpr inline bool check_condition(u32 cond, u32 cpsr) {
  const bool n = cpsr & 0x80000000, z = cpsr & 0x40000000, c = cpsr & 0x20000000, v = cpsr & 0x10000000;
  switch (cond) {
  case 0x0: return z;          case 0x1: return !z;
  case 0x2: return c;          case 0x3: return !c;
  case 0x4: return n;          case 0x5: return !n;
  case 0x6: return v;          case 0x7: return !v;
  case 0x8: return c && !z;    case 0x9: return !c || z;
  case 0xA: return n == v;     case 0xB: return n != v;
  case 0xC: return !z && n == v; case 0xD: return z || n != v;
  default:  return true;       // AL, and NV (0xF) is handled by the caller
  }
}

} // namespace ds::arm
