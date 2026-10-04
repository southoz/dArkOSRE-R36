// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/gpu/gpu.h"
#include <chrono>
#include "core/state/state.h"
#include "core/gpu/vram_map.h"
#include "core/gpu/kernels.h"
#include "core/nds.h"
#include "core/profile.h"

#include <cstdio>
#include <cmath>
#include <cstring>
#include <cstdlib>

// pow() for the gamma LUT below; pins a glibc symbol version for a low aarch64 runtime floor.
extern "C" double ds_pow_compat(double x, double y);

namespace ds::gpu {

// Knobs read once in the constructor.
namespace {
bool g_dbg_join = false, g_dbg_gpu = false, g_dbg_vramnz = false, g_dbg_skip = false;
bool g_no_lazy = false, g_no_lag = false;
const char* g_dump_frame = nullptr;
void read_knobs() {
  static bool done = false;
  if (done) return;
  done = true;
  g_dbg_join = std::getenv("DS_DEBUG_JOIN") != nullptr;
  g_dbg_gpu = std::getenv("DS_DEBUG_GPU") != nullptr;
  g_dbg_vramnz = std::getenv("DS_DEBUG_VRAMNZ") != nullptr;
  g_dbg_skip = std::getenv("DS_DEBUG_SKIP") != nullptr;
  g_dump_frame = std::getenv("DS_DEBUG_DUMP_FRAME");
  g_no_lazy = std::getenv("DS_DEBUG_NOLAZY") != nullptr;   // every frame per-line (the lazy batch's reference)
  g_no_lag = std::getenv("DS_DEBUG_NOLAG") != nullptr;     // per-line lines never stay in flight past their HBlank
}
}  // namespace

static void ev_scanline(NDS& nds, u32) { nds.gpu.on_scanline_start(); }
static void ev_hblank(NDS& nds, u32)   { nds.gpu.on_hblank(); }
static void ev_fifo(NDS& nds, u32 x)   { nds.gpu.on_display_fifo(x); }

Gpu::Gpu(NDS& nds) : engine{Engine2D(nds, 0), Engine2D(nds, 1)}, nds_(nds) {
  read_knobs();
  split_ = thread_layout_on() || std::getenv("DS_2D_BWORKER");
  worker_.start(&Gpu::worker_job, this, split_ ? ThreadRole::EngineA : ThreadRole::Line, split_ ? "2d-engine-a" : "line-worker");
  if (split_) worker_b_.start(&Gpu::worker_b_job, this, ThreadRole::Line, "2d-engine-b");
}

void Gpu::reset() {
  join_worker();
  disarm_trap();
  line_ = 0; hblank_done_ = false;
  lazy_frame_ = false; per_line_[0] = per_line_[1] = false;
  render_next_[0] = render_next_[1] = SCREEN_H; frame_finished_ = false;
  frame_begun_ = false; screens_on_ = false;
  master_bright_g_[0] = master_bright_g_[1] = 0;
  capcnt_ = 0; capture_on_ = false;
  fifo_.fill(0); fifo_rd_ = fifo_wr_ = 0; fifo_line_.fill(0); run_fifo_ = false;
  for (auto& fb : fb_) fb.fill(0);
  engine[0].reset(); engine[1].reset();
  set_powcnt(nds_.io.powcnt1);
  nds_.io.set_vcount(0);
  nds_.sched.schedule(EventId::HBlank, nds_.sched.now() + HBLANK_START, ev_hblank);
}

// ---- registers --------------------------------------------------------------

u32 Gpu::reg_read(u32 addr, u32 width) {
  const u32 r = addr - 0x04000000;
  if (r >= 0x64 && r < 0x70) {
    auto rd16 = [&](u32 a) -> u32 {
      switch (a) {
      case 0x64: return capcnt_ & 0xFFFF;
      case 0x66: return capcnt_ >> 16;
      case 0x6C: return master_bright_g_[0];
      default: return 0;
      }
    };
    if (width == 32) return rd16(r) | (rd16(r + 2) << 16);
    if (width == 16) return rd16(r);
    return (rd16(r & ~1u) >> ((r & 1) * 8)) & 0xFF;
  }
  if (r >= 0x1064 && r < 0x1070) {
    const u32 v = (r & ~1u) == 0x106C ? master_bright_g_[1] : 0;
    return width == 8 ? (v >> ((r & 1) * 8)) & 0xFF : v;
  }
  return engine[r >= 0x1000].read(addr, width);
}

void Gpu::reg_write(u32 addr, u32 width, u32 value) {
  const u32 r = addr - 0x04000000;
  // MASTER_BRIGHT: guest copy here, render side via the journal.
  auto mb = [&](int e, u16 v) { master_bright_g_[e] = v; engine[e].master_bright_write(v); };
  if (r >= 0x64 && r < 0x70) {
    if (width == 32) {
      switch (r) {
      case 0x64: capcnt_ = value & 0xEF3F1F1F; return;
      case 0x68: fifo_[fifo_wr_] = value & 0xFFFF; fifo_[fifo_wr_ + 1] = value >> 16; fifo_wr_ = (fifo_wr_ + 2) & 0xF; return;
      case 0x6C: mb(0, value & 0xC01F); return;
      default: return;
      }
    }
    if (width == 16) {
      switch (r) {
      case 0x64: capcnt_ = (capcnt_ & 0xFFFF0000) | (value & 0x1F1F); return;
      case 0x66: capcnt_ = (capcnt_ & 0x0000FFFF) | ((value & 0xEF3F) << 16); return;
      case 0x68: fifo_[fifo_wr_] = value; return;
      case 0x6A: fifo_[fifo_wr_ + 1] = value; fifo_wr_ = (fifo_wr_ + 2) & 0xF; return;   // the write pointer advances on the high half
      case 0x6C: mb(0, value & 0xC01F); return;
      default: return;
      }
    }
    switch (r) {
    case 0x64: capcnt_ = (capcnt_ & 0xFFFFFF00) | (value & 0x1F); return;
    case 0x65: capcnt_ = (capcnt_ & 0xFFFF00FF) | ((value & 0x1F) << 8); return;
    case 0x66: capcnt_ = (capcnt_ & 0xFF00FFFF) | ((value & 0x3F) << 16); return;
    case 0x67: capcnt_ = (capcnt_ & 0x00FFFFFF) | ((value & 0xEF) << 24); return;
    case 0x68: fifo_[fifo_wr_] = static_cast<u16>(value * 0x0101); return;
    case 0x6A: fifo_[fifo_wr_ + 1] = static_cast<u16>(value * 0x0101); return;
    case 0x6B: fifo_wr_ = (fifo_wr_ + 2) & 0xF; return;
    case 0x6C: mb(0, (master_bright_g_[0] & 0xFF00) | (value & 0x1F)); return;
    case 0x6D: mb(0, (master_bright_g_[0] & 0x00FF) | ((value & 0xC0) << 8)); return;
    default: return;
    }
  }
  if (r >= 0x1064 && r < 0x1070) {
    if (r == 0x106C && width >= 16) mb(1, value & 0xC01F);
    else if (r == 0x106C) mb(1, (master_bright_g_[1] & 0xFF00) | (value & 0x1F));
    else if (r == 0x106D) mb(1, (master_bright_g_[1] & 0x00FF) | ((value & 0xC0) << 8));
    return;
  }
  const int e = r >= 0x1000;
  engine[e].write(addr, width, value);
  // Engine A switching to VRAM display mid-frame starts reading an LCDC bank
  // the trap does not cover: render the rest of the frame per line.
  if (e == 0 && (r & 0xFFF) < 4 && trap_armed_ && !trap_lcdc_ && ((engine[0].read(0x04000000, 32) >> 16) & 3) == 2) fall_back_per_line(3);
}

void Gpu::set_powcnt(u16 value) {
  engine[0].powcnt_write(value);
  engine[1].powcnt_write(value);
  nds_.gpu3d.set_powcnt(value);
}

// ---- slow-path stores and the VRAM trap ---------------------------------------

// Guest bytes change now, engine copy via the journal. A store of the value already there is not journaled.
void Gpu::palette_store(Cpu cpu, u32 addr, u32 width, u32 value) {
  const u32 off = addr & 0x7FF, n = width / 8;
  u8* host = nds_.bus.palette.get() + off;
  if (std::memcmp(host, &value, n) == 0) return;
  if (nds_.cpu(cpu).page_table.entry(addr) & mem::TAG_CODE) mem::store_code(host, &value, n); else std::memcpy(host, &value, n);
  engine[off >> 10].palette_written(off & 0x3FF, width, value);
}
void Gpu::oam_store(Cpu cpu, u32 addr, u32 width, u32 value) {
  const u32 off = addr & 0x7FF, n = width / 8;
  u8* host = nds_.bus.oam.get() + off;
  if (std::memcmp(host, &value, n) == 0) return;
  if (nds_.cpu(cpu).page_table.entry(addr) & mem::TAG_CODE) mem::store_code(host, &value, n); else std::memcpy(host, &value, n);
  engine[off >> 10].oam_written(off & 0x3FF, width, value);
}

// A store into VRAM the engines can see, before it lands. Only ARM9 reaches
// the engine windows; the LCDC window matters only while it is displayed.
void Gpu::vram_store_trap(Cpu cpu, u32 addr) {
  if (!trap_armed_ || cpu != Cpu::ARM9) return;
  if (addr >= 0x06800000 && !trap_lcdc_) return;
  // A's deferred batch: a store reaching what it reads joins it, finishing
  // the frame and lifting the trap. B's windows only when B is deferred too.
  const u32 stale = snapshot_stale_ ? 3u : 0u;
  if (a_deferred_) {
    if ((reach_engines(addr) | stale) & (b_deferred_ ? 3u : 1u)) { prof::add(prof::C_2D_A_JOIN_STORES, 1); join_worker(JoinSite::Trap); }
    return;
  }
  // Charged to the engine its address reaches (both for anything unattributed).
  const u32 mask = reach_engines(addr) | stale;
  // Already per line everywhere it reaches: nothing to do but the lag-mode join below.
  if ((per_line_[0] || !(mask & 1)) && (per_line_[1] || !(mask & 2))) {
    // Lag mode: the store may land on a line engine B is still drawing.
    prof::add(prof::C_2D_LAG_STORES, 1);
    const u32 reach = reach_engines(addr) | stale;
    const bool b_joined = ((reach & 1) && inflight_[0]) || ((reach & 2) && inflight_[1]);
    if (b_joined) { prof::add(prof::C_2D_LAG_STORE_JOINS, 1); join_worker(JoinSite::Trap); }
    // Past either limit, drop lag and lift the trap unless the other engine is still
    // batching. Nothing may stay in flight once the trap is gone.
    const bool over = (b_joined && ++lag_trap_hits_ >= LAG_TRAP_LIMIT) || ++lag_trap_stores_ >= LAG_STORE_LIMIT;
    if (over && lag_frame_) {
      if (!b_joined) join_worker(JoinSite::Trap);
      lag_frame_ = false;
      arm_trap();   // the trap now guards only a batching engine's windows, if any
      prof::add(prof::C_2D_LAG_DROPPED, 1);
    }
    return;
  }
  prof::add(prof::C_2D_TRAP_HITS, 1);
  // One engine per-line with a lag line in flight, the other batching: the
  // catch-up below only joins if the batching engine has lines due, so a store
  // reaching the in-flight line waits for it here.
  if (inflight_[0] || inflight_[1]) {
    const u32 reach = reach_engines(addr) | stale;
    if (((reach & 1) && inflight_[0]) || ((reach & 2) && inflight_[1])) join_worker(JoinSite::Trap);
  }
  // The budget is per engine: one engine streaming tiles must not spend the other's.
  u32 burst_mask = 0;
  for (int e = 0; e < 2; ++e)
    if ((mask & (1u << e)) && !per_line_[e] && ++lazy_bursts_[e] < LAZY_BURST_LIMIT) burst_mask |= 1u << e;
  if (burst_mask) {
    catch_up(burst_mask);
    for (int e = 0; e < 2; ++e)
      if (burst_mask & (1u << e)) { per_line_[e] = true; burst_[e] = true; burst_left_[e] = LAZY_BURST_LINES; }
    arm_trap();   // a bursting engine's windows need no trap until its burst ends
    return;
  }
  fall_back_per_line(mask);
}

// `moved_2d`: engines whose read views this remap actually moves. CENSUS
// ONLY for now -- the catch-up below is still unconditional.
bool Gpu::vram_remap_begin(u32 moved_2d, const VramMap* next) {
  // A job in flight keeps rendering against its snapshot; the live map may
  // change under it as long as the write trap still covers every bank it
  // reads at the bank's new address: an engine window that is trapped, or
  // LCDC while LCDC is trapped. A bank leaving for the ARM7 side or an
  // untrapped window joins as before (the ARM7 is never trapped).
  static const bool snap_off = std::getenv("DS_SNAP") && std::getenv("DS_SNAP")[0] == '0';   // debugging: always join
  static const bool snap_capjoin = std::getenv("DS_SNAP_CAPJOIN") != nullptr;             // debugging: join under a capture
  if (!snap_off && next && lines_in_flight() && trap_armed_) {
    const VramMap& cur = nds_.bus.vram_map();
    auto banks_of = [](const VramView& v) { u32 m = 0; for (u32 b = 0; b < v.blocks(); ++b) m |= v.mask[b]; return m; };
    u32 old_banks = 0;
    if (inflight_[0]) old_banks |= banks_of(cur.abg) | banks_of(cur.aobj) | banks_of(cur.abg_extpal) | banks_of(cur.aobj_extpal) | cur.lcdc_mask;
    if (inflight_[1]) old_banks |= banks_of(cur.bbg) | banks_of(cur.bobj) | banks_of(cur.bbg_extpal) | banks_of(cur.bobj_extpal);
    const u32 na = banks_of(next->abg) | banks_of(next->aobj) | banks_of(next->abg_extpal) | banks_of(next->aobj_extpal);
    const u32 nb = banks_of(next->bbg) | banks_of(next->bobj) | banks_of(next->bbg_extpal) | banks_of(next->bobj_extpal);
    const u32 n7 = banks_of(next->arm7), ntex = banks_of(next->texture) | banks_of(next->texpal);
    bool covered = true;
    // A capture in flight is still writing its bank. The ARM9 is kept off it
    // by the read trap (re-applied over the new mapping after the remap); an
    // engine in flight reads its own snapshot, where the bank is still LCDC.
    // The ARM7, the texture units and an engine rendering inline against the
    // live map cannot be kept off it, so those cases join.
    if (capture_render_) {
      const u32 cap = 1u << ((capcnt_render_ >> 16) & 3);
      if (((n7 | ntex) & cap) || ((na & cap) && !inflight_[0]) || ((nb & cap) && !inflight_[1]) || snap_capjoin) covered = false;
    }
    for (u32 b = 0; b < 9 && covered; ++b) {
      const u32 bit = 1u << b;
      if (!(old_banks & bit)) continue;
      if (n7 & bit) covered = false;
      if ((na & bit) && !(trap_mask_ & 1)) covered = false;
      if ((nb & bit) && !(trap_mask_ & 2)) covered = false;
      if ((next->lcdc_mask & bit) && !trap_lcdc_) covered = false;
    }
    static const bool snap_log = std::getenv("DS_SNAP_LOG") != nullptr;
    if (snap_log) std::fprintf(stderr, "[snap] frame %llu line %u remap covered=%d cap=%d capbank=%u inflight=%d%d moved=%u lcdcnext=%02x\n", (unsigned long long)nds_.frame_count, line_, covered ? 1 : 0, capture_render_ ? 1 : 0, (capcnt_render_ >> 16) & 3, inflight_[0] ? 1 : 0, inflight_[1] ? 1 : 0, moved_2d, next->lcdc_mask);
    if (covered) {
      prof::add(prof::C_VRAM_REMAP_SNAPSHOT, 1);
      // An engine still in flight owes its cache reset at the join (its job
      // reads the snapshot); one that is not renders inline against the live
      // map and takes it now.
      for (int e = 0; e < 2; ++e) { if (inflight_[e]) remap_pending_[e] = true; else engine[e].vram_remapped(); }
      snapshot_stale_ = true;
      // The read trap's saved entries would go stale under the rebuild: lift it now, back after.
      if (read_trap_bank_ >= 0) {
        if (read_trap_generic_) nds_.bus.set_bank_read_trap(read_trap_bank_, false); else nds_.bus.set_lcdc_read_trap(read_trap_bank_, false);
        read_trap_generic_ = false; read_trap_reapply_ = true;
      }
      catch_up(3);   // lines still due below the frontier render now, against the map they were scanned with
      return false;  // the trap stays as it is; vram_remap_end re-applies it over the remapped pages
    }
  }
  if (prof::enabled) {
    prof::add(prof::C_VRAM_REMAP, 1);
    prof::add(moved_2d ? prof::C_VRAM_REMAP_2D_MOVED : prof::C_VRAM_REMAP_2D_STILL, 1);
    if (lines_in_flight()) {
      prof::add(prof::C_VRAM_REMAP_INFLIGHT, 1);
      // capture_on_ is cleared at VBlank before remaps land, so it says nothing here;
      // what matters is whether the in-flight job is rendering a capture (capture_render_).
      const bool alt = phase_period_ > 1, cap = capture_render_;
      if (alt) prof::add(prof::C_VRAM_REMAP_INFLIGHT_ALT, 1);
      if (cap) prof::add(prof::C_VRAM_REMAP_INFLIGHT_CAP, 1);
      if (!alt && !cap) prof::add(prof::C_VRAM_REMAP_INFLIGHT_CLEAN, 1);
    }
    const u32 f = frontier();
    if (f && ((render_next_[0] < f && render_next_[0] < SCREEN_H) ||
              (render_next_[1] < f && render_next_[1] < SCREEN_H)))
      prof::add(prof::C_VRAM_REMAP_PENDING, 1);
  }
  catch_up(3);
  join_worker(JoinSite::Remap);
  engine[0].vram_remapped(); engine[1].vram_remapped();
  const bool was = trap_armed_;
  if (was) disarm_trap();
  return was;
}
void Gpu::vram_remap_end(bool trapped) {
  if (trapped) { arm_trap(); return; }
  // Kept armed across the remap (a job in flight on its snapshot): the page
  // table just rebuilt dropped the trap bits of every page it touched. The
  // capture read trap comes back in its generic form, over every page the
  // bank backs now -- the guest reads it through the new mapping otherwise.
  if (trap_armed_) nds_.bus.set_vram_trap(true, trap_lcdc_, trap_mask_);
  if (read_trap_reapply_) {
    read_trap_reapply_ = false;
    if (read_trap_bank_ >= 0) { nds_.bus.set_bank_read_trap(read_trap_bank_, true); read_trap_generic_ = true; }
  }
}

void Gpu::capture_read_hit() {
  prof::add(prof::C_2D_A_JOIN_READS, 1);
  static const bool snap_log = std::getenv("DS_SNAP_LOG") != nullptr;
  if (snap_log) std::fprintf(stderr, "[snap] frame %llu line %u capture read hit\n", (unsigned long long)nds_.frame_count, line_);
  join_worker(JoinSite::Trap);
}

void Gpu::release_read_trap() {
  if (read_trap_bank_ < 0) return;
  if (read_trap_generic_) nds_.bus.set_bank_read_trap(read_trap_bank_, false);
  else nds_.bus.set_lcdc_read_trap(read_trap_bank_, false);
  read_trap_bank_ = -1; read_trap_generic_ = false; read_trap_reapply_ = false;
}

void Gpu::snapshot_map(int k) {
  const VramMap& live = nds_.bus.vram_map();
  if (job_map_gen_[k] == live.generation()) return;
  job_map_[k] = live;
  job_map_gen_[k] = live.generation();
}

void Gpu::arm_trap() {
  // Windows that need it: a batching engine's (its stores must render the lines
  // before them first), and engine A's in a lag frame (its per-line line is in
  // flight). A per-line engine renders each line at its own HBlank.
  u32 m = lag_frame_ ? 1u : 0u;
  if (lazy_frame_) { if (!per_line_[0]) m |= 1; if (!per_line_[1]) m |= 2; }
  if (!m) { disarm_trap(); return; }
  // LCDC banks trapped when engine A displays one, or a capture writes one (A's reach).
  const bool lcdc = (m & 1) && (((engine[0].dispcnt() >> 16) & 3) == 2 || capture_on_);
  if (trap_armed_) {
    if (m == trap_mask_ && lcdc == trap_lcdc_) return;
    // Clear the old ranges first, or pages the new arming leaves out stay trapped.
    nds_.bus.set_vram_trap(false, trap_lcdc_, trap_mask_);
    prof::add(prof::C_2D_TRAP_NARROWED, 1);
  }
  trap_lcdc_ = lcdc; trap_mask_ = m;
  nds_.bus.set_vram_trap(true, trap_lcdc_, trap_mask_);
  trap_armed_ = true;
}
void Gpu::disarm_trap() {
  if (!trap_armed_) return;
  nds_.bus.set_vram_trap(false, trap_lcdc_, trap_mask_);
  trap_armed_ = false;
}


void Gpu::catch_up(u32 mask) {
  const u32 f = frontier();
  if (f == 0) return;
  const u32 last = (f < SCREEN_H ? f : SCREEN_H) - 1;
  const bool a = (mask & 1) && render_next_[0] <= last && render_next_[0] < SCREEN_H;
  const bool b = (mask & 2) && render_next_[1] <= last && render_next_[1] < SCREEN_H;
  if (!a && !b) return;
  render_ranges(a ? render_next_[0] : 1, a ? last : 0,
                b ? render_next_[1] : 1, b ? last : 0);
  join_worker(JoinSite::CatchUp);
}
void Gpu::fall_back_per_line(u32 mask) {
  catch_up(mask);
  for (int e = 0; e < 2; ++e) if (mask & (1u << e)) { per_line_[e] = true; burst_[e] = false; }
  arm_trap();   // guards what still batches (and a lag frame's in-flight line); lifted when nothing does
}

// ---- timing -----------------------------------------------------------------

void Gpu::on_hblank() {
  prof::Scope hook(prof::GPU_LINE);
  nds_.io.set_hblank(true);
  const bool frame_reset = line_ == 262;
  if (line_ < SCREEN_H) {
    // Rendered now per-line, or all together at the last one. Both engines
    // are always in the same mode.
    u32 f[2], l[2];
    for (int e = 0; e < 2; ++e) {
      const bool batch = lazy_frame_ && !per_line_[e];
      if (batch && line_ != SCREEN_H - 1) { f[e] = 1; l[e] = 0; continue; }   // nothing yet
      f[e] = batch ? render_next_[e] : line_;
      l[e] = batch ? SCREEN_H - 1 : line_;
      if (!batch && render_next_[e] < line_) f[e] = render_next_[e];          // catch up anything skipped
    }
    // Draws account for themselves; subtracted from this hook's time. The
    // worker join in render_ranges has no scope of its own, so it's added back rather than subtracted.
    const u64 t_draw0 = prof::enabled ? prof::now_ns() : 0;
    const u64 join0 = prof::enabled ? join_wait_ns_ : 0;
    render_ranges(f[0], l[0], f[1], l[1]);
    if (prof::enabled)
      prof::add_timed(prof::GPU_LINE, (t_draw0 - prof::now_ns()) + (join_wait_ns_ - join0));   // wraps: a subtraction from the hook's own scope
    // End of a burst window: the lines after this one batch again, trapped.
    for (int e = 0; e < 2; ++e)
      if (burst_[e] && --burst_left_[e] == 0 && line_ < SCREEN_H - 1) {
        burst_[e] = false; per_line_[e] = false; render_next_[e] = line_ + 1; arm_trap();
      }
    hblank_done_ = true;
    nds_.dma.check(Cpu::ARM9, dma::MODE9_HBLANK);
  } else {
    hblank_done_ = true;
    engine[0].latch(Engine2D::L_PREDRAW, line_, frame_reset);
    engine[1].latch(Engine2D::L_PREDRAW, line_, frame_reset);
    if (line_ == 215) {
      // 3D flushed at VBlank rasterises now, ahead of the next frame's display
      // lines, so frameskip is decided here for begin_frame to latch.
      skip_next_ = skip_req_ && skippable();
      const u64 t_r0 = prof::enabled ? prof::now_ns() : 0;
      if (!skip_next_) nds_.gpu3d.render_frame(); else nds_.gpu3d.note_raster_skipped();
      if (prof::enabled) prof::add_timed(prof::GPU_LINE, t_r0 - prof::now_ns());   // wraps, as above
      if (probe_enabled_) async_probe_start();
    } else if (line_ == 262) {
      engine[0].latch(Engine2D::L_SPRITES, 0, false); engine[1].latch(Engine2D::L_SPRITES, 0, false);
      // An engine's skip flag resets at the join while its lines are in flight (finish_a, join_worker).
      if (!inflight_[0]) skipped_[0] = false;
      if (!inflight_[1]) skipped_[1] = false;
    }
    engine[0].latch(Engine2D::L_POSTDRAW, line_, frame_reset);
    engine[1].latch(Engine2D::L_POSTDRAW, line_, frame_reset);
  }
  nds_.sched.schedule(EventId::VBlank_Scanline, nds_.sched.event_time() + (CYCLES_PER_SCANLINE - HBLANK_START), ev_scanline);
}

// ---- async-raster probe (DS_ASYNC_PROBE=1, measurement only) ----------------
// Hashes texture/texture-palette VRAM at line 215 and at two join deadlines
// (line 0 next frame, line 192 swap): a change means an async raster worker
// would have read bytes the CPU was writing.
namespace {
u64 hash_view(const VramView& v) {
  u64 h = 0xcbf29ce484222325ull;
  for (u32 b = 0; b < v.blocks(); ++b) {
    const u8* p = v.ptr[b];
    if (!p) { h = (h ^ 0x9e37) * 0x100000001b3ull; continue; }
    for (u32 o = 0; o < VramView::BLOCK; o += 8) {
      u64 w; std::memcpy(&w, p + o, 8);
      h = (h ^ w) * 0x100000001b3ull;
    }
  }
  return h;
}
u64 hash_tex_vram(const VramMap& vm) { return hash_view(vm.texture) * 31 + hash_view(vm.texpal); }
} // namespace

void Gpu::async_probe_start() {
  const VramMap& vm = nds_.bus.vram_map();
  probe_hash_ = hash_tex_vram(vm);
  probe_open_ = true;
  probe_vramcnt_at_l0_ = 0;
  prof::async_window = true;
  prof::add(prof::C_ASYNC_FRAMES, 1);
}

void Gpu::async_probe_check(bool at_line0) {
  if (!probe_open_) return;
  const bool dirty = hash_tex_vram(nds_.bus.vram_map()) != probe_hash_;
  if (at_line0) {
    if (dirty) prof::add(prof::C_ASYNC_DIRTY_L0, 1);
    probe_vramcnt_at_l0_ = prof::count(prof::C_ASYNC_VRAMCNT_SWAP);
    if (probe_vramcnt_at_l0_ != probe_vramcnt_base_) prof::add(prof::C_ASYNC_VRAMCNT_L0, 1);
    return;
  }
  if (dirty) prof::add(prof::C_ASYNC_DIRTY_SWAP, 1);
  probe_open_ = false;
  prof::async_window = false;
  probe_vramcnt_base_ = prof::count(prof::C_ASYNC_VRAMCNT_SWAP);
}

void Gpu::on_scanline_start() {
  prof::Scope hook(prof::GPU_LINE);
  nds_.io.set_hblank(false);
  line_ = static_cast<u16>((line_ + 1) % SCANLINES_PER_FRAME);
  hblank_done_ = false;

  // Display lines evaluate window edges in step_engine; the rest go through the latch.
  if (line_ >= SCREEN_H) { engine[0].latch(Engine2D::L_WINDOWS, line_, false); engine[1].latch(Engine2D::L_WINDOWS, line_, false); }
  if (line_ == 0) {
    // All display lines must be drawn before the frontend reads them and before begin_frame runs.
    { prof::Scope j(prof::JOIN0); join_worker(JoinSite::Line0); }
    if (probe_enabled_) async_probe_check(true);
    { prof::Scope b(prof::BEGIN_FRAME); begin_frame(); }
    nds_.frame_ready = true;
  } else if (line_ == 192) {
    if (probe_enabled_) async_probe_check(false);
    nds_.io.set_vblank(true);
    fifo_rd_ = fifo_wr_ = 0;
    nds_.dma.stop(Cpu::ARM9, dma::MODE9_DISPLAY_FIFO);
    nds_.dma.check(Cpu::ARM9, dma::MODE9_VBLANK);
    nds_.dma.check(Cpu::ARM7, dma::MODE7_VBLANK);
    { prof::Scope v(prof::GX_VBLANK); nds_.gpu3d.vblank(); }
    if (capture_on_) { capcnt_ &= ~(1u << 31); capture_on_ = false; }
  } else if (line_ == 262) nds_.io.set_vblank(false);
  if (line_ >= 2 && line_ < 194) nds_.dma.check(Cpu::ARM9, dma::MODE9_DISPLAY_START);
  else if (line_ == 194) nds_.dma.stop(Cpu::ARM9, dma::MODE9_DISPLAY_START);
  if (line_ < 192 && run_fifo_) nds_.sched.schedule(EventId::DisplayFifo, nds_.sched.event_time() + 32 * 2, ev_fifo, 0);
  nds_.io.set_vcount(line_);
  nds_.sched.schedule(EventId::HBlank, nds_.sched.event_time() + HBLANK_START, ev_hblank);
}

// Signature of only structural display choices (driving engine, display
// mode/bank, capture destination); per-frame changes like fades/scroll are excluded.
void Gpu::update_phase() {
  const u32 a = engine[0].dispcnt(), b = engine[1].dispcnt();
  const u32 sig = ((nds_.io.powcnt1 >> 15) & 1)
                | (((a >> 16) & 3) << 1) | (((a >> 18) & 3) << 3)
                | (((b >> 16) & 3) << 5)
                | ((capture_on_ ? 1u + ((capcnt_ >> 16) & 0xF) : 0u) << 7);
  for (u32 i = 0; i + 1 < PHASE_HISTORY; ++i) phase_sig_[i] = phase_sig_[i + 1];
  phase_sig_[PHASE_HISTORY - 1] = sig;
  if (phase_seen_ < PHASE_HISTORY) { ++phase_seen_; phase_period_ = 1; return; }
  // Smallest period that explains the whole window; none found means treat as 1.
  for (u32 p = 1; p <= PHASE_MAX; ++p) {
    bool ok = true;
    for (u32 i = p; i < PHASE_HISTORY && ok; ++i) ok = phase_sig_[i] == phase_sig_[i - p];
    if (ok) { phase_period_ = static_cast<u8>(p); return; }
  }
  phase_period_ = 1;
}

void Gpu::begin_frame() {
  frame_begun_ = true;
  if (g_dbg_gpu)
    std::fprintf(stderr, "[gpu] frame %llu powcnt %04x dispcntA %08x dispcntB %08x mb %04x/%04x cap %08x vramcnt %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
                 static_cast<unsigned long long>(nds_.frame_count), nds_.io.powcnt1, engine[0].dispcnt(), engine[1].dispcnt(), master_bright_g_[0], master_bright_g_[1], capcnt_,
                 nds_.io.vramcnt[0], nds_.io.vramcnt[1], nds_.io.vramcnt[2], nds_.io.vramcnt[3], nds_.io.vramcnt[4], nds_.io.vramcnt[5], nds_.io.vramcnt[6], nds_.io.vramcnt[7], nds_.io.vramcnt[8]);
  if (g_dbg_vramnz) {
    std::fprintf(stderr, "[vram] frame %llu nz:", static_cast<unsigned long long>(nds_.frame_count));
    for (int i = 0; i < 9; ++i) { u32 n = 0; const u8* b = nds_.bus.vram_bank(i); for (u32 k = 0; k < mem::Bus::VRAM_BANK_SIZES[i]; ++k) n += b[k] != 0; std::fprintf(stderr, " %c=%u", 'A' + i, n); }
    for (int i : {5, 7}) { const u8* b = nds_.bus.vram_bank(i); u32 lo = ~0u, hi = 0; for (u32 k = 0; k < mem::Bus::VRAM_BANK_SIZES[i]; ++k) if (b[k]) { if (k < lo) lo = k; hi = k; } std::fprintf(stderr, " %c[%x..%x]", 'A' + i, lo, hi); }
    std::fputc('\n', stderr);
  }
  if (const char* f = g_dump_frame) {   // DS_DEBUG_DUMP_LINE=L: engine state and one rendered line
    if (nds_.frame_count == static_cast<u64>(std::atoi(f))) {
      const char* l = std::getenv("DS_DEBUG_DUMP_LINE"); const u32 line = l ? std::atoi(l) : 96;
      engine[0].debug_dump(line); engine[1].debug_dump(line);
    }
  }
  screens_on_ = nds_.io.powcnt1 & 1;
  // FIFO only needs clocking when something displays/captures from it, or a DMA channel awaits it.
  run_fifo_ = uses_fifo() || nds_.dma.in_mode(Cpu::ARM9, dma::MODE9_DISPLAY_FIFO);
  if (capcnt_ & (1u << 31)) capture_on_ = true;
  if (capture_on_) capture_recent_ = CAPTURE_STICKY; else if (capture_recent_) --capture_recent_;
  // 3D frame this display frame reads, latched here since the raster moves on
  // at line 215 while the compositor may still be reading it.
  ref3d_ = nds_.gpu3d.frame_ref(!capture_on_);   // a capture frame reads the 3D of this frame, never the one before
  update_phase();
  if (prof::enabled) {
    prof::add(prof::C_FRAMES_TOTAL, 1);
    if (phase_period_ > 1) prof::add(prof::C_FRAMES_PHASE_ALT, 1);
    if (capture_on_) prof::add(prof::C_FRAMES_CAPTURE, 1);
  }
  // Frameskip: what line 215 assumed, re-checked now the capture bit is known.
  skip_frame_ = skip_next_ && skippable();
  if (g_dbg_skip)
    std::fprintf(stderr, "[skip] frame %llu req %d raster %d capture %d/%u fifo %d period %u -> %s\n",
                 static_cast<unsigned long long>(nds_.frame_count), skip_req_ ? 1 : 0, skip_next_ ? 1 : 0,
                 capture_on_ ? 1 : 0, capture_recent_, run_fifo_ ? 1 : 0, phase_period_, skip_frame_ ? "skipped" : "drawn");
  render_next_[0] = render_next_[1] = 0;
  per_line_prev_[0] = per_line_[0]; per_line_prev_[1] = per_line_[1];
  per_line_[0] = per_line_[1] = false; frame_finished_ = false;
  // Per-engine futility: an engine that keeps ending its frames per line starts
  // them per line (bar a re-probe); the other engine keeps its batch.
  if (lazy_tried_)
    for (int e = 0; e < 2; ++e) eng_futile_[e] = per_line_prev_[e] ? eng_futile_[e] + 1 : 0;
  lazy_frame_ = lazy_enabled_ && !g_no_lazy && !run_fifo_;
  lazy_bursts_[0] = lazy_bursts_[1] = 0; burst_[0] = burst_[1] = false; burst_left_[0] = burst_left_[1] = 0;
  if (lazy_frame_)
    for (int e = 0; e < 2; ++e) {
      if (eng_futile_[e] < LAZY_FUTILE_LIMIT) { eng_probe_in_[e] = LAZY_PROBE_PERIOD; continue; }
      if (--eng_probe_in_[e] == 0) { eng_probe_in_[e] = LAZY_PROBE_PERIOD; continue; }   // re-probe: try batching
      per_line_[e] = true;
      prof::add(e ? prof::C_2D_ENGINE_FUTILE_B : prof::C_2D_ENGINE_FUTILE_A, 1);
    }
  if (per_line_[0] && per_line_[1]) lazy_frame_ = false;   // both futile: nothing batches, nothing to trap for
  lazy_tried_ = lazy_frame_ || (lazy_enabled_ && !g_no_lazy && !run_fifo_);
  lag_frame_ = !run_fifo_ && !g_no_lag;
  lag_trap_hits_ = 0; lag_trap_stores_ = 0;
  arm_trap();
  if (lazy_frame_) prof::add(prof::C_2D_LAZY_FRAMES, 1);
  if (lag_frame_ && !lazy_frame_) prof::add(prof::C_2D_LAG_FRAMES, 1);
  if (!lazy_frame_) per_line_[0] = per_line_[1] = true;
}

// ---- main-memory display FIFO -----------------------------------------------

bool Gpu::uses_fifo() const {
  if (((engine[0].dispcnt() >> 16) & 3) == 3) return true;
  return (capcnt_ & (1 << 25)) && ((capcnt_ >> 29) & 3) != 0;
}

void Gpu::sample_fifo(u32 offset, u32 count) {
  for (u32 i = 0; i < count; ++i) { fifo_line_[offset + i] = fifo_[fifo_rd_]; fifo_rd_ = (fifo_rd_ + 1) & 0xF; }
}

// FIFO read out in 8-pixel steps starting ~3 pixels before the visible line
// (offset from the 8-pixel DMA grid). Each step requests the next DMA transfer (mode 4).
void Gpu::on_display_fifo(u32 x) {
  if (x > 0) { if (x == 8) sample_fifo(0, 5); else sample_fifo(x - 11, 8); }
  if (x < 256) {
    nds_.dma.check(Cpu::ARM9, dma::MODE9_DISPLAY_FIFO);
    nds_.sched.schedule(EventId::DisplayFifo, nds_.sched.event_time() + 6 * 8 * 2, ev_fifo, x + 8);
  } else sample_fifo(253, 3);
}

// ---- rendering and the output stage ------------------------------------------

// One engine's run of display lines on the worker; engine state is disjoint.
void Gpu::worker_job(void* self) {
  Gpu& g = *static_cast<Gpu*>(self);
  for (int e = 0; e < 2; ++e)
    for (u32 l = g.job_first_[e]; l <= g.job_last_[e]; ++l) g.step_engine(e, l);
  for (u32 i = 0; i < g.bscale_n_; ++i) {
    const StashedLine& st = g.bscale_[i];
    g.emit_scaled(st.screen, st.line, st.px);
  }
}

void Gpu::worker_b_job(void* self) {
  Gpu& g = *static_cast<Gpu*>(self);
  for (u32 l = g.bjob_first_; l <= g.bjob_last_; ++l) g.step_engine(1, l);
}

void Gpu::join_worker(JoinSite site) {
  if (!inflight_[0] && !inflight_[1] && !scale_inflight_) return;
  if (b_deferred_) worker_b_.wait(static_cast<int>(site));
  const bool dbg = g_dbg_join;
  if (dbg) std::fprintf(stderr, "[join] frame %llu line %u hblank %d a %u..%u b %u..%u deferred %d\n", (unsigned long long)nds_.frame_count, line_, hblank_done_ ? 1 : 0, job_first_[0], job_last_[0], job_first_[1], job_last_[1], a_deferred_ ? 1 : 0);
  { const u64 t0 = prof::now_ns(); worker_.wait(static_cast<int>(site));
    const u64 dt = prof::now_ns() - t0;
    join_wait_ns_ += dt; prof::add_ns(prof::W2D_JOIN, dt);
    if (prof::enabled) {
      prof::add(prof::C_W2D_JOIN_CALLS, 1);
      static constexpr prof::Counter kBySite[] = {
        prof::C_W2D_JOIN_NS_CATCHUP, prof::C_W2D_JOIN_NS_TRAP, prof::C_W2D_JOIN_NS_JOURNAL,
        prof::C_W2D_JOIN_NS_LINE0, prof::C_W2D_JOIN_NS_REMAP,
        prof::C_W2D_JOIN_NS_RPRE, prof::C_W2D_JOIN_NS_RPOST, prof::C_W2D_JOIN_NS_OTHER };
      prof::add(kBySite[static_cast<int>(site)], dt);
    } }
  inflight_[0] = inflight_[1] = false; scale_inflight_ = false; bscale_n_ = 0;
  for (int e = 0; e < 2; ++e) {
    engine[e].set_vram_override(nullptr);
    if (remap_pending_[e]) { engine[e].vram_remapped(); remap_pending_[e] = false; }
  }
  snapshot_stale_ = false;
  if (b_deferred_) {   // as finish_a: writes the guest made meanwhile were journaled past the frame
    b_deferred_ = false;
    if (line_ == 0 || (line_ == 262 && hblank_done_)) skipped_[1] = false;
    engine[1].apply_pending();
    if (frame_finished_) engine[1].frame_done();
  }
  if (a_deferred_) { a_deferred_ = false; finish_a(); }
}

// Engine A's deferred batch has been joined: what render_ranges does at frame
// end, done now (read trap, then journal, then frame end).
void Gpu::finish_a() {
  release_read_trap();
  if (line_ == 0 || (line_ == 262 && hblank_done_)) skipped_[0] = false;
  engine[0].apply_pending();
  if (frame_finished_) { engine[0].frame_done(); disarm_trap(); }
}

void Gpu::debug_dump(FILE* f) {
  std::fprintf(f, "  gpu: line %u hblank_done %d render_next %u/%u lazy %d per_line %d/%d trap %d job a %u..%u b %u..%u inflight %d/%d deferred %d read_trap %d lag %d frame_ready %d\n", line_, hblank_done_ ? 1 : 0, render_next_[0], render_next_[1],
               lazy_frame_ ? 1 : 0, per_line_[0] ? 1 : 0, per_line_[1] ? 1 : 0, trap_armed_ ? 1 : 0, job_first_[0], job_last_[0], job_first_[1], job_last_[1], inflight_[0] ? 1 : 0, inflight_[1] ? 1 : 0, a_deferred_ ? 1 : 0, read_trap_bank_, lag_frame_ ? 1 : 0, nds_.frame_ready ? 1 : 0);
  worker_.debug_dump(f);
  if (split_) worker_b_.debug_dump(f);
  nds_.gpu3d.debug_dump(f);
}

// Render display lines of both engines: one hand-off to the worker, the other
// engine's run here. An empty range (first > last) means nothing due.
void Gpu::render_ranges(u32 af, u32 al, u32 bf, u32 bl) {
  const bool a_has = af <= al, b_has = bf <= bl;
  if (!a_has && !b_has) return;
  join_worker(JoinSite::RangesPre);   // a job handed now snapshots the live map: nothing stale may still be in flight
  capcnt_render_ = capcnt_; capture_render_ = capture_on_;
  lcdc_mask_render_ = nds_.bus.vram_map().lcdc_mask;   // latched so the job doesn't consult a live map that may be rebuilding
  const bool dbg = g_dbg_join;
  auto hand = [&](bool a, bool b) {
    if (dbg) std::fprintf(stderr, "[hand] frame %llu line %u a %u..%u b %u..%u lag %d stash %u\n", (unsigned long long)nds_.frame_count, line_, a ? af : 1, a ? al : 0, b ? bf : 1, b ? bl : 0, lag_frame_ ? 1 : 0, bscale_n_);
    job_first_[0] = a ? af : 1; job_last_[0] = a ? al : 0;
    job_first_[1] = b ? bf : 1; job_last_[1] = b ? bl : 0;
    scale_inflight_ = bscale_n_ > 0;
    snapshot_map(0);
    if (a) engine[0].set_vram_override(&job_map_[0]);
    if (b) engine[1].set_vram_override(&job_map_[0]);
    worker_.dispatch();
    inflight_[0] = a; inflight_[1] = b;
    // A capture in flight writes an LCDC bank the guest may read before the join: trap it.
    if (a && capture_render_ && read_trap_bank_ < 0) {
      const int bank = static_cast<int>((capcnt_render_ >> 16) & 3);
      nds_.bus.set_lcdc_read_trap(bank, true);
      read_trap_bank_ = bank;
    }
  };
  const u32 a_len = a_has ? al - af + 1 : 0, b_len = b_has ? bl - bf + 1 : 0;
  const u32 last = a_has && b_has ? (al > bl ? al : bl) : a_has ? al : bl;
  bool a_handed = false, b_handed = false;
  // Read once: parked() can change under separate branches.
  const bool parked_at_decision = prof::enabled ? worker_.parked() : false;
  if (prof::enabled) {
    prof::add(prof::C_RR_CALLS, 1);
    prof::add(parked_at_decision ? prof::C_RR_PARKED : prof::C_RR_AWAKE, 1);
    if (b_has && b_len < 24 && !parked_at_decision) prof::add(prof::C_RR_SHORT_HANDED_AWAKE, 1);
    if (b_has && b_len < 24 && parked_at_decision) prof::add(prof::C_RR_SHORT_INLINE_PARKED, 1);
  }
  if (a_has && al == SCREEN_H - 1 && (a_len >= 24 || (!worker_.parked() && a_len >= b_len))) {
    // Run to the last display line: deferred join, engine B drawn here meanwhile.
    prof::add(prof::C_RR_DEFER_A, 1);
    hand(true, false);
    a_handed = true; a_deferred_ = true;
    if (split_ && b_has && bl == SCREEN_H - 1 && b_len >= 24 && trap_armed_ && (trap_mask_ & 2)) {
      // Engine B's batch to its own worker, deferred to the same join: only a
      // batch run to the last line (everything journaled meanwhile is past the
      // frame) with the trap over the whole VRAM window (a lag frame's guards
      // only engine A's).
      bjob_first_ = bf; bjob_last_ = bl;
      snapshot_map(1);
      engine[1].set_vram_override(&job_map_[1]);
      worker_b_.dispatch();
      inflight_[1] = true; b_deferred_ = true; b_handed = true;
    }
  } else if (lag_frame_ && (a_has || (b_has && scaling()))) {
    // Lag: A's lines stay in flight until next HBlank unless this is the
    // last one. B is drawn here (its window streams per-line, joining lag on
    // every store), but its scaling is stashed with the job (bscale_).
    if (b_has && scaling()) {
      bscale_defer_ = true;
      for (u32 x = bf; x <= bl; ++x) step_engine(1, x);
      bscale_defer_ = false;
      b_handed = true;                        // drawn, its scaling in flight
    }
    prof::add(prof::C_RR_LAG, 1);
    hand(a_has, false);
    a_handed = a_has;
  } else if (b_has && (b_len >= 24 || !worker_.parked())) {
    // Short run against a parked worker drawn here: wake-up costs more than the lines do.
    prof::add(prof::C_RR_HAND_B, 1);
    hand(false, true);
    b_handed = true;
  } else {
    prof::add(prof::C_RR_NONE, 1);
  }
  if (a_has) prof::add(prof::C_2D_RANGE_A, 1);
  if (b_has) prof::add(prof::C_2D_RANGE_B, 1);
  if (prof::enabled) {
    prof::add(prof::C_RR_LINES_HANDED, (a_handed ? a_len : 0) + (b_handed ? b_len : 0));
    prof::add(prof::C_RR_LINES_INLINE, (a_has && !a_handed ? a_len : 0) + (b_has && !b_handed ? b_len : 0));
  }
  if (a_has && !a_handed) for (u32 x = af; x <= al; ++x) step_engine(0, x);
  if (b_has && !b_handed) for (u32 x = bf; x <= bl; ++x) step_engine(1, x);
  // A's deferred batch stays in flight until line 0; a lagged run until the
  // next line. The last display line always joins.
  if (!a_deferred_) {
    // A B line left in flight needs B's windows trapped.
    if ((a_handed || b_handed || scale_inflight_) && lag_frame_ && last < SCREEN_H - 1 && !(b_handed && !(trap_armed_ && (trap_mask_ & 2)))) prof::add(prof::C_2D_LAG_LINES, 1);
    else join_worker(JoinSite::RangesPost);
  }
  if (a_has) render_next_[0] = al + 1;
  if (b_has) render_next_[1] = bl + 1;
  if (!frame_finished_ && render_next_[0] >= SCREEN_H && render_next_[1] >= SCREEN_H) {
    frame_finished_ = true;
    if (a_deferred_) { if (!b_deferred_) engine[1].frame_done(); }   // the deferred engines end (and lift traps) at the join
    else {
      join_worker(JoinSite::RangesPost);
      release_read_trap();
      engine[0].frame_done(); engine[1].frame_done(); disarm_trap();
    }
  }
}

// One engine's display line, in hardware order: pre-scanline writes, window
// edges, the line's own writes, latches, the line, its output, next line's sprites.
void Gpu::step_engine(int e, u32 line) {
  Engine2D& en = engine[e];
  {
    prof::Scope jn(prof::JOURNAL);
    en.replay_to(line * 2);
    en.update_windows(line);
    en.replay_to(line * 2 + 1);
    en.pre_draw(line, false);
  }
  // Engine B on a hidden screen draws nothing; the line it comes back on re-renders its sprites.
  const bool draw = !skip_frame_ && !(e == 1 && !screen_visible_[en.screen()]);
  if (!draw) skipped_[e] = true; else if (skipped_[e]) { skipped_[e] = false; en.render_sprites(line); }
  // Reading the 3D line joins the raster bands: skip it when the raster never ran.
  if (e == 0 && (draw || capture_render_)) { prof::Scope l3(prof::R3D_LINE); line3d_ = nds_.gpu3d.line(ref3d_, line); en.set_3d_line(line3d_); }
  if (draw) { en.render_line(line); output_engine(e, line); }
  if (e == 0 && capture_render_ && !skip_frame_) { DS_PROF(CAPTURE); capture(line); }
  // Sprites are rendered one line ahead of the backgrounds.
  if (draw && line < SCREEN_H - 1) {
    prof::Scope sc(prof::OBJ_DRAW, e == 0);
    en.render_sprites(line + 1);
  }
  { prof::Scope jn(prof::JOURNAL); en.post_draw(false); }
}

// The output stage for one engine's line: display mode, master brightness,
// 6->8 bit expansion, into the screen POWCNT1 bit 15 gives it (or scaled
// straight into the frontend's buffer).
void Gpu::output_engine(int e, u32 line) {
  prof::Scope sc(prof::OUTPUT, e == 0);
  const Engine2D& en = engine[e];
  const int screen = en.screen();
  const bool scaled = scaling();
  u32* dst = scaled ? line_out_[e].data() : fb_[screen].data() + line * SCREEN_W;
  if (screens_on_) {
    if (e == 0) {
      const u32 mode = (en.dispcnt() >> 16) & 3;
      if (mode == 1) kern::active::output_line(en.output(), en.master_bright(), dst);
      else if (mode >= 2) output_a(line, dst);      // VRAM / FIFO display: expanded inside
      else { output_a(line, dst); expand_colours(dst); }
    } else {
      if ((en.dispcnt() >> 16) & 1) kern::active::output_line(en.output(), en.master_bright(), dst);
      else { output_b(dst); expand_colours(dst); }
    }
  } else { for (u32 i = 0; i < 256; ++i) dst[i] = 0xFF000000; }
  if (scaled) {
    if (e == 1 && bscale_defer_ && bscale_n_ < SCREEN_H) {
      StashedLine& st = bscale_[bscale_n_++];
      st.line = line; st.screen = screen;
      std::memcpy(st.px, dst, sizeof st.px);
    } else emit_scaled(screen, line, dst);
  }
}

static inline bool row_kept(const Gpu::ScaleTarget& t, u32 y) { return y >= t.y_lo && y < t.y_hi; }
static inline u32* row_at(const Gpu::ScaleTarget& t, u32 y) { return t.px + (static_cast<size_t>(y) - t.y_lo) * t.pitch; }

// Mean of four 0xAARRGGBB pixels, per channel, alpha forced opaque.
static inline u32 mean4(u32 a, u32 b, u32 c, u32 d) {
  const u32 rb = (((a & 0xFF00FFu) + (b & 0xFF00FFu) + (c & 0xFF00FFu) + (d & 0xFF00FFu) + 0x020002u) >> 2) & 0xFF00FFu;
  const u32 g  = (((a & 0xFF00u) + (b & 0xFF00u) + (c & 0xFF00u) + (d & 0xFF00u) + 0x0200u) >> 2) & 0xFF00u;
  return 0xFF000000u | rb | g;
}
// The colour at least two of the four share (ties to the earlier pixel), or their mean when all differ.
static inline u32 mode4(u32 a, u32 b, u32 c, u32 d) {
  if (a == b || a == c || a == d) return a;
  if (b == c || b == d) return b;
  if (c == d) return c;
  return mean4(a, b, c, d);
}

// Rec.601-ish luma, 0..255*256.
static inline u32 luma(u32 c) { return ((c >> 16) & 255) * 77 + ((c >> 8) & 255) * 150 + (c & 255) * 29; }
static inline u32 min4(u32 a, u32 b, u32 c, u32 d) {
  u32 best = a, bl = luma(a);
  for (u32 p : {b, c, d}) { const u32 l = luma(p); if (l < bl) { best = p; bl = l; } }
  return best;
}
static inline u32 max4(u32 a, u32 b, u32 c, u32 d) {
  u32 best = a, bl = luma(a);
  for (u32 p : {b, c, d}) { const u32 l = luma(p); if (l > bl) { best = p; bl = l; } }
  return best;
}
// Darkest/brightest of the four when it stands farther than `thr` from the
// block's mean luma (keeps a thin stroke while dithers/gradients average); mean otherwise.
static inline u32 extreme4(u32 a, u32 b, u32 c, u32 d, u32 thr) {
  const u32 lo = min4(a, b, c, d), hi = max4(a, b, c, d);
  const u32 m = (luma(a) + luma(b) + luma(c) + luma(d)) / 4;
  const u32 dlo = m - luma(lo), dhi = luma(hi) - m;
  if (dlo <= thr && dhi <= thr) return mean4(a, b, c, d);
  return dlo > dhi ? lo : dhi > dlo ? hi : mean4(a, b, c, d);
}

// sRGB <-> linear for the linear-light blend: 8-bit sRGB to 12-bit linear and back, built once.
namespace {
struct GammaLut {
  u16 to_lin[256];
  u8 from_lin[4096];
  GammaLut() {
    for (u32 i = 0; i < 256; ++i) {
      const double c = i / 255.0;
      const double l = c <= 0.04045 ? c / 12.92 : ds_pow_compat((c + 0.055) / 1.055, 2.4);
      to_lin[i] = static_cast<u16>(std::lround(l * 4095.0));
    }
    for (u32 i = 0; i < 4096; ++i) {
      const double l = i / 4095.0;
      const double c = l <= 0.0031308 ? l * 12.92 : 1.055 * ds_pow_compat(l, 1.0 / 2.4) - 0.055;
      from_lin[i] = static_cast<u8>(std::lround(c * 255.0));
    }
  }
};
const GammaLut& gamma_lut() { static const GammaLut lut; return lut; }

// Per-pixel weighted blend in linear light; w per pixel as blend_line_w.
void blend_line_linear(const u32* a, const u32* b, const u8* w, u32* out) {
  const GammaLut& g = gamma_lut();
  for (u32 i = 0; i < 256; ++i) {
    const u32 f = w[i];
    if (!f) { out[i] = a[i]; continue; }
    u32 r = 0xFF000000u;
    for (u32 sh = 0; sh < 24; sh += 8) {
      const u32 la = g.to_lin[(a[i] >> sh) & 255], lb = g.to_lin[(b[i] >> sh) & 255];
      r |= static_cast<u32>(g.from_lin[(la * (256 - f) + lb * f + 128) >> 8]) << sh;
    }
    out[i] = r;
  }
}
} // namespace

void Gpu::blend_rows(const ScaleTarget& t, const u32* a, const u32* b, u32 w, u32* out) {
  alignas(16) u8 wt[SCREEN_W];
  std::memset(wt, static_cast<int>(w), sizeof wt);
  if (t.blend == 2) blend_line_linear(a, b, wt, out); else kern::active::blend_line_w(a, b, wt, out);
}

// One row with box-filter seams: the last pixel of a run that straddles two
// source pixels is their area-weighted blend.
void Gpu::emit_row_straddle(const ScaleTarget& t, const u32* src, u32* dst) {
  alignas(16) u32 next[SCREEN_W], seam[SCREEN_W];
  std::memcpy(next, src + 1, (SCREEN_W - 1) * sizeof(u32));
  next[SCREEN_W - 1] = src[SCREEN_W - 1];
  if (t.blend == 2) blend_line_linear(src, next, t.seam_w, seam); else kern::active::blend_line_w(src, next, t.seam_w, seam);
  kern::active::scale_row_straddle(src, seam, t.seam_w, t.xrun, dst);
}

bool Gpu::build_cell_axis(u32 src_n, u32 cells, u32 cell_px, CellAxis& a) {
  a.cells = cells; a.cell_px = cell_px;
  a.first.assign(cells, 0); a.n.assign(cells, 0); a.w.assign(static_cast<size_t>(cells) * CELL_TAPS, 0);
  for (u32 i = 0; i < cells; ++i) {
    // Cell i covers source [i*src_n/cells, (i+1)*src_n/cells); tap weights are the overlap, 1/256 units.
    const u32 lo = i * src_n, hi = (i + 1) * src_n;         // in 1/cells units
    const u32 s0 = lo / cells, s1 = (hi + cells - 1) / cells; // taps [s0, s1)
    if (s1 - s0 > CELL_TAPS) return false;
    a.first[i] = static_cast<u16>(s0); a.n[i] = static_cast<u8>(s1 - s0);
    u32 sum = 0;
    for (u32 s = s0; s < s1; ++s) {
      const u32 olo = std::max(lo, s * cells), ohi = std::min(hi, (s + 1) * cells);
      u32 w = ((ohi - olo) * 256 + src_n / 2) / src_n;
      if (s + 1 == s1) w = 256 - sum;
      sum += w;
      a.w[static_cast<size_t>(i) * CELL_TAPS + (s - s0)] = static_cast<u16>(w);
    }
  }
  return true;
}

// One cell row: every cell's colour from its taps in the held lines, then the
// row of cells drawn as `cell_px` panel rows (the first the seam row when the
// grid is on) through the grid kernel with the cells' xrun.
void Gpu::emit_cells(int screen, u32 line, const u32* src) {
  const ScaleTarget& t = scale_[screen];
  const CellMap& m = *t.cells;
  const u32 j = cell_row_[screen];
  if (j >= m.y.cells) return;
  const u32 l0 = m.y.first[j], ln = m.y.n[j];
  std::memcpy(cell_lines_[screen][line % CELL_TAPS], src, SCREEN_W * sizeof(u32));
  if (line + 1 < l0 + ln) return;
  auto held = [&](u32 v) { return cell_lines_[screen][(l0 + v) % CELL_TAPS]; };
  // The row is complete.
  alignas(16) u32 cells[SCREEN_W];
  const bool linear = t.blend == 2;
  const GammaLut& g = gamma_lut();
  const u16* wy = &m.y.w[static_cast<size_t>(j) * CELL_TAPS];
  for (u32 i = 0; i < m.x.cells; ++i) {
    const u32 s0 = m.x.first[i], sn = m.x.n[i];
    const u16* wx = &m.x.w[static_cast<size_t>(i) * CELL_TAPS];
    // Mean: the 2D box (weights in 1/65536), in sRGB or linear light.
    u32 acc[3] = {0, 0, 0};
    for (u32 v = 0; v < ln; ++v) for (u32 u = 0; u < sn; ++u) {
      const u32 c = held(v)[s0 + u], wt = wx[u] * wy[v];
      for (u32 ch = 0; ch < 3; ++ch) { const u32 b = (c >> (8 * ch)) & 255; acc[ch] += (linear ? g.to_lin[b] : b) * wt; }
    }
    u32 mean = 0xFF000000u;
    for (u32 ch = 0; ch < 3; ++ch) {
      const u32 v = (acc[ch] + 32768) >> 16;
      mean |= static_cast<u32>(linear ? g.from_lin[std::min<u32>(v, 4095)] : v) << (8 * ch);
    }
    u32 out = mean;
    if (t.chunky != 2) {
      // Other modes use pixels >= half covered on both axes; largest-coverage pixel stands in for "top-left".
      u32 cand[CELL_TAPS * CELL_TAPS]; u32 nc = 0;
      u32 best = 0, bestw = 0;
      for (u32 v = 0; v < ln; ++v) for (u32 u = 0; u < sn; ++u) {
        const u32 wt = wx[u] * wy[v];
        if (wt > bestw) { bestw = wt; best = held(v)[s0 + u]; }
        if (wx[u] >= 128 && wy[v] >= 128) cand[nc++] = held(v)[s0 + u];
      }
      if (nc == 0) cand[nc++] = best;
      switch (t.chunky) {
      case 1: out = best; break;
      case 3: {   // dominant: the most repeated candidate, ties to the earlier; the mean when all differ
        u32 bc = 0; out = mean;
        for (u32 a = 0; a < nc; ++a) { u32 cnt = 0; for (u32 b = 0; b < nc; ++b) cnt += cand[b] == cand[a]; if (cnt > bc && cnt >= 2) { bc = cnt; out = cand[a]; } }
        break;
      }
      case 4: { out = cand[0]; for (u32 a = 1; a < nc; ++a) if (luma(cand[a]) < luma(out)) out = cand[a]; break; }
      case 5: { out = cand[0]; for (u32 a = 1; a < nc; ++a) if (luma(cand[a]) > luma(out)) out = cand[a]; break; }
      default: {  // extreme: the darkest or brightest candidate, if farther than the threshold from the mean
        u32 lo = cand[0], hi = cand[0];
        for (u32 a = 1; a < nc; ++a) { if (luma(cand[a]) < luma(lo)) lo = cand[a]; if (luma(cand[a]) > luma(hi)) hi = cand[a]; }
        const u32 lm = luma(mean), dlo = lm > luma(lo) ? lm - luma(lo) : 0, dhi = luma(hi) > lm ? luma(hi) - lm : 0;
        out = (dlo <= t.chunky_thresh && dhi <= t.chunky_thresh) ? mean : dlo > dhi ? lo : dhi > dlo ? hi : mean;
      }
      }
    }
    cells[i] = out;
  }
  for (u32 i = m.x.cells; i < SCREEN_W; ++i) cells[i] = 0;
  // Draw: rows [j*P, (j+1)*P), the first the seam row.
  const u32 P = m.y.cell_px;
  const u32 y0 = j * P;
  const size_t bytes = static_cast<size_t>(t.xrun[SCREEN_W]) * sizeof(u32);
  const bool grid = t.grid < 256;
  if (t.xrun[SCREEN_W] > SCALED_ROW_MAX) return;
  u32* row = row_scratch_[screen];
  if (grid) kern::active::scale_row_grid(cells, t.xrun, t.grid, 2, 1, false, row);
  else      kern::active::scale_row(cells, t.xrun, row);
  for (u32 y = y0 + (grid ? 1 : 0); y < y0 + P; ++y)
    if (row_kept(t, y)) std::memcpy(row_at(t, y), row, bytes);
  if (grid && row_kept(t, y0)) kern::active::scale_row_grid(cells, t.xrun, t.grid, 2, 1, true, row_at(t, y0));
  cell_row_[screen] = j + 1;
}

void Gpu::scale_image(int screen, const u32* src) {
  if (!scale_[screen].px) return;
  for (u32 line = 0; line < SCREEN_H; ++line) emit_scaled(screen, line, src + line * SCREEN_W);
}

// Bilinear. Destination row y samples v = (y+0.5)*192/h - 0.5 in source
// lines; rows blending L-1 and L are written when L arrives. Rows above line
// 0's centre / below line 191's are that line alone (clamped). Source line L
// owns rows [ystart(L), ystart(L+1)); every row claimed exactly once.
void Gpu::emit_bilinear(int screen, u32 line, const u32* src) {
  const ScaleTarget& t = scale_[screen];
  const u32 w = t.xrun[SCREEN_W];
  if (w > SCALED_ROW_MAX || w == 0 || t.h == 0) return;
  const u32 h = t.h;
  auto ystart = [h](u32 l) -> u32 {
    if (l == 0) return 0;
    const u32 k = ((2 * l - 1) * h + SCREEN_H - 1) / SCREEN_H;   // ceil((2L-1)h/192)
    return std::min(h, k / 2);
  };
  // Widen this line; previous line's row kept if it is line-1.
  const bool have_prev = line > 0 && lin_prev_line_[screen] + 1 == line;
  const u32 cur = have_prev ? lin_cur_[screen] ^ 1u : 0u;
  u32* const hcur = lin_row_[screen][cur];
  const u32* const hprev = lin_row_[screen][cur ^ 1u];
  kern::active::lerp_row_gather(src, t.lin_sx, t.lin_wx, w, hcur);
  lin_cur_[screen] = cur;
  lin_prev_line_[screen] = line;
  const size_t bytes = static_cast<size_t>(w) * sizeof(u32);
  auto out_row = [&](u32 y) -> u32* { return row_kept(t, y) ? row_at(t, y) : nullptr; };   // null: cropped away
  if (line == 0) {
    for (u32 y = 0; y < ystart(1); ++y) if (u32* o = out_row(y)) std::memcpy(o, hcur, bytes);
    if (line + 1 < SCREEN_H) return;
  }
  if (have_prev) {
    const u32 y0 = ystart(line), y1 = ystart(line + 1);
    for (u32 y = y0; y < y1; ++y) {
      // Weight of this line: v - (line-1), in 1/256.
      const s32 wf = static_cast<s32>(((2 * y + 1) * SCREEN_H * 128) / h) - 128 - static_cast<s32>((line - 1) * 256);
      const u32 wy = static_cast<u32>(std::min(255, std::max(0, wf)));
      u32* const o = out_row(y);
      if (!o) continue;
      if (wy == 0) std::memcpy(o, hprev, bytes);
      else kern::active::lerp_rows(hprev, hcur, wy, w, o);
    }
  } else {
    // A gap in the lines (hidden span, or first line after reset): this line stands alone.
    for (u32 y = ystart(line); y < ystart(line + 1); ++y) if (u32* o = out_row(y)) std::memcpy(o, hcur, bytes);
  }
  if (line + 1 == SCREEN_H)
    for (u32 y = ystart(SCREEN_H); y < h; ++y) if (u32* o = out_row(y)) std::memcpy(o, hcur, bytes);
}

void Gpu::emit_scaled(int screen, u32 line, const u32* src) {
  const ScaleTarget& t = scale_[screen];
  if (t.bilinear && t.lin_sx && t.lin_wx) { emit_bilinear(screen, line, src); return; }
  if (t.chunky && t.cells) {
    if (line == 0) cell_row_[screen] = 0;
    emit_cells(screen, line, src);
    return;
  }
  u32 first = line, last = line;
  alignas(16) u32 block[SCREEN_W];
  if (t.chunky) {
    // Each 2x2 block is one cell (xrun merges pixel pairs; line pair merged
    // here). Top-left draws from the even line; other modes resolve on the odd one.
    if (t.chunky == 1) { if (line & 1) return; last = line + 1; }
    else {
      if (!(line & 1)) { std::memcpy(chunk_even_[screen], src, sizeof block); return; }
      first = line - 1;
      const u32* up = chunk_even_[screen];
      switch (t.chunky) {
      case 2:  for (u32 s = 0; s < SCREEN_W; s += 2) block[s] = mean4(up[s], up[s + 1], src[s], src[s + 1]); break;
      case 3:  for (u32 s = 0; s < SCREEN_W; s += 2) block[s] = mode4(up[s], up[s + 1], src[s], src[s + 1]); break;
      case 4:  for (u32 s = 0; s < SCREEN_W; s += 2) block[s] = min4(up[s], up[s + 1], src[s], src[s + 1]); break;
      case 5:  for (u32 s = 0; s < SCREEN_W; s += 2) block[s] = max4(up[s], up[s + 1], src[s], src[s + 1]); break;
      default: for (u32 s = 0; s < SCREEN_W; s += 2) block[s] = extreme4(up[s], up[s + 1], src[s], src[s + 1], t.chunky_thresh); break;
      }
      src = block;
    }
  }
  const u32 y0 = (first * t.h + SCREEN_H - 1) / SCREEN_H;
  const u32 y1 = ((last + 1) * t.h + SCREEN_H - 1) / SCREEN_H;
  if (y0 >= y1) return;                      // downscale: this line is dropped
  const size_t bytes = static_cast<size_t>(t.xrun[SCREEN_W]) * sizeof(u32);
  // Rows built in cached scratch and copied out, never read back from the
  // target. A row wider than the scratch goes direct, which a cropped view cannot.
  const bool stage = t.xrun[SCREEN_W] <= SCALED_ROW_MAX;
  const bool cropped = t.y_lo != 0 || t.y_hi != t.h;
  if (!stage && cropped) return;
  u32* row = stage ? row_scratch_[screen] : row_at(t, y0);
  const bool blend = t.blend && t.seam_w;   // --seam blend: box-filter seams stand in for the grid
  if (blend && t.h >= SCREEN_H) {
    // Box-filter seams: a straddling panel pixel/row is an area-weighted
    // blend, others are nearest. The straddling row of this span is its last
    // and needs the next line, so it's written when that arrives. Upscales
    // only: a downscale drops lines and never returns for the row they left.
    const u32 hb = (last + 1) * t.h;                 // this span's lower boundary, in 1/192 rows
    const bool straddle_below = (hb % SCREEN_H) != 0 && last + 1 < SCREEN_H;
    const u32 ycrisp_end = straddle_below ? y1 - 1 : y1;
    if (ycrisp_end > y0) {
      emit_row_straddle(t, src, row);
      for (u32 y = stage ? y0 : y0 + 1; y < ycrisp_end; ++y)
        if (row_kept(t, y)) std::memcpy(row_at(t, y), row, bytes);
    }
    // Row above straddles the previous line and this one: boundary falls
    // frac/192 down it, previous line owns that much, this line the rest.
    if (first > 0 && seam_prev_line_[screen] + 1 == first) {
      const u32 tb = first * t.h;                    // this span's upper boundary
      const u32 frac = tb % SCREEN_H;
      if (frac) {
        alignas(16) u32 mid[SCREEN_W];
        blend_rows(t, seam_prev_[screen], src, ((SCREEN_H - frac) * 256) / SCREEN_H, mid);   // weight of this line
        if (row_kept(t, y0 - 1)) emit_row_straddle(t, mid, row_at(t, y0 - 1));
      }
    }
    std::memcpy(seam_prev_[screen], src, sizeof seam_prev_[screen]);
    seam_prev_line_[screen] = last;
    return;
  }
  // Plain nearest: no grid, or seam blend on a view too small for its seams.
  if (t.grid >= 256 || blend) {
    kern::active::scale_row(src, t.xrun, row);
    for (u32 y = stage ? y0 : y0 + 1; y < y1; ++y)
      if (row_kept(t, y)) std::memcpy(row_at(t, y), row, bytes);
    return;
  }
  // LCD grid: last output column/row of every source pixel's run/span is
  // dimmed, so each DS pixel shows a lit cell with a dark seam right/below.
  // A run of one pixel is left alone. Only runs the fractional scale widened
  // carry a seam (min_run = ceil(scale)); the seam leads its run. At exactly
  // 2x a seam per pixel reads as a wash, so it goes on every other pixel instead (4-pixel pitch).
  const u32 w = t.xrun[SCREEN_W];
  const u32 min_run = (w + SCREEN_W - 1) / SCREEN_W, min_rows = (t.h + SCREEN_H - 1) / SCREEN_H;
  const u32 pitch_x = w == 2 * SCREEN_W ? 2 : 1, pitch_y = t.h == 2 * SCREEN_H ? 2 : 1;
  const bool seam = y1 - y0 >= std::max<u32>(2, min_rows) && first % pitch_y == 0;
  const u32 yfirst = seam ? y0 + 1 : y0;
  row = stage ? row_scratch_[screen] : row_at(t, yfirst);
  kern::active::scale_row_grid(src, t.xrun, t.grid, min_run, pitch_x, false, row);
  for (u32 y = stage ? yfirst : yfirst + 1; y < y1; ++y)
    if (row_kept(t, y)) std::memcpy(row_at(t, y), row, bytes);
  if (seam && row_kept(t, y0)) kern::active::scale_row_grid(src, t.xrun, t.grid, min_run, pitch_x, true, row_at(t, y0));
}

void Gpu::output_a(u32 line, u32* dst) {
  const u32 dispcnt = engine[0].dispcnt();
  switch ((dispcnt >> 16) & 3) {
  case 0: for (u32 i = 0; i < 256; ++i) dst[i] = 0x3F3F3F; return;          // display off: white
  case 1: { const Pixel* src = engine[0].output(); for (u32 i = 0; i < 256; ++i) dst[i] = src[i]; break; }
  case 2: {                                                                 // VRAM display (LCDC bank)
    const u32 bank = (dispcnt >> 18) & 3;
    const VramMap& vm = nds_.bus.vram_map();   // bank pointers are remap-invariant
    static constexpr u16 kZeroLine[256] = {};
    const u16* src = (lcdc_mask_render_ & (1u << bank)) ? reinterpret_cast<const u16*>(vm.bank(bank)) + line * 256 : kZeroLine;
    kern::active::output_vram_line(src, engine[0].master_bright(), dst);
    return;
  }
  case 3: kern::active::output_vram_line(fifo_line_.data(), engine[0].master_bright(), dst); return;
  }
  apply_master_brightness(engine[0].master_bright(), dst);
}

void Gpu::output_b(u32* dst) {
  if (!((engine[1].dispcnt() >> 16) & 1)) { for (u32 i = 0; i < 256; ++i) dst[i] = 0xFF3F3F3F; return; }
  const Pixel* src = engine[1].output();
  for (u32 i = 0; i < 256; ++i) dst[i] = src[i];
  apply_master_brightness(engine[1].master_bright(), dst);
}

// Display capture: blends source A (engine A composite or the 3D layer) with
// source B (VRAM or the display FIFO) into an LCDC-mapped bank as BGR555.
void Gpu::capture(u32 line) {
  const u32 cnt = capcnt_render_;
  const u32 size = (cnt >> 20) & 3;
  const u32 width = size == 0 ? 128 : 256, height = size == 0 ? 128 : 64 * size;
  if (line >= height) return;
  const u32 dst_bank = (cnt >> 16) & 3;
  // Bank pointers are remap-invariant so the live map is safe here; lcdc_mask is not (latched at hand-off).
  const VramMap& vm = nds_.bus.vram_map();
  const u32 lcdc = lcdc_mask_render_;
  if (!(lcdc & (1u << dst_bank))) return;
  u16* dst = reinterpret_cast<u16*>(vm.bank(dst_bank)) + (((((cnt >> 18) & 3) << 14) + line * width) & 0xFFFF);

  const Pixel* src_a = (cnt & (1 << 24)) ? line3d_ : engine[0].output();
  const u16* src_b = nullptr;
  if (cnt & (1 << 25)) src_b = fifo_line_.data();
  else {
    const u32 dispcnt = engine[0].dispcnt();
    const u32 src_bank = (dispcnt >> 18) & 3;
    if (lcdc & (1u << src_bank)) {
      u32 off = line * 256;
      if (((dispcnt >> 16) & 3) != 2) off += ((cnt >> 26) & 3) << 14;
      src_b = reinterpret_cast<const u16*>(vm.bank(src_bank)) + (off & 0xFFFF);
    }
  }

  switch ((cnt >> 29) & 3) {
  case 0:
    kern::active::capture_a15(src_a, width, dst);
    break;
  case 1:
    if (src_b) std::memcpy(dst, src_b, width * sizeof(u16));
    else std::memset(dst, 0, width * sizeof(u16));
    break;
  default: {
    u32 eva = cnt & 0x1F, evb = (cnt >> 8) & 0x1F;
    if (eva > 16) eva = 16;
    if (evb > 16) evb = 16;
    // A missing B source (unmapped LCDC bank) reads as zero either way.
    static constexpr u16 kZeroLine[256] = {};
    kern::active::capture_blend(src_a, src_b ? src_b : kZeroLine, width, eva, evb, dst);
    break;
  }
  }
}


void Gpu::apply_master_brightness(u16 reg, u32* dst) { kern::active::master_brightness(reg, dst); }

// 6-bit RGB666 records -> 8-bit 0xAARRGGBB (top two bits replicated into the low two).
void Gpu::expand_colours(u32* dst) { kern::active::expand_colours(dst); }


void Gpu::quiesce() {
  join_worker();
  engine[0].apply_pending(); engine[1].apply_pending();
}

void Gpu::prepare_load() {
  join_worker();
  release_read_trap();
  disarm_trap();
  lazy_frame_ = false; per_line_[0] = per_line_[1] = true;
  render_next_[0] = render_next_[1] = SCREEN_H; frame_finished_ = true;
  burst_[0] = burst_[1] = false; burst_left_[0] = burst_left_[1] = 0; lag_frame_ = false;
}

template <class S> void Gpu::sync_state(S& s) {
  s.begin("GPU ");
  s.fields(line_, hblank_done_, frame_begun_, screens_on_, master_bright_g_, capcnt_, capture_on_, fifo_, fifo_rd_, fifo_wr_, fifo_line_, run_fifo_);
  s.fields(fb_);   // what the display shows until the next frame (and the thumbnail)
  s.end();
  engine[0].sync_state(s);
  engine[1].sync_state(s);
  if constexpr (S::reading) {
    nds_.sched.rebind(EventId::HBlank, ev_hblank);
    nds_.sched.rebind(EventId::VBlank_Scanline, ev_scanline);
    nds_.sched.rebind(EventId::DisplayFifo, ev_fifo);
  }
}
template void Gpu::sync_state<state::Writer>(state::Writer&);
template void Gpu::sync_state<state::Reader>(state::Reader&);

void Gpu::after_load() {
  // State was taken right after line 0's begin_frame(); its decisions depend
  // only on restored registers/DMA state, so retaking them re-arms the trap.
  if (frame_begun_ && line_ == 0 && !hblank_done_) begin_frame();
}

} // namespace ds::gpu
