// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/io/dsi_aes.h"
#include "core/nds.h"
#include "core/io/io.h"
#include "core/dma/ndma.h"
#include "core/mem/bus.h"
#include "core/state/state.h"
#include "core/crypto/aes.h"

#include <cstddef>
#include <cstring>

namespace ds {

static_assert(sizeof(AES_ctx) == 192 && offsetof(AES_ctx, Iv) == 176, "DsiAes::ctx_ mirrors tiny-AES-c's AES_ctx");

namespace {

// The engine works on byte-reversed 128-bit blocks: register words are little-endian, the
// cipher sees them mirrored.
void bswap128(u8* dst, const u8* src) { for (int i = 0; i < 16; ++i) dst[i] = src[15 - i]; }
u32 get32(const u8* p) { u32 v; std::memcpy(&v, p, 4); return v; }
void put32(u8* p, u32 v) { std::memcpy(p, &v, 4); }
void masked32(u8* p, u32 value, u32 mask) { put32(p, (get32(p) & ~mask) | (value & mask)); }

} // namespace

#define CTX reinterpret_cast<AES_ctx*>(ctx_)

void DsiAes::rol16(u8* v, u32 n) {
  const u32 coarse = n >> 3, fine = n & 7;
  u8 t[16];
  for (u32 i = 0; i < 16; ++i) t[i] = v[(i - coarse) & 0xF];
  for (u32 i = 0; i < 16; ++i) v[i] = static_cast<u8>((t[i] << fine) | (t[(i - 1) & 0xF] >> (8 - fine)));
}

void DsiAes::derive_normal_key(const u8* kx, const u8* ky, u8* out) {
  static const u8 key_const[16] = {0xFF, 0xFE, 0xFB, 0x4E, 0x29, 0x59, 0x02, 0x58, 0x2A, 0x68, 0x0F, 0x5F, 0x1A, 0x4F, 0x3E, 0x79};
  u8 t[16];
  for (int i = 0; i < 16; ++i) t[i] = kx[i] ^ ky[i];
  u32 carry = 0;
  for (int i = 0; i < 16; ++i) { const u32 r = t[i] + key_const[15 - i] + carry; t[i] = static_cast<u8>(r); carry = r >> 8; }
  rol16(t, 42);
  std::memcpy(out, t, 16);
}

void DsiAes::reset() {
  cnt_ = blkcnt_ = rem_extra_ = rem_blocks_ = 0;
  input_dma_size_ = output_dma_size_ = mode_ = 0;
  in_.clear(); out_.clear();
  std::memset(iv_, 0, sizeof iv_); std::memset(mac_, 0, sizeof mac_);
  std::memset(key_normal_, 0, sizeof key_normal_); std::memset(key_x_, 0, sizeof key_x_); std::memset(key_y_, 0, sizeof key_y_);
  std::memset(cur_key_, 0, sizeof cur_key_); std::memset(cur_mac_, 0, sizeof cur_mac_); std::memset(output_mac_, 0, sizeof output_mac_);
  output_mac_due_ = false;
  static const u8 zero[16] = {};
  AES_init_ctx_iv(CTX, zero, zero);
  // Slot 0 "Nintendo", slots 1/3 mix the console ID, slot 2's KeyX lives in the ARM9i BIOS.
  const u64 cid = nds_.io.dsi.console_id;
  put32(&key_x_[0][0], 0x746E694E); put32(&key_x_[0][4], 0x6F646E65);
  put32(&key_x_[1][0], 0x4E00004A); put32(&key_x_[1][4], 0x4A00004E);
  put32(&key_x_[1][8], static_cast<u32>(cid >> 32) ^ 0xC80C4B72); put32(&key_x_[1][12], static_cast<u32>(cid));
  std::memcpy(key_x_[2], nds_.bus.bios9i.get() + 0x8B8C, 16);
  put32(&key_x_[3][0], static_cast<u32>(cid)); put32(&key_x_[3][4], static_cast<u32>(cid) ^ 0x24EE6906);
  put32(&key_x_[3][8], static_cast<u32>(cid >> 32) ^ 0xE65B601D); put32(&key_x_[3][12], static_cast<u32>(cid >> 32));
  put32(&key_y_[3][0], 0x0AB9DC76); put32(&key_y_[3][4], 0xBD4DC4D3); put32(&key_y_[3][8], 0x202DDD1D);
}

template <class S> void DsiAes::sync_state(S& s) {
  s.fields(cnt_, blkcnt_, rem_extra_, rem_blocks_, input_dma_size_, output_dma_size_, mode_,
           in_.entries, in_.occupied, in_.rd, in_.wr, out_.entries, out_.occupied, out_.rd, out_.wr,
           iv_, mac_, key_normal_, key_x_, key_y_, cur_key_, cur_mac_, output_mac_, output_mac_due_, ctx_);
}
template void DsiAes::sync_state<state::Writer>(state::Writer&);
template void DsiAes::sync_state<state::Reader>(state::Reader&);

// ---- block processing -------------------------------------------------------

void DsiAes::process_ccm_extra() {           // associated data: MAC only
  u8 data[16], rev[16];
  for (int k = 0; k < 4; ++k) put32(&data[k * 4], in_.read());
  bswap128(rev, data);
  for (int i = 0; i < 16; ++i) cur_mac_[i] ^= rev[i];
  AES_ECB_encrypt(CTX, cur_mac_);
}

void DsiAes::process_ccm_decrypt() {
  u8 data[16], rev[16];
  for (int k = 0; k < 4; ++k) put32(&data[k * 4], in_.read());
  bswap128(rev, data);
  AES_CTR_xcrypt_buffer(CTX, rev, 16);
  for (int i = 0; i < 16; ++i) cur_mac_[i] ^= rev[i];
  AES_ECB_encrypt(CTX, cur_mac_);
  bswap128(data, rev);
  for (int k = 0; k < 4; ++k) out_.write(get32(&data[k * 4]));
}

void DsiAes::process_ccm_encrypt() {
  u8 data[16], rev[16];
  for (int k = 0; k < 4; ++k) put32(&data[k * 4], in_.read());
  bswap128(rev, data);
  for (int i = 0; i < 16; ++i) cur_mac_[i] ^= rev[i];
  AES_CTR_xcrypt_buffer(CTX, rev, 16);
  AES_ECB_encrypt(CTX, cur_mac_);
  bswap128(data, rev);
  for (int k = 0; k < 4; ++k) out_.write(get32(&data[k * 4]));
}

// Block goes in/out byte-reversed; XOR the keystream in reversed too, to skip the copies.
void DsiAes::process_ctr() {
  u8 ks[16], ksr[16];
  AES_CTR_next_keystream(CTX, ks);
  bswap128(ksr, ks);
  for (int k = 0; k < 4; ++k) out_.write(in_.read() ^ get32(&ksr[k * 4]));
}

void DsiAes::push_output_mac() {
  for (int k = 0; k < 4; ++k) out_.write(get32(&output_mac_[k * 4]));
  output_mac_due_ = false;
}

// ---- registers ----------------------------------------------------------------

u32 DsiAes::read_cnt() const { return cnt_ | in_.level() | (out_.level() << 5); }

void DsiAes::write_cnt(u32 value) {
  const u32 old = cnt_;
  cnt_ = value & 0xFC1FF000;
  static const u32 dma_in[4] = {0, 4, 8, 12}, dma_out[4] = {4, 8, 12, 16};
  input_dma_size_ = dma_in[(value >> 12) & 3];
  output_dma_size_ = dma_out[(value >> 14) & 3];
  mode_ = (value >> 28) & 3;
  if (value & (1u << 24)) std::memcpy(cur_key_, key_normal_[(value >> 26) & 3], 16);
  if ((old & (1u << 31)) || !(value & (1u << 31))) return;
  // CCM modes count associated blocks in the low half of BLKCNT.
  rem_extra_ = (mode_ < 2) ? (blkcnt_ & 0xFFFF) : 0;
  rem_blocks_ = blkcnt_ >> 16;
  output_mac_due_ = false;
  if (rem_blocks_ == 0 && rem_extra_ == 0) { cnt_ &= ~(1u << 31); return; }
  u8 key[16], iv[16];
  bswap128(key, cur_key_);
  bswap128(iv, iv_);
  if (mode_ < 2) {
    // CCM: counter block is flags | nonce(12) | 0 0 1; MAC's B0 carries MAC length, adata bit,
    // and payload block count.
    u32 maclen = (value >> 16) & 7;
    if (maclen < 1) maclen = 1;
    iv[0] = 0x02;
    for (int i = 0; i < 12; ++i) iv[1 + i] = iv[4 + i];
    iv[13] = 0; iv[14] = 0; iv[15] = 1;
    AES_init_ctx_iv(CTX, key, iv);
    iv[0] |= static_cast<u8>((maclen << 3) | ((blkcnt_ & 0xFFFF) ? (1u << 6) : 0));
    iv[13] = static_cast<u8>(rem_blocks_ >> 12);
    iv[14] = static_cast<u8>(rem_blocks_ >> 4);
    iv[15] = static_cast<u8>(rem_blocks_ << 4);
    std::memcpy(cur_mac_, iv, 16);
    AES_ECB_encrypt(CTX, cur_mac_);
  } else {
    AES_init_ctx_iv(CTX, key, iv);
  }
  nds_.ndma.check(Cpu::ARM7, 0x2A);
}

void DsiAes::write_blkcnt(u32 value) { blkcnt_ = value; }

u32 DsiAes::read_output_fifo() {
  const u32 v = out_.read();
  if (cnt_ & (1u << 31)) {
    check_input_dma();
    check_output_dma();
  } else {
    if (out_.level() > 0) nds_.ndma.check(Cpu::ARM7, 0x2B);
    else nds_.ndma.stop(Cpu::ARM7, 0x2B);
    if (output_mac_due_ && out_.level() <= 12) push_output_mac();
  }
  return v;
}

void DsiAes::write_input_fifo(u32 value) {
  in_.write(value);
  if (cnt_ & (1u << 31)) update();
}

void DsiAes::check_input_dma() {
  if (rem_blocks_ == 0 && rem_extra_ == 0) return;
  if (in_.level() <= input_dma_size_) nds_.ndma.check(Cpu::ARM7, 0x2A);
  update();
}

void DsiAes::check_output_dma() {
  if (out_.level() >= output_dma_size_) nds_.ndma.check(Cpu::ARM7, 0x2B);
}

void DsiAes::update() {
  while (in_.level() >= 4 && rem_extra_ > 0) { process_ccm_extra(); --rem_extra_; }
  if (rem_extra_ == 0) {
    while (in_.level() >= 4 && out_.level() <= 12 && rem_blocks_ > 0) {
      switch (mode_) {
      case 0: process_ccm_decrypt(); break;
      case 1: process_ccm_encrypt(); break;
      default: process_ctr(); break;
      }
      --rem_blocks_;
    }
  }
  check_output_dma();
  if (rem_blocks_ != 0 || rem_extra_ != 0) return;
  // CCM finalises the MAC with counter 0 -- verified against MAC on decrypt (CNT bit 21),
  // appended to the output on encrypt.
  AES_ctx* ctx = CTX;
  if (mode_ == 0) {
    ctx->Iv[13] = ctx->Iv[14] = ctx->Iv[15] = 0;
    AES_CTR_xcrypt_buffer(ctx, cur_mac_, 16);
    cnt_ |= 1u << 21;
    for (int i = 0; i < 16; ++i) if (cur_mac_[15 - i] != mac_[i]) cnt_ &= ~(1u << 21);
  } else if (mode_ == 1) {
    ctx->Iv[13] = ctx->Iv[14] = ctx->Iv[15] = 0;
    AES_CTR_xcrypt_buffer(ctx, cur_mac_, 16);
    bswap128(output_mac_, cur_mac_);
    if (out_.level() <= 12) push_output_mac(); else output_mac_due_ = true;
    cnt_ &= ~(1u << 21);
  } else {
    cnt_ &= ~(1u << 21);
  }
  cnt_ &= ~(1u << 31);
  if (cnt_ & (1u << 30)) nds_.io.request_irq2(io::IRQ2_AES);
  nds_.ndma.stop(Cpu::ARM7, 0x2A);
  if (!out_.empty()) nds_.ndma.check(Cpu::ARM7, 0x2B);
  else nds_.ndma.stop(Cpu::ARM7, 0x2B);
}

void DsiAes::write_iv(u32 offset, u32 value, u32 mask) { masked32(&iv_[offset & 0xC], value, mask); }
void DsiAes::write_mac(u32 offset, u32 value, u32 mask) { masked32(&mac_[offset & 0xC], value, mask); }
void DsiAes::write_key_normal(u32 slot, u32 offset, u32 value, u32 mask) { masked32(&key_normal_[slot & 3][offset & 0xC], value, mask); }
void DsiAes::write_key_x(u32 slot, u32 offset, u32 value, u32 mask) { masked32(&key_x_[slot & 3][offset & 0xC], value, mask); }
void DsiAes::write_key_y(u32 slot, u32 offset, u32 value, u32 mask) {
  masked32(&key_y_[slot & 3][offset & 0xC], value, mask);
  if ((offset & 0xC) >= 0xC) derive_normal_key(key_x_[slot & 3], key_y_[slot & 3], key_normal_[slot & 3]);   // the last word commits the pair
}

#undef CTX

} // namespace ds
