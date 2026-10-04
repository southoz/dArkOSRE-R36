// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Idle-loop detection: skip a CPU sitting in a polling loop to the next
// scheduled event instead of interpreting/recompiling it. Skippable requires
// no stores/MMIO/SWI/coprocessor/PSR writes/calls, all loads from
// page-table-backed RAM (loop-invariant, no writeback), and no loop-carried
// register dependency (distinguishes a poll loop from a counting delay loop).
// The structural verdict is cached per (pc, mode); load addresses are
// re-checked against live registers each time since the base can differ.
#pragma once
#include "core/types.h"

namespace ds {
struct CpuContext;
}

namespace ds::cpu {

// Which addresses a loop's loads may touch. RamOnly: backed RAM. GxstatOnly:
// RAM plus GXSTAT, requiring at least one GXSTAT load (swap-wait shape).
// SpicntOnly: RAM and SPICNT (ARM7 SPI busy poll). All: RAM plus the
// scheduled-event register set.
enum class IdlePorts : u8 { RamOnly, GxstatOnly, SpicntOnly, All };

bool in_idle_loop(CpuContext& cpu, IdlePorts ports = IdlePorts::All);

// Guest code or a memory mapping changed: drop the structural cache.
void invalidate_idle_loops();

// Diagnostics (DS_PROFILE): how the analysis is landing.
enum class IdleReject : u8 { None, Thumb, Fetch, NoBackEdge, TooLong, Store, LoadForm, BadInstr, PcWrite, Carried, NoLoad, Mmio };
const char* idle_reject_name(IdleReject r);
IdleReject idle_loop_last_reject();

struct IdleLoopStats {
  u64 queries, hits, analyses;
  u64 by_reason[12];   // indexed by IdleReject
};
const IdleLoopStats& idle_loop_stats();

}  // namespace ds::cpu
