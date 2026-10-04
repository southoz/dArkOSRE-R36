// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// ARM-state executor (ARMv5TE). melonDS is the behavioural reference for
// DS-specific corners, including where each instruction charges its cycles.
#include "core/cpu/interp/interp_internal.h"
#include "core/cpu/cp15.h"

namespace ds::interp {

using arm::AOp;

namespace {

inline u32& R(CpuContext& cpu, u32 n) { return cpu.hot.regs[n]; }
inline bool is_arm9(const CpuContext& cpu) { return cpu.which == Cpu::ARM9; }

// ---- data processing ----------------------------------------------------
// `op2` and shifter carry `sc` already computed; `internal` is 1 for the
// register-shift forms. Charges before a jump to r15 (melonDS order).
void data_processing(CpuContext& cpu, u32 instr, u32 op2, bool sc, u32 rn_val, u32 internal) {
  const u32 opcode = (instr >> 21) & 0xF;
  const bool s = instr & (1u << 20);
  const u32 rd = (instr >> 12) & 0xF;
  u32 res = 0;
  bool write = true;
  const bool c = carry(cpu);

  if (s && rd != 15) {
    switch (opcode) {
    case 0x0: res = rn_val & op2;  set_nzc(cpu, res, sc); break;               // AND
    case 0x1: res = rn_val ^ op2;  set_nzc(cpu, res, sc); break;               // EOR
    case 0x2: res = sub_flags(cpu, rn_val, op2, 1); break;                     // SUB
    case 0x3: res = sub_flags(cpu, op2, rn_val, 1); break;                     // RSB
    case 0x4: res = add_flags(cpu, rn_val, op2, 0); break;                     // ADD
    case 0x5: res = add_flags(cpu, rn_val, op2, c); break;                     // ADC
    case 0x6: res = sub_flags(cpu, rn_val, op2, c); break;                     // SBC
    case 0x7: res = sub_flags(cpu, op2, rn_val, c); break;                     // RSC
    case 0x8: res = rn_val & op2;  set_nzc(cpu, res, sc); write = false; break; // TST
    case 0x9: res = rn_val ^ op2;  set_nzc(cpu, res, sc); write = false; break; // TEQ
    case 0xA: sub_flags(cpu, rn_val, op2, 1); write = false; break;            // CMP
    case 0xB: add_flags(cpu, rn_val, op2, 0); write = false; break;            // CMN
    case 0xC: res = rn_val | op2;  set_nzc(cpu, res, sc); break;               // ORR
    case 0xD: res = op2;           set_nzc(cpu, res, sc); break;               // MOV
    case 0xE: res = rn_val & ~op2; set_nzc(cpu, res, sc); break;               // BIC
    default:  res = ~op2;          set_nzc(cpu, res, sc); break;               // MVN
    }
  } else {
    switch (opcode) {
    case 0x0: res = rn_val & op2; break;
    case 0x1: res = rn_val ^ op2; break;
    case 0x2: res = rn_val - op2; break;
    case 0x3: res = op2 - rn_val; break;
    case 0x4: res = rn_val + op2; break;
    case 0x5: res = rn_val + op2 + c; break;
    case 0x6: res = rn_val - op2 - !c; break;
    case 0x7: res = op2 - rn_val - !c; break;
    case 0x8: case 0x9: case 0xA: case 0xB: write = false; break;   // S=0 forms of these are MRS/MSR etc, never reach here
    case 0xC: res = rn_val | op2; break;
    case 0xD: res = op2; break;
    case 0xE: res = rn_val & ~op2; break;
    default:  res = ~op2; break;
    }
  }

  if (internal) charge_CI(cpu, internal); else charge_C(cpu);
  if (!write) return;
  if (rd == 15) {
    if (s) cpu.restore_cpsr();            // e.g. MOVS pc, lr / SUBS pc, lr, #4
    cpu.jump(res, false);
    return;
  }
  R(cpu, rd) = res;
}

// ---- load/store address computation -------------------------------------
// Returns effective address; performs base writeback per P/U/W bits.
inline u32 ls_address(CpuContext& cpu, u32 instr, u32 offset, bool& writeback_after, u32& wb_value) {
  const bool p = instr & (1u << 24), u = instr & (1u << 23), w = instr & (1u << 21);
  const u32 rn = (instr >> 16) & 0xF;
  u32 base = R(cpu, rn);
  u32 off_addr = u ? base + offset : base - offset;
  u32 ea = p ? off_addr : base;
  writeback_after = (!p || w);
  wb_value = off_addr;
  return ea;
}

void ldr_str(CpuContext& cpu, u32 instr, u32 offset) {
  const bool l = instr & (1u << 20), b = instr & (1u << 22);
  const u32 rn = (instr >> 16) & 0xF, rd = (instr >> 12) & 0xF;
  bool wb; u32 wbv;
  const u32 ea = ls_address(cpu, instr, offset, wb, wbv);
  if (l) {
    u32 v = b ? mem_read8(cpu, ea) : rotr32(mem_read32(cpu, ea), (ea & 3) * 8);
    if (wb) R(cpu, rn) = wbv;       // writeback before load-to-PC check; Rd==Rn: loaded value wins
    charge_CDI(cpu);                // before the jump
    if (rd == 15) { cpu.jump(v, is_arm9(cpu)); return; }
    R(cpu, rd) = v;
    return;
  }
  u32 v = R(cpu, rd);
  if (rd == 15) v += 4;             // STR pc stores address + 12
  if (b) mem_write8(cpu, ea, static_cast<u8>(v)); else mem_write32(cpu, ea, v);
  if (wb) R(cpu, rn) = wbv;
  charge_CD(cpu);
}

void ldr_str_h(CpuContext& cpu, u32 instr, u32 offset) {
  const bool l = instr & (1u << 20);
  const u32 rn = (instr >> 16) & 0xF, rd = (instr >> 12) & 0xF;
  const u32 sh = (instr >> 5) & 3;   // 1=H, 2=SB, 3=SH
  bool wb; u32 wbv;
  const u32 ea = ls_address(cpu, instr, offset, wb, wbv);
  if (l) {
    u32 v;
    switch (sh) {
    case 1: v = mem_read16(cpu, ea); if (!is_arm9(cpu) && (ea & 1)) v = rotr32(v, 8); break;
    case 2: v = static_cast<u32>(static_cast<s32>(static_cast<s8>(mem_read8(cpu, ea)))); break;
    default:
      if (!is_arm9(cpu) && (ea & 1)) v = static_cast<u32>(static_cast<s32>(static_cast<s8>(mem_read8(cpu, ea))));
      else v = static_cast<u32>(static_cast<s32>(static_cast<s16>(mem_read16(cpu, ea))));
      break;
    }
    if (wb) R(cpu, rn) = wbv;
    charge_CDI(cpu);
    if (rd == 15) { cpu.jump(v, false); return; }
    R(cpu, rd) = v;
    return;
  }
  // store: sh==1 STRH; sh==2 LDRD (v5); sh==3 STRD (v5)
  if (sh == 1) {
    mem_write16(cpu, ea, static_cast<u16>(R(cpu, rd)));
    if (wb) R(cpu, rn) = wbv;
    charge_CD(cpu);
    return;
  }
  if (!is_arm9(cpu)) { charge_C(cpu); return; }     // unpredictable on ARM7; ignore
  if (sh == 2) {                    // LDRD rd, rd+1
    u32 lo = mem_read32(cpu, ea), hi = mem_read32(cpu, ea + 4, true);
    if (wb) R(cpu, rn) = wbv;
    R(cpu, rd) = lo; R(cpu, (rd + 1) & 0xF) = hi;
    charge_CDI(cpu);
    return;
  }
  mem_write32(cpu, ea, R(cpu, rd));          // STRD
  mem_write32(cpu, ea + 4, R(cpu, (rd + 1) & 0xF), true);
  if (wb) R(cpu, rn) = wbv;
  charge_CD(cpu);
}

void ldm_stm(CpuContext& cpu, u32 instr, bool load) {
  const bool p = instr & (1u << 24), u = instr & (1u << 23), s = instr & (1u << 22), w = instr & (1u << 21);
  const u32 rn = (instr >> 16) & 0xF;
  u32 list = instr & 0xFFFF;
  u32 n = popcount16(list);
  if (n == 0) { list = 0x8000; n = 16; }   // empty list: r15, 64 bytes (ARMv4 quirk)
  const u32 base = R(cpu, rn);
  u32 addr, wb;
  if (u) { addr = p ? base + 4 : base; wb = base + n * 4; }
  else   { addr = p ? base - n * 4 : base - n * 4 + 4; wb = base - n * 4; }
  addr &= ~3u;

  // S bit without r15 in a load list = user-bank transfer.
  const bool user_bank = s && !(load && (list & 0x8000));
  u32 saved_mode = 0;
  if (user_bank && (cpu.hot.cpsr & 0x1F) != 0x10) { saved_mode = cpu.hot.cpsr & 0x1F; cpu.switch_mode(0x10); }

  bool first = true;
  if (load) {
    bool pc_loaded = false; u32 pc_val = 0;
    for (u32 i = 0; i < 16; ++i) {
      if (!(list & (1u << i))) continue;
      u32 v = mem_read32(cpu, addr, !first); first = false; addr += 4;
      if (i == 15) { pc_loaded = true; pc_val = v; } else R(cpu, i) = v;
    }
    if (w && !(list & (1u << rn))) R(cpu, rn) = wb;   // Rn in list: loaded value wins
    if (user_bank && saved_mode) cpu.switch_mode(saved_mode);
    if (pc_loaded) {
      if (s) cpu.restore_cpsr();
      cpu.jump(pc_val, is_arm9(cpu) && !s);
      charge_CDI_after_jump(cpu);   // after the jump: new pc, state, code region
    } else charge_CDI(cpu);
  } else {
    for (u32 i = 0; i < 16; ++i) {
      if (!(list & (1u << i))) continue;
      u32 v = R(cpu, i);
      if (i == 15) v += 4;
      // STM Rn-in-list: first reg stores original base, later ones the writeback value.
      if (i == rn && !first && w) v = wb;
      mem_write32(cpu, addr, v, !first); addr += 4;
      first = false;
    }
    if (user_bank && saved_mode) cpu.switch_mode(saved_mode);
    if (w) R(cpu, rn) = wb;
    charge_CD(cpu);
  }
}

void msr(CpuContext& cpu, u32 instr, u32 value) {
  const bool spsr = instr & (1u << 22);
  const u32 fields = (instr >> 16) & 0xF;
  u32 mask = 0;
  if (fields & 1) mask |= 0x000000FF;
  if (fields & 2) mask |= 0x0000FF00;
  if (fields & 4) mask |= 0x00FF0000;
  if (fields & 8) mask |= 0xFF000000;
  charge_C(cpu);
  if (spsr) {
    if ((cpu.hot.cpsr & 0x1F) != 0x10 && (cpu.hot.cpsr & 0x1F) != 0x1F)
      cpu.hot.spsr = (cpu.hot.spsr & ~mask) | (value & mask);
    return;
  }
  if ((cpu.hot.cpsr & 0x1F) == 0x10) mask &= 0xFF000000;   // user mode: flags only
  mask &= ~0x00000020u;                                    // T bit not writable via MSR
  const u32 nv = (cpu.hot.cpsr & ~mask) | (value & mask);
  cpu.set_cpsr(nv);
}

inline s32 half(u32 v, bool top) { return static_cast<s16>(top ? (v >> 16) : (v & 0xFFFF)); }

// Internal cycles of a multiply: the ARM9 takes a fixed count (3 when setting
// flags, else 1); the ARM7 depends on the magnitude of the second operand,
// plus one for an accumulate / long form.
inline u32 mul_cycles(CpuContext& cpu, bool s, u32 rs, bool signed_op, u32 extra) {
  if (cpu.which == Cpu::ARM9) return s ? 3 : 1;
  u32 c;
  auto top = [&](u32 mask) { return (rs & mask) == 0 || (signed_op && (rs & mask) == mask); };
  if (top(0xFFFFFF00)) c = 1; else if (top(0xFFFF0000)) c = 2; else if (top(0xFF000000)) c = 3; else c = 4;
  return c + extra;
}

void undefined(CpuContext& cpu) { cpu.raise_exception(CpuContext::Exception::Undefined); }   // the vector jump charges the refill

} // namespace

void exec_arm(CpuContext& cpu, u32 instr) {
  if (g_fast_timing) cpu.fast_d = static_cast<u8>(fast_data(cpu, instr, false));
  const u32 cond = instr >> 28;
  if (cond == 0xF) {
    // Unconditional space: BLX imm (v5), PLD.
    if (((instr >> 25) & 7) == 5) {
      if (!is_arm9(cpu)) { undefined(cpu); return; }
      s32 off = static_cast<s32>(static_cast<u32>(instr) << 8) >> 6;
      off |= (instr >> 23) & 2;
      R(cpu, 14) = R(cpu, 15) - 4;
      cpu.jump((R(cpu, 15) + static_cast<u32>(off)) | 1, true);
      return;
    }
    if (((instr >> 24) & 0xF7) == 0x55) { charge_C(cpu); return; }   // PLD
    undefined(cpu);
    return;
  }
  if (cond != 0xE && !arm::check_condition(cond, cpu.hot.cpsr)) { charge_C(cpu); return; }

  const AOp op = arm::decode_arm(instr);
  switch (op) {
  case AOp::DpImm: {
    const u32 imm = instr & 0xFF, rot = (instr >> 8) & 0xF;
    bool sc = carry(cpu);
    u32 op2 = rotr32(imm, rot * 2);
    if (rot) sc = op2 >> 31;
    data_processing(cpu, instr, op2, sc, R(cpu, (instr >> 16) & 0xF), 0);
    return;
  }
  case AOp::DpImmShift: {
    bool sc = carry(cpu);
    u32 op2 = shift_imm((instr >> 5) & 3, (instr >> 7) & 0x1F, R(cpu, instr & 0xF), sc);
    data_processing(cpu, instr, op2, sc, R(cpu, (instr >> 16) & 0xF), 0);
    return;
  }
  case AOp::DpRegShift: {
    bool sc = carry(cpu);
    const u32 rm = instr & 0xF, rn = (instr >> 16) & 0xF;
    u32 rm_v = R(cpu, rm) + (rm == 15 ? 4 : 0);      // PC reads +12 with register shift
    u32 rn_v = R(cpu, rn) + (rn == 15 ? 4 : 0);
    u32 op2 = shift_reg((instr >> 5) & 3, R(cpu, (instr >> 8) & 0xF), rm_v, sc);
    data_processing(cpu, instr, op2, sc, rn_v, 1);
    return;
  }
  case AOp::Mrs:
    R(cpu, (instr >> 12) & 0xF) = (instr & (1u << 22)) ? cpu.hot.spsr : cpu.hot.cpsr;
    charge_C(cpu);
    return;
  case AOp::MsrReg: msr(cpu, instr, R(cpu, instr & 0xF)); return;
  case AOp::MsrImm: msr(cpu, instr, rotr32(instr & 0xFF, ((instr >> 8) & 0xF) * 2)); return;

  case AOp::B: case AOp::Bl: {
    s32 off = static_cast<s32>(static_cast<u32>(instr) << 8) >> 6;
    if (op == AOp::Bl) R(cpu, 14) = R(cpu, 15) - 4;
    cpu.jump(R(cpu, 15) + static_cast<u32>(off), false);
    return;
  }
  case AOp::Bx: cpu.jump(R(cpu, instr & 0xF), true); return;
  case AOp::BlxReg: {
    if (!is_arm9(cpu)) break;
    u32 target = R(cpu, instr & 0xF);
    R(cpu, 14) = R(cpu, 15) - 4;
    cpu.jump(target, true);
    return;
  }

  case AOp::Mul: case AOp::Mla: {
    const u32 rd = (instr >> 16) & 0xF, rn = (instr >> 12) & 0xF, rs = (instr >> 8) & 0xF, rm = instr & 0xF;
    const u32 rs_v = R(cpu, rs);
    u32 r = R(cpu, rm) * rs_v;
    if (op == AOp::Mla) r += R(cpu, rn);
    R(cpu, rd) = r;
    if (instr & (1u << 20)) {
      set_nz(cpu, r);
      if (!is_arm9(cpu)) cpu.hot.cpsr &= ~FLAG_C;   // ARM7: carry destroyed
    }
    charge_CI(cpu, mul_cycles(cpu, instr & (1u << 20), rs_v, true, op == AOp::Mla ? 1 : 0));
    return;
  }
  case AOp::Umull: case AOp::Umlal: case AOp::Smull: case AOp::Smlal: {
    const u32 rdhi = (instr >> 16) & 0xF, rdlo = (instr >> 12) & 0xF, rs = (instr >> 8) & 0xF, rm = instr & 0xF;
    const u32 rs_v = R(cpu, rs);
    u64 r;
    if (op == AOp::Umull || op == AOp::Umlal) r = u64{R(cpu, rm)} * rs_v;
    else r = static_cast<u64>(s64{static_cast<s32>(R(cpu, rm))} * static_cast<s32>(rs_v));
    if (op == AOp::Umlal || op == AOp::Smlal) r += (u64{R(cpu, rdhi)} << 32) | R(cpu, rdlo);
    R(cpu, rdlo) = static_cast<u32>(r);
    R(cpu, rdhi) = static_cast<u32>(r >> 32);
    if (instr & (1u << 20)) {
      cpu.hot.cpsr = (cpu.hot.cpsr & ~(FLAG_N | FLAG_Z)) | (static_cast<u32>(r >> 32) & FLAG_N) | (r == 0 ? FLAG_Z : 0);
      if (!is_arm9(cpu)) cpu.hot.cpsr &= ~FLAG_C;
    }
    const bool sgn = (op == AOp::Smull || op == AOp::Smlal);
    charge_CI(cpu, mul_cycles(cpu, instr & (1u << 20), rs_v, sgn, 1));
    return;
  }

  case AOp::Clz: {
    if (!is_arm9(cpu)) break;
    u32 v = R(cpu, instr & 0xF);
    R(cpu, (instr >> 12) & 0xF) = v ? static_cast<u32>(__builtin_clz(v)) : 32;
    charge_C(cpu);
    return;
  }
  case AOp::QAdd: case AOp::QSub: case AOp::QDAdd: case AOp::QDSub: {
    if (!is_arm9(cpu)) break;
    s32 m = static_cast<s32>(R(cpu, instr & 0xF)), n = static_cast<s32>(R(cpu, (instr >> 16) & 0xF));
    if (op == AOp::QDAdd || op == AOp::QDSub) n = sat_add(cpu, n, n);
    R(cpu, (instr >> 12) & 0xF) = static_cast<u32>((op == AOp::QAdd || op == AOp::QDAdd) ? sat_add(cpu, m, n) : sat_sub(cpu, m, n));
    charge_C(cpu);
    return;
  }
  case AOp::SmulXY: case AOp::SmlaXY: {
    if (!is_arm9(cpu)) break;
    const u32 rd = (instr >> 16) & 0xF, rn = (instr >> 12) & 0xF, rs = (instr >> 8) & 0xF, rm = instr & 0xF;
    s32 prod = half(R(cpu, rm), instr & (1u << 5)) * half(R(cpu, rs), instr & (1u << 6));
    if (op == AOp::SmlaXY) {
      s64 sum = s64{prod} + static_cast<s32>(R(cpu, rn));
      if (sum != static_cast<s32>(sum)) cpu.hot.cpsr |= 0x08000000;   // Q
      prod = static_cast<s32>(sum);
    }
    R(cpu, rd) = static_cast<u32>(prod);
    charge_C(cpu);
    return;
  }
  case AOp::SmulwY: case AOp::SmlawY: {
    if (!is_arm9(cpu)) break;
    const u32 rd = (instr >> 16) & 0xF, rn = (instr >> 12) & 0xF, rs = (instr >> 8) & 0xF, rm = instr & 0xF;
    s64 prod = (s64{static_cast<s32>(R(cpu, rm))} * half(R(cpu, rs), instr & (1u << 6))) >> 16;
    s32 r = static_cast<s32>(prod);
    if (op == AOp::SmlawY) {
      s64 sum = s64{r} + static_cast<s32>(R(cpu, rn));
      if (sum != static_cast<s32>(sum)) cpu.hot.cpsr |= 0x08000000;
      r = static_cast<s32>(sum);
    }
    R(cpu, rd) = static_cast<u32>(r);
    charge_C(cpu);
    return;
  }
  case AOp::SmlalXY: {
    if (!is_arm9(cpu)) break;
    const u32 rdhi = (instr >> 16) & 0xF, rdlo = (instr >> 12) & 0xF, rs = (instr >> 8) & 0xF, rm = instr & 0xF;
    s64 prod = s64{half(R(cpu, rm), instr & (1u << 5))} * half(R(cpu, rs), instr & (1u << 6));
    s64 acc = static_cast<s64>((u64{R(cpu, rdhi)} << 32) | R(cpu, rdlo)) + prod;
    R(cpu, rdlo) = static_cast<u32>(acc); R(cpu, rdhi) = static_cast<u32>(static_cast<u64>(acc) >> 32);
    charge_CI(cpu, 1);
    return;
  }

  case AOp::LdrStrImm: ldr_str(cpu, instr, instr & 0xFFF); return;
  case AOp::LdrStrReg: {
    bool sc = carry(cpu);
    u32 off = shift_imm((instr >> 5) & 3, (instr >> 7) & 0x1F, R(cpu, instr & 0xF), sc);
    ldr_str(cpu, instr, off);
    return;
  }
  case AOp::LdrStrHImm: ldr_str_h(cpu, instr, ((instr >> 4) & 0xF0) | (instr & 0xF)); return;
  case AOp::LdrStrHReg: ldr_str_h(cpu, instr, R(cpu, instr & 0xF)); return;

  case AOp::Swp: case AOp::Swpb: {
    const u32 rn = (instr >> 16) & 0xF, rd = (instr >> 12) & 0xF, rm = instr & 0xF;
    const u32 addr = R(cpu, rn);
    // Read and store both charge non-sequential; carry the read's cost by
    // hand since a non-sequential data_cost restarts the count.
    if (op == AOp::Swpb) { u8 old = mem_read8(cpu, addr); const u32 d = cpu.data_cycles; mem_write8(cpu, addr, static_cast<u8>(R(cpu, rm))); cpu.data_cycles += d; R(cpu, rd) = old; }
    else { u32 old = rotr32(mem_read32(cpu, addr), (addr & 3) * 8); const u32 d = cpu.data_cycles; mem_write32(cpu, addr, R(cpu, rm), false); cpu.data_cycles += d; R(cpu, rd) = old; }
    charge_CDI(cpu);
    return;
  }

  case AOp::Ldm: ldm_stm(cpu, instr, true); return;
  case AOp::Stm: ldm_stm(cpu, instr, false); return;

  case AOp::Swi:
    if ((cpu.nds->dsi_font_hle || cpu.nds->dsi_loader_watch) && cpu.nds->dsi_hle_swi(cpu, (instr >> 16) & 0xFF)) return;
    cpu.raise_exception(CpuContext::Exception::Swi); return;
  case AOp::Bkpt: cpu.raise_exception(CpuContext::Exception::PrefetchAbort); return;

  case AOp::Mcr: case AOp::Mrc: {
    const u32 cp = (instr >> 8) & 0xF;
    if (cp != 15 || !is_arm9(cpu)) break;
    const u32 opc1 = (instr >> 21) & 7, crn = (instr >> 16) & 0xF, rd = (instr >> 12) & 0xF,
              crm = instr & 0xF, opc2 = (instr >> 5) & 7;
    if (op == AOp::Mcr) { cp15_write(cpu, opc1, crn, crm, opc2, R(cpu, rd)); charge_CI(cpu, 2); }
    else {
      u32 v = cp15_read(cpu, opc1, crn, crm, opc2);
      if (rd == 15) cpu.hot.cpsr = (cpu.hot.cpsr & 0x0FFFFFFF) | (v & 0xF0000000);
      else R(cpu, rd) = v;
      charge_CI(cpu, 3);
    }
    return;
  }
  case AOp::Cdp: case AOp::Ldc: case AOp::Stc:
    break;                                     // no such coprocessors on the DS
  case AOp::Pld: charge_C(cpu); return;
  case AOp::BlxImm: break;                     // handled under cond==0xF
  case AOp::Undefined: break;
  }
  undefined(cpu);
}

} // namespace ds::interp
