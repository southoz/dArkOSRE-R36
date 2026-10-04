// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"

#include <chrono>
#include <vector>

namespace ds::prof {

// Coarse stage timer for headless builds (DS_PROFILE=1): wall time per stage.
enum Stage : u32 {
  CPU9, CPU7, DMA,
  GX_RUN,   // geometry command replay (Gpu3D::drain_all); "of which" of DMA/CPU9/GX_VBLANK
  BG_DRAW, OBJ_DRAW, WINDOW, SELECT, EFFECTS, OUTPUT, CAPTURE,
  R3D_CLEAR, R3D_SPANS, R3D_FINAL, R3D_WAIT, SPU,
  JIT_TX,    // "of which": nests inside CPU9/CPU7, never add to wall time
  JIT_TX_GEN, JIT_TX_INSTALL, JIT_TX_ICACHE,   // "of which" of JIT_TX: codegen, bookkeeping, the icache sync
  JIT_TX_DECODE, JIT_TX_LIVE, JIT_TX_EMIT, JIT_TX_FINISH,   // "of which" of JIT_TX_GEN
  W2D_JOIN,  // "of which": nests inside DMA/GPU_LINE/JOURNAL/JOIN0
  // Untimed bucket: leaf scopes that don't nest, so they add against wall time.
  SCHED,        // slice loop bookkeeping: deadline, idle test, budgets
  EVENTS,       // event handlers other than scanline ones: timers, DMA starts, display FIFO
  GPU_LINE,     // scanline-start/HBlank handlers, minus the pieces below and 2D draws
  JOIN0,        // waiting for the 2D worker at line 0
  BEGIN_FRAME,  // Gpu::begin_frame
  GX_VBLANK,    // Gpu3D::vblank: swap, polygon sort
  JOURNAL,      // step_engine's journal replay, window and draw latches
  R3D_LINE,     // asking the 3D raster for a line
  R3D_PREP,     // Renderer3D::render up to band dispatch
  R3D_STEAL,    // "of which": bins the caller drew instead of waiting for a worker
  COUNT
};
// DS_PROFILE=1: the stage probes (Scope) and the frame series -- cheap
// enough to read frame times through (each probe's cost is taken off).
// DS_PROFILE=2 (`heavy`): the counters and censuses too -- per DMA unit,
// per span, per slice register hashing -- which cost real frame time.
extern bool enabled, heavy;
void set_level(const char* env);   // the DS_PROFILE value, or null
// Per-access censuses (palette/OAM store counts, DS_CENSUS, DS_IO_CENSUS,
// DS_FASTMEM_CENSUS, code-page store counts) need -DDSPERATE_CENSUS=1; compiled out by default.
#ifndef DSPERATE_CENSUS
#define DSPERATE_CENSUS 0
#endif
constexpr bool census = DSPERATE_CENSUS != 0;
// A census env var: true only if set in a census build; a census-less build notes it is ignored.
bool census_env(const char* var);
// DS_ASYNC_PROBE: true during the window an async raster would be exposed to CPU writes.
extern bool async_window;
// DS_CENSUS_GX: set at swap when the list matches the previous one; read by
// band workers during the following raster. Census, not synchronisation.
extern bool census_same_list;
extern const char* const names[COUNT];
// Event counters, reported alongside the stages.
enum Counter : u32 { C_POLY_LINES, C_SPAN_PIXELS, C_RESOLVED_PIXELS, C_TEX_FAST, C_TEX_SLOW_FMT5, C_TEX_SLOW_VIEWS,
  C_TEXCACHE_HIT, C_TEXCACHE_DECODE, C_TEXCACHE_BYTES, C_R3D_FRAMES_KEPT,
  C_SLICES, C_SLICES_A9_HALTED, C_SLICES_A7_HALTED, C_SLICES_BOTH_HALTED, C_SLICES_DMA, C_SLICES_SKIPPED,
  C_CYC_TOTAL, C_CYC_BOTH_HALTED, C_CYC_A9_ONLY_HALTED, C_CYC_A7_ONLY_HALTED, C_CYC_NEITHER_HALTED,
  C_CYC_A9_SPIN, C_CYC_A7_SPIN, C_CYC_ONE_SPIN_ONE_HALTED, C_CYC_BOTH_SPIN_OR_HALTED,
  C_NS_A9_SPIN, C_NS_A7_SPIN, C_NS_A9_WORK, C_NS_A7_WORK, C_CYC_IDLE_SKIPPED, C_IDLE_NO_DMA, C_IDLE_NO_GX, C_IDLE_NO_IRQ, C_IDLE_NO_FILTER, C_IDLE_NO_LOOP9, C_IDLE_NO_LOOP7, C_IDLE_OK, C_A7_SPI_SLEEP, C_CYC_A7_SPI_SLEPT,
  // DS_IDLE_SURVEY=1: of slices the DMA veto rejected, how many would the loop analyser accept?
  C_IDLE_SURVEY_SEEN, C_IDLE_SURVEY_WOULD_SKIP, C_IDLE_SURVEY_NO_IRQ, C_IDLE_SURVEY_NO_FILTER, C_IDLE_SURVEY_NO_LOOP,
  C_2D_LINES, C_2D_BG_TEXT, C_2D_BG_AFFINE, C_2D_BG_EXT, C_2D_BG_3D, C_2D_OBJ_LINES, C_2D_WINDOW_LINES, C_2D_EFFECT_LINES, C_2D_EFFECT_LIVE, C_2D_FLAT_LINES, C_2D_SELECTS,
  C_2D_L0, C_2D_L1, C_2D_L2, C_2D_L3, C_2D_L4P, C_2D_L1_FULL, C_2D_OBJ_PRESENT, C_2D_3D_PRESENT, C_2D_WIN_PRESENT, C_2D_BG_PAL16, C_2D_BG_PAL256, C_2D_BG_EMPTY, C_2D_BG_3D_EMPTY, C_2D_FAST_BACKDROP, C_2D_FAST_ONE, C_2D_FULL_MODE, C_2D_FULL_3D, C_2D_FULL_OBJ, C_2D_FULL_SECOND, C_2D_FULL_FADE, C_SPAN_FLAT_RGB, C_SPAN_LERP_RGB, C_BAND0_NS, C_BAND1_NS, C_BAND2_NS, C_BAND3_NS, C_BAND_MAX_NS, C_BAND_SUM_NS, C_ASYNC_FRAMES, C_ASYNC_DIRTY_L0, C_ASYNC_DIRTY_SWAP, C_ASYNC_VRAMCNT_L0, C_ASYNC_VRAMCNT_SWAP, C_R3D_SYNC_ALL, C_R3D_STOLEN, C_BATCHES, C_BATCH_SPANS, C_BATCH_PX,
  // Census: how often the 3D frame resubmits unchanged (DS_CENSUS_GX=1 adds the content hash).
  C_GX_SWAP, C_GX_SWAP_SAME_CONTENT, C_GX_NOSWAP, C_GX_NOSWAP_REGS_DIFFER,
  C_GX_RD_DISPCNT, C_GX_RD_CLEAR, C_GX_RD_FOG, C_GX_RD_EDGETOON,
  C_GX_SWAP_POLYS, C_GX_SWAP_VERTS, C_GX_SWAP_MAXPOLYS, C_GX_SWAP_MAXVERTS,
  C_GX_CMP_FULL, C_GX_CMP_EARLY, C_GX_CMP_RUNS,
  C_GX_CMP_FULL_SAME, C_GX_CMP_RUNS_SAME, C_GX_CMP_FULL_DIFF, C_GX_CMP_EARLY_DIFF, C_GX_CMP_RUNS_DIFF,
  C_GX_SAME_POLYS, C_GX_SAME_VERTS,
  C_POLY_LINES_SAME, C_SPAN_PIXELS_SAME,
  // Span-length histogram: spans and their pixels by bucket (1-4, 5-8, 9-16, 17-32, 33-64, 65-128, 129-256).
  C_SL0, C_SL1, C_SL2, C_SL3, C_SL4, C_SL5, C_SL6,
  C_SLPX0, C_SLPX1, C_SLPX2, C_SLPX3, C_SLPX4, C_SLPX5, C_SLPX6,
  C_W_PALETTE, C_W_OAM,   // stores into palette/OAM space
  // Lazy 2D: frames starting batched, and VRAM-trap hits (each drops a frame to per-line rendering).
  C_2D_LAZY_FRAMES, C_2D_ENGINE_FUTILE_A, C_2D_ENGINE_FUTILE_B, C_2D_TRAP_NARROWED, C_2D_TRAP_HITS,
  C_2D_LAG_FRAMES, C_2D_LAG_STORES, C_2D_LAG_STORE_JOINS, C_2D_LAG_DROPPED, C_2D_LAG_LINES, C_2D_A_JOIN_STORES, C_2D_A_JOIN_READS,
  C_W2D_JOIN_CALLS,   // 2D worker join by call site; see Gpu::JoinSite
  // VRAMCNT remaps: whether they move a view a 2D engine reads. STILL = no
  // engine-read view moved, so the catch-up/join were not needed.
  // render_ranges' hand-off decision, split by branch taken and parked() state.
  C_RR_CALLS, C_RR_DEFER_A, C_RR_LAG, C_RR_HAND_B, C_RR_NONE,
  C_RR_PARKED, C_RR_AWAKE,
  // Runs handed over only because the worker happened to be awake, or drawn
  // inline only because it happened to be parked (flip with unrelated joins).
  C_RR_SHORT_HANDED_AWAKE, C_RR_SHORT_INLINE_PARKED,
  C_RR_LINES_INLINE, C_RR_LINES_HANDED,
  C_VRAM_REMAP, C_VRAM_REMAP_2D_MOVED, C_VRAM_REMAP_2D_STILL, C_VRAM_REMAP_PENDING,
  C_VRAM_REMAP_INFLIGHT,   // remaps taken while a 2D job is in flight (would change with a per-job VramMap snapshot)
  C_VRAM_REMAP_INFLIGHT_ALT, C_VRAM_REMAP_INFLIGHT_CAP, C_VRAM_REMAP_INFLIGHT_CLEAN,
  // Where the banks an in-flight job reads end up after the remap (a per-job map snapshot would need the write trap to still cover them):
  C_VRAM_REMAP_INFLIGHT_STAY, C_VRAM_REMAP_INFLIGHT_TO_OTHER, C_VRAM_REMAP_INFLIGHT_TO_LCDC, C_VRAM_REMAP_INFLIGHT_TO_ARM7, C_VRAM_REMAP_INFLIGHT_TO_TEX,
  C_VRAM_REMAP_SNAPSHOT,   // remaps taken without a join: the in-flight job kept its snapshot
  // Frames with alternating display setup (display_phase_period() > 1) and frames with capture on.
  C_FRAMES_PHASE_ALT, C_FRAMES_CAPTURE, C_FRAMES_TOTAL,
  C_W2D_JOIN_NS_CATCHUP, C_W2D_JOIN_NS_TRAP, C_W2D_JOIN_NS_JOURNAL,
  C_W2D_JOIN_NS_LINE0, C_W2D_JOIN_NS_REMAP, C_W2D_JOIN_NS_RPRE, C_W2D_JOIN_NS_RPOST, C_W2D_JOIN_NS_OTHER,
  C_CHUNK_ENTRIES,
  C_SPAN_EMPTY, C_SPAN_OCCLUDED, C_SPAN_DRAWN,
  // Census: geometry-engine register reads (ARM9 polling GXSTAT).
  C_GX_READ, C_GX_READ_GXSTAT, C_GX_READ_GXSTAT_BUSY,
  // Drawn spans/pixels by resolve reason, not by code path taken.
  C_RES_VEC_SPANS, C_RES_VEC_PX, C_RES_TOON_SPANS, C_RES_TOON_PX, C_RES_SHADOW_SPANS, C_RES_SHADOW_PX, C_RES_WIRE_SPANS, C_RES_WIRE_PX,
  // Census: engine2d's per-scanline extended-palette compares, and those that differed.
  C_2D_CMP_BGEXT, C_2D_CMP_OBJEXT, C_2D_CMPD_BGEXT, C_2D_CMPD_OBJEXT,
  // Census: DMA. `run` units move through a direct-mapped page-to-page run;
  // `slow` units go through the bus, one dispatch each.
  C_DMA_STARTS, C_DMA_LOOP,   // C_DMA_LOOP: outer-loop entries; a run counts once, a per-unit step counts one each
  C_DMA_GXF_WORDS, C_DMA_GXF_SLOW, C_DMA_GXF_RUNS,
  C_DMA_RUN_SEGS, C_DMA_RUN_W, C_DMA_RUN_H, C_DMA_SLOW_W, C_DMA_SLOW_H,
  C_DMA_VRAM_TRAP,
  // Units by destination zone, and VRAM traps a run took, by the same zone.
  C_DMA_D_MAIN, C_DMA_D_WRAM, C_DMA_D_PAL, C_DMA_D_OAM, C_DMA_D_IO, C_DMA_D_OTHER,
  C_DMA_D_BGA, C_DMA_D_BGB, C_DMA_D_OBJA, C_DMA_D_OBJB, C_DMA_D_LCDC,
  C_DMA_T_BGA, C_DMA_T_BGB, C_DMA_T_OBJA, C_DMA_T_OBJB, C_DMA_T_LCDC,
  // Render ranges issued per engine: batching means one a frame, per-line means one per line.
  C_2D_RANGE_A, C_2D_RANGE_B,
  C_DMA_M_IMM, C_DMA_M_VBLANK, C_DMA_M_HBLANK, C_DMA_M_DISPSTART, C_DMA_M_DISPFIFO, C_DMA_M_CART, C_DMA_M_GBA, C_DMA_M_GXFIFO, C_DMA_M_ARM7,
  // Census: uniformity of the resolve's per-lane kind decision (opaque /
  // translucent / under-layer variants) within each 8-pixel group/batch.
  C_RK_GROUPS, C_RK_EMPTY, C_RK_UNIFORM, C_RK_MIXED, C_RK_OPAQUE, C_RK_TRANS, C_RK_UNDER,
  C_RK_BATCHES, C_RK_BATCH_EMPTY, C_RK_BATCH_UNIFORM, C_RK_BATCH_MIXED, C_RK_BATCH_OPAQUE,
  C_RK_BATCH_GRP, C_RK_BATCH_UNIFORM_GRP, C_RK_BATCH_MIXED_GRP,   // batch split weighted by groups
  // Why a group drew nothing: UNDER = no lane had pass bit 0 (deferred to
  // pre-pass); TOP = passed depth, killed by alpha test.
  C_RK_EMPTY_UNDER, C_RK_EMPTY_TOP,
  // Groups where every lane draws opaque (fullscreen quad interior): dest
  // loads/bsl selects are dead work. FULL8 = whole group, FULLPX = its pixels.
  C_RK_FULL_OPAQUE, C_RK_FULL_OPAQUE_PX,
  // JIT retimes (ARM9 timing-table rebuilds): calls, and blocks each killed.
  C_JIT_INVALIDATE_CPU, C_JIT_INVALIDATE_CPU_KILLED,
  // Memory-map/timing-table rebuilds: VRAMCNT remaps, TCM/PU window updates,
  // EXMEMCNT slot retimes, and ARM9 timing-range rebuilds from any of them.
  C_BUS_UPDATE_VRAM, C_BUS_UPDATE_TCM, C_BUS_GBA_TIMING, C_TIMING_UPDATE_CPU9,
  // Geometry built for a frame never shown. DROPPED: superseded before its
  // render. SAME: identical to previous, raster skipped. CONSUMED: reached a render.
  C_GX_LIST_DROPPED, C_GX_LIST_SAME, C_GX_LIST_CONSUMED,
  // Exact idle (a stricter spin test): the CPU ends a slice at a PC it ended
  // one of the last eight at, with every register (r0-r14, CPSR) unchanged --
  // it only read. Emulated cycles, and host ns spent emulating them.
  C_CYC_A9_IDLE_EXACT, C_CYC_A7_IDLE_EXACT, C_NS_A9_IDLE_EXACT, C_NS_A7_IDLE_EXACT,
  // Drift idle: registers unchanged but the PC is elsewhere in the same loop
  // (within 256 bytes of a recent slice end) -- a poll of several addresses.
  // Counted in addition to exact idle, not including it.
  C_CYC_A9_IDLE_DRIFT, C_CYC_A7_IDLE_DRIFT, C_NS_A9_IDLE_DRIFT, C_NS_A7_IDLE_DRIFT,
  // The idle cut (Scheduler::cut_*): probes run, probes that found the CPU
  // still idle, and the slice cycles those let each CPU skip.
  C_CUT_PROBES, C_CUT_HITS, C_CYC_CUT_A9, C_CYC_CUT_A7,
  C_COUNT };
// Deduced-size definition in profile.cpp, static_assert'd against C_COUNT;
// a fixed-size [C_COUNT] array would silently pad a short initialiser with
// nullptr instead of catching a missing entry.
extern const char* const count_names[];

struct Accum {
  u64 ns[COUNT] = {};
  u64 count[C_COUNT] = {};
  u64 probes[COUNT] = {};   // Scope probes per stage, for removing their own cost (probe_ns)
  u32 tid = 0;          // registration order: 0 is the emulation thread, 1.. the band workers
};

// The probe clock: the ARM generic counter read directly (cntvct_el0, one
// instruction) where there is one, since clock_gettime through the vDSO is
// what made DS_PROFILE cost 4-6 ms a frame on the A55. Nanoseconds either way.
#if defined(__aarch64__)
namespace detail { extern u64 cnt_mul; }   // ns per tick, 32.32
inline u64 now_ns() {
  u64 t;
  asm volatile("mrs %0, cntvct_el0" : "=r"(t));
  return static_cast<u64>((static_cast<unsigned __int128>(t) * detail::cnt_mul) >> 32);
}
#else
inline u64 now_ns() { return static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count()); }
#endif
// What one Scope probe costs on this host (its two clock reads and the
// bookkeeping), measured once when profiling starts; every stage, the frame
// series and the frontend's frame times have probes x probe_ns taken off.
extern double probe_ns;
// Take the emulation thread's probe cost per frame off the frontend's frame
// samples (tail-aligned to the frame series, as frame_breakdown is). Call
// before frame_report / frame_breakdown.
void deduct_probe_overhead(std::vector<double>& frame_ms, std::vector<double>* work_ms);
namespace detail {
extern thread_local Accum* acc;
Accum* make_acc();                                  // registers a new one (once per thread)
inline Accum* get() { Accum* a = acc; return a ? a : make_acc(); }
}

inline void add(Counter c, u64 n) { if (heavy) detail::get()->count[c] += n; }
inline u64 count(Counter c) { return heavy ? detail::get()->count[c] : 0; }
inline void add_ns(Stage s, u64 n) { if (enabled) detail::get()->ns[s] += n; }
// A manual pair of now_ns() reads around `s`: counted as a probe, so its
// own cost comes off like a Scope's.
inline void add_timed(Stage s, u64 n) { if (enabled) { Accum* a = detail::get(); a->ns[s] += n; ++a->probes[s]; } }
void report();

// frame_mark() snapshots stage accumulators at a frame boundary (call where
// the frontend closes its frame_ms sample); frame_breakdown() then shows
// what the p99 frames spend time on that the report()'s whole-run view hides.
void frame_mark();
void frame_breakdown(const std::vector<double>& frame_ms);

// A stack object; `on` lets a caller skip timing some invocations without
// heap-allocating the scope conditionally.
struct Scope {
  Stage s; bool on; u64 t0;
  explicit Scope(Stage st, bool want = true) : s(st), on(want && enabled) { if (on) t0 = now_ns(); }
  ~Scope() { if (on) { Accum* a = detail::get(); a->ns[s] += now_ns() - t0; ++a->probes[s]; } }
};

} // namespace ds::prof

#define DS_PROF(stage) ::ds::prof::Scope ds_prof_scope_##stage(::ds::prof::stage)
