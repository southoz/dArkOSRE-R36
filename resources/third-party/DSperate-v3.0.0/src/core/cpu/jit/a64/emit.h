// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Minimal AArch64 instruction encoder for the recompiler; only the forms the
// translator and runtime stubs use. Registers are plain numbers 0-31; W/X
// width is chosen by each method's `sf` argument or its name. Register 31
// means SP in address/add-sub-immediate forms and ZR everywhere else.
#pragma once
#include "core/types.h"

#include <cassert>
#include <cstring>

namespace ds::jit {

enum Cond : u32 { EQ = 0, NE, CS, CC, MI, PL, VS, VC, HI, LS, GE, LT, GT, LE, AL, NV };
enum Shift : u32 { LSL = 0, LSR = 1, ASR = 2, ROR = 3 };
constexpr u32 ZR = 31, SP = 31;

inline Cond invert(Cond c) { return static_cast<Cond>(static_cast<u32>(c) ^ 1); }

// Encode a 32-bit value as an A64 logical-immediate bitmask (N is always 0
// for 32-bit operands). Returns false when the value is not encodable: all
// zeros, all ones, or not a replicated rotated run of ones.
inline bool encode_logical_imm32(u32 v, u32& n, u32& immr, u32& imms) {
  if (v == 0 || v == 0xFFFFFFFFu) return false;
  u32 e = 32;                                   // element size: smallest period
  while (e > 2) {
    const u32 half = e / 2;
    const u32 mask = (1u << half) - 1;
    if ((v & mask) != ((v >> half) & mask)) break;
    e = half;
  }
  const u32 emask = (e == 32) ? 0xFFFFFFFFu : ((1u << e) - 1);
  const u32 elem = v & emask;
  const u32 ones = static_cast<u32>(__builtin_popcount(elem));
  if (ones == 0 || ones == e) return false;
  // A rotated run of ones has exactly one 0->1 transition around the
  // element; its position is where the run starts, and the rotation is
  // what moves a run starting at bit 0 there.
  const u32 below = ((elem << 1) | (elem >> (e - 1))) & emask;   // each bit's lower neighbour
  const u32 starts = elem & ~below;
  if (__builtin_popcount(starts) != 1) return false;
  const u32 s = static_cast<u32>(__builtin_ctz(starts));
  immr = (e - s) % e;
  switch (e) {
  case 32: imms = (ones - 1) & 0x1F; break;
  case 16: imms = 0x20 | ((ones - 1) & 0xF); break;
  case 8:  imms = 0x30 | ((ones - 1) & 0x7); break;
  case 4:  imms = 0x38 | ((ones - 1) & 0x3); break;
  default: imms = 0x3C | ((ones - 1) & 0x1); break;
  }
  n = 0;
  return true;
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
  static void patch(u8* at, u32 w) { std::memcpy(at, &w, 4); }

  // ---- moves / immediates ---------------------------------------------------
  void movz(u32 rd, u32 imm16, u32 shift = 0, bool sf = false) { emit((sf ? 0xD2800000u : 0x52800000u) | ((shift / 16) << 21) | (imm16 << 5) | rd); }
  void movk(u32 rd, u32 imm16, u32 shift = 0, bool sf = false) { emit((sf ? 0xF2800000u : 0x72800000u) | ((shift / 16) << 21) | (imm16 << 5) | rd); }
  void movn(u32 rd, u32 imm16, u32 shift = 0, bool sf = false) { emit((sf ? 0x92800000u : 0x12800000u) | ((shift / 16) << 21) | (imm16 << 5) | rd); }
  void mov_imm(u32 rd, u32 v) {
    u32 n, immr, imms;
    if ((v & 0xFFFF0000) == 0) { movz(rd, v & 0xFFFF); return; }
    if ((v & 0x0000FFFF) == 0) { movz(rd, v >> 16, 16); return; }
    if ((v & 0xFFFF0000) == 0xFFFF0000) { movn(rd, (~v) & 0xFFFF); return; }
    if ((v & 0x0000FFFF) == 0x0000FFFF) { movn(rd, (~v) >> 16, 16); return; }
    if (encode_logical_imm32(v, n, immr, imms)) { emit(0x32000000u | (immr << 16) | (imms << 10) | (ZR << 5) | rd); return; }
    movz(rd, v & 0xFFFF); movk(rd, v >> 16, 16);
  }
  void mov_imm64(u32 rd, u64 v) {
    movz(rd, v & 0xFFFF, 0, true);
    if ((v >> 16) & 0xFFFF) movk(rd, (v >> 16) & 0xFFFF, 16, true);
    if ((v >> 32) & 0xFFFF) movk(rd, (v >> 32) & 0xFFFF, 32, true);
    if ((v >> 48) & 0xFFFF) movk(rd, (v >> 48) & 0xFFFF, 48, true);
  }
  void mov(u32 rd, u32 rm, bool sf = false) { orr_reg(rd, ZR, rm, LSL, 0, sf); }   // rd/rm are not SP
  void mov_sp(u32 rd, u32 rn, bool sf = true) { add_imm(rd, rn, 0, sf); }

  // ---- add / sub -----------------------------------------------------------
  void add_imm(u32 rd, u32 rn, u32 imm, bool sf = false, bool s = false) { addsub_imm(0, s, rd, rn, imm, sf); }
  void sub_imm(u32 rd, u32 rn, u32 imm, bool sf = false, bool s = false) { addsub_imm(1, s, rd, rn, imm, sf); }
  void adds_imm(u32 rd, u32 rn, u32 imm, bool sf = false) { addsub_imm(0, true, rd, rn, imm, sf); }
  void subs_imm(u32 rd, u32 rn, u32 imm, bool sf = false) { addsub_imm(1, true, rd, rn, imm, sf); }
  void cmp_imm(u32 rn, u32 imm, bool sf = false) { subs_imm(ZR, rn, imm, sf); }
  void cmn_imm(u32 rn, u32 imm, bool sf = false) { adds_imm(ZR, rn, imm, sf); }
  static bool is_addsub_imm(u32 v) { return v < 0x1000 || ((v & 0xFFF) == 0 && (v >> 12) < 0x1000); }
  void add_imm_any(u32 rd, u32 rn, u32 v, u32 tmp, bool s = false) {
    if (is_addsub_imm(v)) { addsub_imm(0, s, rd, rn, v, false); return; }
    if (!s && is_addsub_imm(0u - v)) { addsub_imm(1, s, rd, rn, 0u - v, false); return; }
    mov_imm(tmp, v); addsub_reg(0, s, rd, rn, tmp, LSL, 0, false);
  }
  void sub_imm_any(u32 rd, u32 rn, u32 v, u32 tmp, bool s = false) {
    if (is_addsub_imm(v)) { addsub_imm(1, s, rd, rn, v, false); return; }
    if (!s && is_addsub_imm(0u - v)) { addsub_imm(0, s, rd, rn, 0u - v, false); return; }
    mov_imm(tmp, v); addsub_reg(1, s, rd, rn, tmp, LSL, 0, false);
  }
  void add_sub(bool sub, bool s, u32 rd, u32 rn, u32 rm) { addsub_reg(sub ? 1 : 0, s, rd, rn, rm, LSL, 0, false); }
  void add_reg(u32 rd, u32 rn, u32 rm, Shift sh = LSL, u32 amt = 0, bool sf = false) { addsub_reg(0, false, rd, rn, rm, sh, amt, sf); }
  void sub_reg(u32 rd, u32 rn, u32 rm, Shift sh = LSL, u32 amt = 0, bool sf = false) { addsub_reg(1, false, rd, rn, rm, sh, amt, sf); }
  void adds_reg(u32 rd, u32 rn, u32 rm, Shift sh = LSL, u32 amt = 0, bool sf = false) { addsub_reg(0, true, rd, rn, rm, sh, amt, sf); }
  void subs_reg(u32 rd, u32 rn, u32 rm, Shift sh = LSL, u32 amt = 0, bool sf = false) { addsub_reg(1, true, rd, rn, rm, sh, amt, sf); }
  void cmp_reg(u32 rn, u32 rm, Shift sh = LSL, u32 amt = 0, bool sf = false) { subs_reg(ZR, rn, rm, sh, amt, sf); }
  void cmn_reg(u32 rn, u32 rm, bool sf = false) { adds_reg(ZR, rn, rm, LSL, 0, sf); }
  void neg(u32 rd, u32 rm) { sub_reg(rd, ZR, rm); }
  void negs(u32 rd, u32 rm) { subs_reg(rd, ZR, rm); }
  // extended register: rd = rn + (rm extended), sf selects the 64-bit form; `uxtw` extension.
  void add_uxtw(u32 rd, u32 rn, u32 rm, u32 lsl = 0) { emit(0x8B200000u | (0b010u << 13) | (lsl << 10) | (rm << 16) | (rn << 5) | rd); }
  void adc(u32 rd, u32 rn, u32 rm, bool s = false) { emit(0x1A000000u | (s ? 0x20000000u : 0) | (rm << 16) | (rn << 5) | rd); }
  void sbc(u32 rd, u32 rn, u32 rm, bool s = false) { emit(0x5A000000u | (s ? 0x20000000u : 0) | (rm << 16) | (rn << 5) | rd); }

  // ---- logical --------------------------------------------------------------
  void and_reg(u32 rd, u32 rn, u32 rm, Shift sh = LSL, u32 amt = 0, bool sf = false) { logical_reg(0, false, rd, rn, rm, sh, amt, sf); }
  void orr_reg(u32 rd, u32 rn, u32 rm, Shift sh = LSL, u32 amt = 0, bool sf = false) { logical_reg(1, false, rd, rn, rm, sh, amt, sf); }
  void eor_reg(u32 rd, u32 rn, u32 rm, Shift sh = LSL, u32 amt = 0, bool sf = false) { logical_reg(2, false, rd, rn, rm, sh, amt, sf); }
  void ands_reg(u32 rd, u32 rn, u32 rm, Shift sh = LSL, u32 amt = 0, bool sf = false) { logical_reg(3, false, rd, rn, rm, sh, amt, sf); }
  void bic_reg(u32 rd, u32 rn, u32 rm, Shift sh = LSL, u32 amt = 0, bool sf = false) { logical_reg(0, true, rd, rn, rm, sh, amt, sf); }
  void orn_reg(u32 rd, u32 rn, u32 rm, Shift sh = LSL, u32 amt = 0, bool sf = false) { logical_reg(1, true, rd, rn, rm, sh, amt, sf); }
  void bics_reg(u32 rd, u32 rn, u32 rm, Shift sh = LSL, u32 amt = 0, bool sf = false) { logical_reg(3, true, rd, rn, rm, sh, amt, sf); }
  void mvn(u32 rd, u32 rm, Shift sh = LSL, u32 amt = 0) { orn_reg(rd, ZR, rm, sh, amt); }
  void tst_reg(u32 rn, u32 rm, bool sf = false) { ands_reg(ZR, rn, rm, LSL, 0, sf); }
  bool and_imm(u32 rd, u32 rn, u32 v, bool s = false) { return logical_imm(s ? 3 : 0, rd, rn, v); }
  bool orr_imm(u32 rd, u32 rn, u32 v) { return logical_imm(1, rd, rn, v); }
  bool eor_imm(u32 rd, u32 rn, u32 v) { return logical_imm(2, rd, rn, v); }
  bool tst_imm(u32 rn, u32 v) { return logical_imm(3, ZR, rn, v); }
  void and_imm64(u32 rd, u32 rn, u32 n, u32 immr, u32 imms) { emit(0x92000000u | (n << 22) | (immr << 16) | (imms << 10) | (rn << 5) | rd); }
  void and_imm_any(u32 rd, u32 rn, u32 v, u32 tmp) { if (!and_imm(rd, rn, v)) { mov_imm(tmp, v); and_reg(rd, rn, tmp); } }
  void orr_imm_any(u32 rd, u32 rn, u32 v, u32 tmp) { if (!orr_imm(rd, rn, v)) { mov_imm(tmp, v); orr_reg(rd, rn, tmp); } }
  void eor_imm_any(u32 rd, u32 rn, u32 v, u32 tmp) { if (!eor_imm(rd, rn, v)) { mov_imm(tmp, v); eor_reg(rd, rn, tmp); } }

  // ---- shifts / bitfields ---------------------------------------------------
  void lsl_imm(u32 rd, u32 rn, u32 amt, bool sf = false) {
    const u32 bits = sf ? 64 : 32;
    ubfm(rd, rn, (bits - amt) % bits, bits - 1 - amt, sf);
  }
  void lsr_imm(u32 rd, u32 rn, u32 amt, bool sf = false) { ubfm(rd, rn, amt, sf ? 63 : 31, sf); }
  void asr_imm(u32 rd, u32 rn, u32 amt, bool sf = false) { sbfm(rd, rn, amt, sf ? 63 : 31, sf); }
  void ror_imm(u32 rd, u32 rn, u32 amt) { emit(0x13800000u | (rn << 16) | (amt << 10) | (rn << 5) | rd); }   // EXTR rd, rn, rn, #amt
  void extr(u32 rd, u32 rn, u32 rm, u32 lsb) { emit(0x13800000u | (rm << 16) | (lsb << 10) | (rn << 5) | rd); }
  void lslv(u32 rd, u32 rn, u32 rm, bool sf = false) { emit((sf ? 0x9AC02000u : 0x1AC02000u) | (rm << 16) | (rn << 5) | rd); }
  void lsrv(u32 rd, u32 rn, u32 rm, bool sf = false) { emit((sf ? 0x9AC02400u : 0x1AC02400u) | (rm << 16) | (rn << 5) | rd); }
  void asrv(u32 rd, u32 rn, u32 rm, bool sf = false) { emit((sf ? 0x9AC02800u : 0x1AC02800u) | (rm << 16) | (rn << 5) | rd); }
  void rorv(u32 rd, u32 rn, u32 rm, bool sf = false) { emit((sf ? 0x9AC02C00u : 0x1AC02C00u) | (rm << 16) | (rn << 5) | rd); }
  void ubfm(u32 rd, u32 rn, u32 immr, u32 imms, bool sf = false) { emit((sf ? 0xD3400000u : 0x53000000u) | (immr << 16) | (imms << 10) | (rn << 5) | rd); }
  void sbfm(u32 rd, u32 rn, u32 immr, u32 imms, bool sf = false) { emit((sf ? 0x93400000u : 0x13000000u) | (immr << 16) | (imms << 10) | (rn << 5) | rd); }
  void bfm(u32 rd, u32 rn, u32 immr, u32 imms, bool sf = false) { emit((sf ? 0xB3400000u : 0x33000000u) | (immr << 16) | (imms << 10) | (rn << 5) | rd); }
  void ubfx(u32 rd, u32 rn, u32 lsb, u32 width, bool sf = false) { ubfm(rd, rn, lsb, lsb + width - 1, sf); }
  void sbfx(u32 rd, u32 rn, u32 lsb, u32 width, bool sf = false) { sbfm(rd, rn, lsb, lsb + width - 1, sf); }
  void ubfiz(u32 rd, u32 rn, u32 lsb, u32 width, bool sf = false) { const u32 bits = sf ? 64 : 32; ubfm(rd, rn, (bits - lsb) % bits, width - 1, sf); }
  void bfi(u32 rd, u32 rn, u32 lsb, u32 width, bool sf = false) { const u32 bits = sf ? 64 : 32; bfm(rd, rn, (bits - lsb) % bits, width - 1, sf); }
  void bfxil(u32 rd, u32 rn, u32 lsb, u32 width, bool sf = false) { bfm(rd, rn, lsb, lsb + width - 1, sf); }
  void uxtb(u32 rd, u32 rn) { ubfm(rd, rn, 0, 7); }
  void uxth(u32 rd, u32 rn) { ubfm(rd, rn, 0, 15); }
  void sxtb(u32 rd, u32 rn) { sbfm(rd, rn, 0, 7); }
  void sxth(u32 rd, u32 rn) { sbfm(rd, rn, 0, 15); }
  void clz(u32 rd, u32 rn) { emit(0x5AC01000u | (rn << 5) | rd); }
  void rev(u32 rd, u32 rn) { emit(0x5AC00800u | (rn << 5) | rd); }

  // ---- multiply / divide ----------------------------------------------------
  void madd(u32 rd, u32 rn, u32 rm, u32 ra, bool sf = false) { emit((sf ? 0x9B000000u : 0x1B000000u) | (rm << 16) | (ra << 10) | (rn << 5) | rd); }
  void msub(u32 rd, u32 rn, u32 rm, u32 ra, bool sf = false) { emit((sf ? 0x9B008000u : 0x1B008000u) | (rm << 16) | (ra << 10) | (rn << 5) | rd); }
  void mul(u32 rd, u32 rn, u32 rm, bool sf = false) { madd(rd, rn, rm, ZR, sf); }
  void smaddl(u32 xd, u32 wn, u32 wm, u32 xa) { emit(0x9B200000u | (wm << 16) | (xa << 10) | (wn << 5) | xd); }
  void umaddl(u32 xd, u32 wn, u32 wm, u32 xa) { emit(0x9BA00000u | (wm << 16) | (xa << 10) | (wn << 5) | xd); }
  void smull(u32 xd, u32 wn, u32 wm) { smaddl(xd, wn, wm, ZR); }
  void umull(u32 xd, u32 wn, u32 wm) { umaddl(xd, wn, wm, ZR); }

  // ---- conditional ----------------------------------------------------------
  void csel(u32 rd, u32 rn, u32 rm, Cond c, bool sf = false) { emit((sf ? 0x9A800000u : 0x1A800000u) | (rm << 16) | (c << 12) | (rn << 5) | rd); }
  void csinc(u32 rd, u32 rn, u32 rm, Cond c, bool sf = false) { emit((sf ? 0x9A800400u : 0x1A800400u) | (rm << 16) | (c << 12) | (rn << 5) | rd); }
  void csinv(u32 rd, u32 rn, u32 rm, Cond c, bool sf = false) { emit((sf ? 0xDA800000u : 0x5A800000u) | (rm << 16) | (c << 12) | (rn << 5) | rd); }
  void csneg(u32 rd, u32 rn, u32 rm, Cond c, bool sf = false) { emit((sf ? 0xDA800400u : 0x5A800400u) | (rm << 16) | (c << 12) | (rn << 5) | rd); }
  void cset(u32 rd, Cond c) { csinc(rd, ZR, ZR, invert(c)); }
  void csetm(u32 rd, Cond c) { csinv(rd, ZR, ZR, invert(c)); }
  void ccmp_imm(u32 rn, u32 imm5, u32 nzcv, Cond c, bool sf = false) { emit((sf ? 0xFA400800u : 0x7A400800u) | (imm5 << 16) | (c << 12) | (rn << 5) | nzcv); }
  void ccmp_reg(u32 rn, u32 rm, u32 nzcv, Cond c, bool sf = false) { emit((sf ? 0xFA400000u : 0x7A400000u) | (rm << 16) | (c << 12) | (rn << 5) | nzcv); }
  void ccmn_imm(u32 rn, u32 imm5, u32 nzcv, Cond c, bool sf = false) { emit((sf ? 0xBA400800u : 0x3A400800u) | (imm5 << 16) | (c << 12) | (rn << 5) | nzcv); }

  // ---- system ---------------------------------------------------------------
  void mrs_nzcv(u32 xt) { emit(0xD53B4200u | xt); }
  void msr_nzcv(u32 xt) { emit(0xD51B4200u | xt); }
  void nop() { emit(0xD503201Fu); }
  void brk(u32 imm = 0) { emit(0xD4200000u | (imm << 5)); }

  // ---- loads / stores -------------------------------------------------------
  // Unsigned scaled immediate offset (must be a multiple of the access size).
  void ldr_w(u32 rt, u32 rn, u32 off = 0)   { ldst_uimm(0xB9400000u, rt, rn, off, 4); }
  void str_w(u32 rt, u32 rn, u32 off = 0)   { ldst_uimm(0xB9000000u, rt, rn, off, 4); }
  void ldr_x(u32 rt, u32 rn, u32 off = 0)   { ldst_uimm(0xF9400000u, rt, rn, off, 8); }
  void str_x(u32 rt, u32 rn, u32 off = 0)   { ldst_uimm(0xF9000000u, rt, rn, off, 8); }
  void ldrb(u32 rt, u32 rn, u32 off = 0)    { ldst_uimm(0x39400000u, rt, rn, off, 1); }
  void strb(u32 rt, u32 rn, u32 off = 0)    { ldst_uimm(0x39000000u, rt, rn, off, 1); }
  void ldrh(u32 rt, u32 rn, u32 off = 0)    { ldst_uimm(0x79400000u, rt, rn, off, 2); }
  void strh(u32 rt, u32 rn, u32 off = 0)    { ldst_uimm(0x79000000u, rt, rn, off, 2); }
  void ldrsb_w(u32 rt, u32 rn, u32 off = 0) { ldst_uimm(0x39C00000u, rt, rn, off, 1); }
  void ldrsh_w(u32 rt, u32 rn, u32 off = 0) { ldst_uimm(0x79C00000u, rt, rn, off, 2); }
  // Unscaled signed 9-bit offset.
  void ldur_w(u32 rt, u32 rn, s32 off) { ldst_simm9(0xB8400000u, rt, rn, off); }
  void stur_w(u32 rt, u32 rn, s32 off) { ldst_simm9(0xB8000000u, rt, rn, off); }
  void ldur_x(u32 rt, u32 rn, s32 off) { ldst_simm9(0xF8400000u, rt, rn, off); }
  void stur_x(u32 rt, u32 rn, s32 off) { ldst_simm9(0xF8000000u, rt, rn, off); }
  // Register offset: [xn, wm, uxtw] (ext = 2) or [xn, xm] (ext = 3), optional shift by access size.
  void ldr_w_reg(u32 rt, u32 xn, u32 rm, bool x = false, bool scale = false)   { ldst_reg(0xB8600800u, rt, xn, rm, x, scale); }
  void str_w_reg(u32 rt, u32 xn, u32 rm, bool x = false, bool scale = false)   { ldst_reg(0xB8200800u, rt, xn, rm, x, scale); }
  void ldr_x_reg(u32 rt, u32 xn, u32 rm, bool x = false, bool scale = false)   { ldst_reg(0xF8600800u, rt, xn, rm, x, scale); }
  void ldrb_reg(u32 rt, u32 xn, u32 rm, bool x = false)                        { ldst_reg(0x38600800u, rt, xn, rm, x, false); }
  void strb_reg(u32 rt, u32 xn, u32 rm, bool x = false)                        { ldst_reg(0x38200800u, rt, xn, rm, x, false); }
  void ldrh_reg(u32 rt, u32 xn, u32 rm, bool x = false)                        { ldst_reg(0x78600800u, rt, xn, rm, x, false); }
  void strh_reg(u32 rt, u32 xn, u32 rm, bool x = false)                        { ldst_reg(0x78200800u, rt, xn, rm, x, false); }
  void ldrsb_w_reg(u32 rt, u32 xn, u32 rm, bool x = false)                     { ldst_reg(0x38E00800u, rt, xn, rm, x, false); }
  void ldrsh_w_reg(u32 rt, u32 xn, u32 rm, bool x = false)                     { ldst_reg(0x78E00800u, rt, xn, rm, x, false); }
  // Pairs (64-bit), signed 7-bit scaled offset; pre/post-index variants for the stack.
  void stp_x(u32 rt, u32 rt2, u32 rn, s32 off) { ldstp(0xA9000000u, rt, rt2, rn, off); }
  void ldp_x(u32 rt, u32 rt2, u32 rn, s32 off) { ldstp(0xA9400000u, rt, rt2, rn, off); }
  void stp_w(u32 rt, u32 rt2, u32 rn, s32 off) { ldstp(0x29000000u, rt, rt2, rn, off, 4); }
  void ldp_w(u32 rt, u32 rt2, u32 rn, s32 off) { ldstp(0x29400000u, rt, rt2, rn, off, 4); }
  void stp_x_pre(u32 rt, u32 rt2, u32 rn, s32 off) { ldstp(0xA9800000u, rt, rt2, rn, off); }
  void ldp_x_post(u32 rt, u32 rt2, u32 rn, s32 off) { ldstp(0xA8C00000u, rt, rt2, rn, off); }
  void str_x_pre(u32 rt, u32 rn, s32 off) { emit(0xF8000C00u | ((static_cast<u32>(off) & 0x1FF) << 12) | (rn << 5) | rt); }
  void ldr_x_post(u32 rt, u32 rn, s32 off) { emit(0xF8400400u | ((static_cast<u32>(off) & 0x1FF) << 12) | (rn << 5) | rt); }

  // ---- branches -------------------------------------------------------------
  // Relative branches take a target address; the caller guarantees range.
  void b(const void* target) { emit(0x14000000u | imm26(target)); }
  void bl(const void* target) { emit(0x94000000u | imm26(target)); }
  void b_cond(Cond c, const void* target) { emit(0x54000000u | (imm19(target) << 5) | c); }
  void cbz(u32 rt, const void* target, bool sf = false) { emit((sf ? 0xB4000000u : 0x34000000u) | (imm19(target) << 5) | rt); }
  void cbnz(u32 rt, const void* target, bool sf = false) { emit((sf ? 0xB5000000u : 0x35000000u) | (imm19(target) << 5) | rt); }
  void tbz(u32 rt, u32 bit, const void* target) { emit(0x36000000u | ((bit >> 5) << 31) | ((bit & 31) << 19) | (imm14(target) << 5) | rt); }
  void tbnz(u32 rt, u32 bit, const void* target) { emit(0x37000000u | ((bit >> 5) << 31) | ((bit & 31) << 19) | (imm14(target) << 5) | rt); }
  void br(u32 xn) { emit(0xD61F0000u | (xn << 5)); }
  void blr(u32 xn) { emit(0xD63F0000u | (xn << 5)); }
  void ret(u32 xn = 30) { emit(0xD65F0000u | (xn << 5)); }

  // Forward branches: emit a placeholder, patch when the target is known.
  struct Label { size_t pos = 0; bool bound = false; };
  struct Fixup { size_t at; u32 kind; u32 rt_or_cond; u32 bit; };
  size_t b_fwd() { size_t at = pos_; emit(0x14000000u); return at; }
  size_t bl_fwd() { size_t at = pos_; emit(0x94000000u); return at; }
  void word(u32 v) { emit(v); }
  static void patch_rel(u8* at, const u8* target) {
    u32 w; std::memcpy(&w, at, 4);
    const s64 delta = target - at;
    if ((w & 0x7C000000u) == 0x14000000u) {                 // b / bl
      w = (w & 0xFC000000u) | (static_cast<u32>(delta >> 2) & 0x03FFFFFFu);
    } else if ((w & 0xFF000010u) == 0x54000000u) {          // b.cond
      w = (w & 0xFF00001Fu) | ((static_cast<u32>(delta >> 2) & 0x7FFFFu) << 5);
    } else if (((w >> 24) & 0x7E) == 0x34) {                // cbz / cbnz
      w = (w & 0xFF00001Fu) | ((static_cast<u32>(delta >> 2) & 0x7FFFFu) << 5);
    } else {                                                // tbz / tbnz
      assert(delta >= -(s64{1} << 15) && delta < (s64{1} << 15));
      w = (w & 0xFFF8001Fu) | ((static_cast<u32>(delta >> 2) & 0x3FFFu) << 5);
    }
    std::memcpy(at, &w, 4);
  }
  size_t b_cond_fwd(Cond c) { size_t at = pos_; emit(0x54000000u | c); return at; }
  size_t cbz_fwd(u32 rt, bool sf = false) { size_t at = pos_; emit((sf ? 0xB4000000u : 0x34000000u) | rt); return at; }
  size_t cbnz_fwd(u32 rt, bool sf = false) { size_t at = pos_; emit((sf ? 0xB5000000u : 0x35000000u) | rt); return at; }
  size_t tbz_fwd(u32 rt, u32 bit) { size_t at = pos_; emit(0x36000000u | ((bit >> 5) << 31) | ((bit & 31) << 19) | rt); return at; }
  size_t tbnz_fwd(u32 rt, u32 bit) { size_t at = pos_; emit(0x37000000u | ((bit >> 5) << 31) | ((bit & 31) << 19) | rt); return at; }
  void bind(size_t at) { bind_to(at, pos_); }
  void bind_to(size_t at, size_t target) {
    u32 w; std::memcpy(&w, base_ + at, 4);
    const s64 delta = static_cast<s64>(target) - static_cast<s64>(at);
    const u32 top = w >> 24;
    if ((w & 0x7C000000u) == 0x14000000u) {                 // b / bl
      w = (w & 0x80000000u) | 0x14000000u | (static_cast<u32>(delta >> 2) & 0x03FFFFFFu);
    } else if ((w & 0xFF000010u) == 0x54000000u) {          // b.cond
      w = (w & 0xFF00001Fu) | ((static_cast<u32>(delta >> 2) & 0x7FFFFu) << 5);
    } else if ((top & 0x7E) == 0x34) {                      // cbz / cbnz
      w = (w & 0xFF00001Fu) | ((static_cast<u32>(delta >> 2) & 0x7FFFFu) << 5);
    } else {                                                // tbz / tbnz
      w = (w & 0xFFF8001Fu) | ((static_cast<u32>(delta >> 2) & 0x3FFFu) << 5);
    }
    std::memcpy(base_ + at, &w, 4);
  }

private:
  u8* base_;
  size_t cap_, pos_;

  u32 imm26(const void* target) const {
    const s64 d = reinterpret_cast<const u8*>(target) - cur();
    assert(d >= -(s64{1} << 27) && d < (s64{1} << 27) && !(d & 3));
    return static_cast<u32>(d >> 2) & 0x03FFFFFFu;
  }
  u32 imm19(const void* target) const {
    const s64 d = reinterpret_cast<const u8*>(target) - cur();
    assert(d >= -(s64{1} << 20) && d < (s64{1} << 20) && !(d & 3));
    return static_cast<u32>(d >> 2) & 0x7FFFFu;
  }
  u32 imm14(const void* target) const {
    const s64 d = reinterpret_cast<const u8*>(target) - cur();
    assert(d >= -(s64{1} << 15) && d < (s64{1} << 15) && !(d & 3));
    return static_cast<u32>(d >> 2) & 0x3FFFu;
  }
  void addsub_imm(u32 op, bool s, u32 rd, u32 rn, u32 imm, bool sf) {
    u32 sh = 0;
    if (imm >= 0x1000) { assert((imm & 0xFFF) == 0 && (imm >> 12) < 0x1000); imm >>= 12; sh = 1; }
    emit((sf ? 0x80000000u : 0) | (op << 30) | (s ? 0x20000000u : 0) | 0x11000000u | (sh << 22) | (imm << 10) | (rn << 5) | rd);
  }
  void addsub_reg(u32 op, bool s, u32 rd, u32 rn, u32 rm, Shift sh, u32 amt, bool sf) {
    assert(sh != ROR);
    emit((sf ? 0x80000000u : 0) | (op << 30) | (s ? 0x20000000u : 0) | 0x0B000000u | (sh << 22) | (rm << 16) | (amt << 10) | (rn << 5) | rd);
  }
  void logical_reg(u32 opc, bool n, u32 rd, u32 rn, u32 rm, Shift sh, u32 amt, bool sf) {
    emit((sf ? 0x80000000u : 0) | (opc << 29) | 0x0A000000u | (sh << 22) | (n ? 0x00200000u : 0) | (rm << 16) | (amt << 10) | (rn << 5) | rd);
  }
  bool logical_imm(u32 opc, u32 rd, u32 rn, u32 v) {
    u32 n, immr, imms;
    if (!encode_logical_imm32(v, n, immr, imms)) return false;
    emit((opc << 29) | 0x12000000u | (n << 22) | (immr << 16) | (imms << 10) | (rn << 5) | rd);
    return true;
  }
  void ldst_uimm(u32 op, u32 rt, u32 rn, u32 off, u32 size) {
    assert(off % size == 0 && off / size < 0x1000);
    emit(op | ((off / size) << 10) | (rn << 5) | rt);
  }
  void ldst_simm9(u32 op, u32 rt, u32 rn, s32 off) {
    assert(off >= -256 && off < 256);
    emit(op | ((static_cast<u32>(off) & 0x1FF) << 12) | (rn << 5) | rt);
  }
  void ldst_reg(u32 op, u32 rt, u32 xn, u32 rm, bool x, bool scale) {
    const u32 option = x ? 0b011 : 0b010;
    emit(op | (rm << 16) | (option << 13) | ((scale ? 1u : 0u) << 12) | (xn << 5) | rt);
  }
  void ldstp(u32 op, u32 rt, u32 rt2, u32 rn, s32 off, u32 scale = 8) {
    assert(off % static_cast<s32>(scale) == 0);
    const s32 imm7 = off / static_cast<s32>(scale);
    assert(imm7 >= -64 && imm7 < 64);
    emit(op | ((static_cast<u32>(imm7) & 0x7F) << 15) | (rt2 << 10) | (rn << 5) | rt);
  }
};

} // namespace ds::jit
