// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// 3D engine tests: command FIFO and status, matrix stack, position test, and
// an end-to-end quad through the geometry engine and rasteriser.
#include "core/nds.h"

#include <cstdio>

using namespace ds;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)
#define CHECK_EQ(a, b) do { auto va_ = (a); auto vb_ = (b); if (static_cast<unsigned long long>(va_) != static_cast<unsigned long long>(vb_)) { std::fprintf(stderr, "FAIL %s:%d: %s = %llx, expected %llx\n", __FILE__, __LINE__, #a, (unsigned long long)va_, (unsigned long long)vb_); ++failures; } } while (0)

static void w32(NDS& nds, u32 a, u32 v) { nds.bus.dma_write32(Cpu::ARM9, a, v); }
static u32  r32(NDS& nds, u32 a) { return nds.bus.dma_read32(Cpu::ARM9, a); }
static void w16(NDS& nds, u32 a, u16 v) { nds.bus.dma_write16(Cpu::ARM9, a, v); }

// Command port addresses.
constexpr u32 CMD(u32 c) { return 0x04000400 + c * 4; }

static void power_on(NDS& nds) { w16(nds, 0x04000304, 0x820F); }
// Run the geometry engine for `cycles` system cycles.
static void advance(NDS& nds, u32 cycles) { nds.sched.run_until(nds.sched.now() + cycles * 2); }

// The synthesised GXSTAT (Phase 1a). There is no FIFO level, no stall and no
// execution clock: a read replays whatever is queued and then answers from a
// fixed, deliberately safe shape. This is the contract §3.4 says must not be
// weakened, so it is asserted directly rather than inferred from a drain.
static void test_gxstat_synthesis() {
  NDS nds; power_on(nds);
  auto st = [&] { return r32(nds, 0x04000600); };
  // Empty, below half full, not busy -- before anything is queued.
  CHECK_EQ(st() & 0x07FF0000, 0x06000000u);
  CHECK(!(st() & (1u << 27)));
  // 300 commands: more than the old 4-deep pipe plus 256-entry FIFO plus
  // 64-entry stall queue could hold. The level still reads zero, the
  // half-full and empty bits still read set, and nothing stalls.
  for (int i = 0; i < 300; ++i) w32(nds, CMD(0x10), 0);
  CHECK_EQ(st() & 0x07FF0000, 0x06000000u);
  advance(nds, 5000);
  CHECK_EQ(st() & 0x07FF0000, 0x06000000u);
}

// The busy bit (27) is the one piece of GXSTAT that is not constant: it is
// held from SWAP_BUFFERS until 650 cycles past the swap, so a game polling
// "has my swap landed?" sees a busy -> idle edge at a plausible time rather
// than an immediate idle.
static void test_gxstat_swap_busy() {
  NDS nds; power_on(nds);
  CHECK(!(r32(nds, 0x04000600) & (1u << 27)));
  w32(nds, CMD(0x50), 0);                                      // swap buffers
  CHECK(r32(nds, 0x04000600) & (1u << 27));                    // busy from the command
  // VBlank is where swap_wait_ becomes a deadline and the 650-cycle tail
  // starts; measured, the bit drops between 0.8 M and 1.0 M ARM9 cycles from
  // a cold boot, so run well past that rather than up against it.
  advance(nds, 600000);                                        // past VBlank and the 650-cycle tail
  CHECK(!(r32(nds, 0x04000600) & (1u << 27)));
}

static void test_matrix_stack() {
  NDS nds; power_on(nds);
  w32(nds, CMD(0x10), 1);                                      // position matrix
  w32(nds, CMD(0x15), 0);                                      // identity
  w32(nds, CMD(0x1C), 0x3000); w32(nds, CMD(0x1C), 0x2000); w32(nds, CMD(0x1C), 0x1000);   // translate (3,2,1)
  w32(nds, CMD(0x11), 0);                                      // push
  w32(nds, CMD(0x1B), 0x2000); w32(nds, CMD(0x1B), 0x2000); w32(nds, CMD(0x1B), 0x2000);   // scale 2
  advance(nds, 1000);
  CHECK_EQ((r32(nds, 0x04000600) >> 8) & 0x1F, 1u);            // stack pointer
  CHECK_EQ(r32(nds, 0x04000640), 0x2000u);                     // clip matrix = proj(identity) * pos
  CHECK_EQ(r32(nds, 0x04000670), 0x3000u);                     // translation survives the scale
  w32(nds, CMD(0x12), 1);                                      // pop
  advance(nds, 1000);
  CHECK_EQ((r32(nds, 0x04000600) >> 8) & 0x1F, 0u);
  CHECK_EQ(r32(nds, 0x04000640), 0x1000u);
  // Position test of (1,1,1) through the translated matrix.
  w32(nds, CMD(0x71), 0x10001000); w32(nds, CMD(0x71), 0x1000);
  advance(nds, 1000);
  CHECK_EQ(r32(nds, 0x04000620), 0x4000u);
  CHECK_EQ(r32(nds, 0x04000624), 0x3000u);
  CHECK_EQ(r32(nds, 0x04000628), 0x2000u);
  CHECK_EQ(r32(nds, 0x0400062C), 0x1000u);
}

// Orthographic projection so clip-space X/Y map directly to the screen.
static void ortho(NDS& nds) {
  w32(nds, CMD(0x10), 0);
  w32(nds, CMD(0x15), 0);
  w32(nds, CMD(0x10), 1);
  w32(nds, CMD(0x15), 0);
  w32(nds, CMD(0x60), 0xBFFF0000);                             // viewport 0,0 - 255,191
}

static void test_flat_quad() {
  NDS nds; power_on(nds);
  w32(nds, 0x04000060, 0);                                     // no texturing
  w32(nds, 0x04000350, 0x001F0000);                            // clear colour: black, alpha 31
  ortho(nds);
  w32(nds, CMD(0x29), 0x001F00C0);                             // alpha 31, both faces
  w32(nds, CMD(0x2A), 0);
  w32(nds, CMD(0x40), 1);                                      // quads
  w32(nds, CMD(0x20), 0x001F);                                 // red
  // A quad covering clip x in [-0.5, 0.5], y in [-0.5, 0.5] at z = 0 (w = 1).
  auto v = [&](s16 x, s16 y) { w32(nds, CMD(0x25), (static_cast<u16>(y) << 16) | static_cast<u16>(x)); };
  v(-0x800, -0x800); v(0x800, -0x800); v(0x800, 0x800); v(-0x800, 0x800);   // z stays 0 from reset
  w32(nds, CMD(0x41), 0);
  advance(nds, 3000);
  CHECK_EQ(r32(nds, 0x04000604), 0x00040001u);                 // one polygon, four vertices
  w32(nds, CMD(0x50), 0);                                      // swap buffers
  // Run to the frame after next so the polygon is flushed, rendered and displayed.
  nds.sched.run_until(nds.sched.now() + CYCLES_PER_FRAME * 2);
  const u32* line = nds.gpu3d.line(nds.gpu3d.frame_ref(), 96);
  CHECK_EQ(line[128] & 0x1F00003F, 0x1F00003Fu);              // centre: red, opaque
  CHECK_EQ(line[10] >> 24, 0x1Fu);                              // outside the quad: clear colour
  CHECK_EQ(line[10] & 0xFFFFFF, 0u);
  // Quad spans x 64..191 on the 256-wide viewport.
  CHECK_EQ(line[64] & 0x3F, 0x3Fu);
  CHECK_EQ(line[63] & 0x3F, 0u);
  CHECK_EQ(line[191] & 0x3F, 0x3Fu);
  CHECK_EQ(line[192] & 0x3F, 0u);
}

static void test_final_pass() {
  NDS nds; power_on(nds);
  // Each pass alone (AA, edge marking, fog with and without colour), then all together.
  static const u32 modes[] = {1u << 4, 1u << 5, 1u << 7, (1u << 7) | (1u << 6), (1u << 4) | (1u << 5) | (1u << 7)};
  for (u32 m : modes) for (u32 seed = 0; seed < 4; ++seed) {
    const u32 d = nds.gpu3d.renderer().selftest_final_pass(seed, m);
    if (d) std::fprintf(stderr, "final_pass dispcnt %x seed %u: %u diffs\n", m, seed, d);
    CHECK_EQ(d, 0u);
  }
}

int main() {
  test_gxstat_synthesis();
  test_gxstat_swap_busy();
  test_matrix_stack();
  test_flat_quad();
  test_final_pass();
  if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
  std::puts("gpu3d: ok");
  return 0;
}
