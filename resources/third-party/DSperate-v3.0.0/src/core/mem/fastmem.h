// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Guest memory mapped into host address space (DS_FASTMEM=1). The page table
// (page_table.h) is still the source of truth for the interpreter/DMA/slow
// paths; this adds a per-CPU host range `view` for translated code, where
// `view + guest_address` is the guest byte -- one host instruction per
// load/store, faulting back to the table for anything it wouldn't serve
// directly (I/O, VRAM, unmapped, code or trapped pages).
//
// HostArena: one shared-memory object holding every guest buffer (memfd, or
// an unlinked tmpfs file; without either there's no arena/views and buffers
// come from aligned_alloc as before).
//
// GuestView: a PROT_NONE reservation per CPU. A 4 KB view page holds two 2 KB
// guest pages and is mapped only when both halves are backed by the arena
// contiguously (rw if both are plain RAM, ro if both are readable, else
// unmapped). Invariant is one-sided: the view never serves what the table
// wouldn't, but may refuse what the table would allow.
//
// Laid lazily, map-on-fault: flush() applies only restrictions (unmap, or rw
// -> ro) reported by the table, which can't wait; fault() grants everything
// else the table allows.
#pragma once
#include "core/types.h"
#include "core/mem/page_table.h"

#include <memory>
#include <string>
#include <vector>

namespace ds::mem {

// DS_FASTMEM: 1 = views are built (the arena and the table hook), 0 = off;
// unset = on where the JIT uses them (AArch64, ARMv7), off elsewhere.
bool fastmem_requested();

class HostArena {
public:
  // nullptr when no backing object can be made (the caller falls back).
  static std::unique_ptr<HostArena> create(size_t bytes);
  ~HostArena();
  HostArena(const HostArena&) = delete;
  HostArena& operator=(const HostArena&) = delete;

  // A zeroed slice, 4 KB-aligned in the object (and so PAGE_SIZE-aligned in
  // memory, which the JIT's code tracking needs). nullptr when full.
  u8* take(size_t bytes);
  int fd() const { return fd_; }
  u8* base() const { return base_; }
  size_t size() const { return size_; }
  const char* kind() const { return kind_; }
  bool contains(const u8* p) const { return p >= base_ && p < base_ + size_; }

private:
  HostArena() = default;
  int fd_ = -1;
  u8* base_ = nullptr;
  size_t size_ = 0, used_ = 0;
  const char* kind_ = "";
};

class GuestView {
public:
  static constexpr u32 HOST_PAGE = 4096;
#if UINTPTR_MAX > 0xFFFFFFFFu
  static constexpr u64 RESERVE = u64{1} << 32;    // whole guest space
  static constexpr u32 SPAN    = 0x10000000u;     // DS memory lives below 256 MB
#else
  static constexpr u64 RESERVE = u64{64} << 20;   // 0x00000000-0x03FFFFFF
  static constexpr u32 SPAN    = 0x04000000u;
#endif

  static std::unique_ptr<GuestView> create(const HostArena& arena, const PageTable& table);
  ~GuestView();
  GuestView(const GuestView&) = delete;
  GuestView& operator=(const GuestView&) = delete;

  u8* base() const { return base_; }
  // The table wrote the entry of 2 KB guest page `guest_page`.
  void note(u32 guest_page) {
    const u32 v = guest_page >> 1;
    // Skip VRAM (never laid, toggles hundreds of times/frame) and pages with
    // nothing laid (nothing to take away).
    if (v >= SPAN / HOST_PAGE || (v >> 12) == 0x6 || dirty_[v] || !laid_[v].off_plus1) return;
    dirty_[v] = 1;
    pending_.push_back(v);
  }
  bool pending() const { return !pending_.empty(); }
  bool flush();
  // Refused access at host `addr`, from the fault handler (async-signal-safe:
  // table reads and mmap only). True if the table allows more than laid;
  // grants the page (and a following run) and the access can be retried.
  bool fault(uintptr_t addr);
  bool contains(uintptr_t addr) const { return addr >= reinterpret_cast<uintptr_t>(base_) && addr - reinterpret_cast<uintptr_t>(base_) < RESERVE; }
  // DS_FASTMEM_VERIFY: checks nothing laid exceeds the table, and mapped
  // bytes match. Flushes first.
  bool verify(std::string* why);

  struct Stats { u64 flushes = 0, map_calls = 0, pages_laid = 0, flush_ns = 0, grants = 0, volatile_refusals = 0; };
  const Stats& stats() const { return stats_; }

private:
  GuestView(const HostArena& a, const PageTable& t) : arena_(a), table_(t) {}
  struct Laid { u32 off_plus1 = 0; u8 writable = 0; bool operator==(const Laid& o) const { return off_plus1 == o.off_plus1 && writable == o.writable; } };
  Laid desired(u32 v) const;
  bool lay_run(u32 v0, u32 n, const Laid& first);
  bool protect_run(u32 v0, u32 n, bool writable);

  const HostArena& arena_;
  const PageTable& table_;
  u8* base_ = nullptr;
  std::vector<u8> dirty_;
  std::vector<u32> pending_;
  std::vector<Laid> laid_;
  // Times each page lost its backing while laid; past kVolatile it's no
  // longer granted (a page that keeps moving costs more to remap than walk).
  static constexpr u8 kVolatile = 3;
  std::vector<u8> moved_;
  Stats stats_;
};

} // namespace ds::mem
