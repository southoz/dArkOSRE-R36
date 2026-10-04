// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/cpu/cp15.h"
#include "core/nds.h"

#include <algorithm>
#include <cstring>
#include <memory>

namespace ds {

// Rebuild the per-4 KB cacheability map from the PU regions and refresh the
// ARM9 timing table for pages that changed. Region 7 has highest priority.
void cp15_update_pu_map(CpuContext& cpu);
static void update_pu_map(CpuContext& cpu) { cp15_update_pu_map(cpu); }
namespace {
// [start, start + size) in 4 KB pages, and the cacheability bits it paints.
struct PuRegion { bool on; u32 start, size; u8 m; };
PuRegion decode_region(u32 ctl, u32 dc, u32 cc, u32 rgn, int n) {
  PuRegion r{};
  if (!(rgn & 1)) return r;
  const int size_bits = static_cast<int>((rgn >> 1) & 0x1F) - 11;   // in 4 KB pages
  r.size = size_bits <= 0 ? 1 : (size_bits >= 20 ? 0x100000 : (1u << size_bits));
  r.start = ((rgn >> 12) / r.size) * r.size;
  if ((ctl & (1u << 2)) && ((dc >> n) & 1)) r.m |= 0x10;
  if ((ctl & (1u << 12)) && ((cc >> n) & 1)) r.m |= 0x40;
  r.on = true;
  return r;
}
bool same_region(const PuRegion& a, const PuRegion& b) {
  return a.on == b.on && (!a.on || (a.start == b.start && a.size == b.size && a.m == b.m));
}
} // namespace

void cp15_update_pu_map(CpuContext& cpu) {
  mem::Timing& t = cpu.nds->bus.timing();
  u8* map = t.pu_map.get();
  const u32 ctl = cpu.cp15_control;
  mem::Timing::PuMemo& memo = t.pu_memo;
  bool changed = false;

  if ((ctl & 1) && memo.valid && (memo.ctl & 1)) {
    // PU on before and after: only the old and new extents of changed
    // regions need rescanning, avoiding a full 1 MB rebuild/compare.
    PuRegion cur[8], prev[8];
    for (int n = 0; n < 8; ++n) {
      cur[n] = decode_region(ctl, cpu.pu_data_cacheable, cpu.pu_code_cacheable, cpu.pu_region[n], n);
      prev[n] = decode_region(memo.ctl, memo.dc, memo.cc, memo.region[n], n);
    }
    auto value = [&](u32 page) -> u8 {   // highest-numbered enabled region wins
      for (int n = 7; n >= 0; --n) if (cur[n].on && page - cur[n].start < cur[n].size) return cur[n].m;
      return 0;
    };
    auto rescan = [&](const PuRegion& r) {
      if (!r.on) return;
      const u32 end = std::min<u32>(r.start + r.size, 0x100000);
      u32 run_start = 0; bool in_run = false;
      for (u32 i = r.start; i <= end; ++i) {
        const bool differs = i < end && map[i] != value(i);
        if (differs) { if (!in_run) { run_start = i; in_run = true; } map[i] = value(i); }
        else if (in_run) {
          t.update_cpu9(cpu, run_start << 12, i == 0x100000 ? 0xFFFFFFFF : (i << 12), false);
          changed = true; in_run = false;
        }
      }
    };
    for (int n = 0; n < 8; ++n) {
      if (same_region(cur[n], prev[n])) continue;
      rescan(prev[n]);
      rescan(cur[n]);
    }
  } else {
    // PU toggled, disabled, or memo stale: build the whole map and diff it.
    static std::unique_ptr<u8[]> fresh(new u8[0x100000]);
    u8* next = fresh.get();
    if (!(ctl & 1)) {
      u8 m = 0; if (ctl & (1u << 2)) m |= 0x10; if (ctl & (1u << 12)) m |= 0x40;
      std::memset(next, m, 0x100000);
    } else {
      std::memset(next, 0, 0x100000);
      for (int n = 0; n < 8; ++n) {
        const PuRegion r = decode_region(ctl, cpu.pu_data_cacheable, cpu.pu_code_cacheable, cpu.pu_region[n], n);
        if (r.on) for (u32 i = r.start; i < r.start + r.size && i < 0x100000; ++i) next[i] = r.m;
      }
    }
    constexpr u32 CHUNK = 256;
    u32 run_start = 0; bool in_run = false;
    for (u32 i = 0; i <= 0x100000; i += CHUNK) {
      const bool differs = i < 0x100000 && std::memcmp(map + i, next + i, CHUNK) != 0;
      if (differs) { if (!in_run) { run_start = i; in_run = true; } std::memcpy(map + i, next + i, CHUNK); }
      else if (in_run) {
        t.update_cpu9(cpu, run_start << 12, i == 0x100000 ? 0xFFFFFFFF : (i << 12), false);
        changed = true; in_run = false;
      }
    }
  }
  memo.valid = true; memo.ctl = ctl; memo.dc = cpu.pu_data_cacheable; memo.cc = cpu.pu_code_cacheable;
  for (int n = 0; n < 8; ++n) memo.region[n] = cpu.pu_region[n];
  if (changed) t.notify_cpu9(cpu);
}

u32 cp15_read(CpuContext& cpu, u32 opc1, u32 crn, u32 crm, u32 opc2) {
  switch ((crn << 8) | (crm << 4) | opc2) {
  case 0x000: return 0x41059461;            // main ID: ARM946E-S rev 1
  case 0x001: return 0x0F0D2112;            // cache type: 8K I / 4K D, 4-way, 32-byte lines
  case 0x002: return 0x00140180;            // TCM size: 32K ITCM, 16K DTCM
  case 0x100: return cpu.cp15_control;
  case 0x200: return cpu.pu_data_cacheable;
  case 0x201: return cpu.pu_code_cacheable;
  case 0x300: return cpu.pu_data_bufferable;
  case 0x502: return cpu.pu_data_perm;
  case 0x503: return cpu.pu_code_perm;
  case 0x600: case 0x610: case 0x620: case 0x630: case 0x640: case 0x650: case 0x660: case 0x670:
  case 0x601: case 0x611: case 0x621: case 0x631: case 0x641: case 0x651: case 0x661: case 0x671:
    return cpu.pu_region[crm];
  case 0x910: return cpu.cp15_dtcm;
  case 0x911: return cpu.cp15_itcm;
  default:    return 0;
  }
}

void cp15_write(CpuContext& cpu, u32 opc1, u32 crn, u32 crm, u32 opc2, u32 value) {
  switch ((crn << 8) | (crm << 4) | opc2) {
  case 0x100: {
    // Writable: M, big-endian, D/I-cache, V (vector base), RR, DTCM/ITCM enable/load.
    const u32 mask = 0x000FF085;
    const u32 old = cpu.cp15_control;
    cpu.cp15_control = (old & ~mask) | (value & mask) | 0x00000078;   // bits 3-6 read as 1
    if ((old ^ cpu.cp15_control) & 0x000D0000) cpu.nds->bus.update_tcm(cpu);
    if ((old ^ cpu.cp15_control) & 0x00001005) update_pu_map(cpu);    // PU / caches toggled
    return;
  }
  case 0x200: if (cpu.pu_data_cacheable != value) { cpu.pu_data_cacheable = value; update_pu_map(cpu); } return;
  case 0x201: if (cpu.pu_code_cacheable != value) { cpu.pu_code_cacheable = value; update_pu_map(cpu); } return;
  case 0x300: cpu.pu_data_bufferable = value; return;
  case 0x502: cpu.pu_data_perm = value; return;
  case 0x503: cpu.pu_code_perm = value; return;
  case 0x600: case 0x610: case 0x620: case 0x630: case 0x640: case 0x650: case 0x660: case 0x670:
  case 0x601: case 0x611: case 0x621: case 0x631: case 0x641: case 0x651: case 0x661: case 0x671:
    if (cpu.pu_region[crm] != value) { cpu.pu_region[crm] = value; update_pu_map(cpu); }
    return;
  // Games rewrite TCM registers with their current values; remap/rebuild only on real change.
  case 0x910: { const u32 v = value & 0xFFFFF03E; if (cpu.cp15_dtcm != v) { cpu.cp15_dtcm = v; cpu.nds->bus.update_tcm(cpu); } return; }
  case 0x911: { const u32 v = value & 0x0000003E; if (cpu.cp15_itcm != v) { cpu.cp15_itcm = v; cpu.nds->bus.update_tcm(cpu); } return; }
  case 0x704: case 0x782:                   // wait for interrupt
    cpu.halted = true; return;          // the run loop ends the slice
  default:
    return;                                 // PU regions, cache maintenance: accepted, ignored
  }
}

} // namespace ds
