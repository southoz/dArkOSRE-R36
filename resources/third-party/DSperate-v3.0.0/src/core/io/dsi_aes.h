// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// DSi's AES engine at 0x04004400 (ARM7 only): AES-128 CTR/CCM over 16-byte blocks through 16-word
// input/output FIFOs, four key slots (direct or KeyX/KeyY derived), NDMA modes 0x2A/0x2B. Blocks
// process synchronously as data arrives.
#pragma once
#include "core/types.h"

namespace ds {

struct NDS;

class DsiAes {
public:
  explicit DsiAes(NDS& nds) : nds_(nds) {}
  void reset();
  template <class S> void sync_state(S& s);

  // Register file, offsets from 0x04004400. IV/MAC/key writes come through with a byte mask.
  u32  read_cnt() const;
  void write_cnt(u32 value);
  void write_blkcnt(u32 value);
  u32  read_output_fifo();
  void write_input_fifo(u32 value);
  void write_iv(u32 offset, u32 value, u32 mask);
  void write_mac(u32 offset, u32 value, u32 mask);
  void write_key_normal(u32 slot, u32 offset, u32 value, u32 mask);
  void write_key_x(u32 slot, u32 offset, u32 value, u32 mask);
  void write_key_y(u32 slot, u32 offset, u32 value, u32 mask);

  void check_input_dma();
  void check_output_dma();

  // Key scrambler: normal = ROL128((X ^ Y) + C, 42).
  static void rol16(u8* v, u32 n);
  static void derive_normal_key(const u8* kx, const u8* ky, u8* out);

private:
  struct Fifo {
    u32 entries[16] = {};
    u32 occupied = 0, rd = 0, wr = 0;
    void clear() { occupied = rd = wr = 0; entries[0] = 0; }
    bool empty() const { return occupied == 0; }
    bool full() const { return occupied >= 16; }
    u32 level() const { return occupied; }
    void write(u32 v) { if (full()) return; entries[wr] = v; if (++wr >= 16) wr = 0; ++occupied; }
    u32 read() {                       // an empty FIFO re-reads the last entry
      if (empty()) return entries[(rd == 0 ? 16 : rd) - 1];
      const u32 v = entries[rd]; if (++rd >= 16) rd = 0; --occupied; return v;
    }
  };
  void update();
  void process_ccm_extra();
  void process_ccm_decrypt();
  void process_ccm_encrypt();
  void process_ctr();
  void push_output_mac();

  NDS& nds_;
  u32 cnt_ = 0, blkcnt_ = 0, rem_extra_ = 0, rem_blocks_ = 0;
  u32 input_dma_size_ = 0, output_dma_size_ = 0, mode_ = 0;
  Fifo in_, out_;
  u8 iv_[16] = {}, mac_[16] = {};
  u8 key_normal_[4][16] = {}, key_x_[4][16] = {}, key_y_[4][16] = {};
  u8 cur_key_[16] = {}, cur_mac_[16] = {}, output_mac_[16] = {};
  bool output_mac_due_ = false;
  // Running cipher: tiny-AES-c AES_ctx, kept as raw bytes so this header needs no view of it.
  alignas(4) u8 ctx_[192] = {};
};

} // namespace ds
