// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Thumb-state executor (Thumb v2 on the ARM9: BLX, BKPT). Each handler
// charges its cycles where melonDS does.
#include "core/cpu/interp/interp_internal.h"

namespace ds::interp {

using arm::TOp;

namespace {
inline u32& R(CpuContext& cpu, u32 n) { return cpu.hot.regs[n]; }
inline bool is_arm9(const CpuContext& cpu) { return cpu.which == Cpu::ARM9; }
}

void exec_thumb(CpuContext& cpu, u16 instr) {
  if (g_fast_timing) cpu.fast_d = static_cast<u8>(fast_data(cpu, instr, true));
  const TOp op = arm::decode_thumb(instr);
  switch (op) {
  case TOp::ShiftImm: {
    const u32 type = (instr >> 11) & 3, amt = (instr >> 6) & 0x1F, rs = (instr >> 3) & 7, rd = instr & 7;
    bool c = carry(cpu);
    u32 r = shift_imm(type, amt, R(cpu, rs), c);
    R(cpu, rd) = r; set_nzc(cpu, r, c);
    charge_C(cpu);
    return;
  }
  case TOp::AddSubReg: {
    const u32 rn = (instr >> 6) & 7, rs = (instr >> 3) & 7, rd = instr & 7;
    R(cpu, rd) = (instr & (1 << 9)) ? sub_flags(cpu, R(cpu, rs), R(cpu, rn), 1) : add_flags(cpu, R(cpu, rs), R(cpu, rn), 0);
    charge_C(cpu);
    return;
  }
  case TOp::AddSubImm3: {
    const u32 imm = (instr >> 6) & 7, rs = (instr >> 3) & 7, rd = instr & 7;
    R(cpu, rd) = (instr & (1 << 9)) ? sub_flags(cpu, R(cpu, rs), imm, 1) : add_flags(cpu, R(cpu, rs), imm, 0);
    charge_C(cpu);
    return;
  }
  case TOp::MovCmpAddSubImm8: {
    const u32 rd = (instr >> 8) & 7, imm = instr & 0xFF;
    switch ((instr >> 11) & 3) {
    case 0: R(cpu, rd) = imm; set_nz(cpu, imm); break;
    case 1: sub_flags(cpu, R(cpu, rd), imm, 1); break;
    case 2: R(cpu, rd) = add_flags(cpu, R(cpu, rd), imm, 0); break;
    default: R(cpu, rd) = sub_flags(cpu, R(cpu, rd), imm, 1); break;
    }
    charge_C(cpu);
    return;
  }
  case TOp::Alu: {
    const u32 rs = (instr >> 3) & 7, rd = instr & 7;
    u32 a = R(cpu, rd), b = R(cpu, rs), r;
    bool c = carry(cpu);
    switch ((instr >> 6) & 0xF) {
    case 0x0: r = a & b; R(cpu, rd) = r; set_nz(cpu, r); break;                 // AND
    case 0x1: r = a ^ b; R(cpu, rd) = r; set_nz(cpu, r); break;                 // EOR
    case 0x2: r = shift_reg(0, b, a, c); R(cpu, rd) = r; set_nzc(cpu, r, c); charge_CI(cpu, 1); return;   // LSL
    case 0x3: r = shift_reg(1, b, a, c); R(cpu, rd) = r; set_nzc(cpu, r, c); charge_CI(cpu, 1); return;   // LSR
    case 0x4: r = shift_reg(2, b, a, c); R(cpu, rd) = r; set_nzc(cpu, r, c); charge_CI(cpu, 1); return;   // ASR
    case 0x5: R(cpu, rd) = add_flags(cpu, a, b, c); break;                      // ADC
    case 0x6: R(cpu, rd) = sub_flags(cpu, a, b, c); break;                      // SBC
    case 0x7: r = shift_reg(3, b, a, c); R(cpu, rd) = r; set_nzc(cpu, r, c); charge_CI(cpu, 1); return;   // ROR
    case 0x8: r = a & b; set_nz(cpu, r); break;                                 // TST
    case 0x9: R(cpu, rd) = sub_flags(cpu, 0, b, 1); break;                      // NEG
    case 0xA: sub_flags(cpu, a, b, 1); break;                                   // CMP
    case 0xB: add_flags(cpu, a, b, 0); break;                                   // CMN
    case 0xC: r = a | b; R(cpu, rd) = r; set_nz(cpu, r); break;                 // ORR
    case 0xD: {                                                                 // MUL
      r = a * b; R(cpu, rd) = r; set_nz(cpu, r);
      if (is_arm9(cpu)) { charge_CI(cpu, 3); return; }
      cpu.hot.cpsr &= ~FLAG_C;                                                  // ARM7: carry destroyed
      u32 k; if ((a & 0xFFFFFF00) == 0 || (a & 0xFFFFFF00) == 0xFFFFFF00) k = 1; else if ((a & 0xFFFF0000) == 0 || (a & 0xFFFF0000) == 0xFFFF0000) k = 2; else if ((a & 0xFF000000) == 0 || (a & 0xFF000000) == 0xFF000000) k = 3; else k = 4;
      charge_CI(cpu, k);
      return;
    }
    case 0xE: r = a & ~b; R(cpu, rd) = r; set_nz(cpu, r); break;                // BIC
    default:  r = ~b; R(cpu, rd) = r; set_nz(cpu, r); break;                    // MVN
    }
    charge_C(cpu);
    return;
  }
  case TOp::HiRegOp: {
    const u32 rd = (instr & 7) | ((instr >> 4) & 8), rs = (instr >> 3) & 0xF;
    const u32 b = R(cpu, rs);
    charge_C(cpu);                          // before the jump
    switch ((instr >> 8) & 3) {
    case 0:
      if (rd == 15) { cpu.jump(R(cpu, 15) + b, false); return; }
      R(cpu, rd) += b; return;
    case 1: sub_flags(cpu, R(cpu, rd), b, 1); return;
    case 2:
      if (rd == 15) { cpu.jump(b, false); return; }
      R(cpu, rd) = b; return;
    default: return;
    }
  }
  case TOp::BxBlx: {
    const u32 rs = (instr >> 3) & 0xF;
    u32 target = R(cpu, rs);
    if (instr & (1 << 7)) {                 // BLX (v5)
      if (!is_arm9(cpu)) break;
      R(cpu, 14) = (R(cpu, 15) - 2) | 1;
    }
    cpu.jump(target, true);
    return;
  }
  case TOp::LdrPcRel: {
    const u32 rd = (instr >> 8) & 7, addr = (R(cpu, 15) & ~3u) + ((instr & 0xFF) << 2);
    R(cpu, rd) = mem_read32(cpu, addr);
    charge_CDI(cpu);
    return;
  }
  case TOp::LdrStrReg: {
    const u32 ro = (instr >> 6) & 7, rb = (instr >> 3) & 7, rd = instr & 7;
    const u32 ea = R(cpu, rb) + R(cpu, ro);
    switch ((instr >> 9) & 7) {
    case 0: mem_write32(cpu, ea, R(cpu, rd)); charge_CD(cpu); return;                         // STR
    case 1: mem_write16(cpu, ea, static_cast<u16>(R(cpu, rd))); charge_CD(cpu); return;      // STRH
    case 2: mem_write8(cpu, ea, static_cast<u8>(R(cpu, rd))); charge_CD(cpu); return;        // STRB
    case 3: R(cpu, rd) = static_cast<u32>(static_cast<s32>(static_cast<s8>(mem_read8(cpu, ea)))); charge_CDI(cpu); return;  // LDRSB
    case 4: R(cpu, rd) = rotr32(mem_read32(cpu, ea), (ea & 3) * 8); charge_CDI(cpu); return;  // LDR
    case 5: { u32 v = mem_read16(cpu, ea); if (!is_arm9(cpu) && (ea & 1)) v = rotr32(v, 8); R(cpu, rd) = v; charge_CDI(cpu); return; } // LDRH
    case 6: R(cpu, rd) = mem_read8(cpu, ea); charge_CDI(cpu); return;                         // LDRB
    default:                                                                    // LDRSH
      if (!is_arm9(cpu) && (ea & 1)) R(cpu, rd) = static_cast<u32>(static_cast<s32>(static_cast<s8>(mem_read8(cpu, ea))));
      else R(cpu, rd) = static_cast<u32>(static_cast<s32>(static_cast<s16>(mem_read16(cpu, ea))));
      charge_CDI(cpu);
      return;
    }
  }
  case TOp::LdrStrImm5: {
    const u32 rb = (instr >> 3) & 7, rd = instr & 7, imm = (instr >> 6) & 0x1F;
    const bool l = instr & (1 << 11), b = instr & (1 << 12);
    if (b) {
      const u32 ea = R(cpu, rb) + imm;
      if (l) { R(cpu, rd) = mem_read8(cpu, ea); charge_CDI(cpu); return; }
      mem_write8(cpu, ea, static_cast<u8>(R(cpu, rd))); charge_CD(cpu); return;
    }
    const u32 ea = R(cpu, rb) + imm * 4;
    if (l) { R(cpu, rd) = rotr32(mem_read32(cpu, ea), (ea & 3) * 8); charge_CDI(cpu); return; }
    mem_write32(cpu, ea, R(cpu, rd)); charge_CD(cpu); return;
  }
  case TOp::LdrStrHImm5: {
    const u32 rb = (instr >> 3) & 7, rd = instr & 7, ea = R(cpu, rb) + ((instr >> 6) & 0x1F) * 2;
    if (instr & (1 << 11)) { u32 v = mem_read16(cpu, ea); if (!is_arm9(cpu) && (ea & 1)) v = rotr32(v, 8); R(cpu, rd) = v; charge_CDI(cpu); return; }
    mem_write16(cpu, ea, static_cast<u16>(R(cpu, rd))); charge_CD(cpu); return;
  }
  case TOp::LdrStrSpRel: {
    const u32 rd = (instr >> 8) & 7, ea = R(cpu, 13) + (instr & 0xFF) * 4;
    if (instr & (1 << 11)) { R(cpu, rd) = rotr32(mem_read32(cpu, ea), (ea & 3) * 8); charge_CDI(cpu); return; }
    mem_write32(cpu, ea, R(cpu, rd)); charge_CD(cpu); return;
  }
  case TOp::AddPcSp: {
    const u32 rd = (instr >> 8) & 7, imm = (instr & 0xFF) * 4;
    R(cpu, rd) = (instr & (1 << 11)) ? R(cpu, 13) + imm : (R(cpu, 15) & ~3u) + imm;
    charge_C(cpu);
    return;
  }
  case TOp::AdjustSp: {
    const u32 imm = (instr & 0x7F) * 4;
    if (instr & (1 << 7)) R(cpu, 13) -= imm; else R(cpu, 13) += imm;
    charge_C(cpu);
    return;
  }
  case TOp::PushPop: {
    u32 list = instr & 0xFF;
    const bool pop = instr & (1 << 11), r = instr & (1 << 8);
    u32 n = popcount16(list) + (r ? 1 : 0);
    if (pop) {
      u32 addr = R(cpu, 13);
      bool first = true;
      for (u32 i = 0; i < 8; ++i) if (list & (1u << i)) { R(cpu, i) = mem_read32(cpu, addr, !first); first = false; addr += 4; }
      if (r) {
        u32 pc = mem_read32(cpu, addr, !first); addr += 4;
        R(cpu, 13) = addr;
        cpu.jump(pc, is_arm9(cpu));
        charge_CDI_after_jump(cpu);         // after the jump
        return;
      }
      R(cpu, 13) = addr;
      charge_CDI(cpu);
      return;
    }
    u32 addr = R(cpu, 13) - n * 4;
    R(cpu, 13) = addr;
    bool first = true;
    for (u32 i = 0; i < 8; ++i) if (list & (1u << i)) { mem_write32(cpu, addr, R(cpu, i), !first); first = false; addr += 4; }
    if (r) mem_write32(cpu, addr, R(cpu, 14), !first);
    charge_CD(cpu);
    return;
  }
  case TOp::StmLdm: {
    const u32 rb = (instr >> 8) & 7;
    u32 list = instr & 0xFF;
    const bool load = instr & (1 << 11);
    u32 addr = R(cpu, rb);
    if (list == 0) {                        // empty list: r15, base += 0x40
      if (load) { cpu.jump(mem_read32(cpu, addr), false); R(cpu, rb) = addr + 0x40; charge_CDI_after_jump(cpu); return; }
      mem_write32(cpu, addr, R(cpu, 15) + 2);
      R(cpu, rb) = addr + 0x40;
      charge_CD(cpu);
      return;
    }
    u32 n = popcount16(list);
    if (load) {
      bool first = true;
      for (u32 i = 0; i < 8; ++i) if (list & (1u << i)) { R(cpu, i) = mem_read32(cpu, addr, !first); first = false; addr += 4; }
      if (!(list & (1u << rb))) R(cpu, rb) = addr;
      charge_CDI(cpu);
    } else {
      const u32 wb = addr + n * 4;
      bool first = true;
      for (u32 i = 0; i < 8; ++i) if (list & (1u << i)) {
        mem_write32(cpu, addr, (i == rb && !first) ? wb : R(cpu, i), !first); addr += 4; first = false;
      }
      R(cpu, rb) = wb;
      charge_CD(cpu);
    }
    return;
  }
  case TOp::BCond: {
    const u32 cond = (instr >> 8) & 0xF;
    if (!arm::check_condition(cond, cpu.hot.cpsr)) { charge_C(cpu); return; }
    s32 off = static_cast<s8>(instr & 0xFF) * 2;
    cpu.jump(R(cpu, 15) + static_cast<u32>(off), false);
    return;
  }
  case TOp::Swi:
    if ((cpu.nds->dsi_font_hle || cpu.nds->dsi_loader_watch) && cpu.nds->dsi_hle_swi(cpu, instr & 0xFF)) return;
    cpu.raise_exception(CpuContext::Exception::Swi); return;
  case TOp::Bkpt:
    if (!is_arm9(cpu)) break;
    cpu.raise_exception(CpuContext::Exception::PrefetchAbort); return;
  case TOp::B: {
    s32 off = (static_cast<s32>(static_cast<u32>(instr) << 21) >> 20);
    cpu.jump(R(cpu, 15) + static_cast<u32>(off), false);
    return;
  }
  case TOp::BlPrefix: {
    s32 off = (static_cast<s32>(static_cast<u32>(instr) << 21) >> 9);
    R(cpu, 14) = R(cpu, 15) + static_cast<u32>(off);
    charge_C(cpu);
    return;
  }
  case TOp::BlSuffix: case TOp::BlxSuffix: {
    if (op == TOp::BlxSuffix && !is_arm9(cpu)) break;
    const u32 ret = (R(cpu, 15) - 2) | 1;
    u32 target = R(cpu, 14) + ((instr & 0x7FF) << 1);
    R(cpu, 14) = ret;
    if (op == TOp::BlxSuffix) { cpu.jump(target & ~3u, true); }   // bit0 clear => ARM
    else cpu.jump(target | 1, true);
    return;
  }
  case TOp::Undefined: break;
  }
  cpu.raise_exception(CpuContext::Exception::Undefined);   // the vector jump charges the refill
}

} // namespace ds::interp
