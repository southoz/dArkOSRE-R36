// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// The RetroAchievements memory window and the condition runtime on top of it.
// Everything here is local: no session, no server, no network.
//
// Three things are being pinned down, and each has a failure mode that is
// silent rather than loud:
//
//   * The address translation. Get it wrong and achievements evaluate against
//     the wrong bytes -- they do not error, they just never fire, or worse,
//     fire when they should not.
//   * Unbacked addresses reading zero *and reporting themselves unbacked*, so
//     rcheevos disables those achievements instead of comparing against
//     whatever the DS left lying around.
//   * One rc_runtime_do_frame per emulated frame. rcheevos keeps a delta per
//     memory reference, so an extra call per frame corrupts hit counts and edge
//     triggers with nothing in any log to say so. The hit-count test below is
//     the regression guard for that rule.
#include <cstdio>
#include <cstring>
#include <string>

#include "check.h"
#include "cheevos/cheevos_memory.h"
#include "core/mem/bus.h"
#include "core/nds.h"

extern "C" {
#include "rc_runtime.h"
}

using namespace ds;

namespace {

// The RetroAchievements addresses, as rc_consoles.h publishes them for the DS.
constexpr u32 RA_MAIN = 0x000000;                       // 4 MB of main RAM
constexpr u32 RA_HOLE = 0x400000;                       // 12 MB of DSi-only padding
constexpr u32 RA_DTCM = 0x1000000;                      // 16 KB of data TCM
constexpr u32 MAIN_LEN = mem::Bus::MAIN_RAM_SIZE;
constexpr u32 DTCM_LEN = mem::Bus::DTCM_SIZE;

struct Fixture {
  NDS nds;
  cheevos::Memory mem;
  Fixture() {
    std::string err;
    CHECK(mem.attach(nds, err));
    CHECK(err.empty());
    CHECK(mem.attached());
  }
  u8* main_ram() { return nds.bus.main_ram.get(); }
  u8* dtcm() { return nds.bus.dtcm.get(); }
};

void the_map_is_the_one_we_expect() {
  Fixture f;
  // Main RAM and data TCM, with the hole between them. The last address the map
  // describes is the end of DTCM.
  CHECK(f.mem.max_address() == RA_DTCM + DTCM_LEN - 1);
  CHECK(f.mem.supported(RA_MAIN));
  CHECK(f.mem.supported(RA_MAIN + MAIN_LEN - 1));
  CHECK(f.mem.supported(RA_DTCM));
  CHECK(f.mem.supported(RA_DTCM + DTCM_LEN - 1));
}

void main_ram_and_dtcm_read_through() {
  Fixture f;
  f.main_ram()[0] = 0x11;
  f.main_ram()[MAIN_LEN - 1] = 0x22;
  f.dtcm()[0] = 0x33;
  f.dtcm()[DTCM_LEN - 1] = 0x44;

  u8 b = 0;
  CHECK(f.mem.read(RA_MAIN, &b, 1) == 1 && b == 0x11);
  CHECK(f.mem.read(RA_MAIN + MAIN_LEN - 1, &b, 1) == 1 && b == 0x22);
  CHECK(f.mem.read(RA_DTCM, &b, 1) == 1 && b == 0x33);
  CHECK(f.mem.read(RA_DTCM + DTCM_LEN - 1, &b, 1) == 1 && b == 0x44);

  // A multi-byte read, little-endian through peek() as rcheevos sees it.
  f.main_ram()[0x100] = 0x78; f.main_ram()[0x101] = 0x56;
  f.main_ram()[0x102] = 0x34; f.main_ram()[0x103] = 0x12;
  CHECK(cheevos::Memory::peek(RA_MAIN + 0x100, 4, &f.mem) == 0x12345678u);
  CHECK(cheevos::Memory::peek(RA_MAIN + 0x100, 2, &f.mem) == 0x5678u);
  CHECK(cheevos::Memory::peek(RA_MAIN + 0x100, 1, &f.mem) == 0x78u);
}

// The DSi-only hole. It is a real region in rcheevos' map, and we must answer
// for it with zero and "unbacked" -- not with whatever the DS has at that
// address, which is how a condition written against DSi RAM turns into a false
// unlock instead of a disabled achievement.
void the_dsi_hole_reads_zero_and_is_unbacked() {
  Fixture f;
  std::memset(f.main_ram(), 0xFF, 64);

  u8 b[8];
  std::memset(b, 0xAA, sizeof b);
  CHECK(f.mem.read(RA_HOLE, b, sizeof b) == 0);         // nothing backed
  for (u8 v : b) CHECK(v == 0);                          // and it reads zero
  CHECK(!f.mem.supported(RA_HOLE));
  CHECK(!f.mem.supported(RA_HOLE + 0xBFFFFF));
  CHECK(cheevos::Memory::peek(RA_HOLE, 4, &f.mem) == 0);
}

// A read that starts in main RAM and runs into the hole has to report the
// partial count: that is the signal rcheevos uses to decide an address is not
// fully supported here.
void a_read_across_the_boundary_is_partial() {
  Fixture f;
  std::memset(f.main_ram() + MAIN_LEN - 2, 0x5A, 2);

  u8 b[4];
  std::memset(b, 0xAA, sizeof b);
  CHECK(f.mem.read(RA_MAIN + MAIN_LEN - 2, b, 4) == 2);
  CHECK(b[0] == 0x5A && b[1] == 0x5A);
  CHECK(b[2] == 0 && b[3] == 0);
  CHECK(!f.mem.supported(RA_MAIN + MAIN_LEN - 2, 4));
  CHECK(f.mem.supported(RA_MAIN + MAIN_LEN - 2, 2));
}

void past_the_map_reads_zero() {
  Fixture f;
  u8 b[4];
  std::memset(b, 0xAA, sizeof b);
  const u32 past = f.mem.max_address() + 1;
  CHECK(f.mem.read(past, b, sizeof b) == 0);
  for (u8 v : b) CHECK(v == 0);
  CHECK(!f.mem.supported(past));
}

// rcheevos may evaluate before a game is loaded; that must be quiet, not a
// crash through a null pointer.
void an_unattached_window_is_safe() {
  cheevos::Memory mem;
  CHECK(!mem.attached());
  u8 b[4];
  std::memset(b, 0xAA, sizeof b);
  CHECK(mem.read(RA_MAIN, b, sizeof b) == 0);
  for (u8 v : b) CHECK(v == 0);
  CHECK(cheevos::Memory::peek(RA_MAIN, 4, &mem) == 0);
  CHECK(cheevos::Memory::read_memory(RA_MAIN, b, 4, nullptr) == 0);
}

// ---------------------------------------------------------------------------
// The runtime on top of the window.

int g_triggered = 0;
u32 g_triggered_id = 0;

void on_event(const rc_runtime_event_t* e) {
  if (e->type == RC_RUNTIME_EVENT_ACHIEVEMENT_TRIGGERED) {
    ++g_triggered;
    g_triggered_id = e->id;
  }
}

struct Runtime {
  rc_runtime_t rt;
  cheevos::Memory* mem;
  explicit Runtime(cheevos::Memory& m) : mem(&m) { rc_runtime_init(&rt); g_triggered = 0; g_triggered_id = 0; }
  ~Runtime() { rc_runtime_destroy(&rt); }
  void activate(u32 id, const char* memaddr) {
    CHECK(rc_runtime_activate_achievement(&rt, id, memaddr, nullptr, 0) == RC_OK);
  }
  // Exactly what the emulator will do: one call per emulated frame.
  void frame() { rc_runtime_do_frame(&rt, on_event, cheevos::Memory::peek, mem, nullptr); }
};

// A condition over main RAM fires on the frame it becomes true, and the id that
// comes back is the one we activated.
void a_trigger_fires_on_the_right_frame() {
  Fixture f;
  Runtime r(f.mem);
  r.activate(7, "0xH000010=5");          // 8-bit at RA 0x10 equals 5

  // rcheevos holds a new achievement in WAITING until its condition has been
  // false once, so that loading into an already-satisfied state does not unlock
  // it. Frame one does that.
  f.main_ram()[0x10] = 0;
  r.frame();
  CHECK(g_triggered == 0);

  f.main_ram()[0x10] = 5;
  r.frame();
  CHECK(g_triggered == 1);
  CHECK(g_triggered_id == 7);

  // And it does not fire again while it stays true.
  r.frame();
  r.frame();
  CHECK(g_triggered == 1);
}

// Data TCM is reachable the same way -- it is the second backed region, so a
// condition there exercises the offset arithmetic rather than the base case.
//
// Note the right-hand side is decimal. In RetroAchievements' syntax an
// unprefixed "0x..." is a *16-bit memory read*, not a constant, so
// "0xH1000004=0x2A" asks whether the byte at 0x1000004 equals the halfword at
// address 0x2A -- which is a perfectly valid condition, just not the one
// intended. A hex constant is written "h2A". Cost an hour the first time.
void a_trigger_can_read_data_tcm() {
  Fixture f;
  Runtime r(f.mem);
  r.activate(9, "0xH1000004=42");
  f.dtcm()[4] = 0;
  r.frame();
  CHECK(g_triggered == 0);
  f.dtcm()[4] = 0x2A;
  r.frame();
  CHECK(g_triggered == 1);
  CHECK(g_triggered_id == 9);
}

// THE frame-pacing guard. A hit count says "this condition held for N frames",
// which is only true if do_frame is called once per frame. Call it twice per
// frame and this fires after five; miss calls and it never fires. Either way
// the symptom in the wild is an achievement that unlocks at the wrong time,
// with nothing logged, so it is pinned here.
void hit_counts_follow_the_frame_count() {
  Fixture f;
  Runtime r(f.mem);
  r.activate(11, "0xH000020=1.10.");     // true for 10 frames

  f.main_ram()[0x20] = 0;
  r.frame();                             // out of WAITING
  CHECK(g_triggered == 0);

  f.main_ram()[0x20] = 1;
  for (int i = 0; i < 9; ++i) {
    r.frame();
    CHECK(g_triggered == 0);             // not yet: only i+1 hits
  }
  r.frame();                             // the tenth hit
  CHECK(g_triggered == 1);
}

// A condition over the DSi hole must not unlock. The window hands rcheevos
// zero for it, so a condition looking for a non-zero value can never be true
// -- which is the conservative half of the design; the other half is
// rc_runtime_validate_addresses, which phase 3 wires to mark it unsupported.
void a_condition_over_the_hole_never_fires() {
  Fixture f;
  Runtime r(f.mem);
  r.activate(13, "0xH400000=5");
  std::memset(f.main_ram(), 5, 64);      // the bytes it would alias if mis-mapped
  for (int i = 0; i < 20; ++i) r.frame();
  CHECK(g_triggered == 0);
}

// ---------------------------------------------------------------------------

// The string the server decides whether to talk to us at all by.
void the_user_agent_is_well_formed() {
  const std::string ua = cheevos::user_agent();
  // A product token with no spaces in the product name, then rcheevos' clause:
  // "DSperate/<version> rcheevos/<version>". RetroAchievements answers anything
  // it cannot recognise with 403 unsupported_client.
  CHECK(ua.rfind("DSperate/", 0) == 0);
  const size_t sp = ua.find(' ');
  CHECK(sp != std::string::npos);                        // there is a second clause
  CHECK(ua.find(' ', sp + 1) == std::string::npos);      // and only one space
  CHECK(ua.find(" rcheevos/") == sp);

  // The version is numeric and non-empty, so the server can compare it.
  const std::string version = ua.substr(9, sp - 9);
  CHECK(!version.empty());
  for (char c : version) CHECK((c >= '0' && c <= '9') || c == '.');
  CHECK(version.find('.') != std::string::npos);
  std::printf("cheevos_memory: user agent %s\n", ua.c_str());
}

} // namespace

// On the DSi machine the map is rcheevos' DSi one: the hole is real RAM, so the
// whole 16 MB reads through at the same addresses, and data TCM follows it.
void the_dsi_map_backs_all_of_main_ram() {
  NDS nds;
  nds.dsi = true;
  cheevos::Memory mem;
  std::string err;
  CHECK(mem.attach(nds, err));
  CHECK(err.empty());
  constexpr u32 DSI_LEN = mem::Bus::MAIN_RAM_SIZE_DSI;
  nds.bus.main_ram.get()[RA_HOLE] = 0x5C;              // above the DS's 4 MB
  nds.bus.main_ram.get()[DSI_LEN - 1] = 0x6D;
  nds.bus.dtcm.get()[0] = 0x7E;
  u8 b = 0;
  CHECK(mem.read(RA_HOLE, &b, 1) == 1 && b == 0x5C);
  CHECK(mem.read(DSI_LEN - 1, &b, 1) == 1 && b == 0x6D);
  CHECK(mem.read(RA_DTCM, &b, 1) == 1 && b == 0x7E);
  CHECK(mem.supported(RA_MAIN, DSI_LEN));
  CHECK(mem.max_address() == RA_DTCM + DTCM_LEN - 1);
  u8 past[2] = {0xAA, 0xAA};
  CHECK(mem.read(RA_DTCM + DTCM_LEN, past, 2) == 0 && past[0] == 0);
}

int main() {
  the_map_is_the_one_we_expect();
  the_dsi_map_backs_all_of_main_ram();
  main_ram_and_dtcm_read_through();
  the_dsi_hole_reads_zero_and_is_unbacked();
  a_read_across_the_boundary_is_partial();
  past_the_map_reads_zero();
  an_unattached_window_is_safe();
  a_trigger_fires_on_the_right_frame();
  a_trigger_can_read_data_tcm();
  hit_counts_follow_the_frame_count();
  a_condition_over_the_hole_never_fires();
  the_user_agent_is_well_formed();
  std::printf("cheevos_memory: ok\n");
  return 0;
}
