// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/mem/page_table.h"
#include "check.h"

#include <cstring>
#include <memory>

using namespace ds;
using namespace ds::mem;

int main() {
  PageTable pt;
  auto ram = alloc_page_buf(64 * 1024);   // PAGE_SIZE-aligned, as map() requires

  // Unmapped => slow path.
  CHECK(pt.read_ptr(0x02000000) == nullptr);

  // Map with a mirror and check biasing: host + guest must land on the right byte.
  pt.map(0x02000000, 64 * 1024, ram.get(), PAGE_READABLE | PAGE_WRITABLE);
  pt.map(0x02010000, 64 * 1024, ram.get(), PAGE_READABLE | PAGE_WRITABLE);
  ram[0x1234] = 0xAB;
  CHECK(pt.read_ptr(0x02001234) && *pt.read_ptr(0x02001234) == 0xAB);
  CHECK(pt.read_ptr(0x02011234) && *pt.read_ptr(0x02011234) == 0xAB);
  CHECK(pt.read_ptr(0x0200FFFF) == ram.get() + 0xFFFF);
  CHECK(pt.read_ptr(0x02020000) == nullptr);

  // Read-only page: loads fast, stores slow.
  pt.map(0x02020000, PAGE_SIZE, ram.get(), PAGE_READABLE);
  bool code = true;
  CHECK(pt.read_ptr(0x02020010) == ram.get() + 0x10);
  CHECK(pt.write_ptr(0x02020010, &code) == nullptr);

  // MMIO: both slow.
  pt.map_mmio(0x04000000, PAGE_SIZE);
  CHECK(pt.read_ptr(0x04000004) == nullptr);
  CHECK(pt.write_ptr(0x04000004, &code) == nullptr);

  // CODE tag: loads unaffected, stores flagged.
  pt.set_code(0x02000800, 16, true);
  CHECK(pt.read_ptr(0x02000800) == ram.get() + 0x800);
  code = false;
  CHECK(pt.write_ptr(0x02000900, &code) == ram.get() + 0x900 && code);
  code = true;
  CHECK(pt.write_ptr(0x02000400, &code) == ram.get() + 0x400 && !code);
  pt.set_code(0x02000800, 16, false);
  code = true;
  CHECK(pt.write_ptr(0x02000900, &code) && !code);

  pt.unmap(0x02000000, 64 * 1024);
  CHECK(pt.read_ptr(0x02001234) == nullptr);
  CHECK(pt.read_ptr(0x02011234) != nullptr);   // mirror still mapped
  std::puts("page_table: ok");
  return 0;
}
