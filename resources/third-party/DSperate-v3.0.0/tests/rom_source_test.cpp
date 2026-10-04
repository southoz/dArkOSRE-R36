// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// RomSource: a mapped file and the same bytes in memory must be
// indistinguishable, including where the card's padding shows -- past the
// image, and across the partial last page mmap would zero-fill.
#include "core/cart/rom_source.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

using namespace ds;
using cart::RomSource;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static std::vector<u8> pattern(size_t n, u32 seed) {
  std::vector<u8> v(n);
  u32 x = seed;
  for (size_t i = 0; i < n; ++i) { x = x * 1664525u + 1013904223u; v[i] = static_cast<u8>(x >> 24); }
  return v;
}

static std::string write_temp(const std::vector<u8>& bytes) {
  char name[] = "/tmp/dsperate-romsrc-XXXXXX";
  const int fd = mkstemp(name);
  CHECK(fd >= 0);
  CHECK(write(fd, bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size()));
  close(fd);
  return name;
}

// Every read the cart makes, both ways, compared.
static void compare(const RomSource& a, const RomSource& b, const char* what) {
  CHECK(a.size() == b.size());
  CHECK(a.mask() == b.mask());
  for (u32 p = 0; p <= a.mask(); p += RomSource::PAGE) {
    if (std::memcmp(a.page(p), b.page(p), RomSource::PAGE) != 0) { std::fprintf(stderr, "%s: page %x differs\n", what, p); ++failures; return; }
  }
  // Wrap: an address above the mask lands on the same page.
  CHECK(a.page(a.mask() + 1 + 0x123) == a.page(0x123) || std::memcmp(a.page(a.mask() + 1 + 0x123), a.page(0x123), RomSource::PAGE) == 0);
  u8 x[9], y[9];
  a.read(a.size() - 5, x, 9); b.read(b.size() - 5, y, 9);   // straddles the end
  CHECK(std::memcmp(x, y, 9) == 0);
  CHECK(a.read32(0x10) == b.read32(0x10));
}

int main() {
  // 1. A size that is not a multiple of a page: the tail page carries the
  //    real bytes then 0xFF, and everything past it is 0xFF.
  {
    const std::vector<u8> rom = pattern(0x4A00 + 0x321, 7);
    const std::string path = write_temp(rom);
    std::string err;
    auto mapped = RomSource::map_file(path, err);
    CHECK(mapped && err.empty());
    auto owned = RomSource::from_memory(rom);
    CHECK(owned->size() == rom.size());
    CHECK(owned->mask() == 0x7FFF);          // padded to 32 KB
    CHECK(mapped->mapped() && !owned->mapped());
    compare(*mapped, *owned, "unaligned");
    const u8* tail = mapped->page(0x4000);
    CHECK(std::memcmp(tail, rom.data() + 0x4000, 0xD21) == 0);
    CHECK(tail[0xD21] == 0xFF && tail[0xFFF] == 0xFF);
    CHECK(mapped->page(0x5000)[0] == 0xFF && mapped->page(0x7FFF)[0x7FF] == 0xFF);
    CHECK(mapped->read32(rom.size() - 2) == ((0xFFFFu << 16) | rom[rom.size() - 2] | (rom[rom.size() - 1] << 8)));
    unlink(path.c_str());
  }
  // 2. A page-multiple size, exactly the padded size: no tail, no 0xFF page reachable.
  {
    const std::vector<u8> rom = pattern(0x8000, 9);
    const std::string path = write_temp(rom);
    std::string err;
    auto mapped = RomSource::map_file(path, err);
    CHECK(mapped);
    CHECK(mapped->mask() == 0x7FFF);
    compare(*mapped, *RomSource::from_memory(rom), "aligned");
    unlink(path.c_str());
  }
  // 3. A range inside a larger file (a stored zip entry), at an offset that
  //    is not page-aligned.
  {
    std::vector<u8> file = pattern(0x1234, 1);
    const std::vector<u8> rom = pattern(0x5000, 3);
    file.insert(file.end(), rom.begin(), rom.end());
    const std::vector<u8> trailer = pattern(0x777, 5);
    file.insert(file.end(), trailer.begin(), trailer.end());
    const std::string path = write_temp(file);
    std::string err;
    auto mapped = RomSource::map_file(path, 0x1234, rom.size(), err);
    CHECK(mapped && err.empty());
    compare(*mapped, *RomSource::from_memory(rom), "range");
    CHECK(!RomSource::map_file(path, 0x1234, file.size(), err) && !err.empty());   // runs past the end
    CHECK(!RomSource::map_file(path, file.size() + 1, 0, err));
    unlink(path.c_str());
  }
  // 4. patch(): the overlay page is served afterwards, the neighbours are not touched.
  {
    const std::vector<u8> rom = pattern(0x8000, 11);
    const std::string path = write_temp(rom);
    std::string err;
    auto mapped = RomSource::map_file(path, err);
    u8* sec = mapped->patch(0x4000);
    CHECK(std::memcmp(sec, rom.data() + 0x4000, 0x1000) == 0);
    std::memcpy(sec, "encryObj", 8);
    CHECK(std::memcmp(mapped->page(0x4123), "encryObj", 8) == 0);
    CHECK(mapped->patch(0x4800) == sec);                       // same page, same copy
    CHECK(std::memcmp(mapped->page(0x3000), rom.data() + 0x3000, 0x1000) == 0);
    CHECK(std::memcmp(mapped->page(0x5000), rom.data() + 0x5000, 0x1000) == 0);
    u8 b[8]; mapped->read(0x4000, b, 8);
    CHECK(std::memcmp(b, "encryObj", 8) == 0);
    unlink(path.c_str());
  }
  // 5. Missing file.
  {
    std::string err;
    CHECK(!RomSource::map_file("/nonexistent/rom.nds", err) && !err.empty());
  }
  if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
  std::puts("rom_source: ok");
  return 0;
}
