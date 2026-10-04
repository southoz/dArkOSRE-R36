// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// AArch64 stubs and runtime code patches. Stubs are entered by `bl`; literal args
// follow the bl and are read/skipped via x30. x17 and x30 are free temps in stubs.
#include "core/cpu/jit/jit_internal.h"
#include "core/cpu/timing_mode.h"
#include "core/cpu/jit/a64/convention.h"
#include "core/cpu/jit/a64/emit.h"
#include "core/mem/timing.h"
#include "core/sched/scheduler.h"

#include <cstring>
#include <vector>

namespace ds::jit {

// Returns x0 = context (nullptr ends the run), x1 = native entry.
extern "C" ds::SliceNext ds_slice_next(void* scheduler);

namespace {
void emit_store_callee_saved_guest(Emitter& e) {
  for (u32 r = 0; r < 8; r += 2) e.stp_w(host_reg(r), host_reg(r + 1), R_CTX, off_reg(r));
  e.stp_w(host_reg(13), host_reg(14), R_CTX, off_reg(13));
}
void emit_load_callee_saved_guest(Emitter& e) {
  for (u32 r = 0; r < 8; r += 2) e.ldp_w(host_reg(r), host_reg(r + 1), R_CTX, off_reg(r));
  e.ldp_w(host_reg(13), host_reg(14), R_CTX, off_reg(13));
}
// R_BUDGET holds budget - 1.
void emit_store_caller_saved_guest(Emitter& e) {
  e.stp_w(host_reg(8), host_reg(9), R_CTX, off_reg(8));
  e.stp_w(host_reg(10), host_reg(11), R_CTX, off_reg(10));
  e.str_w(host_reg(12), R_CTX, off_reg(12));
  e.add_imm(17, R_BUDGET, 1);
  e.str_w(17, R_CTX, OFF_BUDGET);
}
void emit_load_caller_saved_guest(Emitter& e) {
  e.ldp_w(host_reg(8), host_reg(9), R_CTX, off_reg(8));
  e.ldp_w(host_reg(10), host_reg(11), R_CTX, off_reg(10));
  e.ldr_w(host_reg(12), R_CTX, off_reg(12));
  e.ldr_w(R_BUDGET, R_CTX, OFF_BUDGET);
  e.sub_imm(R_BUDGET, R_BUDGET, 1);
  e.ldr_x(17, R_CTX, OFF_JIT);
  e.ldr_x(R_PT, 17, OFF_JC_PT);
  e.ldr_x(R_TIM, 17, OFF_JC_TIM);
  e.ldr_x(R_ARENA, 17, OFF_JC_ARENA);
}
void emit_save_flags(Emitter& e, u32 t0, u32 t1) {
  e.mrs_nzcv(t0);
  e.ldr_w(t1, R_CTX, OFF_CPSR);
  e.and_imm(t1, t1, 0x0FFFFFFF);
  e.orr_reg(t1, t1, t0);
  e.str_w(t1, R_CTX, OFF_CPSR);
}
void emit_load_flags(Emitter& e, u32 t0) {
  e.ldr_w(t0, R_CTX, OFF_CPSR);
  e.msr_nzcv(t0);
}

// ARM9 fetch cost: c = tbl[a>>12][0]; c == 0xFF ? ((branch || !(a & 0x1F)) ? 3 : 1) : c
void emit_fetch_cost9(Emitter& e, u32 wa, u32 wc, u32 t, bool branch) {
  e.lsr_imm(t, wa, 12);
  e.add_reg(t, R_TIM, t, LSL, 3, true);
  e.ldrb(wc, t, 0);
  e.cmp_imm(wc, 0xFF);
  if (branch) {
    e.movz(t, 3);
    e.csel(wc, t, wc, EQ);
  } else {
    size_t skip = e.b_cond_fwd(NE);
    e.tst_imm(wa, 0x1F);
    e.movz(t, 3);
    e.movz(wc, 1);
    e.csel(wc, t, wc, EQ);
    e.bind(skip);
  }
}

// Branches to `leave` on budget exhausted, alert, or unmasked IRQ. Clobbers w3.
void emit_poll(Emitter& e, std::vector<size_t>& leave) {
  leave.push_back(e.tbnz_fwd(R_BUDGET, 31));
  e.ldr_w(3, R_CTX, OFF_ALERTS);
  leave.push_back(e.cbnz_fwd(3));
  e.ldr_w(3, R_CTX, OFF_IRQ);
  size_t ok = e.cbz_fwd(3);
  e.ldr_w(3, R_CTX, OFF_CPSR);
  size_t ok2 = e.tbnz_fwd(3, 7);
  leave.push_back(e.b_fwd());
  e.bind(ok);
  e.bind(ok2);
}

} // namespace

namespace backend {

void emit_stubs(Runtime& rt) {
  Emitter e(rt.arena, rt.cap);
  e.set_pos(LUT_AREA);

  // ---- enter(ctx, native): C-callable ----
  rt.enter = reinterpret_cast<void (*)(CpuContext*, const void*)>(e.cur());
  e.stp_x_pre(29, 30, SP, -96);
  e.stp_x(19, 20, SP, 16);
  e.stp_x(21, 22, SP, 32);
  e.stp_x(23, 24, SP, 48);
  e.stp_x(25, 26, SP, 64);
  e.stp_x(27, 28, SP, 80);
  size_t to_light = e.bl_fwd();
  e.ldp_x(19, 20, SP, 16);
  e.ldp_x(21, 22, SP, 32);
  e.ldp_x(23, 24, SP, 48);
  e.ldp_x(25, 26, SP, 64);
  e.ldp_x(27, 28, SP, 80);
  e.ldp_x_post(29, 30, SP, 96);
  e.ret();
  // ---- enter_light(ctx, native): saves only x29/x30; exits return here ----
  e.bind(to_light);
  rt.enter_light = e.cur();
  e.stp_x_pre(29, 30, SP, -16);
  e.mov(R_CTX, 0, true);
  emit_load_callee_saved_guest(e);
  emit_load_caller_saved_guest(e);
  emit_load_flags(e, 17);
  e.br(1);

  // ---- exit_key_lit: `bl exit_key_lit; .word key` ----
  rt.exit_key_lit = e.cur();
  e.ldr_w(0, 30, 0);
  // ---- exit_key: w0 = key of the next instruction ----
  rt.exit_key = e.cur();
  e.and_imm(1, 0, 1);
  e.and_imm(2, 0, ~1u);
  e.add_imm(2, 2, 8);
  e.sub_reg(2, 2, 1, LSL, 2);         // pc + 8 - 4T
  e.str_w(2, R_CTX, off_reg(15));
  // exit_r15: ctx.r15 already correct
  rt.exit_r15 = e.cur();
  emit_store_callee_saved_guest(e);
  emit_store_caller_saved_guest(e);
  emit_save_flags(e, 17, 30);
  e.ldp_x_post(29, 30, SP, 16);
  e.ret();

  // ---- run_loop(scheduler): the native slice loop ----
  rt.run_loop = reinterpret_cast<void (*)(void*)>(e.cur());
  e.stp_x_pre(29, 30, SP, -112);
  e.stp_x(19, 20, SP, 16);
  e.stp_x(21, 22, SP, 32);
  e.stp_x(23, 24, SP, 48);
  e.stp_x(25, 26, SP, 64);
  e.stp_x(27, 28, SP, 80);
  e.str_x(0, SP, 96);
  const size_t loop_top = e.size();
  e.ldr_x(0, SP, 96);
  e.mov_imm64(16, reinterpret_cast<u64>(&ds_slice_next));
  e.blr(16);
  size_t loop_exit = e.cbz_fwd(0, true);
  e.bl(rt.enter_light);
  e.b(e.base() + loop_top);
  e.bind(loop_exit);
  e.ldp_x(19, 20, SP, 16);
  e.ldp_x(21, 22, SP, 32);
  e.ldp_x(23, 24, SP, 48);
  e.ldp_x(25, 26, SP, 64);
  e.ldp_x(27, 28, SP, 80);
  e.ldp_x_post(29, 30, SP, 112);
  e.ret();

  // ---- flush_exit: arena full; ctx.r15 already set ----
  rt.flush_exit = e.cur();
  e.mov_imm64(1, reinterpret_cast<u64>(&rt.need_reset));
  e.movz(2, 1);
  e.strb(2, 1, 0);
  e.b(rt.exit_r15);

  // ---- call_pure: x16 = fn, args x0-x3 ----
  rt.call_pure = e.cur();
  e.str_x_pre(30, SP, -16);
  emit_save_flags(e, 17, 30);
  emit_store_caller_saved_guest(e);
  e.blr(R_FN);
  emit_load_caller_saved_guest(e);
  emit_load_flags(e, 17);
  e.ldr_x_post(30, SP, 16);
  e.ret();

  // ---- call_full: x16 = fn, args x0-x3 ----
  rt.call_full = e.cur();
  e.str_x_pre(30, SP, -16);
  emit_save_flags(e, 17, 30);
  emit_store_caller_saved_guest(e);
  emit_store_callee_saved_guest(e);
  e.blr(R_FN);
  emit_load_callee_saved_guest(e);
  emit_load_caller_saved_guest(e);
  emit_load_flags(e, 17);
  e.ldr_x_post(30, SP, 16);
  e.ret();

  // ---- call2: `bl call2; .word a; .word b; .xword fn` -> call_full fn(ctx, a, b) ----
  rt.call2 = e.cur();
  e.ldp_w(1, 2, 30, 0);
  e.ldr_x(R_FN, 30, 8);
  e.add_imm(30, 30, 16, true);
  e.str_x_pre(30, SP, -16);
  e.mov(0, R_CTX, true);
  e.bl(rt.call_full);
  e.ldr_x_post(30, SP, 16);
  e.ret();

  // ---- poll: `bl poll; .word next_key` ----
  rt.poll = e.cur();
  {
    std::vector<size_t> leave;
    emit_poll(e, leave);
    e.add_imm(30, 30, 4, true);
    e.ret();
    for (size_t f : leave) e.bind(f);
    e.ldr_w(0, 30, 0);
    e.b(rt.exit_key);
  }

  // ---- slow loads/stores: w1 = address (w2 = value); x1, x7 preserved ----
  {
    const void* lds[3] = {reinterpret_cast<const void*>(&jit_h_ld8), reinterpret_cast<const void*>(&jit_h_ld16), reinterpret_cast<const void*>(&jit_h_ld32)};
    const void* sts[3] = {reinterpret_cast<const void*>(&jit_h_st8), reinterpret_cast<const void*>(&jit_h_st16), reinterpret_cast<const void*>(&jit_h_st32)};
    for (int k = 0; k < 6; ++k) {
      (k < 3 ? rt.slow_load[k] : rt.slow_store[k - 3]) = e.cur();
      e.stp_x_pre(30, 1, SP, -32);
      e.str_x(7, SP, 16);
      e.mov(0, R_CTX, true);
      e.mov_imm64(R_FN, reinterpret_cast<u64>(k < 3 ? lds[k] : sts[k - 3]));
      e.bl(rt.call_pure);
      e.ldr_x(7, SP, 16);
      e.ldp_x_post(30, 1, SP, 32);
      e.ret();
    }
  }

  // ---- merge_keep_cv: N,Z from w0; C,V unchanged ----
  rt.merge_keep_cv = e.cur();
  e.mrs_nzcv(1);
  e.tst_reg(0, 0);
  e.mrs_nzcv(2);
  e.ubfx(3, 1, 28, 2, true);
  e.bfi(2, 3, 28, 2, true);
  e.msr_nzcv(2);
  e.ret();
  // ---- merge_set_c: N,Z from w0, C from w1 (0/1); V unchanged ----
  rt.merge_set_c = e.cur();
  e.mrs_nzcv(2);
  e.tst_reg(0, 0);
  e.mrs_nzcv(3);
  e.ubfx(2, 2, 28, 1, true);
  e.bfi(3, 2, 28, 1, true);
  e.bfi(3, 1, 29, 1, true);
  e.msr_nzcv(3);
  e.ret();

  // ---- per-CPU stubs ----
  for (int c = 0; c < 2; ++c) {
    JitCpu& jc = rt.cpus[c];

    // dispatch: w0 = key. LUT entry = (native offset << 32) | key, at R_ARENA.
    jc.dispatch = e.cur();
    e.ubfx(2, 0, 1, LUT_BITS);
    if (c != 0) {
      const bool ok = e.orr_imm(2, 2, static_cast<u32>(LUT_STRIDE / 8));
      assert(ok && "LUT_STRIDE/8 must be a logical immediate"); (void)ok;
    }
    e.ldr_x_reg(3, R_ARENA, 2, true, true);
    e.eor_reg(4, 3, 0);
    size_t miss = e.cbnz_fwd(4);
    e.lsr_imm(3, 3, 32, true);
    e.add_reg(3, R_ARENA, 3, LSL, 0, true);
    e.br(3);
    e.bind(miss);
    e.mov(1, 0);
    e.mov(0, R_CTX, true);
    e.mov_imm64(R_FN, reinterpret_cast<u64>(&jit_h_lookup));
    e.bl(rt.call_pure);
    e.br(0);

    // link: `bl link; .word key`
    jc.link = e.cur();
    e.ldr_w(1, 30, 0);
    e.sub_imm(2, 30, 4, true);
    e.mov(0, R_CTX, true);
    e.mov_imm64(R_FN, reinterpret_cast<u64>(&jit_h_link));
    e.bl(rt.call_pure);
    e.br(0);

    // fallback: `bl fallback; .word instr; .word key`; interprets one instruction.
    jc.fallback = e.cur();
    {
      e.ldp_w(1, 2, 30, 0);
      e.add_imm(30, 30, 8, true);
      e.stp_x_pre(30, 2, SP, -16);
      e.mov(0, R_CTX, true);
      e.mov_imm64(R_FN, reinterpret_cast<u64>(&jit_h_fallback));
      e.bl(rt.call_full);
      size_t jumped = e.cbnz_fwd(0);
      std::vector<size_t> leave;
      emit_poll(e, leave);
      e.ldp_x_post(30, 2, SP, 16);
      e.ret();
      for (size_t f : leave) e.bind(f);
      e.ldp_x_post(30, 2, SP, 16);
      e.and_imm(3, 2, 1);               // key + 4 - 2T
      e.add_imm(0, 2, 4);
      e.sub_reg(0, 0, 3, LSL, 1);
      e.b(rt.exit_key);
      e.bind(jumped);
      e.ldp_x_post(30, 2, SP, 16);
      std::vector<size_t> leave2;
      emit_poll(e, leave2);
      // key = (r15 - 8 + 4T) | T
      e.ldr_w(0, R_CTX, off_reg(15));
      e.ldr_w(1, R_CTX, OFF_CPSR);
      e.ubfx(2, 1, 5, 1);
      e.sub_imm(0, 0, 8);
      e.add_reg(0, 0, 2, LSL, 2);
      e.orr_reg(0, 0, 2);
      e.b(jc.dispatch);
      for (size_t f : leave2) e.bind(f);
      e.b(rt.exit_r15);
    }

    // branch_indirect_cdi: w0 = target | T, w1 = numD, w2 = data address. Charges
    // LDM/POP-to-pc CDI cost as the interpreter does, then joins branch_indirect.
    // Guest flags parked in x17.
    jc.branch_indirect_cdi = e.cur();
    e.mrs_nzcv(17);
    {
      size_t is_thumb = e.tbnz_fwd(0, 0);
      e.and_imm(0, 0, ~3u);
      e.bind(is_thumb);
      e.and_imm(3, 0, ~1u);                 // a
      if (c == 0) {
        // numC = (T && (a + 4) & 2) ? 0 : code_after;  code_after: ARM cost(a+4,S); Thumb a&2 ? cost(a+2,S) : cost(a,B)
        size_t thumb = e.tbnz_fwd(0, 0);
        e.add_imm(5, 3, 4);
        emit_fetch_cost9(e, 5, 4, 6, false);
        size_t done = e.b_fwd();
        e.bind(thumb);
        size_t odd = e.tbnz_fwd(3, 1);
        emit_fetch_cost9(e, 3, 4, 6, true);
        e.movz(4, 0);                       // even a: new r15 has bit 1 set -> numC 0
        size_t done2 = e.b_fwd();
        e.bind(odd);
        e.add_imm(5, 3, 2);
        emit_fetch_cost9(e, 5, 4, 6, false);
        e.bind(done);
        e.bind(done2);
        // cost = max(numC + numD - 6, max(numC, numD))
        e.add_imm(5, 4, 0);
        e.add_reg(5, 4, 1);
        e.sub_imm(5, 5, 6);
        e.cmp_reg(4, 1);
        e.csel(6, 4, 1, HI);
        e.cmp_reg(5, 6);
        e.csel(5, 5, 6, GT);
      } else {
        // numC = t_new[T ? 0 : 2]; main-RAM rules with code region = target, data region = w2
        e.lsr_imm(4, 3, 15);
        e.add_reg(4, R_TIM, 4, LSL, 2, true);
        size_t thumb = e.tbnz_fwd(0, 0);
        e.ldrb(4, 4, 2);
        size_t done = e.b_fwd();
        e.bind(thumb);
        e.ldrb(4, 4, 0);
        e.bind(done);
        e.lsr_imm(5, 3, 24); e.cmp_imm(5, 2); e.cset(5, EQ);     // code_main
        e.lsr_imm(6, 2, 24); e.cmp_imm(6, 2); e.cset(6, EQ);     // data_main
        // data_main ? (code_main ? nC + d : (nC+1, max(nC+d-3, max(nC,d)))) : (code_main ? (d+1, max(...)) : nC + d + 1)
        size_t not_main = e.cbz_fwd(6);
        size_t both = e.cbnz_fwd(5);
        e.add_imm(4, 4, 1);
        size_t mx = e.b_fwd();
        e.bind(both);
        e.add_reg(5, 4, 1);
        size_t fin = e.b_fwd();
        e.bind(not_main);
        size_t plain = e.cbz_fwd(5);
        e.add_imm(1, 1, 1);
        e.bind(mx);
        e.add_reg(5, 4, 1); e.sub_imm(5, 5, 3);
        e.cmp_reg(4, 1); e.csel(6, 4, 1, HI);
        e.cmp_reg(5, 6); e.csel(5, 5, 6, GT);
        size_t fin2 = e.b_fwd();
        e.bind(plain);
        e.add_reg(5, 4, 1); e.add_imm(5, 5, 1);
        e.bind(fin);
        e.bind(fin2);
      }
      e.sub_reg(R_BUDGET, R_BUDGET, 5);
    }
    size_t to_body = e.b_fwd();

    // Updates T, charges the refill (w0 = key after); guest flags parked in x17.
    auto refill_body = [&] {
      size_t is_thumb = e.tbnz_fwd(0, 0);
      e.and_imm(0, 0, ~3u);
      e.bind(is_thumb);
      e.ldr_w(1, R_CTX, OFF_CPSR);
      e.bfi(1, 0, 5, 1);
      e.str_w(1, R_CTX, OFF_CPSR);
      e.and_imm(2, 0, ~1u);
      if (g_fast_timing) e.movz(3, c == 0 ? g_fast.r9 : g_fast.r7);   // fast timing: a constant refill
      else if (c == 0) {
        // ARM9 refill: ARM cost(a,B)+cost(a+4,S); Thumb a&2 ? cost(a-2,B)+cost(a+2,S) : cost(a,B).
        // Byte from Timing's refill9 table: page of first = a - 2*odd; index by second =
        // a + 4 - 2*T: 3 if dropped (T && !odd), 2 if page start, 1 if line start, else 0.
        e.and_imm(4, 0, 1);                       // T
        e.ubfx(5, 2, 1, 1);                       // odd
        e.sub_reg(3, 2, 5, LSL, 1);
        e.lsr_imm(3, 3, 12);
        e.add_imm(7, 2, 4);
        e.sub_reg(7, 7, 4, LSL, 1);
        e.bic_reg(4, 4, 5);
        e.tst_imm(7, 0xFFF);
        e.cset(6, EQ);
        e.tst_imm(7, 0x1F);
        e.csinc(6, 6, 6, NE);
        e.movz(1, 3);
        e.cmp_imm(4, 0);
        e.csel(6, 1, 6, NE);
        e.add_reg(3, R_TIM, 3, LSL, 2, true);
        e.add_imm(3, 3, mem::Timing::REFILL9_OFFSET, true);
        e.ldrb_reg(3, 3, 6);
      } else {
        // ARM7 refill: t = timing7[a >> 15]; Thumb: t0 + t1; ARM: t2 + t3
        e.lsr_imm(4, 2, 15);
        e.add_reg(4, R_TIM, 4, LSL, 2, true);
        size_t thumb = e.tbnz_fwd(0, 0);
        e.ldrb(3, 4, 2);
        e.ldrb(5, 4, 3);
        size_t done = e.b_fwd();
        e.bind(thumb);
        e.ldrb(3, 4, 0);
        e.ldrb(5, 4, 1);
        e.bind(done);
        e.add_reg(3, 3, 5);
      }
      e.sub_reg(R_BUDGET, R_BUDGET, 3);
      e.msr_nzcv(17);
    };

    // branch_indirect: w0 = target | T
    jc.branch_indirect = e.cur();
    e.mrs_nzcv(17);
    e.bind(to_body);
    refill_body();
    e.b(jc.dispatch);

    // branch_indirect_poll: the same, then the interpreter's per-instruction IRQ
    // check: an exception return (SUBS pc, lr / MOVS pc) may unmask a pending IRQ,
    // taken before the target's first instruction.
    jc.branch_indirect_poll = e.cur();
    e.mrs_nzcv(17);
    refill_body();
    {
      std::vector<size_t> leave;
      emit_poll(e, leave);
      e.b(jc.dispatch);
      for (size_t f : leave) e.bind(f);
      e.b(rt.exit_key);
    }
  }

  rt.stubs_end = (e.size() + 63) & ~size_t{63};
  rt.pos = rt.stubs_end;
  sync_icache(rt.arena + LUT_AREA, rt.stubs_end - LUT_AREA);
}

// Killed block: `movz/movk w0; b dispatch` = the 12 bytes of ENTRY_PATCH.
void write_entry_redirect(u8* entry, u32 key, const u8* dispatch) {
  Emitter e(entry, ENTRY_PATCH);
  e.movz(0, key & 0xFFFF);
  e.movk(0, key >> 16, 16);
  e.b(dispatch);
}

bool fastmem_capable() { return true; }

void patch_link(u8* site, const u8* target) {
  const s64 delta = target - site;
  const u32 w = 0x14000000u | (static_cast<u32>(delta >> 2) & 0x03FFFFFFu);
  Emitter::patch(site, w);
}

} // namespace backend
} // namespace ds::jit
