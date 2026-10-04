// SPDX-License-Identifier: GPL-3.0-or-later
#include "dmaheap.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace ds::sdl::dmaheap {
namespace {

// linux/dma-buf.h, not shipped in the handhelds' sysroots; kernel UAPI, fixed.
struct dma_buf_sync { unsigned long long flags; };
constexpr unsigned long long kSyncWrite = 2ull;      // DMA_BUF_SYNC_WRITE
constexpr unsigned long long kSyncStart = 0ull << 2; // DMA_BUF_SYNC_START
constexpr unsigned long long kSyncEnd   = 1ull << 2; // DMA_BUF_SYNC_END
#ifndef DMA_BUF_IOCTL_SYNC
#define DMA_BUF_IOCTL_SYNC _IOW('b', 0, struct ds::sdl::dmaheap::dma_buf_sync)
#endif

bool g_no_sync = false;  // set once the ioctl is known unsupported

void sync(int fd, unsigned long long flags) {
  if (fd < 0 || g_no_sync) return;
  dma_buf_sync s{flags};
  while (ioctl(fd, DMA_BUF_IOCTL_SYNC, &s) != 0) {
    if (errno == EINTR || errno == EAGAIN) continue;
    // ENOTTY: no sync ioctl (legacy ION, uncached anyway); anything else is logged.
    if (errno != ENOTTY) std::fprintf(stderr, "dmabuf: sync failed (%s); frames may tear\n", std::strerror(errno));
    g_no_sync = true;
    return;
  }
}

} // namespace

void sync_begin_write(int fd) { sync(fd, kSyncStart | kSyncWrite); }
void sync_end_write(int fd) { sync(fd, kSyncEnd | kSyncWrite); }



namespace {

// From <linux/dma-heap.h>, declared here so the sysroot need not carry it.
struct dma_heap_allocation_data {
  uint64_t len;
  uint32_t fd;
  uint32_t fd_flags;
  uint64_t heap_flags;
};
#define DMA_HEAP_IOCTL_ALLOC _IOWR('H', 0x0, struct dma_heap_allocation_data)

// From <linux/ion.h>, both generations: new ABI returns an fd from
// ION_IOC_ALLOC, legacy returns a handle ION_IOC_SHARE turns into an fd.
// The ioctl numbers collide by design; struct sizes tell them apart.
struct ion_allocation_data_new {
  uint64_t len;
  uint32_t heap_id_mask;
  uint32_t flags;
  uint32_t fd;
  uint32_t unused;
};
struct ion_heap_data {
  char name[32];
  uint32_t type;
  uint32_t heap_id;
  uint32_t reserved0, reserved1, reserved2;
};
struct ion_heap_query {
  uint32_t cnt;
  uint32_t reserved0;
  uint64_t heaps;
  uint32_t reserved1;
  uint32_t reserved2;
};
struct ion_allocation_data_legacy {
  size_t len;                       // target's size_t: 32-bit ABI on arm32
  size_t align;
  unsigned int heap_id_mask;
  unsigned int flags;
  int handle;
};
struct ion_handle_data { int handle; };
struct ion_fd_data { int handle; int fd; };
#define ION_IOC_ALLOC_NEW    _IOWR('I', 0, struct ion_allocation_data_new)
#define ION_IOC_HEAP_QUERY   _IOWR('I', 8, struct ion_heap_query)
#define ION_IOC_ALLOC_LEGACY _IOWR('I', 0, struct ion_allocation_data_legacy)
#define ION_IOC_FREE         _IOWR('I', 1, struct ion_handle_data)
#define ION_IOC_SHARE        _IOWR('I', 4, struct ion_fd_data)
enum { ION_HEAP_TYPE_SYSTEM = 0, ION_HEAP_TYPE_SYSTEM_CONTIG = 1, ION_HEAP_TYPE_CARVEOUT = 2,
       ION_HEAP_TYPE_CHUNK = 3, ION_HEAP_TYPE_DMA = 4 };

// `ion_mask` == 0 means a dma-heap at `path`.
struct Source {
  std::string path;               // dma-heap device, or "ion:<name>" for the log
  uint32_t ion_mask = 0;
  bool swept = false;             // blind id-bit candidate: quiet on failure
};

std::string g_chosen;
Source g_pinned;
bool g_have_pinned = false;

int alloc_dmaheap(const std::string& path, size_t len) {
  int heap = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
  if (heap < 0) return -1;
  dma_heap_allocation_data a = {};
  a.len = len;
  a.fd_flags = O_RDWR | O_CLOEXEC;
  const int r = ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &a);
  ::close(heap);
  return r < 0 ? -1 : static_cast<int>(a.fd);
}

// flags 0: uncached where the heap has the choice (write-once, display-read
// buffer). ABI is detected by trying new-style alloc first: each ABI's ALLOC
// number encodes its struct size, so the wrong one is ENOTTY, never a misread.
int alloc_ion(const Source& s, size_t len) {
  int ion = ::open("/dev/ion", O_RDWR | O_CLOEXEC);
  if (ion < 0) return -1;
  int fd = -1;
  ion_allocation_data_new a = {};
  a.len = len;
  a.heap_id_mask = s.ion_mask;
  if (ioctl(ion, ION_IOC_ALLOC_NEW, &a) == 0) {
    fd = static_cast<int>(a.fd);
  } else if (errno == ENOTTY) {
    ion_allocation_data_legacy l = {};
    l.len = len;
    l.align = 4096;
    l.heap_id_mask = s.ion_mask;
    if (ioctl(ion, ION_IOC_ALLOC_LEGACY, &l) == 0) {
      ion_fd_data f = {};
      f.handle = l.handle;
      if (ioctl(ion, ION_IOC_SHARE, &f) == 0) fd = f.fd;
      ion_handle_data h = {l.handle};
      ioctl(ion, ION_IOC_FREE, &h);   // fd keeps the buffer alive
    }
  }
  const int err = errno;
  ::close(ion);
  errno = err;
  return fd;
}

int alloc_from(const Source& s, size_t len) {
  return s.ion_mask ? alloc_ion(s, len) : alloc_dmaheap(s.path, len);
}

// Rank a dma-heap by name: contiguous memory preferred, uncached over cached.
int heap_rank(const std::string& n) {
  const bool uncached = n.find("uncached") != std::string::npos;
  if (n == "linux,cma") return 0;                                  // mainline
  if (n.find("cma") != std::string::npos || n.find("contig") != std::string::npos) return uncached ? 1 : 2;
  if (n.find("reserved") != std::string::npos || n.find("carveout") != std::string::npos) return 3;
  if (n.find("system") != std::string::npos) return uncached ? 4 : 5;   // contiguous only behind an IOMMU
  return 6;
}

void dma_heaps(std::vector<Source>& out) {
  DIR* d = opendir("/dev/dma_heap");
  if (!d) return;
  std::vector<std::string> names;
  while (dirent* e = readdir(d)) if (e->d_name[0] != '.') names.emplace_back(e->d_name);
  closedir(d);
  std::stable_sort(names.begin(), names.end(),
                   [](const std::string& a, const std::string& b) { return heap_rank(a) < heap_rank(b); });
  for (const auto& n : names) out.push_back({"/dev/dma_heap/" + n});
}

int ion_type_rank(uint32_t t) {
  switch (t) {
    case ION_HEAP_TYPE_DMA: return 0;
    case ION_HEAP_TYPE_CARVEOUT: return 1;
    case ION_HEAP_TYPE_SYSTEM_CONTIG: return 2;
    case ION_HEAP_TYPE_CHUNK: return 3;
    case ION_HEAP_TYPE_SYSTEM: return 5;
    default: return 4;
  }
}

void ion_heaps(std::vector<Source>& out) {
  int ion = ::open("/dev/ion", O_RDWR | O_CLOEXEC);
  if (ion < 0) return;
  const size_t before = out.size();
  ion_heap_query q = {};
  const int qr = ioctl(ion, ION_IOC_HEAP_QUERY, &q);
  if (std::getenv("DS_DMA_HEAP_VERBOSE"))
    std::fprintf(stderr, "dmaheap: ion query %d (%s) cnt %u\n", qr, qr ? std::strerror(errno) : "ok", q.cnt);
  if (qr == 0 && q.cnt > 0 && q.cnt < 64) {
    std::vector<ion_heap_data> hs(q.cnt);
    q.heaps = reinterpret_cast<uint64_t>(hs.data());
    if (ioctl(ion, ION_IOC_HEAP_QUERY, &q) == 0) {
      std::stable_sort(hs.begin(), hs.end(),
                       [](const ion_heap_data& a, const ion_heap_data& b) { return ion_type_rank(a.type) < ion_type_rank(b.type); });
      for (const auto& h : hs) {
        std::string name(h.name, strnlen(h.name, sizeof h.name));
        out.push_back({"ion:" + name, 1u << (h.heap_id & 31), false});
      }
    } else if (std::getenv("DS_DMA_HEAP_VERBOSE")) {
      std::fprintf(stderr, "dmaheap: ion heap query (fill): %s\n", std::strerror(errno));
    }
  }
  ::close(ion);
  if (out.size() > before) return;
  // No usable query (legacy ION). Heap ids are per vendor, but common BSPs
  // number by type (DMA 4, carveout 2, system-contig 1, chunk 3, system 0);
  // try those first, then the rest of the bits; caller's import test decides.
  static const unsigned order[] = {4, 2, 1, 3, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
                                   17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 0};
  for (unsigned b : order) out.push_back({"ion:id" + std::to_string(b), 1u << b, true});
}

std::vector<Source> candidates() {
  std::vector<Source> v;
  if (const char* e = std::getenv("DS_DMA_HEAP"); e && *e) {
    if (!std::strcmp(e, "ion")) { ion_heaps(v); return v; }
    if (!std::strncmp(e, "ion:", 4)) {
      const uint32_t m = static_cast<uint32_t>(std::strtoul(e + 4, nullptr, 0));
      v.push_back({std::string("ion:") + (e + 4), m, false});
      return v;
    }
    v.push_back({e[0] == '/' ? std::string(e) : "/dev/dma_heap/" + std::string(e)});
    return v;
  }
  dma_heaps(v);
  ion_heaps(v);
  return v;
}

} // namespace

int alloc(size_t len, const std::function<bool(int fd)>& usable, const char* tag) {
  // DS_DMABUF_FAIL_AFTER=N: every allocation after the first N fails (tests the
  // fallbacks for a CMA pool that page cache or another client has used up).
  static const long fail_after = [] { const char* e = std::getenv("DS_DMABUF_FAIL_AFTER"); return e ? std::atol(e) : -1L; }();
  static long allocs = 0;
  if (fail_after >= 0 && allocs++ >= fail_after) { errno = ENOMEM; std::fprintf(stderr, "%s: alloc: %s (DS_DMABUF_FAIL_AFTER)\n", tag, std::strerror(errno)); return -1; }
  if (g_have_pinned) {
    const int fd = alloc_from(g_pinned, len);
    if (fd < 0) { std::fprintf(stderr, "%s: %s: alloc: %s\n", tag, g_chosen.c_str(), std::strerror(errno)); return -1; }
    if (usable(fd)) return fd;
    ::close(fd);
    std::fprintf(stderr, "%s: %s: buffer rejected\n", tag, g_chosen.c_str());
    return -1;
  }

  const std::vector<Source> cands = candidates();
  if (cands.empty()) {
    std::fprintf(stderr, "%s: no dmabuf allocator (no /dev/dma_heap/*, no /dev/ion)\n", tag);
    return -1;
  }
  for (const Source& s : cands) {
    const int fd = alloc_from(s, len);
    if (fd < 0) {
      // legacy-ION sweep: most of its 32 bits are not heaps, so stay quiet
      if (!s.swept || std::getenv("DS_DMA_HEAP_VERBOSE")) std::fprintf(stderr, "%s: %s: %s\n", tag, s.path.c_str(), std::strerror(errno));
      continue;
    }
    if (usable(fd)) {
      g_pinned = s;
      g_have_pinned = true;
      g_chosen = s.path;
      std::fprintf(stderr, "%s: buffers from %s\n", tag, g_chosen.c_str());
      return fd;
    }
    ::close(fd);
    std::fprintf(stderr, "%s: %s: allocates but the display rejects it\n", tag, s.path.c_str());
  }
  std::fprintf(stderr, "%s: no usable dmabuf source\n", tag);
  return -1;
}

const char* chosen() { return g_chosen.c_str(); }

} // namespace ds::sdl::dmaheap
