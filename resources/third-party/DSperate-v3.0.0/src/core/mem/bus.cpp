// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/mem/bus.h"
#include "core/cpu/cp15.h"
#include "core/state/state.h"
#include "core/profile.h"
#include "core/nds.h"
#if DSPERATE_JIT
#include "core/cpu/jit/jit.h"
#endif

#include <cassert>
#include <cstdio>
#include <cstring>
#include <algorithm>

namespace ds::mem {

constexpr u32 Bus::VRAM_BANK_SIZES[9];

// Debug watchpoint (DS_WATCH=<hex addr>): the 2 KB page holding the address
// is taken out of both page tables so accesses come through here.
static u32 watch_addr = 0; static bool watch_on = false; static u32 watch_hits = 0;
static u8* watch_host[2] = {nullptr, nullptr};   // watched page's host bytes per CPU

static size_t arena_bytes() {
  auto r = [](size_t n) { return (n + GuestView::HOST_PAGE - 1) & ~size_t{GuestView::HOST_PAGE - 1}; };
  return r(Bus::MAIN_RAM_SIZE_DSI) + r(Bus::SHARED_WRAM_SIZE) + r(Bus::ARM7_WRAM_SIZE) + r(Bus::ITCM_SIZE) + r(Bus::DTCM_SIZE) + r(Bus::VRAM_TOTAL) +
         r(Bus::PALETTE_SIZE) + r(Bus::OAM_SIZE) + r(Bus::BIOS9_SIZE) + r(Bus::BIOS7_SIZE) + 3 * r(Bus::NWRAM_BANK_SIZE) + r(Bus::BIOS9I_SIZE) +
         r(Bus::BIOS7I_SIZE);
}

PageBuf Bus::take_buf(size_t bytes) {
  if (arena_)
    if (u8* p = arena_->take(bytes)) return PageBuf(p, PageBufFree{false});
  return alloc_page_buf(bytes);
}

Bus::Bus(NDS& nds)
    : arena_(fastmem_requested() ? HostArena::create(arena_bytes()) : nullptr),
      main_ram(take_buf(MAIN_RAM_SIZE_DSI)), shared_wram(take_buf(SHARED_WRAM_SIZE)),
      arm7_wram(take_buf(ARM7_WRAM_SIZE)), itcm(take_buf(ITCM_SIZE)),
      dtcm(take_buf(DTCM_SIZE)), vram(take_buf(VRAM_TOTAL)), palette(take_buf(PALETTE_SIZE)),
      oam(take_buf(OAM_SIZE)), bios9(take_buf(BIOS9_SIZE)), bios7(take_buf(BIOS7_SIZE)),
      nwram{take_buf(NWRAM_BANK_SIZE), take_buf(NWRAM_BANK_SIZE), take_buf(NWRAM_BANK_SIZE)},
      bios9i(take_buf(BIOS9I_SIZE)), bios7i(take_buf(BIOS7I_SIZE)), nds_(nds) {
  if (fastmem_requested() && !arena_) std::fprintf(stderr, "fastmem: no shared-memory backing (memfd or tmpfs); off\n");
  std::memset(bios9.get(), 0, BIOS9_SIZE);
  std::memset(bios7.get(), 0, BIOS7_SIZE);
}

u32 Bus::main_ram_size() const { return nds_.dsi ? MAIN_RAM_SIZE_DSI : MAIN_RAM_SIZE; }

Bus::~Bus() {
  for (int c = 0; c < 2; ++c)
    if (views_[c]) nds_.cpu(c == 0 ? Cpu::ARM9 : Cpu::ARM7).page_table.attach_view(nullptr);
}

void Bus::fastmem_flush() {
  for (auto& v : views_) if (v && v->pending()) v->flush();
}

bool Bus::fastmem_verify(u64 frame) {
  for (int c = 0; c < 2; ++c) {
    if (!views_[c]) continue;
    std::string why;
    if (!views_[c]->verify(&why)) {
      std::fprintf(stderr, "fastmem verify: frame %llu arm%d: %s\n", static_cast<unsigned long long>(frame), c ? 7 : 9, why.c_str());
      return false;
    }
  }
  return true;
}

void Bus::fastmem_report() const {
  if (!arena_) return;
  for (int c = 0; c < 2; ++c) {
    if (!views_[c]) continue;
    const GuestView::Stats& s = views_[c]->stats();
    std::fprintf(stderr, "fastmem: arm%d view at %p (%s): %llu flushes, %llu mmap calls, %llu pages laid, %llu grants, %llu volatile refusals, %.1f ms flushing\n", c ? 7 : 9,
                 static_cast<void*>(views_[c]->base()), arena_->kind(), static_cast<unsigned long long>(s.flushes),
                 static_cast<unsigned long long>(s.map_calls), static_cast<unsigned long long>(s.pages_laid), static_cast<unsigned long long>(s.grants),
                 static_cast<unsigned long long>(s.volatile_refusals), s.flush_ns / 1e6);
  }
}

u8* Bus::vram_bank(int i) {
  u32 off = 0;
  for (int k = 0; k < i; ++k) off += VRAM_BANK_SIZES[k];
  return vram.get() + off;
}

void Bus::reset() {
  std::memset(main_ram.get(), 0, main_ram_size());
  for (auto& b : nwram) std::memset(b.get(), 0, NWRAM_BANK_SIZE);
  std::memset(nwram_map_, 0, sizeof nwram_map_);
  wram_key_[0] = wram_key_[1] = ~0u;   // next apply lays everything
  timing_.clock9_shift = nds_.dsi ? 2 : 1;   // SCFG_CLK9 bit 0 set at DSi reset
  std::memset(shared_wram.get(), 0, SHARED_WRAM_SIZE);
  std::memset(arm7_wram.get(), 0, ARM7_WRAM_SIZE);
  std::memset(itcm.get(), 0, ITCM_SIZE);
  std::memset(dtcm.get(), 0, DTCM_SIZE);
  std::memset(vram.get(), 0, VRAM_TOTAL);
  std::memset(palette.get(), 0, PALETTE_SIZE);
  std::memset(oam.get(), 0, OAM_SIZE);
  timing_.reset();
  if (nds_.dsi) {
    timing_.set_region9(0x0C000000, 0x0D000000, REGION_MAIN_RAM, 16, 8, 1);   // uncached main-RAM alias
    timing_.set_region7(0x0C000000, 0x0D000000, REGION_MAIN_RAM, 16, 8, 1);
  }
  vram_hosts_valid_ = false;
  nds_.cpu(Cpu::ARM9).timing9 = timing_.cpu9();
  nds_.cpu(Cpu::ARM9).timing7 = timing_.cpu7();
  nds_.cpu(Cpu::ARM9).cost7 = timing_.cost7();
  nds_.cpu(Cpu::ARM7).timing9 = timing_.cpu9();
  nds_.cpu(Cpu::ARM7).timing7 = timing_.cpu7();
  nds_.cpu(Cpu::ARM7).cost7 = timing_.cost7();
  map_fixed_regions();
  update_nwram();
  update_vram();
  update_tcm(nds_.cpu(Cpu::ARM9), true);
  if (nds_.dsi) update_vram_timings();
  gba_slot_applied_ = -1;
  update_gba_slot_timings();
  if (arena_) {
    for (int c = 0; c < 2; ++c) {
      if (views_[c]) continue;
      PageTable& pt = nds_.cpu(c == 0 ? Cpu::ARM9 : Cpu::ARM7).page_table;
      views_[c] = GuestView::create(*arena_, pt);
      if (views_[c]) pt.attach_view(views_[c].get());
      else std::fprintf(stderr, "fastmem: cannot reserve the arm%d view; off for it\n", c ? 7 : 9);
    }
  }
}

void Bus::update_wifi_timings() {
  if (nds_.io.powcnt2 & 0x0002) {
    static const int ntimings[4] = {10, 8, 6, 18};
    const u16 v = nds_.io.wifiwaitcnt;
    timing_.set_region7(0x04800000, 0x04808000, REGION_WIFI0, 16, ntimings[v & 3], (v & 0x04) ? 4 : 6);
    timing_.set_region7(0x04808000, 0x04810000, REGION_WIFI1, 16, ntimings[(v >> 3) & 3], (v & 0x20) ? 4 : 10);
  } else {
    timing_.set_region7(0x04800000, 0x04808000, REGION_WIFI0, 32, 1, 1);
    timing_.set_region7(0x04808000, 0x04810000, REGION_WIFI1, 32, 1, 1);
  }
}

void Bus::update_gba_slot_timings() {
  // Only bits 0-4 and 7 reach the slot timings (5-6 are the PHI output).
  const u16 ex = nds_.io.exmemcnt & 0x9F;
  if (gba_slot_applied_ == ex) return;
  gba_slot_applied_ = ex;
  prof::add(prof::C_BUS_GBA_TIMING, 1);
  static const bool debug = std::getenv("DS_DEBUG_TIMING") != nullptr;
  if (debug) std::fprintf(stderr, "[timing] exmemcnt %04x frame %llu\n", nds_.io.exmemcnt, (unsigned long long)nds_.frame_count);
  static const int rom_n[4] = {10, 8, 6, 18};
  const int rn = rom_n[(ex >> 2) & 3], rs = (ex & 0x10) ? 4 : 6;
  static const int ram_n[4] = {10, 8, 6, 18};
  const int ran = ram_n[ex & 3];
  if (ex & 0x80) {          // ARM7 owns the slot
    timing_.set_region9(0x08000000, 0x0A000000, REGION_NONE, 32, 1, 1);
    timing_.set_region9(0x0A000000, 0x0B000000, REGION_NONE, 32, 1, 1);
    timing_.set_region7(0x08000000, 0x0A000000, REGION_GBA_ROM, 16, rn, rs);
    timing_.set_region7(0x0A000000, 0x0B000000, REGION_GBA_RAM, 8, ran, ran);
  } else {
    timing_.set_region9(0x08000000, 0x0A000000, REGION_GBA_ROM, 16, rn, rs);
    timing_.set_region9(0x0A000000, 0x0B000000, REGION_GBA_RAM, 8, ran, ran);
    timing_.set_region7(0x08000000, 0x0A000000, REGION_NONE, 32, 1, 1);
    timing_.set_region7(0x0A000000, 0x0B000000, REGION_NONE, 32, 1, 1);
  }
  timing_.update_cpu9(nds_.cpu(Cpu::ARM9), 0x08000000, 0x0B000000);
}

// Map `host` of `size` bytes at `guest`, mirrored up to `mirror_end`.
void Bus::map_page_aligned(PageTable& pt, u32 guest, u32 size, u8* host, u32 flags, u32 mirror_end) {
  for (u32 a = guest; a < mirror_end; a += size) pt.map(a, size, host, flags);
}

void Bus::map_fixed_regions() {
  PageTable& pt9 = nds_.cpu(Cpu::ARM9).page_table;
  PageTable& pt7 = nds_.cpu(Cpu::ARM7).page_table;
  const u32 RW = PAGE_READABLE | PAGE_WRITABLE, RO = PAGE_READABLE;

  for (PageTable* pt : {&pt9, &pt7}) {
    pt->unmap(0x00000000, 0x10000000);
    map_main_ram(*pt);
    pt->map_mmio(0x04000000, 0x01000000);
  }
  update_bios_map();
  // Palette/OAM read directly but store through the slow path: the 2D
  // engines render lazily from their own copies and need every store
  // journaled (Gpu::palette_store / oam_store).
  map_page_aligned(pt9, 0x05000000, PALETTE_SIZE, palette.get(), RO, 0x06000000);
  map_page_aligned(pt9, 0x07000000, OAM_SIZE, oam.get(), RO, 0x08000000);
  // ARM7 private WRAM (default; WRAMCNT may put shared WRAM in
  // 03000000-037FFFFF, handled in update_wram).
  map_page_aligned(pt7, 0x03800000, ARM7_WRAM_SIZE, arm7_wram.get(), RW, 0x04000000);
  pt9.map_mmio(0x08000000, 0x02000000);   // GBA slot, no cart: open bus via slow path
  pt7.map_mmio(0x08000000, 0x02000000);
}

// DS: 4 MB mirrored over 02000000-02FFFFFF. DSi: also mirrored at 0C000000
// (the uncached alias both CPUs decode).
void Bus::map_main_ram(PageTable& pt) {
  const u32 RW = PAGE_READABLE | PAGE_WRITABLE;
  map_page_aligned(pt, 0x02000000, main_ram_span(), main_ram.get(), RW, 0x03000000);
  if (nds_.dsi) map_page_aligned(pt, 0x0C000000, main_ram_span(), main_ram.get(), RW, 0x0D000000);
}

u32 Bus::main_ram_span() const {
  if (!nds_.dsi) return MAIN_RAM_SIZE;
  return ((nds_.io.dsi.scfg_ext[1] >> 14) & 3) >= 2 ? MAIN_RAM_SIZE_DSI : MAIN_RAM_SIZE;
}

void Bus::update_main_ram() {
  // Rebuild whole so TCM windows over main RAM go back on top.
  update_tcm(nds_.cpu(Cpu::ARM9), true);
  PageTable& pt7 = nds_.cpu(Cpu::ARM7).page_table;
  pt7.unmap(0x02000000, 0x01000000);
  pt7.unmap(0x0C000000, 0x01000000);
  map_main_ram(pt7);
  if (watch_on && watch_host[1]) pt7.map_mmio(watch_addr & ~0x7FFu, 0x800);
#if DSPERATE_JIT
  // A block translated at 02400000+ was built from bytes that address now shows elsewhere.
  for (Cpu c : {Cpu::ARM9, Cpu::ARM7}) jit::flush(nds_.cpu(c));
#endif
}

// DS: 4 KB ARM9 image mirrored over the top 64 KB, 16 KB ARM7 image at 0.
// DSi: 64 KB images unless SCFG_BIOS switched a CPU back to its DS image;
// hidden upper halves read as all ones via the slow path.
void Bus::update_bios_map() {
  PageTable& pt9 = nds_.cpu(Cpu::ARM9).page_table;
  PageTable& pt7 = nds_.cpu(Cpu::ARM7).page_table;
  const u32 RO = PAGE_READABLE;
  pt9.unmap(0xFFFF0000, 0x10000);
  pt7.unmap(0x00000000, 0x10000);
  const u16 scfg_bios = nds_.dsi ? nds_.io.dsi.scfg_bios : 0;
  if (nds_.dsi && !(scfg_bios & 0x0002)) {
    pt9.map(0xFFFF0000, (scfg_bios & 0x0001) ? 0x8000 : 0x10000, bios9i.get(), RO);
  } else {
    map_page_aligned(pt9, 0xFFFF0000, BIOS9_SIZE, bios9.get(), RO, 0xFFFFFFFFu - 0xFFF);
    pt9.map(0xFFFFF000, 0x1000, bios9.get(), RO);
  }
  // DSi ARM7 BIOS: left unmapped so every access goes through io_read, which
  // applies the BIOS protection rules.
  if (!(nds_.dsi && !(scfg_bios & 0x0200))) pt7.map(0x00000000, BIOS7_SIZE, bios7.get(), RO);
}

void Bus::update_vram_timings() {
  const int width = (nds_.dsi && (nds_.io.dsi.scfg_ext[0] & (1u << 13))) ? 32 : 16;
  timing_.set_region9(0x06000000, 0x07000000, REGION_VRAM, width, 1, 1);
  timing_.set_region7(0x06000000, 0x07000000, REGION_VRAM, width, 1, 1);
  timing_.update_cpu9(nds_.cpu(Cpu::ARM9), 0x06000000, 0x07000000);
}

void Bus::set_clock9_shift(u32 shift) {
  if (timing_.clock9_shift == shift) return;
  timing_.clock9_shift = shift;
  timing_.update_cpu9(nds_.cpu(Cpu::ARM9), 0, 0xFFFFFFFF);
  nds_.dma.set_clock9_shift(shift);
  nds_.sched.set_clock9_shift(shift);
}

// The 0x03000000 region is laid out in 16 KB cells (wram_cell_host) and
// applied with PageTable::remap, which touches only entries that change.

// Windows MBK6-8 give CPU `c` over banks A, B and C (end <= start: none).
void Bus::nwram_windows(int c, bool nwram, u32 win[3][2]) const {
  for (int bank = 0; bank < 3; ++bank) win[bank][0] = win[bank][1] = 0;
  if (!nwram || !nds_.dsi) return;
  const io::DsiIo& d = nds_.io.dsi;
  if (!(d.scfg_ext[c] & (1u << 25))) return;
  for (int bank = 0; bank < 3; ++bank) {
    const u32 v = d.mbk[c][5 + bank];
    u32 start, end;
    if (bank == 0) { start = 0x03000000 + (((v >> 4) & 0xFF) << 16); end = 0x03000000 + (((v >> 20) & 0x1FF) << 16); }
    else { start = 0x03000000 + (((v >> 3) & 0x1FF) << 15); end = 0x03000000 + (((v >> 19) & 0x3FF) << 15); }
    win[bank][0] = start; win[bank][1] = std::min(end, 0x04000000u);   // cut at end of region
  }
}

// Host pointer for CPU `c`'s 16 KB cell at `a`: WRAMCNT's view, under the
// windows `win` (A over B over C); nullptr is unmapped.
u8* Bus::wram_cell_host(int c, const u32 win[3][2], u32 a) const {
  if (nds_.dsi) {
    const io::DsiIo& d = nds_.io.dsi;
    for (int bank = 0; bank < 3; ++bank) {
      if (a < win[bank][0] || a >= win[bank][1]) continue;
      const u32 v = d.mbk[c][5 + bank];
      u32 mask;
      if (bank == 0) { static const u32 masks[4] = {0, 0, 1, 3}; mask = masks[(v >> 12) & 3]; }
      else { static const u32 masks[4] = {0, 1, 3, 7}; mask = masks[(v >> 12) & 3]; }
      const u32 shift = bank == 0 ? 16 : 15;
      u8* host = nwram_map_[bank][c][(a >> shift) & mask];   // unbacked: reads 0, writes dropped
      return host ? host + (a & ((1u << shift) - 1)) : nullptr;
    }
  }
  const u32 cnt = nds_.io.wramcnt & 3;
  u8* const half0 = shared_wram.get(), * const half1 = shared_wram.get() + 0x4000;
  if (c == 0) {
    // ARM9: all 32K (0), second 16K (1), first (2), none (3).
    if (cnt == 0) return shared_wram.get() + (a & 0x7FFF);
    return cnt == 3 ? nullptr : cnt == 1 ? half1 : half0;
  }
  if (a >= 0x03800000 || cnt == 0) return arm7_wram.get() + (a & 0xFFFF);
  if (cnt == 3) return shared_wram.get() + (a & 0x7FFF);
  return cnt == 1 ? half0 : half1;
}

// `windows_only`: only MBK1-8 changed, so only cells in the old/new windows
// whose pointer changed are remapped. Otherwise the whole region is redone.
void Bus::apply_wram(bool nwram, bool windows_only) {
  constexpr u32 CELL = 0x4000, PAGES = CELL >> PAGE_SHIFT;
  for (int c = 0; c < 2; ++c) {
    u32 win[3][2];
    nwram_windows(c, nwram, win);
    const u32 key = (nds_.io.wramcnt & 3) | (nwram ? 4 : 0) | (nds_.dsi ? 8 : 0) | (nds_.dsi ? (nds_.io.dsi.scfg_ext[c] >> 25 & 1) << 4 : 0);
    const bool partial = windows_only && wram_key_[c] == key;
    u32 lo = 0x03000000, hi = 0x04000000;
    if (partial) {
      lo = 0x04000000; hi = 0x03000000;
      for (const auto* w : {win, wram_win_[c]})
        for (int bank = 0; bank < 3; ++bank)
          if (w[bank][0] < w[bank][1]) { lo = std::min(lo, w[bank][0]); hi = std::max(hi, w[bank][1]); }
      lo &= ~(CELL - 1); hi = (hi + CELL - 1) & ~(CELL - 1);
    }
    PageTable& pt = nds_.cpu(c ? Cpu::ARM7 : Cpu::ARM9).page_table;
    u8** cells = wram_cells_[c];
    for (u32 a = lo; a < hi; a += CELL) {
      const u32 k = (a - 0x03000000) / CELL;
      u8* host = wram_cell_host(c, win, a);
      if (partial && cells[k] == host) continue;
      cells[k] = host;
      u8* pages[PAGES];
      for (u32 i = 0; i < PAGES; ++i) pages[i] = host ? host + (i << PAGE_SHIFT) : nullptr;
      pt.remap(a, CELL, pages, PAGE_READABLE | PAGE_WRITABLE);
    }
    wram_key_[c] = key;
    std::memcpy(wram_win_[c], win, sizeof win);
  }
}

void Bus::update_wram() {
  apply_wram(false, false);
}

// Each MBK1-5 byte assigns its slot to a CPU and position; a CPU's window
// (MBK6-8) shows the slots at positions (addr >> 16|15) & mask, A over B
// over C where they overlap. Not modelled: two slots at the same position
// both written by a store there; here the one that reads wins.
void Bus::update_nwram(bool windows_only) {
  // Watched page is re-laid with the rest: take it out again after.
  struct Retrap { Bus& b; ~Retrap() { if (!watch_on) return; for (int c = 0; c < 2; ++c) if (watch_host[c]) b.nds_.cpu(c ? Cpu::ARM7 : Cpu::ARM9).page_table.map_mmio(watch_addr & ~0x7FFu, 0x800); } } retrap{*this};
  std::memset(nwram_map_, 0, sizeof nwram_map_);
  if (nds_.dsi) {
    const io::DsiIo& d = nds_.io.dsi;
    if (getenv("DS_DEBUG_MBK"))
      std::fprintf(stderr, "[nwram] rebuild scfg_ext %08x/%08x (bit25 a9=%d a7=%d) t=%llu\n",
                   d.scfg_ext[0], d.scfg_ext[1], (d.scfg_ext[0] >> 25) & 1, (d.scfg_ext[1] >> 25) & 1,
                   (unsigned long long)nds_.sched.now());
    for (int part = 3; part >= 0; --part) {
      const u8 v = static_cast<u8>((d.mbk[0][0] >> (part * 8)) & 0xFD);
      if (v & 0x80) nwram_map_[0][v & 3][(v >> 2) & 3] = nwram[0].get() + (part << 16);
    }
    for (int bank = 1; bank <= 2; ++bank)
      for (int part = 7; part >= 0; --part) {
        u8 v = static_cast<u8>((d.mbk[0][(bank == 1 ? 1 : 3) + (part >> 2)] >> ((part & 3) * 8)) & 0xFF);
        if (!(v & 0x80)) continue;
        if (v & 0x02) v &= 0xFE;
        nwram_map_[bank][v & 3][(v >> 2) & 7] = nwram[bank].get() + (part << 15);
      }
  }
  apply_wram(true, windows_only);
}

void Bus::update_vram() {
  prof::add(prof::C_BUS_UPDATE_VRAM, 1);
  // Band workers read texture/texture-palette VRAM directly and never see it
  // mapped into a CPU's address space, so this is the only place either can
  // move. Rebuilt into a copy first to test whether they actually moved
  // before joining the workers (most VRAMCNT traffic leaves them alone).
  u8* banks[9];
  for (int i = 0; i < 9; ++i) banks[i] = vram_bank(i);
  gpu::VramMap next = vram_map_;
  next.rebuild(nds_.io.vramcnt, banks);
  const auto differs = [](const gpu::VramView& a, const gpu::VramView& b) {
    return a.size != b.size || a.ptr != b.ptr || a.mask != b.mask;
  };
  const bool tex_moved = differs(next.texture, vram_map_.texture) || differs(next.texpal, vram_map_.texpal);
  if (tex_moved) {
    nds_.gpu3d.sync_raster();
    if (prof::enabled && prof::async_window) prof::add(prof::C_ASYNC_VRAMCNT_SWAP, 1);
  }
  PageTable& pt9 = nds_.cpu(Cpu::ARM9).page_table;
  PageTable& pt7 = nds_.cpu(Cpu::ARM7).page_table;
  const u32 RW = PAGE_READABLE | PAGE_WRITABLE;
  // A remap mid-frame changes what the deferred 2D render reads: lines past
  // HBlank are rendered now against the old views, and the write trap
  // (which the remap drops) is re-armed after. Which engine's views moved,
  // per engine's four fetch views plus extended palettes; LCDC counts for
  // engine A. Census only for now: gpu's catch-up stays unconditional.
  const auto view_moved = [&](const gpu::VramView& a, const gpu::VramView& b) { return differs(a, b); };
  const u32 moved_2d =
      ((view_moved(next.abg, vram_map_.abg) || view_moved(next.aobj, vram_map_.aobj) ||
        view_moved(next.abg_extpal, vram_map_.abg_extpal) || view_moved(next.aobj_extpal, vram_map_.aobj_extpal) ||
        next.lcdc_mask != vram_map_.lcdc_mask) ? 1u : 0u) |
      ((view_moved(next.bbg, vram_map_.bbg) || view_moved(next.bobj, vram_map_.bobj) ||
        view_moved(next.bbg_extpal, vram_map_.bbg_extpal) || view_moved(next.bobj_extpal, vram_map_.bobj_extpal)) ? 2u : 0u);
  const bool trapped = nds_.gpu.vram_remap_begin(moved_2d, &next);
  vram_map_ = next;
  // Whole 16 MB region described as one host pointer per page, applied as a
  // diff. Blocks backed by one bank map into the page table; blocks with
  // overlapping banks stay unmapped so the slow path can OR them together.
  u8** const h9 = vram_hosts_[0].get();
  u8** const h7 = vram_hosts_[1].get();
  std::memset(h9, 0, VRAM_PAGES * sizeof(u8*));
  std::memset(h7, 0, VRAM_PAGES * sizeof(u8*));
  auto set_pages = [](u8** hosts, u32 addr, u32 size, u8* host) {
    u8** pg = hosts + ((addr - 0x06000000) >> PAGE_SHIFT);
    for (u32 i = 0; i < size / PAGE_SIZE; ++i) pg[i] = host + i * PAGE_SIZE;
  };
  auto map_view = [&](u8** hosts, const gpu::VramView& v, u32 base, u32 end) {
    for (u32 mirror = base; mirror < end; mirror += v.size)
      for (u32 b = 0; b < v.blocks(); ++b)
        if (v.ptr[b]) set_pages(hosts, mirror + b * gpu::VramView::BLOCK, gpu::VramView::BLOCK, v.ptr[b]);
  };
  map_view(h9, vram_map_.abg,  0x06000000, 0x06200000);
  map_view(h9, vram_map_.bbg,  0x06200000, 0x06400000);
  map_view(h9, vram_map_.aobj, 0x06400000, 0x06600000);
  map_view(h9, vram_map_.bobj, 0x06600000, 0x06800000);
  map_view(h7, vram_map_.arm7, 0x06000000, 0x07000000);
  static const u32 lcdc_base[9] = {0x00000, 0x20000, 0x40000, 0x60000, 0x80000, 0x90000, 0x94000, 0x98000, 0xA0000};
  for (int i = 0; i < 9; ++i) {
    if (!(vram_map_.lcdc_mask & (1u << i))) continue;
    for (u32 mirror = 0x06800000; mirror < 0x07000000; mirror += 0x100000) set_pages(h9, mirror + lcdc_base[i], VRAM_BANK_SIZES[i], banks[i]);
  }
  // Only runs of pages whose host changed go through the page table.
  auto apply = [&](PageTable& pt, u8** cur, u8** prev) {
    constexpr u32 CHUNK = 256;   // entries (512 KB of guest space)
    u32 run = 0; bool in_run = false;
    for (u32 i = 0; i <= VRAM_PAGES; i += CHUNK) {
      const bool d = i < VRAM_PAGES && std::memcmp(cur + i, prev + i, CHUNK * sizeof(u8*)) != 0;
      if (d) { if (!in_run) { run = i; in_run = true; } }
      else if (in_run) { pt.remap(0x06000000 + (run << PAGE_SHIFT), (i - run) << PAGE_SHIFT, cur + run, RW); in_run = false; }
    }
  };
  for (u32 w = 0; w < VRAM_PAGES / 64; ++w) {
    u64 m = 0;
    for (u32 b = 0; b < 64; ++b) if (h9[w * 64 + b]) m |= u64{1} << b;
    vram_mapped9_[w] = m;
  }
  if (!vram_hosts_valid_) { pt9.remap(0x06000000, 0x01000000, h9, RW); pt7.remap(0x06000000, 0x01000000, h7, RW); vram_hosts_valid_ = true; }
  else { apply(pt9, h9, vram_hosts_prev_[0].get()); apply(pt7, h7, vram_hosts_prev_[1].get()); }
  std::swap(vram_hosts_[0], vram_hosts_prev_[0]);
  std::swap(vram_hosts_[1], vram_hosts_prev_[1]);
  nds_.gpu.vram_remap_end(trapped);
  if (watch_on && (watch_addr >> 24) == 0x06) { pt9.map_mmio(watch_addr & ~0x7FFu, 0x800); pt7.map_mmio(watch_addr & ~0x7FFu, 0x800); }
}

// Slow-path VRAM access for blocks with overlapping banks (and LCDC gaps).
static const gpu::VramView* vram_view_for(const gpu::VramMap& m, Cpu cpu, u32 addr, int& lcdc_bank, u32& off) {
  lcdc_bank = -1;
  if (cpu == Cpu::ARM7) { off = addr; return &m.arm7; }
  switch ((addr >> 21) & 7) {
  case 0: off = addr; return &m.abg;
  case 1: off = addr; return &m.bbg;
  case 2: off = addr; return &m.aobj;
  case 3: off = addr; return &m.bobj;
  default: {
    const u32 o = addr & 0xFFFFF;
    static const u32 base[9] = {0x00000, 0x20000, 0x40000, 0x60000, 0x80000, 0x90000, 0x94000, 0x98000, 0xA0000};
    for (int i = 0; i < 9; ++i)
      if (o >= base[i] && o < base[i] + Bus::VRAM_BANK_SIZES[i]) { lcdc_bank = i; off = o - base[i]; return nullptr; }
    return nullptr;
  }
  }
}

u32 Bus::vram_read(Cpu cpu, u32 addr, u32 width) {
  if (addr >= 0x06800000 && nds_.gpu.lcdc_read_trapped()) nds_.gpu.lcdc_read_hit(addr);
  if (bank_read_trapped(addr)) nds_.gpu.capture_read_hit();
  int bank; u32 off;
  const gpu::VramView* v = vram_view_for(vram_map_, cpu, addr, bank, off);
  if (v) return width == 8 ? vram_map_.read8(*v, off) : width == 16 ? vram_map_.read16(*v, off) : vram_map_.read32(*v, off);
  if (bank < 0 || !(vram_map_.lcdc_mask & (1u << bank))) return 0;
  u32 r = 0; std::memcpy(&r, vram_bank(bank) + off, width / 8); return r;
}

void Bus::vram_write(Cpu cpu, u32 addr, u32 width, u32 val) {
  if (watch_on && addr >= (watch_addr & ~0x7FFu) && addr < (watch_addr & ~0x7FFu) + 0x800 && watch_hits++ < 1000000)
    std::fprintf(stderr, "[watch] cpu%d vram write%u %08x = %08x pc %08x frame %llu line %u vramcnt_h %02x\n", cpu == Cpu::ARM9 ? 9 : 7, width, addr, val, nds_.cpu(cpu).hot.regs[15], (unsigned long long)nds_.frame_count, nds_.gpu.line(), nds_.io.vramcnt[7]);
  int bank; u32 off;
  const gpu::VramView* v = vram_view_for(vram_map_, cpu, addr, bank, off);
  if (v) { if (width == 8) vram_map_.write8(*v, off, val); else if (width == 16) vram_map_.write16(*v, off, val); else vram_map_.write32(*v, off, val); return; }
  if (bank < 0 || !(vram_map_.lcdc_mask & (1u << bank))) return;
  std::memcpy(vram_bank(bank) + off, &val, width / 8);
}

void Bus::update_tcm(CpuContext& cpu, bool force) {
  prof::add(prof::C_BUS_UPDATE_TCM, 1);
  // Skip if the effective windows are unchanged: the full remap and timing
  // rebuild (1M entries) are expensive and invalidate every translated block.
  const u32 old_itcm = cpu.itcm_size, old_dbase = cpu.dtcm_base, old_dmask = cpu.dtcm_mask;
  cpu.update_tcm_windows();
  if (!force && cpu.itcm_size == old_itcm && cpu.dtcm_base == old_dbase && cpu.dtcm_mask == old_dmask) return;
  PageTable& pt = cpu.page_table;
  const u32 RW = PAGE_READABLE | PAGE_WRITABLE, RO = PAGE_READABLE;
  if (force) pt.unmap(0x00000000, 0x10000000);
  vram_hosts_valid_ = false;   // unmaps above may have touched VRAM pages
  if (tcm_prev_itcm_) pt.unmap(0, std::min(tcm_prev_itcm_, 0x02000000u));
  if (tcm_prev_dtcm_size_) pt.unmap(tcm_prev_dtcm_base_, tcm_prev_dtcm_size_);
  tcm_prev_itcm_ = 0; tcm_prev_dtcm_size_ = 0;
  map_main_ram(pt);
  pt.map_mmio(0x04000000, 0x01000000);
  map_page_aligned(pt, 0x05000000, PALETTE_SIZE, palette.get(), RO, 0x06000000);   // stores journaled
  map_page_aligned(pt, 0x07000000, OAM_SIZE, oam.get(), RO, 0x08000000);
  pt.map_mmio(0x08000000, 0x02000000);
  if (force) update_bios_map();
  update_nwram();
  update_vram();
  // TCM windows are baked into the cost table: rebuild old and new windows.
  if (force) timing_.update_cpu9(cpu, 0, 0xFFFFFFFF);
  else {
    auto window = [&](u32 base, u32 mask, u32 itcm) {
      if (itcm) timing_.update_cpu9(cpu, 0, std::min(itcm, 0x10000000u), false);
      if (mask) { const u32 end = base + (~mask + 1); timing_.update_cpu9(cpu, base, end < base ? 0xFFFFFFFF : end, false); }   // ~mask + 1 = window size
    };
    window(old_dbase, old_dmask, old_itcm);
    window(cpu.dtcm_base, cpu.dtcm_mask, cpu.itcm_size);
    timing_.notify_cpu9(cpu);
  }
  if (watch_on) pt.map_mmio(watch_addr & ~0x7FFu, 0x800);
  const u32 ctl = cpu.cp15_control;
  if (ctl & (1u << 16)) {                                       // DTCM enabled
    const u32 base = cpu.cp15_dtcm & 0xFFFFF000;
    u32 size = 512u << ((cpu.cp15_dtcm >> 1) & 0x1F);
    if (size < DTCM_SIZE) size = DTCM_SIZE;
    for (u32 off = 0; off < size; off += DTCM_SIZE) {
      pt.map(base + off, DTCM_SIZE, dtcm.get(), RW);   // load mode (write-only) approximated as RW
    }
    tcm_prev_dtcm_base_ = base; tcm_prev_dtcm_size_ = size;
  }
  if (ctl & (1u << 18)) {                                       // ITCM enabled (base fixed at 0)
    u32 size = 512u << ((cpu.cp15_itcm >> 1) & 0x1F);
    if (size < ITCM_SIZE) size = ITCM_SIZE;
    if (size > 0x02000000) size = 0x02000000;
    for (u32 off = 0; off < size; off += ITCM_SIZE) {
      pt.map(off, ITCM_SIZE, itcm.get(), RW);
    }
    tcm_prev_itcm_ = size;
  }
}

// ---- slow paths -------------------------------------------------------------
void Bus::enable_watch(u32 addr) {
  watch_addr = addr; watch_on = true;
  // Host bytes of the page are recorded per CPU before it's taken out of the
  // tables; the slow path serves them below. A later remap of that page ends
  // the watch.
  for (int c = 0; c < 2; ++c) {
    PageTable& pt = nds_.cpu(c ? Cpu::ARM7 : Cpu::ARM9).page_table;
    u8* p = pt.read_ptr(addr & ~0x7FFu);
    watch_host[c] = p ? p : ((addr & 0xFF000000) == 0x02000000 ? main_ram.get() + (addr & (main_ram_size() - 1) & ~0x7FFu) : nullptr);
    if (watch_host[c]) pt.map_mmio(addr & ~0x7FFu, 0x800);
  }
}

bool Bus::watch_active() { return watch_on; }

u32 Bus::io_read(Cpu cpu, u32 addr, u32 width) {
  if (watch_on && (addr & ~0x7FFu) == (watch_addr & ~0x7FFu) && watch_host[cpu == Cpu::ARM9 ? 0 : 1]) {
    u32 v = 0; std::memcpy(&v, watch_host[cpu == Cpu::ARM9 ? 0 : 1] + (addr & 0x7FF), width / 8);
    if (addr < watch_addr + 4 && addr + width / 8 > watch_addr)
      std::fprintf(stderr, "[watch] cpu%d read%u %08x = %08x pc %08x t %llu frame %llu line %u\n", cpu == Cpu::ARM9 ? 9 : 7, width, addr, v, nds_.cpu(cpu).hot.regs[15], (unsigned long long)nds_.sched.now(), (unsigned long long)nds_.frame_count, nds_.gpu.line());
    return v;
  }
  if ((addr & 0xFF000000) == 0x04000000) return nds_.io.read(cpu, addr, width);
  // DSi: BIOS halves SCFG_BIOS hides read as all ones on both CPUs.
  if (nds_.dsi && (cpu == Cpu::ARM9 ? addr >= 0xFFFF0000 : addr < 0x00010000)) {
    const u32 ones = width == 32 ? 0xFFFFFFFFu : width == 16 ? 0xFFFFu : 0xFFu;
    if (cpu == Cpu::ARM9) return ones;
    const u16 scfg_bios = nds_.io.dsi.scfg_bios;
    if (scfg_bios & 0x0200) return ones;                     // DS BIOS selected: mapped directly, never here
    const u32 pc = nds_.cpu(cpu).hot.regs[15], prot = nds_.io.arm7_bios_prot;
    if (addr >= 0x8000 && (scfg_bios & 0x0100)) return ones;
    if (pc >= 0x10000) return ones;
    if (addr < prot && pc >= prot) return ones;
    u32 v = 0; std::memcpy(&v, bios7i.get() + (addr & 0xFFFF & ~(width / 8 - 1)), width / 8); return v;
  }
  if ((addr & 0xFF000000) == 0x06000000) return vram_read(cpu, addr, width);
  if ((addr & 0xFF000000) == 0x08000000 || (addr & 0xFF000000) == 0x09000000) {
    // Open bus pattern per GBATEK.
    u32 v = static_cast<u32>((addr >> 1) & 0xFFFF) | (static_cast<u32>(((addr + 2) >> 1) & 0xFFFF) << 16);
    return width == 8 ? (v >> ((addr & 1) * 8)) & 0xFF : width == 16 ? v & 0xFFFF : v;
  }
  return 0;
}
void Bus::io_write(Cpu cpu, u32 addr, u32 width, u32 v) {
  if (watch_on && (addr & ~0x7FFu) == (watch_addr & ~0x7FFu) && watch_host[cpu == Cpu::ARM9 ? 0 : 1]) {
    if (addr < watch_addr + 4 && addr + width / 8 > watch_addr)
      std::fprintf(stderr, "[watch] cpu%d write%u %08x = %08x pc %08x t %llu frame %llu line %u\n", cpu == Cpu::ARM9 ? 9 : 7, width, addr, v, nds_.cpu(cpu).hot.regs[15], (unsigned long long)nds_.sched.now(), (unsigned long long)nds_.frame_count, nds_.gpu.line());
    std::memcpy(watch_host[cpu == Cpu::ARM9 ? 0 : 1] + (addr & 0x7FF), &v, width / 8); return;
  }
  switch (addr >> 24) {
  case 0x04:
    // ARM9 word stores to GXFIFO/direct command ports: straight to the
    // geometry engine instead of through io_unowned's slower path.
    if (width == 32 && cpu == Cpu::ARM9 && addr - 0x04000400 < 0x1CC && !io::Io::census_on()) { nds_.gpu3d.gx_port_write(addr, v); return; }
    nds_.io.write(cpu, addr, width, v); return;
  case 0x05: nds_.gpu.palette_store(cpu, addr, width, v); return;
  case 0x07: nds_.gpu.oam_store(cpu, addr, width, v); return;
  case 0x06: {
    // Overlapping-bank blocks always land here; directly mapped pages only
    // while the lazy-2D write trap holds them.
    nds_.gpu.vram_store_trap(cpu, addr);
    const Entry e = nds_.cpu(cpu).page_table.entry(addr);
    if ((e & TAG_CODE) && (e & BASE_MASK)) { store_code(reinterpret_cast<u8*>(((e & BASE_MASK) << 2) + addr), &v, width / 8); return; }
    vram_write(cpu, addr, width, v);
    return;
  }
  default: return;
  }
}

void Bus::set_vram_trap(bool on, bool lcdc, u32 windows) {
  PageTable& pt9 = nds_.cpu(Cpu::ARM9).page_table;
  // Only over mapped pages (~330 of the 8K in the engine windows).
  auto range = [&](u32 addr, u32 size) {
    const u32 first = (addr - 0x06000000) >> PAGE_SHIFT;
    pt9.set_write_trap_bits(addr >> PAGE_SHIFT, size >> PAGE_SHIFT, vram_mapped9_ + first / 64, on);
  };
  if (windows & 1) { range(0x06000000, 0x00200000); range(0x06400000, 0x00200000); }   // BG-A, OBJ-A
  if (windows & 2) { range(0x06200000, 0x00200000); range(0x06600000, 0x00200000); }   // BG-B, OBJ-B
  if (lcdc) range(0x06800000, 0x00800000);    // LCDC and its 1 MB mirrors
}

// A trapped entry keeps only its tags: read_ptr/write_ptr see no base and
// fall to io_read/io_write. Saved entry restored on lift, keeping whatever
// code tag it picked up meanwhile. Raw entry writes: views never map VRAM.
void Bus::set_lcdc_read_trap(int bank, bool on) {
  assert(bank >= 0 && bank < 4);
  static const u32 lcdc_base[4] = {0x00000, 0x20000, 0x40000, 0x60000};
  Entry* const t = nds_.cpu(Cpu::ARM9).page_table.raw();
  const u32 pages = VRAM_BANK_SIZES[bank] >> PAGE_SHIFT;
  u32 k = 0;
  for (u32 mirror = 0x06800000; mirror < 0x07000000; mirror += 0x100000) {
    const u32 first = (mirror + lcdc_base[bank]) >> PAGE_SHIFT;
    for (u32 p = 0; p < pages; ++p, ++k) {
      Entry& e = t[first + p];
      if (on) { lcdc_read_save_[k] = e; if (e & BASE_MASK) e = TAG_SPECIAL | (e & TAG_CODE); }
      else if (!(e & BASE_MASK)) e = lcdc_read_save_[k] | (e & TAG_CODE);
    }
  }
}

void Bus::set_bank_read_trap(int bank, bool on) {
  assert(bank >= 0 && bank < 9);
  Entry* const t = nds_.cpu(Cpu::ARM9).page_table.raw();
  if (on) {
    assert(!bank_trap_on_ && vram_hosts_valid_);
    // Every page whose block the bank backs, alone (a host pointer inside
    // the bank) or with others (an OR-read block, no host pointer).
    const u8* const lo = vram_bank(bank);
    const u8* const hi = lo + VRAM_BANK_SIZES[bank];
    u8* const* hosts = vram_hosts_prev_[0].get();   // what the ARM9 table holds now
    bank_trap_save_.clear();
    std::memset(bank_trap_bits_, 0, sizeof bank_trap_bits_);
    for (u32 p = 0; p < VRAM_PAGES; ++p) {
      bool backed = hosts[p] && hosts[p] >= lo && hosts[p] < hi;
      if (!backed && !hosts[p]) {
        int b; u32 off;
        const u32 addr = 0x06000000 + (p << PAGE_SHIFT);
        if (const gpu::VramView* v = vram_view_for(vram_map_, Cpu::ARM9, addr, b, off)) backed = (v->mask[(off & v->addr_mask()) / gpu::VramView::BLOCK] >> bank) & 1;
        else if (b == bank) backed = true;   // its LCDC slot, unmapped
      }
      if (!backed) continue;
      Entry& e = t[(0x06000000 >> PAGE_SHIFT) + p];
      bank_trap_save_.push_back({p, e});
      if (e & BASE_MASK) e = TAG_SPECIAL | (e & TAG_CODE);
      bank_trap_bits_[p >> 6] |= u64{1} << (p & 63);
    }
    bank_trap_on_ = true;
    return;
  }
  if (!bank_trap_on_) return;
  for (const auto& [p, saved] : bank_trap_save_) {
    Entry& e = t[(0x06000000 >> PAGE_SHIFT) + p];
    if (!(e & BASE_MASK)) e = saved | (e & TAG_CODE);
  }
  bank_trap_save_.clear();
  std::memset(bank_trap_bits_, 0, sizeof bank_trap_bits_);
  bank_trap_on_ = false;
}

u8 Bus::dma_read8(Cpu cpu, u32 addr) {
  if (u8* p = nds_.cpu(cpu).page_table.read_ptr(addr)) return *p;
  return static_cast<u8>(io_read(cpu, addr, 8));
}
u16 Bus::dma_read16(Cpu cpu, u32 addr) {
  addr &= ~1u;
  if (u8* p = nds_.cpu(cpu).page_table.read_ptr(addr)) { u16 v; std::memcpy(&v, p, 2); return v; }
  return static_cast<u16>(io_read(cpu, addr, 16));
}
u32 Bus::dma_read32(Cpu cpu, u32 addr) {
  addr &= ~3u;
  if (u8* p = nds_.cpu(cpu).page_table.read_ptr(addr)) { u32 v; std::memcpy(&v, p, 4); return v; }
  return io_read(cpu, addr, 32);
}
void Bus::dma_write8(Cpu cpu, u32 addr, u8 v) {
  bool code = false;
  if (u8* p = nds_.cpu(cpu).page_table.write_ptr(addr, &code)) { if (code) store_code(p, &v, 1); else *p = v; return; }
  io_write(cpu, addr, 8, v);
}
void Bus::dma_write16(Cpu cpu, u32 addr, u16 v) {
  addr &= ~1u; bool code = false;
  if (u8* p = nds_.cpu(cpu).page_table.write_ptr(addr, &code)) { if (code) store_code(p, &v, 2); else std::memcpy(p, &v, 2); return; }
  io_write(cpu, addr, 16, v);
}
void Bus::dma_write32(Cpu cpu, u32 addr, u32 v) {
  addr &= ~3u; bool code = false;
  if (u8* p = nds_.cpu(cpu).page_table.write_ptr(addr, &code)) { if (code) store_code(p, &v, 4); else std::memcpy(p, &v, 4); return; }
  io_write(cpu, addr, 32, v);
}

u8  Bus::read8 (Cpu cpu, u32 addr) { return static_cast<u8>(io_read(cpu, addr, 8)); }
u16 Bus::read16(Cpu cpu, u32 addr) { return static_cast<u16>(io_read(cpu, addr, 16)); }
u32 Bus::read32(Cpu cpu, u32 addr) { return io_read(cpu, addr, 32); }
void Bus::write8 (Cpu cpu, u32 addr, u8  v) { io_write(cpu, addr, 8, v); }
void Bus::write16(Cpu cpu, u32 addr, u16 v) { io_write(cpu, addr, 16, v); }
void Bus::write32(Cpu cpu, u32 addr, u32 v) { io_write(cpu, addr, 32, v); }
u32 Bus::fetch(Cpu cpu, u32 addr, u32 width) {
  if (nds_.dsi && cpu == Cpu::ARM7 && addr < 0x00010000 && !(nds_.io.dsi.scfg_bios & 0x0200)) {
    if (addr >= 0x8000 && (nds_.io.dsi.scfg_bios & 0x0100)) return width == 32 ? 0xFFFFFFFFu : 0xFFFFu;
    u32 v = 0; std::memcpy(&v, bios7i.get() + (addr & 0xFFFF & ~(width / 8 - 1)), width / 8); return v;
  }
  return io_read(cpu, addr, width);
}


template <class S> void Bus::sync_state(S& s) {
  s.begin("MEM ");
  s.blob(main_ram.get(), main_ram_size());
  s.blob(shared_wram.get(), SHARED_WRAM_SIZE);
  s.blob(arm7_wram.get(), ARM7_WRAM_SIZE);
  s.blob(itcm.get(), ITCM_SIZE);
  s.blob(dtcm.get(), DTCM_SIZE);
  s.blob(vram.get(), VRAM_TOTAL);
  s.blob(palette.get(), PALETTE_SIZE);
  s.blob(oam.get(), OAM_SIZE);
  if (nds_.dsi) for (auto& b : nwram) s.blob(b.get(), NWRAM_BANK_SIZE);
  s.end();
}
template void Bus::sync_state<state::Writer>(state::Writer&);
template void Bus::sync_state<state::Reader>(state::Reader&);

void Bus::relink() {
  CpuContext& a9 = nds_.cpu(Cpu::ARM9);
  CpuContext& a7 = nds_.cpu(Cpu::ARM7);
  a9.timing9 = a7.timing9 = timing_.cpu9();
  a9.timing7 = a7.timing7 = timing_.cpu7();
  a9.cost7 = a7.cost7 = timing_.cost7();
  cp15_update_pu_map(a9);
  update_main_ram();            // rebuilds TCM/BIOS/NWRAM/VRAM maps too
  gba_slot_applied_ = -1;        // loaded EXMEMCNT is not what the tables hold
  update_gba_slot_timings();
  if (nds_.dsi) {
    set_clock9_shift((nds_.io.dsi.scfg_clock9 & 1) ? 2 : 1);
    update_vram_timings();
  }
}

} // namespace ds::mem
