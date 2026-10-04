// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/mem/timing.h"
#include "core/profile.h"
#include "core/cpu/cpu.h"
#include "core/nds.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ds::mem {

constexpr u32 CACHE_CODE = 3, CACHE_DATA = 3;   // cycles for a cached fetch/access (line fill approximation)

Timing::Timing()
    : pu_map(new u8[0x100000]), retime_flags_(new u8[0x100000]), bus9_(new u8[0x40000 * 8]), regions9_(new u8[0x40000]),
      tim7_(new u8[COST7_OFFSET + COST7_BYTES]), regions7_(new u8[0x20000]), cpu9_(new u8[CPU9_BYTES + REFILL9_BYTES]) {
  std::memset(retime_flags_.get(), 0, 0x100000);
  reset();
}

void Timing::reset() {
  cost7_ready_ = false;   // set_region7 calls below rebuild once, at the end
  std::memset(pu_map.get(), 0, 0x100000);
  pu_memo = PuMemo{};
  retime_clear();
  set_region9(0x00000000, 0xFFFFFFFF, REGION_NONE, 32, 1, 1);
  set_region9(0xFFFF0000, 0xFFFFFFFF, REGION_BIOS, 32, 1, 1);
  set_region9(0x02000000, 0x03000000, REGION_MAIN_RAM, 16, 8, 1);
  set_region9(0x03000000, 0x04000000, REGION_WRAM, 32, 1, 1);
  set_region9(0x04000000, 0x05000000, REGION_IO, 32, 1, 1);
  set_region9(0x05000000, 0x06000000, REGION_PALETTE, 16, 1, 1);
  set_region9(0x06000000, 0x07000000, REGION_VRAM, 16, 1, 1);
  set_region9(0x07000000, 0x08000000, REGION_OAM, 32, 1, 1);
  set_region9(0x08000000, 0x0A000000, REGION_GBA_ROM, 16, 10, 6);
  set_region9(0x0A000000, 0x0B000000, REGION_GBA_RAM, 8, 10, 10);
  set_region7(0x00000000, 0xFFFFFFFF, REGION_NONE, 32, 1, 1);
  set_region7(0x00000000, 0x00010000, REGION_BIOS, 32, 1, 1);
  set_region7(0x02000000, 0x03000000, REGION_MAIN_RAM, 16, 8, 1);
  set_region7(0x03000000, 0x04000000, REGION_WRAM, 32, 1, 1);
  set_region7(0x04000000, 0x04800000, REGION_IO, 32, 1, 1);
  set_region7(0x04800000, 0x04808000, REGION_WIFI0, 32, 1, 1);
  set_region7(0x04808000, 0x04810000, REGION_WIFI1, 32, 1, 1);
  set_region7(0x06000000, 0x07000000, REGION_VRAM, 16, 1, 1);
  // CPU table: no PU yet -> nothing cached. One slot per 16 KB bus entry,
  // stored as a word (byte stores here were a visible share of boot time).
  for (u32 g = 0; g < 0x40000; ++g) {
    const u8* b = &bus9_[g * 8];
    u8 slot[8];
    const u32 sh = clock9_shift;
    slot[0] = static_cast<u8>(b[2] << sh); slot[1] = static_cast<u8>(b[0] << sh); slot[2] = static_cast<u8>(b[2] << sh); slot[3] = static_cast<u8>(b[3] << sh);
    slot[4] = slot[0]; slot[5] = slot[1]; slot[6] = slot[2]; slot[7] = slot[3];
    u64 w; std::memcpy(&w, slot, 8);
    u64* c = reinterpret_cast<u64*>(&cpu9_[g * 4 * 8]);
    c[0] = w; c[1] = w; c[2] = w; c[3] = w;
  }
  build_refill9(0, 0x100000);
  build_cost7();
  ++cpu9_version;
}

// Mirrors refill_cycles()/fetch_cost9() (cpu_cycles.h): branch fetch on a
// cacheable page (0xFF) is a line fill (3), sequential fetch is a hit (1)
// unless it starts a line (3); else costs the page's code byte.
void Timing::build_refill9(u32 first_page, u32 last_page) {
  auto X = [&](u32 p) { const u8 c = cpu9_[p * 8]; return c == 0xFF ? 3u : c; };
  auto Y = [&](u32 p) { const u8 c = cpu9_[p * 8]; return c == 0xFF ? 1u : c; };
  if (last_page > 0x100000) last_page = 0x100000;
  for (u32 p = first_page; p < last_page; ++p) {
    const u32 x = X(p), xn = p + 1 < 0x100000 ? X(p + 1) : x;
    const u32 r = (x + Y(p)) | ((x + x) << 8) | ((x + xn) << 16) | (x << 24);
    std::memcpy(refill9_rw() + p * 4, &r, 4);
  }
}

// ARM7 data cost, evaluated once per (page, translate-time constants) instead
// of per access. Mirrors emit_charge_data_body's ARM7 arm in translate.cpp
// exactly, including signed maxima; the fuzzer checks the two agree.
void Timing::build_cost7() {
  // Distinct code-fetch costs the ARM7 region table produces; outside this
  // set keeps the inline model (nc7_index returns -1).
  u32 n = 0;
  for (u32 i = 0; i < NC7_SLOTS; ++i) nc7_values_[i] = 0xFFFFFFFFu;
  for (u32 p = 0; p < 0x20000 && n < NC7_SLOTS; ++p) {
    const u8* t = &bus7()[p * 4];
    const u32 nc_slots[2] = {0u, 2u};            // numC_nonseq7: n16 (Thumb) or n32 (ARM)
    for (u32 k : nc_slots) {
      bool seen = false;
      for (u32 i = 0; i < n; ++i) if (nc7_values_[i] == t[k]) seen = true;
      if (!seen && n < NC7_SLOTS) nc7_values_[n++] = t[k];
    }
  }

  if (std::getenv("DS_DEBUG_TIMING")) {
    std::fprintf(stderr, "[timing] arm7 nc slots:");
    for (u32 i = 0; i < NC7_SLOTS; ++i)
      if (nc7_values_[i] != 0xFFFFFFFFu) std::fprintf(stderr, " %u", nc7_values_[i]);
    std::fprintf(stderr, "\n");
  }

  build_cost7_range(0, 0x20000);
  cost7_ready_ = true;
}

// Fill [first_page, last_page). The 32-byte block depends only on the page's
// two bus bytes and whether it's main RAM, and consecutive pages nearly
// always agree, so it's evaluated once per change of input and copied
// otherwise.
void Timing::build_cost7_range(u32 first_page, u32 last_page) {
  auto smax = [](s32 a, s32 b) { return a > b ? a : b; };
  u8 block[COST7_STRIDE];
  u32 key = 0xFFFFFFFFu;   // (nd16, nd32, data_main) of `block`
  for (u32 p = first_page; p < last_page; ++p) {
    const u8* t = &bus7()[p * 4];
    const bool data_main = (p >> 9) == 2;        // (addr >> 24) == 2
    const u32 k = t[0] | (t[2] << 8) | (data_main ? 0x10000u : 0u);
    if (k != key) {
      key = k;
      for (u32 cm = 0; cm < 2; ++cm)
        for (u32 cdi = 0; cdi < 2; ++cdi)
          for (u32 ni = 0; ni < NC7_SLOTS; ++ni)
            for (u32 w = 0; w < 2; ++w) {
              const u32 v = nc7_values_[ni];
              if (v == 0xFFFFFFFFu) { block[cost7_offset(cm, cdi, ni, w)] = 0; continue; }
              const s32 nc = static_cast<s32>(v);
              const s32 nd = t[w ? 2 : 0];         // singles are never sequential
              s32 cost;
              if (data_main) {
                if (cm) cost = nd + nc;
                else {
                  const s32 ncx = nc + static_cast<s32>(cdi);
                  cost = smax(smax(ncx, nd), nd + ncx - 3);
                }
              } else {
                if (cm) {
                  const s32 ndp = nd + static_cast<s32>(cdi);
                  cost = smax(smax(nc, ndp), ndp + nc - 3);
                } else {
                  cost = nd + nc + static_cast<s32>(cdi);
                }
              }
              block[cost7_offset(cm, cdi, ni, w)] = static_cast<u8>(cost);
            }
    }
    std::memcpy(&cost7_rw()[p * COST7_STRIDE], block, COST7_STRIDE);
  }
}

void Timing::set_region9(u32 start, u32 end, Region r, int bus_width, int nonseq, int seq) {
  const int n16 = nonseq, s16 = seq;
  const int n32 = bus_width == 16 ? n16 + s16 : n16, s32 = bus_width == 16 ? s16 + s16 : s16;
  const int cpu_n = (r == REGION_MAIN_RAM) ? 0 : 3;
  const u32 first = start >> 14, last = (end == 0xFFFFFFFF) ? 0x40000 : (end >> 14);
  for (u32 i = first; i < last; ++i) {
    u8* t = &bus9_[i * 8];
    t[0] = static_cast<u8>(n16 + cpu_n); t[1] = static_cast<u8>(s16); t[2] = static_cast<u8>(n32 + cpu_n); t[3] = static_cast<u8>(s32);
    t[4] = static_cast<u8>(n16); t[5] = static_cast<u8>(s16); t[6] = static_cast<u8>(n32); t[7] = static_cast<u8>(s32);
    regions9_[i] = r;
  }
  ++stamp;
}

void Timing::set_region7(u32 start, u32 end, Region r, int bus_width, int nonseq, int seq) {
  const int n16 = nonseq, s16 = seq;
  const int n32 = bus_width == 16 ? n16 + s16 : n16, s32 = bus_width == 16 ? s16 + s16 : s16;
  const u32 first = start >> 15, last = (end == 0xFFFFFFFF) ? 0x20000 : (end >> 15);
  for (u32 i = first; i < last; ++i) {
    u8* t = &bus7()[i * 4];
    t[0] = static_cast<u8>(n16); t[1] = static_cast<u8>(s16); t[2] = static_cast<u8>(n32); t[3] = static_cast<u8>(s32);
    regions7_[i] = r;
  }
  // Keep the precomputed cost table in step; a stale entry is a silent
  // timing divergence. A cost not in the nc slots falls back to the inline
  // model (nc7_index returns -1).
  if (cost7_ready_) build_cost7_range(first, last);
  ++stamp;
}

// TCM windows are baked into the table (4 KB pages, both windows aligned to
// their size): an ITCM page costs 1 for code and data, a DTCM page 1 for
// data, so both engines cost an access with one table lookup.
void Timing::update_cpu9(const CpuContext& cpu, u32 start, u32 end, bool notify) {
  prof::add(prof::C_TIMING_UPDATE_CPU9, 1);
  static const bool debug = std::getenv("DS_DEBUG_TIMING") != nullptr;   // logs every rebuild
  if (debug) std::fprintf(stderr, "[timing] update_cpu9 %08x-%08x ctl %08x dtcm %08x itcm %08x pu %08x/%08x\n", start, end, cpu.cp15_control, cpu.cp15_dtcm, cpu.cp15_itcm, cpu.pu_data_cacheable, cpu.pu_code_cacheable);
  // Stores ([5..7]): the ARM946E-S data cache doesn't allocate on write, so a
  // store to a cacheable page that misses goes out at bus speed; only TCMs
  // are free. DSi prices stores as cache hits instead (DSi PictoChat's boot
  // heap needs it).
  const bool store_bus = !(cpu.nds && cpu.nds->dsi);
  const u32 first = start >> 12, last = (end == 0xFFFFFFFF) ? 0x100000 : (end >> 12);
  const u32 sh = clock9_shift;
  // One page's slot: depends on the 16 KB bus entry, the page's two PU
  // cacheability bits, and whether a TCM covers it.
  const auto compose = [&](const u8* b, u8 pu, bool tcm_i, bool tcm_d) {
    u8 c[8];
    c[0] = tcm_i ? 1 : (pu & 0x40) ? 0xFF : static_cast<u8>(b[2] << sh);
    if (tcm_i || tcm_d) { c[1] = 1; c[2] = 1; c[3] = 1; }
    else if (pu & 0x10) { c[1] = CACHE_DATA; c[2] = CACHE_DATA; c[3] = 1; }
    else { c[1] = static_cast<u8>(b[0] << sh); c[2] = static_cast<u8>(b[2] << sh); c[3] = static_cast<u8>(b[3] << sh); }
    c[4] = c[0];
    if (!(tcm_i || tcm_d) && (pu & 0x10) && store_bus) { c[5] = static_cast<u8>(b[0] << sh); c[6] = static_cast<u8>(b[2] << sh); c[7] = static_cast<u8>(b[3] << sh); }
    else { c[5] = c[1]; c[6] = c[2]; c[7] = c[3]; }
    u64 w; std::memcpy(&w, c, 8); return w;
  };
  const auto bus_word = [&](u32 group) { u64 w; std::memcpy(&w, &bus9_[group * 8], 8); return w; };
  // Walk is by runs, not pages, since a full rebuild visits a million pages.
  // TCMs are two page intervals (sizes are powers of two, aligned); between
  // their edges a run with the same PU bits and bus entry shares one slot,
  // and whole 16-page blocks are skipped where nothing changed.
  const u64 itcm_end = cpu.itcm_size >> 12;
  const u64 dtcm_lo = cpu.dtcm_mask ? (cpu.dtcm_base >> 12) : 0x100000, dtcm_hi = cpu.dtcm_mask ? ((static_cast<u64>(cpu.dtcm_base) + (~cpu.dtcm_mask + 1ull)) >> 12) : 0x100000;
  const u8* pu_map_p = pu_map.get();
  u8* const refill = refill9_rw();
  u32 x_prev = 0, y_prev = 0; bool changed_prev = false;
  u32 i = first;
  while (i < last) {
    u32 seg_end = last;
    for (u64 e : {itcm_end, dtcm_lo, dtcm_hi}) if (e > i && e < seg_end) seg_end = static_cast<u32>(e);
    const bool ti = i < itcm_end, td = i >= dtcm_lo && i < dtcm_hi;
    while (i < seg_end) {
      const u8 pu = pu_map_p[i];
      const u64 bw = bus_word(i >> 2);
      const u64 W = compose(&bus9_[(i >> 2) * 8], pu, ti, td);
      const u8 code = static_cast<u8>(W), data = static_cast<u8>(W >> 16);
      const u32 x = code == 0xFF ? 3u : code, y = code == 0xFF ? 1u : code;
      const u32 R = static_cast<u8>(x + y) | (static_cast<u32>(static_cast<u8>(x + x)) << 8) | (static_cast<u32>(static_cast<u8>(x + x)) << 16) | (static_cast<u32>(static_cast<u8>(x)) << 24);   // page k's refill entry when page k+1 has the same slot
      // Run: pages with this PU byte and this bus entry.
      const u64 pu8 = 0x0101010101010101ull * pu;
      u32 j = i + 1;
      while (j < seg_end) {
        if ((j & 7) == 0 && j + 8 <= seg_end) { u64 v; std::memcpy(&v, pu_map_p + j, 8); if (v == pu8 && bus_word(j >> 2) == bw && bus_word((j >> 2) + 1) == bw) { j += 8; continue; } }
        if (pu_map_p[j] != pu || ((j & 3) == 0 && bus_word(j >> 2) != bw)) break;
        ++j;
      }
      for (; i < j; ++i) {
        u8* slot = &cpu9_[i * 8];
        u64 old_w; std::memcpy(&old_w, slot, 8);
        if (old_w == W && !changed_prev) {
          x_prev = x; y_prev = y;
          while ((i & 15) == 15 && i + 17 <= j) {
            u64 acc = 0;
            for (u32 k = 1; k <= 16; ++k) { u64 o; std::memcpy(&o, &cpu9_[(i + k) * 8], 8); acc |= o ^ W; }
            if (acc) break;
            i += 16;
          }
          continue;
        }
        if (old_w != W) std::memcpy(slot, &W, 8);
        const u8 old_code = static_cast<u8>(old_w), old_data = static_cast<u8>(old_w >> 16);
        // Refill entries written only where a branch cost moved: page i-1's
        // entry [2] needs this page's cost, so each page finishes one
        // iteration late.
        const bool changed = code != old_code;
        if (i > first && (changed || changed_prev)) {
          if (x_prev == x && y_prev == y) std::memcpy(refill + (i - 1) * 4, &R, 4);
          else { u8* r = refill + (i - 1) * 4; r[0] = static_cast<u8>(x_prev + y_prev); r[1] = static_cast<u8>(x_prev + x_prev); r[2] = static_cast<u8>(x_prev + x); r[3] = static_cast<u8>(x_prev); }
        }
        x_prev = x; y_prev = y; changed_prev = changed;
        // Only bytes a translation could have baked in count as a retime.
        const u8 f = static_cast<u8>((changed ? RETIME_CODE : 0) | (data != old_data ? RETIME_DATA : 0));
        if (f && (retime_flags_[i] & f) != f) {
          if (!retime_flags_[i]) { if (retime_list_.size() < RETIME_LIST_MAX) retime_list_.push_back(i); else retime_overflow_ = true; }
          retime_flags_[i] |= f;
        }
      }
    }
  }
  // Pages at both edges of the range: the last one, and the one before
  // `first` (its entry [2] reads page `first`, which just changed).
  if (last > first && changed_prev) build_refill9(last - 1, last);
  if (first) build_refill9(first - 1, first);
  ++cpu9_version;
  if (notify) notify_cpu9(cpu);
}

void Timing::notify_cpu9(const CpuContext& cpu) {
  if (cpu.jit_timing_changed) cpu.jit_timing_changed(const_cast<CpuContext&>(cpu));
}

} // namespace ds::mem
