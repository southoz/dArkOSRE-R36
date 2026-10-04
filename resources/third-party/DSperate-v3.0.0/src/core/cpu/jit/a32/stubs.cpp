// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// ARMv7 (A32) stubs and runtime code patches. Stubs are entered by `bl`; literal
// args follow the bl and are read/skipped via lr. At stub boundaries: pinned guest
// regs in r4-r8, guest NZCVQ in APSR, budget in r9, rest in memory. No flag-neutral
// compare: a stub that tests must park guest flags first. C helpers may be Thumb-2
// (call via `blx r12`); SliceNext returns in memory (r0 = &result, args shift up).
#include "core/cpu/jit/jit_internal.h"
#include "core/cpu/jit/a32/convention.h"
#include "core/cpu/jit/a32/emit.h"
#include "core/sched/scheduler.h"

#include <cstring>
#include <vector>

namespace ds::jit {

extern "C" ds::SliceNext ds_slice_next(void* scheduler);

namespace {

constexpr u32 M(u32 r) { return 1u << r; }
constexpr u32 CALLEE_SAVED = M(4) | M(5) | M(6) | M(7) | M(8) | M(9) | M(10) | M(11);
constexpr u32 PINNED_LOW = M(4) | M(5) | M(6) | M(7);   // guest r0-r3

// R_BUDGET holds cycle_budget - 1.
void emit_store_budget(Emitter& e, u32 t) {
  e.add_imm(t, R_BUDGET, 1, t);
  e.str(t, R_CTX, OFF_BUDGET);
}
void emit_load_budget(Emitter& e) {
  e.ldr(R_BUDGET, R_CTX, OFF_BUDGET);
  e.sub_imm(R_BUDGET, R_BUDGET, 1, R_BUDGET);
}
void emit_load_pt(Emitter& e) {
  e.ldr(R_PT, R_CTX, OFF_JIT);
  e.ldr(R_PT, R_PT, OFF_JC_PT);
}
void emit_store_pinned(Emitter& e, u32 t) {
  e.add_imm(t, R_CTX, off_reg(0), t);
  e.stm(t, PINNED_LOW);
  e.str(8, R_CTX, off_reg(13));
}
void emit_load_pinned(Emitter& e, u32 t) {
  e.add_imm(t, R_CTX, off_reg(0), t);
  e.ldm(t, PINNED_LOW);
  e.ldr(8, R_CTX, off_reg(13));
}
void emit_save_flags(Emitter& e, u32 t0, u32 t1) {
  e.mrs_apsr(t0);
  e.lsr_imm(t0, t0, 27);
  e.ldr(t1, R_CTX, OFF_CPSR);
  e.bfi(t1, t0, 27, 5);
  e.str(t1, R_CTX, OFF_CPSR);
}
void emit_load_flags(Emitter& e, u32 t) {
  e.ldr(t, R_CTX, OFF_CPSR);
  e.msr_apsr_nzcvq(t);
}

// Branches to `leave` on budget exhausted, alert, or unmasked IRQ.
// Clobbers r0, r1 and host flags.
void emit_poll(Emitter& e, std::vector<size_t>& leave) {
  e.tst_imm(R_BUDGET, 0x80000000u);
  leave.push_back(e.b_fwd(NE));
  e.ldr(0, R_CTX, OFF_ALERTS);
  e.cmp_imm(0, 0);
  leave.push_back(e.b_fwd(NE));
  e.ldr(0, R_CTX, OFF_IRQ);
  e.cmp_imm(0, 0);
  size_t none = e.b_fwd(EQ);
  e.ldr(1, R_CTX, OFF_CPSR);
  e.tst_imm(1, 0x80);
  leave.push_back(e.b_fwd(EQ));
  e.bind(none);
}

} // namespace

namespace backend {

void emit_stubs(Runtime& rt) {
  Emitter e(rt.arena, rt.cap);
  e.set_pos(LUT_AREA);

  // Every frame is a multiple of 8 bytes: AAPCS sp alignment at C calls.
  // ---- enter(ctx, native): C-callable ----
  rt.enter = reinterpret_cast<void (*)(CpuContext*, const void*)>(e.cur());
  e.push(CALLEE_SAVED | M(R_LR));
  e.dp_imm(SUB, false, R_SP, R_SP, 4);       // pad to 40
  size_t to_light = e.bl_fwd();
  e.dp_imm(ADD, false, R_SP, R_SP, 4);
  e.pop(CALLEE_SAVED | M(R_PC));
  // ---- enter_light(ctx, native): an 8-byte frame; exits return here ----
  e.bind(to_light);
  rt.enter_light = e.cur();
  e.push(M(R_FN) | M(R_LR));
  e.mov(R_CTX, 0);
  emit_load_budget(e);
  emit_load_pt(e);
  emit_load_pinned(e, 2);
  emit_load_flags(e, 2);
  e.bx(1);

  // ---- exit_key_lit: `bl exit_key_lit; .word key` ----
  rt.exit_key_lit = e.cur();
  e.ldr(0, R_LR, 0);
  // ---- exit_key: r0 = next key; must not touch flags ----
  rt.exit_key = e.cur();
  e.and_imm(1, 0, 1);
  e.and_imm(2, 0, ~1u);
  e.add_imm(2, 2, 8, 3);
  e.sub_reg(2, 2, 1, LSL, 2);               // pc + 8 - 4T
  e.str(2, R_CTX, off_reg(15));
  // ---- exit_r15: ctx.r15 already correct ----
  rt.exit_r15 = e.cur();
  emit_store_pinned(e, 0);
  emit_save_flags(e, 0, 1);
  emit_store_budget(e, 0);
  e.pop(M(R_FN) | M(R_PC));

  // ---- run_loop(scheduler): the native slice loop ----
  rt.run_loop = reinterpret_cast<void (*)(void*)>(e.cur());
  e.push(CALLEE_SAVED | M(R_LR));
  e.dp_imm(SUB, false, R_SP, R_SP, 20);     // [sp] = SliceNext, [sp+8] = scheduler
  e.str(0, R_SP, 8);
  const size_t loop_top = e.size();
  e.mov(0, R_SP);
  e.ldr(1, R_SP, 8);
  e.mov_ptr(R_FN, reinterpret_cast<const void*>(&ds_slice_next));
  e.blx(R_FN);
  e.ldr(0, R_SP, 0);
  e.ldr(1, R_SP, 4);
  e.cmp_imm(0, 0);
  size_t loop_exit = e.b_fwd(EQ);
  e.bl(rt.enter_light);
  e.b(e.base() + loop_top);
  e.bind(loop_exit);
  e.dp_imm(ADD, false, R_SP, R_SP, 20);
  e.pop(CALLEE_SAVED | M(R_PC));

  // ---- flush_exit: r0 = key; arena full ----
  rt.flush_exit = e.cur();
  e.mov_ptr(1, &rt.need_reset);
  e.mov_imm(2, 1);
  e.strb(2, 1, 0);
  e.b(rt.exit_r15);

  // ---- call_pure: r12 = fn, args r0-r3; helper touches no guest state ----
  rt.call_pure = e.cur();
  e.push(M(R_LR));
  e.add_imm(R_LR, R_BUDGET, 1, R_LR);
  e.str(R_LR, R_CTX, OFF_BUDGET);
  e.mrs_apsr(R_LR);
  e.push(M(R_LR));
  e.blx(R_FN);
  emit_load_budget(e);
  e.pop(M(1));
  e.msr_apsr_nzcvq(1);
  e.pop(M(R_PC));

  // ---- call_full: r12 = fn, args r0-r2; helper may touch guest state ----
  rt.call_full = e.cur();
  e.push(M(3) | M(R_LR));
  emit_save_flags(e, 3, R_LR);
  emit_store_pinned(e, 3);
  e.add_imm(R_LR, R_BUDGET, 1, R_LR);
  e.str(R_LR, R_CTX, OFF_BUDGET);
  e.blx(R_FN);
  emit_load_budget(e);
  emit_load_pinned(e, 3);
  emit_load_flags(e, 3);
  e.pop(M(3) | M(R_PC));

  // ---- call2: `bl call2; .word a; .word b; .word fn` -> fn(ctx, a, b) ----
  rt.call2 = e.cur();
  e.ldr(1, R_LR, 0);
  e.ldr(2, R_LR, 4);
  e.ldr(R_FN, R_LR, 8);
  e.add_imm(R_LR, R_LR, 12, 3);
  e.push(M(3) | M(R_LR));
  e.mov(0, R_CTX);
  e.bl(rt.call_full);
  e.pop(M(3) | M(R_PC));

  // ---- poll: `bl poll; .word next_key` ----
  rt.poll = e.cur();
  {
    std::vector<size_t> leave;
    e.mrs_apsr(3);
    emit_poll(e, leave);
    e.msr_apsr_nzcvq(3);
    e.add_imm(R_LR, R_LR, 4, 0);
    e.bx(R_LR);
    for (size_t f : leave) e.bind(f);
    e.msr_apsr_nzcvq(3);
    e.ldr(0, R_LR, 0);
    e.b(rt.exit_key);
  }

  // ---- slow loads/stores: r1 = address (r2 = value) -> r0; r1-r3, r12 preserved ----
  {
    const void* lds[3] = {reinterpret_cast<const void*>(&jit_h_ld8), reinterpret_cast<const void*>(&jit_h_ld16), reinterpret_cast<const void*>(&jit_h_ld32)};
    const void* sts[3] = {reinterpret_cast<const void*>(&jit_h_st8), reinterpret_cast<const void*>(&jit_h_st16), reinterpret_cast<const void*>(&jit_h_st32)};
    for (int k = 0; k < 6; ++k) {
      (k < 3 ? rt.slow_load[k] : rt.slow_store[k - 3]) = e.cur();
      e.push(M(0) | M(1) | M(2) | M(3) | M(R_FN) | M(R_LR));
      e.mov(0, R_CTX);
      e.mov_ptr(R_FN, k < 3 ? lds[k] : sts[k - 3]);
      e.bl(rt.call_pure);
      e.str(0, R_SP, 0);
      e.pop(M(0) | M(1) | M(2) | M(3) | M(R_FN) | M(R_PC));
    }
  }
  rt.merge_keep_cv = rt.merge_set_c = nullptr;

  // ---- per-CPU stubs ----
  for (int c = 0; c < 2; ++c) {
    JitCpu& jc = rt.cpus[c];
    const u8* lut = rt.arena + c * LUT_STRIDE;

    // dispatch: r0 = key. LUT entry = (native offset << 32) | key.
    jc.dispatch = e.cur();
    e.mrs_apsr(R_FN);
    e.ubfx(2, 0, 1, LUT_BITS);
    e.mov_ptr(1, lut);
    e.add_reg(1, 1, 2, LSL, 3);
    e.ldrd(2, 1, 0);
    e.cmp_reg(2, 0);
    size_t miss = e.b_fwd(NE);
    e.msr_apsr_nzcvq(R_FN);
    e.mov_ptr(1, rt.arena);
    e.add_reg(R_PC, 1, 3);
    e.bind(miss);
    e.msr_apsr_nzcvq(R_FN);
    e.mov(1, 0);
    e.mov(0, R_CTX);
    e.mov_ptr(R_FN, reinterpret_cast<const void*>(&jit_h_lookup));
    e.bl(rt.call_pure);
    e.bx(0);

    // link: `bl link; .word key`
    jc.link = e.cur();
    e.ldr(1, R_LR, 0);
    e.sub_imm(2, R_LR, 4, 3);
    e.mov(0, R_CTX);
    e.mov_ptr(R_FN, reinterpret_cast<const void*>(&jit_h_link));
    e.bl(rt.call_pure);
    e.bx(0);

    // fallback: `bl fallback; .word instr; .word key`; interprets one instruction.
    jc.fallback = e.cur();
    {
      e.ldr(1, R_LR, 0);
      e.ldr(2, R_LR, 4);
      e.add_imm(R_LR, R_LR, 8, 3);
      e.push(M(2) | M(R_LR));
      emit_save_flags(e, 3, R_LR);
      emit_store_pinned(e, 3);
      e.add_imm(3, R_BUDGET, 1, 3);
      e.str(3, R_CTX, OFF_BUDGET);
      e.mov(0, R_CTX);
      e.mov_ptr(R_FN, reinterpret_cast<const void*>(&jit_h_fallback));
      e.blx(R_FN);
      emit_load_budget(e);
      e.cmp_imm(0, 0);
      size_t jumped = e.b_fwd(NE);
      std::vector<size_t> leave;
      emit_poll(e, leave);
      emit_load_pinned(e, 3);
      emit_load_flags(e, 3);
      e.pop(M(2) | M(R_PC));
      for (size_t f : leave) e.bind(f);
      e.pop(M(2) | M(R_LR));
      emit_load_pinned(e, 3);
      emit_load_flags(e, 3);
      e.and_imm(3, 2, 1);                   // key + 4 - 2T
      e.add_imm(0, 2, 4, 0);
      e.sub_reg(0, 0, 3, LSL, 1);
      e.b(rt.exit_key);
      e.bind(jumped);
      e.pop(M(2) | M(R_LR));
      std::vector<size_t> leave2;
      emit_poll(e, leave2);
      emit_load_pinned(e, 3);
      emit_load_flags(e, 3);
      e.ldr(0, R_CTX, off_reg(15));
      e.ldr(1, R_CTX, OFF_CPSR);
      e.ubfx(2, 1, 5, 1);
      e.sub_imm(0, 0, 8, 3);
      e.add_reg(0, 0, 2, LSL, 2);           // r15 - 8 + 4T
      e.orr_reg(0, 0, 2);
      e.b(jc.dispatch);
      for (size_t f : leave2) e.bind(f);
      emit_load_pinned(e, 3);
      emit_load_flags(e, 3);
      e.b(rt.exit_r15);
    }

    // branch_indirect_cdi: r0 = target | T, r1 = numD, r2 = data address. Charges
    // LDM/POP-to-pc CDI cost as the interpreter does, then joins branch_indirect.
    const u32 OFF_TIMING9 = offsetof(CpuContext, timing9), OFF_TIMING7 = offsetof(CpuContext, timing7);
    jc.branch_indirect_cdi = e.cur();
    e.mrs_apsr(R_FN);
    e.dp_imm(SUB, false, R_SP, R_SP, 8);
    e.str(R_FN, R_SP, 0);
    e.tst_imm(0, 1);
    e.and_imm(0, 0, ~3u, EQ);
    e.and_imm(3, 0, ~1u);                       // a
    if (c == 0) {
      // numC after the jump: ARM cost(a+4, S); Thumb odd cost(a+2, S); Thumb even 0.
      e.ldr(R_LR, R_CTX, OFF_TIMING9);
      size_t thumb = e.b_fwd(NE);
      e.add_imm(R_FN, 3, 4, R_FN);
      size_t fetch = e.b_fwd();
      e.bind(thumb);
      e.tst_imm(3, 2);
      e.mov_imm(R_FN, 0, EQ);
      size_t even = e.b_fwd(EQ);
      e.add_imm(R_FN, 3, 2, R_FN);
      e.bind(fetch);
      // r12 = cost(x = r12, S): tbl[x >> 12][0], 0xFF -> (x & 0x1F) == 0 ? 3 : 1
      e.lsr_imm(3, R_FN, 12);
      e.add_reg(3, R_LR, 3, LSL, 3);
      e.ldrb(3, 3, 0);
      e.cmp_imm(3, 0xFF);
      size_t plain = e.b_fwd(NE);
      e.tst_imm(R_FN, 0x1F);
      e.mov_imm(3, 3, EQ);
      e.mov_imm(3, 1, NE);
      e.bind(plain);
      e.mov(R_FN, 3);
      e.bind(even);
      // cost = max3(nc + d - 6, nc, d)
      e.add_reg(3, R_FN, 1);
      e.sub_imm(3, 3, 6, R_LR);
      e.cmp_reg(R_FN, 1);
      e.mov(R_FN, 1, LS);
      e.cmp_reg(3, R_FN);
      e.mov(R_FN, 3, GT);                       // signed
      e.sub_reg(R_BUDGET, R_BUDGET, R_FN);
    } else {
      // numC = t_new[T ? 0 : 2], then charge_CDI's main-RAM rules (code = target, data = r2).
      e.ldr(R_LR, R_CTX, OFF_TIMING7);
      e.lsr_imm(R_FN, 3, 15);
      e.add_reg(R_FN, R_LR, R_FN, LSL, 2);
      e.tst_imm(0, 1);
      e.ldrb(R_FN, R_FN, 0, NE);
      e.ldrb(R_FN, R_FN, 2, EQ);                // nc
      e.lsr_imm(R_LR, 3, 24);
      e.lsr_imm(3, 2, 24);
      e.cmp_imm(3, 2);
      size_t not_data_main = e.b_fwd(NE);
      e.cmp_imm(R_LR, 2);
      e.add_reg(R_FN, R_FN, 1, LSL, 0, EQ);     // both main: nc + d
      size_t done = e.b_fwd(EQ);
      e.add_imm(R_FN, R_FN, 1, 3);              // nc + 1
      size_t maxform = e.b_fwd();
      e.bind(not_data_main);
      e.cmp_imm(R_LR, 2);
      size_t plain = e.b_fwd(NE);
      e.add_imm(1, 1, 1, 3);                    // d + 1
      e.bind(maxform);                          // max3(nc + d - 3, nc, d)
      e.add_reg(3, R_FN, 1);
      e.sub_imm(3, 3, 3, R_LR);
      e.cmp_reg(R_FN, 1);
      e.mov(R_FN, 1, LS);
      e.cmp_reg(3, R_FN);
      e.mov(R_FN, 3, GT);
      size_t done2 = e.b_fwd();
      e.bind(plain);                            // neither main: nc + d + 1
      e.add_reg(R_FN, R_FN, 1);
      e.add_imm(R_FN, R_FN, 1, 3);
      e.bind(done);
      e.bind(done2);
      e.sub_reg(R_BUDGET, R_BUDGET, R_FN);
    }
    size_t to_body = e.b_fwd();

    // branch_indirect: r0 = target | T.
    jc.branch_indirect = e.cur();
    e.mrs_apsr(R_FN);
    e.dp_imm(SUB, false, R_SP, R_SP, 8);
    e.str(R_FN, R_SP, 0);
    e.tst_imm(0, 1);
    e.and_imm(0, 0, ~3u, EQ);
    e.bind(to_body);
    e.ldr(1, R_CTX, OFF_CPSR);
    e.bfi(1, 0, 5, 1);
    e.str(1, R_CTX, OFF_CPSR);
    e.and_imm(2, 0, ~1u);                       // a
    if (c == 0) {
      // ARM9 refill: ARM cost(a,B)+cost(a+4,S); Thumb odd cost(a-2,B)+cost(a+2,S), even cost(a,B).
      // first = a - 2*odd (B fetch), second = a + 4 - 2*T (S fetch, dropped when T && !odd).
      e.ldr(R_LR, R_CTX, OFF_TIMING9);
      e.and_imm(3, 0, 1);
      e.ubfx(R_FN, 2, 1, 1);                    // odd
      e.sub_reg(1, 2, R_FN, LSL, 1);
      e.bic_reg(R_FN, 3, R_FN);
      e.sub_reg(3, 2, 3, LSL, 1);
      e.add_imm(3, 3, 4, 2);                    // a is dead now
      e.lsr_imm(1, 1, 12);
      e.add_reg(1, R_LR, 1, LSL, 3);
      e.ldrb(1, 1, 0);
      e.cmp_imm(1, 0xFF);
      e.mov_imm(1, 3, EQ);
      e.lsr_imm(2, 3, 12);
      e.add_reg(2, R_LR, 2, LSL, 3);
      e.ldrb(2, 2, 0);
      e.cmp_imm(2, 0xFF);
      size_t have2 = e.b_fwd(NE);
      e.tst_imm(3, 0x1F);
      e.mov_imm(2, 3, EQ);
      e.mov_imm(2, 1, NE);
      e.bind(have2);
      e.cmp_imm(R_FN, 0);
      e.mov_imm(2, 0, NE);
      e.add_reg(1, 1, 2);
    } else {
      // ARM7 refill: t = timing7[a >> 15]; Thumb t0 + t1, ARM t2 + t3.
      e.ldr(R_LR, R_CTX, OFF_TIMING7);
      e.lsr_imm(1, 2, 15);
      e.add_reg(1, R_LR, 1, LSL, 2);
      e.tst_imm(0, 1);
      e.ldrb(3, 1, 0, NE);
      e.ldrb(1, 1, 1, NE);
      e.ldrb(3, 1, 2, EQ);
      e.ldrb(1, 1, 3, EQ);
      e.add_reg(1, 1, 3);
    }
    e.sub_reg(R_BUDGET, R_BUDGET, 1);
    e.ldr(R_FN, R_SP, 0);
    e.dp_imm(ADD, false, R_SP, R_SP, 8);
    e.msr_apsr_nzcvq(R_FN);
    e.b(jc.dispatch);
  }

  rt.stubs_end = (e.size() + 63) & ~size_t{63};
  rt.pos = rt.stubs_end;
  sync_icache(rt.arena + LUT_AREA, rt.stubs_end - LUT_AREA);
}

// Killed block: `movw r0; movt r0; b dispatch` = the 12 bytes of ENTRY_PATCH.
void write_entry_redirect(u8* entry, u32 key, const u8* dispatch) {
  Emitter e(entry, ENTRY_PATCH);
  e.movw(0, key & 0xFFFF);
  e.movt(0, key >> 16);
  e.b(dispatch);
}

bool fastmem_capable() { return true; }

void patch_link(u8* site, const u8* target) {
  Emitter::patch(site, 0xEA000000u | Emitter::rel24_from(site, target));
}

} // namespace backend
} // namespace ds::jit
