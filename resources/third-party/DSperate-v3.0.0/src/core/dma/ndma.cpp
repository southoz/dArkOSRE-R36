// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/dma/ndma.h"
#include "core/dma/dma.h"
#include "core/state/state.h"
#include "core/nds.h"
#include "core/mem/timing.h"

#include <cstdio>
#include <cstdlib>

namespace ds::dma {

Ndma::Ndma(NDS& nds) : nds_(nds) { reset(); }

void Ndma::reset() {
  for (int i = 0; i < 8; ++i) { ch_[i] = Channel{}; ch_[i].cpu = i < 4 ? Cpu::ARM9 : Cpu::ARM7; ch_[i].num = i & 3; }
  gcr_[0] = gcr_[1] = 0;
  running_[0] = running_[1] = 0;
}

void Ndma::set_running(Channel& c, u32 v) {
  c.running = v;
  u8& m = running_[c.cpu == Cpu::ARM9 ? 0 : 1];
  if (v) m |= static_cast<u8>(1u << c.num); else m &= static_cast<u8>(~(1u << c.num));
  nds_.dma.set_ndma_running(c.cpu, m != 0);
}

u32 Ndma::read(Cpu cpu, u32 addr) const {
  const u32 r = addr & 0xFF;
  if (r == 0x00) return gcr_[cpu == Cpu::ARM9 ? 0 : 1];
  if (r < 0x04 || r >= 0x04 + 4 * 0x1C) return 0;
  const Channel& c = channel(cpu, static_cast<int>((r - 4) / 0x1C));
  switch ((r - 4) % 0x1C) {
  case 0x00: return c.src;
  case 0x04: return c.dst;
  case 0x08: return c.total;
  case 0x0C: return c.block;
  case 0x10: return c.subblock_timer;
  case 0x14: return c.fill;
  case 0x18: return c.cnt;
  default: return 0;
  }
}

void Ndma::write(Cpu cpu, u32 addr, u32 v) {
  const u32 r = addr & 0xFF;
  if (r == 0x00) { gcr_[cpu == Cpu::ARM9 ? 0 : 1] = v & 0x800F0000; return; }
  if (r < 0x04 || r >= 0x04 + 4 * 0x1C) return;
  Channel& c = channel(cpu, static_cast<int>((r - 4) / 0x1C));
  switch ((r - 4) % 0x1C) {
  case 0x00: c.src = v & 0xFFFFFFFC; return;
  case 0x04: c.dst = v & 0xFFFFFFFC; return;
  case 0x08: c.total = v & 0x0FFFFFFF; return;
  case 0x0C: c.block = v & 0x00FFFFFF; return;
  case 0x10: c.subblock_timer = v & 0x0003FFFF; return;
  case 0x14: c.fill = v; return;
  case 0x18: write_cnt(c, v); return;
  default: return;
  }
}

void Ndma::write_cnt(Channel& c, u32 v) {
  const u32 old = c.cnt;
  c.cnt = v;
  if ((old & 0x80000000) || !(v & 0x80000000)) return;
  c.cur_src = c.src; c.cur_dst = c.dst; c.total_rem = c.total;
  switch ((v >> 10) & 3) { case 0: c.dst_inc = 1; break; case 1: c.dst_inc = -1; break; case 2: c.dst_inc = 0; break; default: c.dst_inc = 1; break; }
  switch ((v >> 13) & 3) { case 0: c.src_inc = 1; break; case 1: c.src_inc = -1; break; default: c.src_inc = 0; break; }   // 3 = fill
  c.start_mode = (v >> 24) & 0x1F;
  if (c.start_mode > 0x10) c.start_mode = 0x10;
  if (c.cpu == Cpu::ARM7) c.start_mode |= 0x20;
  nds_.dma.update_armed();
  if ((c.start_mode & 0x1F) == 0x10) start(c);
  else if (c.start_mode == 0x04 || c.start_mode == 0x24) { if (nds_.io.cart_drq()) start(c); }
  else if (c.start_mode == 0x0A) nds_.gpu3d.check_fifo_dma();
  const u32 m = c.start_mode;
  if (m <= 0x03 || (m >= 0x0C && m <= 0x0F) || (m >= 0x20 && m <= 0x23) || m == 0x27 || (m >= 0x2D && m <= 0x2F))
    std::fprintf(stderr, "[ndma] unimplemented arm%d channel %d start mode %02x\n", c.cpu == Cpu::ARM9 ? 9 : 7, c.num, m);
}

void Ndma::start(Channel& c) {
  if (c.running) return;
  static const bool dbg = std::getenv("DS_DEBUG_DMA") != nullptr;
  if (dbg) std::fprintf(stderr, "[ndma] t=%llu arm%d ch%d mode %02x cnt %08x src %08x dst %08x block %u total %u\n", (unsigned long long)nds_.sched.now(), c.cpu == Cpu::ARM9 ? 9 : 7, c.num, c.start_mode, c.cnt, c.cur_src, c.cur_dst, c.block, c.total_rem);
  if (!c.in_progress) { c.rem_count = c.block; if (!c.rem_count) c.rem_count = 0x1000000; }
  c.iter_count = (c.start_mode == 0x0A && c.rem_count > 112) ? 112 : c.rem_count;
  if ((c.start_mode & 0x1F) != 0x10 && !(c.cnt & (1u << 29))) {
    if (c.iter_count > c.total_rem) { c.iter_count = c.total_rem; c.rem_count = c.iter_count; }
  }
  if (c.cnt & (1u << 12)) c.cur_dst = c.dst;
  if (c.cnt & (1u << 15)) c.cur_src = c.src;
  set_running(c, 2);
  c.in_progress = true;
  if (!nds_.sched.in_dma()) nds_.sched.preempt(nds_.cpu(c.cpu));
}

void Ndma::check(Cpu cpu, u32 mode) {
  for (int n = 0; n < 4; ++n) { Channel& c = channel(cpu, n); if (c.start_mode == mode && (c.cnt & 0x80000000)) start(c); }
}
void Ndma::stop(Cpu cpu, u32 mode) {
  bool any = false;   // update_armed only caches in_mode answers: nothing to redo unless a channel stopped
  for (int n = 0; n < 4; ++n) { Channel& c = channel(cpu, n); if (c.start_mode == mode && (c.cnt & 0x80000000u)) { c.cnt &= ~0x80000000u; any = true; } }
  if (any) nds_.dma.update_armed();
}
bool Ndma::in_mode(Cpu cpu, u32 mode) const {
  for (int n = 0; n < 4; ++n) { const Channel& c = channel(cpu, n); if (c.start_mode == mode && (c.cnt & 0x80000000)) return true; }
  return false;
}

// Unit cost is the two ends' N32 (both in main RAM) or S32 costs, plus one
// when both ends are in the same region and minus one when only the source
// is main RAM; ARM9 units are scaled to the core clock.
u32 Ndma::run_channel(Channel& c, u32 budget) {
  const bool a9 = c.cpu == Cpu::ARM9;
  set_running(c, 1);
  const mem::Timing& t = nds_.bus.timing();
  u32 sn, ss, dn, dsx;
  t.ndma_cost(a9, c.cur_src, sn, ss);
  t.ndma_cost(a9, c.cur_dst, dn, dsx);
  u32 unit;
  const bool src_main = (c.cur_src >> 24) == 0x02, dst_main = (c.cur_dst >> 24) == 0x02;
  if (src_main && dst_main) unit = sn + dn;
  else {
    unit = ss + dsx;
    const bool same = a9 ? ((c.cur_src >> 24) == (c.cur_dst >> 24)) : ((c.cur_src >> 23) == (c.cur_dst >> 23));
    if (same) unit++;
    else if (src_main) unit--;
  }
  if (a9) unit <<= shift9_;
  const bool fill = ((c.cnt >> 13) & 3) == 3;
  mem::Bus& bus = nds_.bus;
  const bool direct = !a9 && !io::Io::census_on() && !mem::Bus::watch_active();
  // SCFG_EXT gates the ports; nothing a transfer does can change it, so
  // checked once here, not per word.
  const bool src_fifo = direct && !fill && c.src_inc == 0 && (c.cur_src == 0x0400490C || c.cur_src == 0x0400440C) && nds_.io.dsi_io_access(Cpu::ARM7, c.cur_src);
  const bool dst_aes = direct && c.dst_inc == 0 && c.cur_dst == 0x04004408 && nds_.io.dsi_io_access(Cpu::ARM7, c.cur_dst);
  u32 used = 0;
  while (c.iter_count > 0) {
    used += unit;
    nds_.sched.dma_progress(run_base_ + used);
    const u32 v = fill ? c.fill : src_fifo ? nds_.io.ndma_read7(c.cur_src) : bus.dma_read32(c.cpu, c.cur_src);
    if (dst_aes) nds_.io.ndma_write7_aes(v);
    else bus.dma_write32(c.cpu, c.cur_dst, v);
    c.cur_src += c.src_inc * 4;
    c.cur_dst += c.dst_inc * 4;
    c.iter_count--; c.rem_count--; c.total_rem--;
    if (used >= budget) break;
  }
  if (c.rem_count) {
    if (c.iter_count == 0) {
      set_running(c, 0);
      if (c.start_mode == 0x0A) nds_.gpu3d.check_fifo_dma();
      if (c.cpu == Cpu::ARM7) { nds_.io.aes.check_input_dma(); nds_.io.aes.check_output_dma(); }
    }
    return used;
  }
  finished(c);
  return used;
}

void Ndma::finished(Channel& c) {
  const bool irq = c.cnt & (1u << 30);
  if ((c.start_mode & 0x1F) == 0x10) { c.cnt &= ~(1u << 31); if (irq) nds_.io.request_irq(c.cpu, io::IRQ_DSI_NDMA0 + c.num); }
  else if (c.cnt & (1u << 29)) { if (irq) nds_.io.request_irq(c.cpu, io::IRQ_DSI_NDMA0 + c.num); }
  else if (c.total_rem == 0) { c.cnt &= ~(1u << 31); if (irq) nds_.io.request_irq(c.cpu, io::IRQ_DSI_NDMA0 + c.num); }
  set_running(c, 0);
  c.in_progress = false;
  nds_.dma.update_armed();
  if (c.cpu == Cpu::ARM7) { nds_.io.aes.check_input_dma(); nds_.io.aes.check_output_dma(); }   // every ARM7 NDMA end re-polls the AES FIFOs
  if (c.start_mode == 0x04 || c.start_mode == 0x24) { if (nds_.io.cart_drq()) start(c); }
}

u32 Ndma::run(Cpu cpu, u32 budget) {
  u32 used = 0;
  const u32 base0 = run_base_;
  for (int n = 0; n < 4 && used < budget; ++n) {
    Channel& c = channel(cpu, n);
    if (c.running) { run_base_ = base0 + used; used += run_channel(c, budget - used); }
  }
  run_base_ = base0;
  return used;
}

template <class S> void Ndma::sync_state(S& s) {
  s.fields(gcr_);
  for (Channel& c : ch_)
    s.fields(c.src, c.dst, c.total, c.block, c.subblock_timer, c.fill, c.cnt, c.cur_src, c.cur_dst, c.src_inc, c.dst_inc,
             c.start_mode, c.rem_count, c.iter_count, c.total_rem, c.running, c.in_progress);
  if constexpr (S::reading) {
    running_[0] = running_[1] = 0;
    for (Channel& c : ch_) if (c.running) running_[c.cpu == Cpu::ARM9 ? 0 : 1] |= static_cast<u8>(1u << c.num);
    nds_.dma.set_ndma_running(Cpu::ARM9, running_[0] != 0);
    nds_.dma.set_ndma_running(Cpu::ARM7, running_[1] != 0);
    nds_.dma.update_armed();
  }
}
template void Ndma::sync_state<state::Writer>(state::Writer&);
template void Ndma::sync_state<state::Reader>(state::Reader&);

} // namespace ds::dma
