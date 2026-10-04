// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Recompiler internals shared by the runtime and translator; private to cpu/jit.
#pragma once
#include "core/cpu/jit/jit.h"
#include "core/cpu/cpu.h"

#include <cstddef>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <deque>
#include <unordered_map>
#include <vector>

namespace ds::jit {

// ---- CpuContext offsets ---------------------------------------------------------
constexpr u32 OFF_HALTED   = offsetof(CpuContext, halted);
constexpr u32 OFF_JUMPED   = offsetof(CpuContext, jumped);
constexpr u32 OFF_REGS     = offsetof(CpuContext, hot) + offsetof(JitHot, regs);
constexpr u32 OFF_CPSR     = offsetof(CpuContext, hot) + offsetof(JitHot, cpsr);
constexpr u32 OFF_BUDGET   = offsetof(CpuContext, hot) + offsetof(JitHot, cycle_budget);
constexpr u32 OFF_IRQ      = offsetof(CpuContext, hot) + offsetof(JitHot, irq_pending);
constexpr u32 OFF_ALERTS   = offsetof(CpuContext, hot) + offsetof(JitHot, alerts);
constexpr u32 OFF_JIT      = offsetof(CpuContext, jit);
constexpr u32 OFF_JC_PT    = 0;                    // JitCpuHot::pt
constexpr u32 OFF_JC_TIM   = sizeof(void*);        // JitCpuHot::timing
constexpr u32 OFF_JC_ARENA = 2 * sizeof(void*);    // JitCpuHot::arena
constexpr u32 OFF_JC_TABLE = 3 * sizeof(void*);    // JitCpuHot::table
inline constexpr u32 off_reg(u32 r) { return OFF_REGS + 4 * r; }
// USR r13/r14 bank slots for inline `LDM ^` / `STM ^`; must stay in ldr_w reach.
constexpr u32 OFF_BANK_R13 = offsetof(CpuContext, bank_r13);
constexpr u32 OFF_BANK_R14 = offsetof(CpuContext, bank_r14);
static_assert(OFF_BANK_R13 < 16380 && OFF_BANK_R14 < 16380, "user bank slots out of ldr_w/str_w immediate range");

// Alert bits (JitHot::alerts): post-helper poll leaves the block when set.
constexpr u32 ALERT_INVALIDATED = 1;   // possibly this block
constexpr u32 ALERT_HALTED      = 2;

// ---- blocks -----------------------------------------------------------------------
// Key = guest pc | Thumb in bit 0.
inline constexpr u32 make_key(u32 pc, bool thumb) { return thumb ? (pc & ~1u) | 1u : (pc & ~3u); }
inline constexpr u32 key_pc(u32 key) { return key & ~1u; }
inline constexpr bool key_thumb(u32 key) { return key & 1; }
inline constexpr u32 key_r15(u32 key) { return key_pc(key) + (key_thumb(key) ? 4 : 8); }

// DS_JIT_DENSITY: one slot per translation (not per key).
struct DensitySlot {
  u64 execs = 0;          // bumped from translated code
  u32 hot_bytes = 0;      // instrumentation excluded
  u32 guest_instrs = 0;   // translated inline
  // DS_JIT_CENSUS (implies density): static facts, weighted by `execs`.
  u16 reg_reads[16] = {};
  u16 reg_writes[16] = {};
  u16 live_in = 0;          // read before written
  u16 written = 0;
  u8  n_instrs = 0;         // inline + fallback
  u8  n_fallback = 0;
  u8  n_mem = 0;
  u8  n_mem_flags_live = 0; // NZCV live after
  u8  n_mem_flags_intra = 0;// NZCV read later in the block
  u8  n_flags_live = 0;
  bool entry_flags_live = false;
};

constexpr u32 GUEST_COPY_MAX = 64 * 4;   // max 64 ARM instructions per block

struct Block {
  u32  key;
  u8*  entry;
  u32  size;         // native bytes
  u32  hot_size;     // cold section follows
  u32  guest_len;
  const u8* host_pages[2];   // 2 KB host pages (0-2 used)
  u32  npages;
  const u8* host_lo;         // guest code host byte range, inclusive
  const u8* host_hi;
  // Guest bytes this translation depends on beyond its own, inclusive (0, 0:
  // none): successor code read by the exit flag liveness. Writes there
  // invalidate the block; span_hash (of the whole range) is what revives it.
  u32  span_lo, span_hi;
  u64  span_hash;
  u8   owner;        // index into Runtime::cpus
  bool dead;
  bool pooled;       // in Runtime::block_pool (freed by arena reset), not heap
  // Park-and-revive: a killed block keeps code, guest bytes, overwritten entry
  // bytes and timing stamp; matching guest bytes later revive it.
  u32  entry_words[3];
  u64  stamp;
  u32  guest_copy_len;
  u8   guest_copy[GUEST_COPY_MAX];
  // ARM9 timing pages baked into this block; on overflow it dies on every retime.
  static constexpr u32 DEP_MAX = 8;
  u32  dep_page[DEP_MAX];
  u8   dep_kind[DEP_MAX];
  u8   ndep;
  bool dep_overflow;
};

// Per-CPU direct-mapped LUT indexed by key >> 1; entry = (native offset << 32) | key.
constexpr u32 LUT_BITS = 16;
constexpr u32 LUT_BITS_MAX = 16;
constexpr u32 LUT_SIZE = 1u << LUT_BITS;
constexpr u32 LUT_EMPTY_KEY = 0xFFFFFFFFu;
// Arena: [CPU0 LUT][CPU1 LUT][stubs][blocks...]; LUTs reserved at max size so stub offsets are constant.
constexpr size_t LUT_STRIDE = (size_t{1} << LUT_BITS_MAX) * 8;   // 512 KB
constexpr size_t LUT_AREA   = 2 * LUT_STRIDE;
static_assert(LUT_BITS <= LUT_BITS_MAX, "the arena only reserves room for LUT_BITS_MAX");

// Standard-layout head of JitCpu; translated code uses fixed offsets (OFF_JC_*).
struct JitCpuHot {
  mem::Entry* pt;      // page table, or (fastmem) host view base; walks then use `table`
  const u8* timing;    // timing9 (4 KB pages, 8 B/entry) or timing7 (32 KB, 4 B/entry)
  u8*       arena;     // R_ARENA
  mem::Entry* table;   // always the page table
};

struct JitCpu {
  JitCpuHot hot{};
  bool  fastmem = false;                     // hot.pt is the host view
  // 32-bit hosts: hot.pt points here; host = region[a >> 26] + a. Region 0 is
  // the view, others point into fm_guard so they fault.
  u32   fm_region[64] = {};
  CpuContext* ctx = nullptr;
  NDS*  nds = nullptr;
  bool  arm9 = false;
  u64*  lut = nullptr;                       // into the arena
  std::unordered_map<u32, Block*> blocks;
  std::vector<Block*> all_blocks;
  std::unordered_map<u32, std::vector<Block*>> parked;   // killed translations, newest last
  // Cold-code threshold (DS_JIT_WARM, opt-in): lookups per key (direct-mapped,
  // the LUT's hash) before a block is translated; below it the slice is
  // interpreted. Translating a block costs ~12 us on the A55, and a load's
  // 1000-block burst was 7 ms in one frame.
  u8 warm[LUT_SIZE] = {};
  u8*   dispatch = nullptr;                  // w0 = key
  u8*   link = nullptr;                      // `bl link; .word key`: patches the bl into `b block`
  u8*   fallback = nullptr;                  // `bl fallback; .word instr; .word key`: interpret one instr, poll, dispatch
  u8*   branch_indirect = nullptr;           // w0 = target (bit 0 = T): updates T, charges refill, dispatches
  u8*   branch_indirect_cdi = nullptr;       // same, plus LDM/POP pc's CDI charge: w1 = numD, w2 = data address
  u8*   branch_indirect_poll = nullptr;      // same as branch_indirect, then leaves if an IRQ is now takeable (exception return)
};

struct Runtime {
  u8*    arena = nullptr;
  size_t cap = 0;
  size_t pos = 0;
  size_t stubs_end = 0;        // arena below this is permanent
  bool   stubs_fast = false;   // the timing model the stubs were emitted for (g_fast_timing)
  bool   need_reset = false;   // reset at the next safe point

  void (*enter)(CpuContext*, const void*) = nullptr;
  u8* enter_light = nullptr;   // enter without saving x19-x28; caller must have
  void (*run_loop)(void*) = nullptr;   // arg: Scheduler*
  u8* exit_key = nullptr;      // w0 = key of the next instruction; stores r15, leaves
  u8* exit_key_lit = nullptr;  // `bl exit_key_lit; .word key`
  u8* exit_r15 = nullptr;      // ctx.r15 already correct; leaves
  u8* call_pure = nullptr;     // x16 = fn; spills caller-saved guest regs, flags, budget
  u8* call_full = nullptr;     // x16 = fn; spills everything, reloads everything
  u8* call2 = nullptr;         // `bl call2; .word a; .word b; .xword fn`: call_full fn(ctx, a, b)
  u8* poll = nullptr;          // `bl poll; .word next_key`: leave on budget/alert/IRQ, else return
  u8* flush_exit = nullptr;    // w0 = key; request arena reset and leave
  u8* slow_load[3] = {};       // w1 = address -> w0 = value (8/16/32); preserves x1, x7
  u8* slow_store[3] = {};      // w1 = address, w2 = value; preserves x1, x7
  u8* merge_keep_cv = nullptr; // w0 = result: N,Z from it, C,V kept
  u8* merge_set_c = nullptr;   // w0 = result, w1 = carry: N,Z from it, C from w1, V kept

  JitCpu cpus[2];

  // DS_FASTMEM: a faulting access site is patched to branch to its cold
  // page-table walk; fm_slow makes retranslations emit the walk directly.
  // fm_blocks is sorted by entry for the fault handler. Cleared with the arena.
  struct FmRel { u32 fault, patch, resume; u64 guest; };
  struct FmBlock { const u8* entry; u32 size; u32 first; u32 count; };
  std::vector<FmBlock> fm_blocks;
  std::vector<FmRel> fm_rels;
  std::unordered_set<u64> fm_slow;                  // fm_key()s
  u64 fm_slow_bits[1024] = {};                      // hash prefilter for fm_slow
  static u32 fm_bit(u64 key) { return static_cast<u32>((key * 0x9E3779B97F4A7C15ull) >> 48); }
  bool fm_is_slow(u64 key) const {
    const u32 b = fm_bit(key);
    return (fm_slow_bits[b >> 6] >> (b & 63) & 1) && fm_slow.count(key);
  }
  u8* fm_guard = nullptr;                           // 32-bit: 64 MB PROT_NONE
  // Faults, drained into fm_slow on the emulation thread (handler can't allocate).
  u64 fm_ring[512] = {};
  u32 fm_ring_n = 0;
  u64 fm_faults = 0;
  std::vector<FmRel> fm_new;    // moved to fm_rels on block install
  std::deque<Block> block_pool;   // deque: stable addresses
  size_t blocks_live = 0;         // since last reset; translate() resets at MAX_BLOCKS
  // host page -> blocks; span = lo | hi << 16 (page offsets) for store range tests.
  struct PageBlocks {
    std::vector<Block*> blocks;
    std::vector<u32> span;
    void add(Block* b, u32 lo, u32 hi) { blocks.push_back(b); span.push_back(lo | (hi << 16)); }
    void remove_at(size_t k) { blocks[k] = blocks.back(); blocks.pop_back(); span[k] = span.back(); span.pop_back(); }
    bool remove(const Block* b) { for (size_t k = 0; k < blocks.size(); ++k) if (blocks[k] == b) { remove_at(k); return true; } return false; }
    bool empty() const { return blocks.empty(); }
  };
  std::unordered_map<const u8*, PageBlocks> code_pages;
  bool trace = false;
  bool strict = false;    // budget check after every instruction (interpreter lockstep)
  bool debug = false;     // DS_JIT_DEBUG: log fallbacks
  bool cyclog = false;    // DS_DEBUG_CYCLES: log the budget after every instruction (needs strict)
  bool density = false;   // DS_JIT_DENSITY: exec-weighted native bytes per guest instruction
  bool census = false;    // DS_JIT_CENSUS (implies density)
  // Entry code embeds &slot.execs: slots must never move. Emulation thread only.
  std::deque<DensitySlot> density_slots;
  bool hist = false;      // DS_JIT_HIST: histogram of fallback executions by pc
  std::unordered_map<u64, u64> fallback_hist;
  Stats stats;
};

Runtime& rt();
inline u64 fm_key(bool arm9, u32 pc, bool thumb) { return (u64{arm9 ? 0u : 1u} << 33) | (u64{thumb} << 32) | pc; }

// Runtime services used by the translator.
void   invalidate_host_range(const u8* host_page, const u8* lo, const u8* hi);
void   invalidate_cpu(JitCpu& jc);
void   lut_insert(JitCpu& jc, Block* b);
DensitySlot* density_new_slot();                 // null unless DS_JIT_DENSITY
Block* translate(JitCpu& jc, u32 key);          // null when the arena is full
const u8* find_native(JitCpu& jc, u32 key);      // translates on miss; null when arena is full
// ---- backend (a64/, a32/) ----
namespace backend {
// Bytes the killed-block redirect overwrites; every block is at least this long.
constexpr u32 ENTRY_PATCH = 12;
void emit_stubs(Runtime& rt);
// False when out of room (caller resets the arena).
bool translate_block(JitCpu& jc, u32 key, u8* buf, size_t cap, Block& b, u32& size);
void write_entry_redirect(u8* entry, u32 key, const u8* dispatch);
bool fastmem_capable();
void patch_link(u8* site, const u8* target);
}

// Helpers called from translated code (through the stubs).
extern "C" {
u32         jit_h_fallback(CpuContext* cpu, u32 instr, u32 key);   // returns cpu->jumped
// DSi ARM9 BIOS SHA-1 loop: such blocks begin with a fallback carrying this
// marker (an undefined instruction), which runs it natively or declines.
constexpr u32 BIOS_SHA1_MARKER = 0xE7F5A1F0;
bool bios_sha1_hook_wanted(CpuContext& cpu, u32 pc, bool thumb);
bool bios_sha1_run(CpuContext& cpu);
// ARM7 BIOS WaitByLoop head (cpu/wait_loop.h): the same shape of hook.
constexpr u32 WAIT_LOOP_MARKER = 0xE7F5A1F1;
const void* jit_h_lookup(CpuContext* cpu, u32 key);
const void* jit_h_link(CpuContext* cpu, u32 key, u8* patch_site);
void        jit_h_trace(CpuContext* cpu, u32 instr, u32 key);
void        jit_h_cyclog(CpuContext* cpu, u32 instr, u32 key);
void        jit_h_msr_cpsr(CpuContext* cpu, u32 value, u32 mask);
u32         jit_h_exc_return(CpuContext* cpu, u32 target);
u32         jit_h_ld8(CpuContext* cpu, u32 addr);
u32         jit_h_ld16(CpuContext* cpu, u32 addr);
u32         jit_h_ld32(CpuContext* cpu, u32 addr);
void        jit_h_st8(CpuContext* cpu, u32 addr, u32 v);
void        jit_h_st16(CpuContext* cpu, u32 addr, u32 v);
void        jit_h_st32(CpuContext* cpu, u32 addr, u32 v);
}

inline void sync_icache(u8* start, size_t len) { __builtin___clear_cache(reinterpret_cast<char*>(start), reinterpret_cast<char*>(start + len)); }

} // namespace ds::jit
