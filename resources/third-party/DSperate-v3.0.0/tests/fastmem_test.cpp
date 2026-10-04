// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// core/mem/fastmem.h: a host view derived from a page table serves exactly
// the plain-RAM accesses the table serves directly, aliases mirrors, and
// faults on everything else -- read-only, trapped, code, unmapped pages, and
// data sharing a 4 KB host page with a trapped half.
#include "core/mem/fastmem.h"
#include "check.h"

#include <csetjmp>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>

using namespace ds;
using namespace ds::mem;

namespace {

sigjmp_buf g_jmp;
GuestView* g_view = nullptr;
// What the JIT's handler does: a refused access first asks the view, which
// lays the page when the table allows it (views are laid on demand), and the
// access retries; only a real refusal counts as a fault.
void on_fault(int, siginfo_t* si, void*) {
  if (g_view && g_view->fault(reinterpret_cast<uintptr_t>(si->si_addr))) return;
  siglongjmp(g_jmp, 1);
}

// Whether touching the byte faults for real (a store writes back what it
// read, so a successful store changes nothing).
bool faults(u8* p, bool store) {
  struct sigaction sa {}, old_segv {}, old_bus {};
  sa.sa_sigaction = on_fault;
  sa.sa_flags = SA_SIGINFO | SA_NODEFER;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGSEGV, &sa, &old_segv);
  sigaction(SIGBUS, &sa, &old_bus);
  volatile bool hit = true;
  if (sigsetjmp(g_jmp, 1) == 0) {
    volatile u8 v = *p;
    if (store) *p = v;
    hit = false;
  }
  sigaction(SIGSEGV, &old_segv, nullptr);
  sigaction(SIGBUS, &old_bus, nullptr);
  return hit;
}

} // namespace

int main() {
  std::unique_ptr<HostArena> arena = HostArena::create(64 * 1024);
  if (!arena) { std::printf("fastmem: no shared memory on this host, skipped\n"); return 0; }
  u8* ram = arena->take(16 * 1024);
  u8* rom = arena->take(8 * 1024);
  CHECK(ram && rom && arena->take(64 * 1024) == nullptr);
  CHECK((reinterpret_cast<uintptr_t>(ram) & 0xFFF) == 0);

  PageTable pt;
  pt.map(0x02000000, 16 * 1024, ram, PAGE_READABLE | PAGE_WRITABLE);
  pt.map(0x02004000, 16 * 1024, ram, PAGE_READABLE | PAGE_WRITABLE);   // a mirror
  pt.map(0x03000000, 8 * 1024, rom, PAGE_READABLE);                    // read-only
  pt.map_mmio(0x04000000, 8 * 1024);
  std::unique_ptr<GuestView> view = GuestView::create(*arena, pt);
  CHECK(view != nullptr);
  pt.attach_view(view.get());
  g_view = view.get();
  CHECK(view->flush());
  u8* const g = view->base();
  std::string why;

  // Mirrors alias; stores through the view land in the buffer (every touch
  // goes through faults(), which grants pages as the JIT's handler would).
  ram[0x10] = 0x5A;
  CHECK(!faults(g + 0x02000000, true) && !faults(g + 0x02007FFF, true) && !faults(g + 0x02004000, true));
  CHECK(g[0x02000010] == 0x5A && g[0x02004010] == 0x5A);
  g[0x02004020] = 0x77;
  CHECK(ram[0x20] == 0x77 && g[0x02000020] == 0x77);
  // Read-only, I/O and unmapped.
  rom[0x30] = 0x42;
  CHECK(!faults(g + 0x03000030, false) && g[0x03000030] == 0x42 && faults(g + 0x03000030, true));
  CHECK(faults(g + 0x01000000, false));   // unmapped inside the reservation
  // A 32-bit host reserves only 0x00000000-0x03FFFFFF; beyond it the JIT's
  // region table points at a guard, not at the view.
  if (GuestView::RESERVE > 0x07000000u) CHECK(faults(g + 0x04000000, false) && faults(g + 0x05000000, false) && faults(g + 0x06000000, false));
  CHECK(view->verify(&why));

  // A write trap on one 2 KB half protects its whole 4 KB host page, the other half too.
  pt.set_write_trap(0x02000000, PAGE_SIZE, true);
  CHECK(view->flush());
  CHECK(!faults(g + 0x02000800, false) && faults(g + 0x02000800, true) && !faults(g + 0x02001000, true));
  pt.set_write_trap(0x02000000, PAGE_SIZE, false);
  CHECK(view->flush() && !faults(g + 0x02000800, true));

  // Code tags follow the host page into every mirror.
  pt.set_code_host(ram, true);
  CHECK(view->flush());
  CHECK(faults(g + 0x02000000, true) && faults(g + 0x02004000, true) && !faults(g + 0x02004000, false));
  pt.set_code_host(ram, false);
  CHECK(view->flush() && !faults(g + 0x02004000, true));   // untagging stays read-only until a store asks

  // A remap to a buffer offset that does not start on 4 KB: not laid (the
  // table still serves it), and an unmap takes the view page away.
  pt.map(0x02008000, 4 * 1024, ram + 2 * 1024, PAGE_READABLE | PAGE_WRITABLE);
  CHECK(view->flush() && faults(g + 0x02008000, false) && pt.read_ptr(0x02008000) != nullptr);
  pt.unmap(0x02004000, 16 * 1024);
  CHECK(view->flush() && faults(g + 0x02004000, false));
  CHECK(view->verify(&why));
  // A remap moves the backing: the old bytes must be gone from the view at
  // once, the new ones laid on the next touch.
  CHECK(!faults(g + 0x02000000, false));
  pt.map(0x02000000, 8 * 1024, rom, PAGE_READABLE | PAGE_WRITABLE);
  CHECK(view->flush() && view->verify(&why));
  CHECK(!faults(g + 0x02000030, false) && g[0x02000030] == 0x42);

  g_view = nullptr;
  pt.attach_view(nullptr);
  std::printf("fastmem: ok (%s)\n", arena->kind());
  return 0;
}
