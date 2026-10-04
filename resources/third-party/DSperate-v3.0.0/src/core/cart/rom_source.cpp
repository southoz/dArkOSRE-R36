// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/cart/rom_source.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/vfs.h>
#endif

namespace ds::cart {

static constexpr u64 MAX_ROM = 512ull << 20;   // a DS card tops out at 512 MB

RomSource::~RomSource() {
  if (map_) munmap(map_, map_len_);
}

const u8* RomSource::ff_page() {
  static const u8* p = [] { static u8 page[PAGE]; std::memset(page, 0xFF, PAGE); return page; }();
  return p;
}

void RomSource::finish() {
  u32 m = PAGE; while (m < size_) m <<= 1;
  mask_ = m - 1;
  const u32 rem = size_ & (PAGE - 1);
  if (rem) {
    // Real bytes then 0xFF, so nothing reads the mapping past EOF.
    tail_.assign(PAGE, 0xFF);
    std::memcpy(tail_.data(), data_ + (size_ - rem), rem);
  }
}

std::unique_ptr<RomSource> RomSource::from_memory(std::vector<u8> bytes) {
  std::unique_ptr<RomSource> s(new RomSource);
  s->owned_ = std::move(bytes);
  s->data_ = s->owned_.data();
  s->size_ = static_cast<u32>(s->owned_.size());
  s->finish();
  return s;
}

namespace {
RomSource::Preload g_preload = RomSource::Preload::Auto;

// The filesystem behind `fd` is a network one (NFS, SMB/CIFS, 9p, AFS, Coda,
// or anything through FUSE, which is how sshfs and most share clients mount).
bool on_network_fs(int fd) {
#if defined(__linux__)
  struct statfs sf{};
  if (fstatfs(fd, &sf) != 0) return false;
  switch (static_cast<unsigned long>(sf.f_type)) {
    case 0x6969UL: case 0x517BUL: case 0xFF534D42UL: case 0xFE534D42UL: case 0x65735546UL:
    case 0x01021997UL: case 0x5346414FUL: case 0x73757245UL: case 0x564CUL:
      return true;
    default: return false;
  }
#else
  (void)fd; return false;
#endif
}

// MemAvailable, or 0 when unknown.
u64 mem_available() {
#if defined(__linux__)
  if (std::FILE* f = std::fopen("/proc/meminfo", "r")) {
    char line[128]; u64 kb = 0;
    while (std::fgets(line, sizeof line, f)) if (std::sscanf(line, "MemAvailable: %llu kB", reinterpret_cast<unsigned long long*>(&kb)) == 1) break;
    std::fclose(f);
    return kb << 10;
  }
#endif
  return 0;
}
} // namespace

void RomSource::set_preload(Preload p) { g_preload = p; }
RomSource::Preload RomSource::preload() { return g_preload; }

std::unique_ptr<RomSource> RomSource::map_file(const std::string& path, u64 offset, u64 size,
                                               std::string& err) {
  err.clear();
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) { err = std::strerror(errno); return nullptr; }
  struct stat st{};
  if (fstat(fd, &st) != 0) { err = std::strerror(errno); close(fd); return nullptr; }
  const u64 file = static_cast<u64>(st.st_size);
  if (offset > file) { err = "range starts past the end of the file"; close(fd); return nullptr; }
  if (size == 0) size = file - offset;
  if (size > file - offset) { err = "range runs past the end of the file"; close(fd); return nullptr; }
  if (size > MAX_ROM) { err = "larger than any DS card"; close(fd); return nullptr; }
  if (size == 0) { err = "empty"; close(fd); return nullptr; }

  const long ps = sysconf(_SC_PAGESIZE);
  const u64 align = static_cast<u64>(ps > 0 ? ps : 4096);
  const u64 base = offset & ~(align - 1);
  const size_t len = static_cast<size_t>(offset - base + size);
  // Read in whole now (cart.preload), or page by page as the game touches it.
  bool populate = g_preload == Preload::On;
  if (g_preload == Preload::Auto && on_network_fs(fd)) {
    const u64 avail = mem_available();
    populate = avail == 0 || avail >= len + (128ull << 20);
    if (!populate) std::fprintf(stderr, "rom: on a network filesystem, but %llu MB free is too little to preload %llu MB\n", (unsigned long long)(avail >> 20), (unsigned long long)(len >> 20));
  }
  int flags = MAP_PRIVATE;
#if defined(MAP_POPULATE)
  if (populate) flags |= MAP_POPULATE;
#endif
  void* m = mmap(nullptr, len, PROT_READ, flags, fd, static_cast<off_t>(base));
#if !defined(MAP_POPULATE)
  if (populate && m != MAP_FAILED) { volatile u8 sink = 0; for (size_t i = 0; i < len; i += 4096) sink += static_cast<const u8*>(m)[i]; (void)sink; }
#endif
  if (populate && m != MAP_FAILED) std::fprintf(stderr, "rom: preloaded %llu MB%s\n", (unsigned long long)((len + (1 << 20) - 1) >> 20), g_preload == Preload::Auto ? " (network filesystem)" : "");
  close(fd);
  if (m == MAP_FAILED) { err = std::string("mmap: ") + std::strerror(errno); return nullptr; }

  std::unique_ptr<RomSource> s(new RomSource);
  s->map_ = m; s->map_len_ = len;
  s->data_ = static_cast<const u8*>(m) + (offset - base);
  s->size_ = static_cast<u32>(size);
  s->finish();
  return s;
}

u8* RomSource::patch(u32 addr) {
  const u32 p = addr & mask_ & ~(PAGE - 1);
  for (Patch& o : overlay_) if (o.base == p) return o.bytes.data();
  Patch o; o.base = p; o.bytes.assign(page(p), page(p) + PAGE);
  overlay_.push_back(std::move(o));
  return overlay_.back().bytes.data();
}

void RomSource::read(u32 addr, u8* dst, u32 n) const {
  while (n) {
    const u8* pg = page(addr);
    const u32 off = addr & (PAGE - 1);
    const u32 take = n < PAGE - off ? n : PAGE - off;
    std::memcpy(dst, pg + off, take);
    dst += take; addr += take; n -= take;
  }
}

u32 RomSource::read_unpatched(u32 addr, u8* dst, u32 n) const {
  // Deliberately linear, not wrapping at the mask: a wrapping request counts
  // as unavailable rather than reading from the front of the image.
  const u32 start = addr & mask_;
  u32 have = 0;
  if (start < size_) have = n < size_ - start ? n : size_ - start;

  while (n) {
    const u8* pg = page_unpatched(addr & mask_ & ~(PAGE - 1));
    const u32 off = addr & (PAGE - 1);
    const u32 take = n < PAGE - off ? n : PAGE - off;
    std::memcpy(dst, pg + off, take);
    dst += take; addr += take; n -= take;
  }
  return have;
}

} // namespace ds::cart
