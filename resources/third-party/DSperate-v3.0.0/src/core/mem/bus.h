// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"
#include "core/mem/page_table.h"
#include "core/mem/timing.h"
#include "core/mem/fastmem.h"
#include "core/gpu/vram_map.h"

#include <memory>
#include <utility>
#include <vector>

namespace ds {
struct NDS;
struct CpuContext;
}

namespace ds::mem {

// Owns the physical memories and keeps both CPUs' page tables in sync with the
// guest-visible mapping (VRAM bank control, shared-WRAM split, ITCM/DTCM moves).
// Every mapping change goes through here so that the JIT's CODE tags and the
// page tables never drift apart.
class Bus {
public:
  explicit Bus(NDS& nds);
  ~Bus();

  // DS_FASTMEM (fastmem.h): shared object the buffers live in, and one host
  // view per CPU derived from its page table. nullptr when off/unavailable.
  std::unique_ptr<HostArena> arena_;
  std::unique_ptr<GuestView> views_[2];
  GuestView* view(Cpu cpu) const { return views_[cpu == Cpu::ARM9 ? 0 : 1].get(); }
  void fastmem_flush();
  bool fastmem_verify(u64 frame);        // DS_FASTMEM_VERIFY
  void fastmem_report() const;

  void reset();
  // Save states: physical memories, then rebuild every mapping/timing table
  // from restored control registers (WRAMCNT, VRAMCNT, EXMEMCNT, CP15).
  template <class S> void sync_state(S& s);
  void relink();

  // Remaps after control-register changes.
  void update_tcm(CpuContext& cpu, bool force = false);
  u32 tcm_prev_itcm_ = 0, tcm_prev_dtcm_base_ = 0, tcm_prev_dtcm_size_ = 0;   // windows from last update_tcm (CP15, ARM9)
  void update_wram();                 // WRAMCNT (DS), or beneath the NWRAM windows (DSi)
  // DSi: rebuild both CPUs' 0x03000000 windows from MBK1-9 over the WRAMCNT
  // split. A slot with no backing MBK entry reads 0, drops writes, as on hw.
  void update_nwram(bool windows_only = false);   // windows_only: only MBK1-8 changed
  // DSi: BIOS pair per SCFG_BIOS (bits 0/8 hide upper 32 KB, bits 1/9 fall
  // back to DS images).
  void update_bios_map();
  void set_clock9_shift(u32 shift);   // DSi: SCFG_CLK9 bit 0 doubles ARM9 clock; rebuilds the timing table
  void update_vram_timings();         // DSi: SCFG_EXT bit 13 widens VRAM bus to 32 bits
  // DSi: main RAM size the CPUs address, 4 or 16 MB (SCFG_EXT7 bits 14-15);
  // the buffer itself stays 16 MB.
  u32 main_ram_span() const;
  void update_main_ram();
  void update_vram();                 // VRAMCNT A-I
  Entry lcdc_read_save_[8 * 64] = {};   // set_lcdc_read_trap: 8 mirrors x 128 KB / PAGE_SIZE
  // Lazy 2D: trap ARM9 stores to the engines' VRAM windows (and, with
  // `lcdc`, the LCDC window) so the first store in a frame flushes the
  // deferred render first. `a_only`: engine A only (lag mode).
  // `windows`: bit 0 engine A's BG/OBJ windows, bit 1 engine B's.
  void set_vram_trap(bool on, bool lcdc, u32 windows = 3);
  // Read trap on one LCDC bank, all mirrors: ARM9 loads/DMA reads take the
  // slow path while a display capture writing the bank is still in flight
  // (Gpu::join_worker). Saved/restored; a VRAMCNT remap lifts it first.
  void set_lcdc_read_trap(int bank, bool on);
  // The same trap over every ARM9 page bank `bank` currently backs, whatever
  // window it maps to (after a remap under a capture in flight: the guest
  // must not read the bank through its new mapping before the job is done).
  void set_bank_read_trap(int bank, bool on);
  bool bank_read_trapped(u32 addr) const {
    if (!bank_trap_on_ || addr < 0x06000000 || addr >= 0x07000000) return false;
    const u32 p = (addr - 0x06000000) >> PAGE_SHIFT;
    return (bank_trap_bits_[p >> 6] >> (p & 63)) & 1;
  }

  // DMA accesses (no CPU cycle accounting). The 8-bit pair is for the cheat
  // engine (ar_engine.h), not real DMA which is 16/32-bit only.
  u8  dma_read8 (Cpu cpu, u32 addr);
  void dma_write8(Cpu cpu, u32 addr, u8 v);
  u16 dma_read16(Cpu cpu, u32 addr);
  u32 dma_read32(Cpu cpu, u32 addr);
  void dma_write16(Cpu cpu, u32 addr, u16 v);
  void dma_write32(Cpu cpu, u32 addr, u32 v);

  Timing& timing() { return timing_; }
  const Timing& timing() const { return timing_; }
  void update_gba_slot_timings();    // EXMEMCNT
  void update_wifi_timings();        // WIFIWAITCNT / POWCNT2 bit 1
  int gba_slot_applied_ = -1;        // EXMEMCNT timing bits currently applied
  void enable_watch(u32 addr);       // debug: log writes to a main-RAM word (DS_WATCH, headless)
  static bool watch_active();

  // Slow paths, reached when the page table returns nullptr.
  u8  read8 (Cpu cpu, u32 addr);
  u16 read16(Cpu cpu, u32 addr);
  u32 read32(Cpu cpu, u32 addr);
  // Fetch fallback for pages not directly mapped; DSi ARM7 BIOS lives here.
  u32 fetch(Cpu cpu, u32 addr, u32 width);
  void write8 (Cpu cpu, u32 addr, u8  v);
  void write16(Cpu cpu, u32 addr, u16 v);
  void write32(Cpu cpu, u32 addr, u32 v);

  // Physical memories (sizes per GBATEK). MAIN_RAM_SIZE is the DS's 4 MB the
  // RetroAchievements map is written against; the buffer is always 16 MB
  // (DSi), and main_ram_size() says how much of it is in use.
  static constexpr u32 MAIN_RAM_SIZE    = 4 * 1024 * 1024;
  static constexpr u32 MAIN_RAM_SIZE_DSI = 16 * 1024 * 1024;
  static constexpr u32 NWRAM_BANK_SIZE  = 256 * 1024;      // three banks, 64 KB (A) / 32 KB (B, C) slots
  static constexpr u32 BIOS9I_SIZE      = 64 * 1024;
  static constexpr u32 BIOS7I_SIZE      = 64 * 1024;
  u32 main_ram_size() const;
  static constexpr u32 SHARED_WRAM_SIZE = 32 * 1024;
  static constexpr u32 ARM7_WRAM_SIZE   = 64 * 1024;
  static constexpr u32 ITCM_SIZE        = 32 * 1024;
  static constexpr u32 DTCM_SIZE        = 16 * 1024;
  static constexpr u32 PALETTE_SIZE     = 2 * 1024;
  static constexpr u32 OAM_SIZE         = 2 * 1024;
  static constexpr u32 BIOS9_SIZE       = 4 * 1024;
  static constexpr u32 BIOS7_SIZE       = 16 * 1024;
  static constexpr u32 VRAM_PAGES = 0x01000000 / PAGE_SIZE;   // the 0x06000000 region, per CPU
  std::unique_ptr<u8*[]> vram_hosts_[2] = {std::make_unique<u8*[]>(VRAM_PAGES), std::make_unique<u8*[]>(VRAM_PAGES)};   // update_vram scratch (next)
  std::unique_ptr<u8*[]> vram_hosts_prev_[2] = {std::make_unique<u8*[]>(VRAM_PAGES), std::make_unique<u8*[]>(VRAM_PAGES)};   // what the tables hold now
  bool vram_hosts_valid_ = false;
  bool bank_trap_on_ = false;
  std::vector<std::pair<u32, Entry>> bank_trap_save_;   // page index, entry before the trap
  u64 bank_trap_bits_[VRAM_PAGES / 64] = {};
  // Pages of the ARM9 0x06000000 window with a host (rebuilt by update_vram):
  // the write trap toggles visit these, not all 8 K entries.
  u64 vram_mapped9_[VRAM_PAGES / 64] = {};
  static constexpr u32 VRAM_BANK_SIZES[9] = {0x20000, 0x20000, 0x20000, 0x20000, 0x10000, 0x4000, 0x4000, 0x8000, 0x4000};
  static constexpr u32 VRAM_TOTAL = 0x20000 * 4 + 0x10000 + 0x4000 * 2 + 0x8000 + 0x4000;

  const gpu::VramMap& vram_map() const { return vram_map_; }

  PageBuf take_buf(size_t bytes);   // from the arena, else alloc_page_buf
  PageBuf main_ram, shared_wram, arm7_wram, itcm, dtcm, vram, palette, oam, bios9, bios7;   // PAGE_SIZE-aligned, see alloc_page_buf
  PageBuf nwram[3], bios9i, bios7i;   // DSi: NWRAM A/B/C and the 64 KB BIOS pair (zero on a DS)
  u8* vram_bank(int i);

private:
  NDS& nds_;
  Timing timing_;
  gpu::VramMap vram_map_;
  u32 vram_read(Cpu cpu, u32 addr, u32 width);
  void vram_write(Cpu cpu, u32 addr, u32 width, u32 v);
  void map_fixed_regions();
  void map_main_ram(PageTable& pt);
  void map_page_aligned(PageTable& pt, u32 guest, u32 size, u8* host, u32 flags, u32 mirror_end);
  // DSi NWRAM slot tables from MBK1-5: [bank][cpu 0 ARM9 / 1 ARM7 / 2 DSP][slot].
  u8* nwram_map_[3][3][8] = {};
  void nwram_windows(int c, bool nwram, u32 win[3][2]) const;
  u8* wram_cell_host(int c, const u32 win[3][2], u32 a) const;   // 16 KB cell at `a`: WRAMCNT under the windows
  u8* wram_cells_[2][0x400] = {};   // what the last apply mapped, per cell
  void apply_wram(bool nwram, bool windows_only);
  u32 wram_key_[2] = {~0u, ~0u};   // what the last apply laid under the windows (~0: nothing yet)
  u32 wram_win_[2][3][2] = {};     // and the windows it laid
public:
  // Non-RAM access path (I/O, VRAM slow blocks, GBA slot). The JIT's
  // slow-store helper enters here for I/O to get the GXFIFO fast path.
  u32 io_read(Cpu cpu, u32 addr, u32 width);
  void io_write(Cpu cpu, u32 addr, u32 width, u32 v);
};

} // namespace ds::mem
