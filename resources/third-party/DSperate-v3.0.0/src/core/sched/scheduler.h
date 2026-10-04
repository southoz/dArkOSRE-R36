// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include <map>
#include "core/types.h"
#include "core/cpu/cpu.h"

#include <array>
#include <chrono>

namespace ds {

struct NDS;

// Event scheduler and the CPU interleave.
//
// Time is a u64 count of ARM9 cycles. Each CPU runs from a *downward* budget
// (CpuContext::hot.cycle_budget): distance to the next pending event; the
// engine runs until the sign bit sets, the only scheduling test in the hot
// path. ARM7 runs at half clock; its budget is issued in ARM7 cycles.

enum class EventId : u8 {
  HBlank, VBlank_Scanline, Timer0, Timer1, Timer2, Timer3,
  Timer7_0, Timer7_1, Timer7_2, Timer7_3,
  Dma, Spu, Spi, Rtc, Cart, Gx3D, DisplayFifo, Div, Sqrt, LcdIrq, Wifi,
  // DSi only.
  RtcClock, CamIrq, SdMmc, Sdio, NWifi, CamTransfer, CartPower1, CartPower2, Count
};

// CPU interleave is event-bound: each CPU runs to the next scheduled event,
// capped at SLICE_QUANTUM ARM9 cycles because the SDK's IPCSYNC boot
// handshake times out on the ARM7 side if it goes unanswered too long.
constexpr s64 SLICE_QUANTUM = 2048;

using EventFn = void (*)(NDS& nds, u32 param);

// What the native slice loop runs next: the context to enter and the native
// entry (returned in x0/x1); ctx == nullptr ends the run.
struct SliceNext { CpuContext* ctx; const void* native; };
// Event names, for the per-event census (DS_PROFILE).
const char* event_name(u32 id);

class Scheduler {
public:
  explicit Scheduler(NDS& nds);
  ~Scheduler();   // prints the per-event census when DS_PROFILE is on

  void reset();

  // Set at reset, before set_clock9_shift.
  void set_dsi(bool dsi) { dsi_ = dsi; arm9_carry_ = 0; slice_margin_ = dsi ? 16 : 0; cut_on_schedule_ = dsi; }
  // Timing::clock9_shift (1 or 2). On a DSi this can change mid-slice (SCFG_CLK9).
  void set_clock9_shift(u32 timing_shift);
  u32  clock9_shift() const { return shift9_; }   // 1 = DSi ARM9 at 134 MHz
  // DSi SCFG_CLK9 write: floors ARM9Timestamp down to system cycles and back.
  void floor_arm9_clock(CpuContext& a9) {
    if (!dsi_) return;
    if (running_ != &a9) { arm9_carry_ = 0; return; }
    const s64 c = static_cast<s64>(running_start_budget_ - a9.hot.cycle_budget - a9.preempt_residual) + arm9_carry_;
    a9.hot.cycle_budget += static_cast<s32>(c & ((s64{1} << (shift9_ + 1)) - 1));
  }

  void schedule(EventId id, u64 at, EventFn fn, u32 param = 0);
  void cancel(EventId id);
  // schedule() overwrites a live event rather than refusing to re-arm it.
  bool armed(EventId id) const { return (armed_ & (1u << static_cast<u32>(id))) != 0; }

  // Current time, including the running CPU's cycles consumed so far, so
  // events scheduled mid-instruction are stamped relative to its position.
  u64 now() const {
    if (!running_) return now_;
    const u64 c = static_cast<u64>(running_start_budget_ - running_->hot.cycle_budget - running_->preempt_residual) + dma_used_;
    if (running_rshift_) return running_base_ + (((c + running_carry_) >> running_rshift_) << 1);
    return running_base_ + (c << running_shift_);
  }
  const CpuContext* running() const { return running_; }
  // Debug: the running DSi ARM9's clock in its own core cycles, before now()'s flooring.
  u64 now_fine9() const {
    const u64 c = static_cast<u64>(running_start_budget_ - running_->hot.cycle_budget - running_->preempt_residual) + dma_used_;
    return (running_base_ << (running_rshift_ - 1)) + c + running_carry_;
  }
  bool idle_skip_enabled() const { return idle_skip_ != 0; }
  // The idle cut (on by default; DS_IDLE_CUT=0 / emu.idle_cut = false for the
  // exact timing): a CPU that keeps ending slices in a state it has already
  // been in -- same PC (or nearby, in the same loop) and identical registers --
  // is only re-reading. It gets a short probe instead of the whole slice; if the
  // probe ends in a known state too, the rest of the slice counts as run.
  void set_idle_cut(bool on) { idle_cut_ = on; }
  bool idle_cut() const { return idle_cut_; }
  void set_idle_skip(const char* mode);   // DS_IDLE_SKIP / emu.idle_skip: "0", "1" or "all"/"2"

  // Called when an immediate DMA starts on `cpu`: it leaves its run loop
  // after the current instruction and the DMA takes over its slice, as a bus stall would.
  void preempt(CpuContext& cpu) {
    if (running_ != &cpu || cpu.hot.cycle_budget <= 0) return;
    cpu.preempt_residual += cpu.hot.cycle_budget;
    cpu.hot.cycle_budget = 0;
  }
  // Called when `cpu` writes something the other CPU waits on with a tight
  // timeout (IPCSYNC boot handshake): ends the slice so the other CPU runs next.
  void yield(CpuContext& cpu) {
    if (running_ != &cpu || cpu.hot.cycle_budget <= 0) return;
    cpu.yielded = true;
    cpu.preempt_residual += cpu.hot.cycle_budget;
    cpu.hot.cycle_budget = 0;
  }
  u64 next_deadline() const { return next_; }
  bool in_dma() const { return in_dma_; }
  void dma_progress(u32 used) { if (dsi_) dma_used_ = used; }   // DSi only

  // Nominal time of the event whose handler is running; a periodic handler
  // must reschedule from this, not now(), or lateness accumulates.
  u64 event_time() const { return firing_at_; }
  // ARM7's own clock at a slice end (melonDS's ARM7Timestamp). arm7_debt_ is
  // what it still owes toward now_ (negative = ran past the slice end).
  u64 event_base7() const { return static_cast<u64>(static_cast<s64>(now_) - arm7_debt_); }

  // Runs ARM9 then ARM7 up to the next event, fires due events, repeats
  // until `until`. Returns cycles advanced.
  u64 run_until(u64 until);

  // Bounded by the GPU's frame flag instead of a deadline: entered once per frame.
  u64 run_until_frame();
  // Stops at the frame flag or `until`, whichever first.
  u64 run_until_or_frame(u64 until);

  // State machine for the recompiler's native slice loop: runs the scheduler
  // up to the next entry into translated code and returns it; the loop
  // re-enters and calls again when the code leaves.
  SliceNext slice_next();

  // Handlers are function pointers, so only deadlines are saved; each
  // subsystem re-binds its events with rebind() on load, checked by after_load().
  template <class S> void sync_state(S& s);
  void rebind(EventId id, EventFn fn) { const u32 i = static_cast<u32>(id); if (armed_ & (1u << i)) fn_[i] = fn; }
  bool after_load();
  bool at_slice_boundary() const { return running_ == nullptr && !in_dma_; }


private:
  struct SliceState {
    u64 until = 0;
    bool until_frame = false;   // stop on NDS::frame_ready, not on `until`
    int phase = 0;
    s64 slice = 0, ran9 = 0;
    s32 budget7 = 0;
    bool skip9 = false, skip7 = false;   // proven idle loop: do not execute this slice
    u64 t0 = 0;   // prof::now_ns() at the CPU run's start
  } sl_;
  u64 run_until_native(u64 until, bool until_frame);
  u64 run_until_impl(u64 until, bool until_frame);
  void rescan();
  bool done(u64 until, bool until_frame) const;
  u64 next_ = ~u64{0};   // earliest armed deadline (cached)
  u64 firing_at_ = 0;    // deadline of the event being fired
  bool in_dma_ = false;                 // inside Dma::run (a preempt there would corrupt the DMA's budget)
  u32  dma_used_ = 0;                   // budget units the current Dma::run has consumed so far (see now())
  // DSi: an ARM9 DMA gets its own iteration, ARM7 catching up to where it stopped.
  bool a9_dma_iter_ = false;
  bool debug_slices_ = false;           // DS_DEBUG_SLICES
  // Idle-loop skip mode (DS_IDLE_SKIP): 0 off; 1 (default) only while the
  // ARM9 is in a poll loop on GXSTAT with a swap pending; 2 = "all": any
  // proven poll loop.
  u8 idle_skip_ = 1;
  bool idle_cut_ = false;   // DS_IDLE_CUT=1: parked as a net loss on the device (see the commit); re-measured later
  // Idle cut state per CPU (ARM9, ARM7): recent slice-end states, whether the
  // CPU is marked idle, and the budget held back behind this slice's probe.
  static constexpr s32 CUT_PROBE9 = 256, CUT_PROBE7 = 128;
  u32 cut_pc_[2][8] = {};
  u64 cut_h_[2][8] = {};
  u32 cut_pos_[2] = {};
  bool cut_idle_[2] = {};
  s32 cut_held_[2] = {};
  bool cut_ok(const CpuContext& c) const;
  bool cut_seen(int i, const CpuContext& c) const;
  void cut_note(int i, const CpuContext& c);
  void cut_arm(int i, CpuContext& c, s32 probe);
  bool cut_resolve(int i, CpuContext& c);
  bool idle_survey_ = false;      // DS_IDLE_SURVEY: count what the dma veto costs, change nothing
  bool idle_analyse(bool& skip9, bool& skip7, bool survey) const;
  void advance_dma_only(CpuContext& cpu);   // a skipped CPU still lets its DMA run
  static constexpr u32 EVENT_COUNT = static_cast<u32>(EventId::Count);
  std::array<u64, EVENT_COUNT>     at_{};
  std::array<EventFn, EVENT_COUNT> fn_{};
  std::array<u32, EVENT_COUNT>     param_{};
  u32 armed_ = 0;                 // bit i = events_[i] is armed
  u64 ev_ns_[EVENT_COUNT] = {}, ev_n_[EVENT_COUNT] = {};   // DS_PROFILE: host time and count per event id
  u32 next_id_ = EVENT_COUNT;     // which event `next_` belongs to (EVENT_COUNT: none)
  NDS& nds_;
  u64  now_;
  const CpuContext* running_ = nullptr;
  s32  running_start_budget_ = 0;
  u32  running_shift_ = 0;           // 0 for ARM9 cycles, 1 for ARM7 (half clock)
  u32  running_rshift_ = 0;          // DSi ARM9: core cycles per system cycle as a shift (2 at 134 MHz, 1 at 67); 0 otherwise
  u64  running_base_ = 0;            // where the running CPU's clock starts this phase (true position on a DSi ARM7)
  u64  running_carry_ = 0;   // the ARM9's arm9_carry_ at the slice start (DSi)
  s64  arm7_debt_ = 0;               // ARM9 cycles the ARM7 still have to cover (carries overshoot and odd cycles)
  // ARM9 tick: 1 core cycle (DS; DSi at 67 MHz) or 2 (DSi at 134 MHz);
  // arm9_carry_ holds the part of a system cycle past the boundary.
  bool dsi_ = false;
  u32  shift9_ = 0;
  s64  arm9_carry_ = 0;
  // A CPU's boot stall (CpuContext::boot_stall) comes off its budget before it runs.
  void defer_preempt_cost(CpuContext& cpu);
  static void take_stall(CpuContext& cpu) {
    if (cpu.boot_stall <= 0) return;
    const s32 take = cpu.boot_stall < cpu.hot.cycle_budget ? cpu.boot_stall : cpu.hot.cycle_budget;
    cpu.boot_stall -= take; cpu.hot.cycle_budget -= take;
  }
  // ARM9 core cycles consumed -> scheduler ticks (half ARM7 cycles), remainder carried.
  s32 budget9_for(s64 slice) const { return static_cast<s32>((slice << shift9_) - arm9_carry_); }
  s64 ticks9(s64 consumed9) {
    if (!dsi_) return consumed9;
    arm9_carry_ += consumed9;
    const s64 sys = arm9_carry_ >> (shift9_ + 1);
    arm9_carry_ -= sys << (shift9_ + 1);
    return sys << 1;
  }
  // A slice may run up to a small margin past SLICE_QUANTUM to land on an
  // event rather than split off a tiny slice. 0 on DS, 16 on DSi.
  s64  slice_margin_ = 0;
  // DSi only: an event scheduled by the ARM9 earlier than its target
  // shortens the target so the ARM7 catches up only that far.
  bool cut_on_schedule_ = false;
  u64  slice_end_ = 0;      // the running slice's end (now_ + slice)
  s32  budget9_ = 0;        // the ARM9's budget for the running slice, as cut
  void cut_arm9_at(u64 at);
  void fire_due();
  void count_slice(bool skipped, s64 slice) const;
  // See cpu/idle_loop.h. Treats a CPU proven to be in a side-effect-free poll loop as halted.
  bool machine_idle(bool& skip9, bool& skip7) const;
  // ARM7 sitting in a proven SPICNT poll loop while a transfer is in flight
  // is treated as halted; `wake` gets the transfer's ready time.
  bool arm7_spi_poll(u64& wake) const;
  mutable u32 spi_pc_ring_[8] = {};
  mutable u32 spi_pc_pos_ = 0;
  // Rings of each CPU's recent slice-start PCs; "still in the loop" is
  // membership in the last few, not equality with the last one. Ring 0 is
  // the idle-skip pre-filter, ring 1 the DS_PROFILE spin proxy.
  mutable u32 idle_pc_ring_[2][8] = {};
  mutable u32 idle_pc_pos_[2] = {};
  mutable u32 spin_ring_[2][8] = {};
  mutable u64 spin_hash_[2][8] = {};   // register-file hash beside each ring PC (exact idle)
  mutable bool idle_now_[2] = {}, drift_now_[2] = {};
  mutable u32 spin_pos_[2] = {};
  mutable bool spin_now_[2] = {};
  static inline std::map<u32, u32>* spin_opcodes_ = nullptr;
  static inline std::map<u32, const char*>* spin_reject_ = nullptr;   // this slice's classification, for host-time attribution
  // Both CPUs halted with nothing pending that could wake them before the
  // next event: the slice can run to the deadline instead of SLICE_QUANTUM.
  bool both_idle() const;
  void run_cpu(CpuContext& cpu, RunFn run);
};

} // namespace ds
