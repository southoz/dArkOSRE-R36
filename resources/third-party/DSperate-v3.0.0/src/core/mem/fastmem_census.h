// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// DS_FASTMEM_CENSUS=1 (census builds): measures what mapping guest memory into host address
// space would meet, on the running workload, before any of it is built.
// Access census (--interp): classifies each access as direct or a fault
// (and why); a fault permanently rewrites its site to walk the page table.
// Churn census (any run): page-table entry changes outside VRAM, code-tag
// set/clear -- the mmap/mprotect calls a view would need to mirror.
#pragma once
#include "core/types.h"
#include "core/profile.h"

namespace ds {
struct CpuContext;
}

namespace ds::mem::fmc {

extern bool g_on;              // DS_FASTMEM_CENSUS, read once at startup
inline bool on() { return prof::census && g_on; }
void init();                   // reads the environment; call once before running

void access(CpuContext& cpu, u32 addr, bool store);
void fetch(CpuContext& cpu, u32 addr);
// Page-table write: `guest_page` whose entry changed from `old_e` to `new_e`.
void entry_changed(u32 guest_page, uintptr_t old_e, uintptr_t new_e);
void code_tag(bool on);

// Counting window: accesses and churn outside it are not counted, but the
// per-site rewrite state persists (a site rewritten in warm-up stays rewritten).
void set_counting(bool on);
void report(u64 frames);

} // namespace ds::mem::fmc
