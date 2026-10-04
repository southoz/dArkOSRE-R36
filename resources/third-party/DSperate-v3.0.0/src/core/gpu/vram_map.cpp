// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/gpu/vram_map.h"

#include <cstring>

namespace ds::gpu {

constexpr u32 VramMap::BANK_MASK[9];

void VramMap::add(VramView& v, u32 base, u32 len, int bank) {
  // `base`/`len` in bytes within the view; caller passes each mirror separately.
  for (u32 off = 0; off < len; off += VramView::BLOCK) {
    const u32 b = ((base + off) & v.addr_mask()) / VramView::BLOCK;
    v.mask[b] |= static_cast<u16>(1u << bank);
  }
}

void VramMap::finish(VramView& v) {
  // A view smaller than a block still gets its direct pointer.
  const u32 nblocks = v.blocks() ? v.blocks() : 1;
  for (u32 b = 0; b < nblocks; ++b) {
    const u32 m = v.mask[b];
    v.ptr[b] = nullptr;
    if (m && !(m & (m - 1))) {
      const int bank = __builtin_ctz(m);
      v.ptr[b] = banks_[bank] + ((b * VramView::BLOCK) & BANK_MASK[bank]);
    }
  }
}

void VramMap::rebuild(const u8 vramcnt[9], u8* const banks[9]) {
  for (int i = 0; i < 9; ++i) banks_[i] = banks[i];
  auto clear = [](VramView& v, u32 size) { v.size = size; v.mask.fill(0); v.ptr.fill(nullptr); };
  clear(abg, 0x80000); clear(aobj, 0x40000); clear(bbg, 0x20000); clear(bobj, 0x20000);
  clear(abg_extpal, 0x8000); clear(bbg_extpal, 0x8000); clear(aobj_extpal, 0x2000); clear(bobj_extpal, 0x2000);
  clear(texture, 0x80000); clear(texpal, 0x20000); clear(arm7, 0x40000);
  lcdc_mask = 0;
  ++gen_;

  for (int i = 0; i < 9; ++i) {
    const u8 cnt = vramcnt[i];
    if (!(cnt & 0x80)) continue;
    const u32 mst = cnt & 7, ofs = (cnt >> 3) & 3;
    // Modes with no CPU mapping (see generation()); everything else counts as writable.
    const bool unmapped = (i <= 3 && (mst & (i <= 1 ? 3u : 7u)) == 3) || (i == 4 && (mst == 3 || mst == 4)) ||
                          ((i == 5 || i == 6) && mst >= 3 && mst <= 5) || (i == 7 && (mst & 3) == 2) || (i == 8 && (mst & 3) == 3);
    if (!unmapped) writable_gen_[i] = gen_;
    switch (i) {
    case 0: case 1:                                   // A, B: 128 K
      switch (mst & 3) {
      case 0: lcdc_mask |= 1u << i; break;
      case 1: add(abg, ofs * 0x20000, 0x20000, i); break;
      case 2: add(aobj, (ofs & 1) * 0x20000, 0x20000, i); break;
      case 3: add(texture, ofs * 0x20000, 0x20000, i); break;
      }
      break;
    case 2: case 3:                                   // C, D: 128 K
      switch (mst) {
      case 0: lcdc_mask |= 1u << i; break;
      case 1: add(abg, ofs * 0x20000, 0x20000, i); break;
      case 2: add(arm7, (ofs & 1) * 0x20000, 0x20000, i); break;
      case 3: add(texture, ofs * 0x20000, 0x20000, i); break;
      case 4: if (i == 2) add(bbg, 0, 0x20000, i); else add(bobj, 0, 0x20000, i); break;
      default: break;
      }
      break;
    case 4:                                           // E: 64 K
      switch (mst) {
      case 0: lcdc_mask |= 1u << i; break;
      case 1: add(abg, 0, 0x10000, i); break;
      case 2: add(aobj, 0, 0x10000, i); break;
      case 3: add(texpal, 0, 0x10000, i); break;
      case 4: add(abg_extpal, 0, 0x8000, i); break;
      default: break;
      }
      break;
    case 5: case 6: {                                 // F, G: 16 K
      const u32 o = (ofs & 1) * 0x4000 + (ofs & 2) * 0x8000;
      switch (mst) {
      case 0: lcdc_mask |= 1u << i; break;
      case 1: add(abg, o, 0x4000, i); add(abg, o + 0x8000, 0x4000, i); break;      // mirrors at +0x8000
      case 2: add(aobj, o, 0x4000, i); add(aobj, o + 0x8000, 0x4000, i); break;
      case 3: add(texpal, o, 0x4000, i); break;
      case 4: add(abg_extpal, (ofs & 1) * 0x4000, 0x4000, i); break;
      case 5: add(aobj_extpal, 0, 0x2000, i); break;
      default: break;
      }
      break;
    }
    case 7:                                           // H: 32 K
      switch (mst & 3) {
      case 0: lcdc_mask |= 1u << i; break;
      case 1: add(bbg, 0, 0x8000, i); add(bbg, 0x10000, 0x8000, i); break;        // mirrors every 64 K
      case 2: add(bbg_extpal, 0, 0x8000, i); break;
      default: break;
      }
      break;
    case 8:                                           // I: 16 K
      switch (mst & 3) {
      case 0: lcdc_mask |= 1u << i; break;
      case 1: add(bbg, 0x8000, 0x4000, i); add(bbg, 0xC000, 0x4000, i); add(bbg, 0x18000, 0x4000, i); add(bbg, 0x1C000, 0x4000, i); break;
      case 2: add(bobj, 0, 0x20000, i); break;                                    // mirrors every 16 K
      case 3: add(bobj_extpal, 0, 0x2000, i); break;
      }
      break;
    }
  }
  for (VramView* v : {&abg, &aobj, &bbg, &bobj, &abg_extpal, &bbg_extpal, &aobj_extpal, &bobj_extpal, &texture, &texpal, &arm7}) finish(*v);
}

u64 VramMap::block_signature(const VramView& v, u32 addr, u32 len, u32& banks) const {
  u64 h = 0xcbf29ce484222325ull;
  for (u32 off = 0; off < len; off += VramView::BLOCK - (( addr + off) & (VramView::BLOCK - 1))) {
    const u32 b = ((addr + off) & v.addr_mask()) / VramView::BLOCK;
    banks |= v.mask[b];
    h = (h ^ (static_cast<u64>(v.mask[b]) | (static_cast<u64>(b) << 16))) * 0x100000001b3ull;
  }
  return h;
}

template <typename T>
static inline T or_read(const VramMap& m, const VramView& v, u32 addr) {
  addr &= v.addr_mask();
  const u32 b = addr / VramView::BLOCK;
  if (const u8* p = v.ptr[b]) { T r; std::memcpy(&r, p + (addr & (VramView::BLOCK - 1)), sizeof(T)); return r; }
  T r = 0;
  u32 mask = v.mask[b];
  while (mask) {
    const int bank = __builtin_ctz(mask); mask &= mask - 1;
    T x; std::memcpy(&x, m.bank(bank) + (addr & VramMap::BANK_MASK[bank]), sizeof(T)); r |= x;
  }
  return r;
}

template <typename T>
static inline void all_write(const VramMap& m, const VramView& v, u32 addr, T val) {
  addr &= v.addr_mask();
  u32 mask = v.mask[addr / VramView::BLOCK];
  while (mask) {
    const int bank = __builtin_ctz(mask); mask &= mask - 1;
    std::memcpy(m.bank(bank) + (addr & VramMap::BANK_MASK[bank]), &val, sizeof(T));
  }
}

u8  VramMap::read8 (const VramView& v, u32 a) const { return or_read<u8>(*this, v, a); }
u16 VramMap::read16(const VramView& v, u32 a) const { return or_read<u16>(*this, v, a & ~1u); }
u32 VramMap::read32(const VramView& v, u32 a) const { return or_read<u32>(*this, v, a & ~3u); }
void VramMap::write8 (const VramView& v, u32 a, u8 val)  const { all_write<u8>(*this, v, a, val); }
void VramMap::write16(const VramView& v, u32 a, u16 val) const { all_write<u16>(*this, v, a & ~1u, val); }
void VramMap::write32(const VramView& v, u32 a, u32 val) const { all_write<u32>(*this, v, a & ~3u, val); }

} // namespace ds::gpu
