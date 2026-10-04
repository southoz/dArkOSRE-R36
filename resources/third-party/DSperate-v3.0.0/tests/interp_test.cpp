// SPDX-License-Identifier: GPL-3.0-or-later
// Hand-encoded instruction sequences run through the interpreter.
#include "core/nds.h"
#include "core/cpu/arm_decode.h"
#include "core/cpu/interp/interp.h"
#include "check.h"

#include <cstring>
#include <vector>

using namespace ds;

struct Harness {
  NDS nds;
  CpuContext& cpu;
  explicit Harness(Cpu which = Cpu::ARM9) : cpu(nds.cpu(which)) {}
  void load(u32 addr, const std::vector<u32>& words) {
    for (size_t i = 0; i < words.size(); ++i) nds.bus.write32(cpu.which, addr + 4 * i, words[i]), poke32(addr + 4 * i, words[i]);
  }
  void poke32(u32 addr, u32 v) { std::memcpy(cpu.page_table.read_ptr(addr), &v, 4); }
  u32  peek32(u32 addr) { u32 v; std::memcpy(&v, cpu.page_table.read_ptr(addr), 4); return v; }
  void poke16(u32 addr, u16 v) { std::memcpy(cpu.page_table.read_ptr(addr), &v, 2); }
  void start_arm(u32 pc) { cpu.hot.cpsr = 0x1F; cpu.hot.regs[15] = pc + 8; }
  void start_thumb(u32 pc) { cpu.hot.cpsr = 0x3F; cpu.hot.regs[15] = pc + 4; }
  void step(int n = 1) { cpu.steps = 0; cpu.step_limit = static_cast<u32>(n); cpu.hot.cycle_budget = 1 << 30; interp::run(cpu); cpu.step_limit = 0; }
  u32& r(int i) { return cpu.hot.regs[i]; }
  u32 cpsr() const { return cpu.hot.cpsr; }
};

static void test_decode() {
  using namespace ds::arm;
  CHECK(decode_arm(0xE0821003) == AOp::DpImmShift);   // add r1, r2, r3
  CHECK(decode_arm(0xE2821001) == AOp::DpImm);        // add r1, r2, #1
  CHECK(decode_arm(0xE1A01312) == AOp::DpRegShift);   // mov r1, r2, lsl r3
  CHECK(decode_arm(0xE0010392) == AOp::Mul);          // mul r1, r2, r3
  CHECK(decode_arm(0xE0810392) == AOp::Umull);
  CHECK(decode_arm(0xE1021093) == AOp::Swp);
  CHECK(decode_arm(0xE1439099) == AOp::Swpb);         // swpb r9, r9, [r3] (bit 22 = byte; was decoded as undefined)
  CHECK(decode_arm(0xE12FFF1E) == AOp::Bx);
  CHECK(decode_arm(0xE12FFF3E) == AOp::BlxReg);
  CHECK(decode_arm(0xE16F1F12) == AOp::Clz);
  CHECK(decode_arm(0xE10F1000) == AOp::Mrs);
  CHECK(decode_arm(0xE129F001) == AOp::MsrReg);
  CHECK(decode_arm(0xE321F0D3) == AOp::MsrImm);
  CHECK(decode_arm(0xE5921004) == AOp::LdrStrImm);
  CHECK(decode_arm(0xE7921003) == AOp::LdrStrReg);
  CHECK(decode_arm(0xE1D210B4) == AOp::LdrStrHImm);   // ldrh r1, [r2, #4]
  CHECK(decode_arm(0xE19210B3) == AOp::LdrStrHReg);
  CHECK(decode_arm(0xE8BD8FF0) == AOp::Ldm);
  CHECK(decode_arm(0xE92D4FF0) == AOp::Stm);
  CHECK(decode_arm(0xEA000000) == AOp::B);
  CHECK(decode_arm(0xEB000000) == AOp::Bl);
  CHECK(decode_arm(0xEF000000) == AOp::Swi);
  CHECK(decode_arm(0xEE070F9A) == AOp::Mcr);
  CHECK(decode_arm(0xEE100F10) == AOp::Mrc);
  CHECK(decode_arm(0xE1020051) == AOp::QAdd);
  CHECK(decode_arm(0xE1220051) == AOp::QSub);
  CHECK(decode_arm(0xE1610382) == AOp::SmulXY);      // smulbb r1, r2, r3
  CHECK(decode_thumb(0x1888) == TOp::AddSubReg);     // add r0, r1, r2
  CHECK(decode_thumb(0x0048) == TOp::ShiftImm);      // lsl r0, r1, #1
  CHECK(decode_thumb(0x2001) == TOp::MovCmpAddSubImm8);
  CHECK(decode_thumb(0x4008) == TOp::Alu);
  CHECK(decode_thumb(0x4470) == TOp::HiRegOp);
  CHECK(decode_thumb(0x4770) == TOp::BxBlx);
  CHECK(decode_thumb(0x4800) == TOp::LdrPcRel);
  CHECK(decode_thumb(0xB500) == TOp::PushPop);
  CHECK(decode_thumb(0xBD00) == TOp::PushPop);
  CHECK(decode_thumb(0xB080) == TOp::AdjustSp);
  CHECK(decode_thumb(0xD0FE) == TOp::BCond);
  CHECK(decode_thumb(0xDF00) == TOp::Swi);
  CHECK(decode_thumb(0xE7FE) == TOp::B);
  CHECK(decode_thumb(0xF000) == TOp::BlPrefix);
  CHECK(decode_thumb(0xF800) == TOp::BlSuffix);
  CHECK(decode_thumb(0xE800) == TOp::BlxSuffix);
  CHECK(decode_thumb(0xC800) == TOp::StmLdm);
  std::puts("decode: ok");
}

static void test_arm_alu() {
  Harness h;
  const u32 base = 0x02000000;
  h.load(base, {
    0xE3A01005,   // mov r1, #5
    0xE3A02003,   // mov r2, #3
    0xE0413002,   // sub r3, r1, r2        -> 2
    0xE0524001,   // subs r4, r2, r1       -> -2, N set, C clear
    0xE2915000,   // adds r5, r1, #0       -> 5, flags NZCV=0000
    0xE1A06121,   // mov r6, r1, lsr #2    -> 1
    0xE3A07102,   // mov r7, #0x80000000
    0xE0978007,   // adds r8, r7, r7       -> 0, Z C V set
    0xE2A09000,   // adc r9, r0, #0        -> 1 (r0 = 0, carry in)
  });
  h.start_arm(base);
  h.step(9);
  CHECK(h.r(1) == 5 && h.r(2) == 3 && h.r(3) == 2);
  CHECK(h.r(4) == 0xFFFFFFFE);
  CHECK(h.r(5) == 5);
  CHECK(h.r(6) == 1);
  CHECK(h.r(8) == 0);
  CHECK((h.cpsr() & 0xF0000000) == 0x70000000);   // Z C V
  CHECK(h.r(9) == 1);
  CHECK(h.r(15) == base + 9 * 4 + 8);
  std::puts("arm alu: ok");
}

static void test_arm_memory_and_branch() {
  Harness h;
  const u32 base = 0x02000000, data = 0x02001000;
  h.load(base, {
    0xE59F1010,   // ldr r1, [pc, #16]      -> literal at base+0x18
    0xE3A02042,   // mov r2, #0x42
    0xE5C12003,   // strb r2, [r1, #3]
    0xE5913000,   // ldr r3, [r1]
    0xE1A0F00E,   // mov pc, lr (lr = 0) -> jumps to 0, we stop before
    0xEAFFFFFE,   // b . (filler)
    data,         // literal
  });
  h.poke32(data, 0x11223344);
  h.start_arm(base);
  h.step(4);
  CHECK(h.r(1) == data);
  CHECK(h.r(3) == 0x42223344);
  // BL and return.
  h.load(base, {
    0xEB000001,   // bl +4  -> base+12
    0xE3A00001,   // mov r0, #1   (skipped)
    0xE3A00002,   // mov r0, #2   (skipped)
    0xE3A00003,   // mov r0, #3   (target)
    0xE12FFF1E,   // bx lr
  });
  h.start_arm(base);
  h.r(0) = 0;
  h.step(3);
  CHECK(h.r(0) == 3);
  CHECK(h.r(14) == base + 4);
  CHECK(h.r(15) == base + 4 + 8);
  std::puts("arm mem/branch: ok");
}

static void test_ldm_stm() {
  Harness h;
  const u32 base = 0x02000000, sp = 0x02002000;
  h.load(base, {
    0xE92D000E,   // push {r1-r3}   (stmdb sp!, {r1,r2,r3})
    0xE3A01000, 0xE3A02000, 0xE3A03000,   // clear r1-r3
    0xE8BD000E,   // pop {r1-r3}
  });
  h.start_arm(base);
  h.r(13) = sp; h.r(1) = 0xA; h.r(2) = 0xB; h.r(3) = 0xC;
  h.step(5);
  CHECK(h.r(13) == sp);
  CHECK(h.r(1) == 0xA && h.r(2) == 0xB && h.r(3) == 0xC);
  CHECK(h.peek32(sp - 12) == 0xA && h.peek32(sp - 4) == 0xC);
  std::puts("ldm/stm: ok");
}

static void test_thumb() {
  Harness h;
  const u32 base = 0x02000000;
  std::vector<u16> code = {
    0x2005,         // movs r0, #5
    0x2103,         // movs r1, #3
    0x1A42,         // subs r2, r0, r1   -> 2
    0x0083,         // lsls r3, r0, #2   -> 20
    0x4008,         // ands r0, r1       -> 1
    0x4249,         // negs r1, r1       -> -3
    0xB403,         // push {r0, r1}
    0x2000, 0x2100, // clear
    0xBC03,         // pop {r0, r1}
    0xF000, 0xF802, // bl +4
    0x2099,         // (skipped)
    0x2098,         // (skipped)
    0x2077,         // target: movs r0, #0x77
    0x4770,         // bx lr
  };
  for (size_t i = 0; i < code.size(); ++i) h.poke16(base + 2 * i, code[i]);
  h.start_thumb(base);
  h.r(13) = 0x02002000;
  h.step(6);
  CHECK(h.r(2) == 2 && h.r(3) == 20 && h.r(0) == 1 && h.r(1) == 0xFFFFFFFD);
  CHECK(h.cpsr() & 0x80000000);        // N from negs
  h.step(4);                            // push, clear, clear, pop
  CHECK(h.r(0) == 1 && h.r(1) == 0xFFFFFFFD && h.r(13) == 0x02002000);
  h.step(2);                            // bl prefix + suffix
  CHECK(h.r(14) == ((base + 2 * 12) | 1));
  CHECK(h.r(15) == base + 2 * 14 + 4);
  h.step(2);                            // movs, bx lr
  CHECK(h.r(0) == 0x77);
  CHECK(h.r(15) == base + 2 * 12 + 4);
  CHECK(h.cpsr() & 0x20);              // still Thumb
  std::puts("thumb: ok");
}

static void test_exceptions_and_modes() {
  Harness h;
  const u32 base = 0x02000000;
  h.load(base, {
    0xEF000042,   // swi 0x42
  });
  h.start_arm(base);
  h.cpu.hot.cpsr = 0x10;                // user mode, IRQs enabled
  h.r(13) = 0x1111;                     // user sp
  h.cpu.cp15_control |= (1u << 13);     // vectors at 0xFFFF0000
  h.step(1);
  CHECK((h.cpsr() & 0x1F) == 0x13);     // SVC
  CHECK(h.cpsr() & 0x80);               // IRQ masked
  CHECK(h.cpu.hot.spsr == 0x10);
  CHECK(h.r(14) == base + 4);
  CHECK(h.r(15) == 0xFFFF0008 + 8);
  CHECK(h.r(13) != 0x1111);             // banked sp
  // Return with movs pc, lr -> user mode, sp restored.
  h.cpu.page_table.map(0xFFFF0000, mem::PAGE_SIZE, h.nds.bus.main_ram.get() + 0x3000, mem::PAGE_READABLE | mem::PAGE_WRITABLE);
  h.poke32(0xFFFF0008, 0xE1B0F00E);     // movs pc, lr
  h.step(1);
  CHECK((h.cpsr() & 0x1F) == 0x10);
  CHECK(h.r(13) == 0x1111);
  CHECK(h.r(15) == base + 4 + 8);
  // IRQ delivery.
  h.cpu.hot.irq_pending = 1;
  h.step(1);
  CHECK((h.cpsr() & 0x1F) == 0x12);
  CHECK(h.r(14) == base + 8);            // IRQ LR = next instruction + 4
  CHECK(h.r(15) == 0xFFFF001C + 8);      // vector taken, then one (nop) instruction ran
  std::puts("exceptions/modes: ok");
}

int main() {
  test_decode();
  test_arm_alu();
  test_arm_memory_and_branch();
  test_ldm_stm();
  test_thumb();
  test_exceptions_and_modes();
  return 0;
}
