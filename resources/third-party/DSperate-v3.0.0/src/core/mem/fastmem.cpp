// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/mem/fastmem.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(__linux__)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/vfs.h>
#include <unistd.h>
#endif

namespace ds::mem {

// Default on where the JIT reads views (AArch64/ARMv7 with JIT); DS_FASTMEM
// overrides.
bool fastmem_requested() {
#if (defined(__aarch64__) || defined(__arm__)) && DSPERATE_JIT
  constexpr bool kDefault = true;
#else
  constexpr bool kDefault = false;
#endif
  static const bool on = [] { const char* e = std::getenv("DS_FASTMEM"); return e ? std::atoi(e) != 0 : kDefault; }();
  return on;
}

#if defined(__linux__)

namespace {

constexpr long TMPFS_MAGIC_ = 0x01021994;

// By syscall number: glibc < 2.27 has no wrapper; kernel < 3.17 has no call.
int try_memfd() {
#if defined(SYS_memfd_create)
  return static_cast<int>(syscall(SYS_memfd_create, "dsperate-guest", 1u /* MFD_CLOEXEC */));
#else
  return -1;
#endif
}

// An unlinked file on a tmpfs, as fallback for no memfd. Disk-backed
// directories are refused (MAP_SHARED would write guest RAM to storage), as
// is a tmpfs without room for the whole object (else the sparse file
// SIGBUSes on the first page past its limit).
int try_tmpfs(size_t bytes, const char** where) {
  static const char* const dirs[] = {"/dev/shm", "/tmp/shm", "/tmp"};
  for (const char* d : dirs) {
    struct statfs st {};
    if (statfs(d, &st) != 0 || static_cast<long>(st.f_type) != TMPFS_MAGIC_) continue;
    if (static_cast<u64>(st.f_bavail) * static_cast<u64>(st.f_bsize) < static_cast<u64>(bytes) * 2) continue;   // room, with slack for others
    char path[64];
    std::snprintf(path, sizeof path, "%s/dsperate-guest-XXXXXX", d);
    const int fd = mkostemp(path, O_CLOEXEC);
    if (fd < 0) continue;
    unlink(path);
    *where = d;
    return fd;
  }
  return -1;
}

} // namespace

std::unique_ptr<HostArena> HostArena::create(size_t bytes) {
  const size_t size = (bytes + GuestView::HOST_PAGE - 1) & ~size_t{GuestView::HOST_PAGE - 1};
  std::unique_ptr<HostArena> a(new HostArena);
  const char* where = nullptr;
  a->fd_ = try_memfd();
  if (a->fd_ >= 0) a->kind_ = "memfd";
  else if ((a->fd_ = try_tmpfs(size, &where)) >= 0) a->kind_ = where;
  else return nullptr;   // no arena, no views: caller falls back
  if (ftruncate(a->fd_, static_cast<off_t>(size)) != 0) return nullptr;
  void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, a->fd_, 0);
  if (p == MAP_FAILED) return nullptr;
  a->base_ = static_cast<u8*>(p);
  a->size_ = size;
  return a;
}

HostArena::~HostArena() {
  if (base_) munmap(base_, size_);
  if (fd_ >= 0) close(fd_);
}

u8* HostArena::take(size_t bytes) {
  const size_t n = (bytes + GuestView::HOST_PAGE - 1) & ~size_t{GuestView::HOST_PAGE - 1};
  if (used_ + n > size_) return nullptr;
  u8* p = base_ + used_;
  used_ += n;
  return p;   // a fresh tmpfs page reads as zero
}

std::unique_ptr<GuestView> GuestView::create(const HostArena& arena, const PageTable& table) {
  std::unique_ptr<GuestView> v(new GuestView(arena, table));
  void* p = mmap(nullptr, static_cast<size_t>(RESERVE), PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (p == MAP_FAILED) return nullptr;
  v->base_ = static_cast<u8*>(p);
  const u32 pages = SPAN / HOST_PAGE;
  v->dirty_.assign(pages, 0);
  v->laid_.assign(pages, Laid{});
  v->moved_.assign(pages, 0);
  v->pending_.reserve(pages);
  return v;   // nothing laid: fault() grants pages as they are touched
}

GuestView::~GuestView() {
  if (base_) munmap(base_, static_cast<size_t>(RESERVE));
}

// What view page `v` should be: both 2 KB halves backed by the arena at
// consecutive offsets starting on a 4 KB boundary, outside VRAM.
GuestView::Laid GuestView::desired(u32 v) const {
  const u32 a0 = v * HOST_PAGE;
  if ((a0 >> 24) == 0x06) return {};   // VRAM: traps toggle every frame
  u32 off[2];
  bool writable = true;
  for (int h = 0; h < 2; ++h) {
    const u32 a = a0 + static_cast<u32>(h) * PAGE_SIZE;
    const Entry e = table_.entry(a);
    const uintptr_t base = e << 2;
    if (!base) return {};
    const u8* host = reinterpret_cast<const u8*>(base + a);
    if (!arena_.contains(host) || !arena_.contains(host + PAGE_SIZE - 1)) return {};
    off[h] = static_cast<u32>(host - arena_.base());
    if (e & (TAG_CODE | TAG_SPECIAL)) writable = false;
  }
  if ((off[0] & (HOST_PAGE - 1)) != 0 || off[1] != off[0] + PAGE_SIZE) return {};
  return {off[0] + 1, static_cast<u8>(writable)};
}

bool GuestView::lay_run(u32 v0, u32 n, const Laid& first) {
  void* const addr = base_ + size_t{v0} * HOST_PAGE;
  const size_t len = size_t{n} * HOST_PAGE;
  void* r;
  if (!first.off_plus1)
    r = mmap(addr, len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
  else
    r = mmap(addr, len, PROT_READ | (first.writable ? PROT_WRITE : 0), MAP_SHARED | MAP_FIXED, arena_.fd(),
             static_cast<off_t>(first.off_plus1 - 1));
  ++stats_.map_calls;
  stats_.pages_laid += n;
  if (r == MAP_FAILED) {
    std::fprintf(stderr, "fastmem: mmap of view pages %05x+%u failed\n", v0, n);
    return false;
  }
  return true;
}

bool GuestView::protect_run(u32 v0, u32 n, bool writable) {
  ++stats_.map_calls;
  stats_.pages_laid += n;
  if (mprotect(base_ + size_t{v0} * HOST_PAGE, size_t{n} * HOST_PAGE, PROT_READ | (writable ? PROT_WRITE : 0)) != 0) {
    std::fprintf(stderr, "fastmem: mprotect of view pages %05x+%u failed\n", v0, n);
    return false;
  }
  return true;
}

static inline bool same_backing(u32 a_plus1, u32 b_plus1) { return a_plus1 && a_plus1 == b_plus1; }

bool GuestView::flush() {
  if (pending_.empty()) return true;
  ++stats_.flushes;
  const auto t0 = std::chrono::steady_clock::now();
  std::sort(pending_.begin(), pending_.end());
  bool ok = true;
  // Coalesces consecutive pages: take away (backing moved / table stopped
  // serving), or drop rw to ro (same backing: mprotect).
  size_t i = 0;
  while (i < pending_.size()) {
    const u32 v0 = pending_[i];
    dirty_[v0] = 0;
    const Laid l0 = laid_[v0];
    if (!l0.off_plus1) { ++i; continue; }
    const Laid d0 = desired(v0);
    const bool keep0 = same_backing(l0.off_plus1, d0.off_plus1);
    if (keep0 && (d0.writable || !l0.writable)) { ++i; continue; }   // nothing to take away
    u32 n = 1;
    laid_[v0] = keep0 ? Laid{l0.off_plus1, 0} : Laid{};
    if (!keep0 && moved_[v0] < 255) ++moved_[v0];
    while (i + n < pending_.size() && pending_[i + n] == v0 + n) {
      const u32 v = v0 + n;
      const Laid l = laid_[v], d = desired(v);
      if (!l.off_plus1) break;
      const bool keep = same_backing(l.off_plus1, d.off_plus1);
      if (keep != keep0 || (keep && (d.writable || !l.writable))) break;
      dirty_[v] = 0;
      laid_[v] = keep ? Laid{l.off_plus1, 0} : Laid{};
      if (!keep && moved_[v] < 255) ++moved_[v];
      ++n;
    }
    ok &= keep0 ? protect_run(v0, n, false) : lay_run(v0, n, Laid{});
    i += n;
  }
  pending_.clear();
  stats_.flush_ns += static_cast<u64>((std::chrono::steady_clock::now() - t0).count());
  return ok;
}

bool GuestView::fault(uintptr_t addr) {
  if (!contains(addr)) return false;
  const uintptr_t a = addr - reinterpret_cast<uintptr_t>(base_);
  if (a >= SPAN) return false;
  const u32 v0 = static_cast<u32>(a / HOST_PAGE);
  const Laid d0 = desired(v0), l0 = laid_[v0];
  if (!d0.off_plus1 || (same_backing(l0.off_plus1, d0.off_plus1) && l0.writable >= d0.writable)) return false;   // the table refuses too
  if (moved_[v0] >= kVolatile) { ++stats_.volatile_refusals; return false; }
  // Grant a run of consecutive pages too (one mmap, not one fault each).
  constexpr u32 kRun = 256;
  u32 n = 1;
  while (n < kRun && v0 + n < SPAN / HOST_PAGE) {
    const Laid d = desired(v0 + n), l = laid_[v0 + n];
    if (d.writable != d0.writable || d.off_plus1 != d0.off_plus1 + n * HOST_PAGE || l == d || moved_[v0 + n] >= kVolatile) break;
    ++n;
  }
  if (!lay_run(v0, n, d0)) return false;
  for (u32 k = 0; k < n; ++k) laid_[v0 + k] = Laid{d0.off_plus1 + k * HOST_PAGE, d0.writable};
  ++stats_.grants;
  return true;
}

bool GuestView::verify(std::string* why) {
  if (!flush()) { *why = "flush failed"; return false; }
  char buf[160];
  for (u32 v = 0; v < SPAN / HOST_PAGE; ++v) {
    const Laid d = desired(v), l = laid_[v];
    if (!l.off_plus1) continue;
    if (l.off_plus1 != d.off_plus1 || (l.writable && !d.writable)) {
      std::snprintf(buf, sizeof buf, "view page %08x laid off %x w%d, but the table allows only off %x w%d", v * HOST_PAGE, l.off_plus1, l.writable, d.off_plus1, d.writable);
      *why = buf;
      return false;
    }
    for (int h = 0; h < 2; ++h) {
      const u32 a = v * HOST_PAGE + static_cast<u32>(h) * PAGE_SIZE;
      const u8* t = table_.read_ptr(a);
      bool code = false;
      const PageTable& tt = table_;
      const u8* w = tt.write_ptr(a, &code);
      if (t != arena_.base() + (l.off_plus1 - 1) + static_cast<u32>(h) * PAGE_SIZE || (l.writable && (!w || code))) {
        std::snprintf(buf, sizeof buf, "guest %08x: view serves %s, table read %p write %p code %d", a, l.writable ? "rw" : "r", static_cast<void*>(const_cast<u8*>(t)),
                      static_cast<void*>(const_cast<u8*>(w)), code ? 1 : 0);
        *why = buf;
        return false;
      }
      if (std::memcmp(base_ + a, t, PAGE_SIZE) != 0) {
        std::snprintf(buf, sizeof buf, "guest %08x: bytes through the view differ from the table's", a);
        *why = buf;
        return false;
      }
    }
  }
  return true;
}

#else   // not Linux: no arena, no views

std::unique_ptr<HostArena> HostArena::create(size_t) { return nullptr; }
HostArena::~HostArena() = default;
u8* HostArena::take(size_t) { return nullptr; }
std::unique_ptr<GuestView> GuestView::create(const HostArena&, const PageTable&) { return nullptr; }
GuestView::~GuestView() = default;
GuestView::Laid GuestView::desired(u32) const { return {}; }
bool GuestView::lay_run(u32, u32, const Laid&) { return false; }
bool GuestView::flush() { return true; }
bool GuestView::fault(uintptr_t) { return false; }
bool GuestView::protect_run(u32, u32, bool) { return false; }
bool GuestView::verify(std::string*) { return true; }

#endif

} // namespace ds::mem
