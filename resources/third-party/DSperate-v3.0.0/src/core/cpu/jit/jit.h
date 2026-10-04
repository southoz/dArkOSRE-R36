// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// ARM -> AArch64 recompiler: public interface.
#pragma once
#include "core/cpu/cpu.h"

#include <cstdio>

namespace ds { struct NDS; }

namespace ds::jit {

struct Stats {
  u64 blocks_translated = 0;
  u64 instrs_translated = 0;   // emitted inline
  u64 instrs_fallback = 0;     // routed to the interpreter helper
  u64 blocks_invalidated = 0;
  u64 flushes = 0;
  u64 entries = 0;             // native entries (one per scheduler slice at most)
  u64 code_bytes = 0;          // native bytes emitted (cold sections included)
  u64 hot_bytes = 0;           // of which hot-section (fast path) bytes
  u64 slow_accesses = 0;       // loads/stores that left the inline page-table path
  u64 blocks_revived = 0;      // killed blocks whose guest bytes matched again
  u64 bios_sha1_blocks = 0;    // DSi BIOS SHA-1 blocks run natively
  u64 wait_loop_runs = 0;      // ARM7 BIOS WaitByLoop heads fast-forwarded
  u64 slices_interpreted = 0;  // slices the interpreter ran for cold code (DS_JIT_WARM)
};

// Safe to call once per NDS; `trace` mirrors NDS::trace at attach time
// (toggling tracing later requires `set_trace`).
bool attach(NDS& nds, bool arm9, bool arm7);
void detach(NDS& nds);

// RunFn entry: runs `cpu` until its budget is exhausted or it halts.
void run(CpuContext& cpu);

// Native slice loop: drives Scheduler::slice_next from a loop in the code
// arena, saving callee-saved regs once and skipping the per-entry frame.
// `lookup` is the block lookup run() does before each entry.
const void* lookup(CpuContext& cpu);
bool has_runtime();
void run_loop(void* scheduler);

// Drop every translated block of `cpu`. Cheap to call; translation is lazy.
void flush(CpuContext& cpu);
void flush_all();

// Per-instruction tracing: when enabled, every translated instruction calls
// NDS::trace before executing, exactly as the interpreter does.
void set_trace(bool on);

// DS_JIT_STRICT: per-instruction budget checks, lockstep with the
// interpreter. Slower, exact. Flushes both CPUs on change.
void set_strict(bool on);

// DS_JIT_DENSITY: zero the executed-density counters (not the translations
// themselves), so a run can exclude the frames that only warm the code cache.
void density_reset();

const Stats& stats();
// Print the counters (and, with DS_JIT_HIST=1, the hottest fallback sites).
void report(std::FILE* out);

} // namespace ds::jit
