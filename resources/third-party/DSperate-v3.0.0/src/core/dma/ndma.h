// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"

#include <array>

namespace ds { struct NDS; }

namespace ds::dma {

// DSi "new" DMA: four channels per CPU at 0x04004100, 32-bit words only,
// with block/total counts, a fill mode and its own start-mode numbering.
// Sub-block timer and round-robin arbitration are not modelled. Stalls its
// CPU like the old DMA; Dma::run hands the CPU's share to run() after the
// old channels, running bits sit in Dma's masks (bits 4-7).
class Ndma {
public:
  explicit Ndma(NDS& nds);
  void reset();
  template <class S> void sync_state(S& s);

  // Register file, 0x04004100-0x040041EF (both CPUs, each its own).
  u32  read(Cpu cpu, u32 addr) const;
  void write(Cpu cpu, u32 addr, u32 v);

  // Start modes in NDMA numbering (old DMA modes translate through
  // Dma::ndma_mode). ARM7 channels carry 0x20.
  static constexpr u32 MODE_IMMEDIATE = 0x10;
  void check(Cpu cpu, u32 mode);     // start channels waiting on `mode`
  void stop(Cpu cpu, u32 mode);      // clear the enable of channels in `mode`
  bool in_mode(Cpu cpu, u32 mode) const;
  bool any_running(Cpu cpu) const { return running_[cpu == Cpu::ARM9 ? 0 : 1] != 0; }

  // Run the CPU's channels for up to `budget` of that CPU's cycles; returns
  // the cycles consumed.
  u32 run(Cpu cpu, u32 budget);
  void set_clock9_shift(u32 s) { shift9_ = s; }
  u32  run_base_ = 0;                 // see Dma::run_base_

private:
  struct Channel {
    Cpu cpu; int num;
    u32 src = 0, dst = 0, total = 0, block = 0, subblock_timer = 0, fill = 0, cnt = 0;
    u32 cur_src = 0, cur_dst = 0;
    s32 src_inc = 1, dst_inc = 1;
    u32 start_mode = 0;
    u32 rem_count = 0, iter_count = 0, total_rem = 0;
    u32 running = 0;          // 0 idle, 1 running, 2 running (burst start)
    bool in_progress = false;
  };
  NDS& nds_;
  u32 gcr_[2] = {0, 0};      // 0x04004100 per CPU
  u8  running_[2] = {0, 0};
  u32 shift9_ = 2;
  std::array<Channel, 8> ch_;
  Channel& channel(Cpu cpu, int n) { return ch_[static_cast<int>(cpu) * 4 + n]; }
  const Channel& channel(Cpu cpu, int n) const { return ch_[static_cast<int>(cpu) * 4 + n]; }
  void write_cnt(Channel& c, u32 v);
  void start(Channel& c);
  void set_running(Channel& c, u32 v);
  u32  run_channel(Channel& c, u32 budget);
  void finished(Channel& c);
};

} // namespace ds::dma
