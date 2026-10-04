// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// ARM9 I/O block tests: hardware divider / square root, VRAMCNT register
// layout, GXSTAT placeholder, and the frontend input path (buttons, KEYCNT
// interrupts, touchscreen samples over SPI).
#include "core/nds.h"
#include "core/input/input_log.h"

#include <cstdio>
#include <cstring>
#include <memory>

using namespace ds;

static int failures = 0;
#define CHECK_EQ(a, b) do { auto va_ = (a); auto vb_ = (b); if (static_cast<unsigned long long>(va_) != static_cast<unsigned long long>(vb_)) { std::fprintf(stderr, "FAIL %s:%d: %s = %llx, expected %llx\n", __FILE__, __LINE__, #a, (unsigned long long)va_, (unsigned long long)vb_); ++failures; } } while (0)

static void w32(NDS& nds, u32 a, u32 v) { nds.bus.dma_write32(Cpu::ARM9, a, v); }
static u32  r32(NDS& nds, u32 a) { return nds.bus.dma_read32(Cpu::ARM9, a); }
static void w16(NDS& nds, u32 a, u16 v) { nds.bus.dma_write16(Cpu::ARM9, a, v); }
static u16  r16(NDS& nds, u32 a) { return nds.bus.dma_read16(Cpu::ARM9, a); }

// Let scheduled events (the unit's latency) fire.
static void settle(NDS& nds) { nds.sched.run_until(nds.sched.now() + 400); }

static void test_div() {
  NDS nds;
  // 32/32: 100 / 7 = 14 r 2.
  w32(nds, 0x04000280, 0);
  w32(nds, 0x04000290, 100); w32(nds, 0x04000298, 7);
  CHECK_EQ(r16(nds, 0x04000280) & 0x8000, 0x8000u);          // busy
  settle(nds);
  CHECK_EQ(r16(nds, 0x04000280) & 0xC000, 0u);
  CHECK_EQ(r32(nds, 0x040002A0), 14u); CHECK_EQ(r32(nds, 0x040002A4), 0u);
  CHECK_EQ(r32(nds, 0x040002A8), 2u);
  // Negative 32-bit: -100 / 7 = -14 r -2, sign-extended into 64 bits.
  w32(nds, 0x04000290, static_cast<u32>(-100)); settle(nds);
  CHECK_EQ(r32(nds, 0x040002A0), static_cast<u32>(-14)); CHECK_EQ(r32(nds, 0x040002A4), 0xFFFFFFFFu);
  CHECK_EQ(r32(nds, 0x040002A8), static_cast<u32>(-2)); CHECK_EQ(r32(nds, 0x040002AC), 0xFFFFFFFFu);
  // Division by zero (32-bit mode): quotient +/-1 with the odd upper half, remainder = numerator, flag set.
  w32(nds, 0x04000290, 5); w32(nds, 0x04000298, 0); settle(nds);
  CHECK_EQ(r32(nds, 0x040002A0), 0xFFFFFFFFu); CHECK_EQ(r32(nds, 0x040002A4), 0u);
  CHECK_EQ(r32(nds, 0x040002A8), 5u);
  CHECK_EQ(r16(nds, 0x04000280) & 0x4000, 0x4000u);
  // 64/32: 0x1_0000_0000 / 2.
  w32(nds, 0x04000280, 1);
  w32(nds, 0x04000290, 0); w32(nds, 0x04000294, 1); w32(nds, 0x04000298, 2); settle(nds);
  CHECK_EQ(r32(nds, 0x040002A0), 0x80000000u); CHECK_EQ(r32(nds, 0x040002A4), 0u);
  CHECK_EQ(r16(nds, 0x04000280) & 0x4000, 0u);
  // 64/64 overflow: INT64_MIN / -1.
  w32(nds, 0x04000280, 2);
  w32(nds, 0x04000290, 0); w32(nds, 0x04000294, 0x80000000); w32(nds, 0x04000298, 0xFFFFFFFF); w32(nds, 0x0400029C, 0xFFFFFFFF); settle(nds);
  CHECK_EQ(r32(nds, 0x040002A0), 0u); CHECK_EQ(r32(nds, 0x040002A4), 0x80000000u);
  CHECK_EQ(r32(nds, 0x040002A8), 0u);
  // 16-bit halves of the operand registers are writable too.
  w32(nds, 0x04000280, 0);
  w16(nds, 0x04000290, 81); w16(nds, 0x04000292, 0); w16(nds, 0x04000298, 9); w16(nds, 0x0400029A, 0); settle(nds);
  CHECK_EQ(r16(nds, 0x040002A0), 9u);
}

static void test_sqrt() {
  NDS nds;
  w32(nds, 0x040002B0, 0);
  w32(nds, 0x040002B8, 1024); settle(nds);
  CHECK_EQ(r32(nds, 0x040002B4), 32u);
  w32(nds, 0x040002B8, 0xFFFFFFFF); settle(nds);
  CHECK_EQ(r32(nds, 0x040002B4), 65535u);
  w32(nds, 0x040002B0, 1);                                  // 64-bit
  w32(nds, 0x040002B8, 0); w32(nds, 0x040002BC, 1); settle(nds);   // sqrt(2^32) = 65536
  CHECK_EQ(r32(nds, 0x040002B4), 65536u);
  w32(nds, 0x040002B8, 0xFFFFFFFF); w32(nds, 0x040002BC, 0xFFFFFFFF); settle(nds);
  CHECK_EQ(r32(nds, 0x040002B4), 0xFFFFFFFFu);
  CHECK_EQ(r16(nds, 0x040002B0) & 0x8000, 0u);
}

static void test_vramcnt_layout() {
  NDS nds;
  // 0x247 is WRAMCNT; 0x248/0x249 are banks H and I.
  w16(nds, 0x04000248, 0x8382);
  CHECK_EQ(nds.io.vramcnt[7], 0x82u); CHECK_EQ(nds.io.vramcnt[8], 0x83u);
  CHECK_EQ(r16(nds, 0x04000248), 0x8382u);
  nds.bus.dma_write16(Cpu::ARM9, 0x04000246, 0x0381);        // G = 0x81, WRAMCNT = 3
  CHECK_EQ(nds.io.vramcnt[6], 0x81u); CHECK_EQ(static_cast<u32>(nds.io.wramcnt), 3u);
  CHECK_EQ(nds.io.powcnt1, 0u);                             // untouched by the neighbouring writes
}

static void test_gxstat_irq() {
  NDS nds;
  nds.io.cpu_io[0].ie = 1u << 21; nds.io.cpu_io[0].ime = 1;
  w32(nds, 0x04000600, 0x40000000);                         // ignored: geometry engine powered down
  CHECK_EQ(nds.io.cpu_io[0].if_ & (1u << 21), 0u);
  w16(nds, 0x04000304, 0x820F);
  w32(nds, 0x04000600, 0x40000000);                         // IRQ while the FIFO is less than half full
  CHECK_EQ(nds.io.cpu_io[0].if_ & (1u << 21), 1u << 21);
  CHECK_EQ(r32(nds, 0x04000600) & 0x06000000, 0x06000000u); // FIFO empty + less than half
  w32(nds, 0x04000214, 1u << 21);                           // level-sensitive: acknowledging re-raises it
  CHECK_EQ(nds.io.cpu_io[0].if_ & (1u << 21), 1u << 21);
  w32(nds, 0x04000600, 0);
  CHECK_EQ(nds.io.cpu_io[0].if_ & (1u << 21), 0u);
}

// Reads one 12-bit sample from the touchscreen controller the way a game
// does: control byte with the channel, then two data bytes.
static u16 tsc_read(NDS& nds, u32 channel, bool bits8 = false) {
  auto spi = [&](u8 v) {
    nds.io.write(Cpu::ARM7, 0x040001C0, 16, 0x8000 | 0x0800 | 0x0200);   // enable, hold, device 2
    nds.io.write(Cpu::ARM7, 0x040001C2, 8, v);
    nds.sched.run_until(nds.sched.now() + 4000);                         // transfer latency
    return static_cast<u8>(nds.io.read(Cpu::ARM7, 0x040001C2, 8));
  };
  spi(static_cast<u8>(0x80 | (channel << 4) | (bits8 ? 0x08 : 0)));
  const u8 hi = spi(0), lo = spi(0);
  nds.io.write(Cpu::ARM7, 0x040001C0, 16, 0);                            // deselect
  return bits8 ? static_cast<u16>((hi << 1) | (lo >> 7)) : static_cast<u16>((hi << 5) | (lo >> 3));
}

// Microphone through the PMIC amplifier and the TSC's AUX channel, and the
// hinge bit with its wake-up interrupt.
static void test_mic_and_lid() {
  NDS nds;
  static const s16 buf[4] = {0, 1024, -2048, 32767};
  nds.io.set_mic(buf, 4);
  CHECK_EQ(tsc_read(nds, 6), 0x800u);                       // first quarter of the frame: sample 0, gain x20
  nds.sched.run_until(nds.sched.now() + CYCLES_PER_FRAME / 4);
  CHECK_EQ(tsc_read(nds, 6), 0x800u + 64);                  // 1024 >> 4
  nds.sched.run_until(nds.sched.now() + CYCLES_PER_FRAME / 4);
  CHECK_EQ(tsc_read(nds, 6), 0x800u - 128);
  CHECK_EQ(tsc_read(nds, 6, true), (0x800u - 128) >> 4);    // 8-bit conversion
  nds.io.spi_pm.regs[3] = 3;                                // x160: 8x the x20 level, clipped
  CHECK_EQ(tsc_read(nds, 6), 0x800u - 1024);
  nds.sched.run_until(nds.sched.now() + CYCLES_PER_FRAME / 4);
  CHECK_EQ(tsc_read(nds, 6), 0xFFFu);
  nds.io.set_mic(nullptr, 0);                               // silence
  CHECK_EQ(tsc_read(nds, 6), 0x800u);

  CHECK_EQ(nds.io.read(Cpu::ARM7, 0x04000136, 16) & 0x80, 0u);        // open
  nds.io.cpu_io[1].if_ = 0;
  nds.io.set_lid(true);
  CHECK_EQ(nds.io.read(Cpu::ARM7, 0x04000136, 16) & 0x80, 0x80u);
  CHECK_EQ(nds.io.cpu_io[1].if_ & (1u << 22), 0u);                     // closing is not an IRQ
  nds.io.set_lid(false);
  CHECK_EQ(nds.io.read(Cpu::ARM7, 0x04000136, 16) & 0x80, 0u);
  CHECK_EQ(nds.io.cpu_io[1].if_ & (1u << 22), 1u << 22);               // opening is
  CHECK_EQ(nds.io.read(Cpu::ARM9, 0x04000136, 16), 0u);
}

static void test_input() {
  NDS nds;
  using B = io::Io::Button;
  CHECK_EQ(r16(nds, 0x04000130), 0x03FFu);                 // nothing held
  CHECK_EQ(nds.io.read(Cpu::ARM7, 0x04000136, 16), 0x007Fu);

  nds.io.set_buttons((1u << B::BTN_A) | (1u << B::BTN_DOWN) | (1u << B::BTN_X));
  CHECK_EQ(r16(nds, 0x04000130), 0x03FFu & ~((1u << 0) | (1u << 7)));
  CHECK_EQ(nds.io.read(Cpu::ARM7, 0x04000136, 16), 0x007Eu);   // X held, pen up
  nds.io.set_buttons(0);
  CHECK_EQ(r16(nds, 0x04000130), 0x03FFu);
  CHECK_EQ(nds.io.read(Cpu::ARM7, 0x04000136, 16), 0x007Fu);

  // KEYCNT: IRQ when any selected key is held (bit 14 on, bit 15 off).
  nds.cpu(Cpu::ARM9).hot.regs[15] = 0;
  w16(nds, 0x04000132, 0x4000 | (1u << 1));                // B
  nds.io.cpu_io[0].if_ = 0;
  nds.io.set_buttons(1u << B::BTN_A);
  CHECK_EQ(nds.io.cpu_io[0].if_ & (1u << 12), 0u);         // A is not selected
  nds.io.set_buttons(1u << B::BTN_B);
  CHECK_EQ(nds.io.cpu_io[0].if_ & (1u << 12), 1u << 12);
  // AND condition: both selected keys must be held.
  nds.io.cpu_io[0].if_ = 0;
  w16(nds, 0x04000132, 0xC000 | (1u << 0) | (1u << 1));
  nds.io.set_buttons(1u << B::BTN_A);
  CHECK_EQ(nds.io.cpu_io[0].if_ & (1u << 12), 0u);
  nds.io.set_buttons((1u << B::BTN_A) | (1u << B::BTN_B));
  CHECK_EQ(nds.io.cpu_io[0].if_ & (1u << 12), 1u << 12);
  w16(nds, 0x04000132, 0);
  nds.io.set_buttons(0);

  // Touchscreen: calibration is normalised at load, so ADC = pixel << 4.
  CHECK_EQ(nds.io.read(Cpu::ARM7, 0x04000136, 16) & 0x40, 0x40u);   // pen up
  nds.io.set_touch(100, 50, true);
  CHECK_EQ(nds.io.read(Cpu::ARM7, 0x04000136, 16) & 0x40, 0u);      // pen down
  CHECK_EQ(tsc_read(nds, 5), 100u << 4);                            // X
  CHECK_EQ(tsc_read(nds, 1), 50u << 4);                             // Y
  nds.io.set_touch(400, -8, true);                                  // clamped to the screen
  CHECK_EQ(tsc_read(nds, 5), 255u << 4);
  CHECK_EQ(tsc_read(nds, 1), 0u);
  nds.io.set_touch(0, 0, false);
  CHECK_EQ(nds.io.read(Cpu::ARM7, 0x04000136, 16) & 0x40, 0x40u);
  CHECK_EQ(tsc_read(nds, 1), 0xFFFu);
}

static void test_input_log() {
  const char* path = "/tmp/dsperate_input_log_test.bin";
  {
    input::Log w;
    CHECK_EQ(w.open_write(path), true);
    for (u32 i = 0; i < 1000; ++i) {
      input::Frame f{static_cast<u16>(i * 7), static_cast<u8>(i), static_cast<u8>(255 - i), (i & 3) == 0, (i & 7) == 1};
      for (int k = 0; k < input::Frame::MIC_SAMPLES; ++k) f.mic[k] = static_cast<s8>(i + k);
      w.write(f);
    }
  }
  input::Log r;
  CHECK_EQ(r.open_read(path), true);
  CHECK_EQ(r.frames(), 1000u);
  input::Frame f; u32 n = 0;
  while (r.read(f)) {
    input::Frame want{static_cast<u16>(n * 7), static_cast<u8>(n), static_cast<u8>(255 - n), (n & 3) == 0, (n & 7) == 1};
    for (int k = 0; k < input::Frame::MIC_SAMPLES; ++k) want.mic[k] = static_cast<s8>(n + k);
    CHECK_EQ(f == want, true);
    ++n;
  }
  CHECK_EQ(n, 1000u);
  // Applying a frame drives the registers the games read.
  NDS nds;
  input::Frame fr{1u << io::Io::BTN_START, 10, 20, true, true};
  fr.mic[2] = 16;
  input::apply(nds, fr);
  CHECK_EQ(r16(nds, 0x04000130), 0x03FFu & ~(1u << 3));
  CHECK_EQ(nds.io.read(Cpu::ARM7, 0x04000136, 16) & 0xC0, 0x80u);   // pen down, lid closed
  nds.sched.run_until(nds.sched.now() + CYCLES_PER_FRAME * 5 / 16);   // slot 2 of 8
  CHECK_EQ(tsc_read(nds, 6), 0x800u + 256);                           // 16 * 256 >> 4
  // A version-1 log (8-byte records, no mic or lid) still reads.
  {
    FILE* v1 = std::fopen(path, "wb");
    const u8 h[16] = {'D', 'S', 'I', 'N', 1, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0};
    const u8 r[16] = {3, 0, 5, 6, 1, 0, 0, 0,  0, 0, 7, 8, 0, 0, 0, 0};
    std::fwrite(h, 1, 16, v1); std::fwrite(r, 1, 16, v1); std::fclose(v1);
  }
  input::Log old;
  CHECK_EQ(old.open_read(path), true);
  CHECK_EQ(old.frames(), 2u);
  CHECK_EQ(old.read(f), true);
  CHECK_EQ((f == input::Frame{3, 5, 6, true}), true);
  CHECK_EQ(old.read(f), true);
  CHECK_EQ((f == input::Frame{0, 7, 8, false}), true);
  CHECK_EQ(old.read(f), false);
  // Decimation keeps the loudest sample of each span.
  s16 cap[80] = {};
  cap[25] = -12800; cap[27] = 3000; cap[79] = 25600;
  input::decimate_mic(fr, cap, 80);
  CHECK_EQ(fr.mic[0], 0); CHECK_EQ(fr.mic[2], -50); CHECK_EQ(fr.mic[7], 100);
  std::remove(path);
}

// ---- RTC free-running clock ------------------------------------------------
// The clock only runs when a frontend asks for it; what is tested here is the
// carry, because a game may have written its own date into the chip and
// whatever it wrote has to advance correctly from there.
// Fire the one-second event directly rather than running a whole second of
// emulation for each tick: what is under test is the carry, not the schedule.
static void advance_seconds(NDS& nds, u32 n) {
  for (u32 i = 0; i < n; ++i) { nds.io.rtc.next_tick = nds.sched.now(); nds.io.rtc_event(); }
}

static void set_date(NDS& nds, u8 yy, u8 mm, u8 dd, u8 dow, u8 hh, u8 mi, u8 ss) {
  const u8 d[7] = {yy, mm, dd, dow, hh, mi, ss};
  std::memcpy(nds.io.rtc.datetime, d, 7);
}

static void check_date(NDS& nds, u8 yy, u8 mm, u8 dd, u8 hh, u8 mi, u8 ss, int line) {
  const u8* d = nds.io.rtc.datetime;
  const u8 want[7] = {yy, mm, dd, d[3], hh, mi, ss};
  for (int i = 0; i < 7; ++i)
    if (d[i] != want[i]) {
      std::fprintf(stderr, "FAIL %s:%d: rtc %02x-%02x-%02x %02x:%02x:%02x, expected %02x-%02x-%02x %02x:%02x:%02x\n",
                   __FILE__, line, d[0], d[1], d[2], d[4], d[5], d[6], yy, mm, dd, hh, mi, ss);
      ++failures;
      return;
    }
}
#define CHECK_DATE(nds, ...) check_date(nds, __VA_ARGS__, __LINE__)

static void test_rtc_clock() {
  NDS nds;
  // Off by default: the harness compares runs against each other and against
  // melonDS, so time must not move on its own.
  CHECK_EQ(nds.io.rtc.ticking, false);
  advance_seconds(nds, 5);
  CHECK_DATE(nds, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00);
  CHECK_EQ(nds.io.rtc.status1 & 0x80, 0x80u);       // power lost, as melonDS resets it

  nds.io.start_rtc_clock();
  // Seeded from the wall, so the value is not fixed -- but the power-lost bit
  // has to be gone, because that bit is what sends the firmware into its
  // first-boot setup wizard instead of the menu.
  CHECK_EQ(nds.io.rtc.status1 & 0x80, 0u);
  CHECK_EQ(nds.io.rtc.ticking, true);

  // Second, minute and hour carry.
  set_date(nds, 0x26, 0x09, 0x02, 3, 0x11, 0x59, 0x59);
  advance_seconds(nds, 1);
  CHECK_DATE(nds, 0x26, 0x09, 0x02, 0x12, 0x00, 0x00);
  set_date(nds, 0x26, 0x09, 0x02, 3, 0x23, 0x59, 0x59);
  advance_seconds(nds, 1);
  CHECK_DATE(nds, 0x26, 0x09, 0x03, 0x00, 0x00, 0x00);
  CHECK_EQ(nds.io.rtc.datetime[3], 4u);             // and the day of the week with it

  // Month end, year end, and the leap day. Years are two digits from 2000, so
  // every year divisible by four in range is a leap year -- 2024 has a 29th
  // of February and 2026 does not.
  set_date(nds, 0x26, 0x09, 0x30, 3, 0x23, 0x59, 0x59);
  advance_seconds(nds, 1);
  CHECK_DATE(nds, 0x26, 0x10, 0x01, 0x00, 0x00, 0x00);
  set_date(nds, 0x26, 0x12, 0x31, 4, 0x23, 0x59, 0x59);
  advance_seconds(nds, 1);
  CHECK_DATE(nds, 0x27, 0x01, 0x01, 0x00, 0x00, 0x00);
  set_date(nds, 0x26, 0x02, 0x28, 6, 0x23, 0x59, 0x59);
  advance_seconds(nds, 1);
  CHECK_DATE(nds, 0x26, 0x03, 0x01, 0x00, 0x00, 0x00);
  set_date(nds, 0x24, 0x02, 0x28, 3, 0x23, 0x59, 0x59);
  advance_seconds(nds, 1);
  CHECK_DATE(nds, 0x24, 0x02, 0x29, 0x00, 0x00, 0x00);
  set_date(nds, 0x24, 0x02, 0x29, 4, 0x23, 0x59, 0x59);
  advance_seconds(nds, 1);
  CHECK_DATE(nds, 0x24, 0x03, 0x01, 0x00, 0x00, 0x00);

  // A slice that overruns the due time still lands on the right second: the
  // event catches up whole seconds rather than dropping them. Ten seconds
  // late here, so ten carries out of one event.
  // Ten seconds of scheduler time have to have passed for the mark to be able
  // to sit that far back, so this is the one place that runs real cycles.
  nds.sched.run_until(10 * static_cast<u64>(ARM9_CLOCK_HZ));
  set_date(nds, 0x26, 0x09, 0x02, 3, 0x00, 0x00, 0x00);
  nds.io.rtc.next_tick = nds.sched.now() - 9 * static_cast<u64>(ARM9_CLOCK_HZ);
  nds.io.rtc_event();
  CHECK_DATE(nds, 0x26, 0x09, 0x02, 0x00, 0x00, 0x10);
}

// ---- firmware settings sidecar --------------------------------------------
// The firmware saves the console's name, birthday and so on by writing its own
// flash. Those pages go to a file beside the firmware, never into the dump.
// SPICNT and SPIDATA are the ARM7's; device 1 (bits 8-9) is the firmware
// flash, bit 11 holds the chip select for the next byte. The transfer takes
// time and a byte written while the bus is busy is dropped, exactly as on
// hardware, so each byte is given its cycles.
static void fw_spi(NDS& nds, u8 byte, bool hold) {
  nds.bus.dma_write16(Cpu::ARM7, 0x040001C0, static_cast<u16>(0x8000 | (hold ? 0x0800 : 0) | 0x0100));
  nds.bus.dma_write8(Cpu::ARM7, 0x040001C2, byte);
  settle(nds);
}

static void test_firmware_override() {
  // Each NDS carries the console's memory, so they go on the heap and one at
  // a time -- five of them at once overflows the stack.
  auto fresh = [] {
    auto p = std::make_unique<NDS>();
    p->firmware.assign(0x40000, 0xFF);
    p->firmware_id = 0x1234;
    p->fw_page_dirty.assign(p->firmware.size() / NDS::FW_PAGE, 0);
    p->fw_dirty_pages = 0;
    return p;
  };
  const char* path = "test_fw_override.ovr";
  std::string err;

  {
    auto nds = fresh();
    // Write "Hi" at 0x3FE06 the way the firmware does: WREN, then a page
    // write with a three-byte address.
    fw_spi(*nds, 0x06, false);                      // WREN
    const u8 seq[] = {0x0A, 0x03, 0xFE, 0x06, 'H', 'i'};
    for (size_t i = 0; i < sizeof seq; ++i) fw_spi(*nds, seq[i], i + 1 < sizeof seq);
    CHECK_EQ(nds->firmware[0x3FE06], 'H');
    CHECK_EQ(nds->firmware[0x3FE07], 'i');
    CHECK_EQ(nds->firmware_override_dirty(), true);
    CHECK_EQ(nds->save_firmware_override(path, err), true);
  }
  {
    // Without WREN nothing is written and no page is marked.
    auto nds = fresh();
    const u8 seq[] = {0x0A, 0x03, 0xFE, 0x06, 'X'};
    for (size_t i = 0; i < sizeof seq; ++i) fw_spi(*nds, seq[i], i + 1 < sizeof seq);
    CHECK_EQ(nds->firmware[0x3FE06], 0xFFu);
    CHECK_EQ(nds->firmware_override_dirty(), false);
  }
  {
    // Round trip: the sidecar carries the changed page and nothing else.
    auto nds = fresh();
    CHECK_EQ(nds->load_firmware_override(path, err), true);
    CHECK_EQ(err.empty(), true);                    // same dump, so no warning
    CHECK_EQ(nds->firmware[0x3FE06], 'H');
    CHECK_EQ(nds->firmware[0x3FE07], 'i');
    CHECK_EQ(nds->firmware[0x3FD00], 0xFFu);        // a page it never touched
    // A loaded page stays dirty, so a session that changes nothing still
    // writes the settings from the session before it back out.
    CHECK_EQ(nds->firmware_override_dirty(), true);
  }
  {
    // A sidecar made from a different dump warns but still applies: retail
    // firmwares put the settings in the same place, and refusing to boot
    // because someone re-dumped their console would be the worse failure.
    auto nds = fresh();
    nds->firmware_id = 0x9999;
    CHECK_EQ(nds->load_firmware_override(path, err), true);
    CHECK_EQ(err.empty(), false);
    CHECK_EQ(nds->firmware[0x3FE06], 'H');
  }
  {
    // A firmware of another size is refused outright.
    auto nds = fresh();
    nds->firmware.assign(0x20000, 0xFF);
    nds->fw_page_dirty.assign(nds->firmware.size() / NDS::FW_PAGE, 0);
    CHECK_EQ(nds->load_firmware_override(path, err), false);
  }
  std::remove(path);
}

// ---- power off, and the power-lost bit across a reboot ---------------------
// The ARM7 shuts the console down by setting bit 6 of power-management
// register 0. The firmware does it on the way out of its settings pages,
// which is how a frontend knows the settings it just wrote are complete.
static void pmic_write(NDS& nds, u8 reg, u8 value) {
  nds.bus.dma_write16(Cpu::ARM7, 0x040001C0, 0x8800);        // enabled, device 0, hold
  nds.bus.dma_write8(Cpu::ARM7, 0x040001C2, reg);            // command: write this register
  settle(nds);
  nds.bus.dma_write16(Cpu::ARM7, 0x040001C0, 0x8000);        // last byte, drop the select
  nds.bus.dma_write8(Cpu::ARM7, 0x040001C2, value);
  settle(nds);
}

// The RTC is bit-banged a bit at a time on 0x04000138: CS high to start,
// then a value per clock edge, the host driving SIO for a write.
static void rtc_send(NDS& nds, u8 v) {
  for (int i = 0; i < 8; ++i) {
    const u16 base = static_cast<u16>(0x0004 | 0x0010 | ((v >> i) & 1));
    nds.bus.dma_write16(Cpu::ARM7, 0x04000138, static_cast<u16>(base | 0x0002));   // clock high
    nds.bus.dma_write16(Cpu::ARM7, 0x04000138, base);                              // clock low: shift
  }
}

static u8 rtc_recv(NDS& nds) {
  u8 v = 0;
  for (int i = 0; i < 8; ++i) {
    nds.bus.dma_write16(Cpu::ARM7, 0x04000138, 0x0006);
    nds.bus.dma_write16(Cpu::ARM7, 0x04000138, 0x0004);
    if (nds.bus.dma_read16(Cpu::ARM7, 0x04000138) & 1) v |= static_cast<u8>(1u << i);
  }
  return v;
}

// Command 0x86: read (bit 7), register 0 (bits 6-4) = status1, 6 in the low
// nibble as the chip requires.
static u8 read_status1(NDS& nds) {
  nds.bus.dma_write16(Cpu::ARM7, 0x04000138, 0x0000);
  nds.bus.dma_write16(Cpu::ARM7, 0x04000138, 0x0004);   // CS rising: start
  rtc_send(nds, 0x86);
  return rtc_recv(nds);
}

static void test_generated_firmware() {
  // A generated firmware: valid checksums, the settings asked for, and no
  // persistence (nothing on disk to persist against).
  ds::bios::UserSettings user;
  user.nickname = "Player"; user.birthday_month = 7; user.birthday_day = 23; user.favourite_colour = 10; user.language = 2;
  auto nds = std::make_unique<NDS>();
  CHECK_EQ(nds->load_bios("", "", "", user), true);
  CHECK_EQ(nds->bios_native, false);
  CHECK_EQ(nds->firmware_synthetic, true);
  CHECK_EQ(nds->can_boot_firmware(), false);
  CHECK_EQ(nds->firmware.size(), 0x40000u);
  const u32 off = static_cast<u32>(nds->firmware[0x20] | (nds->firmware[0x21] << 8)) << 3;
  CHECK_EQ(off, 0x3FE00u);
  const u8* u = nds->firmware.data() + off;
  CHECK_EQ(u[0x03], 7u); CHECK_EQ(u[0x04], 23u); CHECK_EQ(u[0x02], 10u);
  CHECK_EQ(u[0x06], 'P'); CHECK_EQ(u[0x08], 'l'); CHECK_EQ(u[0x1A], 6u);
  CHECK_EQ(u[0x64] & 7, 2u);
  CHECK_EQ(static_cast<u32>(u[0x72] | (u[0x73] << 8)), static_cast<u32>(ds::bios::crc16(u, 0x70, 0xFFFF)));
  CHECK_EQ(std::memcmp(u, u + 0x100, 0x100), 0);
  // The FreeBIOS vectors are in place: the SWI vector at 8 is a branch.
  CHECK_EQ(nds->bus.bios9.get()[0x0B], 0xEAu);
  CHECK_EQ(nds->bus.bios7.get()[0x0B], 0xEAu);
  // A page write lands but marks nothing dirty.
  fw_spi(*nds, 0x06, false);
  const u8 seq[] = {0x0A, 0x03, 0xFE, 0x06, 'Q'};
  for (size_t i = 0; i < sizeof seq; ++i) fw_spi(*nds, seq[i], i + 1 < sizeof seq);
  CHECK_EQ(nds->firmware[0x3FE06], 'Q');
  CHECK_EQ(nds->firmware_override_dirty(), false);
  std::string err;
  CHECK_EQ(nds->load_firmware_override("nonexistent.ovr", err), false);
  // A path naming no file falls back; a file that exists but is wrong fails.
  CHECK_EQ(nds->load_bios("/nonexistent/bios9.bin", "/nonexistent/bios7.bin", "/nonexistent/firmware.bin", user), true);
  CHECK_EQ(nds->bios_native, false);
  CHECK_EQ(nds->load_bios("/dev/null", "/dev/null", "", user, &err), false);
  CHECK_EQ(err.empty(), false);
  CHECK_EQ(nds->load_bios("/dev/null", "/nonexistent/bios7.bin", "", user, &err), false);   // one half only
}

static void test_power_off() {
  auto nds = std::make_unique<NDS>();
  CHECK_EQ(nds->power_off, false);
  pmic_write(*nds, 0x00, 0x0D);                  // backlights and the amplifier: not a shutdown
  CHECK_EQ(nds->power_off, false);
  pmic_write(*nds, 0x00, 0x4D);                  // bit 6: the power line drops
  CHECK_EQ(nds->power_off, true);
  nds->reset();
  CHECK_EQ(nds->power_off, false);               // a reboot starts with the line up

  // status1 bit 7 says the clock lost power, and it is what sends the
  // firmware into its first-boot setup wizard. The chip clears it when the
  // console reads it, and only a flat battery sets it again -- so a reboot
  // (which is what reset() is) must not bring it back, or every power-off
  // would land in the wizard again.
  CHECK_EQ(read_status1(*nds) & 0x80, 0x80u);    // first boot: reported once
  CHECK_EQ(read_status1(*nds) & 0x80, 0u);       // and cleared by that read
  nds->reset();
  CHECK_EQ(read_status1(*nds) & 0x80, 0u);       // still clear after the reboot

  // A fresh console is a fresh battery.
  auto cold = std::make_unique<NDS>();
  CHECK_EQ(read_status1(*cold) & 0x80, 0x80u);
}

int main() {
  test_div();
  test_sqrt();
  test_vramcnt_layout();
  test_gxstat_irq();
  test_input();
  test_mic_and_lid();
  test_input_log();
  test_rtc_clock();
  test_firmware_override();
  test_generated_firmware();
  test_power_off();
  if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
  std::puts("io: ok");
  return 0;
}
