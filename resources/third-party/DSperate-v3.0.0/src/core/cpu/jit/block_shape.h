// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Where a block ends. Shared by every backend on purpose: block boundaries
// decide where the budget is tested, so two backends that cut blocks
// identically produce identical slice interleaving.
#pragma once
#include "core/cpu/arm_decode.h"
#include "core/types.h"

namespace ds::jit::shape {

// CP15 writes the core accepts and ignores (cp15.cpp): every c7 cache /
// write-buffer operation except wait-for-interrupt (c7,c0,4 and c7,c8,2).
inline bool mcr_is_nop(u32 instr, bool a9) {
  if (!a9 || ((instr >> 8) & 0xF) != 15) return false;
  const u32 crn = (instr >> 16) & 0xF, crm = instr & 0xF, opc2 = (instr >> 5) & 7;
  return crn == 7 && !((crm == 0 && opc2 == 4) || (crm == 8 && opc2 == 2));
}
// SUBS/ADDS/MOVS pc, rn, #imm with S set: exception return, inlined as one
// restore_cpsr + interworking branch. Register-operand forms stay interpreted.
inline bool dp_exc_return(u32 instr) {
  if ((instr >> 28) != 0xE) return false;                       // unconditional only
  if (arm::decode_arm(instr) != arm::AOp::DpImm) return false;
  if (!(instr & (1u << 20))) return false;                      // S
  if (((instr >> 12) & 0xF) != 15) return false;                // rd == pc
  const u32 opc = (instr >> 21) & 0xF;
  return opc == 0x2 || opc == 0x4 || opc == 0xD;                // SUB / ADD / MOV
}

inline bool msr_inline(u32 instr) {
  // SPSR writes inline too (one word, no bank/mode change); USR/SYS (no SPSR)
  // checked at run time.
  return !(arm::decode_arm(instr) == arm::AOp::MsrReg && (instr & 0xF) == 15);
}

// `LDM ^` / `STM ^` (S bit, no r15 in load list): user-bank transfer, inlined
// where the user bank is reachable without a mode switch (IRQ/SVC/ABT/UND;
// FIQ and USR/SYS keep the interpreter). Writeback stays interpreted:
// UNPREDICTABLE with S, and r13/r14 could land in the wrong bank.
inline bool ldm_user_inline(u32 instr) {
  using arm::AOp;
  const AOp op = arm::decode_arm(instr);
  if (op != AOp::Ldm && op != AOp::Stm) return false;
  if (!(instr & (1u << 22))) return false;                      // S
  if (instr & (1u << 21)) return false;                         // writeback
  const u32 list = instr & 0xFFFF;
  if (list == 0 || ((instr >> 16) & 0xF) == 15) return false;
  const bool load = instr & (1u << 20);
  return !(load && (list & 0x8000));                            // else: CPSR restore
}

inline bool arm_ends_block(u32 instr, bool a9) {
  using arm::AOp;
  const u32 cond = instr >> 28;
  if (cond == 0xF) return true;
  switch (arm::decode_arm(instr)) {
  case AOp::B: case AOp::Bl: case AOp::Bx: case AOp::BlxReg: case AOp::Swi: case AOp::Bkpt: case AOp::Undefined:
  case AOp::Cdp: case AOp::Ldc: case AOp::Stc: case AOp::Mrc:
    return true;
  case AOp::Mcr: return !mcr_is_nop(instr, a9);
  case AOp::MsrReg: case AOp::MsrImm: return !msr_inline(instr);
  case AOp::DpImm: case AOp::DpImmShift: case AOp::DpRegShift: {
    const u32 opcode = (instr >> 21) & 0xF;
    const bool test = opcode >= 8 && opcode <= 0xB;
    return !test && ((instr >> 12) & 0xF) == 15;
  }
  case AOp::LdrStrImm: case AOp::LdrStrReg: case AOp::LdrStrHImm: case AOp::LdrStrHReg:
    return (instr & (1u << 20)) && ((instr >> 12) & 0xF) == 15;
  case AOp::Ldm: return (instr & 0x8000) != 0;
  default: return false;
  }
}

inline bool thumb_ends_block(u16 instr) {
  using arm::TOp;
  switch (arm::decode_thumb(instr)) {
  case TOp::HiRegOp: {
    const u32 rd = (instr & 7) | ((instr >> 4) & 8), op = (instr >> 8) & 3;
    return rd == 15 && (op == 0 || op == 2);
  }
  case TOp::BxBlx: case TOp::BCond: case TOp::Swi: case TOp::B: case TOp::BlSuffix: case TOp::BlxSuffix:
  case TOp::Bkpt: case TOp::Undefined:
    return true;
  case TOp::PushPop: return (instr & (1 << 11)) && (instr & (1 << 8));
  case TOp::StmLdm: return (instr & 0xFF) == 0;
  default: return false;
  }
}

} // namespace ds::jit::shape
