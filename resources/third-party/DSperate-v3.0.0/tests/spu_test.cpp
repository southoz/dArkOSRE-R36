// SPDX-License-Identifier: GPL-3.0-or-later
// SPU channel / mixer / capture checks against hand-computed expectations.
#include "core/nds.h"
#include "check.h"

#include <algorithm>
#include <cstdio>
#include <vector>

using namespace ds;

namespace {

constexpr u32 RAM = 0x02000000;

struct Rig {
  NDS nds;
  Rig() {
    nds.reset();
    nds.io.write(Cpu::ARM7, 0x04000304, 16, 1);          // POWCNT2: sound on
    nds.io.write(Cpu::ARM7, 0x04000500, 16, 0x8000 | 127); // master enable, full volume
    nds.io.write(Cpu::ARM7, 0x04000504, 16, 0x200);
    nds.spu.drain();
  }
  void w32(u32 a, u32 v) { nds.bus.dma_write32(Cpu::ARM7, a, v); }
  void w16(u32 a, u16 v) { nds.bus.dma_write16(Cpu::ARM7, a, v); }
  void chan(int n, u32 cnt, u32 src, u16 timer, u16 loop, u32 len) {
    const u32 b = 0x04000400 + n * 16;
    nds.io.write(Cpu::ARM7, b + 4, 32, src);
    nds.io.write(Cpu::ARM7, b + 8, 32, timer | (static_cast<u32>(loop) << 16));
    nds.io.write(Cpu::ARM7, b + 12, 32, len);
    nds.io.write(Cpu::ARM7, b, 32, cnt);
  }
  std::vector<s16> run(size_t frames) {          // mixes `frames` output samples
    std::vector<s16> out(frames * 2);
    for (size_t done = 0; done < frames;) {         // in ring-sized chunks
      const size_t n = std::min<size_t>(frames - done, 4096);
      nds.sched.run_until(nds.sched.now() + spu::Spu::MIX_PERIOD * n);
      nds.spu.run_to(nds.sched.now());              // samples are mixed in batches; take what is due
      const size_t got = nds.spu.take(out.data() + done * 2, n);   // the scheduler may stop a slice short
      CHECK(got > 0);
      done += got;
    }
    return out;
  }
};

// One source sample per output sample.
constexpr u16 TIMER_1X = 0x10000 - 512;
constexpr u32 ON = 0x80000000u;
constexpr u32 PAN_C = 64u << 16;
constexpr u32 LOOP = 1u << 27, ONESHOT = 2u << 27;
constexpr u32 PCM8 = 0u << 29, PCM16 = 1u << 29, ADPCM = 2u << 29, PSG = 3u << 29;

void test_pcm16() {
  Rig r;
  const s16 src[8] = {1000, -2000, 3000, -4000, 5000, -6000, 7000, 32767};
  for (int i = 0; i < 8; ++i) r.w16(RAM + i * 2, static_cast<u16>(src[i]));
  r.chan(0, ON | PCM16 | LOOP | PAN_C | 127, RAM, TIMER_1X, 0, 4);   // 16 bytes, loops
  auto out = r.run(20);
  // Start latency: pos begins at -3, so the first two mixes are silent.
  CHECK(out[0] == 0 && out[1] == 0 && out[2] == 0 && out[3] == 0);
  for (int i = 2; i < 20; ++i) {
    const s32 s = src[(i - 2) & 7];
    const s16 expect = static_cast<s16>(s >> 1);    // centre pan halves each side
    CHECK(out[i * 2] == expect && out[i * 2 + 1] == expect);
  }
  // Channel control reads back (without the write-only fields).
  CHECK(r.nds.io.read(Cpu::ARM7, 0x04000400, 32) == (ON | PCM16 | LOOP | PAN_C | 127));
  CHECK(r.nds.io.read(Cpu::ARM7, 0x04000404, 32) == 0);
  std::puts("pcm16 ok");
}

void test_pcm8_oneshot() {
  Rig r;
  for (int i = 0; i < 16; ++i) r.nds.bus.dma_write16(Cpu::ARM7, RAM + (i & ~1), static_cast<u16>(((i | 1) << 8) | (i & ~1)));
  // samples 0..15 as bytes -> <<8; pan hard left, 1/2 volume divider.
  r.chan(1, ON | PCM8 | ONESHOT | (0u << 16) | (1u << 8) | 127, RAM, TIMER_1X, 0, 4);
  auto out = r.run(24);
  CHECK(out[0] == 0 && out[1] == 0 && out[2] == 0);
  for (int i = 2; i < 18; ++i) {
    const s32 s = (i - 2) << 8;
    CHECK(out[i * 2] == static_cast<s16>(s >> 1) && out[i * 2 + 1] == 0);
  }
  for (int i = 18; i < 24; ++i) CHECK(out[i * 2] == 0);          // stopped after 16 samples
  CHECK(!(r.nds.io.read(Cpu::ARM7, 0x04000410, 32) & ON));    // busy bit cleared
  std::puts("pcm8 one-shot ok");
}

// Independent IMA decoder for the ADPCM check.
s32 ima_decode(std::vector<s16>& out, const u8* data, u32 nibbles) {
  s32 val = static_cast<s16>(data[0] | (data[1] << 8)), idx = (data[2] | (data[3] << 8)) & 0x7F;
  if (idx > 88) idx = 88;
  for (u32 n = 0; n < nibbles; ++n) {
    const u8 nib = (data[4 + n / 2] >> ((n & 1) * 4)) & 0xF;
    const u32 st = spu::Spu::ADPCM_TABLE[idx];
    u32 d = st >> 3; if (nib & 1) d += st >> 2; if (nib & 2) d += st >> 1; if (nib & 4) d += st;
    val = (nib & 8) ? val - static_cast<s32>(d) : val + static_cast<s32>(d);
    if (val > 0x7FFF) val = 0x7FFF;
    if (val < -0x7FFF) val = -0x7FFF;
    idx += spu::Spu::ADPCM_INDEX[nib & 7]; if (idx < 0) idx = 0; if (idx > 88) idx = 88;
    out.push_back(static_cast<s16>(val));
  }
  return val;
}

void test_adpcm() {
  Rig r;
  u8 blob[4 + 32];
  blob[0] = 0x10; blob[1] = 0x00; blob[2] = 20; blob[3] = 0;     // predictor 16, index 20
  u32 seed = 12345;
  for (int i = 4; i < 36; ++i) { seed = seed * 1103515245 + 12345; blob[i] = static_cast<u8>(seed >> 16); }
  for (int i = 0; i < 36; i += 4) r.w32(RAM + i, blob[i] | (blob[i + 1] << 8) | (blob[i + 2] << 16) | (blob[i + 3] << 24));
  std::vector<s16> ref; ima_decode(ref, blob, 64);
  r.chan(2, ON | ADPCM | ONESHOT | (127u << 16) | 127, RAM, TIMER_1X, 0, 9);   // 36 bytes, pan hard right
  auto out = r.run(80);
  // pos runs -2..7 over the first ten mixes (header read at 0); the first nibble decodes at pos 8.
  for (int i = 0; i < 10; ++i) CHECK(out[i * 2 + 1] == 0 && out[i * 2] == 0);
  for (int i = 0; i < 64; ++i) if (out[(10 + i) * 2 + 1] != ref[i]) { std::printf("adpcm mismatch at %d: got %d ref %d (prev got %d ref %d)\n", i, out[(10 + i) * 2 + 1], ref[i], i ? out[(9 + i) * 2 + 1] : 0, i ? ref[i - 1] : 0); for (int k = 0; k < 14; ++k) std::printf("%d ", out[k*2+1]); std::puts(""); }
  for (int i = 0; i < 64; ++i) CHECK(out[(10 + i) * 2 + 1] == ref[i]);
  for (int i = 75; i < 80; ++i) CHECK(out[i * 2 + 1] == 0);
  std::puts("adpcm ok");
}

void test_psg_noise() {
  Rig r;
  r.chan(8, ON | PSG | (3u << 24) | PAN_C | 127, 0, TIMER_1X, 0, 0);   // duty 3: half
  auto out = r.run(16);
  for (int i = 0; i < 16; ++i) {
    // pos starts at -1, so output i corresponds to phase i & 7; duty 3 -> phases 0-3 low, 4-7 high.
    const s16 e = ((i & 7) < 4) ? static_cast<s16>(-0x7FFF >> 1) : static_cast<s16>(0x7FFF >> 1);
    CHECK(out[i * 2] == e);
  }
  Rig n;
  n.chan(14, ON | PSG | PAN_C | 127, 0, TIMER_1X, 0, 0);
  auto no = n.run(40000);
  // 15-bit LFSR: x^15 + x^14 + 1 has period 32767; outputs are full scale.
  for (int i = 0; i < 40000; ++i) CHECK(no[i * 2] == static_cast<s16>(0x7FFF >> 1) || no[i * 2] == static_cast<s16>(-0x7FFF >> 1));
  bool differs = false;
  for (int i = 0; i < 100; ++i) differs |= no[i * 2] != no[(i + 32766) * 2];
  CHECK(differs);
  for (int i = 0; i < 1000; ++i) CHECK(no[i * 2] == no[(i + 32767) * 2]);
  std::puts("psg/noise ok");
}

void test_capture() {
  Rig r;
  const s16 src[8] = {100, -200, 300, -400, 500, -600, 700, -800};
  for (int i = 0; i < 8; ++i) r.w16(RAM + i * 2, static_cast<u16>(src[i]));
  r.chan(1, ON | PCM16 | LOOP | PAN_C | 127, RAM, TIMER_1X, 0, 4);
  // Capture 0: mixer left, 16-bit, loop, 32 bytes at RAM+0x1000.
  r.nds.io.write(Cpu::ARM7, 0x04000510, 32, RAM + 0x1000);
  r.nds.io.write(Cpu::ARM7, 0x04000514, 32, 8);
  r.nds.io.write(Cpu::ARM7, 0x04000508, 8, 0x80);
  r.run(2 + 16);
  // Capture value = mixer left >> 8 = (s * 2048 * 64 >> 10) >> 8 = s >> 1.
  for (int i = 0; i < 16; ++i) {
    const s16 got = static_cast<s16>(r.nds.bus.dma_read16(Cpu::ARM7, RAM + 0x1000 + i * 2));
    const s16 exp = i < 2 ? 0 : static_cast<s16>(src[(i - 2) & 7] >> 1);
    CHECK(got == exp);
  }
  std::puts("capture ok");
}

void test_mute_and_master() {
  Rig r;
  r.w16(RAM, 0x4000);
  r.chan(0, ON | PCM16 | LOOP | PAN_C | 127, RAM, TIMER_1X, 0, 4);
  r.nds.io.write(Cpu::ARM7, 0x04000500, 16, 0x8000 | 64);     // master half
  auto out = r.run(4);
  CHECK(out[2 * 2] == 0x1000);                                 // 0x4000/2 (pan) / 2 (master)
  r.nds.io.write(Cpu::ARM7, 0x04000304, 16, 0);                // POWCNT2 off: silence
  out = r.run(4);
  for (int i = 0; i < 8; ++i) CHECK(out[i] == 0);
  std::puts("mute/master ok");
}

} // namespace

int main() {
  test_pcm16();
  test_pcm8_oneshot();
  test_adpcm();
  test_psg_noise();
  test_capture();
  test_mute_and_master();
  std::puts("spu tests passed");
  return 0;
}
