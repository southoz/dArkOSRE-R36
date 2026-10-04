// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/io/dsi_dsp.h"
#include "core/nds.h"
#include "core/io/io.h"
#include "core/state/state.h"
#include <cstdio>

namespace ds {

void DsiDsp::reset() {
  padr_ = pcfg_ = psts_ = psem_ = pclear_ = 0;
  pmask_ = 0xFF;
  cmd_[0] = cmd_[1] = cmd_[2] = 0;
  pdata_dma_len_ = 0;
  rd_fifo_.clear();
}

void DsiDsp::set_rst_line(bool release) {
  rst_ = release;
  reset();
  note_core_state();
}

template <class S> void DsiDsp::sync_state(S& s) {
  s.fields(rst_, padr_, pcfg_, psts_, psem_, pmask_, pclear_, cmd_, pdata_dma_len_,
           rd_fifo_.entries, rd_fifo_.occupied, rd_fifo_.rd, rd_fifo_.wr);
}
template void DsiDsp::sync_state<state::Writer>(state::Writer&);
template void DsiDsp::sync_state<state::Reader>(state::Reader&);

// Log once when the (unmodelled) core would start, rather than diverge silently.
void DsiDsp::note_core_state() {
  static bool warned = false;
  if (warned) return;
  if ((nds_.io.dsi.scfg_clock9 & (1u << 1)) && rst_ && !(pcfg_ & 1)) {
    warned = true;
    std::fprintf(stderr, "[dsp] DSP core enabled (reset released) -- not modelled\n");
  }
}

// Bit 9 (semaphore IRQ) is the only sticky bit; write FIFO always empty; bits 10-15 clear.
u16 DsiDsp::psts() const {
  u16 r = static_cast<u16>((psts_ & (1u << 9)) | (1u << 8));
  if (rd_fifo_.full()) r |= 1u << 5;
  if (!rd_fifo_.empty()) r |= (1u << 6) | (1u << 0);
  return r;
}

void DsiDsp::pdata_write(u16) {
  if (pcfg_ & (1u << 1)) ++padr_;   // auto-increment (the value would go to the core)
  nds_.io.request_irq(Cpu::ARM9, io::IRQ_DSI_DSP);   // write FIFO empty
}

u16 DsiDsp::pdata_dma_read() {
  if (pcfg_ & (1u << 1)) ++padr_;
  return 0;
}

void DsiDsp::pdata_dma_fetch() {
  if (!pdata_dma_len_) return;
  rd_fifo_.write(pdata_dma_read());
  if (pdata_dma_len_ > 0) --pdata_dma_len_;
}

void DsiDsp::pdata_dma_start() {
  static const s32 lens[4] = {1, 8, 16, -1};
  pdata_dma_len_ = lens[(pcfg_ >> 2) & 3];
  const s32 amt = pdata_dma_len_ < 0 ? 16 : pdata_dma_len_;
  for (s32 i = 0; i < amt; ++i) pdata_dma_fetch();
  nds_.io.request_irq(Cpu::ARM9, io::IRQ_DSI_DSP);
}

u16 DsiDsp::pdata_read_mmio() {
  u16 ret = 0;
  if (!rd_fifo_.empty()) ret = rd_fifo_.read();
  if (pdata_dma_len_ != 0) {
    s32 left = 16 - static_cast<s32>(rd_fifo_.occupied);
    if (pdata_dma_len_ > 0 && pdata_dma_len_ < left) left = pdata_dma_len_;
    for (s32 i = 0; i < left; ++i) pdata_dma_fetch();
  }
  if (!rd_fifo_.empty() || rd_fifo_.full()) nds_.io.request_irq(Cpu::ARM9, io::IRQ_DSI_DSP);
  return ret;
}

u8 DsiDsp::read8(u32 addr) {
  switch (addr & 0x3F) {
  case 0x08: return static_cast<u8>(pcfg_);
  case 0x09: return static_cast<u8>(pcfg_ >> 8);
  case 0x0C: return static_cast<u8>(psts());
  case 0x0D: return static_cast<u8>(psts() >> 8);
  case 0x10: return static_cast<u8>(psem_);
  case 0x11: return static_cast<u8>(psem_ >> 8);
  case 0x14: return static_cast<u8>(pmask_);
  case 0x15: return static_cast<u8>(pmask_ >> 8);
  default: return 0;   // no 8-bit PDATA/PADR/PCLEAR reads; SEM is the core's
  }
}

u16 DsiDsp::read16(u32 addr) {
  switch (addr & 0x3E) {
  case 0x00: return pdata_read_mmio();
  case 0x08: return pcfg_;
  case 0x0C: return psts();
  case 0x10: return psem_;
  case 0x14: return pmask_;
  case 0x20: return cmd_[0];
  case 0x28: return cmd_[1];
  case 0x30: return cmd_[2];
  default: return 0;   // PADR/PCLEAR unreadable; SEM and REP0-2 come from the core
  }
}

void DsiDsp::write8(u32 addr, u8 value) {
  switch (addr & 0x3F) {
  case 0x08: pcfg_ = static_cast<u16>((pcfg_ & 0xFF00) | value); break;   // byte writes skip the side effects
  case 0x09: pcfg_ = static_cast<u16>((pcfg_ & 0x00FF) | (value << 8)); break;
  default: break;
  }
  note_core_state();
}

void DsiDsp::write16(u32 addr, u16 value) {
  switch (addr & 0x3E) {
  case 0x00: pdata_write(value); break;
  case 0x04: padr_ = value; break;
  case 0x08:
    // Bit 0 falling would start the core; no core here.
    if ((pcfg_ & 1) && !(value & 1) && !nds_.dsi_dsp_started) {
      nds_.dsi_dsp_started = true;
      std::fprintf(stderr, "[dsp] DSP start requested (PCFG bit 0 released) -- no core\n");
    }
    pcfg_ = value;
    if (pcfg_ & (1u << 4)) pdata_dma_start();
    else { pdata_dma_len_ = 0; rd_fifo_.clear(); }
    break;
  case 0x10: psem_ = value; break;
  case 0x14: pmask_ = value; break;
  case 0x18: psts_ &= static_cast<u16>(~(1u << 9)); break;   // PCLEAR: with no core the semaphore is always clear
  case 0x20: cmd_[0] = value; break;
  case 0x28: cmd_[1] = value; break;
  case 0x30: cmd_[2] = value; break;
  default: break;
  }
  note_core_state();
}

} // namespace ds
