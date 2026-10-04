// SPDX-License-Identifier: GPL-3.0-or-later
// Texture cache: every format decodes to what a straightforward GBATEK
// decoder says; entries are reused across frames and re-decoded when the
// texels or the palette change.
#include "core/nds.h"
#include "core/gpu/texcache.h"
#include "check.h"

#include <cstdio>
#include <vector>

using namespace ds;

namespace {

// Banks A and B (128 K each) as texture slots 0 and 1 (the compressed
// format keeps its palette info in slot 1), bank E (64 K) as texture palette
// slot 0. Data is written while the banks are LCDC-mapped, then switched.
struct Rig {
  NDS nds;
  std::vector<u8> tex = std::vector<u8>(0x40000), pal = std::vector<u8>(0x10000);
  Rig() {
    nds.reset();
    u32 seed = 0xC0FFEE;
    auto rnd = [&] { seed = seed * 1103515245 + 12345; return static_cast<u8>(seed >> 16); };
    for (auto& b : tex) b = rnd();
    for (auto& b : pal) b = rnd();
    map_lcdc();
    upload();
    map_texture();
  }
  void map_lcdc() { for (u32 r : {0x04000240u, 0x04000241u, 0x04000244u}) nds.io.write(Cpu::ARM9, r, 8, 0x80); }
  void map_texture() { nds.io.write(Cpu::ARM9, 0x04000240, 8, 0x83); nds.io.write(Cpu::ARM9, 0x04000241, 8, 0x8B); nds.io.write(Cpu::ARM9, 0x04000244, 8, 0x83); }
  void upload() {
    for (u32 i = 0; i < tex.size(); i += 4) { u32 v; std::memcpy(&v, &tex[i], 4); nds.bus.dma_write32(Cpu::ARM9, 0x06800000 + i, v); }   // A then B are contiguous in LCDC
    for (u32 i = 0; i < pal.size(); i += 4) { u32 v; std::memcpy(&v, &pal[i], 4); nds.bus.dma_write32(Cpu::ARM9, 0x06880000 + i, v); }
  }
  u8  t8(u32 a) const { return tex[a & 0x3FFFF]; }
  u16 t16(u32 a) const { a &= 0x3FFFF; return static_cast<u16>(tex[a] | (tex[a + 1] << 8)); }
  u16 p16(u32 a) const { a &= 0xFFFF; return static_cast<u16>(pal[a] | (pal[a + 1] << 8)); }

  // Reference decode of one texel, written from GBATEK independently of the
  // renderer's sampler.
  u32 reference(u32 fmt, u32 base, u32 w, u32 s, u32 t, u32 texpal, u32 alpha0) const {
    const u32 i = t * w + s;
    u32 c = 0, a = 31;
    switch (fmt) {
    case 1: { const u8 px = t8(base + i); a = (px >> 5) * 4 + (px >> 5) / 2; c = p16((texpal << 4) + (px & 0x1F) * 2); break; }
    case 2: { const u32 px = (t8(base + i / 4) >> ((s % 4) * 2)) & 3; a = px ? 31 : alpha0; c = p16((texpal << 3) + px * 2); break; }
    case 3: { const u32 px = (t8(base + i / 2) >> ((s % 2) * 4)) & 15; a = px ? 31 : alpha0; c = p16((texpal << 4) + px * 2); break; }
    case 4: { const u32 px = t8(base + i); a = px ? 31 : alpha0; c = p16((texpal << 4) + px * 2); break; }
    case 5: {
      const u32 bx = s / 4, by = t / 4, blocks_per_row = w / 4;
      const u32 block = base + (by * blocks_per_row + bx) * 4;
      const u32 row = t8(block + (t % 4));
      const u32 px = (row >> ((s % 4) * 2)) & 3;
      const u16 info = t16(0x20000 + (block - 0) / 2);          // slot 0 texture -> slot 1 info
      const u32 pb = (texpal << 4) + (info & 0x3FFF) * 4, mode = info >> 14;
      const u16 c0 = p16(pb), c1 = p16(pb + 2);
      auto blend = [&](u32 wa, u32 wb, u32 sh) {
        const u32 r = ((c0 & 0x1F) * wa + (c1 & 0x1F) * wb) >> sh;
        const u32 g = (((c0 >> 5) & 0x1F) * wa + ((c1 >> 5) & 0x1F) * wb) >> sh;
        const u32 b = (((c0 >> 10) & 0x1F) * wa + ((c1 >> 10) & 0x1F) * wb) >> sh;
        return r | (g << 5) | (b << 10);
      };
      if (px == 0) c = c0;
      else if (px == 1) c = c1;
      else if (px == 2) c = mode == 1 ? blend(1, 1, 1) : (mode == 3 ? blend(5, 3, 3) : p16(pb + 4));
      else if (mode == 2) c = p16(pb + 6);
      else if (mode == 3) c = blend(3, 5, 3);
      else { c = 0; a = 0; }
      break;
    }
    case 6: { const u8 px = t8(base + i); a = px >> 3; c = p16((texpal << 4) + (px & 7) * 2); break; }
    default: { const u16 v = t16(base + i * 2); a = (v & 0x8000) ? 31 : 0; c = v; break; }
    }
    return c | (a << 16);
  }
};

void test_formats() {
  Rig r;
  gpu::TextureCache cache;
  cache.begin_frame(1);
  const u32 fmts[] = {1, 2, 3, 4, 5, 6, 7};
  u32 checked = 0;
  for (u32 fmt : fmts) {
    for (u32 alpha0 : {0u, 31u}) {
      const u32 w = 32, h = 16, base = 0x1000 + fmt * 0x400, texpal = 0x20 + fmt;
      const u32* d = cache.lookup(r.nds.bus.vram_map(), fmt, base, w, h, texpal, alpha0);
      CHECK(d != nullptr);
      for (u32 t = 0; t < h; ++t) for (u32 s = 0; s < w; ++s) {
        const u32 got = d[t * w + s], want = r.reference(fmt, base, w, s, t, texpal, alpha0);
        if (got != want) { std::fprintf(stderr, "fmt %u alpha0 %u (%u,%u): got %08x want %08x\n", fmt, alpha0, s, t, got, want); }
        CHECK(got == want); ++checked;
      }
    }
  }
  std::printf("formats ok (%u texels)\n", checked);
}

void test_validation() {
  Rig r;
  gpu::TextureCache cache;
  const u32 fmt = 4, w = 16, h = 16, base = 0x2000, texpal = 0x10;
  cache.begin_frame(1);
  const u32* a = cache.lookup(r.nds.bus.vram_map(), fmt, base, w, h, texpal, 31);
  cache.begin_frame(2);
  const u32* b = cache.lookup(r.nds.bus.vram_map(), fmt, base, w, h, texpal, 31);
  CHECK(a == b);                                    // unchanged source: same entry reused
  const u32 before = b[5];
  // Change one texel byte in VRAM and a palette entry; the next frame must re-decode.
  r.map_lcdc();
  r.tex[base + 5] ^= 0x55; r.pal[(texpal << 4) + r.tex[base + 5] * 2] ^= 0x1234 & 0xFF;
  r.upload();
  r.map_texture();
  cache.begin_frame(3);
  const u32* c = cache.lookup(r.nds.bus.vram_map(), fmt, base, w, h, texpal, 31);
  CHECK(c[5] != before);
  CHECK(c[5] == r.reference(fmt, base, w, 5, 0, texpal, 31));
  // A different palette number is a different entry.
  const u32* d = cache.lookup(r.nds.bus.vram_map(), fmt, base, w, h, texpal + 1, 31);
  CHECK(d != c);
  std::puts("validation ok");
}

// The generation gate: with no remap, or with a remap that touches only
// banks the entry does not read, the bytes are not compared at all -- shown
// by poking the bank storage behind the cache's back, which no guest write
// could do while the bank is in texture mode. A remap that puts the bank in
// a CPU-writable mode makes the next lookup compare and see the poke.
void test_remap_gate() {
  Rig r;
  gpu::TextureCache cache;
  const u32 fmt = 4, w = 8, h = 8, base = 0x100, texpal = 0x20;
  cache.begin_frame(1);
  const u32* a = cache.lookup(r.nds.bus.vram_map(), fmt, base, w, h, texpal, 31);
  const u32 before = a[3];
  u8* bankA = r.nds.bus.vram_bank(0);
  bankA[base + 3] ^= 0x3C;                                   // behind the cache's back
  cache.begin_frame(2);
  CHECK(cache.lookup(r.nds.bus.vram_map(), fmt, base, w, h, texpal, 31)[3] == before);   // no remap: not even compared
  r.nds.io.write(Cpu::ARM9, 0x04000242, 8, 0x80);            // bank C to LCDC: a remap, but not of A / E
  cache.begin_frame(3);
  CHECK(cache.lookup(r.nds.bus.vram_map(), fmt, base, w, h, texpal, 31)[3] == before);   // unrelated remap: still gated
  r.map_lcdc(); r.map_texture();                            // A was writable in between: compared, re-decoded
  cache.begin_frame(4);
  const u32* c = cache.lookup(r.nds.bus.vram_map(), fmt, base, w, h, texpal, 31);
  CHECK(c[3] != before);
  r.tex[base + 3] ^= 0x3C;
  CHECK(c[3] == r.reference(fmt, base, w, 3, 0, texpal, 31));
  std::puts("remap gate ok");
}

} // namespace

int main() {
  test_formats();
  test_validation();
  test_remap_gate();
  std::puts("texcache tests passed");
  return 0;
}
