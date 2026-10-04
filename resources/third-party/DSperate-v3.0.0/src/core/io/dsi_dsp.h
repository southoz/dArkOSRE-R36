// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// DSi's DSP host interface at 0x04004300 (ARM9), no DSP core attached: PDATA/PADR/PCFG/PSTS/
// PSEM/PMASK/PCLEAR, three CMD/REP port pairs, 16-word PDATA read FIFO fed by the PCFG-started
// "DMA". With no core, every DSP-side read is 0 and the write FIFO is always empty (PSTS bit 8),
// matching what system software sees while it holds the DSP in reset.
//
// Not modelled: the core and its catch-up scheduling while enabled. Logged once if reached.
#pragma once
#include "core/types.h"

namespace ds {

struct NDS;

class DsiDsp {
public:
  explicit DsiDsp(NDS& nds) : nds_(nds) {}
  void reset();
  void set_rst_line(bool release);   // SCFG_RST bit 0; also resets the registers
  template <class S> void sync_state(S& s);

  // Offsets from 0x04004300 (the page mirrors every 0x40 bytes).
  u8   read8(u32 addr);
  u16  read16(u32 addr);
  u32  read32(u32 addr) { return read16(addr & 0x3C); }
  void write8(u32 addr, u8 value);
  void write16(u32 addr, u16 value);
  void write32(u32 addr, u32 value) { write16(addr & 0x3C, static_cast<u16>(value)); }

private:
  struct Fifo {
    u16 entries[16] = {};
    u32 occupied = 0, rd = 0, wr = 0;
    void clear() { occupied = rd = wr = 0; }
    bool empty() const { return occupied == 0; }
    bool full() const { return occupied >= 16; }
    void write(u16 v) { if (full()) return; entries[wr] = v; if (++wr >= 16) wr = 0; ++occupied; }
    u16 read() { const u16 v = entries[rd]; if (++rd >= 16) rd = 0; --occupied; return v; }
  };
  u16  psts() const;
  void pdata_write(u16 value);
  u16  pdata_dma_read();
  void pdata_dma_fetch();
  void pdata_dma_start();
  u16  pdata_read_mmio();
  void note_core_state();

  NDS& nds_;
  bool rst_ = false;
  u16 padr_ = 0, pcfg_ = 0, psts_ = 0, psem_ = 0, pmask_ = 0xFF, pclear_ = 0;
  u16 cmd_[3] = {};
  s32 pdata_dma_len_ = 0;   // -1: until cancelled
  Fifo rd_fifo_;
};

} // namespace ds
