// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Where a cart's ROM bytes come from: mapped (read-only mmap, demand-paged,
// reclaimable) or owned (a vector, for the built-in loader cart, tests, and
// inflated zips). Indistinguishable from the cart's side.
//
//   * Reads past `size()` return 0xFF, matching a real card's wraparound. The
//     partial last page of an unpadded file is served from a padded copy,
//     since mmap zero-fills past EOF but a fault beyond the last file page is
//     SIGBUS.
//   * The secure-area re-encryption at construction writes 0x800 bytes; a
//     mapped source is PROT_READ, so `patch()` copies the page into an
//     overlay that `page()` serves first.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "core/types.h"

namespace ds::cart {

class RomSource {
public:
  static constexpr u32 PAGE = 0x1000;

  // cart.preload: whether a mapped image is read in whole at load (the map
  // populated: pages resident up front, still reclaimable) instead of on
  // first touch. Auto does it for a file on a network filesystem that fits
  // in available memory with room to spare -- there a demand fault is a
  // round trip mid-frame -- and leaves local storage demand-paged, where a
  // populate measured as a loss (a hitch at frame 0 for nothing gained).
  enum class Preload : u8 { Auto, On, Off };
  static void set_preload(Preload p);
  static Preload preload();

  ~RomSource();
  RomSource(const RomSource&) = delete;
  RomSource& operator=(const RomSource&) = delete;

  static std::unique_ptr<RomSource> from_memory(std::vector<u8> bytes);
  // Maps `size` bytes of `path` at `offset` (0, 0 = the whole file). Offset
  // need not be page-aligned. Null with a reason in `err` on failure.
  static std::unique_ptr<RomSource> map_file(const std::string& path, u64 offset, u64 size,
                                             std::string& err);
  static std::unique_ptr<RomSource> map_file(const std::string& path, std::string& err) {
    return map_file(path, 0, 0, err);
  }

  u32 size() const { return size_; }          // bytes in the image
  u32 mask() const { return mask_; }          // padded power-of-two size minus one
  bool mapped() const { return map_ != nullptr; }

  // The 4 KB page holding `addr` (masked). Never null: past the image it is
  // the 0xFF page. Pointer is valid for the life of the source.
  const u8* page(u32 addr) const {
    const u32 p = addr & mask_ & ~(PAGE - 1);
    if (!overlay_.empty()) {
      for (const Patch& o : overlay_) if (o.base == p) return o.bytes.data();
    }
    return page_unpatched(p);
  }
  // A writable copy of the page holding `addr`, served by page() from then
  // on. For the secure-area rewrite only.
  u8* patch(u32 addr);

  // Small bounded reads for construction and direct boot. Beyond the image
  // the bytes are 0xFF.
  void read(u32 addr, u8* dst, u32 n) const;
  // Like read(), but bypassing any patch() overlay -- needed for the
  // RetroAchievements hash, which must cover the file's own secure-area bytes,
  // not Cart's re-encrypted copy. Returns the count actually within the
  // image (rest filled with 0xFF); rcheevos needs the short count to
  // distinguish a real short read from 0xFF padding.
  u32 read_unpatched(u32 addr, u8* dst, u32 n) const;
  u32 read32(u32 addr) const { u8 b[4]; read(addr, b, 4); return static_cast<u32>(b[0]) | (b[1] << 8) | (b[2] << 16) | (static_cast<u32>(b[3]) << 24); }

private:
  RomSource() = default;
  void finish();   // mask_, tail_ from data_/size_
  static const u8* ff_page();
  // `p` already masked and page-aligned; overlay not consulted.
  const u8* page_unpatched(u32 p) const {
    if (p + PAGE <= size_) return data_ + p;
    if (p < size_) return tail_.data();
    return ff_page();
  }

  struct Patch { u32 base; std::vector<u8> bytes; };
  const u8* data_ = nullptr;
  u32 size_ = 0, mask_ = 0;
  std::vector<u8> owned_;
  std::vector<u8> tail_;
  std::vector<Patch> overlay_;
  void* map_ = nullptr; size_t map_len_ = 0;
};

} // namespace ds::cart
