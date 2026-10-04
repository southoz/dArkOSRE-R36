// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Minimal A32 (ARM state, ARMv7-A) instruction encoder for the recompiler;
// only the forms the translator and runtime stubs use. Every instruction
// takes a condition (default AL); registers are plain numbers 0-15. Always
// ARM state, never Thumb-2. C helpers are Thumb-2 on the handheld
// toolchains: call them with `blx <reg>`, never `bl`.
#pragma once
#include "core/types.h"

#include <cassert>
#include <cstring>

namespace ds::jit {

enum Cond : u32 { EQ = 0, NE, CS, CC, MI, PL, VS, VC, HI, LS, GE, LT, GT, LE, AL, NV };
enum Shift : u32 { LSL = 0, LSR = 1, ASR = 2, ROR = 3 };
enum DpOp : u32 { AND = 0, EOR, SUB, RSB, ADD, ADC, SBC, RSC, TST, TEQ, CMP, CMN, ORR, MOV, BIC, MVN };

inline Cond invert(Cond c) { return static_cast<Cond>(static_cast<u32>(c) ^ 1); }

// Encode `v` as an A32 modified immediate (8 bits rotated right by an even
// amount). False when it cannot be.
inline bool encode_imm12(u32 v, u32& imm12) {
  for (u32 rot = 0; rot < 32; rot += 2) {
    const u32 r = (v << rot) | (rot ? (v >> (32 - rot)) : 0);   // rotate left by rot = undo a rotate right
    if ((r & ~0xFFu) == 0) { imm12 = ((rot / 2) << 8) | r; return true; }
  }
  return false;
}

class Emitter {
public:
  Emitter(u8* base, size_t cap) : base_(base), cap_(cap), pos_(0) {}

  u8*    base() const { return base_; }
  u8*    cur() const { return base_ + pos_; }
  size_t size() const { return pos_; }
  size_t remaining() const { return cap_ - pos_; }
  void   set_pos(size_t p) { pos_ = p; }

  void emit(u32 w) {
    assert(pos_ + 4 <= cap_);
    std::memcpy(base_ + pos_, &w, 4);
    pos_ += 4;
  }
  void word(u32 w) { emit(w); }
  static void patch(u8* at, u32 w) { std::memcpy(at, &w, 4); }
  static u32 read(const u8* at) { u32 w; std::memcpy(&w, at, 4); return w; }

  // ---- data processing ------------------------------------------------------
  void dp_imm(DpOp op, bool s, u32 rd, u32 rn, u32 imm12, Cond c = AL) {
    emit((c << 28) | (1u << 25) | (op << 21) | (s ? (1u << 20) : 0) | (rn << 16) | (rd << 12) | imm12);
  }
  void dp_reg(DpOp op, bool s, u32 rd, u32 rn, u32 rm, Shift sh = LSL, u32 amt = 0, Cond c = AL) {
    assert(amt < 32);
    emit((c << 28) | (op << 21) | (s ? (1u << 20) : 0) | (rn << 16) | (rd << 12) | (amt << 7) | (sh << 5) | rm);
  }
  void dp_regshift(DpOp op, bool s, u32 rd, u32 rn, u32 rm, Shift sh, u32 rs, Cond c = AL) {
    emit((c << 28) | (op << 21) | (s ? (1u << 20) : 0) | (rn << 16) | (rd << 12) | (rs << 8) | (sh << 5) | (1u << 4) | rm);
  }

  void mov(u32 rd, u32 rm, Cond c = AL) { dp_reg(MOV, false, rd, 0, rm, LSL, 0, c); }
  void movs(u32 rd, u32 rm, Cond c = AL) { dp_reg(MOV, true, rd, 0, rm, LSL, 0, c); }
  void mvn(u32 rd, u32 rm, Cond c = AL) { dp_reg(MVN, false, rd, 0, rm, LSL, 0, c); }
  void movw(u32 rd, u32 imm16, Cond c = AL) { emit((c << 28) | 0x03000000u | ((imm16 >> 12) << 16) | (rd << 12) | (imm16 & 0xFFF)); }
  void movt(u32 rd, u32 imm16, Cond c = AL) { emit((c << 28) | 0x03400000u | ((imm16 >> 12) << 16) | (rd << 12) | (imm16 & 0xFFF)); }
  void mov_imm(u32 rd, u32 v, Cond c = AL) {
    u32 i;
    if (encode_imm12(v, i)) { dp_imm(MOV, false, rd, 0, i, c); return; }
    if (encode_imm12(~v, i)) { dp_imm(MVN, false, rd, 0, i, c); return; }
    movw(rd, v & 0xFFFF, c);
    if (v >> 16) movt(rd, v >> 16, c);
  }
  void mov_ptr(u32 rd, const void* p, Cond c = AL) { mov_imm(rd, static_cast<u32>(reinterpret_cast<uintptr_t>(p)), c); }

  void lsl_imm(u32 rd, u32 rm, u32 amt, Cond c = AL) { dp_reg(MOV, false, rd, 0, rm, LSL, amt, c); }
  void lsr_imm(u32 rd, u32 rm, u32 amt, Cond c = AL) { dp_reg(MOV, false, rd, 0, rm, LSR, amt & 31, c); }
  void asr_imm(u32 rd, u32 rm, u32 amt, Cond c = AL) { dp_reg(MOV, false, rd, 0, rm, ASR, amt & 31, c); }
  void ror_imm(u32 rd, u32 rm, u32 amt, Cond c = AL) { dp_reg(MOV, false, rd, 0, rm, ROR, amt, c); }
  void lsl_reg(u32 rd, u32 rm, u32 rs, Cond c = AL) { dp_regshift(MOV, false, rd, 0, rm, LSL, rs, c); }
  void lsr_reg(u32 rd, u32 rm, u32 rs, Cond c = AL) { dp_regshift(MOV, false, rd, 0, rm, LSR, rs, c); }
  void asr_reg(u32 rd, u32 rm, u32 rs, Cond c = AL) { dp_regshift(MOV, false, rd, 0, rm, ASR, rs, c); }
  void ror_reg(u32 rd, u32 rm, u32 rs, Cond c = AL) { dp_regshift(MOV, false, rd, 0, rm, ROR, rs, c); }

  void add_imm(u32 rd, u32 rn, u32 v, u32 tmp, bool s = false, Cond c = AL) {
    u32 i;
    if (encode_imm12(v, i)) { dp_imm(ADD, s, rd, rn, i, c); return; }
    if (encode_imm12(0u - v, i)) { dp_imm(SUB, s, rd, rn, i, c); return; }
    mov_imm(tmp, v, c); dp_reg(ADD, s, rd, rn, tmp, LSL, 0, c);
  }
  void sub_imm(u32 rd, u32 rn, u32 v, u32 tmp, bool s = false, Cond c = AL) { add_imm(rd, rn, 0u - v, tmp, s, c); }
  void add_reg(u32 rd, u32 rn, u32 rm, Shift sh = LSL, u32 amt = 0, Cond c = AL) { dp_reg(ADD, false, rd, rn, rm, sh, amt, c); }
  void sub_reg(u32 rd, u32 rn, u32 rm, Shift sh = LSL, u32 amt = 0, Cond c = AL) { dp_reg(SUB, false, rd, rn, rm, sh, amt, c); }
  void rsb_reg(u32 rd, u32 rn, u32 rm, Shift sh = LSL, u32 amt = 0, Cond c = AL) { dp_reg(RSB, false, rd, rn, rm, sh, amt, c); }
  void and_reg(u32 rd, u32 rn, u32 rm, Shift sh = LSL, u32 amt = 0, Cond c = AL) { dp_reg(AND, false, rd, rn, rm, sh, amt, c); }
  void orr_reg(u32 rd, u32 rn, u32 rm, Shift sh = LSL, u32 amt = 0, Cond c = AL) { dp_reg(ORR, false, rd, rn, rm, sh, amt, c); }
  void eor_reg(u32 rd, u32 rn, u32 rm, Shift sh = LSL, u32 amt = 0, Cond c = AL) { dp_reg(EOR, false, rd, rn, rm, sh, amt, c); }
  void bic_reg(u32 rd, u32 rn, u32 rm, Shift sh = LSL, u32 amt = 0, Cond c = AL) { dp_reg(BIC, false, rd, rn, rm, sh, amt, c); }
  bool and_imm(u32 rd, u32 rn, u32 v, Cond c = AL) { u32 i; if (encode_imm12(v, i)) { dp_imm(AND, false, rd, rn, i, c); return true; } if (encode_imm12(~v, i)) { dp_imm(BIC, false, rd, rn, i, c); return true; } return false; }
  bool orr_imm(u32 rd, u32 rn, u32 v, Cond c = AL) { u32 i; if (!encode_imm12(v, i)) return false; dp_imm(ORR, false, rd, rn, i, c); return true; }
  bool eor_imm(u32 rd, u32 rn, u32 v, Cond c = AL) { u32 i; if (!encode_imm12(v, i)) return false; dp_imm(EOR, false, rd, rn, i, c); return true; }
  bool tst_imm(u32 rn, u32 v, Cond c = AL) { u32 i; if (!encode_imm12(v, i)) return false; dp_imm(TST, true, 0, rn, i, c); return true; }
  bool cmp_imm(u32 rn, u32 v, Cond c = AL) { u32 i; if (encode_imm12(v, i)) { dp_imm(CMP, true, 0, rn, i, c); return true; } if (encode_imm12(0u - v, i)) { dp_imm(CMN, true, 0, rn, i, c); return true; } return false; }
  void tst_reg(u32 rn, u32 rm, Cond c = AL) { dp_reg(TST, true, 0, rn, rm, LSL, 0, c); }
  void cmp_reg(u32 rn, u32 rm, Cond c = AL) { dp_reg(CMP, true, 0, rn, rm, LSL, 0, c); }
  void teq_reg(u32 rn, u32 rm, Cond c = AL) { dp_reg(TEQ, true, 0, rn, rm, LSL, 0, c); }

  // ---- bit fields / extension (ARMv6T2+) ------------------------------------------
  void ubfx(u32 rd, u32 rn, u32 lsb, u32 width, Cond c = AL) { emit((c << 28) | 0x07E00050u | ((width - 1) << 16) | (rd << 12) | (lsb << 7) | rn); }
  void sbfx(u32 rd, u32 rn, u32 lsb, u32 width, Cond c = AL) { emit((c << 28) | 0x07A00050u | ((width - 1) << 16) | (rd << 12) | (lsb << 7) | rn); }
  void bfi(u32 rd, u32 rn, u32 lsb, u32 width, Cond c = AL) { emit((c << 28) | 0x07C00010u | ((lsb + width - 1) << 16) | (rd << 12) | (lsb << 7) | rn); }
  void uxtb(u32 rd, u32 rm, Cond c = AL) { emit((c << 28) | 0x06EF0070u | (rd << 12) | rm); }
  void uxth(u32 rd, u32 rm, Cond c = AL) { emit((c << 28) | 0x06FF0070u | (rd << 12) | rm); }
  void sxtb(u32 rd, u32 rm, Cond c = AL) { emit((c << 28) | 0x06AF0070u | (rd << 12) | rm); }
  void sxth(u32 rd, u32 rm, Cond c = AL) { emit((c << 28) | 0x06BF0070u | (rd << 12) | rm); }
  void clz(u32 rd, u32 rm, Cond c = AL) { emit((c << 28) | 0x016F0F10u | (rd << 12) | rm); }

  // ---- multiply -------------------------------------------------------------------
  void mul(u32 rd, u32 rn, u32 rm, bool s = false, Cond c = AL) { emit((c << 28) | 0x00000090u | (s ? (1u << 20) : 0) | (rd << 16) | (rm << 8) | rn); }
  void mla(u32 rd, u32 rn, u32 rm, u32 ra, bool s = false, Cond c = AL) { emit((c << 28) | 0x00200090u | (s ? (1u << 20) : 0) | (rd << 16) | (ra << 12) | (rm << 8) | rn); }
  void umull(u32 rdlo, u32 rdhi, u32 rn, u32 rm, bool s = false, Cond c = AL) { emit((c << 28) | 0x00800090u | (s ? (1u << 20) : 0) | (rdhi << 16) | (rdlo << 12) | (rm << 8) | rn); }
  void smull(u32 rdlo, u32 rdhi, u32 rn, u32 rm, bool s = false, Cond c = AL) { emit((c << 28) | 0x00C00090u | (s ? (1u << 20) : 0) | (rdhi << 16) | (rdlo << 12) | (rm << 8) | rn); }
  void umlal(u32 rdlo, u32 rdhi, u32 rn, u32 rm, bool s = false, Cond c = AL) { emit((c << 28) | 0x00A00090u | (s ? (1u << 20) : 0) | (rdhi << 16) | (rdlo << 12) | (rm << 8) | rn); }
  void smlal(u32 rdlo, u32 rdhi, u32 rn, u32 rm, bool s = false, Cond c = AL) { emit((c << 28) | 0x00E00090u | (s ? (1u << 20) : 0) | (rdhi << 16) | (rdlo << 12) | (rm << 8) | rn); }
  // v5TE halfword multiplies (x, y select top halves of rn, rm); accumulating
  // forms set Q on overflow.
  void smla_xy(u32 rd, u32 rn, u32 rm, u32 ra, bool x, bool y, Cond c = AL) { emit((c << 28) | 0x01000080u | (rd << 16) | (ra << 12) | (rm << 8) | (y ? 0x40u : 0) | (x ? 0x20u : 0) | rn); }
  void smul_xy(u32 rd, u32 rn, u32 rm, bool x, bool y, Cond c = AL) { emit((c << 28) | 0x01600080u | (rd << 16) | (rm << 8) | (y ? 0x40u : 0) | (x ? 0x20u : 0) | rn); }
  void smlaw_y(u32 rd, u32 rn, u32 rm, u32 ra, bool y, Cond c = AL) { emit((c << 28) | 0x01200080u | (rd << 16) | (ra << 12) | (rm << 8) | (y ? 0x40u : 0) | rn); }
  void smulw_y(u32 rd, u32 rn, u32 rm, bool y, Cond c = AL) { emit((c << 28) | 0x012000A0u | (rd << 16) | (rm << 8) | (y ? 0x40u : 0) | rn); }

  // ---- loads / stores -------------------------------------------------------------
  // Word/byte, immediate offset (-4095..4095), pre-indexed, no writeback.
  void ldr(u32 rt, u32 rn, s32 off, Cond c = AL) { ldst_imm(true, false, rt, rn, off, c); }
  void str(u32 rt, u32 rn, s32 off, Cond c = AL) { ldst_imm(false, false, rt, rn, off, c); }
  void ldrb(u32 rt, u32 rn, s32 off, Cond c = AL) { ldst_imm(true, true, rt, rn, off, c); }
  void strb(u32 rt, u32 rn, s32 off, Cond c = AL) { ldst_imm(false, true, rt, rn, off, c); }
  void ldr_reg(u32 rt, u32 rn, u32 rm, Shift sh = LSL, u32 amt = 0, Cond c = AL) { ldst_reg(true, false, rt, rn, rm, sh, amt, c); }
  void str_reg(u32 rt, u32 rn, u32 rm, Shift sh = LSL, u32 amt = 0, Cond c = AL) { ldst_reg(false, false, rt, rn, rm, sh, amt, c); }
  void ldrb_reg(u32 rt, u32 rn, u32 rm, Shift sh = LSL, u32 amt = 0, Cond c = AL) { ldst_reg(true, true, rt, rn, rm, sh, amt, c); }
  void strb_reg(u32 rt, u32 rn, u32 rm, Shift sh = LSL, u32 amt = 0, Cond c = AL) { ldst_reg(false, true, rt, rn, rm, sh, amt, c); }
  // Halfword/signed forms, immediate offset (-255..255).
  void ldrh(u32 rt, u32 rn, s32 off, Cond c = AL) { ldst_h(true, 1, 1, rt, rn, off, c); }
  void strh(u32 rt, u32 rn, s32 off, Cond c = AL) { ldst_h(false, 1, 1, rt, rn, off, c); }
  void ldrsb(u32 rt, u32 rn, s32 off, Cond c = AL) { ldst_h(true, 1, 0, rt, rn, off, c); }
  void ldrsh(u32 rt, u32 rn, s32 off, Cond c = AL) { ldst_h(true, 1, 1, rt, rn, off, c, true); }
  void ldrh_reg(u32 rt, u32 rn, u32 rm, Cond c = AL) { ldst_h_reg(true, 0b01, rt, rn, rm, c); }
  void strh_reg(u32 rt, u32 rn, u32 rm, Cond c = AL) { ldst_h_reg(false, 0b01, rt, rn, rm, c); }
  void ldrsb_reg(u32 rt, u32 rn, u32 rm, Cond c = AL) { ldst_h_reg(true, 0b10, rt, rn, rm, c); }
  void ldrsh_reg(u32 rt, u32 rn, u32 rm, Cond c = AL) { ldst_h_reg(true, 0b11, rt, rn, rm, c); }
  // ldrd/strd: rt even, rt+1 implied; offset (-255..255), 8-byte aligned address.
  void ldrd(u32 rt, u32 rn, s32 off, Cond c = AL) { assert((rt & 1) == 0); ldst_h(false, 1, 0, rt, rn, off, c); }
  void strd(u32 rt, u32 rn, s32 off, Cond c = AL) { assert((rt & 1) == 0); ldst_h(false, 1, 1, rt, rn, off, c, true); }
  void push(u32 mask, Cond c = AL) { emit((c << 28) | 0x092D0000u | mask); }
  void pop(u32 mask, Cond c = AL) { emit((c << 28) | 0x08BD0000u | mask); }
  void ldm(u32 rn, u32 mask, bool wb = false, Cond c = AL) { emit((c << 28) | 0x08900000u | (wb ? (1u << 21) : 0) | (rn << 16) | mask); }
  void stm(u32 rn, u32 mask, bool wb = false, Cond c = AL) { emit((c << 28) | 0x08800000u | (wb ? (1u << 21) : 0) | (rn << 16) | mask); }

  // ---- status ------------------------------------------------------------------------
  void mrs_apsr(u32 rd, Cond c = AL) { emit((c << 28) | 0x010F0000u | (rd << 12)); }
  void msr_apsr_nzcvq(u32 rn, Cond c = AL) { emit((c << 28) | 0x0128F000u | rn); }   // mask = f (bits 31:24)

  // ---- branches -----------------------------------------------------------------------
  // Relative branch, target within +-32 MB.
  void b(const void* target, Cond c = AL) { emit((c << 28) | 0x0A000000u | rel24(target)); }
  void bl(const void* target, Cond c = AL) { emit((c << 28) | 0x0B000000u | rel24(target)); }
  void bx(u32 rm, Cond c = AL) { emit((c << 28) | 0x012FFF10u | rm); }
  void blx(u32 rm, Cond c = AL) { emit((c << 28) | 0x012FFF30u | rm); }
  void ret(Cond c = AL) { bx(14, c); }
  // Forward branches: returns the position to `bind` later.
  size_t b_fwd(Cond c = AL) { const size_t p = pos_; emit((c << 28) | 0x0A000000u); return p; }
  size_t bl_fwd(Cond c = AL) { const size_t p = pos_; emit((c << 28) | 0x0B000000u); return p; }
  void bind(size_t at) { patch_rel(base_ + at, cur()); }
  // Retarget the relative branch at `at` (b/bl, any condition) to `target`.
  static void patch_rel(u8* at, const u8* target) {
    const u32 w = read(at);
    const s32 delta = static_cast<s32>(target - (at + 8));
    patch(at, (w & 0xFF000000u) | ((static_cast<u32>(delta) >> 2) & 0x00FFFFFFu));
  }
  static u32 rel24_from(const u8* at, const u8* target) {
    const s32 delta = static_cast<s32>(target - (at + 8));
    assert(delta >= -(1 << 25) && delta < (1 << 25) && (delta & 3) == 0);
    return (static_cast<u32>(delta) >> 2) & 0x00FFFFFFu;
  }

private:
  u32 rel24(const void* target) const { return rel24_from(cur(), static_cast<const u8*>(target)); }
  void ldst_imm(bool load, bool byte, u32 rt, u32 rn, s32 off, Cond c) {
    assert(off > -4096 && off < 4096);
    const u32 u = off >= 0 ? 1u : 0u, a = static_cast<u32>(off >= 0 ? off : -off);
    emit((c << 28) | 0x05000000u | (u << 23) | (byte ? (1u << 22) : 0) | (load ? (1u << 20) : 0) | (rn << 16) | (rt << 12) | a);
  }
  void ldst_reg(bool load, bool byte, u32 rt, u32 rn, u32 rm, Shift sh, u32 amt, Cond c) {
    emit((c << 28) | 0x07800000u | (byte ? (1u << 22) : 0) | (load ? (1u << 20) : 0) | (rn << 16) | (rt << 12) | (amt << 7) | (sh << 5) | rm);
  }
  void ldst_h(bool load, u32 h_or_d, u32 second, u32 rt, u32 rn, s32 off, Cond c, bool sh_form = false) {
    assert(off > -256 && off < 256);
    const u32 u = off >= 0 ? 1u : 0u, a = static_cast<u32>(off >= 0 ? off : -off);
    u32 op2;
    if (load) op2 = sh_form ? 0b11u : (second ? 0b01u : 0b10u);      // ldrsh / ldrh / ldrsb
    else       op2 = sh_form ? 0b11u : (second ? 0b01u : 0b10u);      // strd  / strh / ldrd
    (void)h_or_d;
    emit((c << 28) | 0x01400090u | (u << 23) | (load ? (1u << 20) : 0) | (rn << 16) | (rt << 12) | ((a >> 4) << 8) | (op2 << 5) | (a & 0xF));
  }

  void ldst_h_reg(bool load, u32 op2, u32 rt, u32 rn, u32 rm, Cond c) {
    emit((c << 28) | 0x01800090u | (load ? (1u << 20) : 0) | (rn << 16) | (rt << 12) | (op2 << 5) | rm);
  }

  u8*    base_;
  size_t cap_;
  size_t pos_;
};

} // namespace ds::jit
