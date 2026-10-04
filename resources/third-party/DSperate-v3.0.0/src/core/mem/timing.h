// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"

#include <algorithm>
#include <memory>
#include <vector>

namespace ds { struct CpuContext; }

namespace ds::mem {

enum Region : u8 {
  REGION_NONE = 0, REGION_BIOS, REGION_MAIN_RAM, REGION_WRAM, REGION_IO, REGION_PALETTE,
  REGION_VRAM, REGION_OAM, REGION_GBA_ROM, REGION_GBA_RAM, REGION_WIFI0, REGION_WIFI1,
};

// Bus access timing per region, in system (ARM7) cycles.
//   ARM9 bus table: per 16 KB, [0..3] CPU N16,S16,N32,S32 (N includes the
//   3-cycle non-sequential penalty outside main RAM), [4..7] DMA N16,S16,N32,S32.
//   ARM7 bus table: per 32 KB, N16,S16,N32,S32 (CPU and DMA alike).
//   ARM9 CPU table: per 4 KB, derived from the bus table and the PU's
//   cacheability map: [0] code cost in ARM9 cycles (0xFF = cacheable),
//   [1] data N16, [2] data N32, [3] data S32, in ARM9 cycles.
class Timing {
public:
  // Bumped after every retime (set_region9/7); a parked JIT block revives only
  // under the stamp it was translated with.
  u64 stamp = 0;

  // Bus cycles to ARM9 cycles: 1 on DS, 2 on DSi with SCFG_CLK9 bit 0 set.
  // Bus::set_clock9_shift changes it live.
  u32 clock9_shift = 1;

  // Bumped on every ARM9 CPU table change; a cost cached from it (BIOS
  // SHA-1 hook, jit/bios_sha1.cpp) is kept until this moves.
  u64 cpu9_version = 0;

  // ARM7 precomputed data-cost table: `nc`/`cdi`/`code_main`/width are all
  // known at translation time, so the cost model is evaluated once here and
  // the recompiler spends a load instead of a branch tree.
  // 32 bytes per 32 KB page: [code_main][cdi][nc_idx][word]. Allocated right
  // after the raw ARM7 bus table so the pinned timing register still points
  // at it; the recompiler reaches the cost table with +COST7_OFFSET.
  static constexpr u32 COST7_STRIDE = 32;
  static constexpr u32 BUS7_BYTES   = 0x20000 * 4;
  static constexpr u32 COST7_OFFSET = BUS7_BYTES;
  static constexpr u32 COST7_BYTES  = 0x20000 * COST7_STRIDE;
  static constexpr u32 NC7_SLOTS    = 4;

  // ARM9 precomputed pipeline-refill table: indirect-branch cost per page,
  // 4 bytes indexed by the second fetch's placement (first fetch is always a
  // line fill on a cacheable page, `X` below):
  //   [0] X + Y (second mid-line)  [1] X + X (second line-aligned)
  //   [2] X + X[page+1] (second in next page)  [3] X only (Thumb, aligned)
  // with X = fetch_cost9(page, branch), Y = fetch_cost9(page, sequential).
  // Follows cpu9_ in one allocation; reached via +REFILL9_OFFSET.
  static constexpr u32 CPU9_BYTES     = 0x100000 * 8;
  static constexpr u32 REFILL9_OFFSET = CPU9_BYTES;
  static constexpr u32 REFILL9_BYTES  = 0x100000 * 4;

  Timing();
  void reset();

  // Addresses are in bytes; table granularity rounds.
  void set_region9(u32 start, u32 end, Region r, int bus_width, int nonseq, int seq);
  void set_region7(u32 start, u32 end, Region r, int bus_width, int nonseq, int seq);

  // Rebuild [start, end) of the ARM9 per-4 KB CPU table from the PU map;
  // pass notify=false while rebuilding several ranges, then call
  // notify_cpu9 once.
  void update_cpu9(const CpuContext& cpu, u32 start, u32 end, bool notify = true);
  void notify_cpu9(const CpuContext& cpu);

  const u8 (*cpu9() const)[8] { return reinterpret_cast<const u8 (*)[8]>(cpu9_.get()); }
  const u8 (*cpu7() const)[4] { return reinterpret_cast<const u8 (*)[4]>(bus7()); }

  const u8* cost7() const { return tim7_.get() + COST7_OFFSET; }
  // Slot for a code-fetch cost, or -1 if `nc` isn't a value the region table
  // produces (recompiler then keeps the inline model).
  int nc7_index(u32 nc) const {
    for (u32 i = 0; i < NC7_SLOTS; ++i) if (nc7_values_[i] == nc) return static_cast<int>(i);
    return -1;
  }
  static u32 cost7_offset(bool code_main, bool cdi, u32 nc_idx, bool word) {
    return (code_main ? 16u : 0u) + (cdi ? 8u : 0u) + nc_idx * 2u + (word ? 1u : 0u);
  }
  u32 region(bool arm9, u32 addr) const { return arm9 ? regions9_[addr >> 14] : regions7_[addr >> 15]; }
  // CPU-side N32/S32 bus costs (with ARM9 non-sequential penalty); what the DSi's NDMA is priced from.
  void ndma_cost(bool arm9, u32 addr, u32& n32, u32& s32) const {
    if (arm9) { const u8* t = &bus9_[(addr >> 14) * 8]; n32 = t[2]; s32 = t[3]; }
    else      { const u8* t = &bus7()[(addr >> 15) * 4]; n32 = t[2]; s32 = t[3]; }
  }
  void dma_cost(bool arm9, u32 addr, bool word, u32& n, u32& s) const {
    if (arm9) { const u8* t = &bus9_[(addr >> 14) * 8]; n = t[word ? 6 : 4]; s = t[word ? 7 : 5]; }
    else      { const u8* t = &bus7()[(addr >> 15) * 4]; n = t[word ? 2 : 0]; s = t[word ? 3 : 1]; }
  }

  // PU map for the ARM9: per 4 KB, bit 4 = data cacheable, bit 6 = code cacheable.
  std::unique_ptr<u8[]> pu_map;

  // PU state pu_map was last built from (cp15_update_pu_map), so a PU write
  // re-derives only the changed regions' pages. Invalid after reset().
  struct PuMemo { bool valid = false; u32 ctl = 0, dc = 0, cc = 0, region[8] = {}; } pu_memo;

  // Retime dependency set for the recompiler. A translation bakes at most
  // c[0] (code fetch cost) and c[2] (N32/pc-relative load) of a 4 KB entry;
  // update_cpu9 flags pages where either byte changed, the JIT kills blocks
  // depending on a flagged page, then calls retime_clear(). Flags are sticky
  // across the several update_cpu9 calls of one notify.
  static constexpr u8 RETIME_CODE = 1, RETIME_DATA = 2;
  u8 retime_flag(u32 page) const { return retime_flags_[page]; }
  bool retime_pending() const { return retime_overflow_ || !retime_list_.empty(); }
  void retime_clear() {
    if (retime_overflow_) std::fill_n(retime_flags_.get(), 0x100000, u8{0});
    else for (u32 p : retime_list_) retime_flags_[p] = 0;
    retime_list_.clear(); retime_overflow_ = false;
  }

private:
  std::unique_ptr<u8[]> retime_flags_;   // 0x100000, RETIME_* bits
  std::vector<u32> retime_list_;         // pages with a non-zero flag (the first RETIME_LIST_MAX)
  bool retime_overflow_ = false;
  static constexpr size_t RETIME_LIST_MAX = 1u << 16;
  std::unique_ptr<u8[]> bus9_;     // 0x40000 * 8
  std::unique_ptr<u8[]> regions9_; // 0x40000
  // [0, BUS7_BYTES) the raw ARM7 bus table, then the precomputed cost table.
  std::unique_ptr<u8[]> tim7_;
  u8* bus7() const { return tim7_.get(); }
  u8* cost7_rw() const { return tim7_.get() + COST7_OFFSET; }
  void build_cost7();                                  // full: rescan nc values, then all pages
  void build_cost7_range(u32 first_page, u32 last_page);
  u32  nc7_values_[NC7_SLOTS] = {};
  bool cost7_ready_ = false;   // set_region7 keeps the table current once this is true
  std::unique_ptr<u8[]> regions7_; // 0x20000
  std::unique_ptr<u8[]> cpu9_;     // [0, CPU9_BYTES) 8 bytes per page, then the refill table
  u8* refill9_rw() const { return cpu9_.get() + REFILL9_OFFSET; }
  void build_refill9(u32 first_page, u32 last_page);   // [first, last), clamped
};

} // namespace ds::mem
