// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// The Action Replay interpreter: every opcode family, the condition stack,
// the loop, and the ways a malformed code has to fail rather than run away.
#include "core/nds.h"
#include "core/cheat/ar_engine.h"
#include "check.h"

#include <initializer_list>
#include <vector>

using namespace ds;
using cheat::Stop;

namespace {

constexpr u32 RAM = 0x02000000;

u32 r32(NDS& nds, u32 a) { return nds.bus.dma_read32(Cpu::ARM7, a); }
u16 r16(NDS& nds, u32 a) { return nds.bus.dma_read16(Cpu::ARM7, a); }
u8  r8 (NDS& nds, u32 a) { return nds.bus.dma_read8 (Cpu::ARM7, a); }
void w32(NDS& nds, u32 a, u32 v) { nds.bus.dma_write32(Cpu::ARM7, a, v); }

Stop run(NDS& nds, std::initializer_list<u32> words) {
  cheat::Engine e;
  cheat::Code c;
  c.name = "test";
  c.words.assign(words);
  return e.run_code(nds, c);
}

// The address field of a write opcode is 28 bits, so main RAM is named by
// its offset from 0x02000000 with the opcode in the top byte.
constexpr u32 at(u32 op, u32 addr) { return (op << 24) | (addr & 0x0FFFFFFF); }

void test_writes() {
  NDS nds;
  CHECK(run(nds, {at(0x00, 0x2000000), 0xCAFEBABE,
                  at(0x10, 0x2000010), 0x1234,
                  at(0x20, 0x2000020), 0x56}) == Stop::Ok);
  CHECK(r32(nds, RAM + 0x00) == 0xCAFEBABE);
  CHECK(r16(nds, RAM + 0x10) == 0x1234);
  CHECK(r8 (nds, RAM + 0x20) == 0x56);
  // Writes are relative to the offset register (D3 sets it).
  CHECK(run(nds, {0xD3000000, RAM + 0x100, at(0x00, 0x0000004), 0xAABBCCDD}) == Stop::Ok);
  CHECK(r32(nds, RAM + 0x104) == 0xAABBCCDD);
}

// Each 32-bit conditional, and the rule that a zero address field means the
// offset register rather than address zero.
void test_conditionals_32() {
  NDS nds;
  w32(nds, RAM, 100);
  const u32 store = at(0x00, 0x2000040);
  // b > mem, b < mem, b == mem, b != mem
  CHECK(run(nds, {at(0x30, 0x2000000), 200, store, 1, 0xD0000000, 0}) == Stop::Ok);
  CHECK(r32(nds, RAM + 0x40) == 1);
  CHECK(run(nds, {at(0x30, 0x2000000), 50, store, 2, 0xD0000000, 0}) == Stop::Ok);
  CHECK(r32(nds, RAM + 0x40) == 1);                       // not taken, no write
  CHECK(run(nds, {at(0x40, 0x2000000), 50, store, 3, 0xD0000000, 0}) == Stop::Ok);
  CHECK(r32(nds, RAM + 0x40) == 3);
  CHECK(run(nds, {at(0x50, 0x2000000), 100, store, 4, 0xD0000000, 0}) == Stop::Ok);
  CHECK(r32(nds, RAM + 0x40) == 4);
  CHECK(run(nds, {at(0x60, 0x2000000), 100, store, 5, 0xD0000000, 0}) == Stop::Ok);
  CHECK(r32(nds, RAM + 0x40) == 4);                       // equal, so != is false
  // Address field zero: the offset register supplies the address, and is not
  // added to it.
  // The offset is cleared again before the store, which would otherwise add it.
  CHECK(run(nds, {0xD3000000, RAM, at(0x50, 0), 100, 0xD3000000, 0, store, 6, 0xD0000000, 0}) == Stop::Ok);
  CHECK(r32(nds, RAM + 0x40) == 6);
}

// The 16-bit conditionals mask the loaded halfword with ~b.h before testing.
void test_conditionals_16() {
  NDS nds;
  w32(nds, RAM, 0x0000FF37);
  const u32 store = at(0x00, 0x2000040);
  // ~mask 0xFF00 leaves 0x0037; equal to 0x0037.
  CHECK(run(nds, {at(0x90, 0x2000000), 0xFF000037, store, 1, 0xD0000000, 0}) == Stop::Ok);
  CHECK(r32(nds, RAM + 0x40) == 1);
  // Without the mask the halfword is 0xFF37, so the same test fails.
  CHECK(run(nds, {at(0x90, 0x2000000), 0x00000037, store, 2, 0xD0000000, 0}) == Stop::Ok);
  CHECK(r32(nds, RAM + 0x40) == 1);
  CHECK(run(nds, {at(0xA0, 0x2000000), 0x00000037, store, 3, 0xD0000000, 0}) == Stop::Ok);
  CHECK(r32(nds, RAM + 0x40) == 3);
  CHECK(run(nds, {at(0x70, 0x2000000), 0xFF000038, store, 4, 0xD0000000, 0}) == Stop::Ok);
  CHECK(r32(nds, RAM + 0x40) == 4);                       // 0x38 > 0x37
  CHECK(run(nds, {at(0x80, 0x2000000), 0xFF000036, store, 5, 0xD0000000, 0}) == Stop::Ok);
  CHECK(r32(nds, RAM + 0x40) == 5);                       // 0x36 < 0x37
}

// Nesting works the ordinary way as long as the outer condition holds: the
// inner one pushes, and its ENDIF restores the outer state.
void test_nested_conditions() {
  NDS nds;
  w32(nds, RAM, 1);
  w32(nds, RAM + 0x40, 0);
  CHECK(run(nds, {at(0x50, 0x2000000), 1,                 // outer: true
                  at(0x50, 0x2000000), 999,               // inner: false
                  at(0x00, 0x2000040), 7,                 // skipped
                  0xD0000000, 0,                          // ENDIF: back to the outer true
                  at(0x00, 0x2000044), 8,
                  0xD0000000, 0,
                  at(0x00, 0x2000048), 9}) == Stop::Ok);
  CHECK(r32(nds, RAM + 0x40) == 0);
  CHECK(r32(nds, RAM + 0x44) == 8);
  CHECK(r32(nds, RAM + 0x48) == 9);
}

// A conditional reached while the flag is clear is skipped outright, so it
// never pushes -- but ENDIF pops regardless. A code that nests inside a false
// condition therefore pops a frame that was never pushed and stays disabled
// for the rest of the run. That is what the hardware does and what melonDS
// does; it is pinned here because it looks like a bug when a code misbehaves.
void test_skipped_conditional_does_not_push() {
  NDS nds;
  w32(nds, RAM, 1);
  w32(nds, RAM + 0x50, 0);
  CHECK(run(nds, {at(0x50, 0x2000000), 999,               // false
                  at(0x50, 0x2000000), 1,                 // skipped, so not pushed
                  0xD0000000, 0,                          // pops the outer frame: true again
                  at(0x00, 0x2000050), 5,                 // so this one runs
                  0xD0000000, 0,                          // pops an empty stack: false
                  at(0x00, 0x2000054), 6}) == Stop::Ok);  // and this one does not
  CHECK(r32(nds, RAM + 0x50) == 5);
  CHECK(r32(nds, RAM + 0x54) == 0);
}

void test_loop() {
  NDS nds;
  w32(nds, RAM + 0x40, 0);
  // FOR 0..3 { datareg += 1 }  -- four passes in total (b, then b-1..0).
  CHECK(run(nds, {0xD5000000, 0,                          // datareg = 0
                  0xC0000000, 3,                          // FOR
                  0xD4000000, 1,                          // datareg += 1
                  0xD1000000, 0,                          // NEXT
                  0xD6000000, RAM + 0x40}) == Stop::Ok);  // u32[b] = datareg
  CHECK(r32(nds, RAM + 0x40) == 4);
}

// D2 ends a loop like D1 but then clears the offset, the data register and
// the condition stack.
void test_loop_flush() {
  NDS nds;
  CHECK(run(nds, {0xD3000000, RAM + 0x200,                // offset = ...
                  0xD5000000, 0x99,                       // datareg = 0x99
                  0xC0000000, 0,                          // FOR 0..0 (one pass)
                  0xD2000000, 0,                          // NEXT+FLUSH
                  at(0x00, 0x2000050), 1}) == Stop::Ok);
  // The write landed at 0x2000050, not 0x2000050 + old offset.
  CHECK(r32(nds, RAM + 0x50) == 1);
}

void test_data_register() {
  NDS nds;
  const u32 out = 0xD6000000, dst = RAM + 0x60;
  CHECK(run(nds, {0xD5000000, 0x0F0F0F0F, 0xD4000001, 0xF0F0F0F0, out, dst}) == Stop::Ok);
  CHECK(r32(nds, dst) == 0xFFFFFFFF);                     // OR
  CHECK(run(nds, {0xD5000000, 0xFF00FF00, 0xD4000002, 0x0FF00FF0, out, dst}) == Stop::Ok);
  CHECK(r32(nds, dst) == 0x0F000F00);                     // AND
  CHECK(run(nds, {0xD5000000, 0xFFFF0000, 0xD4000003, 0xFF00FF00, out, dst}) == Stop::Ok);
  CHECK(r32(nds, dst) == 0x00FFFF00);                     // XOR
  CHECK(run(nds, {0xD5000000, 1, 0xD4000004, 4, out, dst}) == Stop::Ok);
  CHECK(r32(nds, dst) == 0x10);                           // shift left
  CHECK(run(nds, {0xD5000000, 0x80, 0xD4000005, 4, out, dst}) == Stop::Ok);
  CHECK(r32(nds, dst) == 0x8);                            // shift right
  CHECK(run(nds, {0xD5000000, 0x00000001, 0xD4000006, 4, out, dst}) == Stop::Ok);
  CHECK(r32(nds, dst) == 0x10000000);                     // rotate right
  CHECK(run(nds, {0xD5000000, 0xFFFFFFF0, 0xD4000007, 4, out, dst}) == Stop::Ok);
  CHECK(r32(nds, dst) == 0xFFFFFFFF);                     // arithmetic shift right
  CHECK(run(nds, {0xD5000000, 7, 0xD4000008, 6, out, dst}) == Stop::Ok);
  CHECK(r32(nds, dst) == 42);                             // multiply
  // A shift of more than 31 clears the register; the arithmetic one saturates.
  CHECK(run(nds, {0xD5000000, 0xFF, 0xD4000004, 99, out, dst}) == Stop::Ok);
  CHECK(r32(nds, dst) == 0);
  CHECK(run(nds, {0xD5000000, 0x80000000, 0xD4000007, 99, out, dst}) == Stop::Ok);
  CHECK(r32(nds, dst) == 0xFFFFFFFF);
}

// The sized stores advance the offset by their own width; the loads do not.
void test_datareg_memory() {
  NDS nds;
  w32(nds, RAM + 0x80, 0x11223344);
  // Both the load and the store add the offset to their operand.
  CHECK(run(nds, {0xD3000000, 0x80, 0xD9000000, RAM, 0xD6000000, RAM + 0x90}) == Stop::Ok);
  CHECK(r32(nds, RAM + 0x110) == 0x11223344);             // 0x90 + the 0x80 offset
  CHECK(run(nds, {0xD3000000, 0, 0xDA000000, RAM + 0x80, 0xD6000000, RAM + 0x94}) == Stop::Ok);
  CHECK(r32(nds, RAM + 0x94) == 0x3344);                  // 16-bit load
  CHECK(run(nds, {0xD3000000, 0, 0xDB000000, RAM + 0x80, 0xD6000000, RAM + 0x98}) == Stop::Ok);
  CHECK(r32(nds, RAM + 0x98) == 0x44);                    // 8-bit load
  // Three stores from offset 0 land at +0, +4 and +6 as the offset advances.
  CHECK(run(nds, {0xD3000000, 0, 0xD5000000, 0xAABBCCDD,
                  0xD6000000, RAM + 0xA0,                 // 32-bit, offset += 4
                  0xD7000000, RAM + 0xA0,                 // 16-bit at +4, offset += 2
                  0xD8000000, RAM + 0xA0}) == Stop::Ok);  // 8-bit at +6
  CHECK(r32(nds, RAM + 0xA0) == 0xAABBCCDD);
  CHECK(r16(nds, RAM + 0xA4) == 0xCCDD);
  CHECK(r8 (nds, RAM + 0xA6) == 0xDD);
}

void test_offset_ops() {
  NDS nds;
  w32(nds, RAM + 0xB0, RAM + 0xC0);
  // B0: offset = u32[a + offset]
  CHECK(run(nds, {at(0xB0, 0x20000B0), 0, at(0x00, 0), 0x5A5A5A5A}) == Stop::Ok);
  CHECK(r32(nds, RAM + 0xC0) == 0x5A5A5A5A);
  // DC adds to the offset; C6 stores it.
  CHECK(run(nds, {0xD3000000, 0x100, 0xDC000000, 0x23, 0xC6000000, RAM + 0xD0}) == Stop::Ok);
  CHECK(r32(nds, RAM + 0xD0) == 0x123);
}

// C5 counts every time it is reached, and tests the count against b.
void test_c5_counter() {
  NDS nds;
  w32(nds, RAM + 0xE0, 0);
  // Four passes; the write runs only when (count & 1) == 1, i.e. on 1 and 3.
  CHECK(run(nds, {0xD5000000, 0,
                  0xC0000000, 3,
                  0xC5000000, 0x00010001,
                  0xD4000000, 1,
                  0xD0000000, 0,
                  0xD1000000, 0,
                  0xD6000000, RAM + 0xE0}) == Stop::Ok);
  CHECK(r32(nds, RAM + 0xE0) == 2);
}

void test_block_write() {
  NDS nds;
  // Eight bytes: two whole words, no tail.
  CHECK(run(nds, {at(0xE0, 0x2000100), 8, 0x11223344, 0x55667788}) == Stop::Ok);
  CHECK(r32(nds, RAM + 0x100) == 0x11223344);
  CHECK(r32(nds, RAM + 0x104) == 0x55667788);
  // Five bytes: a word and a byte, still costing a whole pair of data words.
  CHECK(run(nds, {at(0xE0, 0x2000110), 5, 0xAABBCCDD, 0x000000EE,
                  at(0x00, 0x2000120), 0x600D}) == Stop::Ok);
  CHECK(r32(nds, RAM + 0x110) == 0xAABBCCDD);
  CHECK(r8 (nds, RAM + 0x114) == 0xEE);
  CHECK(r32(nds, RAM + 0x120) == 0x600D);                 // the pair was stepped over exactly
  // Three bytes: no word, three bytes, one pair.
  CHECK(run(nds, {at(0xE0, 0x2000130), 3, 0x00332211, 0,
                  at(0x00, 0x2000140), 0xF00D}) == Stop::Ok);
  CHECK(r8(nds, RAM + 0x130) == 0x11);
  CHECK(r8(nds, RAM + 0x131) == 0x22);
  CHECK(r8(nds, RAM + 0x132) == 0x33);
  CHECK(r32(nds, RAM + 0x140) == 0xF00D);
}

// A skipped block write must step over its data, or the words are executed.
void test_block_write_skipped() {
  NDS nds;
  w32(nds, RAM, 1);
  w32(nds, RAM + 0x150, 0);
  CHECK(run(nds, {at(0x50, 0x2000000), 999,               // false
                  at(0xE0, 0x2000150), 8, 0xDEADBEEF, 0xDEADBEEF,
                  0xD0000000, 0,
                  at(0x00, 0x2000160), 0xBEEF}) == Stop::Ok);
  CHECK(r32(nds, RAM + 0x150) == 0);                      // the block did not run
  CHECK(r32(nds, RAM + 0x160) == 0xBEEF);                 // and the data was not executed
}

void test_block_copy() {
  NDS nds;
  w32(nds, RAM + 0x180, 0x01020304);
  w32(nds, RAM + 0x184, 0x05060708);
  CHECK(run(nds, {0xD3000000, RAM + 0x180, at(0xF0, 0x2000190), 8}) == Stop::Ok);
  CHECK(r32(nds, RAM + 0x190) == 0x01020304);
  CHECK(r32(nds, RAM + 0x194) == 0x05060708);
  // A length that is not a multiple of four finishes byte by byte.
  CHECK(run(nds, {0xD3000000, RAM + 0x180, at(0xF0, 0x20001A0), 6}) == Stop::Ok);
  CHECK(r32(nds, RAM + 0x1A0) == 0x01020304);
  CHECK(r16(nds, RAM + 0x1A4) == 0x0708);
}

// Malformed codes must stop, not run off the end or spin for ever.
void test_malformed() {
  NDS nds;
  // An opcode that does not exist.
  CHECK(run(nds, {0xC1000000, 0}) == Stop::BadOpcode);
  CHECK(run(nds, {0xDD000000, 0}) == Stop::BadOpcode);
  CHECK(run(nds, {0xD400000F, 0}) == Stop::BadOpcode);    // no such data op
  // C4 is refused rather than guessed at.
  CHECK(run(nds, {0xC4000000, 0}) == Stop::Unsupported);
  // A block write whose data is not there.
  CHECK(run(nds, {at(0xE0, 0x2000100), 64, 0, 0}) == Stop::Truncated);
  // The same, while skipping.
  w32(nds, RAM, 1);
  CHECK(run(nds, {at(0x50, 0x2000000), 999, at(0xE0, 0x2000100), 64, 0, 0}) == Stop::Truncated);
  // A loop with no end runs into the step cap instead of hanging.
  CHECK(run(nds, {0xC0000000, 0xFFFFFFFF, 0xD1000000, 0}) == Stop::RunawayLoop);
  // An odd trailing word cannot form a pair and is ignored.
  CHECK(run(nds, {at(0x00, 0x2000000), 0x1234, 0xDEADBEEF}) == Stop::Ok);
  CHECK(r32(nds, RAM) == 0x1234);
  // An empty code is fine.
  CHECK(run(nds, {}) == Stop::Ok);
}

// The engine runs enabled codes in order and leaves the disabled ones alone.
void test_engine_enable() {
  NDS nds;
  w32(nds, RAM + 0x1C0, 0);
  w32(nds, RAM + 0x1C4, 0);
  cheat::Engine e;
  // Named fields: Code gained a description and a group when the database
  // loader landed, and a positional list would silently follow it.
  cheat::Code on;  on.name = "on";   on.enabled = true;  on.words = {at(0x00, 0x20001C0), 1};
  cheat::Code off; off.name = "off"; off.enabled = false; off.words = {at(0x00, 0x20001C4), 1};
  e.codes.push_back(on);
  e.codes.push_back(off);
  e.run(nds);
  CHECK(r32(nds, RAM + 0x1C0) == 1);
  CHECK(r32(nds, RAM + 0x1C4) == 0);
  // Codes run every call, so a later frame sees the write again.
  w32(nds, RAM + 0x1C0, 0);
  e.run(nds);
  CHECK(r32(nds, RAM + 0x1C0) == 1);
}

// The hook: codes run when the ARM7 takes its VBlank IRQ, and only then.
// Every path that takes an IRQ -- interpreter, JIT and the scheduler's
// between-slices check -- goes through CpuContext::check_irq, so this covers
// all three.
void arm_vblank(NDS& nds) {
  CpuContext& arm7 = nds.cpu(Cpu::ARM7);
  arm7.hot.cpsr &= ~0x80u;                                   // IRQs unmasked
  nds.io.cpu_io[1].ime = 1;
  nds.io.cpu_io[1].ie = 1u << io::IRQ_VBLANK;
  nds.io.set_irq_line(Cpu::ARM7, io::IRQ_VBLANK, true);      // the GPU's own path
  CHECK(arm7.hot.irq_pending);
}

void test_hook_runs_on_arm7_vblank() {
  NDS nds;
  w32(nds, RAM + 0x200, 0);
  cheat::Code c;
  c.name = "hook";
  c.enabled = true;
  c.words = {at(0x00, 0x2000200), 0x1234};
  nds.cheats.codes.push_back(c);

  arm_vblank(nds);
  nds.cpu(Cpu::ARM7).check_irq();
  CHECK(r32(nds, RAM + 0x200) == 0x1234);

  // A disabled code is not run by the hook either.
  w32(nds, RAM + 0x200, 0);
  nds.cheats.codes[0].enabled = false;
  nds.io.set_irq_line(Cpu::ARM7, io::IRQ_VBLANK, true);
  nds.cpu(Cpu::ARM7).hot.cpsr &= ~0x80u;
  nds.cpu(Cpu::ARM7).check_irq();
  CHECK(r32(nds, RAM + 0x200) == 0);
}

// The ARM9 takes VBlank too, and must not run the codes: on hardware the
// cartridge hooks the ARM7's handler, and running them twice a frame would
// double every "add one" style code.
void test_hook_ignores_arm9() {
  NDS nds;
  w32(nds, RAM + 0x204, 0);
  cheat::Code c;
  c.name = "hook";
  c.enabled = true;
  c.words = {at(0x00, 0x2000204), 0x99};
  nds.cheats.codes.push_back(c);

  CpuContext& arm9 = nds.cpu(Cpu::ARM9);
  arm9.hot.cpsr &= ~0x80u;
  nds.io.cpu_io[0].ime = 1;
  nds.io.cpu_io[0].ie = 1u << io::IRQ_VBLANK;
  nds.io.set_irq_line(Cpu::ARM9, io::IRQ_VBLANK, true);
  arm9.check_irq();
  CHECK(r32(nds, RAM + 0x204) == 0);
}

// Another IRQ on the ARM7 is not the cheats' cue.
void test_hook_ignores_other_irqs() {
  NDS nds;
  w32(nds, RAM + 0x208, 0);
  cheat::Code c;
  c.name = "hook";
  c.enabled = true;
  c.words = {at(0x00, 0x2000208), 0x77};
  nds.cheats.codes.push_back(c);

  CpuContext& arm7 = nds.cpu(Cpu::ARM7);
  arm7.hot.cpsr &= ~0x80u;
  nds.io.cpu_io[1].ime = 1;
  nds.io.cpu_io[1].ie = 1u << io::IRQ_HBLANK;
  nds.io.set_irq_line(Cpu::ARM7, io::IRQ_HBLANK, true);
  arm7.check_irq();
  CHECK(r32(nds, RAM + 0x208) == 0);
}

// A VBlank that is pending but not enabled is not the handler running, so it
// is not the cheats' cue either.
void test_hook_needs_ie_and_if() {
  NDS nds;
  w32(nds, RAM + 0x20C, 0);
  cheat::Code c;
  c.name = "hook";
  c.enabled = true;
  c.words = {at(0x00, 0x200020C), 0x55};
  nds.cheats.codes.push_back(c);

  CpuContext& arm7 = nds.cpu(Cpu::ARM7);
  arm7.hot.cpsr &= ~0x80u;
  nds.io.cpu_io[1].ime = 1;
  nds.io.cpu_io[1].ie = 1u << io::IRQ_HBLANK;      // HBlank is what is enabled
  nds.io.cpu_io[1].if_ = 1u << io::IRQ_VBLANK;     // VBlank is merely pending
  nds.io.set_irq_line(Cpu::ARM7, io::IRQ_HBLANK, true);
  arm7.check_irq();
  CHECK(r32(nds, RAM + 0x20C) == 0);
}

} // namespace

int main() {
  test_writes();
  test_conditionals_32();
  test_conditionals_16();
  test_nested_conditions();
  test_skipped_conditional_does_not_push();
  test_loop();
  test_loop_flush();
  test_data_register();
  test_datareg_memory();
  test_offset_ops();
  test_c5_counter();
  test_block_write();
  test_block_write_skipped();
  test_block_copy();
  test_malformed();
  test_engine_enable();
  test_hook_runs_on_arm7_vblank();
  test_hook_ignores_arm9();
  test_hook_ignores_other_irqs();
  test_hook_needs_ie_and_if();
  std::printf("cheat: ok\n");
  return 0;
}
