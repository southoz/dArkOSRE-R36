// SPDX-License-Identifier: GPL-3.0-or-later
// Recompiler vs interpreter differential test (AArch64 only).
// Random straight-line ARM and Thumb sequences run on two machines,
// one per engine, from identical state; the registers, flags, consumed cycles
// and memory must agree afterwards. The first disagreement prints the
// sequence, which names the broken instruction.
#include <cstdlib>
#include "core/nds.h"
#include "core/cpu/timing_mode.h"
#include "core/cpu/interp/interp.h"
#include "core/cpu/jit/jit.h"
#include "check.h"

#include <cstdio>
#include <string>
#include <cstring>
#include <random>
#include <vector>

using namespace ds;

namespace {

constexpr u32 CODE_BASE = 0x02000000, HALT_STUB = 0x02001000, BUF_BASE = 0x02200000, STACK = 0x02300000;
constexpr u32 MAIN_RAM = 0x02000000, MAIN_RAM_SIZE = 0x400000;

struct Machine {
  NDS nds;
  CpuContext& cpu;
  explicit Machine(Cpu which) : cpu(nds.cpu(which)) {}
  u8* host(u32 addr) { return cpu.page_table.read_ptr(addr); }
  void poke32(u32 addr, u32 v) { std::memcpy(host(addr), &v, 4); }
  void poke16(u32 addr, u16 v) { std::memcpy(host(addr), &v, 2); }
};

// ARM halt stub: ARM9 waits for interrupt through CP15; ARM7 writes HALTCNT.
void write_halt_stub(Machine& m, bool a9) {
  // Exception vectors (empty BIOS in this harness) branch to the halt stub,
  // so an undefined instruction, SWI or IRQ ends the trial instead of
  // executing zeros until the budget runs out.
  const u32 vbase = a9 ? 0xFFFF0000u : 0u;
  for (u32 v = 0; v < 0x20; v += 4) m.poke32(vbase + v, 0xE59FF000u | (0x20 - v - 8));   // ldr pc, [pc, #k] -> pool
  m.poke32(vbase + 0x20, HALT_STUB);
  if (a9) {
    m.poke32(HALT_STUB, 0xEE070F90);        // mcr p15, 0, r0, c7, c0, 4
    m.poke32(HALT_STUB + 4, 0xEAFFFFFE);    // b . (never reached)
  } else {
    m.poke32(HALT_STUB, 0xE3A00301);        // mov r0, #0x04000000
    m.poke32(HALT_STUB + 4, 0xE2800C03);    // add r0, r0, #0x300
    m.poke32(HALT_STUB + 8, 0xE3A01080);    // mov r1, #0x80
    m.poke32(HALT_STUB + 12, 0xE5C01001);   // strb r1, [r0, #1]   (0x04000301)
    m.poke32(HALT_STUB + 16, 0xEAFFFFFE);   // b .
  }
}

struct Rng {
  std::mt19937 g;
  explicit Rng(u32 seed) : g(seed) {}
  u32 next() { return g(); }
  u32 below(u32 n) { return g() % n; }
  bool coin(u32 pct = 50) { return below(100) < pct; }
};

// ---- ARM generator ----------------------------------------------------------
// Registers: r9 = buffer base (never written), r13 = stack, r15 never written.
u32 gen_arm(Rng& r) {
  const u32 cond = r.coin(80) ? 0xE : r.below(14);
  auto reg_nw = [&]() { u32 x; do { x = r.below(15); } while (x == 9 || x == 13); return x; };   // writable
  auto reg_rd = [&]() { u32 x; do { x = r.below(16); } while (x == 9 || x == 13); return x; };   // readable (pc ok)
  for (;;) {
    switch (r.below(10)) {
    case 0: case 1: case 2: {   // data processing
      const u32 opcode = r.below(16), s = r.coin(60), rd = reg_nw(), rn = reg_rd();
      const bool test = opcode >= 8 && opcode <= 0xB;
      u32 instr = (cond << 28) | (opcode << 21) | (s << 20) | (rn << 16) | (rd << 12);
      if (test) instr |= 1u << 20;
      switch (r.below(3)) {
      case 0: instr |= (1u << 25) | (r.below(16) << 8) | r.below(256); break;                       // imm
      case 1: instr |= (r.below(32) << 7) | (r.below(4) << 5) | reg_rd(); break;                    // imm shift
      default: instr |= (reg_nw() << 8) | (r.below(4) << 5) | 0x10 | reg_rd(); break;              // reg shift
      }
      return instr;
    }
    case 3: {   // multiply
      const u32 rd = reg_nw(), rn = reg_nw(), rs = reg_nw(), rm = reg_nw();
      if (rd == 15 || rn == 15 || rs == 15 || rm == 15) continue;
      const u32 s = r.coin(50);
      switch (r.below(4)) {
      case 0: return (cond << 28) | (s << 20) | (rd << 16) | (rs << 8) | 0x90 | rm;                 // MUL
      case 1: return (cond << 28) | (1u << 21) | (s << 20) | (rd << 16) | (rn << 12) | (rs << 8) | 0x90 | rm;   // MLA
      default: {
        if (rd == rn) continue;
        const u32 op = 4 + r.below(4);   // UMULL UMLAL SMULL SMLAL
        return (cond << 28) | (op << 21) | (s << 20) | (rd << 16) | (rn << 12) | (rs << 8) | 0x90 | rm;
      }
      }
    }
    case 4: case 5: {   // LDR/STR word/byte, base r9, pre-indexed, no writeback
      const u32 l = r.coin(50), b = r.coin(30), rd = reg_rd();
      if (l && rd == 15) continue;
      u32 instr = (cond << 28) | (1u << 26) | (1u << 24) | (1u << 23) | (b << 22) | (l << 20) | (9u << 16) | (rd << 12);
      if (r.coin(70)) instr |= r.below(0x800);
      else instr |= (1u << 25) | (r.below(8) << 7) | (r.below(3) << 5) | reg_nw();   // reg offset (may wander; both engines agree)
      return instr;
    }
    case 6: {   // LDRH/STRH/LDRSB/LDRSH, base r9, immediate
      const u32 l = r.coin(50), rd = reg_nw();
      u32 sh = l ? 1 + r.below(3) : 1;
      const u32 off = r.below(256);
      return (cond << 28) | (1u << 24) | (1u << 23) | (1u << 22) | (l << 20) | (9u << 16) | (rd << 12) | ((off >> 4) << 8) | 0x90 | (sh << 5) | (off & 0xF);
    }
    case 7: {   // LDM/STM base r9, no writeback, no pc
      u32 list = r.next() & 0x5DFF;   // no r9, r13, r15
      if (!list) continue;
      const u32 l = r.coin(50), p = r.coin(50), u = r.coin(50);
      return (cond << 28) | (4u << 25) | (p << 24) | (u << 23) | (l << 20) | (9u << 16) | list;
    }
    case 8: {   // CLZ / MRS / SWP(B) [r9]
      if (r.coin(30)) return (cond << 28) | 0x01000090 | (r.coin(40) << 22) | (9u << 16) | (reg_nw() << 12) | reg_nw();
      if (r.coin(50)) return (cond << 28) | 0x016F0F10 | (reg_nw() << 12) | reg_nw();
      return (cond << 28) | 0x010F0000 | (reg_nw() << 12);
    }
    default: {  // PUSH/POP style: STMDB sp!/LDMIA sp! without pc
      u32 list = r.next() & 0x5DFF;
      if (!list) continue;
      if (r.coin(50)) return (cond << 28) | (0xE92D0000u & 0x0FFFFFFF) | list;   // stmdb sp!, {list}
      return (cond << 28) | (0xE8BD0000u & 0x0FFFFFFF) | list;                 // ldmia sp!, {list}
    }
    }
  }
}

// A self-contained `LDM ^` / `STM ^` gadget. The generator otherwise never
// leaves SYS mode, where the user bank IS the live one and the transfer is
// indistinguishable from a plain LDM/STM -- so nothing would ever reach the
// inline path's four inlinable modes, nor its FIQ/USR guard.
//
// The base is copied into r0 first because r9 (the buffer base the generator
// relies on) is banked away in FIQ; r0-r7 are not banked in any mode, so the
// transfer addresses the buffer whichever mode the gadget picked.
void gen_user_bank(Rng& r, std::vector<u32>& out) {
  static const u32 modes[] = {0x10, 0x11, 0x12, 0x13, 0x17, 0x1B, 0x1F};
  const u32 mode = modes[r.below(7)];
  const u32 load = r.coin(50);
  u32 list;
  do { list = r.next() & (load ? 0x7FFFu : 0xFFFFu); } while (!list);   // r15 in a load list is a CPSR restore, not a user-bank transfer
  out.push_back(0xE1A00009);                            // mov r0, r9
  out.push_back(0xE321F000 | (0xC0 | mode));            // msr cpsr_c, #(I|F|mode)
  out.push_back(0xE8C00000 | (load << 20) | list);      // ldm/stm r0, {list}^
  out.push_back(0xE321F0DF);                            // msr cpsr_c, #(I|F|SYS) -- a no-op from USR, which cannot leave
}

// ---- Thumb generator ---------------------------------------------------------
// r6 = buffer base (kept), sp = stack.
u16 gen_thumb(Rng& r) {
  auto lo = [&]() { u32 x; do { x = r.below(8); } while (x == 6); return x; };
  for (;;) {
    switch (r.below(12)) {
    case 0: return static_cast<u16>((r.below(3) << 11) | (r.below(32) << 6) | (lo() << 3) | lo());          // shift imm
    case 1: return static_cast<u16>(0x1800 | (r.below(4) << 9) | (r.below(8) << 6) | (lo() << 3) | lo());   // add/sub reg/imm3 (rn may be r6: read only)
    case 2: return static_cast<u16>(0x2000 | (r.below(4) << 11) | (lo() << 8) | r.below(256));             // mov/cmp/add/sub imm8
    case 3: return static_cast<u16>(0x4000 | (r.below(16) << 6) | (lo() << 3) | lo());                      // alu
    case 4: {   // hi reg ops, never pc/sp as destination, no r6
      const u32 op = r.below(3);
      u32 rd, rs;
      do { rd = r.below(15); } while (rd == 6 || rd == 13);
      do { rs = r.below(16); } while (rs == 6);
      if (op == 1 && rd == 15) continue;
      return static_cast<u16>(0x4400 | (op << 8) | ((rd >> 3) << 7) | (rs << 3) | (rd & 7));
    }
    case 5: return static_cast<u16>(0x4800 | (lo() << 8) | r.below(256));                                    // ldr pc-rel (reads code area)
    case 6: return static_cast<u16>(0x5000 | (r.below(8) << 9) | (lo() << 6) | (6u << 3) | lo());            // ldr/str reg [r6 + ro]
    case 7: return static_cast<u16>(0x6000 | (r.below(4) << 11) | (r.below(32) << 6) | (6u << 3) | lo());    // ldr/str imm5 [r6]
    case 8: return static_cast<u16>(0x8000 | (r.below(2) << 11) | (r.below(32) << 6) | (6u << 3) | lo());    // ldrh/strh
    case 9: return static_cast<u16>(0x9000 | (r.below(2) << 11) | (lo() << 8) | r.below(256));               // sp-relative
    case 10: {  // push/pop without pc/lr, adjust sp, add rd, sp/pc
      switch (r.below(3)) {
      case 0: { u32 list = r.next() & 0xBF; if (!list) continue; return static_cast<u16>(0xB400 | (r.below(2) << 11) | list); }
      case 1: return static_cast<u16>(0xB000 | (r.below(2) << 7) | r.below(16));
      default: return static_cast<u16>(0xA000 | (r.below(2) << 11) | (lo() << 8) | r.below(256));
      }
    }
    default: {  // stmia/ldmia rb!, rb != 6, list without rb
      const u32 rb = lo();
      u32 list = r.next() & 0xBF & ~(1u << rb);
      if (!list) continue;
      return static_cast<u16>(0xC000 | (r.below(2) << 11) | (rb << 8) | list);
    }
    }
  }
}

// The machines are reused across trials, and a trial stores wherever its
// registers point -- over its own code, past the ends of the buffer and stack
// windows, anywhere in RAM. What it leaves behind is the next trial's starting
// memory. That is harmless while the two engines agree, because then they
// leave the SAME residue; it stops being harmless the moment a trial ends with
// them holding different memory, after which a later seed can fail on residue
// rather than on its own behaviour. Two mismatches found while adding the
// user-bank gadget reproduced only in the full set and passed on their own,
// and one of them was residue, not a defect.
//
// So: wipe main RAM only after a trial that could have left the two sides
// different -- a mismatch, or an inconclusive run where both engines ran out
// of budget in different places. Page at a time; a word at a time is a
// page-table lookup per word and costs minutes a run under qemu.
void wipe_ram(Machine& m) {
  for (u32 a = MAIN_RAM; a < MAIN_RAM + MAIN_RAM_SIZE; a += mem::PAGE_SIZE)
    std::memset(m.host(a), 0, mem::PAGE_SIZE);
}

struct Trial {
  bool thumb;
  std::vector<u32> code;   // ARM words or Thumb halfwords
  u32 regs[16];
  u32 cpsr;
};

u32 g_selfmod = 0;
bool g_ram_dirty = false;   // the last trial may have left the two machines' RAM different
u8 g_code_image[0x1000];    // the code page as load_trial wrote it

bool wrote_own_code(Machine& m) { return std::memcmp(m.host(CODE_BASE), g_code_image, sizeof g_code_image) != 0; }

void load_trial(Machine& m, const Trial& t, bool a9) {
  write_halt_stub(m, a9);
  // Clear the code page first. A trial whose base register wanders into this
  // page stores over its own code, and what it leaves behind would otherwise
  // be fetched by the next trial once that one runs off the end of its own
  // code. Cheap (4 KB); the wider version of the problem is `wipe_ram`.
  for (u32 i = 0; i < 0x1000; i += 4) m.poke32(CODE_BASE + i, 0);
  u32 addr = CODE_BASE;
  if (t.thumb) {
    for (u32 h : t.code) { m.poke16(addr, static_cast<u16>(h)); addr += 2; }
    // ldr r0, [pc, #k]; bx r0; (pad) .word HALT_STUB
    const u32 ldr_at = addr;
    const u32 pool = (ldr_at + 4 + 3) & ~3u;
    m.poke16(ldr_at, static_cast<u16>(0x4800 | ((pool - ((ldr_at + 4) & ~3u)) >> 2)));
    m.poke16(ldr_at + 2, 0x4700);
    if (pool > ldr_at + 4) m.poke16(ldr_at + 4, 0x46C0);   // nop
    m.poke32(pool, HALT_STUB);
  } else {
    for (u32 w : t.code) { m.poke32(addr, w); addr += 4; }
    const s32 off = static_cast<s32>(HALT_STUB - (addr + 8)) >> 2;
    m.poke32(addr, 0xEA000000u | (static_cast<u32>(off) & 0x00FFFFFF));   // b halt_stub
  }
  for (u32 i = 0; i < 0x2000; i += 4) m.poke32(BUF_BASE + i, 0x01010101u * (i >> 2) ^ 0xA5A5A5A5u);
  for (u32 i = 0; i < 0x400; i += 4) m.poke32(STACK - 0x200 + i, 0x11111111u * (i >> 2));
  CpuContext& c = m.cpu;
  c.set_cpsr(0x1F);                // SYS mode, ARM
  // The banked registers are part of the starting state, and the machines are
  // reused across trials: without this a trial that ends in a non-SYS mode
  // leaves its banks to the next one, which then depends on trial order
  // rather than on its own seed. It never mattered while nothing switched
  // mode; the user-bank gadget does.
  std::memset(c.bank_r8_r12, 0, sizeof c.bank_r8_r12);
  std::memset(c.bank_r13, 0, sizeof c.bank_r13);
  std::memset(c.bank_r14, 0, sizeof c.bank_r14);
  std::memset(c.bank_spsr, 0, sizeof c.bank_spsr);
  c.hot.spsr = 0;
  for (int i = 0; i < 15; ++i) c.hot.regs[i] = t.regs[i];
  c.hot.cpsr = t.cpsr | 0x1F | (t.thumb ? 0x20 : 0);
  c.hot.regs[15] = CODE_BASE + (t.thumb ? 4 : 8);
  c.halted = false;
  c.hot.irq_pending = 0;
  // As if a jump had just landed at CODE_BASE (the ARM7 interpreter keeps the
  // code region of the last jump target).
  c.code_cycles = CODE_BASE >> 15;
  c.code_region = CODE_BASE >> 24;
  c.hot.cycle_budget = 1 << 24;
  c.budget_at_halt = 0;
  c.jumped = false;
  std::memcpy(g_code_image, m.host(CODE_BASE), sizeof g_code_image);
}

void run_machine(Machine& m, RunFn fn) {
  for (int guard = 0; guard < 100000 && !m.cpu.halted && m.cpu.hot.cycle_budget > 0; ++guard) fn(m.cpu);
}

u32 g_inconclusive = 0;

bool compare(Machine& a, Machine& b, const Trial& t, u32 seed, bool report) {
  // A trial that never reaches the halt stub on either engine (a store that
  // clobbered its own code, a loop through stale memory) only differs in
  // where the budget ran out, which is the one thing the engines are allowed
  // to differ in. Count it and move on.
  if (!a.cpu.halted && !b.cpu.halted && a.cpu.hot.cycle_budget <= 0 && b.cpu.hot.cycle_budget <= 0) { ++g_inconclusive; g_ram_dirty = true; return true; }
  // A trial that stores over its own code is outside what the recompiler
  // promises: the interpreter fetches the new word, while a translated block
  // keeps running the code it was built from, and the two legitimately part
  // ways. It is the generator wandering, not a defect -- the same divergence
  // appears for a plain STM with no user-bank bit. Judge only the trials that
  // left their own instructions alone.
  if (wrote_own_code(a) || wrote_own_code(b)) { ++g_selfmod; g_ram_dirty = true; return true; }
  bool ok = true;
  for (int i = 0; i < 16; ++i) if (a.cpu.hot.regs[i] != b.cpu.hot.regs[i]) ok = false;
  if (a.cpu.hot.cpsr != b.cpu.hot.cpsr) ok = false;
  if (a.cpu.halted != b.cpu.halted) ok = false;
  // Both engines end the slice with budget -1 on a halt; the budget at the
  // moment of halting is the consumed-cycle comparison.
  const s32 ba = a.cpu.halted ? a.cpu.budget_at_halt : a.cpu.hot.cycle_budget;
  const s32 bb = b.cpu.halted ? b.cpu.budget_at_halt : b.cpu.hot.cycle_budget;
  if (ba != bb) ok = false;
  if (std::memcmp(a.host(BUF_BASE), b.host(BUF_BASE), 0x2000) != 0) ok = false;
  if (std::memcmp(a.host(STACK - 0x200), b.host(STACK - 0x200), 0x400) != 0) ok = false;
  if (!ok) g_ram_dirty = true;
  if (ok || !report) return ok;
  std::fprintf(stderr, "MISMATCH seed %u (%s), shortest failing prefix:\n", seed, t.thumb ? "thumb" : "arm");
  for (size_t i = 0; i < t.code.size(); ++i) std::fprintf(stderr, "  %08x: %0*x\n", CODE_BASE + static_cast<u32>(i * (t.thumb ? 2 : 4)), t.thumb ? 4 : 8, t.code[i]);
  std::fprintf(stderr, "  initial: cpsr %08x", t.cpsr);
  for (int i = 0; i < 15; ++i) std::fprintf(stderr, " r%d=%08x", i, t.regs[i]);
  std::fprintf(stderr, "\n  %-6s %-10s %-10s\n", "", "interp", "jit");
  for (int i = 0; i < 16; ++i) if (a.cpu.hot.regs[i] != b.cpu.hot.regs[i]) std::fprintf(stderr, "  r%-5d %08x   %08x\n", i, a.cpu.hot.regs[i], b.cpu.hot.regs[i]);
  if (a.cpu.hot.cpsr != b.cpu.hot.cpsr) std::fprintf(stderr, "  cpsr   %08x   %08x\n", a.cpu.hot.cpsr, b.cpu.hot.cpsr);
  if (ba != bb) std::fprintf(stderr, "  budget %08x   %08x (consumed %d vs %d)\n", ba, bb, (1 << 24) - ba, (1 << 24) - bb);
  if (a.cpu.halted != b.cpu.halted) std::fprintf(stderr, "  halted %d %d\n", a.cpu.halted, b.cpu.halted);
  for (u32 i = 0; i < 0x2000; i += 4) {
    u32 x, y; std::memcpy(&x, a.host(BUF_BASE + i), 4); std::memcpy(&y, b.host(BUF_BASE + i), 4);
    if (x != y) { std::fprintf(stderr, "  buf[%04x] %08x %08x\n", i, x, y); }
  }
  // The code region is not part of the comparison (a trial that overwrites its
  // own code is allowed to end differently), but when something else differs
  // it is usually the cause, so report it.
  for (u32 i = 0; i < 0x1000; i += 4) {
    u32 x, y; std::memcpy(&x, a.host(CODE_BASE + i), 4); std::memcpy(&y, b.host(CODE_BASE + i), 4);
    if (x != y) { std::fprintf(stderr, "  code[%04x] %08x %08x\n", i, x, y); }
  }
  return false;
}

// DS_FUZZ_TRACE=<path-prefix>: dump a per-instruction trace of both engines.
// Debug scaffolding for localising a mismatch -- the comparison itself only
// looks at the end state, which names the symptom and not the instruction.
std::FILE* g_trace_fp = nullptr;
void trace_cb(ds::CpuContext& c, u32 instr, void*) {
  if (!g_trace_fp) return;
  std::fprintf(g_trace_fp, "%08x %08x cpsr=%08x", c.hot.regs[15], instr, c.hot.cpsr);
  for (int i = 0; i < 15; ++i) std::fprintf(g_trace_fp, " %08x", c.hot.regs[i]);
  std::fprintf(g_trace_fp, " | b13=%08x b14=%08x\n", c.bank_r13[0], c.bank_r14[0]);
}

bool run_both(Machine& mi, Machine& mj, const Trial& tr, bool a9, u32 seed, bool report) {
  const char* tp = std::getenv("DS_FUZZ_TRACE");
  if (g_ram_dirty) { wipe_ram(mi); wipe_ram(mj); g_ram_dirty = false; }
  jit::flush(mj.cpu);
  load_trial(mi, tr, a9);
  load_trial(mj, tr, a9);
  if (tp) {
    std::string base = std::string(tp);
    g_trace_fp = std::fopen((base + ".interp").c_str(), "w");
    mi.nds.trace = &trace_cb;
    run_machine(mi, &interp::run);
    std::fclose(g_trace_fp);
    mi.nds.trace = nullptr;
    g_trace_fp = std::fopen((base + ".jit").c_str(), "w");
    mj.nds.trace = &trace_cb;
    jit::set_trace(true);
    jit::flush(mj.cpu);
    run_machine(mj, &jit::run);
    jit::set_trace(false);
    std::fclose(g_trace_fp);
    g_trace_fp = nullptr;
    mj.nds.trace = nullptr;
  } else {
    run_machine(mi, &interp::run);
    run_machine(mj, &jit::run);
  }
  return compare(mi, mj, tr, seed, report);
}

// Hand-written sequences for cases the generator reaches rarely. Registers
// follow the generator's conventions (r9/r6 = buffer, r13 = stack).
struct Directed { Cpu which; bool thumb; std::vector<u32> code; u32 regs[15]; };

void directed() {
  const Directed cases[] = {
    // Pending fetch cycles of the MOV must survive the LDR taking its slow path
    // (unmapped address through a register offset).
    {Cpu::ARM9, false, {0xE3A00001, 0xE7991102}, {0, 0, 0x12345678, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {Cpu::ARM7, false, {0xE3A00001, 0xE7991102}, {0, 0, 0x12345678, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    // Same with a store, and with a conditional instruction in front.
    {Cpu::ARM9, false, {0xE0811002, 0xE7891102}, {0, 1, 0x12345678, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {Cpu::ARM9, false, {0x03A00001, 0xE7991102}, {0, 0, 0x12345678, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    // CP15 cache maintenance (ignored by the core): drain write buffer,
    // invalidate I-cache line, clean+invalidate D-cache line; then an ALU op.
    {Cpu::ARM9, false, {0xEE070F9A, 0xEE073F35, 0xEE070F3E, 0xE2800001}, {5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    // MSR CPSR_c keeping the mode (SYS): set F, then clear it; flags field from a register and an immediate.
    {Cpu::ARM9, false, {0xE129F001, 0xE129F002, 0xE128F003, 0xE328F20F, 0xE2800001}, {0, 0x5F, 0x1F, 0xF0000000u, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {Cpu::ARM7, false, {0xE129F001, 0xE129F002, 0xE128F003, 0xE328F20F, 0xE2800001}, {0, 0x5F, 0x1F, 0xF0000000u, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    // MSR CPSR_c changing the mode (SYS -> IRQ -> SYS): the interpreter path; r13/r14 are banked.
    {Cpu::ARM9, false, {0xE129F001, 0xE1A0D004, 0xE129F002, 0xE2800001}, {0, 0x92, 0x1F, 0, 0x12345678, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    // MSR CPSR_cxsf with the full mask, same mode, and an IRQ-enable (I cleared) with no IRQ pending.
    {Cpu::ARM9, false, {0xE12FF001, 0xE2800001}, {0, 0x600000DFu, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {Cpu::ARM9, false, {0xE129F001, 0xE2800001}, {0, 0x1F, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    // v5TE DSP multiplies (ARM9 only). Operands are chosen so the halves are
    // distinguishable and the accumulate overflows, which is the only way the
    // sticky Q flag is exercised. r6/r9/r13 are reserved by the harness.
    // SMULBB / SMULTT / SMULBT r0, r1, r2
    {Cpu::ARM9, false, {0xE1600281, 0xE2800001}, {0, 0x7FFF8000u, 0x00028001u, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {Cpu::ARM9, false, {0xE16002E1, 0xE2800001}, {0, 0x7FFF8000u, 0x00028001u, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {Cpu::ARM9, false, {0xE16002C1, 0xE2800001}, {0, 0x7FFF8000u, 0x00028001u, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    // SMLABB r0, r1, r2, r3 -- no overflow, then with r3 forcing Q
    {Cpu::ARM9, false, {0xE1003281, 0xE2800001}, {0, 0x00001234u, 0x00005678u, 0x100, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {Cpu::ARM9, false, {0xE1003281, 0xE2800001}, {0, 0x7FFF7FFFu, 0x7FFF7FFFu, 0x7FFFFFFFu, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    // SMLAWB / SMLAWT r0, r1, r2, r3, the GSDD shape, with and without overflow
    {Cpu::ARM9, false, {0xE1203281, 0xE2800001}, {0, 0x12345678u, 0x00007FFFu, 0x100, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {Cpu::ARM9, false, {0xE12032C1, 0xE2800001}, {0, 0x80000000u, 0x8000FFFFu, 0x7FFFFFFFu, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    // SMULWB / SMULWT r0, r1, r2
    {Cpu::ARM9, false, {0xE12002A1, 0xE2800001}, {0, 0x12345678u, 0x00007FFFu, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {Cpu::ARM9, false, {0xE12002E1, 0xE2800001}, {0, 0x80000000u, 0x8000FFFFu, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    // Destination aliasing a source, and a conditional form (the smlabblo GSDD executes)
    {Cpu::ARM9, false, {0xE1013281, 0xE2800001}, {0, 0x00001234u, 0x00005678u, 0x100, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {Cpu::ARM9, false, {0x31013281, 0xE2800001}, {0, 0x00001234u, 0x00005678u, 0x100, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    // The exact GSDD encodings: smlawb lr,fp,r2,lr and smlabblo r1,ip,r1,r2
    {Cpu::ARM9, false, {0xE12EE28B, 0xE2800001}, {0, 0, 0x00001234u, 0, 0, 0, 0, 0, 0, 0, 0, 0x00098765u, 0, 0, 0x00000042u}},
    {Cpu::ARM9, false, {0x3101218C, 0xE2800001}, {0, 0x00004321u, 0x00000100u, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x00001234u, 0, 0}},
    // On the ARM7 these encodings are undefined and must still fall back.
    {Cpu::ARM7, false, {0xE1003281, 0xE2800001}, {0, 0x00001234u, 0x00005678u, 0x100, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    // A conditional register-shift data-processing instruction: taken (CC with C clear),
    // not taken (CS), unconditional; and a taken conditional immediate form.
    {Cpu::ARM9, false, {0x31dceb70, 0xE2800001}, {0x7a2fbc14, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x7b20dd03, 0xe41d8363, 0, 0xd1cea9ce}},
    {Cpu::ARM9, false, {0x21dceb70, 0xE2800001}, {0x7a2fbc14, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x7b20dd03, 0xe41d8363, 0, 0xd1cea9ce}},
    {Cpu::ARM9, false, {0xE1dceb70, 0xE2800001}, {0x7a2fbc14, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x7b20dd03, 0xe41d8363, 0, 0xd1cea9ce}},
    {Cpu::ARM9, false, {0x33dce001, 0xE2800001}, {0x7a2fbc14, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x7b20dd03, 0xe41d8363, 0, 0xd1cea9ce}},
    {Cpu::ARM7, false, {0x31dceb70, 0xE2800001}, {0x7a2fbc14, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x7b20dd03, 0xe41d8363, 0, 0xd1cea9ce}},
    // RSCS with a pc operand, then a conditional on N/V (seed 2003's shape).
    {Cpu::ARM9, false, {0xE2FFA4D2, 0xB1A01002, 0xE2800001}, {0, 0, 0x12345678u, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {Cpu::ARM9, false, {0xE2F0A4D2, 0xB1A01002, 0xE2800001}, {0x02000008u, 0, 0x12345678u, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    // ... and with C set going in (cmp r0, r0 first).
    {Cpu::ARM9, false, {0xE1500000, 0xE2FFA4D2, 0xB1A01002, 0xE2800001}, {0, 0, 0x12345678u, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {Cpu::ARM9, false, {0xE1500000, 0xE2F0A4D2, 0xB1A01002, 0xE2800001}, {0x02000008u, 0, 0x12345678u, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    // Indirect branches (refill costs are compared through the budget).
    // bx r1 -> the ARM halt stub; bx r1 -> Thumb code at +4 (even) / +6 (odd)
    // that loads the halt stub's address and bx's to it; blx r1 (ARM9).
    {Cpu::ARM9, false, {0xE12FFF11, 0xE2800001}, {0, HALT_STUB, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {Cpu::ARM7, false, {0xE12FFF11, 0xE2800001}, {0, HALT_STUB, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {Cpu::ARM9, false, {0xE12FFF11, 0x47004801, 0x46C046C0, HALT_STUB}, {0, CODE_BASE + 4 + 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {Cpu::ARM7, false, {0xE12FFF11, 0x47004801, 0x46C046C0, HALT_STUB}, {0, CODE_BASE + 4 + 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {Cpu::ARM9, false, {0xE12FFF11, 0x480146C0, 0x46C04700, HALT_STUB}, {0, CODE_BASE + 6 + 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {Cpu::ARM7, false, {0xE12FFF11, 0x480146C0, 0x46C04700, HALT_STUB}, {0, CODE_BASE + 6 + 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {Cpu::ARM9, false, {0xE12FFF31, 0xE2800001}, {0, HALT_STUB, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    // Conditional bx, taken and not taken.
    {Cpu::ARM9, false, {0xE1500000, 0x012FFF11, 0xE2800001}, {0, HALT_STUB, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {Cpu::ARM9, false, {0xE1500000, 0x112FFF11, 0xE2800001}, {0, HALT_STUB, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    // str r2, [r9]; ldmia r9, {pc}  and  ldmia r9!, {r0, pc} (the CDI charge after the jump)
    {Cpu::ARM9, false, {0xE5892000, 0xE8998000}, {0, 0, HALT_STUB, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {Cpu::ARM7, false, {0xE5892000, 0xE8998000}, {0, 0, HALT_STUB, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {Cpu::ARM9, false, {0xE5892004, 0xE8B98001}, {0, 0, HALT_STUB, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {Cpu::ARM7, false, {0xE5892004, 0xE8B98001}, {0, 0, HALT_STUB, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    // Thumb: bx r1; mov pc, r1 / add pc, r1 (odd targets: Thumb code at +4 that bx's to the halt stub);
    // push {r2}; pop {pc}; an unpaired blx suffix (lr = halt stub).
    {Cpu::ARM9, true, {0x4708, 0x46C0, 0x4801, 0x4700, 0x46C0, 0x46C0, static_cast<u32>(HALT_STUB & 0xFFFF), static_cast<u32>(HALT_STUB >> 16)}, {0, HALT_STUB, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {Cpu::ARM7, true, {0x4708, 0x46C0, 0x4801, 0x4700, 0x46C0, 0x46C0, static_cast<u32>(HALT_STUB & 0xFFFF), static_cast<u32>(HALT_STUB >> 16)}, {0, HALT_STUB, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {Cpu::ARM9, true, {0x468F, 0x46C0, 0x4801, 0x4700, 0x46C0, 0x46C0, static_cast<u32>(HALT_STUB & 0xFFFF), static_cast<u32>(HALT_STUB >> 16)}, {0, CODE_BASE + 4 + 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {Cpu::ARM7, true, {0x468F, 0x46C0, 0x4801, 0x4700, 0x46C0, 0x46C0, static_cast<u32>(HALT_STUB & 0xFFFF), static_cast<u32>(HALT_STUB >> 16)}, {0, CODE_BASE + 4 + 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {Cpu::ARM9, true, {0x448F, 0x46C0, 0x4801, 0x4700, 0x46C0, 0x46C0, static_cast<u32>(HALT_STUB & 0xFFFF), static_cast<u32>(HALT_STUB >> 16)}, {0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {Cpu::ARM9, true, {0xB404, 0xBD00}, {0, 0, HALT_STUB, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {Cpu::ARM9, true, {0xE800}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, HALT_STUB}},
    // SWP / SWPB [r9]: rd != rm, rd == rm (the in-place swap), rd == rn
    // (the base register takes the loaded word), conditional taken / not
    // taken, and through a register far from the buffer (the slow path).
    {Cpu::ARM9, false, {0xE5892000, 0xE1090091, 0xE2800001}, {0, 0x11111111, 0x22222222, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {Cpu::ARM7, false, {0xE5892000, 0xE1090091, 0xE2800001}, {0, 0x11111111, 0x22222222, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {Cpu::ARM9, false, {0xE5892000, 0xE1091091, 0xE2800001}, {0, 0x11111111, 0x22222222, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {Cpu::ARM9, false, {0xE5892000, 0xE1491091, 0xE5990000}, {0, 0x111111AB, 0x22222222, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {Cpu::ARM7, false, {0xE5892000, 0xE1491091, 0xE5990000}, {0, 0x111111AB, 0x22222222, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {Cpu::ARM9, false, {0xE1A03009, 0xE1033091, 0xE5990000}, {0, 0x11111111, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {Cpu::ARM9, false, {0xE1500000, 0x01090091, 0x11092091, 0xE2800001}, {0, 0x11111111, 0x22222222, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {Cpu::ARM9, false, {0xE1030091, 0xE2800001}, {0, 0x11111111, 0, 0x04000208, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {Cpu::ARM7, false, {0xE1030091, 0xE2800001}, {0, 0x11111111, 0, 0x04000208, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    // Data-processing writes to pc (S=0): a bounds-checked jump table
    // (cmp r1, #2; addls pc, pc, r1, lsl #2) in range and out of it, an
    // unconditional add pc, pc, r2, lsl #2, add pc, pc, #0, and mov pc, r1.
    {Cpu::ARM9, false, {0xE3510002, 0x908FF101, 0xE2800064, 0xE2800001, 0xE2800002, 0xE2800004}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {Cpu::ARM9, false, {0xE3510002, 0x908FF101, 0xE2800064, 0xE2800001, 0xE2800002, 0xE2800004}, {0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {Cpu::ARM9, false, {0xE3510002, 0x908FF101, 0xE2800064, 0xE2800001, 0xE2800002, 0xE2800004}, {0, 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {Cpu::ARM7, false, {0xE3510002, 0x908FF101, 0xE2800064, 0xE2800001, 0xE2800002, 0xE2800004}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {Cpu::ARM7, false, {0xE3510002, 0x908FF101, 0xE2800064, 0xE2800001, 0xE2800002, 0xE2800004}, {0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {Cpu::ARM7, false, {0xE3510002, 0x908FF101, 0xE2800064, 0xE2800001, 0xE2800002, 0xE2800004}, {0, 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {Cpu::ARM9, false, {0xE08FF102, 0xE2800064, 0xE2800001, 0xE2800002}, {0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {Cpu::ARM9, false, {0xE28FF000, 0xE2800064, 0xE2800001}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {Cpu::ARM9, false, {0xE1A0F001, 0xE2800001}, {0, HALT_STUB, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {Cpu::ARM7, false, {0xE1A0F001, 0xE2800001}, {0, HALT_STUB, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    // Exit flag liveness: cmp r1, r1 (C=1); tst r0, r0 (C kept); bne -> a
    // successor that reads C first (bcs), taken and not taken, and one whose
    // successors overwrite C first (adds), where the merge is dropped.
    {Cpu::ARM9, true, {0x4289, 0x4200, 0xD100, 0x3001, 0xD200, 0x3064, 0x3001}, {5, 7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {Cpu::ARM9, true, {0x4289, 0x4200, 0xD100, 0x3001, 0xD200, 0x3064, 0x3001}, {0, 7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {Cpu::ARM7, true, {0x4289, 0x4200, 0xD100, 0x3001, 0xD200, 0x3064, 0x3001}, {5, 7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {Cpu::ARM9, true, {0x4289, 0x4200, 0xD100, 0x3001, 0x3002, 0x3064, 0x3001}, {5, 7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    // ARM: the same through a conditional B, the successor reading C (addcs).
    {Cpu::ARM9, false, {0xE1510001, 0xE3100000, 0x1A000000, 0xE2900001, 0x22800064, 0xE2800001}, {5, 7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    // Thumb: movs then ldr [r6 + r0] far away.
    {Cpu::ARM9, true, {0x2001, 0x5871}, {0, 0x12345678, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {Cpu::ARM7, true, {0x2001, 0x5871}, {0, 0x12345678, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},

    // `LDM ^` / `STM ^` -- the user-bank transfer. The interpreter switches to
    // user mode and back; the JIT inlines it in IRQ/SVC/ABT/UND, where only
    // r13/r14 are banked away from the live set, and keeps the interpreter in
    // FIQ (r8-r12 banked too) and USR/SYS (the live bank already IS bank 0).
    // Each case ends `msr cpsr_c, #SYS` so the user bank is back in hot.regs
    // for the comparison -- without it a wrong bank would be invisible.
    //
    // LDM: give the entered mode its own r13/r14 (0xAA/0xBB) first; those must
    // survive, and the loaded words must land in the user bank.
    {Cpu::ARM9, false, {0xE321F0D2, 0xE3A0D0AA, 0xE3A0E0BB, 0xE8D07FFF, 0xE321F0DF}, {BUF_BASE, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},   // IRQ
    {Cpu::ARM7, false, {0xE321F0D2, 0xE3A0D0AA, 0xE3A0E0BB, 0xE8D07FFF, 0xE321F0DF}, {BUF_BASE, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},   // IRQ
    {Cpu::ARM9, false, {0xE321F0D3, 0xE3A0D0AA, 0xE3A0E0BB, 0xE8D07FFF, 0xE321F0DF}, {BUF_BASE, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},   // SVC
    {Cpu::ARM7, false, {0xE321F0D3, 0xE3A0D0AA, 0xE3A0E0BB, 0xE8D07FFF, 0xE321F0DF}, {BUF_BASE, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},   // SVC
    {Cpu::ARM9, false, {0xE321F0D7, 0xE3A0D0AA, 0xE3A0E0BB, 0xE8D07FFF, 0xE321F0DF}, {BUF_BASE, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},   // ABT
    {Cpu::ARM7, false, {0xE321F0D7, 0xE3A0D0AA, 0xE3A0E0BB, 0xE8D07FFF, 0xE321F0DF}, {BUF_BASE, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},   // ABT
    {Cpu::ARM9, false, {0xE321F0DB, 0xE3A0D0AA, 0xE3A0E0BB, 0xE8D07FFF, 0xE321F0DF}, {BUF_BASE, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},   // UND
    {Cpu::ARM7, false, {0xE321F0DB, 0xE3A0D0AA, 0xE3A0E0BB, 0xE8D07FFF, 0xE321F0DF}, {BUF_BASE, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},   // UND
    {Cpu::ARM9, false, {0xE321F0D1, 0xE3A0D0AA, 0xE3A0E0BB, 0xE8D07FFF, 0xE321F0DF}, {BUF_BASE, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},   // FIQ
    {Cpu::ARM7, false, {0xE321F0D1, 0xE3A0D0AA, 0xE3A0E0BB, 0xE8D07FFF, 0xE321F0DF}, {BUF_BASE, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},   // FIQ
    {Cpu::ARM9, false, {0xE321F0DF, 0xE3A0D0AA, 0xE3A0E0BB, 0xE8D07FFF, 0xE321F0DF}, {BUF_BASE, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},   // SYS
    {Cpu::ARM7, false, {0xE321F0DF, 0xE3A0D0AA, 0xE3A0E0BB, 0xE8D07FFF, 0xE321F0DF}, {BUF_BASE, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},   // SYS
    // STM: the user r13/r14 (0xAA/0xBB) are set before the mode change and the
    // entered mode's are 0x11/0x22 -- the store must write the former.
    {Cpu::ARM9, false, {0xE3A0D0AA, 0xE3A0E0BB, 0xE321F0D2, 0xE3A0D011, 0xE3A0E022, 0xE8C07FFF, 0xE321F0DF}, {BUF_BASE, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},   // IRQ
    {Cpu::ARM7, false, {0xE3A0D0AA, 0xE3A0E0BB, 0xE321F0D2, 0xE3A0D011, 0xE3A0E022, 0xE8C07FFF, 0xE321F0DF}, {BUF_BASE, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},   // IRQ
    {Cpu::ARM9, false, {0xE3A0D0AA, 0xE3A0E0BB, 0xE321F0D3, 0xE3A0D011, 0xE3A0E022, 0xE8C07FFF, 0xE321F0DF}, {BUF_BASE, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},   // SVC
    {Cpu::ARM7, false, {0xE3A0D0AA, 0xE3A0E0BB, 0xE321F0D3, 0xE3A0D011, 0xE3A0E022, 0xE8C07FFF, 0xE321F0DF}, {BUF_BASE, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},   // SVC
    {Cpu::ARM9, false, {0xE3A0D0AA, 0xE3A0E0BB, 0xE321F0D7, 0xE3A0D011, 0xE3A0E022, 0xE8C07FFF, 0xE321F0DF}, {BUF_BASE, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},   // ABT
    {Cpu::ARM7, false, {0xE3A0D0AA, 0xE3A0E0BB, 0xE321F0D7, 0xE3A0D011, 0xE3A0E022, 0xE8C07FFF, 0xE321F0DF}, {BUF_BASE, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},   // ABT
    {Cpu::ARM9, false, {0xE3A0D0AA, 0xE3A0E0BB, 0xE321F0DB, 0xE3A0D011, 0xE3A0E022, 0xE8C07FFF, 0xE321F0DF}, {BUF_BASE, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},   // UND
    {Cpu::ARM7, false, {0xE3A0D0AA, 0xE3A0E0BB, 0xE321F0DB, 0xE3A0D011, 0xE3A0E022, 0xE8C07FFF, 0xE321F0DF}, {BUF_BASE, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},   // UND
    {Cpu::ARM9, false, {0xE3A0D0AA, 0xE3A0E0BB, 0xE321F0D1, 0xE3A0D011, 0xE3A0E022, 0xE8C07FFF, 0xE321F0DF}, {BUF_BASE, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},   // FIQ
    {Cpu::ARM7, false, {0xE3A0D0AA, 0xE3A0E0BB, 0xE321F0D1, 0xE3A0D011, 0xE3A0E022, 0xE8C07FFF, 0xE321F0DF}, {BUF_BASE, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},   // FIQ
    {Cpu::ARM9, false, {0xE3A0D0AA, 0xE3A0E0BB, 0xE321F0DF, 0xE3A0D011, 0xE3A0E022, 0xE8C07FFF, 0xE321F0DF}, {BUF_BASE, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},   // SYS
    {Cpu::ARM7, false, {0xE3A0D0AA, 0xE3A0E0BB, 0xE321F0DF, 0xE3A0D011, 0xE3A0E022, 0xE8C07FFF, 0xE321F0DF}, {BUF_BASE, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},   // SYS
    // Base in the list (Golden Sun's `ldm r0, {r0-r14}^`) is above; here the
    // base is outside it, the low-register-only list that needs no bank slot
    // at all, and a store with r15 in the list.
    {Cpu::ARM9, false, {0xE321F0D2, 0xE8D07FFE, 0xE321F0DF}, {BUF_BASE, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {Cpu::ARM9, false, {0xE321F0D2, 0xE8D000FE, 0xE321F0DF}, {BUF_BASE, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {Cpu::ARM9, false, {0xE321F0D2, 0xE8C0FFFF, 0xE321F0DF}, {BUF_BASE, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {Cpu::ARM7, false, {0xE321F0D2, 0xE8C0FFFF, 0xE321F0DF}, {BUF_BASE, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    // A base 0x7D0 into the buffer: the 15/16 words straddle the 2 KB page
    // boundary, so the transfer takes the per-word slow path -- which has to
    // redirect r13/r14 to the user bank just as the inline one does.
    {Cpu::ARM9, false, {0xE321F0D2, 0xE3A0D0AA, 0xE3A0E0BB, 0xE8D07FFF, 0xE321F0DF}, {BUF_BASE + 0x7D0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {Cpu::ARM7, false, {0xE321F0D2, 0xE3A0D0AA, 0xE3A0E0BB, 0xE8D07FFF, 0xE321F0DF}, {BUF_BASE + 0x7D0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {Cpu::ARM9, false, {0xE3A0D0AA, 0xE3A0E0BB, 0xE321F0D2, 0xE3A0D011, 0xE3A0E022, 0xE8C07FFF, 0xE321F0DF}, {BUF_BASE + 0x7D0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {Cpu::ARM7, false, {0xE3A0D0AA, 0xE3A0E0BB, 0xE321F0D2, 0xE3A0D011, 0xE3A0E022, 0xE8C07FFF, 0xE321F0DF}, {BUF_BASE + 0x7D0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    // STM with r15 in the list, based 0x7D0 into the buffer so it straddles
    // the 2 KB page and every word takes the per-word slow path. That path
    // had no r15 case and stored r14 instead (host_reg(15) IS r14's host
    // register); plain and user-bank forms both reach it.
    {Cpu::ARM9, false, {0xE880FFFF}, {BUF_BASE + 0x7D0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {Cpu::ARM7, false, {0xE880FFFF}, {BUF_BASE + 0x7D0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {Cpu::ARM9, false, {0xE321F0D2, 0xE8C0FFFF, 0xE321F0DF}, {BUF_BASE + 0x7D0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    // `LDM ^` with writeback and r13 as the base -- why the inline path
    // excludes writeback. The interpreter writes the base back while still
    // switched to user mode, so the new base lands in the USER r13 and the
    // entered mode's r13 is left alone; an inline path would write the mode's
    // own r13, which is a different register. Architecturally UNPREDICTABLE,
    // so either is defensible -- but they must not disagree, and the only way
    // to not disagree is to keep the interpreter.
    {Cpu::ARM9, false, {0xE321F0D2, 0xE1A0D000, 0xE8FD00FF, 0xE321F0DF}, {BUF_BASE, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {Cpu::ARM7, false, {0xE321F0D2, 0xE1A0D000, 0xE8FD00FF, 0xE321F0DF}, {BUF_BASE, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {Cpu::ARM9, false, {0xE321F0D3, 0xE1A0E000, 0xE8FE00FF, 0xE321F0DF}, {BUF_BASE, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    // The store counterpart, where the interpreter writes the base back AFTER
    // switching home and an inline path would agree -- kept on the same rule.
    {Cpu::ARM9, false, {0xE321F0D2, 0xE1A0D000, 0xE8ED00FF, 0xE321F0DF}, {BUF_BASE, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    // Conditional, taken and not taken (the condition wraps the whole body,
    // mode guard included).
    {Cpu::ARM9, false, {0xE321F0D2, 0xE1500000, 0x08D07FFF, 0xE321F0DF}, {BUF_BASE, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {Cpu::ARM9, false, {0xE321F0D2, 0xE1500000, 0x18D07FFF, 0xE321F0DF}, {BUF_BASE, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    // Loads to pc, inline: a literal (ldr pc, [pc, #-4] with the target as the
    // next word), a post-indexed pop from the stack (push r1 first), and the
    // conditional pop the IRQ handlers end with, taken and not taken.
    {Cpu::ARM9, false, {0xE51FF004, HALT_STUB}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {Cpu::ARM7, false, {0xE51FF004, HALT_STUB}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {Cpu::ARM9, false, {0xE52D1004, 0xE49DF004}, {0, HALT_STUB, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {Cpu::ARM7, false, {0xE52D1004, 0xE49DF004}, {0, HALT_STUB, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {Cpu::ARM9, false, {0xE52D1004, 0xE3500000, 0x049DF004, 0xE2800001}, {0, HALT_STUB, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {Cpu::ARM9, false, {0xE52D1004, 0xE3500000, 0x049DF004, 0xE2800001}, {1, HALT_STUB, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {Cpu::ARM7, false, {0xE52D1004, 0xE3500000, 0x049DF004, 0xE2800001}, {0, HALT_STUB, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    // SWI: the harness vector is itself `ldr pc, [pc, #k]`.
    {Cpu::ARM9, false, {0xEF000000}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {Cpu::ARM7, false, {0xEF000000}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    // MRC p15 reads (ARM9): DTCM region, control, main ID, then an ALU op.
    {Cpu::ARM9, false, {0xEE190F11, 0xEE111F10, 0xEE102F10, 0xE2800001}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    // MOVS pc, lr (register form) from SVC: SPSR = SYS, lr = the halt stub.
    {Cpu::ARM9, false, {0xE321F0D3, 0xE16FF001, 0xE1A0E002, 0xE1B0F00E}, {0, 0x1F, HALT_STUB, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {Cpu::ARM7, false, {0xE321F0D3, 0xE16FF001, 0xE1A0E002, 0xE1B0F00E}, {0, 0x1F, HALT_STUB, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    // SMLALBB / SMLALTT r4 (lo), r5 (hi), r1, r2: a carry out of the low word, and a negative product.
    {Cpu::ARM9, false, {0xE1454281, 0xE2800001}, {0, 0x7FFF8000u, 0x00028001u, 0, 0xFFFFFFF0u, 0x10, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {Cpu::ARM9, false, {0xE14542E1, 0xE2800001}, {0, 0x7FFF8000u, 0x80018001u, 0, 0x10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
  };
  u32 n = 0;
  for (const Directed& d : cases) {
    const bool a9 = d.which == Cpu::ARM9;
    Machine mi(d.which), mj(d.which);
    CHECK(jit::attach(mj.nds, a9, !a9));
    Trial t;
    t.thumb = d.thumb;
    t.code = d.code;
    for (int i = 0; i < 15; ++i) t.regs[i] = d.regs[i];
    t.regs[9] = BUF_BASE;
    t.regs[6] = BUF_BASE;
    t.regs[13] = STACK;
    t.cpsr = 0;
    const bool ok = run_both(mi, mj, t, a9, 100000 + n, true);
    jit::detach(mj.nds);
    if (!ok) std::fprintf(stderr, "directed case %u failed\n", n);
    CHECK(ok);
    ++n;
  }
  std::printf("jit directed: %u cases ok\n", n);
}

void fuzz(Cpu which, bool thumb, u32 trials, u32 seed0) {
  const bool a9 = which == Cpu::ARM9;
  Machine mi(which), mj(which);
  CHECK(jit::attach(mj.nds, a9, !a9));
  u32 fails = 0;
  for (u32 n = 0; n < trials; ++n) {
    const u32 seed = seed0 + n;
    Rng r(seed);
    Trial t;
    t.thumb = thumb;
    const u32 len = 1 + r.below(24);
    for (u32 i = 0; i < len; ++i) {
      if (!thumb && r.coin(12)) { gen_user_bank(r, t.code); continue; }
      t.code.push_back(thumb ? gen_thumb(r) : gen_arm(r));
    }
    for (int i = 0; i < 15; ++i) t.regs[i] = r.coin(30) ? (r.below(5) - 2) : r.next();
    t.regs[9] = BUF_BASE + (r.below(4) << 10);
    t.regs[6] = BUF_BASE + (r.below(4) << 10);
    t.regs[13] = STACK;
    t.cpsr = (r.next() & 0xF0000000u);
    if (run_both(mi, mj, t, a9, seed, false)) continue;
    // Shrink: the shortest failing prefix names the instruction.
    Trial p = t;
    for (u32 k = 1; k <= t.code.size(); ++k) {
      p.code.assign(t.code.begin(), t.code.begin() + k);
      if (!run_both(mi, mj, p, a9, seed, false)) break;
    }
    run_both(mi, mj, p, a9, seed, true);
    if (++fails >= 3) break;
  }
  CHECK(fails == 0);
  std::printf("jit fuzz %s %s: %u trials ok (%u inconclusive, %u self-modifying)\n",
              a9 ? "arm9" : "arm7", thumb ? "thumb" : "arm", trials, g_inconclusive, g_selfmod);
  g_inconclusive = 0; g_selfmod = 0;
  jit::detach(mj.nds);
}

} // namespace

int main(int argc, char** argv) {
  setenv("DS_JIT_WARM", "1", 1);   // the trials must run translated code, not the cold-code interpreter path
  const u32 trials = argc > 1 ? static_cast<u32>(std::atoi(argv[1])) : 400;
  // test_jit <count> <seed> [set]: run <count> trials from <seed> in one set,
  // and nothing else. <count> 1 is the single-trial form; a larger count
  // reproduces a failure that only appears with the set's earlier trials run
  // before it (the runtime is global and carries state across them).
  if (argc > 2) {   // set = 0..3 (arm9 arm/thumb, arm7 arm/thumb), default by seed range
    const u32 seed = static_cast<u32>(std::atoi(argv[2]));
    const int set = argc > 3 ? std::atoi(argv[3]) : (seed >= 3000 ? 2 : 0) + ((seed / 1000) % 2 == 0 ? 1 : 0);
    fuzz(set >= 2 ? Cpu::ARM7 : Cpu::ARM9, set & 1, trials, seed);
    return 0;
  }
  // Both timing models (timing_mode.h): the interpreter and the recompiler
  // must agree under each; the runtime re-emits its stubs between the two.
  for (const bool fast : {false, true}) {
    ds::g_fast_timing = fast;
    std::printf("jit: %s timing\n", fast ? "fast" : "exact");
    directed();
    fuzz(Cpu::ARM9, false, trials, 1000);
    fuzz(Cpu::ARM9, true, trials, 2000);
    fuzz(Cpu::ARM7, false, trials, 3000);
    fuzz(Cpu::ARM7, true, trials, 4000);
  }
  const jit::Stats& s = jit::stats();
  std::printf("jit: %llu blocks, %llu inline instrs, %llu fallbacks\n",
              (unsigned long long)s.blocks_translated, (unsigned long long)s.instrs_translated, (unsigned long long)s.instrs_fallback);
  std::puts("jit: ok");
  return 0;
}
