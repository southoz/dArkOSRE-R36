// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// 2D renderer tests: VRAM bank views and a few end-to-end scanlines through
// the register file, compared against hand-computed expectations.
#include "core/nds.h"
#include "core/gpu/vram_map.h"

#include <cstdio>
#include <cstring>
#include <vector>

using namespace ds;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)
#define CHECK_EQ(a, b) do { auto va_ = (a); auto vb_ = (b); if (static_cast<unsigned long long>(va_) != static_cast<unsigned long long>(vb_)) { std::fprintf(stderr, "FAIL %s:%d: %s = %llx, expected %llx\n", __FILE__, __LINE__, #a, (unsigned long long)va_, (unsigned long long)vb_); ++failures; } } while (0)

static void write9(NDS& nds, u32 addr, u32 width, u32 v) { nds.bus.dma_write32(Cpu::ARM9, addr, v); (void)width; }
static void w16(NDS& nds, u32 addr, u16 v) { nds.bus.dma_write16(Cpu::ARM9, addr, v); }

// Render `line` on engine A and return the composite (before master brightness).
static const gpu::Pixel* render_a(NDS& nds, u32 line) {
  nds.gpu.engine[0].apply_pending();   // the writes above went to the journal
  nds.gpu.engine[0].pre_draw(line, false); nds.gpu.engine[0].pre_draw(line, false); nds.gpu.engine[0].pre_draw(line, false);   // enable latches
  nds.gpu.engine[0].render_sprites(line);
  nds.gpu.engine[0].render_line(line);
  return nds.gpu.engine[0].output();
}

static gpu::Pixel rgb18(u16 c) { return ((c & 0x1F) << 1) | ((((c >> 5) & 0x1F) << 1 | (c >> 15)) << 8) | (((c >> 10) & 0x1F) << 1) << 16; }

static void test_vram_views() {
  NDS nds;
  u8 cnt[9] = {};
  u8* banks[9]; for (int i = 0; i < 9; ++i) banks[i] = nds.bus.vram_bank(i);
  gpu::VramMap m;

  // B -> BG-A slot 1, F -> BG-A at 0x10000 (mirrors at +0x8000), E -> OBJ-A, C -> LCDC.
  cnt[1] = 0x89; cnt[5] = 0x91; cnt[4] = 0x82; cnt[2] = 0x80;
  m.rebuild(cnt, banks);
  CHECK_EQ(m.abg.mask[0], 0u); CHECK_EQ(m.abg.mask[8], 2u); CHECK_EQ(m.abg.mask[4], 0x20u); CHECK_EQ(m.abg.mask[6], 0x20u);
  CHECK(m.abg.ptr[0] == nullptr); CHECK(m.abg.ptr[9] == banks[1] + 0x4000); CHECK(m.abg.ptr[6] == banks[5]);
  CHECK_EQ(m.aobj.mask[0], 0x10u); CHECK(m.aobj.ptr[3] == banks[4] + 0xC000);
  CHECK_EQ(m.lcdc_mask, 4u);
  // Overlap: G on top of F -> ORed read, no direct pointer, write hits both.
  cnt[6] = 0x91; m.rebuild(cnt, banks);
  CHECK(m.abg.ptr[4] == nullptr); CHECK_EQ(m.abg.mask[4], 0x60u);
  banks[5][0x10] = 0x0F; banks[6][0x10] = 0xF0;
  CHECK_EQ(m.read8(m.abg, 0x10010), 0xFFu);
  m.write16(m.abg, 0x10020, 0x1234);
  CHECK_EQ(banks[5][0x20], 0x34u); CHECK_EQ(banks[6][0x21], 0x12u);
  // I as OBJ-B mirrors every 16 K; H+I as BG-B interleave.
  cnt[8] = 0x82; cnt[7] = 0x81; m.rebuild(cnt, banks);
  CHECK(m.bobj.ptr[5] == banks[8]); CHECK(m.bbg.ptr[1] == banks[7] + 0x4000); CHECK(m.bbg.ptr[6] == nullptr || m.bbg.mask[6] == 0);
  cnt[8] = 0x81; m.rebuild(cnt, banks);
  CHECK(m.bbg.ptr[2] == banks[8]); CHECK(m.bbg.ptr[7] == banks[8]);
}

static void test_text_bg_and_backdrop() {
  NDS nds;
  nds.io.powcnt1 = 0x820F; nds.gpu.set_powcnt(0x820F);
  // Bank A as BG-A; 16-colour text BG0, tiles at 0x4000, map at 0.
  nds.bus.dma_write16(Cpu::ARM9, 0x04000240, 0x0081);       // VRAMCNT_A = BG-A (byte write path via 16-bit)
  write9(nds, 0x04000000, 32, 0x00010100);                   // mode 0, BG0 on, display mode 1
  w16(nds, 0x04000008, 0x0004);                              // BG0CNT: char base 1 (0x4000), map base 0, 16 colour
  w16(nds, 0x05000000, 0x7C00);                              // backdrop: blue
  w16(nds, 0x05000002, 0x001F);                              // pal 0 idx 1: red
  w16(nds, 0x05000022, 0x03E0 | 0x8000);                     // pal 1 idx 1: green (bit 15 -> low green bit)
  // Tile 1: left half index 1, right half index 0, every row.
  for (u32 y = 0; y < 8; ++y) write9(nds, 0x06004020 + y * 4, 32, 0x00001111);
  // Map: tile 1 pal 0 at (0,0); tile 1 pal 1 hflipped at (1,0).
  w16(nds, 0x06000000, 0x0001);
  w16(nds, 0x06000002, 0x0001 | (1 << 10) | (1 << 12));
  const gpu::Pixel* out = render_a(nds, 0);
  CHECK_EQ(out[0] & 0xFFFFFF, rgb18(0x001F));                // red
  CHECK_EQ(out[4] & 0xFFFFFF, rgb18(0x7C00));                // transparent -> backdrop
  CHECK_EQ(out[8] & 0xFFFFFF, rgb18(0x7C00));                // flipped: left half transparent
  CHECK_EQ(out[12] & 0xFFFFFF, rgb18(0x83E0));               // green with bit 15 -> 6-bit green 0x3F
  CHECK_EQ((out[12] >> 8) & 0xFF, 0x3Fu);
  CHECK_EQ(out[16] & 0xFFFFFF, rgb18(0x7C00));               // tile 0 (empty)
  // Scroll by 4: pixel 0 now shows tile column 4 (transparent).
  w16(nds, 0x04000010, 4);
  out = render_a(nds, 0);
  CHECK_EQ(out[0] & 0xFFFFFF, rgb18(0x7C00));
  CHECK_EQ(out[4] & 0xFFFFFF, rgb18(0x7C00));
  CHECK_EQ(out[8] & 0xFFFFFF, rgb18(0x83E0));
  // Brightness down on BG0 by 8/16.
  w16(nds, 0x04000050, 0x00C1);                              // effect 3, target BG0
  w16(nds, 0x04000054, 8);
  w16(nds, 0x04000010, 0);
  out = render_a(nds, 0);
  CHECK_EQ(out[0] & 0xFF, 0x3Eu - ((0x3Eu * 8 + 7) >> 4));   // red channel darkened
  CHECK_EQ(out[4] & 0xFFFFFF, rgb18(0x7C00));                // backdrop not a target
}

static void test_sprites_and_window() {
  NDS nds;
  nds.io.powcnt1 = 0x820F; nds.gpu.set_powcnt(0x820F);
  nds.bus.dma_write16(Cpu::ARM9, 0x04000240, 0x0082);       // VRAMCNT_A = OBJ-A
  write9(nds, 0x04000000, 32, 0x00011010);                   // mode 0, OBJ on, 1D mapping, display mode 1
  w16(nds, 0x05000000, 0x0000);                              // backdrop black
  w16(nds, 0x05000202, 0x7FFF);                              // OBJ pal 0 idx 1: white
  for (u32 i = 0; i < 32; i += 4) write9(nds, 0x06400020 + i, 32, 0x11111111);   // tile 1: solid index 1
  // Sprite 0: 8x8 at (10, 0), tile 1, priority 0.
  w16(nds, 0x07000000, 0x0000); w16(nds, 0x07000002, 10); w16(nds, 0x07000004, 0x0001);
  // Sprite 1: same but semi-transparent at (30, 0), priority 0.
  w16(nds, 0x07000008, 0x0400); w16(nds, 0x0700000A, 30); w16(nds, 0x0700000C, 0x0001);
  for (int n = 2; n < 128; ++n) w16(nds, 0x07000000 + n * 8, 0x0200);   // the rest disabled
  const gpu::Pixel* out = render_a(nds, 3);
  CHECK_EQ(out[9] & 0xFFFFFF, 0u);
  CHECK_EQ(out[10] & 0xFFFFFF, rgb18(0x7FFF));
  CHECK_EQ(out[17] & 0xFFFFFF, rgb18(0x7FFF));
  CHECK_EQ(out[18] & 0xFFFFFF, 0u);
  // Semi-transparent sprite over a backdrop that is a second target blends 50/50.
  w16(nds, 0x04000050, 0x2000);                              // backdrop as 2nd target, no effect selected
  w16(nds, 0x04000052, 0x0808);
  out = render_a(nds, 3);
  CHECK_EQ(out[30] & 0xFF, (0x3Eu * 8 + 8) >> 4);
  CHECK_EQ(out[10] & 0xFFFFFF, rgb18(0x7FFF));              // normal sprite unaffected
  // Window 0 covering x 0..19, OBJ only inside, nothing outside.
  w16(nds, 0x04000040, (0 << 8) | 20);                       // x1 = 0, x2 = 20
  w16(nds, 0x04000044, (0 << 8) | 192);
  w16(nds, 0x04000048, 0x0010);                              // WININ: OBJ
  w16(nds, 0x0400004A, 0x0000);                              // WINOUT: nothing
  write9(nds, 0x04000000, 32, 0x00011010 | (1 << 13));
  nds.gpu.engine[0].apply_pending();
  for (u32 l = 0; l <= 3; ++l) nds.gpu.engine[0].update_windows(l);   // the y1 edge at line 0 arms the window
  out = render_a(nds, 3);
  CHECK_EQ(out[10] & 0xFFFFFF, rgb18(0x7FFF));
  CHECK_EQ(out[30] & 0xFFFFFF, 0u);                          // outside window 0: OBJ hidden
  // A wrapping window (x2 < x1) covers the left edge only from the second
  // drawn line on: the x state is carried in from the line before.
  w16(nds, 0x04000040, (200 << 8) | 20);                     // x1 = 200, x2 = 20
  out = render_a(nds, 3);
  CHECK_EQ(out[10] & 0xFFFFFF, 0u);                          // no carry yet: outside
  out = render_a(nds, 3);
  CHECK_EQ(out[10] & 0xFFFFFF, rgb18(0x7FFF));              // carried in from the drawn line
}

static void test_register_access() {
  NDS nds;
  nds.gpu.set_powcnt(0x820F);
  write9(nds, 0x04000000, 32, 0xFFFFFFFF);
  CHECK_EQ(nds.bus.dma_read32(Cpu::ARM9, 0x04000000), 0xFFFFFFFFu);
  write9(nds, 0x04001000, 32, 0xFFFFFFFF);
  CHECK_EQ(nds.bus.dma_read32(Cpu::ARM9, 0x04001000), 0xC0B1FFF7u);   // engine B has fewer bits
  w16(nds, 0x04000054, 0x1F);                                           // BLDY write-only
  CHECK_EQ(nds.bus.dma_read16(Cpu::ARM9, 0x04000054), 0u);
  write9(nds, 0x04000028, 32, 0x08000000);                              // BG2X sign-extends from bit 27
  w16(nds, 0x0400006C, 0xFFFF);
  CHECK_EQ(nds.bus.dma_read16(Cpu::ARM9, 0x0400006C), 0xC01Fu);
  // Powered-down engine ignores everything but DISPCNT.
  nds.gpu.set_powcnt(0x0000);
  w16(nds, 0x04000008, 0x1234);
  CHECK_EQ(nds.bus.dma_read16(Cpu::ARM9, 0x04000008), 0u);
}


// Lazy rendering. A frame driven through the display callbacks, with writes
// landing mid-frame at every point the journal has to distinguish: during a
// line, during its HBlank, into the palette, into VRAM (the write trap), and
// a VRAMCNT remap. The batched frame must equal the per-line frame, and each
// write must show from exactly the line it landed on.
static void run_display_frame(NDS& nds, void (*mid)(NDS&, u32 line, bool hblank)) {
  nds.gpu.begin_frame();                       // line 0 is entered without a scanline event, as run_frame does
  for (u32 l = 0; l < 263; ++l) {
    if (l) nds.gpu.on_scanline_start();
    mid(nds, l, false);
    nds.gpu.on_hblank();
    mid(nds, l, true);
  }
  nds.gpu.quiesce();   // the batched frame is joined at line 0, which this loop never reaches
}
static void lazy_setup(NDS& nds) {
  nds.io.powcnt1 = 0x820F; nds.gpu.set_powcnt(0x820F);     // both engines, A on top
  nds.bus.dma_write16(Cpu::ARM9, 0x04000240, 0x0081);       // bank A as BG-A
  write9(nds, 0x04000000, 32, 0x00010100);                   // mode 0, BG0 on, display mode 1
  w16(nds, 0x04000008, 0x0004);                              // BG0CNT: 16 colour, tiles at 0x4000, map at 0
  w16(nds, 0x05000000, 0x7C00);                              // backdrop blue
  w16(nds, 0x05000002, 0x001F);                              // idx 1 red
  for (u32 y = 0; y < 8; ++y) write9(nds, 0x06004020 + y * 4, 32, 0x11111111);   // tile 1: solid idx 1
  for (u32 ty = 0; ty < 24; ++ty) w16(nds, 0x06000000 + ty * 64, 0x0001);         // tile 1 at column 0 of every row; the rest tile 0 (empty)
}
static void lazy_mid(NDS& nds, u32 line, bool hblank) {
  if (line == 50 && !hblank) w16(nds, 0x05000002, 0x03E0);                       // idx 1 green: from line 50
  if (line == 80 && hblank)  w16(nds, 0x05000000, 0x0000);                       // backdrop black: from line 81
  if (line == 110 && !hblank) w16(nds, 0x04000010, 4);                            // scroll 4: tile 1 covers x 0..3 from line 110
  if (line == 130 && !hblank) write9(nds, 0x06004020 + 2 * 4, 32, 0x00000000);   // tile row 2 cleared through the write trap: line 130 (130 & 7 = 2) sees it, 122 did not
  if (line == 160 && hblank) nds.bus.dma_write16(Cpu::ARM9, 0x04000240, 0x0080);  // bank A to LCDC: BG0 reads nothing from line 161
}
static u32 px(const NDS& nds, u32 line, u32 x) { return nds.gpu.framebuffer(0)[line * 256 + x] & 0xFFFFFF; }
static void test_lazy_journal() {
  NDS lazy, direct;
  lazy.gpu.set_lazy(true); direct.gpu.set_lazy(false);
  for (NDS* n : {&lazy, &direct}) { lazy_setup(*n); run_display_frame(*n, lazy_mid); }
  CHECK(std::memcmp(lazy.gpu.framebuffer(0), direct.gpu.framebuffer(0), 256 * 192 * 4) == 0);
  CHECK(std::memcmp(lazy.gpu.framebuffer(1), direct.gpu.framebuffer(1), 256 * 192 * 4) == 0);
  const u32 red = px(lazy, 2, 0), blue = px(lazy, 2, 8);   // a layer takes two lines to switch on: lines 0-1 are backdrop
  CHECK(red != blue);
  CHECK_EQ(px(lazy, 49, 0), red);
  const u32 green = px(lazy, 50, 0); CHECK(green != red);                    // palette write during line 50
  CHECK_EQ(px(lazy, 80, 8), blue);
  const u32 black = px(lazy, 81, 8); CHECK(black != blue);                   // palette write in HBlank 80
  CHECK_EQ(px(lazy, 109, 6), green);
  CHECK_EQ(px(lazy, 110, 6), black); CHECK_EQ(px(lazy, 110, 2), green);      // scroll from line 110
  CHECK_EQ(px(lazy, 122, 2), green);                                          // tile row 2 before the VRAM write
  CHECK_EQ(px(lazy, 130, 2), black);                                          // ... cleared from line 130
  CHECK_EQ(px(lazy, 131, 2), green);                                          // other rows untouched
  CHECK_EQ(px(lazy, 160, 2), green);
  CHECK_EQ(px(lazy, 161, 2), black);                                          // remap in HBlank 160
}

// The LCD grid on the scanline-scaled path at the RG DS's 2.5x (256x192 ->
// 640x480): every DS pixel's run is 2 or 3 wide/tall; the 3-runs (the ones
// the fractional part widened) have their first column/row dimmed by the
// factor, the 2-runs stay plain, so every lit cell is 2x2 and the seams pair
// DS pixels (0,1), (2,3), ... with no lone pixel at the edges. The scaled
// buffer is checked against a plain-framebuffer run of the same frame.
static u32 dim(u32 c, u32 f) { return (c & 0xFF000000u) | (((c & 0xFF00FFu) * f >> 8) & 0xFF00FFu) | (((c & 0xFF00u) * f >> 8) & 0xFF00u); }
static void test_lcd_grid() {
  const u32 W = 640, H = 480, F = 128;
  NDS ref; lazy_setup(ref); run_display_frame(ref, lazy_mid);
  std::vector<u16> xrun(257);
  for (u32 x = 0; x <= 256; ++x) xrun[x] = static_cast<u16>((x * W + 255) / 256);
  for (u32 grid : {256u, F}) {
    NDS n; lazy_setup(n);
    std::vector<u32> out[2] = {std::vector<u32>(W * H, 0xDEADBEEF), std::vector<u32>(W * H, 0xDEADBEEF)};
    for (int i = 0; i < 2; ++i) n.gpu.set_scale_target(i, gpu::Gpu::ScaleTarget{out[i].data(), W, H, xrun.data(), grid});
    run_display_frame(n, lazy_mid);
    int bad = 0;
    for (u32 line = 0; line < 192 && bad < 8; ++line) {
      const u32 y0 = (line * H + 191) / 192, y1 = ((line + 1) * H + 191) / 192;
      for (u32 sx = 0; sx < 256; ++sx) {
        const u32 want = ref.gpu.framebuffer(0)[line * 256 + sx];
        for (u32 y = y0; y < y1; ++y)
          for (u32 x = xrun[sx]; x < xrun[sx + 1]; ++x) {
            const bool seam = grid < 256 && ((xrun[sx + 1] - xrun[sx] == 3 && x == xrun[sx]) || (y1 - y0 == 3 && y == y0));
            const u32 exp = seam ? dim(want, grid) : want;
            if (out[0][y * W + x] != exp) { if (bad < 8) std::fprintf(stderr, "grid %u: (%u,%u) = %08x, expected %08x\n", grid, x, y, out[0][y * W + x], exp); ++bad; }
          }
      }
    }
    CHECK_EQ(bad, 0);
  }
  // Box-filter seams at 2.5x: the last column of a 3-run straddles the pixel
  // and its right neighbour half and half; the last row of a 3-row span the
  // line and the next; the corner both. Line 191 has no next line (its
  // boundary is integer anyway) and pixel 255 no right neighbour.
  {
    auto avg = [](u32 a, u32 b) { u32 r = 0; for (u32 sh : {0u, 8u, 16u, 24u}) r |= (((((a >> sh) & 255) * 128 + ((b >> sh) & 255) * 128) + 128) >> 8) << sh; return r; };
    std::vector<u8> sw(256, 0);
    for (u32 s = 0; s + 1 < 256; ++s) { const u32 f = ((s + 1) * W) % 256; if (f) sw[s] = static_cast<u8>(f * 256 / 256); }
    NDS n; lazy_setup(n);
    std::vector<u32> out(W * H, 0xDEADBEEF), other(W * H);
    n.gpu.set_scale_target(0, gpu::Gpu::ScaleTarget{out.data(), W, H, xrun.data(), 256, 0, 0, 1, sw.data()});
    n.gpu.set_scale_target(1, gpu::Gpu::ScaleTarget{other.data(), W, H, xrun.data(), 256, 0, 0, 1, sw.data()});
    run_display_frame(n, lazy_mid);
    const u32* fb = ref.gpu.framebuffer(0);
    int bad = 0;
    for (u32 line = 0; line < 192 && bad < 8; ++line) {
      const u32 y0 = (line * H + 191) / 192, y1 = ((line + 1) * H + 191) / 192;
      const bool srow = (((line + 1) * H) % 192) != 0 && line + 1 < 192;
      for (u32 sx = 0; sx < 256; ++sx) {
        const u32 c = fb[line * 256 + sx], r = fb[line * 256 + (sx + 1 < 256 ? sx + 1 : sx)];
        const bool scol = sw[sx] != 0;
        for (u32 y = y0; y < y1; ++y) for (u32 x = xrun[sx]; x < xrun[sx + 1]; ++x) {
          u32 exp;
          if (srow && y == y1 - 1) {
            const u32 dn = fb[(line + 1) * 256 + sx], dnr = fb[(line + 1) * 256 + (sx + 1 < 256 ? sx + 1 : sx)];
            const u32 m = avg(c, dn), mr = avg(r, dnr);
            exp = (scol && x + 1 == xrun[sx + 1]) ? avg(m, mr) : m;
          } else exp = (scol && x + 1 == xrun[sx + 1]) ? avg(c, r) : c;
          if (out[y * W + x] != exp) { if (bad < 8) std::fprintf(stderr, "blend (%u,%u) = %08x, expected %08x\n", x, y, out[y * W + x], exp); ++bad; }
        }
      }
    }
    CHECK_EQ(bad, 0);
  }
  // Seam direction at 3.75x (960x720), where the fractions are not halves:
  // run 0 is panel columns [0, 4) and the boundary to pixel 1 falls at 3.75,
  // so column 3 is three quarters pixel 0 and one quarter pixel 1; rows the
  // same with lines. The weight table stores pixel s+1's share, 1 - frac.
  // And below 1x (213x160, the PiP inset on a 640x480 panel) seams are off
  // and every row is written: the blend path used to leave the straddling
  // rows of lines whose successor was dropped unwritten.
  {
    auto lerp = [](u32 a, u32 b, u32 f) { u32 r = 0; for (u32 sh : {0u, 8u, 16u, 24u}) r |= (((((a >> sh) & 255) * (256 - f) + ((b >> sh) & 255) * f) + 128) >> 8) << sh; return r; };
    auto table = [](u32 w, std::vector<u16>& xr, std::vector<u8>& sw) {
      xr.assign(257, 0); sw.assign(256, 0);
      for (u32 x = 0; x <= 256; ++x) xr[x] = static_cast<u16>((x * w + 255) / 256);
      for (u32 s = 0; s + 1 < 256; ++s) { const u32 f = ((s + 1) * w) % 256; if (f && xr[s + 1] > xr[s]) sw[s] = static_cast<u8>(256 - f * 256 / 256); }
    };
    const u32 red = 0xFFFF0000, blue = 0xFF0000FF;
    std::vector<u32> img(256 * 192, 0xFF00FF00);
    for (u32 y = 0; y < 192; ++y) { img[y * 256 + 0] = y == 0 ? red : blue; img[y * 256 + 1] = blue; }
    std::vector<u16> xr; std::vector<u8> sw;
    table(960, xr, sw);
    NDS n; lazy_setup(n);
    std::vector<u32> out(960 * 720, 0xDEADBEEF);
    n.gpu.set_scale_target(0, gpu::Gpu::ScaleTarget{out.data(), 960, 720, xr.data(), 256, 0, 0, 1, sw.data()});
    n.gpu.scale_image(0, img.data());
    CHECK_EQ(out[0], red);                          // column 0, row 0: crisp
    CHECK_EQ(out[3], lerp(red, blue, 64));          // column 3 straddles pixels 0|1 at 0.75
    CHECK_EQ(out[3 * 960], lerp(red, blue, 64));    // row 3 straddles lines 0|1 at 0.75
    CHECK_EQ(out[3 * 960 + 3], lerp(lerp(red, blue, 64), blue, 64));   // the corner: both
    table(213, xr, sw);
    std::vector<u32> small(213 * 160, 0xDEADBEEF);
    n.gpu.set_scale_target(0, gpu::Gpu::ScaleTarget{small.data(), 213, 160, xr.data(), 256, 0, 0, 1, sw.data()});
    n.gpu.scale_image(0, img.data());
    size_t unwritten = 0; for (u32 v : small) unwritten += v == 0xDEADBEEF;
    CHECK_EQ(unwritten, static_cast<size_t>(0));
    // ... and with a grid strength passed alongside blend (the frontend
    // always passes it), the small view is plain nearest, not gridded.
    std::fill(small.begin(), small.end(), 0xDEADBEEF);
    n.gpu.set_scale_target(0, gpu::Gpu::ScaleTarget{small.data(), 213, 160, xr.data(), 128, 0, 0, 1, sw.data()});
    n.gpu.scale_image(0, img.data());
    size_t dimmed = 0; for (u32 v : small) dimmed += v != 0xFF00FF00 && v != red && v != blue;
    CHECK_EQ(dimmed, static_cast<size_t>(0));
  }
  // Cell chunky at 2.5x with 4-px cells (160x120): each cell the 2D box of
  // the DS pixels it covers (1.6 per axis), weights from build_cell_axis;
  // drawn as a seam row/column and 3x3 of the colour.
  {
    gpu::Gpu::CellMap m;
    CHECK(gpu::Gpu::build_cell_axis(256, 160, 4, m.x)); CHECK(gpu::Gpu::build_cell_axis(192, 120, 4, m.y));
    std::vector<u16> xc(257);
    for (u32 x = 0; x <= 256; ++x) xc[x] = static_cast<u16>(std::min<u32>(x, 160) * 4);
    NDS n; lazy_setup(n);
    std::vector<u32> out(W * H, 0xDEADBEEF), other(W * H);
    n.gpu.set_scale_target(0, gpu::Gpu::ScaleTarget{out.data(), W, H, xc.data(), F, 2, 0, 0, nullptr, &m});
    n.gpu.set_scale_target(1, gpu::Gpu::ScaleTarget{other.data(), W, H, xc.data(), F, 2, 0, 0, nullptr, &m});
    run_display_frame(n, lazy_mid);
    const u32* fb = ref.gpu.framebuffer(0);
    int bad = 0;
    for (u32 j = 0; j < 120 && bad < 8; ++j) for (u32 i = 0; i < 160; ++i) {
      u32 acc[3] = {0, 0, 0};
      for (u32 v = 0; v < m.y.n[j]; ++v) for (u32 u = 0; u < m.x.n[i]; ++u) {
        const u32 c = fb[(m.y.first[j] + v) * 256 + m.x.first[i] + u], wt = m.x.w[i * gpu::Gpu::CELL_TAPS + u] * m.y.w[j * gpu::Gpu::CELL_TAPS + v];
        for (u32 ch = 0; ch < 3; ++ch) acc[ch] += ((c >> (8 * ch)) & 255) * wt;
      }
      u32 want = 0xFF000000u;
      for (u32 ch = 0; ch < 3; ++ch) want |= ((acc[ch] + 32768) >> 16) << (8 * ch);
      for (u32 y = j * 4; y < j * 4 + 4; ++y) for (u32 x = i * 4; x < i * 4 + 4; ++x) {
        const u32 exp = (x == i * 4 || y == j * 4) ? dim(want, F) : want;
        if (out[y * W + x] != exp) { if (bad < 8) std::fprintf(stderr, "cells (%u,%u) = %08x, expected %08x\n", x, y, out[y * W + x], exp); ++bad; }
      }
    }
    CHECK_EQ(bad, 0);
    // Weights: every cell's taps sum to 256; a 4-px cell on 640 covers 1.6 pixels, so 2 or 3 taps.
    for (u32 i = 0; i < 160; ++i) { u32 sum = 0; for (u32 u = 0; u < m.x.n[i]; ++u) sum += m.x.w[i * gpu::Gpu::CELL_TAPS + u]; CHECK_EQ(sum, 256u); CHECK(m.x.n[i] == 2 || m.x.n[i] == 3); }
  }
  // Chunky at 2.5x: pair-merged xrun, line pairs; every 5x5 cell is a leading
  // seam column/row and 4x4 of the block's colour: top-left pixel, mean, or
  // dominant (mean when all four differ).
  auto mean4 = [](u32 a, u32 b, u32 c, u32 d) {
    u32 r = 0;
    for (u32 sh : {0u, 8u, 16u}) r |= ((((a >> sh) & 255) + ((b >> sh) & 255) + ((c >> sh) & 255) + ((d >> sh) & 255) + 2) >> 2) << sh;
    return 0xFF000000u | r;
  };
  auto luma = [](u32 c) { return ((c >> 16) & 255) * 77 + ((c >> 8) & 255) * 150 + (c & 255) * 29; };
  for (u8 ck : {u8(1), u8(2), u8(3), u8(4), u8(5), u8(6)}) {
    std::vector<u16> xc = xrun;
    for (u32 x = 1; x < 256; x += 2) xc[x] = xc[x + 1];
    NDS n; lazy_setup(n);
    std::vector<u32> out(W * H, 0xDEADBEEF);
    const u32 thr = 40 * 256;
    n.gpu.set_scale_target(0, gpu::Gpu::ScaleTarget{out.data(), W, H, xc.data(), F, ck, thr});
    std::vector<u32> other(W * H); n.gpu.set_scale_target(1, gpu::Gpu::ScaleTarget{other.data(), W, H, xc.data(), F, ck, thr});
    run_display_frame(n, lazy_mid);
    int bad = 0;
    for (u32 by = 0; by < 96 && bad < 8; ++by) for (u32 bx = 0; bx < 128; ++bx) {
      const u32* fb = ref.gpu.framebuffer(0);
      const u32 a = fb[(2 * by) * 256 + 2 * bx], b = fb[(2 * by) * 256 + 2 * bx + 1], c = fb[(2 * by + 1) * 256 + 2 * bx], d = fb[(2 * by + 1) * 256 + 2 * bx + 1];
      u32 want = a;
      if (ck == 2) want = mean4(a, b, c, d);
      if (ck == 3) want = (a == b || a == c || a == d) ? a : (b == c || b == d) ? b : (c == d) ? c : mean4(a, b, c, d);
      u32 lo = a, hi = a;
      for (u32 p : {b, c, d}) { if (luma(p) < luma(lo)) lo = p; if (luma(p) > luma(hi)) hi = p; }
      if (ck == 4) want = lo;
      if (ck == 5) want = hi;
      if (ck == 6) { const u32 m = (luma(a) + luma(b) + luma(c) + luma(d)) / 4, dl = m - luma(lo), dh = luma(hi) - m; want = (dl <= thr && dh <= thr) ? mean4(a, b, c, d) : dl > dh ? lo : dh > dl ? hi : mean4(a, b, c, d); }
      for (u32 y = by * 5; y < by * 5 + 5; ++y) for (u32 x = bx * 5; x < bx * 5 + 5; ++x) {
        const u32 exp = (x == bx * 5 || y == by * 5) ? dim(want, F) : want;
        if (out[y * W + x] != exp) { if (bad < 8) std::fprintf(stderr, "chunky %u (%u,%u) = %08x, expected %08x\n", ck, x, y, out[y * W + x], exp); ++bad; }
      }
    }
    CHECK_EQ(bad, 0);
  }
  // Seam geometry at 2.5x: runs of 2 and 3 alternate, never 1 (which would be left undimmed).
  CHECK_EQ(xrun[1] - xrun[0], 3u); CHECK_EQ(xrun[2] - xrun[1], 2u); CHECK_EQ(xrun[256], W);
}

// Bilinear (--linear): every panel pixel against the sample-point
// definition: source u = (x+0.5)*256/W - 0.5, v = (y+0.5)*192/H - 0.5,
// clamped to the picture, the 2x2 around (u,v) blended by the fractional
// parts, horizontally first (each pass rounded as the kernels do). An
// integer scale and a fractional, non-multiple one.
static void test_bilinear() {
  NDS ref; lazy_setup(ref); run_display_frame(ref, lazy_mid);
  const u32* fb = ref.gpu.framebuffer(0);
  auto lerp = [](u32 a, u32 b, u32 f) { u32 r = 0; for (u32 sh : {0u, 8u, 16u, 24u}) r |= ((((a >> sh) & 255) * (256 - f) + ((b >> sh) & 255) * f + 128) >> 8) << sh; return r; };
  for (const auto& [W, H] : {std::pair<u32, u32>{640, 480}, std::pair<u32, u32>{721, 541}, std::pair<u32, u32>{256, 192}}) {
    std::vector<u16> xrun(257), sx(W); std::vector<u8> wx(W);
    for (u32 x = 0; x <= 256; ++x) xrun[x] = static_cast<u16>((x * W + 255) / 256);
    for (u32 x = 0; x < W; ++x) {
      const s32 u = static_cast<s32>(((2 * x + 1) * 256 * 128) / W) - 128;
      u32 s = u <= 0 ? 0 : static_cast<u32>(u) >> 8, f = u <= 0 ? 0 : static_cast<u32>(u) & 255;
      if (s >= 255) { s = 254; f = 255; }
      sx[x] = static_cast<u16>(s); wx[x] = static_cast<u8>(f);
    }
    NDS n; lazy_setup(n);
    std::vector<u32> out(W * H, 0xDEADBEEF), other(W * H);
    gpu::Gpu::ScaleTarget t{out.data(), W, H, xrun.data()};
    t.bilinear = true; t.lin_sx = sx.data(); t.lin_wx = wx.data();
    n.gpu.set_scale_target(0, t);
    t.px = other.data();
    n.gpu.set_scale_target(1, t);
    run_display_frame(n, lazy_mid);
    int bad = 0;
    for (u32 y = 0; y < H && bad < 8; ++y) {
      // Row sample: v*256 = ((2y+1)*192*128)/H - 128, floored.
      const s32 v = static_cast<s32>(((2 * y + 1) * 192 * 128) / H) - 128;
      u32 l0, fy;
      if (v <= 0) { l0 = 0; fy = 0; }
      else { l0 = static_cast<u32>(v) >> 8; fy = static_cast<u32>(v) & 255; if (l0 >= 191) { l0 = 191; fy = 0; } }
      const u32 l1 = l0 + (fy ? 1 : 0);
      for (u32 x = 0; x < W; ++x) {
        const u32 a = lerp(fb[l0 * 256 + sx[x]], fb[l0 * 256 + sx[x] + 1], wx[x]);
        const u32 b = lerp(fb[l1 * 256 + sx[x]], fb[l1 * 256 + sx[x] + 1], wx[x]);
        const u32 exp = fy ? lerp(a, b, fy) : a;
        if (out[y * W + x] != exp) { if (bad < 8) std::fprintf(stderr, "bilinear %ux%u (%u,%u) = %08x, expected %08x\n", W, H, x, y, out[y * W + x], exp); ++bad; }
      }
    }
    CHECK_EQ(bad, 0);
  }
}

int main() {
  test_vram_views();
  test_text_bg_and_backdrop();
  test_sprites_and_window();
  test_register_access();
  test_lazy_journal();
  test_lcd_grid();
  test_bilinear();
  if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
  std::puts("gpu: ok");
  return 0;
}
