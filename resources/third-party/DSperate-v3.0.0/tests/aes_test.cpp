// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// AES tests: the vendored tiny-AES-c against the NIST SP 800-38A vectors, the
// DSi key scrambler, and the AES engine at 0x04004400 driven through its
// ARM7 registers -- CTR, a CCM encrypt/decrypt round trip with the MAC
// check, the FIFO levels in AES_CNT and the completion IRQ in IF2.
#include "core/nds.h"
#include "core/io/dsi_aes.h"
extern "C" {
#include "core/crypto/aes.h"
}

#include <cstdio>
#include <cstring>

using namespace ds;

static int failures = 0;
#define CHECK_EQ(a, b) do { auto va_ = (a); auto vb_ = (b); if (static_cast<unsigned long long>(va_) != static_cast<unsigned long long>(vb_)) { std::fprintf(stderr, "FAIL %s:%d: %s = %llx, expected %llx\n", __FILE__, __LINE__, #a, (unsigned long long)va_, (unsigned long long)vb_); ++failures; } } while (0)
#define CHECK_MEM(a, b, n) do { if (std::memcmp((a), (b), (n)) != 0) { std::fprintf(stderr, "FAIL %s:%d: %s != %s\n", __FILE__, __LINE__, #a, #b); ++failures; } } while (0)

static const u8 NIST_KEY[16] = {0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6, 0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c};
static const u8 NIST_PT[16]  = {0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96, 0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a};

static void test_primitives() {
  // F.1.1 ECB-AES128.Encrypt, block 1.
  static const u8 ecb[16] = {0x3a, 0xd7, 0x7b, 0xb4, 0x0d, 0x7a, 0x36, 0x60, 0xa8, 0x9e, 0xca, 0xf3, 0x24, 0x66, 0xef, 0x97};
  AES_ctx ctx; AES_init_ctx(&ctx, NIST_KEY);
  u8 buf[16]; std::memcpy(buf, NIST_PT, 16);
  AES_ECB_encrypt(&ctx, buf);
  CHECK_MEM(buf, ecb, 16);
  // F.5.1 CTR-AES128.Encrypt, block 1.
  static const u8 ctr_iv[16] = {0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff};
  static const u8 ctr[16] = {0x87, 0x4d, 0x61, 0x91, 0xb6, 0x20, 0xe3, 0x26, 0x1b, 0xef, 0x68, 0x64, 0x99, 0x0d, 0xb6, 0xce};
  AES_init_ctx_iv(&ctx, NIST_KEY, ctr_iv);
  std::memcpy(buf, NIST_PT, 16);
  AES_CTR_xcrypt_buffer(&ctx, buf, 16);
  CHECK_MEM(buf, ctr, 16);
}

// The CPU-instruction cipher (where the host has one) against the portable C:
// ECB and a CTR stream over many keys, counters and lengths.
static void test_hw_matches_software() {
  u32 x = 0x12345678;
  auto rnd = [&x] { x ^= x << 13; x ^= x >> 17; x ^= x << 5; return static_cast<u8>(x); };
  for (int n = 0; n < 200; ++n) {
    u8 key[16], iv[16], a[64], b[64];
    for (u8& v : key) v = rnd();
    for (u8& v : iv) v = n < 8 ? 0xFF : rnd();          // the first few carry through the whole counter
    for (u8& v : a) v = rnd();
    std::memcpy(b, a, sizeof a);
    const size_t len = 1 + n % 64;
    AES_ctx c1, c2;
    AES_force_software(0); AES_init_ctx_iv(&c1, key, iv); AES_CTR_xcrypt_buffer(&c1, a, len); AES_ECB_encrypt(&c1, a + 48);
    AES_force_software(1); AES_init_ctx_iv(&c2, key, iv); AES_CTR_xcrypt_buffer(&c2, b, len); AES_ECB_encrypt(&c2, b + 48);
    CHECK_MEM(a, b, sizeof a);
    CHECK_MEM(c1.Iv, c2.Iv, 16);
    // The keystream call is the xcrypt of a zero block.
    AES_ctx c3, c4; u8 z[16] = {}, ks[16];
    AES_init_ctx_iv(&c3, key, iv); AES_init_ctx_iv(&c4, key, iv);
    AES_CTR_xcrypt_buffer(&c3, z, 16); AES_CTR_next_keystream(&c4, ks);
    CHECK_MEM(z, ks, 16);
    CHECK_MEM(c3.Iv, c4.Iv, 16);
  }
  AES_force_software(0);
}

static void test_scrambler() {
  // ROL128 by 42 = 5 bytes + 2 bits; a single set bit lands where expected.
  u8 v[16] = {1};
  DsiAes::rol16(v, 42);
  u8 want[16] = {}; want[5] = 4;
  CHECK_MEM(v, want, 16);
  // X ^ Y == 0 gives ROL(C, 42): the constant itself, rotated.
  static const u8 key_const[16] = {0xFF, 0xFE, 0xFB, 0x4E, 0x29, 0x59, 0x02, 0x58, 0x2A, 0x68, 0x0F, 0x5F, 0x1A, 0x4F, 0x3E, 0x79};
  u8 zero[16] = {}, out[16], ref[16];
  for (int i = 0; i < 16; ++i) ref[i] = key_const[15 - i];
  DsiAes::rol16(ref, 42);
  DsiAes::derive_normal_key(zero, zero, out);
  CHECK_MEM(out, ref, 16);
}

// ---- the engine through its registers -------------------------------------------

static void w32(NDS& nds, u32 a, u32 v) { nds.bus.dma_write32(Cpu::ARM7, a, v); }
static u32  r32(NDS& nds, u32 a) { return nds.bus.dma_read32(Cpu::ARM7, a); }
static void w8(NDS& nds, u32 a, u8 v) { nds.bus.dma_write8(Cpu::ARM7, a, v); }
static void w16(NDS& nds, u32 a, u16 v) { nds.bus.dma_write16(Cpu::ARM7, a, v); }
static u32 get32(const u8* p) { u32 v; std::memcpy(&v, p, 4); return v; }
static void put32(u8* p, u32 v) { std::memcpy(p, &v, 4); }
static void bswap128(u8* d, const u8* s) { for (int i = 0; i < 16; ++i) d[i] = s[15 - i]; }

enum : u32 { AES_CNT = 0x04004400, AES_BLKCNT = 0x04004404, AES_WRFIFO = 0x04004408, AES_RDFIFO = 0x0400440C, AES_IV = 0x04004420, AES_MAC = 0x04004430, AES_KEY0 = 0x04004440, IF2 = 0x0400021C };
enum : u32 { CNT_START = 1u << 31, CNT_IRQ = 1u << 30, CNT_MODE_SHIFT = 28, CNT_KEYSEL = 1u << 24, CNT_MAC_OK = 1u << 21, CNT_MACLEN_SHIFT = 16, CNT_DMA_OUT16 = 3u << 14, CNT_DMA_IN0 = 0 };

static void write_block(NDS& nds, u32 base, const u8* b) { for (int k = 0; k < 4; ++k) w32(nds, base + k * 4, get32(b + k * 4)); }
static void feed(NDS& nds, const u8* b) { for (int k = 0; k < 4; ++k) w32(nds, AES_WRFIFO, get32(b + k * 4)); }
static void drain(NDS& nds, u8* b) { for (int k = 0; k < 4; ++k) put32(b + k * 4, r32(nds, AES_RDFIFO)); }

static void test_engine_ctr(NDS& nds) {
  // Key slot 0 normal key = NIST key as the engine stores it (register order
  // is the mirror of the cipher's), IV likewise, one block of plaintext.
  u8 key_reg[16], iv_reg[16], pt_reg[16];
  static const u8 ctr_iv[16] = {0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff};
  static const u8 ctr[16] = {0x87, 0x4d, 0x61, 0x91, 0xb6, 0x20, 0xe3, 0x26, 0x1b, 0xef, 0x68, 0x64, 0x99, 0x0d, 0xb6, 0xce};
  bswap128(key_reg, NIST_KEY); bswap128(iv_reg, ctr_iv); bswap128(pt_reg, NIST_PT);
  write_block(nds, AES_KEY0, key_reg);
  write_block(nds, AES_IV, iv_reg);
  w32(nds, AES_BLKCNT, 1u << 16);
  w32(nds, AES_CNT, CNT_START | CNT_KEYSEL | (2u << CNT_MODE_SHIFT) | CNT_DMA_OUT16);
  CHECK_EQ(r32(nds, AES_CNT) & CNT_START, CNT_START);            // waiting for input
  feed(nds, pt_reg);
  CHECK_EQ(r32(nds, AES_CNT) & CNT_START, 0u);                    // one block: done on arrival
  CHECK_EQ((r32(nds, AES_CNT) >> 5) & 0x1F, 4u);                  // output FIFO level
  CHECK_EQ(r32(nds, AES_CNT) & 0x1F, 0u);                         // input FIFO drained
  u8 out_reg[16], out[16]; drain(nds, out_reg); bswap128(out, out_reg);
  CHECK_MEM(out, ctr, 16);
  CHECK_EQ((r32(nds, AES_CNT) >> 5) & 0x1F, 0u);
  // Narrow writes land byte- and halfword-masked in the IV.
  u8 zero_reg[16] = {}; write_block(nds, AES_IV, zero_reg);
  w32(nds, AES_IV, 0x11223344); w8(nds, AES_IV + 1, 0xAA); w16(nds, AES_IV + 2, 0xBBCC);
  w32(nds, AES_BLKCNT, 1u << 16);
  w32(nds, AES_CNT, CNT_START | CNT_KEYSEL | (2u << CNT_MODE_SHIFT) | CNT_DMA_OUT16);
  feed(nds, pt_reg); drain(nds, out_reg);
  AES_ctx ctx; u8 iv2[16] = {}; u8 iv2_reg[16] = {}; put32(iv2_reg, 0xBBCCAA44);
  bswap128(iv2, iv2_reg); AES_init_ctx_iv(&ctx, NIST_KEY, iv2);
  u8 ref[16]; std::memcpy(ref, NIST_PT, 16); AES_CTR_xcrypt_buffer(&ctx, ref, 16);
  bswap128(out, out_reg);
  CHECK_MEM(out, ref, 16);
}

static void test_engine_ccm(NDS& nds) {
  u8 key_reg[16], nonce_reg[16] = {};
  bswap128(key_reg, NIST_KEY);
  for (int i = 0; i < 12; ++i) nonce_reg[i] = static_cast<u8>(0xA0 + i);
  u8 adata[16], pt[2][16];
  for (int i = 0; i < 16; ++i) { adata[i] = static_cast<u8>(i * 7); pt[0][i] = static_cast<u8>(0x30 + i); pt[1][i] = static_cast<u8>(0xC0 - i); }
  write_block(nds, AES_KEY0, key_reg);
  write_block(nds, AES_IV, nonce_reg);
  // Encrypt: 1 associated block, 2 payload blocks, 16-byte MAC, IRQ on done.
  w32(nds, IF2, 0xFFFFFFFF);
  w32(nds, AES_BLKCNT, (2u << 16) | 1u);
  w32(nds, AES_CNT, CNT_START | CNT_IRQ | CNT_KEYSEL | (1u << CNT_MODE_SHIFT) | (4u << CNT_MACLEN_SHIFT) | CNT_DMA_OUT16);
  feed(nds, adata);
  CHECK_EQ((r32(nds, AES_CNT) >> 5) & 0x1F, 0u);                  // adata produces no output
  feed(nds, pt[0]); feed(nds, pt[1]);
  CHECK_EQ(r32(nds, AES_CNT) & CNT_START, 0u);
  CHECK_EQ((r32(nds, AES_CNT) >> 5) & 0x1F, 12u);                 // 2 blocks + the MAC
  CHECK_EQ((r32(nds, IF2) >> 12) & 1, 1u);                        // IRQ2_AES
  u8 ct[2][16], mac[16]; drain(nds, ct[0]); drain(nds, ct[1]); drain(nds, mac);
  CHECK_EQ(std::memcmp(ct[0], pt[0], 16) != 0, 1u);
  // Decrypt with the MAC the engine produced: plaintext back, MAC verified.
  write_block(nds, AES_MAC, mac);
  w32(nds, AES_BLKCNT, (2u << 16) | 1u);
  w32(nds, AES_CNT, CNT_START | CNT_KEYSEL | (0u << CNT_MODE_SHIFT) | (4u << CNT_MACLEN_SHIFT) | CNT_DMA_OUT16);
  feed(nds, adata); feed(nds, ct[0]); feed(nds, ct[1]);
  CHECK_EQ(r32(nds, AES_CNT) & (CNT_START | CNT_MAC_OK), CNT_MAC_OK);
  u8 back[2][16]; drain(nds, back[0]); drain(nds, back[1]);
  CHECK_MEM(back[0], pt[0], 16); CHECK_MEM(back[1], pt[1], 16);
  // A wrong MAC clears the check bit but still decrypts.
  mac[3] ^= 1;
  write_block(nds, AES_MAC, mac);
  w32(nds, AES_BLKCNT, (2u << 16) | 1u);
  w32(nds, AES_CNT, CNT_START | CNT_KEYSEL | (0u << CNT_MODE_SHIFT) | (4u << CNT_MACLEN_SHIFT) | CNT_DMA_OUT16);
  feed(nds, adata); feed(nds, ct[0]); feed(nds, ct[1]);
  CHECK_EQ(r32(nds, AES_CNT) & (CNT_START | CNT_MAC_OK), 0u);
  drain(nds, back[0]); drain(nds, back[1]);
  CHECK_MEM(back[0], pt[0], 16);
}

static void test_engine_keys(NDS& nds) {
  // Writing KeyY's last word derives the slot's normal key from the pair.
  u8 kx[16], ky[16], want[16], out_reg[16], out[16];
  for (int i = 0; i < 16; ++i) { kx[i] = static_cast<u8>(0x10 + i); ky[i] = static_cast<u8>(0x80 ^ i); }
  DsiAes::derive_normal_key(kx, ky, want);
  write_block(nds, AES_KEY0 + 0x30 + 0x10, kx);          // slot 1 KeyX
  write_block(nds, AES_KEY0 + 0x30 + 0x20, ky);          // slot 1 KeyY
  // Verify through a CTR block: the same block through the C API with `want`.
  u8 iv_reg[16] = {}, pt_reg[16]; bswap128(pt_reg, NIST_PT);
  write_block(nds, AES_IV, iv_reg);
  w32(nds, AES_BLKCNT, 1u << 16);
  w32(nds, AES_CNT, CNT_START | CNT_KEYSEL | (1u << 26) | (2u << CNT_MODE_SHIFT) | CNT_DMA_OUT16);
  feed(nds, pt_reg); drain(nds, out_reg); bswap128(out, out_reg);
  u8 key[16], iv[16] = {}; bswap128(key, want);
  AES_ctx ctx; AES_init_ctx_iv(&ctx, key, iv);
  u8 ref[16]; std::memcpy(ref, NIST_PT, 16); AES_CTR_xcrypt_buffer(&ctx, ref, 16);
  CHECK_MEM(out, ref, 16);
  // The ARM9 cannot see the engine.
  CHECK_EQ(nds.bus.dma_read32(Cpu::ARM9, AES_CNT), 0u);
}

int main() {
  test_primitives();
  AES_force_software(1);
  test_primitives();
  AES_force_software(0);
  test_hw_matches_software();
  test_scrambler();
  NDS nds;
  nds.set_dsi(true);
  nds.reset();
  test_engine_ctr(nds);
  test_engine_ccm(nds);
  test_engine_keys(nds);
  if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
  std::puts("aes: ok");
  return 0;
}
