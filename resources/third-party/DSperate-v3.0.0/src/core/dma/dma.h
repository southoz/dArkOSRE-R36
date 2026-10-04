// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"

#include <array>

namespace ds { struct NDS; }

namespace ds::dma {

// Start modes, normalised so ARM7 modes don't collide with ARM9 ones.
enum Mode : u32 {
  MODE9_IMMEDIATE = 0, MODE9_VBLANK = 1, MODE9_HBLANK = 2, MODE9_DISPLAY_START = 3,
  MODE9_DISPLAY_FIFO = 4, MODE9_CART = 5, MODE9_GBA = 6, MODE9_GXFIFO = 7,
  MODE7_IMMEDIATE = 0x10, MODE7_VBLANK = 0x11, MODE7_CART = 0x12, MODE7_WIFI_GBA = 0x13,
};

struct Channel {
  Cpu cpu; int num;
  u32 src = 0, dst = 0, cnt = 0;
  u32 cur_src = 0, cur_dst = 0;
  s32 src_inc = 1, dst_inc = 1;
  u32 start_mode = 0;
  u32 rem_count = 0, iter_count = 0;
  u32 running = 0;          // 0 idle, 1 running, 2 running (first access of a burst)
  bool in_progress = false;
  const u8* burst_table = nullptr; u32 burst_pos = 0;
  // Unit-timing cache: constant until cur_src/cur_dst leaves its 16 KB (ARM9)
  // / 32 KB (ARM7) block. Keyed on the block indices.
  u32 tim_key_src = ~0u, tim_key_dst = ~0u;
  u32 src_rgn = 0, dst_rgn = 0;
  u32 src_n = 0, src_s = 0, dst_n = 0, dst_s = 0;
};

// Eight DMA channels (4 per CPU). A running channel stalls its CPU; the
// scheduler runs `run()` in its place until the transfer (or current
// iteration) completes.
class Dma {
public:
  explicit Dma(NDS& nds);
  void reset();
  template <class S> void sync_state(S& s);

  void write_src(Cpu cpu, int ch, u32 v);
  void write_dst(Cpu cpu, int ch, u32 v);
  void write_cnt(Cpu cpu, int ch, u32 v);
  u32  read_cnt(Cpu cpu, int ch) const { return channel(cpu, ch).cnt; }
  u32  read_src(Cpu cpu, int ch) const { return channel(cpu, ch).src; }
  u32  read_dst(Cpu cpu, int ch) const { return channel(cpu, ch).dst; }

  // Trigger / cancel channels waiting on a start condition. On a DSi the
  // same condition also reaches the NDMA channels (Ndma), in their numbering.
  void check(Cpu cpu, u32 mode);
  void stop(Cpu cpu, u32 mode);
  bool any_running(Cpu cpu) const { return running_mask_[cpu == Cpu::ARM9 ? 0 : 1] != 0; }
  bool in_mode(Cpu cpu, u32 mode) const;
  // NDMA glue: its running state occupies bit 4 of the per-CPU mask so the
  // scheduler's test stays one load.
  void set_ndma_running(Cpu cpu, bool on) { u8& m = running_mask_[cpu == Cpu::ARM9 ? 0 : 1]; if (on) m |= 0x10; else m &= static_cast<u8>(~0x10); }
  void update_armed() { update_cart_armed(); }
  static u32 ndma_mode(u32 mode);
  void set_clock9_shift(u32 s) { shift9_ = s; track_progress_ = s > 1; }   // DSi: report progress to Scheduler::now() (see dma_progress)
  u32  run_base_ = 0;                 // budget used by earlier channels in this Dma::run (progress base)
  bool track_progress_ = false;
  // Cached `in_mode(cart)` for either CPU: the cart transfer path asks per
  // word and per catch-up, and an eight-channel scan there was too costly.
  bool cart_armed() const { return cart_armed_; }
  // An enabled ARM9 channel in GXFIFO start mode exists: the geometry engine
  // asks after every command it retires, so the answer is kept, not searched.
  bool gx_armed() const { return gx_armed_; }

  // Run the CPU's DMA channels for up to `budget` cycles (that CPU's clock).
  // Returns cycles consumed.
  u32 run(Cpu cpu, u32 budget);
  // A cart-mode channel is running (re-armed inside run() when DRQ is already up).
  bool cart_running(Cpu cpu) const;

private:
  void update_cart_armed();
  u32  shift9_ = 1;           // bus cycles to ARM9 cycles (2 on a DSi at 134 MHz)
  bool cart_armed_ = false;
  bool gx_armed_ = false;
  NDS& nds_;
  u8 running_mask_[2] = {};   // per CPU, bit n = channel n running (any_running is one load)
  void set_running(Channel& c, u32 v) {
    c.running = v;
    u8& m = running_mask_[c.cpu == Cpu::ARM9 ? 0 : 1];
    if (v) m |= static_cast<u8>(1u << c.num); else m &= static_cast<u8>(~(1u << c.num));
  }
  std::array<Channel, 8> ch_;
  Channel& channel(Cpu cpu, int n) { return ch_[static_cast<int>(cpu) * 4 + n]; }
  const Channel& channel(Cpu cpu, int n) const { return ch_[static_cast<int>(cpu) * 4 + n]; }
  void start(Channel& c);
  u32  run_channel(Channel& c, u32 budget);
  u32  run_channel_impl(Channel& c, u32 budget);
  u32  unit_cycles(Channel& c, bool burst_start, bool word);
  struct RunCost; RunCost run_cost(Channel& c, bool word);
};

} // namespace ds::dma
