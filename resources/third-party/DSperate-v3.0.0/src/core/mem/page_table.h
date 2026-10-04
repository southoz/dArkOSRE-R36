// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include <cstring>
#include <new>
#include <memory>
#include <cstdint>
#include <cstdlib>
#include "core/types.h"
#include "core/profile.h"

namespace ds::mem {

// The single memory abstraction shared by the interpreter, JIT and DMA: one
// flat table of tagged entries, one per 2 KB guest page, covering the whole
// 32-bit guest address space (no masking before lookup). Each entry stores a
// *pre-biased* host base so `host_base + guest_addr` addresses the byte
// directly; low bits hold base >> 2, top two bits are tags:
//   top bit    CODE     page has translated code -> stores must check for SMC
//   next bit   SPECIAL  not plain RAM for writes -> MMIO / ROM / write-protect
// `entry << 2` discards both tags at once, so a CODE-page load costs nothing
// extra. Host pointers must be 4-byte aligned and the biased value must fit
// in the remaining bits; both asserted at map time.

// SMC notification: every store landing on a CODE page reports the host
// bytes it wrote. The recompiler installs the hook; without one it's a null
// check.
extern void (*code_write_hook)(u8* host, u32 len);
inline void code_written(u8* host, u32 len) { if (code_write_hook) code_write_hook(host, len); }
// Silent-store elimination: a value identical to what's already there is not
// reported (whether a block actually covers the bytes is the hook's business).
struct CodeStoreStats { u64 silent = 0, changed = 0; };
extern CodeStoreStats code_store_stats;   // census builds only; reported by DS_JIT_CHURN
inline void store_code(u8* host, const void* v, u32 len) {
  if (std::memcmp(host, v, len) == 0) { if (prof::census) ++code_store_stats.silent; return; }
  std::memcpy(host, v, len);
  if (prof::census) ++code_store_stats.changed;
  code_written(host, len);
}

constexpr u32 PAGE_SHIFT = 11;
constexpr u32 PAGE_SIZE  = 1u << PAGE_SHIFT;      // 2 KB
constexpr u32 PAGE_COUNT = 1u << (32 - PAGE_SHIFT); // 2 Mi entries = 16 MiB of table

// Host backing for guest memory is PAGE_SIZE-aligned so a guest page and its
// host page are the same window; the recompiler's code tracking (keyed by
// host_page_of(pointer)) depends on that, or stores into stale code go
// unreported.
// `owned` false: bytes belong to a HostArena (fastmem.h), freed all at once.
struct PageBufFree { bool owned = true; void operator()(u8* p) const { if (owned) std::free(p); } };
using PageBuf = std::unique_ptr<u8[], PageBufFree>;
inline PageBuf alloc_page_buf(size_t bytes) {
  const size_t n = (bytes + PAGE_SIZE - 1) & ~size_t{PAGE_SIZE - 1};
  u8* p = static_cast<u8*>(std::aligned_alloc(PAGE_SIZE, n));
  if (!p) throw std::bad_alloc();
  std::memset(p, 0, n);
  return PageBuf(p);
}

using Entry = uintptr_t;
class GuestView;

constexpr Entry TAG_CODE    = Entry{1} << (sizeof(Entry) * 8 - 1);
constexpr Entry TAG_SPECIAL = Entry{1} << (sizeof(Entry) * 8 - 2);
constexpr Entry BASE_MASK   = ~(TAG_CODE | TAG_SPECIAL);

enum PageFlags : u32 {
  PAGE_READABLE = 1u << 0,   // direct loads allowed
  PAGE_WRITABLE = 1u << 1,   // direct stores allowed
  PAGE_MMIO     = 1u << 2,   // no host backing at all
};

class PageTable {
public:
  PageTable();
  ~PageTable();
  PageTable(const PageTable&) = delete;
  PageTable& operator=(const PageTable&) = delete;

  // `host` may alias another mapping (mirrors, shared WRAM views). `size`
  // and `guest` must be PAGE_SIZE aligned.
  void map(u32 guest, u32 size, u8* host, u32 flags);
  void map_mmio(u32 guest, u32 size);
  void unmap(u32 guest, u32 size);
  // Sets the region to `hosts` (one pointer per page, nullptr = unmapped),
  // touching only entries that change.
  void remap(u32 guest, u32 size, u8* const* hosts, u32 flags);

  void set_code(u32 guest, u32 size, bool is_code);

  // Stores to every mapped page in [guest, guest+size) take the slow path
  // until lifted (loads stay direct). Region must hold no read-only/MMIO
  // pages: their SPECIAL bit means something else and lifting would clear it.
  void set_write_trap(u32 guest, u32 size, bool on);
  // As above, over pages whose bit is set in `bits` (bit i = first_page + i).
  void set_write_trap_bits(u32 first_page, u32 count, const u64* bits, bool on);

  // Tags every entry in the low 256 MB mapping the given 2 KB host page.
  // Code pages are tracked by host address so the other CPU's view and DMA
  // see the same tag; `map` re-applies tags through `code_query`.
  void set_code_host(const u8* host_page, bool is_code);
  static bool (*code_query)(const u8* host_page);

  // Return nullptr when the access must take the slow path.
  inline u8* read_ptr(u32 addr) const {
    Entry e = table_[addr >> PAGE_SHIFT];
    const Entry base = e << 2;
    return base ? reinterpret_cast<u8*>(base + addr) : nullptr;
  }
  inline u8* write_ptr(u32 addr, bool* is_code) const {
    if (prof::census && prof::enabled) {   // DSPERATE_CENSUS
      const u32 r = addr >> 24;
      if (r == 5) prof::add(prof::C_W_PALETTE, 1);
      else if (r == 7) prof::add(prof::C_W_OAM, 1);
    }
    Entry e = table_[addr >> PAGE_SHIFT];
    if (e & TAG_SPECIAL) return nullptr;
    *is_code = (e & TAG_CODE) != 0;
    const Entry base = e << 2;
    return base ? reinterpret_cast<u8*>(base + addr) : nullptr;
  }

  // A write-trapped page's backing store (nullptr if it has none), for a caller that
  // has already satisfied the trap.
  inline u8* trapped_write_ptr(u32 addr, bool* is_code) const {
    const Entry e = table_[addr >> PAGE_SHIFT];
    *is_code = (e & TAG_CODE) != 0;
    const Entry base = e << 2;
    return base ? reinterpret_cast<u8*>(base + addr) : nullptr;
  }

  // DS_FASTMEM: host view derived from this table. Every entry written from
  // here on is reported to it; nullptr detaches.
  void attach_view(GuestView* v);

  Entry  entry(u32 addr) const { return table_[addr >> PAGE_SHIFT]; }
  Entry* raw()                 { return table_; }
  const Entry* raw() const     { return table_; }

private:
  Entry* table_;   // PAGE_COUNT entries, mmap'd: untouched pages cost no RSS
  GuestView* view_ = nullptr;
  void note_view(u32 page);
  void flush_view();
  // Reverse index for set_code_host: host page -> guest pages (low 256 MB)
  // mapping it. Open-addressing table, intrusive lists threaded through
  // `next`; no allocation on the remap path.
  struct HostIndex;
  HostIndex* index_;
  void index_insert(u32 guest_page, Entry e);
  void index_remove(u32 guest_page, Entry e);
};

} // namespace ds::mem
