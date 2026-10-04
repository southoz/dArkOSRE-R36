// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// DSi ARM9 BIOS SHA-1 block loop (0xFFFF0A10, r0 = five state words, r1 =
// data, r2 = length), run natively by the recompiler. The loop body has no
// data-dependent branch, so an iteration's effect and cycle cost are a pure
// function of its inputs; this reproduces exactly the register/memory state
// the BIOS code leaves, and charges what the recompiled instructions would.
// Left only between iterations (budget or IRQ pending), so the ARM9 can
// overshoot a slice by one iteration. Recognised by the SHA-1 of its bytes;
// the cycle pattern is decoded from those bytes on first arm.
#include "core/cpu/jit/jit_internal.h"
#include "core/cpu/cpu_cycles.h"
#include "core/crypto/sha1.h"
#include "core/mem/timing.h"
#include "core/nds.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace ds::jit {

namespace {

constexpr u32 FN_START = 0xFFFF0A10, LOOP = 0xFFFF0A18, BRANCH = 0xFFFF1E44, EXIT = 0xFFFF1E48, FN_END = 0xFFFF1E50;
constexpr u8 ROUTINE_SHA1[20] = {0x94, 0xc5, 0xa3, 0x4b, 0xc5, 0x2d, 0x9c, 0x10, 0xca, 0xd6,
                                 0x1d, 0x3e, 0x7d, 0x69, 0xec, 0x21, 0x08, 0x18, 0x4a, 0x3e};

// One instruction of the loop body as the cost model sees it.
enum class Kind : u8 { Plain, Lit, R1Load, SpLoad, SpStore, Ldm, Stm };
struct Op { u32 pc; Kind kind; s32 arg; u8 count; };   // arg: literal address, R1 word index, or sp offset; count: LDM/STM words

struct Shape {
  bool decoded = false, ok = false;
  std::vector<Op> ops;           // LOOP .. BRANCH-4, in order, less the r1 loads
  Op r1[16];                     // the r1 loads (their cost follows r1 every iteration)
};
Shape g_shape;

// Decode the loop body into cost ops; unrecognized forms leave the hook off.
bool decode_shape(CpuContext& cpu) {
  g_shape.decoded = true;
  mem::Bus& bus = cpu.nds->bus;
  u8 bytes[FN_END - FN_START];
  for (u32 a = FN_START; a < FN_END; a += 4) {   // as the translator fetches (the ARM9 BIOS is mapped)
    u32 w;
    if (const u8* p = cpu.page_table.read_ptr(a)) std::memcpy(&w, p, 4); else w = bus.fetch(Cpu::ARM9, a, 32);
    std::memcpy(bytes + (a - FN_START), &w, 4);
  }
  u8 digest[20];
  crypto::sha1(bytes, sizeof bytes, digest);
  if (std::memcmp(digest, ROUTINE_SHA1, 20) != 0) return false;
  s32 sp = 0;          // sp relative to its value at the loop head
  s32 r1_words = 0;
  for (u32 pc = LOOP; pc < BRANCH; pc += 4) {
    u32 w; std::memcpy(&w, bytes + (pc - FN_START), 4);
    if ((w >> 28) != 0xE) return false;
    const u32 rn = (w >> 16) & 0xF;
    if ((w & 0x0C000000) == 0) {                                   // data processing, immediate or immediate-shift operands
      if ((w & 0x02000090) == 0x00000090 || (w & 0x02000010) == 0x00000010) return false;   // register shifts / extension space
      if (((w >> 12) & 0xF) == 13) {                               // add sp, sp, #imm
        if ((w & 0x0FFFF000) != 0x028DD000) return false;
        sp += static_cast<s32>(w & 0xFF);
      }
      g_shape.ops.push_back({pc, Kind::Plain, 0, 0});
    } else if ((w & 0x0E000000) == 0x04000000) {                   // LDR/STR, immediate offset, word
      if (w & (1u << 22)) return false;
      const bool p = w & (1u << 24), u = w & (1u << 23), wb = w & (1u << 21), load = w & (1u << 20);
      const s32 imm = static_cast<s32>(w & 0xFFF) * (u ? 1 : -1);
      if (rn == 15 && load && p && !wb) g_shape.ops.push_back({pc, Kind::Lit, static_cast<s32>(pc + 8 + imm), 0});
      else if (rn == 1 && load && !p && imm == 4) { if (r1_words == 16) return false; g_shape.r1[r1_words] = {pc, Kind::R1Load, r1_words, 0}; ++r1_words; }
      else if (rn == 13 && !load && p && wb && imm == -4) { sp -= 4; g_shape.ops.push_back({pc, Kind::SpStore, sp, 0}); }
      else if (rn == 13 && p && !wb) g_shape.ops.push_back({pc, load ? Kind::SpLoad : Kind::SpStore, sp + imm, 0});
      else return false;
    } else if ((w & 0x0FE08000) == 0x08800000 && rn == 0) {         // LDMIA/STMIA r0, no writeback, no user bank, no pc
      const u8 n = static_cast<u8>(__builtin_popcount(w & 0xFFFF));
      g_shape.ops.push_back({pc, (w & (1u << 20)) ? Kind::Ldm : Kind::Stm, 0, n});
    } else return false;
  }
  u32 br; std::memcpy(&br, bytes + (BRANCH - FN_START), 4);
  if (br != 0x8AFFFAF3 || sp != 0 || r1_words != 16) return false;   // bhi LOOP
  g_shape.ok = true;
  return true;
}

inline u32 max3c(s32 a, s32 b, s32 c) { return static_cast<u32>(std::max(a, std::max(b, c))); }

// A word access's data cost as the translator prices it.
s32 data_cost(const CpuContext& cpu, u32 addr, bool seq, bool store) {
  return cpu.timing9[addr >> 12][(store ? 4 : 0) + (seq ? 3 : 2)];
}

// What the recompiled instructions charge, less the R1 loads (those follow r1).
u32 base_cost(const CpuContext& cpu, u32 sp, u32 r0) {
  u32 total = 0;
  for (const Op& op : g_shape.ops) {
    const u32 pc8 = op.pc + 8;
    const u8 cc = cpu.timing9[pc8 >> 12][0];
    const s32 C = cc == 0xFF ? (!(pc8 & 0x1F) ? 3 : 1) : cc;
    s32 D = -1;
    switch (op.kind) {
    case Kind::Plain: break;
    case Kind::Lit:     D = data_cost(cpu, static_cast<u32>(op.arg), false, false); break;
    case Kind::SpLoad:  D = data_cost(cpu, sp + static_cast<u32>(op.arg), false, false); break;
    case Kind::SpStore: D = data_cost(cpu, sp + static_cast<u32>(op.arg), false, true); break;
    case Kind::Ldm: case Kind::Stm: {
      const bool store = op.kind == Kind::Stm;
      D = data_cost(cpu, r0, false, store);
      for (u32 i = 1; i < op.count; ++i) D += data_cost(cpu, r0 + 4 * i, true, store);
      break;
    }
    default: break;
    }
    total += D < 0 ? static_cast<u32>(C) : max3c(C + D - 6, C, D);
  }
  return total;
}

u32 r1_cost(const CpuContext& cpu, u32 r1) {
  u32 total = 0;
  for (const Op& op : g_shape.r1) {
    const u32 pc8 = op.pc + 8;
    const u8 cc = cpu.timing9[pc8 >> 12][0];
    const s32 C = cc == 0xFF ? (!(pc8 & 0x1F) ? 3 : 1) : cc;
    const s32 D = data_cost(cpu, r1 + 4 * static_cast<u32>(op.arg), false, false);
    total += max3c(C + D - 6, C, D);
  }
  return total;
}

inline u32 rol(u32 v, u32 n) { return (v << n) | (v >> (32 - n)); }
inline u32 bswap(u32 v) { return __builtin_bswap32(v); }

}  // namespace

bool bios_sha1_hook_wanted(CpuContext& cpu, u32 pc, bool thumb) {
  if (pc != LOOP || thumb || cpu.which != Cpu::ARM9 || !cpu.nds->dsi) return false;
  if (!g_shape.decoded) decode_shape(cpu);
  return g_shape.ok;
}

namespace {

// One iteration's outcome, computed without touching the machine.
struct Iter {
  u32 regs[16]; u32 cpsr; u32 stack[16]; u32 state[5]; u32 cost; bool again;
  u8* sw[16]; u8* stw[5];
};

bool compute(CpuContext& cpu, Iter& it) {
  const mem::PageTable& pt = cpu.page_table;
  const mem::Timing& t = cpu.nds->bus.timing();
  const auto& R = cpu.hot.regs;
  const u32 r0 = R[0], r1 = R[1], sp = R[13];
  // The bytes touched must be directly mapped, non-code, and non-overlapping.
  const u32 lo_stack = sp - 64;
  const auto overlap = [](u32 a, u32 an, u32 b, u32 bn) { return a < b + bn && b < a + an; };
  if (lo_stack > sp || overlap(r1, 64, lo_stack, 64) || overlap(r1, 64, r0, 20) || overlap(lo_stack, 64, r0, 20)) return false;
  u32 data[16];
  for (u32 k = 0; k < 16; ++k) { const u8* p = pt.read_ptr(r1 + 4 * k); if (!p) return false; std::memcpy(&data[k], p, 4); }
  bool code = false;
  for (u32 k = 0; k < 16; ++k) { it.sw[k] = pt.write_ptr(lo_stack + 4 * k, &code); if (!it.sw[k] || code) return false; }
  u32 st[5];
  for (u32 k = 0; k < 5; ++k) {
    it.stw[k] = pt.write_ptr(r0 + 4 * k, &code);
    const u8* p = pt.read_ptr(r0 + 4 * k);
    if (!it.stw[k] || code || !p) return false;
    std::memcpy(&st[k], p, 4);
  }

  // Cycles, as the recompiled loop charges them.
  static u64 cache_version = ~0ull; static u32 cache_sp = 0, cache_r0 = 0, cache_base = 0;
  if (cache_version != t.cpu9_version || cache_sp != sp || cache_r0 != r0) {
    cache_version = t.cpu9_version; cache_sp = sp; cache_r0 = r0;
    cache_base = base_cost(cpu, sp, r0);
  }
  const u32 r2 = R[2];
  it.again = r2 > 64;   // bhi after subs r2, r2, #64
  it.cost = cache_base + r1_cost(cpu, r1);
  if (it.again) it.cost += fetch_cost9(cpu, LOOP, true) + fetch_cost9(cpu, LOOP + 4, false);   // the refill
  else { const u32 pc8 = BRANCH + 8; const u8 cc = cpu.timing9[pc8 >> 12][0]; it.cost += cc == 0xFF ? (!(pc8 & 0x1F) ? 3u : 1u) : cc; }

  // SHA-1 over the block from the chaining values in the registers.
  u32 W[80];
  for (u32 k = 0; k < 16; ++k) W[k] = bswap(data[k]);
  for (u32 k = 16; k < 80; ++k) W[k] = rol(W[k - 3] ^ W[k - 8] ^ W[k - 14] ^ W[k - 16], 1);
  u32 a = R[3], b = R[9], c = R[10], d = R[11], e = R[12];
  for (u32 k = 0; k < 80; ++k) {
    u32 f, K;
    if (k < 20) { f = (b & c) | (~b & d); K = 0x5A827999; }
    else if (k < 40) { f = b ^ c ^ d; K = 0x6ED9EBA1; }
    else if (k < 60) { f = (b & c) | (b & d) | (c & d); K = 0x8F1BBCDC; }
    else { f = b ^ c ^ d; K = 0xCA62C1D6; }
    const u32 tmp = rol(a, 5) + f + e + K + W[k];
    e = d; d = c; c = rol(b, 30); b = a; a = tmp;
  }
  const u32 n[5] = {st[0] + a, st[1] + b, st[2] + c, st[3] + d, st[4] + e};
  for (u32 k = 0; k < 5; ++k) it.state[k] = n[k];
  for (u32 k = 0; k < 16; ++k) it.stack[k] = W[79 - k];
  std::memcpy(it.regs, R, sizeof it.regs);
  it.regs[3] = n[0]; it.regs[8] = n[0]; it.regs[9] = n[1]; it.regs[10] = n[2]; it.regs[11] = n[3]; it.regs[12] = n[4];
  it.regs[4] = st[0]; it.regs[5] = st[1]; it.regs[6] = st[2]; it.regs[14] = st[4]; it.regs[7] = 0xCA62C1D6;
  it.regs[1] = r1 + 64;
  const u32 res = r2 - 64;
  it.regs[2] = res;
  it.regs[15] = (it.again ? LOOP : EXIT) + 8;
  u32 cpsr = cpu.hot.cpsr & 0x0FFFFFFF;
  if (res & 0x80000000) cpsr |= 1u << 31;
  if (res == 0) cpsr |= 1u << 30;
  if (r2 >= 64) cpsr |= 1u << 29;
  if (((r2 ^ 64) & (r2 ^ res)) & 0x80000000) cpsr |= 1u << 28;
  it.cpsr = cpsr;
  return true;
}

}  // namespace

bool bios_sha1_run(CpuContext& cpu) {
  Runtime& r = rt();
  if (r.strict || cpu.thumb() || !g_shape.ok || mem::Bus::watch_active()) return false;
  bool ran = false;
  Iter it;
  while (compute(cpu, it)) {
    for (u32 k = 0; k < 5; ++k) std::memcpy(it.stw[k], &it.state[k], 4);
    for (u32 k = 0; k < 16; ++k) std::memcpy(it.sw[k], &it.stack[k], 4);
    std::memcpy(cpu.hot.regs, it.regs, sizeof it.regs);
    cpu.hot.cpsr = it.cpsr;
    cpu.hot.cycle_budget -= static_cast<s32>(it.cost);
    ran = true;
    r.stats.bios_sha1_blocks++;
    if (!it.again || cpu.hot.cycle_budget <= 0 || cpu.hot.irq_pending) break;
  }
  return ran;
}

}  // namespace ds::jit
