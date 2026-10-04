// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// One-pass ARM/Thumb block translator; semantics and cycle model mirror the
// interpreter. Untranslated instructions call the interpreter via the fallback stub.
// Rare paths go to a cold section spliced after the hot code; cross-section
// branches are fixups.
//
// Per-instruction scratch (see jit_internal.h):
//   w0    loaded value / helper result / branch target
//   w1    effective address (preserved across cost arithmetic and slow path)
//   x2,x3 page-table entry / host base
//   w4-w7 temporaries; w7 = writeback value (preserved by the slow path)
#include "core/cpu/jit/jit_internal.h"
#include "core/cpu/wait_loop.h"
#include "core/cpu/jit/a64/convention.h"
#include "core/cpu/jit/a64/emit.h"
#include "core/cpu/jit/block_shape.h"
#include "core/cpu/arm_decode.h"
#include "core/cpu/cpu_cycles.h"
#include "core/cpu/timing_mode.h"
#include "core/nds.h"

#include <algorithm>
#include <cassert>
#include <initializer_list>
#include <cstring>
#include <memory>
#include <vector>

namespace ds::jit {

namespace {

using arm::AOp;
using arm::TOp;

constexpr u32 MAX_INSTRS = 64;
constexpr size_t BLOCK_LIMIT = 28u << 10;    // hot + cold bytes; keeps every cross-section tbnz in range
constexpr size_t COLD_CAP = 32u << 10;
constexpr u32 OFF_SPSR = offsetof(CpuContext, hot) + offsetof(JitHot, spsr);

// Flag bits for the liveness pass.
constexpr u32 F_N = 8, F_Z = 4, F_C = 2, F_V = 1, F_ALL = 15;

u32 cond_reads(u32 cond) {
  switch (cond) {
  case 0x0: case 0x1: return F_Z;
  case 0x2: case 0x3: return F_C;
  case 0x4: case 0x5: return F_N;
  case 0x6: case 0x7: return F_V;
  case 0x8: case 0x9: return F_C | F_Z;
  case 0xA: case 0xB: return F_N | F_V;
  case 0xC: case 0xD: return F_N | F_Z | F_V;
  default:  return 0;
  }
}

struct FlagUse { u32 reads, writes; };

// Fallback instructions read every flag in the liveness pass; keep the two in sync.
using shape::ldm_user_inline;
using shape::mcr_is_nop;
using shape::msr_inline;

// MOVS pc, rm (LSL #0): the register form of the exception return
// (`movs pc, lr` ends the BIOS SWI handlers), inlined as dp_exc_return is.
// Local to this backend: block shape does not depend on it.
bool exc_return_reg(u32 instr) {
  if ((instr >> 28) != 0xE || arm::decode_arm(instr) != AOp::DpImmShift) return false;
  if (!(instr & (1u << 20)) || ((instr >> 12) & 0xF) != 15 || ((instr >> 21) & 0xF) != 0xD) return false;
  return (instr & 0xFF0) == 0 && (instr & 0xF) != 15;
}

bool arm_needs_fallback(u32 instr, bool a9) {
  const u32 cond = instr >> 28;
  if (cond == 0xF) return !(((instr >> 25) & 7) == 5 && a9) && ((instr >> 24) & 0xF7) != 0x55;   // BLX imm (ARM9) and PLD inline
  const AOp op = arm::decode_arm(instr);
  const u32 rd = (instr >> 12) & 0xF, rn = (instr >> 16) & 0xF;
  switch (op) {
  case AOp::DpImm: case AOp::DpImmShift: case AOp::DpRegShift: {
    const u32 opcode = (instr >> 21) & 0xF;
    const bool test = opcode >= 8 && opcode <= 0xB;
    if (rd == 15 && !test && (shape::dp_exc_return(instr) || exc_return_reg(instr))) return false;
    // Other writes to pc: S=0 (jump tables, `add pc, pc, rX, lsl #2`) inline as
    // an indirect branch; S=1 restores CPSR and stays interpreted.
    return rd == 15 && !test && ((instr & (1u << 20)) || op == AOp::DpRegShift);
  }
  case AOp::Mrs: case AOp::B: case AOp::Bl: case AOp::Bx: case AOp::Pld:
    return false;
  case AOp::Mcr: return !mcr_is_nop(instr, a9);
  case AOp::MsrReg: case AOp::MsrImm: return !msr_inline(instr);
  case AOp::BlxReg: case AOp::Clz:
    return !a9;
  case AOp::Mul: case AOp::Mla:
    return rd == 15 || ((instr >> 8) & 0xF) == 15 || (instr & 0xF) == 15 || rn == 15;
  case AOp::Umull: case AOp::Umlal: case AOp::Smull: case AOp::Smlal:
    return rd == 15 || rn == 15 || ((instr >> 8) & 0xF) == 15 || (instr & 0xF) == 15;
  // v5TE, ARM9 only. Rd is bits 19:16 (`rn`), accumulate bits 15:12 (`rd`).
  case AOp::SmlaXY: case AOp::SmulXY: case AOp::SmlawY: case AOp::SmulwY:
    return !a9 || rn == 15 || ((instr >> 8) & 0xF) == 15 || (instr & 0xF) == 15 ||
           ((op == AOp::SmlaXY || op == AOp::SmlawY) && rd == 15);
  case AOp::SmlalXY:   // RdHi bits 19:16, RdLo 15:12; RdHi == RdLo stays interpreted
    return !a9 || rn == 15 || rd == 15 || rn == rd || ((instr >> 8) & 0xF) == 15 || (instr & 0xF) == 15;
  case AOp::Mrc:       // ARM9 CP15 reads: a constant or a context field (cp15_read)
    return !a9 || ((instr >> 8) & 0xF) != 15 || rd == 15;
  case AOp::LdrStrImm: case AOp::LdrStrReg: {
    const bool l = instr & (1u << 20), p = instr & (1u << 24), w = instr & (1u << 21);
    const bool b = instr & (1u << 22);
    return (l && rd == 15 && b) || (rn == 15 && (!p || w)) || (op == AOp::LdrStrReg && (instr & 0xF) == 15);
  }
  case AOp::LdrStrHImm: case AOp::LdrStrHReg: {
    const bool l = instr & (1u << 20), p = instr & (1u << 24), w = instr & (1u << 21);
    const u32 sh = (instr >> 5) & 3;
    return (!l && sh != 1) || rd == 15 || (rn == 15 && (!p || w)) || (op == AOp::LdrStrHReg && (instr & 0xF) == 15);
  }
  case AOp::Ldm: case AOp::Stm:
    return ((instr & (1u << 22)) && !ldm_user_inline(instr)) || (instr & 0xFFFF) == 0 || rn == 15;
  case AOp::Swp: case AOp::Swpb:   // fast timing only: exact's two-access CDI combine stays interpreted
    return !g_fast_timing || rd == 15 || rn == 15 || (instr & 0xF) == 15;
  default:
    return true;
  }
}

bool thumb_needs_fallback(u16 instr, bool a9) {
  switch (arm::decode_thumb(instr)) {
  case TOp::BxBlx: return (instr & (1 << 7)) && !a9;
  case TOp::BlxSuffix: return !a9;
  case TOp::PushPop: return ((instr & 0xFF) | ((instr >> 8) & 1)) == 0;
  case TOp::StmLdm: return (instr & 0xFF) == 0;
  case TOp::Swi: case TOp::Bkpt: case TOp::Undefined: return true;
  default: return false;
  }
}

FlagUse arm_flag_use(u32 instr, bool a9) {
  const u32 cond = instr >> 28;
  if (arm_needs_fallback(instr, a9)) return {F_ALL, 0};
  if (cond == 0xF) return {0, 0};
  FlagUse u{0, 0};
  const AOp op = arm::decode_arm(instr);
  const bool s = instr & (1u << 20);
  const u32 opcode = (instr >> 21) & 0xF;
  const u32 rd = (instr >> 12) & 0xF;
  switch (op) {
  case AOp::DpImm: case AOp::DpImmShift: case AOp::DpRegShift: {
    if (rd == 15 && s) { u = {F_ALL, F_ALL}; break; }
    if (opcode == 5 || opcode == 6 || opcode == 7) u.reads |= F_C;                 // ADC SBC RSC
    if (op == AOp::DpImmShift && ((instr >> 4) & 0xFF) == 0x06) u.reads |= F_C;    // RRX (ROR #0)
    if (s) {
      const bool arith = (opcode >= 2 && opcode <= 7) || opcode == 0xA || opcode == 0xB;
      if (arith) u.writes = F_ALL;
      else {
        u.writes = F_N | F_Z;
        bool carry_written;
        if (op == AOp::DpImm) carry_written = ((instr >> 8) & 0xF) != 0;
        else if (op == AOp::DpImmShift) carry_written = ((instr >> 7) & 0x1F) != 0 || ((instr >> 5) & 3) != 0;
        else { carry_written = true; u.reads |= F_C; }
        if (carry_written) u.writes |= F_C;
      }
    }
    break;
  }
  case AOp::Mul: case AOp::Mla: case AOp::Umull: case AOp::Umlal: case AOp::Smull: case AOp::Smlal:
    if (s) u.writes = F_N | F_Z;
    break;
  case AOp::B: case AOp::Bl: case AOp::Bx: case AOp::BlxReg: case AOp::Clz: case AOp::Pld:
  case AOp::LdrStrImm: case AOp::LdrStrReg: case AOp::LdrStrHImm: case AOp::LdrStrHReg:
  case AOp::Ldm: case AOp::Stm:
  case AOp::SmlaXY: case AOp::SmulXY: case AOp::SmlawY: case AOp::SmulwY: case AOp::SmlalXY: case AOp::Mrc:
    break;
  case AOp::Mrs: u.reads = F_ALL; break;
  case AOp::Mcr: break;
  case AOp::MsrReg: case AOp::MsrImm:
    u.reads = F_ALL;                                   // mode-change path syncs CPSR via the interpreter
    // Only a CPSR write with the f field writes flags; MSR SPSR must not claim them.
    if ((instr & (1u << 19)) && !(instr & (1u << 22))) u.writes = F_ALL;
    break;
  default: u = {F_ALL, F_ALL}; break;
  }
  if (cond != 0xE) { u.reads |= cond_reads(cond); u.writes = 0; }
  return u;
}

FlagUse thumb_flag_use(u16 instr, bool a9) {
  if (thumb_needs_fallback(instr, a9)) return {F_ALL, 0};
  switch (arm::decode_thumb(instr)) {
  case TOp::ShiftImm: {
    const u32 type = (instr >> 11) & 3, amt = (instr >> 6) & 0x1F;
    return {0, (type == 0 && amt == 0) ? (F_N | F_Z) : (F_N | F_Z | F_C)};
  }
  case TOp::AddSubReg: case TOp::AddSubImm3: return {0, F_ALL};
  case TOp::MovCmpAddSubImm8: return {0, ((instr >> 11) & 3) == 0 ? (F_N | F_Z) : F_ALL};
  case TOp::Alu:
    switch ((instr >> 6) & 0xF) {
    case 0x2: case 0x3: case 0x4: case 0x7: return {F_C, F_N | F_Z | F_C};
    case 0x5: case 0x6: return {F_C, F_ALL};
    case 0x9: case 0xA: case 0xB: return {0, F_ALL};
    default: return {0, F_N | F_Z};
    }
  case TOp::HiRegOp: return {0, ((instr >> 8) & 3) == 1 ? F_ALL : 0};
  case TOp::BCond: return {cond_reads((instr >> 8) & 0xF), 0};
  case TOp::Swi: case TOp::Bkpt: case TOp::Undefined: return {F_ALL, F_ALL};
  default: return {0, 0};
  }
}

// DS_JIT_CENSUS: guest register read/write masks (pc = bit 15). Approximate for
// user-bank transfers and coprocessors.
struct RegUse { u16 reads, writes; };
RegUse arm_reg_use(u32 instr) {
  const u32 rn = (instr >> 16) & 0xF, rd = (instr >> 12) & 0xF, rs = (instr >> 8) & 0xF, rm = instr & 0xF;
  auto bit = [](u32 r) { return static_cast<u16>(1u << r); };
  RegUse u{0, 0};
  if ((instr >> 28) == 0xF) return u;
  switch (arm::decode_arm(instr)) {
  case AOp::DpImm: case AOp::DpImmShift: case AOp::DpRegShift: {
    const u32 op = (instr >> 21) & 0xF;
    const bool test = op >= 8 && op <= 0xB, mov = op == 0xD || op == 0xF;
    if (!mov) u.reads |= bit(rn);
    const AOp k = arm::decode_arm(instr);
    if (k != AOp::DpImm) u.reads |= bit(rm);
    if (k == AOp::DpRegShift) u.reads |= bit(rs);
    if (!test) u.writes |= bit(rd);
    break;
  }
  case AOp::Mrs: u.writes = bit(rd); break;
  case AOp::MsrReg: u.reads = bit(rm); break;
  case AOp::Bl: u.writes = bit(14); break;
  case AOp::Bx: u.reads = bit(rm); break;
  case AOp::BlxReg: u.reads = bit(rm); u.writes = bit(14); break;
  case AOp::Mul: u.reads = bit(rm) | bit(rs); u.writes = bit(rn); break;
  case AOp::Mla: u.reads = bit(rm) | bit(rs) | bit(rd); u.writes = bit(rn); break;
  case AOp::Umull: case AOp::Smull: u.reads = bit(rm) | bit(rs); u.writes = bit(rd) | bit(rn); break;
  case AOp::Umlal: case AOp::Smlal: case AOp::SmlalXY: u.reads = bit(rm) | bit(rs) | bit(rd) | bit(rn); u.writes = bit(rd) | bit(rn); break;
  case AOp::Clz: u.reads = bit(rm); u.writes = bit(rd); break;
  case AOp::QAdd: case AOp::QSub: case AOp::QDAdd: case AOp::QDSub: u.reads = bit(rm) | bit(rn); u.writes = bit(rd); break;
  case AOp::SmlaXY: case AOp::SmlawY: u.reads = bit(rm) | bit(rs) | bit(rd); u.writes = bit(rn); break;
  case AOp::SmulXY: case AOp::SmulwY: u.reads = bit(rm) | bit(rs); u.writes = bit(rn); break;
  case AOp::LdrStrImm: case AOp::LdrStrReg: case AOp::LdrStrHImm: case AOp::LdrStrHReg: {
    const AOp k = arm::decode_arm(instr);
    const bool l = instr & (1u << 20), p = instr & (1u << 24), w = instr & (1u << 21);
    u.reads |= bit(rn);
    if (k == AOp::LdrStrReg || k == AOp::LdrStrHReg) u.reads |= bit(rm);
    const bool h = k == AOp::LdrStrHImm || k == AOp::LdrStrHReg;
    const bool dual = h && !l && ((instr >> 5) & 3) != 1;   // LDRD/STRD carry L = 0
    const bool load = dual ? ((instr >> 5) & 3) == 2 : l;
    u16 data = bit(rd); if (dual) data |= bit((rd + 1) & 0xF);
    if (load) u.writes |= data; else u.reads |= data;
    if (!p || w) u.writes |= bit(rn);
    break;
  }
  case AOp::Swp: case AOp::Swpb: u.reads = bit(rm) | bit(rn); u.writes = bit(rd); break;
  case AOp::Ldm: u.reads = bit(rn); u.writes = static_cast<u16>(instr & 0xFFFF); if (instr & (1u << 21)) u.writes |= bit(rn); break;
  case AOp::Stm: u.reads = static_cast<u16>((instr & 0xFFFF) | bit(rn)); if (instr & (1u << 21)) u.writes |= bit(rn); break;
  case AOp::Mcr: u.reads = bit(rd); break;
  case AOp::Mrc: if (rd != 15) u.writes = bit(rd); break;
  case AOp::Ldc: case AOp::Stc: u.reads = bit(rn); break;
  default: break;
  }
  return u;
}
RegUse thumb_reg_use(u16 instr) {
  auto bit = [](u32 r) { return static_cast<u16>(1u << r); };
  const u32 rd = instr & 7, rs = (instr >> 3) & 7, rn = (instr >> 6) & 7, r8 = (instr >> 8) & 7;
  RegUse u{0, 0};
  switch (arm::decode_thumb(instr)) {
  case TOp::ShiftImm: u.reads = bit(rs); u.writes = bit(rd); break;
  case TOp::AddSubReg: u.reads = bit(rs) | bit(rn); u.writes = bit(rd); break;
  case TOp::AddSubImm3: u.reads = bit(rs); u.writes = bit(rd); break;
  case TOp::MovCmpAddSubImm8: {
    const u32 op = (instr >> 11) & 3;
    if (op == 0) u.writes = bit(r8); else if (op == 1) u.reads = bit(r8); else { u.reads = bit(r8); u.writes = bit(r8); }
    break;
  }
  case TOp::Alu: {
    const u32 op = (instr >> 6) & 0xF;
    u.reads = bit(rs);
    if (op == 8 || op == 0xA || op == 0xB) u.reads |= bit(rd);        // TST CMP CMN
    else if (op == 9) u.writes = bit(rd);                             // NEG
    else { u.reads |= bit(rd); u.writes = bit(rd); }
    break;
  }
  case TOp::HiRegOp: {
    const u32 hd = rd | ((instr >> 4) & 8), hs = (instr >> 3) & 0xF, op = (instr >> 8) & 3;
    if (op == 0) { u.reads = bit(hd) | bit(hs); u.writes = bit(hd); }
    else if (op == 1) u.reads = bit(hd) | bit(hs);
    else if (op == 2) { u.reads = bit(hs); u.writes = bit(hd); }
    break;
  }
  case TOp::BxBlx: u.reads = bit((instr >> 3) & 0xF); if (instr & (1 << 7)) u.writes = bit(14); break;
  case TOp::LdrPcRel: u.reads = bit(15); u.writes = bit(r8); break;
  case TOp::LdrStrReg: {
    const u32 op = (instr >> 9) & 7;   // 0 STR 1 STRH 2 STRB 3 LDRSB 4 LDR 5 LDRH 6 LDRB 7 LDRSH
    u.reads = bit(rs) | bit(rn);
    if (op >= 3) u.writes = bit(rd); else u.reads |= bit(rd);
    break;
  }
  case TOp::LdrStrImm5: case TOp::LdrStrHImm5: u.reads = bit(rs); if (instr & (1 << 11)) u.writes = bit(rd); else u.reads |= bit(rd); break;
  case TOp::LdrStrSpRel: u.reads = bit(13); if (instr & (1 << 11)) u.writes = bit(r8); else u.reads |= bit(r8); break;
  case TOp::AddPcSp: u.reads = bit((instr & (1 << 11)) ? 13 : 15); u.writes = bit(r8); break;
  case TOp::AdjustSp: u.reads = bit(13); u.writes = bit(13); break;
  case TOp::PushPop: {
    const u16 list = static_cast<u16>((instr & 0xFF) | ((instr & (1 << 8)) ? ((instr & (1 << 11)) ? bit(15) : bit(14)) : 0));
    u.reads = bit(13); u.writes = bit(13);
    if (instr & (1 << 11)) u.writes |= list; else u.reads |= list;
    break;
  }
  case TOp::StmLdm: {
    const u16 list = static_cast<u16>(instr & 0xFF);
    u.reads = bit(r8); u.writes = bit(r8);
    if (instr & (1 << 11)) u.writes |= list; else u.reads |= list;
    break;
  }
  case TOp::BlPrefix: u.writes = bit(14); break;
  case TOp::BlSuffix: case TOp::BlxSuffix: u.reads = bit(14); u.writes = bit(14); break;
  default: break;
  }
  return u;
}
bool arm_is_mem(u32 instr) {
  switch (arm::decode_arm(instr)) {
  case AOp::LdrStrImm: case AOp::LdrStrReg: case AOp::LdrStrHImm: case AOp::LdrStrHReg: case AOp::Swp: case AOp::Swpb: case AOp::Ldm: case AOp::Stm: return true;
  default: return false;
  }
}
bool thumb_is_mem(u16 instr) {
  switch (arm::decode_thumb(instr)) {
  case TOp::LdrPcRel: case TOp::LdrStrReg: case TOp::LdrStrImm5: case TOp::LdrStrHImm5: case TOp::LdrStrSpRel: case TOp::PushPop: case TOp::StmLdm: return true;
  default: return false;
  }
}

enum class CarryKind { Keep, Const, Reg };
struct Carry { CarryKind kind = CarryKind::Keep; u32 value = 0; u32 reg = 0; };

struct Operand { bool imm; u32 reg; u32 value; };

struct Instr { u32 addr; u32 raw; u32 live_out; };

inline u32 rotr(u32 v, u32 n) { n &= 31; return n ? (v >> n) | (v << (32 - n)) : v; }

class Translator {
public:
  static u8* cold_scratch() {
    static thread_local std::unique_ptr<u8[]> buf;
    if (!buf) buf.reset(new u8[COLD_CAP]);
    return buf.get();
  }
  Translator(JitCpu& jc, u32 key, Emitter& e, Block& b)
      : jc_(jc), cpu_(*jc.ctx), hot_(e), blk_(b), key_(key), thumb_(key_thumb(key)), a9_(jc.arm9),
        instrs_(scratch_instrs()), cold_buf_(cold_scratch()), cold_(cold_buf_, COLD_CAP), cur_(&hot_), fixes_(scratch_fixes()) {
    instrs_.clear(); fixes_.clear();
  }

  bool run();

private:
  JitCpu& jc_;
  CpuContext& cpu_;
  Emitter& hot_;
  Block& blk_;
  const u32 key_;
  const bool thumb_, a9_;
  u32 cur_raw_ = 0;          // the guest instruction being translated (fast timing's data cost)
  int charge_as_ = -1;       // fast timing: emit_single charges this instead of numC + data (>= 0)
  std::vector<Instr>& instrs_;
  u32 pc_ = 0;
  u32 live_ = F_ALL;
  u32 pending_ = 0;          // static cycles not yet subtracted from the budget
  u32 charged_ahead_ = 0;    // cycles already charged before a conditional instruction's body
  bool ended_ = false;
  bool dp_csel_ = false;   // arm_data_processing: write SCRATCH6, not the guest register
  const u8* t7_ = nullptr;
  u32 code_region7_ = 0;
  // Static lr after a Thumb BL prefix at the previous address.
  bool bl_prefix_valid_ = false;
  u32  bl_prefix_lr_ = 0;

  // ---- hot / cold sections ------------------------------------------------------------
  // DS_JIT_DENSITY
  DensitySlot* dslot_ = nullptr;
  u32 dinstrs_ = 0;            // guest instructions translated inline
  u32 dbytes_ = 0;             // hot bytes added by the counter itself

  u8* cold_buf_;
  Emitter cold_;
  Emitter* cur_;
  struct Fix { size_t at; bool at_cold; size_t target; bool target_cold; const void* abs; };
  // Fastmem sites; resume_cold is cold-relative until finish().
  struct FmSiteRel { u32 fault; u32 patch; u32 resume_cold; u64 guest; };
  std::vector<FmSiteRel> fm_sites_;
  u32 fm_faults_[4] = {};
  u32 fm_nfaults_ = 0;
  static std::vector<Instr>& scratch_instrs() { static thread_local std::vector<Instr> v; return v; }
  static std::vector<Fix>& scratch_fixes() { static thread_local std::vector<Fix> v; return v; }
  std::vector<Fix>& fixes_;

  Emitter& e() { return *cur_; }
  bool in_cold() const { return cur_ == &cold_; }
  u32 step() const { return thumb_ ? 2 : 4; }

  void call_stub(const u8* stub) {
    if (!in_cold()) { hot_.bl(stub); return; }
    fixes_.push_back({cold_.bl_fwd(), true, 0, false, stub});
  }
  void jump_stub(const u8* stub) {
    if (!in_cold()) { hot_.b(stub); return; }
    fixes_.push_back({cold_.b_fwd(), true, 0, false, stub});
  }
  struct FailList {
    size_t v[8]; u32 n = 0;
    FailList() = default;
    FailList(std::initializer_list<size_t> l) { for (size_t x : l) push_back(x); }
    void push_back(size_t x) { assert(n < 8); v[n++] = x; }
    const size_t* begin() const { return v; }
    const size_t* end() const { return v + n; }
    bool empty() const { return n == 0; }
  };
  void cold_begin(const FailList& hot_fixups) {
    assert(!in_cold());
    for (size_t f : hot_fixups) fixes_.push_back({f, false, cold_.size(), true, nullptr});
    cur_ = &cold_;
  }
  void cold_end() { cur_ = &hot_; }
  void cold_end_jump(size_t hot_target) {
    fixes_.push_back({cold_.b_fwd(), true, hot_target, false, nullptr});
    cur_ = &hot_;
  }
  // DS_JIT_DENSITY counter at block entry, before the budget check. Must not write NZCV.
  void emit_density_bump() {
    dslot_ = density_new_slot();
    if (!dslot_) return;
    const size_t before = hot_.size();
    hot_.mov_imm64(SCRATCH0, reinterpret_cast<u64>(&dslot_->execs));
    hot_.ldr_x(SCRATCH1, SCRATCH0, 0);
    hot_.add_imm(SCRATCH1, SCRATCH1, 1, true, false);
    hot_.str_x(SCRATCH1, SCRATCH0, 0);
    dbytes_ = static_cast<u32>(hot_.size() - before);
  }

  bool finish() {
    assert(!in_cold());
    const size_t cold_base = hot_.size();
    blk_.hot_size = static_cast<u32>(cold_base);
    if (dslot_) { dslot_->hot_bytes = blk_.hot_size - dbytes_; dslot_->guest_instrs = dinstrs_; }
    if (hot_.remaining() < cold_.size() + 64) return false;
    std::memcpy(hot_.cur(), cold_buf_, cold_.size());
    hot_.set_pos(cold_base + cold_.size());
    u8* base = hot_.base();
    for (const Fix& f : fixes_) {
      u8* at = base + (f.at_cold ? cold_base + f.at : f.at);
      const u8* target = f.abs ? static_cast<const u8*>(f.abs) : base + (f.target_cold ? cold_base + f.target : f.target);
      Emitter::patch_rel(at, target);
    }
    assert(hot_.size() >= 12 && "kill_block patches 12 bytes at the entry");
    for (const FmSiteRel& s : fm_sites_) rt().fm_new.push_back({s.fault, s.patch, static_cast<u32>(cold_base) + s.resume_cold, s.guest});
    return true;
  }

  // ---- exit flag liveness ----------------------------------------------------------------
  // Flags a successor may read before writing them, from its first
  // instructions up to its own block end. Blocks end live in every flag
  // otherwise, so a Thumb `tst` before a branch must keep C and V (a stub call
  // on every such exit) though the target overwrites them first. Only static
  // successors; the bytes read widen [lo, hi], which the block then watches
  // for writes (capped at one page so it spans at most two).
  u32 successor_flag_live(u32 a, u32& lo, u32& hi) {
    constexpr u32 SCAN = 8;
    u32 undecided = F_ALL, live = 0, nlo = std::min(lo, a), nhi = hi;
    for (u32 k = 0; k < SCAN && undecided; ++k, a += step()) {
      if (!cpu_.page_table.read_ptr(a)) return F_ALL;
      nlo = std::min(nlo, a); nhi = std::max(nhi, a + step() - 1);
      if (nhi - nlo >= mem::PAGE_SIZE) return F_ALL;
      const u32 raw = fetch(a);
      const FlagUse u = thumb_ ? thumb_flag_use(static_cast<u16>(raw), a9_) : arm_flag_use(raw, a9_);
      live |= u.reads & undecided;
      undecided &= ~(u.reads | u.writes);
      if (thumb_ ? shape::thumb_ends_block(static_cast<u16>(raw)) : shape::arm_ends_block(raw, a9_)) break;
    }
    lo = nlo; hi = nhi;
    return live | undecided;
  }
  // `next`: the address after the block's last instruction.
  u32 exit_flag_live(u32 next, u32& lo, u32& hi) {
    const Instr& last = instrs_.back();
    const bool ends = thumb_ ? shape::thumb_ends_block(static_cast<u16>(last.raw)) : shape::arm_ends_block(last.raw, a9_);
    if (!ends) return successor_flag_live(next, lo, hi);
    u32 target = 0;
    bool cond = false;
    if (thumb_) {
      const u16 t = static_cast<u16>(last.raw);
      const TOp op = arm::decode_thumb(t);
      if (op == TOp::BCond) { target = last.addr + 4 + static_cast<u32>(static_cast<s32>(static_cast<s8>(t & 0xFF)) * 2); cond = true; }
      else if (op == TOp::B) target = last.addr + 4 + static_cast<u32>((static_cast<s32>(static_cast<u32>(t) << 21) >> 21) * 2);
      else return F_ALL;
    } else {
      if ((last.raw >> 28) == 0xF || arm::decode_arm(last.raw) != AOp::B) return F_ALL;
      target = last.addr + 8 + static_cast<u32>(static_cast<s32>(last.raw << 8) >> 6);
      cond = (last.raw >> 28) != 0xE;
    }
    u32 live = successor_flag_live(target, lo, hi);
    if (cond && live != F_ALL) live |= successor_flag_live(next, lo, hi);
    return cond && live == F_ALL ? F_ALL : live;
  }

  // ---- decoding ------------------------------------------------------------------------
  u32 fetch(u32 addr) {
    if (u8* p = cpu_.page_table.read_ptr(addr)) {
      if (thumb_) { u16 v; std::memcpy(&v, p, 2); return v; }
      u32 v; std::memcpy(&v, p, 4); return v;
    }
    // Unmapped code (DSi ARM7 BIOS) must use Bus::fetch: a data read from
    // outside the BIOS returns 0xFFFFFFFF.
    return cpu_.nds->bus.fetch(cpu_.which, addr, thumb_ ? 16 : 32);
  }
  // ---- cycles -----------------------------------------------------------------------------
  // Record a baked ARM9 timing byte (RETIME_*) of addr's 4 KB page; retiming
  // kills the block only if it changes. Overflow kills conservatively.
  void note_dep(u32 addr, u8 kind) const {
    const u32 page = addr >> 12;
    for (u32 i = 0; i < blk_.ndep; ++i) if (blk_.dep_page[i] == page) { blk_.dep_kind[i] |= kind; return; }
    if (blk_.ndep < Block::DEP_MAX) { blk_.dep_page[blk_.ndep] = page; blk_.dep_kind[blk_.ndep] = kind; ++blk_.ndep; }
    else blk_.dep_overflow = true;
  }
  u32 numC(u32 addr) const {
    if (a9_) {
      const u32 pf = addr + (thumb_ ? 4 : 8);
      if (thumb_ && (pf & 2)) return 0;
      note_dep(pf, mem::Timing::RETIME_CODE);
      return fetch_cost9(cpu_, pf, false);
    }
    return t7_[thumb_ ? 1 : 3];
  }
  // ARM9 CD charge for a translate-time data cost (pc-relative literal).
  // Must mirror emit_charge_data.
  u32 const_charge(u32 nd) const {
    assert(a9_);
    const s32 nc = static_cast<s32>(numC(pc_));
    const s32 d = static_cast<s32>(nd);
    return max3(nc + d - 6, nc, d);
  }
  u32 numC_nonseq7() const { return t7_[thumb_ ? 0 : 2]; }
  u32 numC_internal() const { return a9_ ? numC(pc_) : numC_nonseq7(); }   // CI base cost
  void add_pending(u32 c) {
    if (charged_ahead_) { const u32 k = std::min(c, charged_ahead_); c -= k; charged_ahead_ -= k; }
    pending_ += c;
  }
  void flush_pending() {
    if (!pending_) return;
    e().sub_imm_any(R_BUDGET, R_BUDGET, pending_, SCRATCH0);
    pending_ = 0;
  }
  void emit_budget_check(u32 exit_key) {
    assert(!in_cold());
    const size_t t = hot_.tbnz_fwd(R_BUDGET, 31);
    cold_begin({t});
    call_stub(rt().exit_key_lit);
    e().word(exit_key);
    cold_end();
  }

  // ---- flags ---------------------------------------------------------------------------------
  // N,Z from `res`, C per `carry`, V kept; live flags only. Clobbers x0-x3.
  void set_flags_logical(u32 res, const Carry& carry, bool res64 = false) {
    const bool need_c = live_ & F_C, need_v = live_ & F_V;
    if (!need_c && !need_v) { e().tst_reg(res, res, res64); return; }
    const bool keep_c = need_c && carry.kind == CarryKind::Keep;
    if (res64) {   // long multiplies: inline merge
      if (keep_c || need_v) e().mrs_nzcv(SCRATCH1);
      e().tst_reg(res, res, true);
      e().mrs_nzcv(SCRATCH2);
      if (keep_c && need_v) { e().ubfx(SCRATCH3, SCRATCH1, 28, 2, true); e().bfi(SCRATCH2, SCRATCH3, 28, 2, true); }
      else {
        if (need_v) { e().ubfx(SCRATCH3, SCRATCH1, 28, 1, true); e().bfi(SCRATCH2, SCRATCH3, 28, 1, true); }
        if (keep_c) { e().ubfx(SCRATCH3, SCRATCH1, 29, 1, true); e().bfi(SCRATCH2, SCRATCH3, 29, 1, true); }
      }
      if (need_c && carry.kind == CarryKind::Const && carry.value) e().orr_imm(SCRATCH2, SCRATCH2, 1u << 29);
      if (need_c && carry.kind == CarryKind::Reg) e().bfi(SCRATCH2, carry.reg, 29, 1, true);
      e().msr_nzcv(SCRATCH2);
      return;
    }
    assert(res != SCRATCH1);
    if (!need_c || keep_c) {   // C kept (or dead), V kept
      if (res != SCRATCH0) e().mov(SCRATCH0, res);
      call_stub(rt().merge_keep_cv);
      return;
    }
    if (!need_v) {             // C set, V dead: inline
      e().tst_reg(res, res);
      e().mrs_nzcv(SCRATCH2);
      if (carry.kind == CarryKind::Const) { if (carry.value) e().orr_imm(SCRATCH2, SCRATCH2, 1u << 29); }
      else e().bfi(SCRATCH2, carry.reg, 29, 1, true);
      e().msr_nzcv(SCRATCH2);
      return;
    }
    assert(carry.kind != CarryKind::Reg || carry.reg != SCRATCH0);
    if (carry.kind == CarryKind::Const) e().movz(SCRATCH1, carry.value);
    else if (carry.reg != SCRATCH1) e().mov(SCRATCH1, carry.reg);
    if (res != SCRATCH0) e().mov(SCRATCH0, res);
    call_stub(rt().merge_set_c);
  }

  // ---- operands ---------------------------------------------------------------------------------
  Operand reg_operand(u32 r, u32 pc_value) {
    if (r == 15) return {true, 0, pc_value};
    return {false, host_reg(r), 0};
  }
  u32 to_reg(const Operand& o, u32 scratch) {
    if (!o.imm) return o.reg;
    e().mov_imm(scratch, o.value);
    return scratch;
  }

  // Carry-out in `cscratch` when wanted. Uses x7 for RRX.
  Operand shift_imm(u32 type, u32 amt, const Operand& rm, bool want_carry, Carry& carry, u32 scratch, u32 cscratch) {
    carry = {};
    if (rm.imm && !(type == 3 && amt == 0)) {
      bool c = false; u32 v = rm.value;
      switch (type) {
      case 0: if (amt == 0) return rm; c = (v >> (32 - amt)) & 1; v <<= amt; break;
      case 1: if (amt == 0) { c = v >> 31; v = 0; } else { c = (v >> (amt - 1)) & 1; v >>= amt; } break;
      case 2: if (amt == 0) { c = v >> 31; v = static_cast<u32>(static_cast<s32>(v) >> 31); } else { c = (v >> (amt - 1)) & 1; v = static_cast<u32>(static_cast<s32>(v) >> amt); } break;
      default: c = (v >> (amt - 1)) & 1; v = rotr(v, amt); break;
      }
      carry = {CarryKind::Const, c, 0};
      return {true, 0, v};
    }
    const u32 m = to_reg(rm, scratch);
    switch (type) {
    case 0:
      if (amt == 0) return {false, m, 0};
      if (want_carry) { e().ubfx(cscratch, m, 32 - amt, 1); carry = {CarryKind::Reg, 0, cscratch}; }
      e().lsl_imm(scratch, m, amt);
      return {false, scratch, 0};
    case 1:
      if (amt == 0) { if (want_carry) { e().lsr_imm(cscratch, m, 31); carry = {CarryKind::Reg, 0, cscratch}; } return {true, 0, 0}; }
      if (want_carry) { e().ubfx(cscratch, m, amt - 1, 1); carry = {CarryKind::Reg, 0, cscratch}; }
      e().lsr_imm(scratch, m, amt);
      return {false, scratch, 0};
    case 2:
      if (amt == 0) { if (want_carry) { e().lsr_imm(cscratch, m, 31); carry = {CarryKind::Reg, 0, cscratch}; } e().asr_imm(scratch, m, 31); return {false, scratch, 0}; }
      if (want_carry) { e().ubfx(cscratch, m, amt - 1, 1); carry = {CarryKind::Reg, 0, cscratch}; }
      e().asr_imm(scratch, m, amt);
      return {false, scratch, 0};
    default:
      if (amt == 0) {   // RRX
        if (want_carry) { e().and_imm(cscratch, m, 1); carry = {CarryKind::Reg, 0, cscratch}; }
        e().cset(SCRATCH7, CS);
        e().extr(scratch, SCRATCH7, m, 1);
        return {false, scratch, 0};
      }
      e().ror_imm(scratch, m, amt);
      if (want_carry) { e().lsr_imm(cscratch, scratch, 31); carry = {CarryKind::Reg, 0, cscratch}; }
      return {false, scratch, 0};
    }
  }

  // Amount = rs & 0xFF. Uses x4-x7.
  Operand shift_reg(u32 type, u32 rs_host, const Operand& rm, bool want_carry, Carry& carry, u32 scratch, u32 cscratch) {
    carry = {};
    const u32 m = to_reg(rm, scratch);
    const u32 amt = SCRATCH4, tmp = SCRATCH5, old_c = SCRATCH6, wide = SCRATCH7;
    e().and_imm(amt, rs_host, 0xFF);
    if (want_carry) e().cset(old_c, CS);
    if (type == 3) {
      e().rorv(scratch, m, amt);
      if (want_carry) {
        e().lsr_imm(cscratch, scratch, 31);
        size_t nz = e().cbnz_fwd(amt);
        e().mov(cscratch, old_c);
        e().bind(nz);
        carry = {CarryKind::Reg, 0, cscratch};
      }
      return {false, scratch, 0};
    }
    // tmp = min(amt, 33): a 64-bit shift by it gives ARM result and carry for any amt.
    e().sub_imm(tmp, amt, 33);
    e().bic_reg(tmp, tmp, tmp, ASR, 31);
    e().sub_reg(tmp, amt, tmp);
    switch (type) {
    case 0:
      e().mov(wide, m);
      e().lslv(wide, wide, tmp, true);
      if (want_carry) e().ubfx(cscratch, wide, 32, 1, true);
      e().mov(scratch, wide);
      break;
    case 1:
      e().lsl_imm(wide, m, 32, true);
      e().lsrv(wide, wide, tmp, true);
      if (want_carry) e().ubfx(cscratch, wide, 31, 1, true);
      e().lsr_imm(scratch, wide, 32, true);
      break;
    default:
      e().sbfm(wide, m, 0, 31, true);
      e().lsl_imm(wide, wide, 32, true);
      e().asrv(wide, wide, tmp, true);
      if (want_carry) e().ubfx(cscratch, wide, 31, 1, true);
      e().lsr_imm(scratch, wide, 32, true);
      break;
    }
    if (want_carry) {
      size_t nz = e().cbnz_fwd(amt);
      e().mov(cscratch, old_c);
      e().bind(nz);
      carry = {CarryKind::Reg, 0, cscratch};
    }
    return {false, scratch, 0};
  }

  // ---- helpers: fallback, trace ------------------------------------------------------------------
  // Interpreter evaluates the condition and charges cycles itself.
  void emit_fallback(u32 instr, bool always_jumps) {
    flush_pending();
    call_stub(jc_.fallback);
    e().word(instr);
    e().word(make_key(pc_, thumb_));
    if (always_jumps) ended_ = true;
  }
  void emit_call2(const void* fn, u32 instr, u32 key) {
    call_stub(rt().call2);
    e().word(instr);
    e().word(key);
    const u64 f = reinterpret_cast<u64>(fn);
    e().word(static_cast<u32>(f));
    e().word(static_cast<u32>(f >> 32));
  }
  void emit_trace(u32 instr) { emit_call2(reinterpret_cast<const void*>(&jit_h_trace), instr, make_key(pc_, thumb_)); }

  // ---- branches ----------------------------------------------------------------------------------
  void emit_branch_static(u32 target, bool to_thumb, bool refill) {
    if (refill) {
      if (a9_) {   // pages refill_cycles reads
        if (!to_thumb) { note_dep(target, mem::Timing::RETIME_CODE); note_dep(target + 4, mem::Timing::RETIME_CODE); }
        else if (target & 2) { note_dep(target - 2, mem::Timing::RETIME_CODE); note_dep(target + 2, mem::Timing::RETIME_CODE); }
        else note_dep(target, mem::Timing::RETIME_CODE);
      }
      add_pending(refill_cycles(cpu_, target, to_thumb));
    }
    flush_pending();
    if (to_thumb != thumb_) {
      e().ldr_w(SCRATCH0, R_CTX, OFF_CPSR);
      e().eor_imm(SCRATCH0, SCRATCH0, 0x20);
      e().str_w(SCRATCH0, R_CTX, OFF_CPSR);
    }
    call_stub(jc_.link);
    e().word(make_key(target, to_thumb));
    ended_ = true;
  }
  void emit_branch_indirect(u32 wtarget, bool interwork, bool cdi = false, bool poll = false) {
    flush_pending();
    if (wtarget != SCRATCH0) e().mov(SCRATCH0, wtarget);
    if (!interwork) {
      if (thumb_) e().orr_imm(SCRATCH0, SCRATCH0, 1);
      else e().and_imm(SCRATCH0, SCRATCH0, ~1u);
    }
    jump_stub(cdi ? jc_.branch_indirect_cdi : poll ? jc_.branch_indirect_poll : jc_.branch_indirect);
    ended_ = true;
  }

  // ---- memory ---------------------------------------------------------------------------------------
  enum class Mem { Ld32, Ld16, Ld8, Ld16S, Ld8S, St32, St16, St8 };
  static bool is_load(Mem m) { return m <= Mem::Ld8S; }
  static bool is_word(Mem m) { return m == Mem::Ld32 || m == Mem::St32; }
  static u32 size_index(Mem m) {
    switch (m) {
    case Mem::Ld8: case Mem::Ld8S: case Mem::St8: return 0;
    case Mem::Ld16: case Mem::Ld16S: case Mem::St16: return 1;
    default: return 2;
    }
  }

  // Page entry -> x2 (clobbers x3). Under fastmem R_PT holds the view, not the table.
  void emit_entry_load(u32 waddr) {
    if (!jc_.fastmem) {
      e().lsr_imm(SCRATCH2, waddr, mem::PAGE_SHIFT);
      e().ldr_x_reg(SCRATCH2, R_PT, SCRATCH2, true, true);
      return;
    }
    e().ldr_x(SCRATCH3, R_CTX, OFF_JIT);
    e().ldr_x(SCRATCH3, SCRATCH3, OFF_JC_TABLE);
    e().lsr_imm(SCRATCH2, waddr, mem::PAGE_SHIFT);
    e().ldr_x_reg(SCRATCH2, SCRATCH3, SCRATCH2, true, true);
  }

  // ---- fastmem ----
  bool fm_fast() const { return jc_.fastmem && !rt().fm_is_slow(fm_key(a9_, pc_, thumb_)); }
  size_t fm_base() { const size_t at = hot_.size(); hot_.mov(SCRATCH3, R_PT, true); return at; }
  void fm_fault_here() { fm_faults_[fm_nfaults_++] = static_cast<u32>(hot_.size()); }
  // Cold walk for `patch`: x3 = host base for w1, then resume at patch + 4.
  // fail_to == ~0 returns the failure branches unbound. pre_resume_align re-derives w2.
  FailList fm_cold_walk(size_t patch, bool store, size_t fail_to, bool pre_resume_align) {
    assert(in_cold());
    const u32 resume = static_cast<u32>(cold_.size());
    emit_entry_load(SCRATCH1);
    FailList f;
    if (store) { e().lsr_imm(SCRATCH3, SCRATCH2, 62, true); f.push_back(e().cbnz_fwd(SCRATCH3)); }
    e().lsl_imm(SCRATCH3, SCRATCH2, 2, true);
    f.push_back(e().cbz_fwd(SCRATCH3, true));
    if (pre_resume_align) e().and_imm(SCRATCH2, SCRATCH1, ~3u);
    fixes_.push_back({cold_.b_fwd(), true, patch + 4, false, nullptr});
    for (u32 i = 0; i < fm_nfaults_; ++i) fm_sites_.push_back({fm_faults_[i], static_cast<u32>(patch), resume, fm_key(a9_, pc_, thumb_)});
    fm_nfaults_ = 0;
    if (fail_to != ~size_t{0}) { for (size_t s : f) cold_.bind_to(s, fail_to); return {}; }
    return f;
  }

  // On success x3 = pre-biased host base. Clobbers x2, x3.
  void emit_page_lookup(u32 waddr, bool store, FailList& fail) {
    emit_entry_load(waddr);
    if (store) {
      e().lsr_imm(SCRATCH3, SCRATCH2, 62, true);
      fail.push_back(e().cbnz_fwd(SCRATCH3));
    }
    e().lsl_imm(SCRATCH3, SCRATCH2, 2, true);
    fail.push_back(e().cbz_fwd(SCRATCH3, true));
  }
  // Helper returns the aligned raw value in w0; apply rotation / extension.
  void emit_load_post(Mem m, u32 dst) {
    switch (m) {
    case Mem::Ld32:
      e().lsl_imm(SCRATCH5, SCRATCH1, 3);
      e().rorv(dst, SCRATCH0, SCRATCH5);
      break;
    case Mem::Ld16:
      if (a9_) { if (dst != SCRATCH0) e().mov(dst, SCRATCH0); }
      else { e().ubfiz(SCRATCH5, SCRATCH1, 3, 1); e().rorv(dst, SCRATCH0, SCRATCH5); }
      break;
    case Mem::Ld8:  if (dst != SCRATCH0) e().mov(dst, SCRATCH0); break;
    case Mem::Ld8S: e().sxtb(dst, SCRATCH0); break;
    case Mem::Ld16S:
      if (a9_) e().sxth(dst, SCRATCH0);
      else {
        size_t odd = e().tbnz_fwd(SCRATCH1, 0);
        e().sxth(dst, SCRATCH0);
        size_t j = e().b_fwd();
        e().bind(odd);
        e().lsr_imm(SCRATCH0, SCRATCH0, 8);
        e().sxtb(dst, SCRATCH0);
        e().bind(j);
      }
      break;
    default: break;
    }
  }
  void emit_data_cost(u32 waddr, u32 wcost, bool word, bool seq, bool store) {
    // ARM9 entries are 8 bytes: loads at [1..3], stores at [5..7].
    const u32 k = a9_ ? ((store ? 4 : 0) + (seq ? 3 : (word ? 2 : 1))) : (seq ? (word ? 3 : 1) : (word ? 2 : 0));
    e().lsr_imm(wcost, waddr, a9_ ? 12 : 15);
    e().add_reg(wcost, R_TIM, wcost, LSL, a9_ ? 3 : 2, true);
    e().ldrb(wcost, wcost, k);
  }

  // Precomputed ARM7 cost-table slot, or -1 to use the inline model.
  int cost7_slot(bool cdi, bool word) const {
    if (a9_) return -1;
    const int ni = cpu_.nds->bus.timing().nc7_index(numC_nonseq7());
    if (ni < 0) return -1;
    return static_cast<int>(mem::Timing::cost7_offset(code_region7_ == 0x02, cdi, static_cast<u32>(ni), word));
  }
  // Flagless max.
  void emit_max(u32 wd, u32 wa, u32 wb, u32 tmp) {
    e().sub_reg(tmp, wb, wa);
    e().bic_reg(tmp, tmp, tmp, ASR, 31);
    e().add_reg(wd, wa, tmp);
  }
  // `wd` (w6) = data cost; waddr for the ARM7 main-RAM rule. Temps w4 (ARM9) / w2-w5 (ARM7).
  void emit_charge_data(u32 wd, u32 waddr, bool cdi) {
    flush_pending();
    if (a9_) {
      // max(nc + nd - 6, nc, nd), nd >= 1.
      const u32 nc = numC(pc_);
      if (nc <= 1) { e().sub_reg(R_BUDGET, R_BUDGET, wd); return; }
      if (nc <= 6) {
        e().sub_imm(SCRATCH4, wd, nc);
        e().bic_reg(SCRATCH4, SCRATCH4, SCRATCH4, ASR, 31);
        e().add_imm(SCRATCH4, SCRATCH4, nc);
      } else {
        e().sub_imm(SCRATCH4, wd, 6);
        e().bic_reg(SCRATCH4, SCRATCH4, SCRATCH4, ASR, 31);
        e().add_imm(SCRATCH4, SCRATCH4, nc);
      }
      e().sub_reg(R_BUDGET, R_BUDGET, SCRATCH4);
      return;
    }
    // ARM7: w7 holds a writeback value.
    const u32 nc = numC_nonseq7();
    const bool code_main = code_region7_ == 0x02;
    e().lsr_imm(SCRATCH2, waddr, 24);
    e().eor_imm(SCRATCH2, SCRATCH2, 2);
    size_t not_main = e().cbnz_fwd(SCRATCH2);
    if (code_main) {
      e().add_imm(SCRATCH5, wd, nc);
    } else {
      const u32 ncx = cdi ? nc + 1 : nc;
      e().mov_imm(SCRATCH3, ncx);
      emit_max(SCRATCH5, SCRATCH3, wd, SCRATCH4);
      if (ncx >= 3) e().add_imm(SCRATCH3, wd, ncx - 3); else e().sub_imm(SCRATCH3, wd, 3 - ncx);
      emit_max(SCRATCH5, SCRATCH5, SCRATCH3, SCRATCH4);
    }
    size_t done = e().b_fwd();
    e().bind(not_main);
    if (code_main) {
      if (cdi) e().add_imm(SCRATCH2, wd, 1); else e().mov(SCRATCH2, wd);
      e().mov_imm(SCRATCH3, nc);
      emit_max(SCRATCH5, SCRATCH3, SCRATCH2, SCRATCH4);
      if (nc >= 3) e().add_imm(SCRATCH3, SCRATCH2, nc - 3); else e().sub_imm(SCRATCH3, SCRATCH2, 3 - nc);
      emit_max(SCRATCH5, SCRATCH5, SCRATCH3, SCRATCH4);
    } else {
      e().add_imm(SCRATCH5, wd, nc + (cdi ? 1 : 0));
    }
    e().bind(done);
    e().sub_reg(R_BUDGET, R_BUDGET, SCRATCH5);
  }
  // One access at w1. const_nd >= 0: translate-time data cost, charged before
  // the access (nothing observes the budget in between).
  // `cold_poll`: the slow path polls and may leave at the next instruction; a
  // load to r15 passes false and polls in its branch instead.
  void emit_single(Mem m, u32 wdata, u32 dst, bool wb, u32 wb_reg, bool cdi, int const_nd = -1, bool cold_poll = true) {
    const bool word = is_word(m);
    // Fast timing: numC + a constant, charged where the exact model charges
    // (after the access), one instruction, no table load or combine.
    const bool ftime = g_fast_timing;
    const bool const_cost = const_nd >= 0 && !ftime;
    if (const_cost) add_pending(const_charge(static_cast<u32>(const_nd)));
    flush_pending();
    const int slot7 = (const_cost || ftime) ? -1 : cost7_slot(cdi, word);
    auto cost = [&] {
      if (const_cost || ftime) return;
      if (slot7 < 0) { emit_data_cost(SCRATCH1, SCRATCH6, word, false, !is_load(m)); return; }
      e().lsr_imm(SCRATCH6, SCRATCH1, 15);
      e().add_reg(SCRATCH6, R_TIM, SCRATCH6, LSL, 5, true);
      e().add_imm(SCRATCH6, SCRATCH6, mem::Timing::COST7_OFFSET, true);
      e().ldrb(SCRATCH6, SCRATCH6, static_cast<u32>(slot7));
    };
    auto charge = [&] {
      if (wb) e().mov(host_reg(wb_reg), SCRATCH7);
      if (ftime) {
        const u32 c = charge_as_ >= 0 ? static_cast<u32>(charge_as_) : numC_internal() + fast_data(cpu_, cur_raw_, thumb_);
        if (c) e().sub_imm(R_BUDGET, R_BUDGET, c);
        return;
      }
      if (const_cost) return;
      if (slot7 < 0) { emit_charge_data(SCRATCH6, SCRATCH1, cdi); return; }
      flush_pending();
      e().sub_reg(R_BUDGET, R_BUDGET, SCRATCH6);
    };
    FailList fail;
    const bool fast = fm_fast();
    size_t patch = 0;
    if (!fast) {
      emit_entry_load(SCRATCH1);
    }
    cost();
    if (word) e().and_imm(SCRATCH4, SCRATCH1, ~3u);
    else if (m == Mem::Ld16 || m == Mem::St16 || (m == Mem::Ld16S && a9_)) e().and_imm(SCRATCH4, SCRATCH1, ~1u);
    if (m == Mem::Ld32 && a9_) e().lsl_imm(SCRATCH5, SCRATCH1, 3);
    if (fast) patch = fm_base();
    else {
      if (!is_load(m)) {
        e().lsr_imm(SCRATCH3, SCRATCH2, 62, true);
        fail.push_back(e().cbnz_fwd(SCRATCH3));
      }
      e().lsl_imm(SCRATCH3, SCRATCH2, 2, true);
      fail.push_back(e().cbz_fwd(SCRATCH3, true));
    }
    auto site = [&] { if (fast) fm_fault_here(); };
    switch (m) {
    case Mem::Ld32:
      site();
      e().ldr_w_reg(SCRATCH0, SCRATCH3, SCRATCH4);
      charge();
      if (!a9_) e().lsl_imm(SCRATCH5, SCRATCH1, 3);
      e().rorv(dst, SCRATCH0, SCRATCH5);
      break;
    case Mem::Ld16:
      site();
      if (a9_) { e().ldrh_reg(dst, SCRATCH3, SCRATCH4); charge(); }
      else { e().ldrh_reg(SCRATCH0, SCRATCH3, SCRATCH4); charge(); e().ubfiz(SCRATCH5, SCRATCH1, 3, 1); e().rorv(dst, SCRATCH0, SCRATCH5); }
      break;
    case Mem::Ld8:  site(); e().ldrb_reg(dst, SCRATCH3, SCRATCH1); charge(); break;
    case Mem::Ld8S: site(); e().ldrsb_w_reg(dst, SCRATCH3, SCRATCH1); charge(); break;
    case Mem::Ld16S:
      if (a9_) { site(); e().ldrsh_w_reg(dst, SCRATCH3, SCRATCH4); charge(); }
      else {
        size_t odd = e().tbnz_fwd(SCRATCH1, 0);
        site();
        e().ldrsh_w_reg(dst, SCRATCH3, SCRATCH1);
        size_t j = e().b_fwd();
        e().bind(odd);
        site();
        e().ldrsb_w_reg(dst, SCRATCH3, SCRATCH1);
        e().bind(j);
        charge();
      }
      break;
    case Mem::St32: site(); e().str_w_reg(wdata, SCRATCH3, SCRATCH4); charge(); break;
    case Mem::St16: site(); e().strh_reg(wdata, SCRATCH3, SCRATCH4); charge(); break;
    case Mem::St8:  site(); e().strb_reg(wdata, SCRATCH3, SCRATCH1); charge(); break;
    }
    const size_t join = hot_.size();
    if (fast) {
      cold_begin({});
      fail = fm_cold_walk(patch, !is_load(m), ~size_t{0}, false);
      for (size_t f : fail) cold_.bind(f);
      fail = {};
      cur_ = &hot_;   // cold_begin below re-enters here
    }
    cold_begin(fail);
    if (!is_load(m) && wdata != SCRATCH2) e().mov(SCRATCH2, wdata);
    call_stub(is_load(m) ? rt().slow_load[size_index(m)] : rt().slow_store[size_index(m)]);
    if (is_load(m)) emit_load_post(m, dst);
    cost();
    charge();
    if (cold_poll) {
      call_stub(rt().poll);
      e().word(make_key(pc_ + step(), thumb_));
    }
    cold_end_jump(join);
  }

  void emit_exc_return(u32 instr, u32 opcode, u32 rn);
  void arm_swp(u32 instr, bool byte);
  void emit_exc_return_tail();
  void arm_mrc(u32 instr);
  void arm_smlal_xy(u32 instr);
  // USR/SYS slot for r`i`, or 0 if the host register is already the user's.
  static u32 user_bank_off(u32 i) { return i == 13 ? OFF_BANK_R13 : i == 14 ? OFF_BANK_R14 : 0; }

  // Page-split block transfer: per-word slow stubs. w1 = address, w7 = writeback
  // (both preserved by the stubs). LDM with r15 never reaches here.
  void emit_block_slow(u32 list, bool load, bool writeback, u32 rn, u32 n, bool user_bank, u32 pc_store_value) {
    e().and_imm(SCRATCH1, SCRATCH1, ~3u);
    bool first = true;
    for (u32 i = 0; i < 16; ++i) {
      if (!(list & (1u << i))) continue;
      const u32 ub = user_bank ? user_bank_off(i) : 0;
      if (load) {
        call_stub(rt().slow_load[2]);
        if (ub) e().str_w(SCRATCH0, R_CTX, ub); else e().mov(host_reg(i), SCRATCH0);
      } else {
        // STM of its own base: old base if first in list, else new.
        if (i == 15) e().mov_imm(SCRATCH2, pc_store_value);
        else if (ub) e().ldr_w(SCRATCH2, R_CTX, ub);
        else e().mov(SCRATCH2, (i == rn && !first && writeback) ? SCRATCH7 : host_reg(i));
        call_stub(rt().slow_store[2]);
      }
      e().add_imm(SCRATCH1, SCRATCH1, 4, true);
      first = false;
    }
    // LDM loading its own base keeps the loaded value.
    if (load) { if (writeback && !(list & (1u << rn))) e().mov(host_reg(rn), SCRATCH7); }
    else if (writeback) e().mov(host_reg(rn), SCRATCH7);
    // Cost after the transfers, as the interpreter and the single-access slow path
    // charge it: an I/O access sees the time before this instruction's data cycles
    // (DIVCNT polled after an STM to the divider). Per word, since words span pages.
    if (g_fast_timing) { e().sub_imm(R_BUDGET, R_BUDGET, numC_internal() + n * fast_data(cpu_, cur_raw_, thumb_)); return; }
    e().sub_imm(SCRATCH1, SCRATCH1, n * 4, true);
    for (u32 k = 0; k < n; ++k) {
      if (k == 0) emit_data_cost(SCRATCH1, SCRATCH6, true, false, !load);
      else {
        e().add_imm(SCRATCH3, SCRATCH1, k * 4, true);
        emit_data_cost(SCRATCH3, SCRATCH5, true, true, !load);
        e().add_reg(SCRATCH6, SCRATCH6, SCRATCH5);
      }
    }
    emit_charge_data(SCRATCH6, SCRATCH1, load);
  }

  void emit_block_transfer(u32 instr, u32 list, bool load, bool writeback, u32 rn, bool interwork_pc, u32 pc_store_value,
                           bool user_bank = false) {
    const u32 n = static_cast<u32>(__builtin_popcount(list));
    flush_pending();
    // LDM^/STM^ inline only from IRQ/SVC/ABT/UND; other modes go to the interpreter.
    FailList mode_fail;
    if (user_bank) {
      constexpr u32 INLINABLE_MODES = (1u << 0x12) | (1u << 0x13) | (1u << 0x17) | (1u << 0x1B);
      e().ldr_w(SCRATCH2, R_CTX, OFF_CPSR);
      e().and_imm(SCRATCH2, SCRATCH2, 0x1F);
      e().mov_imm(SCRATCH4, INLINABLE_MODES);
      e().lsrv(SCRATCH4, SCRATCH4, SCRATCH2);
      mode_fail.push_back(e().tbz_fwd(SCRATCH4, 0));
    }
    FailList fail;
    e().add_imm(SCRATCH2, SCRATCH1, n * 4 - 1);
    e().eor_reg(SCRATCH2, SCRATCH2, SCRATCH1);
    e().lsr_imm(SCRATCH2, SCRATCH2, mem::PAGE_SHIFT);
    fail.push_back(e().cbnz_fwd(SCRATCH2));
    // Fastmem: one 2 KB page, so only the first access can fault (before any side effect).
    const bool fast = fm_fast();
    size_t patch = 0;
    if (fast) {
      e().and_imm(SCRATCH2, SCRATCH1, ~3u);
      patch = fm_base();
    } else {
      emit_page_lookup(SCRATCH1, !load, fail);
      e().and_imm(SCRATCH2, SCRATCH1, ~3u);          // Thumb base may be unaligned
    }
    e().add_uxtw(SCRATCH3, SCRATCH3, SCRATCH2);
    u32 k = 0;
    bool first = true;
    const bool pc_in_list = (list & 0x8000) != 0;
    // Fastmem walk failures go to the block's cold fallback at `fallback_at`.
    auto fm_walk = [&](size_t fallback_at) {
      if (!fast) return;
      cold_begin({});
      fm_cold_walk(patch, !load, fallback_at, true);
      cur_ = &hot_;
    };
    if (load) {
      for (u32 i = 0; i < 16; ++i) {
        if (!(list & (1u << i))) continue;
        if (k == 0 && fast) fm_fault_here();
        const u32 ub = user_bank ? user_bank_off(i) : 0;
        e().ldr_w(ub ? SCRATCH2 : i == 15 ? SCRATCH0 : host_reg(i), SCRATCH3, 4 * k);
        if (ub) e().str_w(SCRATCH2, R_CTX, ub);
        ++k;
      }
      if (writeback && !(list & (1u << rn))) e().mov(host_reg(rn), SCRATCH7);
    } else {
      for (u32 i = 0; i < 16; ++i) {
        if (!(list & (1u << i))) continue;
        u32 src;
        const u32 ub = user_bank ? user_bank_off(i) : 0;
        if (ub) { e().ldr_w(SCRATCH2, R_CTX, ub); src = SCRATCH2; }
        else if (i == 15) { e().mov_imm(SCRATCH0, pc_store_value); src = SCRATCH0; }
        else if (i == rn && !first && writeback) src = SCRATCH7;
        else src = host_reg(i);
        if (k == 0 && fast) fm_fault_here();
        e().str_w(src, SCRATCH3, 4 * k);
        ++k; first = false;
      }
      if (writeback) e().mov(host_reg(rn), SCRATCH7);
    }
    const bool ftime = g_fast_timing;
    if (ftime && load && pc_in_list) {
      // Fast timing: the data cost now, the constant refill in the branch
      // (charge_CDI_after_jump's fast form charges the data alone).
      e().sub_imm(R_BUDGET, R_BUDGET, n * fast_data(cpu_, cur_raw_, thumb_));
      emit_branch_indirect(SCRATCH0, interwork_pc);
      cold_begin(fail);
      const size_t fb = cold_.size();
      emit_fallback(instr, true);
      cold_end();
      fm_walk(fb);
      ended_ = true;
      return;
    }
    // N + (n - 1) S
    if (!ftime) {
      emit_data_cost(SCRATCH1, SCRATCH6, true, false, !load);
      if (n > 1) {
        emit_data_cost(SCRATCH1, SCRATCH5, true, true, !load);
        e().mov_imm(SCRATCH4, n - 1);
        e().madd(SCRATCH6, SCRATCH5, SCRATCH4, SCRATCH6);
      }
    }
    if (load && pc_in_list) {
      // CDI charged by the stub after the jump: w1 = numD, w2 = data address.
      e().mov(SCRATCH2, SCRATCH1);
      e().mov(SCRATCH1, SCRATCH6);
      emit_branch_indirect(SCRATCH0, interwork_pc, true);
      cold_begin(fail);
      const size_t fb = cold_.size();
      emit_fallback(instr, true);
      cold_end();
      fm_walk(fb);
      ended_ = true;
      return;
    }
    if (ftime) e().sub_imm(R_BUDGET, R_BUDGET, numC_internal() + n * fast_data(cpu_, cur_raw_, thumb_));
    else emit_charge_data(SCRATCH6, SCRATCH1, load);
    const size_t join = hot_.size();
    cold_begin(fail);
    const size_t fb = cold_.size();
    emit_block_slow(list, load, writeback, rn, n, user_bank, pc_store_value);
    cold_end_jump(join);
    fm_walk(fb);
    if (!mode_fail.empty()) {
      cold_begin(mode_fail);
      emit_fallback(instr, false);   // user-bank forms never write r15
      cold_end_jump(join);
    }
  }

  // EA -> w1; writeback value -> w7.
  void emit_ea(u32 rn, const Operand& off, bool pre, bool up, bool writeback) {
    const u32 hb = to_reg(reg_operand(rn, pc_ + 8), SCRATCH6);
    const u32 dst = pre && !writeback ? SCRATCH1 : SCRATCH7;
    if (off.imm) { if (up) e().add_imm_any(dst, hb, off.value, SCRATCH5); else e().sub_imm_any(dst, hb, off.value, SCRATCH5); }
    else { if (up) e().add_reg(dst, hb, off.reg); else e().sub_reg(dst, hb, off.reg); }
    if (dst == SCRATCH7) { if (pre) e().mov(SCRATCH1, SCRATCH7); else e().mov(SCRATCH1, hb); }
  }

  // ---- drivers -------------------------------------------------------------------------------------------
  void translate_arm(u32 instr);
  void translate_thumb(u16 instr);
  void arm_data_processing(u32 instr, AOp op);
  void arm_multiply(u32 instr, AOp op);
  void arm_dsp_multiply(u32 instr, AOp op);
  void arm_ldr_str(u32 instr, AOp op);
  void arm_ldr_str_h(u32 instr, AOp op);
  void arm_ldm_stm(u32 instr, bool load);
  void arm_msr(u32 instr, AOp op);
  void emit_mul_cycles7(u32 wrs, bool signed_op, u32 extra);
  // MULS: C kept on ARM9, cleared on ARM7.
  Carry mul_carry() const { return a9_ ? Carry{} : Carry{CarryKind::Const, 0, 0}; }
  void thumb_alu(u16 instr);
  void thumb_ldr_str(u16 instr, TOp op);
};

// ---- ARM -----------------------------------------------------------------------------------------------------

// SUBS/ADDS/MOVS pc, rn, #imm. r13/r14 spilled by hand; call_pure carries r8-r12
// and reloads host flags from the restored CPSR.
void Translator::emit_exc_return(u32 instr, u32 opcode, u32 rn) {
  const u32 imm = rotr(instr & 0xFF, ((instr >> 8) & 0xF) * 2);
  if (opcode == 0xD) e().mov_imm(SCRATCH1, imm);
  else if (opcode == 0x2) e().sub_imm_any(SCRATCH1, host_reg(rn), imm, SCRATCH2);
  else e().add_imm_any(SCRATCH1, host_reg(rn), imm, SCRATCH2);
  emit_exc_return_tail();
}

// Target in w1: restore CPSR from SPSR (call_pure carries r8-r12, reloads host
// flags from the restored CPSR; r13/r14 spilled by hand), then branch.
void Translator::emit_exc_return_tail() {
  add_pending(numC(pc_));
  flush_pending();
  e().str_w(host_reg(13), R_CTX, off_reg(13));
  e().str_w(host_reg(14), R_CTX, off_reg(14));
  e().mov(SCRATCH0, R_CTX, true);
  e().mov_imm64(R_FN, reinterpret_cast<u64>(&jit_h_exc_return));
  call_stub(rt().call_pure);
  e().ldr_w(host_reg(13), R_CTX, off_reg(13));
  e().ldr_w(host_reg(14), R_CTX, off_reg(14));
  emit_branch_indirect(SCRATCH0, true, false, true);   // restored T in bit 0; I may now be clear
}

void Translator::arm_data_processing(u32 instr, AOp op) {
  const u32 opcode = (instr >> 21) & 0xF;
  const bool s = instr & (1u << 20);
  const u32 rd = (instr >> 12) & 0xF, rn = (instr >> 16) & 0xF;
  const bool test = opcode >= 8 && opcode <= 0xB;
  const bool logical = opcode <= 1 || opcode == 8 || opcode == 9 || opcode >= 0xC;
  if (rd == 15 && !test && !dp_csel_) {   // dp_csel_: a jump table's target, into w6 (translate_arm)
    if (shape::dp_exc_return(instr)) { emit_exc_return(instr, opcode, rn); return; }
    if (exc_return_reg(instr)) { e().mov(SCRATCH1, host_reg(instr & 0xF)); emit_exc_return_tail(); return; }
    emit_fallback(instr, true); return;
  }
  if (op == AOp::DpRegShift) add_pending(numC_internal() + 1);
  else add_pending(numC(pc_));

  const bool want_carry = s && logical && (live_ & F_C);
  Carry carry;
  Operand op2, rn_v;
  if (op == AOp::DpImm) {
    const u32 imm = instr & 0xFF, rot = (instr >> 8) & 0xF;
    const u32 v = rotr(imm, rot * 2);
    op2 = {true, 0, v};
    carry = rot ? Carry{CarryKind::Const, v >> 31, 0} : Carry{};
    rn_v = reg_operand(rn, pc_ + 8);
  } else if (op == AOp::DpImmShift) {
    op2 = shift_imm((instr >> 5) & 3, (instr >> 7) & 0x1F, reg_operand(instr & 0xF, pc_ + 8), want_carry, carry, SCRATCH2, SCRATCH3);
    rn_v = reg_operand(rn, pc_ + 8);
  } else {
    op2 = shift_reg((instr >> 5) & 3, host_reg((instr >> 8) & 0xF), reg_operand(instr & 0xF, pc_ + 12), want_carry, carry, SCRATCH2, SCRATCH3);
    rn_v = reg_operand(rn, pc_ + 12);
  }

  const u32 dst = (test || dp_csel_) ? SCRATCH6 : host_reg(rd);
  const u32 a = to_reg(rn_v, SCRATCH4);
  auto b_reg = [&]() { return to_reg(op2, SCRATCH5); };

  switch (opcode) {
  case 0x0: case 0x8:
    if (op2.imm) { if (!e().and_imm(dst, a, op2.value)) e().and_reg(dst, a, b_reg()); }
    else e().and_reg(dst, a, op2.reg);
    if (s) set_flags_logical(dst, carry);
    break;
  case 0x1: case 0x9:
    if (op2.imm) { if (!e().eor_imm(dst, a, op2.value)) e().eor_reg(dst, a, b_reg()); }
    else e().eor_reg(dst, a, op2.reg);
    if (s) set_flags_logical(dst, carry);
    break;
  case 0xC:
    if (op2.imm) { if (!e().orr_imm(dst, a, op2.value)) e().orr_reg(dst, a, b_reg()); }
    else e().orr_reg(dst, a, op2.reg);
    if (s) set_flags_logical(dst, carry);
    break;
  case 0xE:
    if (op2.imm) { if (!e().and_imm(dst, a, ~op2.value)) e().bic_reg(dst, a, b_reg()); }
    else e().bic_reg(dst, a, op2.reg);
    if (s) set_flags_logical(dst, carry);
    break;
  case 0xD:
    if (op2.imm) e().mov_imm(dst, op2.value); else if (op2.reg != dst) e().mov(dst, op2.reg);
    if (s) set_flags_logical(dst, carry);
    break;
  case 0xF:
    if (op2.imm) e().mov_imm(dst, ~op2.value); else e().mvn(dst, op2.reg);
    if (s) set_flags_logical(dst, carry);
    break;
  case 0x2: case 0xA: {
    const u32 d = test ? ZR : dst;
    if (op2.imm) e().sub_imm_any(d, a, op2.value, SCRATCH5, s); else e().add_sub(true, s, d, a, op2.reg);
    break;
  }
  case 0x4: case 0xB: {
    const u32 d = test ? ZR : dst;
    if (op2.imm) e().add_imm_any(d, a, op2.value, SCRATCH5, s); else e().add_sub(false, s, d, a, op2.reg);
    break;
  }
  case 0x3:
    if (op2.imm && op2.value == 0) { if (s) e().negs(dst, a); else e().neg(dst, a); }
    else e().add_sub(true, s, dst, b_reg(), a);
    break;
  case 0x5: e().adc(dst, a, b_reg(), s); break;
  case 0x6: e().sbc(dst, a, b_reg(), s); break;
  case 0x7: e().sbc(dst, b_reg(), a, s); break;
  }
}

void Translator::emit_mul_cycles7(u32 wrs, bool signed_op, u32 extra) {
  // 1..4 internal cycles from the magnitude of rs.
  if (signed_op) e().eor_reg(SCRATCH6, wrs, wrs, ASR, 31); else e().mov(SCRATCH6, wrs);
  e().clz(SCRATCH6, SCRATCH6);
  e().lsr_imm(SCRATCH6, SCRATCH6, 3);          // leading zero bytes 0..4
  e().lsl_imm(SCRATCH6, SCRATCH6, 2);
  e().mov_imm(SCRATCH7, 0x11234);              // nibble table: 4,3,2,1,1
  e().lsrv(SCRATCH7, SCRATCH7, SCRATCH6);
  e().and_imm(SCRATCH7, SCRATCH7, 0xF);
  if (extra) e().add_imm(SCRATCH7, SCRATCH7, extra);
  e().sub_reg(R_BUDGET, R_BUDGET, SCRATCH7);
}

void Translator::arm_multiply(u32 instr, AOp op) {
  const bool s = instr & (1u << 20);
  const u32 rs = (instr >> 8) & 0xF, rm = instr & 0xF;
  const u32 hrs = host_reg(rs), hrm = host_reg(rm);
  if (op == AOp::Mul || op == AOp::Mla) {
    const u32 rd = (instr >> 16) & 0xF, rn = (instr >> 12) & 0xF;
    if (a9_) add_pending(numC(pc_) + (s ? 3 : 1));
    else { add_pending(numC_nonseq7()); flush_pending(); emit_mul_cycles7(hrs, true, op == AOp::Mla ? 1 : 0); }
    if (op == AOp::Mla) e().madd(host_reg(rd), hrm, hrs, host_reg(rn)); else e().mul(host_reg(rd), hrm, hrs);
    if (s) set_flags_logical(host_reg(rd), mul_carry());
    return;
  }
  const u32 rdhi = (instr >> 16) & 0xF, rdlo = (instr >> 12) & 0xF;
  const bool sgn = op == AOp::Smull || op == AOp::Smlal;
  const bool acc = op == AOp::Umlal || op == AOp::Smlal;
  if (a9_) add_pending(numC(pc_) + (s ? 3 : 1));
  else { add_pending(numC_nonseq7()); flush_pending(); emit_mul_cycles7(hrs, sgn, 1); }
  if (acc) {
    e().mov(SCRATCH0, host_reg(rdlo));
    e().bfi(SCRATCH0, host_reg(rdhi), 32, 32, true);
    if (sgn) e().smaddl(SCRATCH0, hrm, hrs, SCRATCH0); else e().umaddl(SCRATCH0, hrm, hrs, SCRATCH0);
  } else {
    if (sgn) e().smull(SCRATCH0, hrm, hrs); else e().umull(SCRATCH0, hrm, hrs);
  }
  e().mov(host_reg(rdlo), SCRATCH0);
  e().lsr_imm(host_reg(rdhi), SCRATCH0, 32, true);
  if (s) set_flags_logical(SCRATCH0, mul_carry(), true);
}

// v5TE DSP multiplies, ARM9 only. Accumulating forms set sticky Q in the CPSR
// image (computed without flags; host NZCV holds guest NZCV). Cost is numC only.
void Translator::arm_dsp_multiply(u32 instr, AOp op) {
  const u32 rd = (instr >> 16) & 0xF, rn = (instr >> 12) & 0xF,
            rs = (instr >> 8) & 0xF, rm = instr & 0xF;
  const bool acc = op == AOp::SmlaXY || op == AOp::SmlawY;
  const bool wide = op == AOp::SmlawY || op == AOp::SmulwY;
  add_pending(numC(pc_));
  e().sbfx(SCRATCH1, host_reg(rs), (instr & (1u << 6)) ? 16 : 0, 16);
  if (wide) {
    e().smull(SCRATCH0, host_reg(rm), SCRATCH1);
    e().asr_imm(SCRATCH0, SCRATCH0, 16, true);
  } else {
    e().sbfx(SCRATCH0, host_reg(rm), (instr & (1u << 5)) ? 16 : 0, 16);
    e().mul(SCRATCH0, SCRATCH0, SCRATCH1);
  }
  if (!acc) { e().mov(host_reg(rd), SCRATCH0); return; }
  e().add_reg(SCRATCH2, SCRATCH0, host_reg(rn));
  e().eor_reg(SCRATCH3, SCRATCH0, SCRATCH2);
  e().eor_reg(SCRATCH4, host_reg(rn), SCRATCH2);
  e().and_reg(SCRATCH3, SCRATCH3, SCRATCH4);
  e().lsr_imm(SCRATCH3, SCRATCH3, 31);
  e().ldr_w(SCRATCH4, R_CTX, OFF_CPSR);
  e().orr_reg(SCRATCH4, SCRATCH4, SCRATCH3, LSL, 27);
  e().str_w(SCRATCH4, R_CTX, OFF_CPSR);
  e().mov(host_reg(rd), SCRATCH2);   // last: rd may alias rm/rs/rn
}

// MRC p15 (ARM9, rd != pc): cp15_read's values, charge_CI(3). Ends the block
// as before (block_shape); the block links on to the next instruction.
void Translator::arm_mrc(u32 instr) {
  const u32 crn = (instr >> 16) & 0xF, rd = (instr >> 12) & 0xF, crm = instr & 0xF, opc2 = (instr >> 5) & 7;
  add_pending(numC_internal() + 3);
  const u32 hr = host_reg(rd);
  auto field = [&](size_t off) { e().ldr_w(hr, R_CTX, static_cast<u32>(off)); };
  switch ((crn << 8) | (crm << 4) | opc2) {
  case 0x000: e().mov_imm(hr, 0x41059461); break;   // main ID
  case 0x001: e().mov_imm(hr, 0x0F0D2112); break;   // cache type
  case 0x002: e().mov_imm(hr, 0x00140180); break;   // TCM size
  case 0x100: field(offsetof(CpuContext, cp15_control)); break;
  case 0x200: field(offsetof(CpuContext, pu_data_cacheable)); break;
  case 0x201: field(offsetof(CpuContext, pu_code_cacheable)); break;
  case 0x300: field(offsetof(CpuContext, pu_data_bufferable)); break;
  case 0x502: field(offsetof(CpuContext, pu_data_perm)); break;
  case 0x503: field(offsetof(CpuContext, pu_code_perm)); break;
  case 0x600: case 0x610: case 0x620: case 0x630: case 0x640: case 0x650: case 0x660: case 0x670:
  case 0x601: case 0x611: case 0x621: case 0x631: case 0x641: case 0x651: case 0x661: case 0x671:
    field(offsetof(CpuContext, pu_region) + crm * sizeof(u32)); break;
  case 0x910: field(offsetof(CpuContext, cp15_dtcm)); break;
  case 0x911: field(offsetof(CpuContext, cp15_itcm)); break;
  default: e().mov_imm(hr, 0); break;
  }
}

// SMLAL<x><y> (ARM9): RdHi:RdLo += half(Rm) * half(Rs), charge_CI(1).
void Translator::arm_smlal_xy(u32 instr) {
  const u32 rdhi = (instr >> 16) & 0xF, rdlo = (instr >> 12) & 0xF, rs = (instr >> 8) & 0xF, rm = instr & 0xF;
  add_pending(numC_internal() + 1);
  e().sbfx(SCRATCH1, host_reg(rs), (instr & (1u << 6)) ? 16 : 0, 16);
  e().sbfx(SCRATCH0, host_reg(rm), (instr & (1u << 5)) ? 16 : 0, 16);
  e().mov(SCRATCH2, host_reg(rdlo));
  e().bfi(SCRATCH2, host_reg(rdhi), 32, 32, true);
  e().smaddl(SCRATCH2, SCRATCH0, SCRATCH1, SCRATCH2);
  e().mov(host_reg(rdlo), SCRATCH2);
  e().lsr_imm(host_reg(rdhi), SCRATCH2, 32, true);
}

void Translator::arm_ldr_str(u32 instr, AOp op) {
  const bool l = instr & (1u << 20), b = instr & (1u << 22);
  const bool p = instr & (1u << 24), u = instr & (1u << 23), w = instr & (1u << 21);
  const u32 rn = (instr >> 16) & 0xF, rd = (instr >> 12) & 0xF;
  const bool writeback = !p || w;
  Operand off;
  if (op == AOp::LdrStrImm) off = {true, 0, instr & 0xFFF};
  else { Carry c; off = shift_imm((instr >> 5) & 3, (instr >> 7) & 0x1F, reg_operand(instr & 0xF, pc_ + 8), false, c, SCRATCH2, SCRATCH3); }
  emit_ea(rn, off, p, u, writeback);

  if (l) {
    if (writeback) e().mov(host_reg(rn), SCRATCH7);   // before the load: rd == rn keeps the loaded value
    // ARM9 literal load: bake the page's N32 cost.
    int const_nd = -1;
    if (a9_ && !b && op == AOp::LdrStrImm && rn == 15 && !writeback) {
      const u32 addr = u ? pc_ + 8 + (instr & 0xFFF) : pc_ + 8 - (instr & 0xFFF);
      note_dep(addr, mem::Timing::RETIME_DATA);
      const_nd = cpu_.timing9[addr >> 12][2];
    }
    if (rd == 15) {
      // Load to pc: the data cost as any load (charge_CDI before the jump), then
      // an indirect branch that charges the refill and takes a pending IRQ as the
      // interpreter's next instruction would. ARMv5 interworks on bit 0.
      emit_single(Mem::Ld32, 0, SCRATCH0, false, 0, true, const_nd, false);
      emit_branch_indirect(SCRATCH0, a9_, false, true);
      return;
    }
    emit_single(b ? Mem::Ld8 : Mem::Ld32, 0, host_reg(rd), false, 0, true, const_nd);
  } else {
    u32 data = host_reg(rd);
    if (rd == 15) { e().mov_imm(SCRATCH0, pc_ + 12); data = SCRATCH0; }
    emit_single(b ? Mem::St8 : Mem::St32, data, 0, writeback, rn, false);
  }
}

void Translator::arm_ldr_str_h(u32 instr, AOp op) {
  const bool l = instr & (1u << 20);
  const bool p = instr & (1u << 24), u = instr & (1u << 23), w = instr & (1u << 21);
  const u32 rn = (instr >> 16) & 0xF, rd = (instr >> 12) & 0xF;
  const u32 sh = (instr >> 5) & 3;
  const bool writeback = !p || w;
  Operand off;
  if (op == AOp::LdrStrHImm) off = {true, 0, ((instr >> 4) & 0xF0) | (instr & 0xF)};
  else off = {false, host_reg(instr & 0xF), 0};
  emit_ea(rn, off, p, u, writeback);

  if (l) {
    const Mem m = sh == 1 ? Mem::Ld16 : sh == 2 ? Mem::Ld8S : Mem::Ld16S;
    if (writeback) e().mov(host_reg(rn), SCRATCH7);
    emit_single(m, 0, host_reg(rd), false, 0, true);
  } else {
    emit_single(Mem::St16, host_reg(rd), 0, writeback, rn, false);
  }
}

// SWP/SWPB (fast timing): load into w7, write rd, then store. The slow stubs
// preserve x1 (the address) and x7, and only the store's slow path polls, so a
// poll that leaves at the next instruction finds rd written and nothing left to
// do. The instruction charges once, numC + two accesses, as the interpreter's
// charge_CDI after both.
void Translator::arm_swp(u32 instr, bool byte) {
  const u32 rn = (instr >> 16) & 0xF, rd = (instr >> 12) & 0xF, rm = instr & 0xF;
  e().mov(SCRATCH1, host_reg(rn));
  charge_as_ = 0;
  emit_single(byte ? Mem::Ld8 : Mem::Ld32, 0, SCRATCH7, false, 0, true, -1, false);
  u32 data = host_reg(rm);
  if (rd == rm) {   // swap w7 and rm in place: rm takes the loaded word, w7 the old rm to store
    e().eor_reg(host_reg(rm), host_reg(rm), SCRATCH7);
    e().eor_reg(SCRATCH7, SCRATCH7, host_reg(rm));
    e().eor_reg(host_reg(rm), host_reg(rm), SCRATCH7);
    data = SCRATCH7;
  } else {
    e().mov(host_reg(rd), SCRATCH7);
  }
  charge_as_ = static_cast<int>(numC_internal() + 2 * fast_data(cpu_, cur_raw_, thumb_));
  emit_single(byte ? Mem::St8 : Mem::St32, data, 0, false, 0, false);
  charge_as_ = -1;
}

void Translator::arm_ldm_stm(u32 instr, bool load) {
  const bool p = instr & (1u << 24), u = instr & (1u << 23), w = instr & (1u << 21);
  const u32 rn = (instr >> 16) & 0xF;
  const u32 list = instr & 0xFFFF;
  const u32 n = static_cast<u32>(__builtin_popcount(list));
  const u32 hb = host_reg(rn);
  if (u) {
    if (p) e().add_imm(SCRATCH1, hb, 4); else e().mov(SCRATCH1, hb);
    e().add_imm(SCRATCH7, hb, n * 4);
  } else {
    if (p) e().sub_imm(SCRATCH1, hb, n * 4); else e().sub_imm(SCRATCH1, hb, n * 4 - 4);
    e().sub_imm(SCRATCH7, hb, n * 4);
  }
  e().and_imm(SCRATCH1, SCRATCH1, ~3u);
  emit_block_transfer(instr, list, load, w, rn, a9_, pc_ + 12, (instr & (1u << 22)) != 0);
}

// Inline when the mode is unchanged and not USR (runtime tests); else interpreter.
// T is never written.
void Translator::arm_msr(u32 instr, AOp op) {
  const u32 fields = (instr >> 16) & 0xF;
  flush_pending();                 // numC charged after the tests; cold path charges via the interpreter
  u32 wv;
  if (op == AOp::MsrImm) { e().mov_imm(SCRATCH4, rotr(instr & 0xFF, ((instr >> 8) & 0xF) * 2)); wv = SCRATCH4; }
  else wv = host_reg(instr & 0xF);
  // MSR SPSR: skipped in USR/SYS.
  if (instr & (1u << 22)) {
    const u32 smask = ((fields & 1) ? 0x000000FFu : 0) | ((fields & 2) ? 0x0000FF00u : 0)
                    | ((fields & 4) ? 0x00FF0000u : 0) | ((fields & 8) ? 0xFF000000u : 0);
    add_pending(numC(pc_));
    flush_pending();
    if (!smask) return;
    e().ldr_w(SCRATCH2, R_CTX, OFF_CPSR);
    e().and_imm(SCRATCH3, SCRATCH2, 0x1F);
    e().sub_imm(SCRATCH5, SCRATCH3, 0x10, true);
    const size_t skip_user = e().cbz_fwd(SCRATCH5);
    e().sub_imm(SCRATCH5, SCRATCH3, 0x1F, true);
    const size_t skip_sys = e().cbz_fwd(SCRATCH5);
    e().ldr_w(SCRATCH2, R_CTX, OFF_SPSR);
    if (smask == 0xFFFFFFFFu) e().mov(SCRATCH2, wv);
    else {
      e().mov_imm(SCRATCH5, smask);
      e().and_reg(SCRATCH3, wv, SCRATCH5);
      e().bic_reg(SCRATCH2, SCRATCH2, SCRATCH5);
      e().orr_reg(SCRATCH2, SCRATCH2, SCRATCH3);
    }
    e().str_w(SCRATCH2, R_CTX, OFF_SPSR);
    e().bind(skip_user);
    e().bind(skip_sys);
    return;
  }
  FailList fail;
  const u32 mem_mask = ((fields & 1) ? 0xC0u : 0) | ((fields & 2) ? 0xFF00u : 0) | ((fields & 4) ? 0xFF0000u : 0) | ((fields & 8) ? 0x0F000000u : 0);
  const bool touch_mem = mem_mask != 0 || (fields & 7);
  if (touch_mem) e().ldr_w(SCRATCH2, R_CTX, OFF_CPSR);
  if (fields & 7) {
    e().and_imm(SCRATCH3, SCRATCH2, 0xF);
    fail.push_back(e().cbz_fwd(SCRATCH3));              // user mode
    if (fields & 1) {
      e().eor_reg(SCRATCH3, SCRATCH2, wv);
      e().and_imm(SCRATCH3, SCRATCH3, 0x1F);
      fail.push_back(e().cbnz_fwd(SCRATCH3));           // mode change
    }
  }
  add_pending(numC(pc_));
  flush_pending();
  if (mem_mask) {
    if (mem_mask == 0xC0) { e().ubfx(SCRATCH3, wv, 6, 2); e().bfi(SCRATCH2, SCRATCH3, 6, 2); }
    else {
      e().mov_imm(SCRATCH5, mem_mask);
      e().and_reg(SCRATCH3, wv, SCRATCH5);
      e().bic_reg(SCRATCH2, SCRATCH2, SCRATCH5);
      e().orr_reg(SCRATCH2, SCRATCH2, SCRATCH3);
    }
    e().str_w(SCRATCH2, R_CTX, OFF_CPSR);
  }
  if (fields & 8) e().msr_nzcv(wv);
  if (fields & 1) { call_stub(rt().poll); e().word(make_key(pc_ + 4, thumb_)); }   // I may have changed
  if (fail.empty()) return;
  const size_t join = hot_.size();
  cold_begin(fail);
  // Mode change / user mode. call_pure carries r8-r12 and flags; r13/r14 by hand.
  // numC charged here: the hot path charges it after the mode tests.
  e().sub_imm(R_BUDGET, R_BUDGET, numC(pc_), true);
  e().str_w(host_reg(13), R_CTX, off_reg(13));
  e().str_w(host_reg(14), R_CTX, off_reg(14));
  e().mov(SCRATCH1, wv);
  e().mov_imm(SCRATCH2, ((fields & 1) ? 0x000000FFu : 0) | ((fields & 2) ? 0x0000FF00u : 0)
                      | ((fields & 4) ? 0x00FF0000u : 0) | ((fields & 8) ? 0xFF000000u : 0));
  e().mov(SCRATCH0, R_CTX, true);
  e().mov_imm64(R_FN, reinterpret_cast<u64>(&jit_h_msr_cpsr));
  call_stub(rt().call_pure);
  e().ldr_w(host_reg(13), R_CTX, off_reg(13));
  e().ldr_w(host_reg(14), R_CTX, off_reg(14));
  if (fields & 1) { call_stub(rt().poll); e().word(make_key(pc_ + 4, thumb_)); }   // I may have changed
  cold_end_jump(join);
}

// Conditional DP predicable by csel. Register shifts excluded: cost is not plain numC.
bool use_csel_op(AOp op, u32 instr) {
  if (op != AOp::DpImm && op != AOp::DpImmShift) return false;
  if (instr & (1u << 20)) return false;
  const u32 opcode = (instr >> 21) & 0xF;
  if (opcode >= 8 && opcode <= 0xB) return false;
  return ((instr >> 12) & 0xF) != 15;
}

// Cost is static; conditional forms charge numC before the condition test.
bool arm_simple_cost(AOp op) {
  switch (op) {
  case AOp::DpImm: case AOp::DpImmShift: case AOp::DpRegShift: case AOp::Mrs: case AOp::Clz: case AOp::Pld: case AOp::Mcr:
  case AOp::Mul: case AOp::Mla: case AOp::Umull: case AOp::Umlal: case AOp::Smull: case AOp::Smlal:
  case AOp::SmlaXY: case AOp::SmulXY: case AOp::SmlawY: case AOp::SmulwY:
    return true;
  default: return false;
  }
}

void Translator::translate_arm(u32 instr) {
  const u32 cond = instr >> 28;
  if (cond == 0xF) {
    if (((instr >> 25) & 7) == 5 && a9_) {       // BLX imm
      s32 off = static_cast<s32>(instr << 8) >> 6;
      off |= (instr >> 23) & 2;
      e().mov_imm(host_reg(14), pc_ + 4);
      emit_branch_static((pc_ + 8 + static_cast<u32>(off)) & ~1u, true, true);
      return;
    }
    if (((instr >> 24) & 0xF7) == 0x55) { add_pending(numC(pc_)); return; }   // PLD
    emit_fallback(instr, true);
    return;
  }
  const AOp op = arm::decode_arm(instr);

  if (arm_needs_fallback(instr, a9_)) {
    bool always = false;
    switch (op) {
    case AOp::Swi: break;   // not a sure jump: see translate_thumb
    case AOp::Bkpt: case AOp::Undefined: case AOp::Cdp: case AOp::Ldc: case AOp::Stc:
      always = cond == 0xE; break;
    case AOp::DpImm: case AOp::DpImmShift: case AOp::DpRegShift:
    case AOp::LdrStrImm: case AOp::LdrStrReg: case AOp::LdrStrHImm: case AOp::LdrStrHReg: case AOp::Ldm:
      always = cond == 0xE && (op == AOp::Ldm ? (instr & 0x8000) != 0 : (((instr >> 12) & 0xF) == 15 && (op != AOp::LdrStrImm && op != AOp::LdrStrReg && op != AOp::LdrStrHImm && op != AOp::LdrStrHReg ? true : (instr & (1u << 20)) != 0)));
      break;
    default: break;
    }
    emit_fallback(instr, always);
    return;
  }

  size_t skip = 0;
  const bool conditional = cond != 0xE;
  // csel form: result in x6, committed by the select; cost charged unconditionally.
  const bool csel_form = conditional && use_csel_op(op, instr);
  // A pc write ends the block: its not-taken path charges numC in the epilogue.
  const bool dp_pc = (op == AOp::DpImm || op == AOp::DpImmShift) && ((instr >> 12) & 0xF) == 15;
  const bool precharged = conditional && !csel_form && !dp_pc && arm_simple_cost(op);
  if (conditional && !csel_form) {
    if (precharged) { const u32 c = numC(pc_); add_pending(c); flush_pending(); charged_ahead_ = c; }
    else flush_pending();
    skip = hot_.b_cond_fwd(invert(static_cast<Cond>(cond)));
  }
  dp_csel_ = csel_form;

  switch (op) {
  case AOp::DpImm: case AOp::DpImmShift: case AOp::DpRegShift:
    if (dp_pc && !(instr & (1u << 20)) && (((instr >> 21) & 0xF) < 8 || ((instr >> 21) & 0xF) > 0xB)) {
      // Jump table: the result into w6, then the refill as any ARMv5 data-processing
      // write to pc (no interworking; the stub aligns the target).
      dp_csel_ = true;
      arm_data_processing(instr, op);
      dp_csel_ = false;
      emit_branch_indirect(SCRATCH6, false);
      break;
    }
    arm_data_processing(instr, op);
    break;
  case AOp::Mrs: {
    const u32 rd = (instr >> 12) & 0xF;
    add_pending(numC(pc_));
    if (rd == 15) break;
    if (instr & (1u << 22)) e().ldr_w(host_reg(rd), R_CTX, OFF_SPSR);
    else {
      e().mrs_nzcv(SCRATCH1);
      e().ldr_w(SCRATCH2, R_CTX, OFF_CPSR);
      e().and_imm(SCRATCH2, SCRATCH2, 0x0FFFFFFF);
      e().orr_reg(host_reg(rd), SCRATCH2, SCRATCH1);
    }
    break;
  }
  case AOp::B: case AOp::Bl: {
    const s32 off = static_cast<s32>(instr << 8) >> 6;
    if (op == AOp::Bl) e().mov_imm(host_reg(14), pc_ + 4);
    emit_branch_static(pc_ + 8 + static_cast<u32>(off), false, true);
    break;
  }
  case AOp::Bx: {
    const u32 rm = instr & 0xF;
    if (rm == 15) e().mov_imm(SCRATCH0, pc_ + 8); else e().mov(SCRATCH0, host_reg(rm));
    emit_branch_indirect(SCRATCH0, true);
    break;
  }
  case AOp::BlxReg: {
    const u32 rm = instr & 0xF;
    if (rm == 15) e().mov_imm(SCRATCH0, pc_ + 8); else e().mov(SCRATCH0, host_reg(rm));
    e().mov_imm(host_reg(14), pc_ + 4);
    emit_branch_indirect(SCRATCH0, true);
    break;
  }
  case AOp::Clz: {
    const u32 rd = (instr >> 12) & 0xF, rm = instr & 0xF;
    add_pending(numC(pc_));
    if (rd == 15 || rm == 15) break;
    e().clz(host_reg(rd), host_reg(rm));
    break;
  }
  case AOp::Pld: add_pending(numC(pc_)); break;
  case AOp::Mcr: add_pending(numC(pc_) + 2); break;      // ignored cache op: charge_CI(2)
  case AOp::Mrc: arm_mrc(instr); break;
  case AOp::SmlalXY: arm_smlal_xy(instr); break;
  case AOp::MsrReg: case AOp::MsrImm: arm_msr(instr, op); break;
  case AOp::SmlaXY: case AOp::SmulXY: case AOp::SmlawY: case AOp::SmulwY:
    arm_dsp_multiply(instr, op);
    break;
  case AOp::Mul: case AOp::Mla: case AOp::Umull: case AOp::Umlal: case AOp::Smull: case AOp::Smlal:
    arm_multiply(instr, op);
    break;
  case AOp::LdrStrImm: case AOp::LdrStrReg: arm_ldr_str(instr, op); break;
  case AOp::LdrStrHImm: case AOp::LdrStrHReg: arm_ldr_str_h(instr, op); break;
  case AOp::Ldm: arm_ldm_stm(instr, true); break;
  case AOp::Stm: arm_ldm_stm(instr, false); break;
  case AOp::Swp: case AOp::Swpb: arm_swp(instr, op == AOp::Swpb); break;
  default: break;
  }

  if (csel_form) {
    dp_csel_ = false;
    const u32 rd = (instr >> 12) & 0xF;
    e().csel(host_reg(rd), SCRATCH6, host_reg(rd), static_cast<Cond>(cond));
    return;
  }

  if (conditional) {
    if (ended_) {
      hot_.bind(skip);
      add_pending(numC(pc_));
      emit_branch_static(pc_ + 4, false, false);
    } else if (precharged) {
      flush_pending();
      assert(charged_ahead_ == 0 && "a simple-cost instruction charged less than numC");
      charged_ahead_ = 0;
      hot_.bind(skip);
    } else {
      flush_pending();
      size_t join = hot_.b_fwd();
      hot_.bind(skip);
      add_pending(numC(pc_));
      flush_pending();
      hot_.bind(join);
    }
  }
}

// ---- Thumb -------------------------------------------------------------------------------------------------------

void Translator::thumb_alu(u16 instr) {
  const u32 rs = (instr >> 3) & 7, rd = instr & 7;
  const u32 hs = host_reg(rs), hd = host_reg(rd);
  const u32 aluop = (instr >> 6) & 0xF;
  switch (aluop) {
  case 0x2: case 0x3: case 0x4: case 0x7: {   // LSL LSR ASR ROR by register
    add_pending(numC_internal() + 1);
    Carry c;
    const u32 type = aluop == 2 ? 0 : aluop == 3 ? 1 : aluop == 4 ? 2 : 3;
    Operand r = shift_reg(type, hs, Operand{false, hd, 0}, live_ & F_C, c, SCRATCH2, SCRATCH3);
    e().mov(hd, r.reg);
    set_flags_logical(hd, c);
    return;
  }
  case 0xD: {   // MUL
    if (a9_) add_pending(numC(pc_) + 3);
    else { add_pending(numC_nonseq7()); flush_pending(); emit_mul_cycles7(hd, true, 0); }
    e().mul(hd, hd, hs);
    set_flags_logical(hd, mul_carry());
    return;
  }
  default: break;
  }
  add_pending(numC(pc_));
  switch (aluop) {
  case 0x0: e().and_reg(hd, hd, hs); set_flags_logical(hd, Carry{}); break;
  case 0x1: e().eor_reg(hd, hd, hs); set_flags_logical(hd, Carry{}); break;
  case 0x5: e().adc(hd, hd, hs, true); break;
  case 0x6: e().sbc(hd, hd, hs, true); break;
  case 0x8: e().and_reg(SCRATCH6, hd, hs); set_flags_logical(SCRATCH6, Carry{}); break;
  case 0x9: e().negs(hd, hs); break;
  case 0xA: e().cmp_reg(hd, hs); break;
  case 0xB: e().cmn_reg(hd, hs); break;
  case 0xC: e().orr_reg(hd, hd, hs); set_flags_logical(hd, Carry{}); break;
  case 0xE: e().bic_reg(hd, hd, hs); set_flags_logical(hd, Carry{}); break;
  default:  e().mvn(hd, hs); set_flags_logical(hd, Carry{}); break;
  }
}

void Translator::thumb_ldr_str(u16 instr, TOp op) {
  Mem m;
  u32 rd;
  int const_nd = -1;
  switch (op) {
  case TOp::LdrPcRel: {
    rd = (instr >> 8) & 7;
    const u32 addr = ((pc_ + 4) & ~3u) + ((instr & 0xFF) << 2);
    e().mov_imm(SCRATCH1, addr);
    m = Mem::Ld32;
    if (a9_) { note_dep(addr, mem::Timing::RETIME_DATA); const_nd = cpu_.timing9[addr >> 12][2]; }
    break;
  }
  case TOp::LdrStrReg: {
    const u32 ro = (instr >> 6) & 7, rb = (instr >> 3) & 7;
    rd = instr & 7;
    e().add_reg(SCRATCH1, host_reg(rb), host_reg(ro));
    static const Mem kinds[8] = {Mem::St32, Mem::St16, Mem::St8, Mem::Ld8S, Mem::Ld32, Mem::Ld16, Mem::Ld8, Mem::Ld16S};
    m = kinds[(instr >> 9) & 7];
    break;
  }
  case TOp::LdrStrImm5: {
    const u32 rb = (instr >> 3) & 7, imm = (instr >> 6) & 0x1F;
    rd = instr & 7;
    const bool l = instr & (1 << 11), b = instr & (1 << 12);
    e().add_imm(SCRATCH1, host_reg(rb), b ? imm : imm * 4);
    m = b ? (l ? Mem::Ld8 : Mem::St8) : (l ? Mem::Ld32 : Mem::St32);
    break;
  }
  case TOp::LdrStrHImm5: {
    const u32 rb = (instr >> 3) & 7;
    rd = instr & 7;
    e().add_imm(SCRATCH1, host_reg(rb), ((instr >> 6) & 0x1F) * 2);
    m = (instr & (1 << 11)) ? Mem::Ld16 : Mem::St16;
    break;
  }
  default: {   // LdrStrSpRel
    rd = (instr >> 8) & 7;
    e().add_imm(SCRATCH1, host_reg(13), (instr & 0xFF) * 4);
    m = (instr & (1 << 11)) ? Mem::Ld32 : Mem::St32;
    break;
  }
  }
  if (is_load(m)) emit_single(m, 0, host_reg(rd), false, 0, true, const_nd);
  else emit_single(m, host_reg(rd), 0, false, 0, false);
}

void Translator::translate_thumb(u16 instr) {
  const TOp op = arm::decode_thumb(instr);
  const bool was_prefix = bl_prefix_valid_;
  bl_prefix_valid_ = false;
  if (thumb_needs_fallback(instr, a9_)) {
    // SWI may return into the block (DSi HLE SWI), so it is not a sure jump.
    const bool always = op == TOp::Bkpt || op == TOp::Undefined || op == TOp::BxBlx || op == TOp::BlxSuffix;
    emit_fallback(instr, always);
    return;
  }
  switch (op) {
  case TOp::ShiftImm: {
    const u32 type = (instr >> 11) & 3, amt = (instr >> 6) & 0x1F, rs = (instr >> 3) & 7, rd = instr & 7;
    add_pending(numC(pc_));
    Carry c;
    Operand r = shift_imm(type, amt, Operand{false, host_reg(rs), 0}, live_ & F_C, c, SCRATCH2, SCRATCH3);
    if (r.imm) e().mov_imm(host_reg(rd), r.value); else if (r.reg != host_reg(rd)) e().mov(host_reg(rd), r.reg);
    set_flags_logical(host_reg(rd), c);
    return;
  }
  case TOp::AddSubReg: {
    const u32 rn = (instr >> 6) & 7, rs = (instr >> 3) & 7, rd = instr & 7;
    add_pending(numC(pc_));
    e().add_sub(instr & (1 << 9), true, host_reg(rd), host_reg(rs), host_reg(rn));
    return;
  }
  case TOp::AddSubImm3: {
    const u32 imm = (instr >> 6) & 7, rs = (instr >> 3) & 7, rd = instr & 7;
    add_pending(numC(pc_));
    if (instr & (1 << 9)) e().subs_imm(host_reg(rd), host_reg(rs), imm); else e().adds_imm(host_reg(rd), host_reg(rs), imm);
    return;
  }
  case TOp::MovCmpAddSubImm8: {
    const u32 rd = (instr >> 8) & 7, imm = instr & 0xFF;
    add_pending(numC(pc_));
    switch ((instr >> 11) & 3) {
    case 0: e().movz(host_reg(rd), imm); set_flags_logical(host_reg(rd), Carry{}); break;
    case 1: e().cmp_imm(host_reg(rd), imm); break;
    case 2: e().adds_imm(host_reg(rd), host_reg(rd), imm); break;
    default: e().subs_imm(host_reg(rd), host_reg(rd), imm); break;
    }
    return;
  }
  case TOp::Alu: thumb_alu(instr); return;
  case TOp::HiRegOp: {
    const u32 rd = (instr & 7) | ((instr >> 4) & 8), rs = (instr >> 3) & 0xF;
    const Operand b = reg_operand(rs, pc_ + 4);
    switch ((instr >> 8) & 3) {
    case 0:
      if (rd == 15) {
        add_pending(numC(pc_));
        if (b.imm) e().mov_imm(SCRATCH0, pc_ + 4 + b.value); else e().add_imm_any(SCRATCH0, b.reg, pc_ + 4, SCRATCH2);
        emit_branch_indirect(SCRATCH0, false);
        return;
      }
      add_pending(numC(pc_));
      if (b.imm) e().add_imm_any(host_reg(rd), host_reg(rd), b.value, SCRATCH2); else e().add_reg(host_reg(rd), host_reg(rd), b.reg);
      return;
    case 1:
      add_pending(numC(pc_));
      { const u32 a = to_reg(reg_operand(rd, pc_ + 4), SCRATCH4); const u32 bb = to_reg(b, SCRATCH5); e().cmp_reg(a, bb); }
      return;
    case 2:
      if (rd == 15) {
        add_pending(numC(pc_));
        if (b.imm) e().mov_imm(SCRATCH0, b.value); else e().mov(SCRATCH0, b.reg);
        emit_branch_indirect(SCRATCH0, false);
        return;
      }
      add_pending(numC(pc_));
      if (b.imm) e().mov_imm(host_reg(rd), b.value); else e().mov(host_reg(rd), b.reg);
      return;
    default:
      add_pending(numC(pc_));
      return;
    }
  }
  case TOp::BxBlx: {
    const u32 rs = (instr >> 3) & 0xF;
    if (instr & (1 << 7)) {
      if (!a9_) { emit_fallback(instr, true); return; }
      if (rs == 15) e().mov_imm(SCRATCH0, pc_ + 4); else e().mov(SCRATCH0, host_reg(rs));
      e().mov_imm(host_reg(14), (pc_ + 2) | 1);
      emit_branch_indirect(SCRATCH0, true);
      return;
    }
    if (rs == 15) e().mov_imm(SCRATCH0, pc_ + 4); else e().mov(SCRATCH0, host_reg(rs));
    emit_branch_indirect(SCRATCH0, true);
    return;
  }
  case TOp::LdrPcRel: case TOp::LdrStrReg: case TOp::LdrStrImm5: case TOp::LdrStrHImm5: case TOp::LdrStrSpRel:
    thumb_ldr_str(instr, op);
    return;
  case TOp::AddPcSp: {
    const u32 rd = (instr >> 8) & 7, imm = (instr & 0xFF) * 4;
    add_pending(numC(pc_));
    if (instr & (1 << 11)) e().add_imm(host_reg(rd), host_reg(13), imm);
    else e().mov_imm(host_reg(rd), ((pc_ + 4) & ~3u) + imm);
    return;
  }
  case TOp::AdjustSp: {
    const u32 imm = (instr & 0x7F) * 4;
    add_pending(numC(pc_));
    if (instr & (1 << 7)) e().sub_imm(host_reg(13), host_reg(13), imm); else e().add_imm(host_reg(13), host_reg(13), imm);
    return;
  }
  case TOp::PushPop: {
    u32 list = instr & 0xFF;
    const bool pop = instr & (1 << 11), r = instr & (1 << 8);
    if (pop) {
      if (r) list |= 0x8000;
      const u32 n = static_cast<u32>(__builtin_popcount(list));
      if (n == 0) { emit_fallback(instr, false); return; }
      e().mov(SCRATCH1, host_reg(13));
      e().add_imm(SCRATCH7, host_reg(13), n * 4);
      emit_block_transfer(instr, list, true, true, 13, a9_, 0);
      return;
    }
    if (r) list |= 0x4000;
    const u32 n = static_cast<u32>(__builtin_popcount(list));
    if (n == 0) { emit_fallback(instr, false); return; }
    e().sub_imm(SCRATCH1, host_reg(13), n * 4);
    e().mov(SCRATCH7, SCRATCH1);
    emit_block_transfer(instr, list, false, true, 13, false, 0);
    return;
  }
  case TOp::StmLdm: {
    const u32 rb = (instr >> 8) & 7, list = instr & 0xFF;
    const bool load = instr & (1 << 11);
    if (list == 0) { emit_fallback(instr, load); return; }
    const u32 n = static_cast<u32>(__builtin_popcount(list));
    e().mov(SCRATCH1, host_reg(rb));
    e().add_imm(SCRATCH7, host_reg(rb), n * 4);
    emit_block_transfer(instr, list, load, true, rb, false, 0);
    return;
  }
  case TOp::BCond: {
    const u32 cond = (instr >> 8) & 0xF;
    const s32 off = static_cast<s8>(instr & 0xFF) * 2;
    flush_pending();
    size_t skip = hot_.b_cond_fwd(invert(static_cast<Cond>(cond)));
    emit_branch_static(pc_ + 4 + static_cast<u32>(off), true, true);
    hot_.bind(skip);
    add_pending(numC(pc_));
    emit_branch_static(pc_ + 2, true, false);
    return;
  }
  case TOp::B: {
    const s32 off = static_cast<s32>(static_cast<u32>(instr) << 21) >> 20;
    emit_branch_static(pc_ + 4 + static_cast<u32>(off), true, true);
    return;
  }
  case TOp::BlPrefix: {
    const s32 off = static_cast<s32>(static_cast<u32>(instr) << 21) >> 9;
    add_pending(numC(pc_));
    bl_prefix_lr_ = pc_ + 4 + static_cast<u32>(off);
    e().mov_imm(host_reg(14), bl_prefix_lr_);
    bl_prefix_valid_ = true;
    return;
  }
  case TOp::BlSuffix: case TOp::BlxSuffix: {
    const bool blx = op == TOp::BlxSuffix;
    if (blx && !a9_) { emit_fallback(instr, true); return; }
    const u32 ret = (pc_ + 2) | 1;
    if (was_prefix) {
      u32 target = bl_prefix_lr_ + ((instr & 0x7FF) << 1);
      e().mov_imm(host_reg(14), ret);
      if (blx) emit_branch_static(target & ~3u, false, true);
      else emit_branch_static(target & ~1u, true, true);
      return;
    }
    e().add_imm_any(SCRATCH0, host_reg(14), (instr & 0x7FF) << 1, SCRATCH2);
    e().mov_imm(host_reg(14), ret);
    if (blx) e().and_imm(SCRATCH0, SCRATCH0, ~3u); else e().orr_imm(SCRATCH0, SCRATCH0, 1);
    emit_branch_indirect(SCRATCH0, true);
    return;
  }
  case TOp::Swi: emit_fallback(instr, false); return;
  case TOp::Bkpt: emit_fallback(instr, true); return;
  case TOp::Undefined: emit_fallback(instr, true); return;
  }
}

// ---- block driver ---------------------------------------------------------------------------------------------------

bool Translator::run() {
  const u32 start = key_pc(key_);
  if (!a9_) { t7_ = cpu_.timing7[start >> 15]; code_region7_ = start >> 24; }

  u32 addr = start;
  u64 t_ph = prof::enabled ? prof::now_ns() : 0;
  auto phase = [&](prof::Stage s) { if (prof::enabled) { const u64 n = prof::now_ns(); prof::add_timed(s, n - t_ph); t_ph = n; } };
  for (u32 i = 0; i < MAX_INSTRS; ++i) {
    const u32 raw = fetch(addr);
    instrs_.push_back({addr, raw, F_ALL});
    const bool ends = thumb_ ? shape::thumb_ends_block(static_cast<u16>(raw)) : shape::arm_ends_block(raw, a9_);
    addr += step();
    if (ends) break;
    if (!cpu_.page_table.read_ptr(addr)) break;
  }

  phase(prof::JIT_TX_DECODE);
  // Backward flag liveness, from what the static successors need at the exit.
  u32 span_lo = start, span_hi = addr - 1;
  u32 live = exit_flag_live(addr, span_lo, span_hi);
  for (size_t i = instrs_.size(); i-- > 0;) {
    instrs_[i].live_out = live;
    const FlagUse u = thumb_ ? thumb_flag_use(static_cast<u16>(instrs_[i].raw), a9_) : arm_flag_use(instrs_[i].raw, a9_);
    live = u.reads | (live & ~u.writes);
  }

  phase(prof::JIT_TX_LIVE);
  // DS_JIT_CENSUS static facts, stored in the density slot.
  const bool census = rt().census;
  u16 c_live_in = 0, c_written = 0;
  u32 c_mem = 0, c_memfl = 0, c_memfl_intra = 0, c_fl = 0, c_fb = 0;
  u16 c_reads[16] = {}, c_writes[16] = {};
  std::vector<u32> intra_live;   // liveness assuming nothing live at block end
  if (census) {
    intra_live.assign(instrs_.size(), 0);
    u32 il = 0;
    for (size_t i = instrs_.size(); i-- > 0;) {
      intra_live[i] = il;
      const FlagUse u = thumb_ ? thumb_flag_use(static_cast<u16>(instrs_[i].raw), a9_) : arm_flag_use(instrs_[i].raw, a9_);
      il = u.reads | (il & ~u.writes);
    }
    for (size_t i = 0; i < instrs_.size(); ++i) {
      const u32 raw = instrs_[i].raw;
      const RegUse u = thumb_ ? thumb_reg_use(static_cast<u16>(raw)) : arm_reg_use(raw);
      for (int r = 0; r < 16; ++r) {
        if (u.reads & (1u << r)) { ++c_reads[r]; if (!(c_written & (1u << r))) c_live_in |= static_cast<u16>(1u << r); }
        if (u.writes & (1u << r)) ++c_writes[r];
      }
      c_written |= u.writes;
      const bool mem = thumb_ ? thumb_is_mem(static_cast<u16>(raw)) : arm_is_mem(raw);
      const bool flags_live = instrs_[i].live_out != 0;
      if (mem) { ++c_mem; if (flags_live) ++c_memfl; if (intra_live[i]) ++c_memfl_intra; }
      if (flags_live) ++c_fl;
      if (thumb_ ? thumb_needs_fallback(static_cast<u16>(raw), a9_) : arm_needs_fallback(raw, a9_)) ++c_fb;
    }
  }

  if (rt().density) emit_density_bump();
  if (census && dslot_) {
    for (int r = 0; r < 16; ++r) { dslot_->reg_reads[r] = c_reads[r]; dslot_->reg_writes[r] = c_writes[r]; }
    dslot_->live_in = c_live_in; dslot_->written = c_written;
    dslot_->n_instrs = static_cast<u8>(instrs_.size()); dslot_->n_fallback = static_cast<u8>(c_fb);
    dslot_->n_mem = static_cast<u8>(c_mem); dslot_->n_mem_flags_live = static_cast<u8>(c_memfl); dslot_->n_mem_flags_intra = static_cast<u8>(c_memfl_intra);
    dslot_->n_flags_live = static_cast<u8>(c_fl); dslot_->entry_flags_live = live != 0;
  }
  emit_budget_check(key_);
  // DSi BIOS SHA-1 loop head: try the native loop first.
  if (a9_ && bios_sha1_hook_wanted(cpu_, start, thumb_)) {
    flush_pending();
    call_stub(jc_.fallback);
    e().word(BIOS_SHA1_MARKER);
    e().word(make_key(start - 4, false));
  }
  // ARM7 BIOS WaitByLoop head: skip the iterations the budget covers, then
  // the loop below runs the rest.
  if (!a9_ && cpu::wait_loop_hook_wanted(cpu_, start, thumb_)) {
    flush_pending();
    call_stub(jc_.fallback);
    e().word(WAIT_LOOP_MARKER);
    e().word(make_key(start - 2, true));
  }

  u32 end_addr = addr;
  for (size_t i = 0; i < instrs_.size(); ++i) {
    const Instr& in = instrs_[i];
    if (i > 0 && hot_.size() + cold_.size() > BLOCK_LIMIT) { end_addr = in.addr; break; }
    if (hot_.remaining() < 8192 || cold_.remaining() < 4096) return false;
    pc_ = in.addr;
    live_ = in.live_out;
    if (rt().trace) { flush_pending(); emit_trace(in.raw); }
    if (rt().cyclog) { flush_pending(); emit_call2(reinterpret_cast<const void*>(&jit_h_cyclog), in.raw, make_key(in.addr, thumb_)); }
    cur_raw_ = in.raw;
    if (thumb_) translate_thumb(static_cast<u16>(in.raw)); else translate_arm(in.raw);
    rt().stats.instrs_translated++;
    ++dinstrs_;
    if (ended_) break;
    if (rt().strict) {
      // Budget check per instruction, like the interpreter.
      flush_pending();
      emit_budget_check(make_key(in.addr + step(), thumb_));
    }
  }
  blk_.guest_len = end_addr - start;
  if (span_lo != start || span_hi != addr - 1) { blk_.span_lo = span_lo; blk_.span_hi = span_hi; }
  if (!ended_) emit_branch_static(end_addr, thumb_, false);
  phase(prof::JIT_TX_EMIT);
  const bool ok = finish();
  phase(prof::JIT_TX_FINISH);
  return ok;
}

} // namespace

bool backend::translate_block(JitCpu& jc, u32 key, u8* buf, size_t cap, Block& b, u32& size) {
  Emitter e(buf, cap);
  Translator t(jc, key, e, b);
  if (!t.run()) return false;
  size = static_cast<u32>(e.size());
  return true;
}

} // namespace ds::jit
