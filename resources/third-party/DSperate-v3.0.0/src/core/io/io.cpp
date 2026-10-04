// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// I/O register file for both CPUs. Register semantics per GBATEK; melonDS is
// the behavioural reference where GBATEK is silent.
#include "core/io/io.h"
#include "core/io/dsi_nwifi.h"
#include "core/state/state.h"
#include "core/nds.h"
#include "core/dma/dma.h"
#include "core/dma/ndma.h"
#include "core/cpu/timing_mode.h"

#include <algorithm>
#include <cstdint>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>
#include <algorithm>

namespace ds::io {

namespace {
inline int ci(Cpu c) { return static_cast<int>(c); }
inline Cpu other(Cpu c) { return c == Cpu::ARM9 ? Cpu::ARM7 : Cpu::ARM9; }
}

Io::Io(NDS& nds) : aes(nds), dsp(nds), sd(nds, 0), sdio(nds, 1), cam(nds), wifi(nds), nds_(nds) { reset(); }

static void ev_lcd_irq(NDS& nds, u32) { nds.io.flush_lcd_irq(); }
void Io::lcd_irq(Cpu cpu, u32 bit) {
  if (lcd_irq_delay == 0) { request_irq(cpu, bit); return; }
  lcd_irq_pending[ci(cpu)] |= 1u << bit;
  nds_.sched.schedule(EventId::LcdIrq, nds_.sched.now() + lcd_irq_delay, ev_lcd_irq);
}
void Io::flush_lcd_irq() {
  for (int i = 0; i < 2; ++i) for (u32 m = lcd_irq_pending[i]; m; m &= m - 1) request_irq(static_cast<Cpu>(i), static_cast<u32>(__builtin_ctz(m)));
  lcd_irq_pending[0] = lcd_irq_pending[1] = 0;
}

void Io::reset() {
  lcd_irq_pending[0] = lcd_irq_pending[1] = 0;
  // DSi raises LCD IRQs at the DISPSTAT event itself; DS delays two bus cycles.
  lcd_irq_delay = nds_.dsi ? 0 : 4;
  cpu_io[0] = CpuIo{}; cpu_io[1] = CpuIo{};
  dispstat[0] = dispstat[1] = 0; vcount = 0;
  wramcnt = 0; std::memset(vramcnt, 0, sizeof vramcnt);
  powcnt1 = 0; powcnt2 = 0; math = MathUnit{};
  keyinput = 0x03FF; extkeyin = 0x007F; keycnt[0] = keycnt[1] = 0;
  exmemcnt = 0x6000; rcnt = 0; wifiwaitcnt = 0;   // boot2's ARM7 spins on these bits
  spicnt = 0; spidata = 0; spi_ready_at = 0; spi_busy_ = false; spi_flag_mode_ = false;
  spi_fw = SpiFirmware{}; spi_tsc = SpiTouch{}; spi_pm = SpiPower{};
  mic_ = nullptr; mic_count_ = 0; mic_start_ = 0;
  rtc = Rtc{};
  if (rtc_host_clock_) rtc_seed();
  else if (rtc_power_lost_seen_) rtc.status1 = 0x02;   // a reboot, not a flat battery
  cart = Cart{};
  wifi.reset();
  dsi = DsiIo{};
  if (nds_.dsi) dsi_reset();
}

// ---- IRQ ------------------------------------------------------------------
void Io::update_irq(Cpu cpu) {
  CpuIo& c = cpu_io[ci(cpu)];
  CpuContext& ctx = nds_.cpu(cpu);
  bool any = (c.ie & c.if_) != 0;
  if (cpu == Cpu::ARM7 && nds_.dsi && (dsi.ie2 & dsi.if2)) any = true;   // DSi's second IE/IF pair
  const u32 pending = ((c.ime & 1) && any) ? 1 : 0;
  // Raised off-slice: taken after the CPU's next instruction (or before any,
  // if halted). A CPU stopped by its own DMA counts as off-slice too.
  if (nds_.dsi && pending && !ctx.hot.irq_pending && (nds_.sched.running() != &ctx || nds_.sched.in_dma()) && !ctx.halted) ctx.irq_offline = true;
  ctx.hot.irq_pending = pending;
  // Halt exits on IE&IF; the ARM9 additionally needs IME.
  if (any && ctx.halted && (cpu == Cpu::ARM7 || (c.ime & 1))) ctx.halted = false;
}


void Io::request_irq2(u32 bit) { dsi.if2 |= 1u << bit; update_irq(Cpu::ARM7); }

void Io::request_irq(Cpu cpu, u32 bit) {
  CpuIo& c = cpu_io[ci(cpu)];
  // A level source (GX FIFO) re-requests every poll; skip the redundant
  // recompute when the bit's already set and the CPU is running.
  if ((c.if_ & (1u << bit)) && !nds_.cpu(cpu).halted) return;
  c.if_ |= (1u << bit);
  update_irq(cpu);
}

// ---- display status -------------------------------------------------------
void Io::set_vcount(u16 line) {
  vcount = line;
  for (int i = 0; i < 2; ++i) {
    const u16 target = static_cast<u16>((dispstat[i] >> 8) | ((dispstat[i] & 0x80) << 1));
    if (line == target) {
      dispstat[i] |= 4;
      if (dispstat[i] & 0x20) lcd_irq(static_cast<Cpu>(i), IRQ_VCOUNT);
    } else {
      dispstat[i] &= ~4;
    }
  }
}
void Io::set_hblank(bool on) {
  for (int i = 0; i < 2; ++i) {
    if (on) { dispstat[i] |= 2; if (dispstat[i] & 0x10) lcd_irq(static_cast<Cpu>(i), IRQ_HBLANK); }
    else dispstat[i] &= ~2;
  }
}
void Io::set_vblank(bool on) {
  for (int i = 0; i < 2; ++i) {
    if (on) { dispstat[i] |= 1; if (dispstat[i] & 0x08) lcd_irq(static_cast<Cpu>(i), IRQ_VBLANK); }
    else dispstat[i] &= ~1;
  }
}

// ---- IPC ------------------------------------------------------------------
void Io::ipc_sync_write(Cpu cpu, u16 value) {
  static const bool log = std::getenv("DS_IPC_LOG") != nullptr;
  if (log) std::fprintf(stderr, "[ipcsync] %s out=%x irq=%d t=%llu frame %llu line %u pc %08x\n", cpu == Cpu::ARM9 ? "arm9" : "arm7", (value >> 8) & 0xF, (value >> 13) & 1,
                        (unsigned long long)nds_.sched.now(), (unsigned long long)nds_.frame_count, nds_.gpu.line(), nds_.cpu(cpu).hot.regs[15]);
  CpuIo& me = cpu_io[ci(cpu)];
  CpuIo& them = cpu_io[ci(other(cpu))];
  // DSi-loader handshake: ARM7's IPCSYNC 0 applies the RAM size ARM9
  // requested via SCFG_EXT.
  if (nds_.dsi && cpu == Cpu::ARM7 && !(value & 0x0F00) && ((dsi.scfg_ext[0] ^ dsi.scfg_ext[1]) & 0xC000u)) dsi_apply_ram_size();
  me.ipc_sync = (me.ipc_sync & 0x000F) | (value & 0x4F00);
  them.ipc_sync = (them.ipc_sync & 0x4F00) | ((value >> 8) & 0xF);
  if ((value & 0x2000) && (them.ipc_sync & 0x4000)) request_irq(other(cpu), IRQ_IPC_SYNC);
  nds_.sched.yield(nds_.cpu(cpu));   // other CPU may be spinning on this (SDK boot handshake)
}

u16 Io::ipc_fifo_cnt_read(Cpu cpu) {
  CpuIo& me = cpu_io[ci(cpu)];
  CpuIo& them = cpu_io[ci(other(cpu))];
  u16 v = me.ipc_fifo_cnt & 0x8404;
  if (me.fifo_out.empty()) v |= 0x0001;
  if (me.fifo_out.full())  v |= 0x0002;
  if (them.fifo_out.empty()) v |= 0x0100;
  if (them.fifo_out.full())  v |= 0x0200;
  if (me.ipc_fifo_cnt & 0x4000) v |= 0x4000;
  return v;
}

void Io::ipc_fifo_cnt_write(Cpu cpu, u16 value) {
  CpuIo& me = cpu_io[ci(cpu)];
  CpuIo& them = cpu_io[ci(other(cpu))];
  const bool was_send_irq = me.ipc_fifo_cnt & 0x0004;
  const bool was_recv_irq = me.ipc_fifo_cnt & 0x0400;
  if (value & 0x0008) { me.fifo_out.clear(); }        // flush send FIFO
  if (value & 0x4000) me.ipc_fifo_cnt &= ~0x4000;      // ack error
  me.ipc_fifo_cnt = (me.ipc_fifo_cnt & 0x4000) | (value & 0x8404);
  if (!was_send_irq && (value & 0x0004) && me.fifo_out.empty()) request_irq(cpu, IRQ_IPC_SEND_EMPTY);
  if (!was_recv_irq && (value & 0x0400) && !them.fifo_out.empty()) request_irq(cpu, IRQ_IPC_RECV);
  if (value & 0x0008) {
    if (me.ipc_fifo_cnt & 0x0004) request_irq(cpu, IRQ_IPC_SEND_EMPTY);
  }
}

void Io::ipc_fifo_send(Cpu cpu, u32 value) {
  CpuIo& me = cpu_io[ci(cpu)];
  CpuIo& them = cpu_io[ci(other(cpu))];
  if (!(me.ipc_fifo_cnt & 0x8000)) return;
  if (me.fifo_out.full()) { me.ipc_fifo_cnt |= 0x4000; return; }
  const bool was_empty = me.fifo_out.empty();
  static const bool log = std::getenv("DS_IPC_LOG") != nullptr;
  if (log) std::fprintf(stderr, "[ipcfifo] %s send %08x t=%llu frame %llu line %u pc %08x\n", cpu == Cpu::ARM9 ? "arm9" : "arm7", value, (unsigned long long)nds_.sched.now(), (unsigned long long)nds_.frame_count, nds_.gpu.line(), nds_.cpu(cpu).hot.regs[15]);
  me.fifo_out.push(value);
  if (was_empty && (them.ipc_fifo_cnt & 0x0400)) request_irq(other(cpu), IRQ_IPC_RECV);
}

u32 Io::ipc_fifo_recv(Cpu cpu) {
  CpuIo& me = cpu_io[ci(cpu)];
  CpuIo& them = cpu_io[ci(other(cpu))];
  if (them.fifo_out.empty()) { me.ipc_fifo_cnt |= 0x4000; return them.fifo_out.last; }
  if (!(me.ipc_fifo_cnt & 0x8000)) return them.fifo_out.data[them.fifo_out.head];
  u32 v = them.fifo_out.pop();
  if (them.fifo_out.empty() && (them.ipc_fifo_cnt & 0x0004)) request_irq(other(cpu), IRQ_IPC_SEND_EMPTY);
  return v;
}

// ---- timers ---------------------------------------------------------------
static void timer_event(NDS& nds, u32 param) {
  nds.io.timer_overflow(static_cast<Cpu>(param >> 2), static_cast<int>(param & 3));
}

u16 Io::timer_value(Cpu cpu, int idx) {
  Timer& t = cpu_io[ci(cpu)].timers[idx];
  if (!t.running() || t.count_up()) return t.counter;
  u64 elapsed = (nds_.sched.now() - t.start_time) >> 1;   // 33 MHz system clock
  u64 ticks = elapsed >> t.prescaler_shift();
  u64 v = t.counter + ticks;
  return static_cast<u16>(v);   // overflow handling happens in the event
}

void Io::timer_schedule(Cpu cpu, int idx) {
  Timer& t = cpu_io[ci(cpu)].timers[idx];
  const EventId id = static_cast<EventId>(static_cast<int>(cpu == Cpu::ARM9 ? EventId::Timer0 : EventId::Timer7_0) + idx);
  if (!t.running() || t.count_up()) { nds_.sched.cancel(id); return; }
  u64 ticks_left = 0x10000 - t.counter;
  u64 cycles = (ticks_left << t.prescaler_shift()) << 1;   // system -> ARM9 cycles
  nds_.sched.schedule(id, t.start_time + cycles, timer_event, (static_cast<u32>(cpu) << 2) | idx);   // from the sample point, exact
}

void Io::timer_overflow(Cpu cpu, int idx) {
  Timer& t = cpu_io[ci(cpu)].timers[idx];
  t.counter = t.reload;
  t.start_time = nds_.sched.event_time();   // the overflow's nominal time, not the (late) slice end
  static const bool dbg = std::getenv("DS_DEBUG_STATE") != nullptr;
  static int dbg_left = 12;
  if (dbg && dbg_left > 0 && cpu == Cpu::ARM7) { --dbg_left;
    std::fprintf(stderr, "[state] arm7 timer%d overflow at %llu (now %llu) control %04x reload %04x\n", idx,
                 (unsigned long long)t.start_time, (unsigned long long)nds_.sched.now(), t.control, t.reload); }
  if (t.control & 0x40) request_irq(cpu, IRQ_TIMER0 + idx);
  if (idx < 3) {
    Timer& n = cpu_io[ci(cpu)].timers[idx + 1];
    if (n.running() && n.count_up()) {
      if (++n.counter == 0) timer_overflow(cpu, idx + 1);
    }
  }
  timer_schedule(cpu, idx);
}

void Io::timer_write_control(Cpu cpu, int idx, u16 value) {
  Timer& t = cpu_io[ci(cpu)].timers[idx];
  const bool was = t.running();
  const u64 now = nds_.sched.now();
  // A running timer keeps its prescaler phase across a control rewrite (only
  // restarts if the start bit goes 0 -> 1); remainder re-expressed in the
  // new prescaler's ticks.
  u64 rem = 0;                                    // system cycles into the current tick
  const u32 old_shift = t.prescaler_shift();
  if (was && !t.count_up()) {
    const u64 elapsed = (now - t.start_time) >> 1;
    const u64 ticks = elapsed >> old_shift;
    rem = elapsed - (ticks << old_shift);
    t.counter = static_cast<u16>(t.counter + ticks);
  }
  t.control = value & 0xC7;
  const u32 new_shift = t.prescaler_shift();
  if (new_shift != old_shift) rem = (rem << (10 - old_shift)) >> (10 - new_shift);
  t.start_time = now - (rem << 1);
  if (!was && t.running()) { t.counter = t.reload; t.start_time = now; }
  timer_schedule(cpu, idx);
  static const bool dbg = std::getenv("DS_DEBUG_TIMER") != nullptr;   // DS_DEBUG_TIMER=1: every control write
  if (dbg) std::fprintf(stderr, "[timer] arm%d t%d ctl %04x (was %d rem %llu) reload %04x counter %04x at %llu overflow at %llu\n", cpu == Cpu::ARM9 ? 9 : 7, idx, t.control, was ? 1 : 0, (unsigned long long)rem, t.reload, t.counter, (unsigned long long)t.start_time,
                        (unsigned long long)(t.running() && !t.count_up() ? t.start_time + (((0x10000ull - t.counter) << t.prescaler_shift()) << 1) : 0));
}

// ---- SPI (ARM7) -----------------------------------------------------------
static void spi_event(NDS& nds, u32) { nds.io.spi_done(); }

void Io::spi_done() {
  spi_busy_ = false;
  if (spicnt & 0x4000) request_irq(Cpu::ARM7, IRQ_SPI);
}

u64 Io::nds_sched_now() const { return nds_.sched.now(); }

u8 Io::spi_transfer(u8 value) {
  const int dev = (spicnt >> 8) & 3;
  switch (dev) {
  case 0: {                                             // power management
    SpiPower& p = spi_pm;
    if (!p.hold) { p.hold = true; p.cmd = value; p.pos = 1; p.data = 0; return 0; }
    const u32 reg = p.cmd & 0x7F;
    if (p.cmd & 0x80) p.data = (reg < 8) ? p.regs[reg] : 0;
    else {
      if (reg < 8) p.regs[reg] = value;
      if (reg == 0 && (value & 0x40)) nds_.power_off = true;   // system power line; frontend decides what it means
      // DSi register 0x10 bit 0: reset. A DS-mode title's only way back to
      // the DSi Menu (no I2C reset access); same soft reset as bptwl_write.
      if (reg == 0x10 && (value & 0x01) && nds_.dsi) {
        nds_.dsi_soft_reset_pending = true;
        nds_.cpu(Cpu::ARM7).halted = true;
      }
      static const bool log = std::getenv("DS_MIC_LOG") != nullptr;
      if (log && (reg == 2 || reg == 3)) std::fprintf(stderr, "[mic] PMIC reg %u = %02x (frame %llu)\n", reg, value, (unsigned long long)nds_.frame_count);
      p.data = 0;
    }
    return p.data;
  }
  case 1: {                                             // firmware flash
    SpiFirmware& f = spi_fw;
    if (!f.hold) {
      f.hold = true; f.cmd = value; f.pos = 1; f.data = 0;
      if (value == 0x06) f.status |= 2;
      if (value == 0x04) f.status &= ~2;
      f.addr = 0;
      return 0;
    }
    switch (f.cmd) {
    case 0x03:                                          // read
      if (f.pos < 4) { f.addr = (f.addr << 8) | value; f.data = 0; }
      else {
        const auto& fw = nds_.firmware;
        f.data = fw.empty() ? 0xFF : fw[f.addr % fw.size()];
        f.addr++;
      }
      f.pos++;
      return f.data;
    case 0x05: return f.status;                         // read status
    case 0x9F: {                                        // JEDEC id
      static const u8 id[3] = {0x20, 0x40, 0x12};
      f.data = (f.pos >= 1 && f.pos <= 3) ? id[f.pos - 1] : 0;
      f.pos++;
      return f.data;
    }
    case 0x0A: {                                        // page write (WEL set): into the in-memory image
      auto& fw = nds_.firmware;
      if (f.pos < 4) { f.addr = (f.addr << 8) | value; f.data = 0; }
      else {
        if ((f.status & 2) && !fw.empty()) {
          const u32 off = f.addr % fw.size();
          if (fw[off] != value) { fw[off] = value; nds_.firmware_written(off); }
        }
        f.data = value;
        f.addr++;
      }
      f.pos++;
      return f.data;
    }
    default:
      return 0;
    }
  }
  case 2: {                                             // touchscreen
    SpiTouch& t = spi_tsc;
    if (nds_.dsi && t.dsi_mode) return dsi_tsc_transfer(value);
    if (value & 0x80) {                                 // control byte
      t.cmd = value; t.pos = 0;
      const u32 channel = (value >> 4) & 7;
      u16 sample = 0;
      switch (channel) {
      case 1: sample = t.y; break;        // Y
      case 5: sample = t.x; break;        // X
      case 6: sample = mic_sample(); break;   // AUX: the microphone
      default: sample = 0; break;
      }
      // Bit 3 selects an 8-bit conversion (one byte less per sample, used by
      // mic sampling loops). Result stream starts one clock after the
      // control byte, MSB first: 12-bit = 11..5 then 4..0<<3, 8-bit = 7..1
      // then bit 0<<7.
      if (value & 0x08) sample = static_cast<u16>((sample >> 4) << 4);
      t.sample = sample;
      t.data = static_cast<u8>(t.sample >> 5);          // first byte: bits 11:5
      t.pos = 1;
      return 0;
    }
    if (t.pos == 1) { t.pos = 2; return t.data; }
    t.pos = 0;
    return static_cast<u8>((t.sample << 3) & 0xF8);
  }
  default:
    return 0;
  }
}

// ---- input ----

void Io::set_buttons(u32 pressed) {
  keyinput = static_cast<u16>(0x03FF & ~(pressed & 0x03FF));
  const u16 xy = static_cast<u16>(((pressed >> BTN_X) & 1) | (((pressed >> BTN_Y) & 1) << 1));
  extkeyin = static_cast<u16>((extkeyin & ~0x0003u) | (~xy & 0x0003u));
  update_key_irq();
}

void Io::set_touch(int x, int y, bool down) {
  x = x < 0 ? 0 : (x > 255 ? 255 : x);
  y = y < 0 ? 0 : (y > 191 ? 191 : y);
  if (nds_.dsi && spi_tsc.dsi_mode) {
    // DSi CODEC: pen state lives in bank 3 registers 0x09/0x0E plus a
    // "changed" flag on the next sample.
    SpiTouch& t = spi_tsc;
    const u8 old_up = t.dsi_bank3[0x0E] & 1;
    if (!down) { t.dsi_tx = 0x7000; t.dsi_ty = 0x7000; t.dsi_bank3[0x09] = 0x40; t.dsi_bank3[0x0E] |= 1; }
    else { t.dsi_tx = static_cast<u16>(x << 4); t.dsi_ty = static_cast<u16>(y << 4); t.dsi_bank3[0x09] = 0x80; t.dsi_bank3[0x0E] &= ~1; }   // 12-bit ADC units: pixel << 4; bit 15 is the pen-changed flag
    if (old_up ^ (t.dsi_bank3[0x0E] & 1)) { t.dsi_tx |= 0x8000; t.dsi_ty |= 0x8000; }
    return;
  }
  if (!down) {                       // release values; games test bit 6
    spi_tsc.x = 0; spi_tsc.y = 0xFFF;
    extkeyin |= 1u << 6;
    return;
  }
  spi_tsc.x = static_cast<u16>(x << 4);
  spi_tsc.y = static_cast<u16>(y << 4);
  extkeyin &= ~(1u << 6);
}

void Io::set_lid(bool closed) {
  const bool was = lid_closed();
  if (closed) extkeyin |= 1u << 7; else extkeyin &= ~(1u << 7);
  if (was && !closed) request_irq(Cpu::ARM7, IRQ_LID);
}

void Io::set_net_driver(NetDriver* net) {
  wifi.set_net_driver(net);
  if (NWifi* n = sdio.nwifi()) n->set_net_driver(net);
}

s16 Io::mic_at(u64 t) const {
  if (mic_count_ == 0) return 0;
  const u64 elapsed = t > mic_start_ ? t - mic_start_ : 0;
  size_t i = static_cast<size_t>((elapsed * mic_count_) / CYCLES_PER_FRAME);
  if (i >= mic_count_) i = mic_count_ - 1;
  return mic_[i];
}

// ---- DSi microphone ----

u16 Io::dsi_mic_read_cnt() const {
  u16 v = dsi.mic_cnt;
  if (dsi.mic_level == 0) v |= 1 << 8;
  if (dsi.mic_level >= 8) v |= 1 << 9;
  if (dsi.mic_level == 16) v |= 1 << 10;
  return v;
}

void Io::dsi_mic_write_cnt(u16 value, u16 mask) {
  nds_.spu.catch_up();
  static const bool log = std::getenv("DS_MIC_LOG") != nullptr;
  if (log) std::fprintf(stderr, "[mic] MIC_CNT %04x -> %04x (mask %04x), FIFO %u, frame %llu\n", dsi.mic_cnt, value, mask, dsi.mic_level, (unsigned long long)nds_.frame_count);
  if (dsi.mic_cnt & 0x8000) mask &= ~0x100F;   // format/rate/FIFO clear only take while stopped
  const u16 v = static_cast<u16>((value & mask) | (dsi.mic_cnt & ~mask));
  if (v & (1 << 12)) {
    dsi.mic_cnt &= ~(1 << 11);
    dsi.mic_rd = dsi.mic_wr = dsi.mic_level = 0;
    dsi.mic_temp = 0;
    dsi.mic_temp_count = 0;
  }
  if ((v ^ dsi.mic_cnt) & 0x8000 && (v & 0x8000)) {
    dsi.mic_divider = 0;
    dsi.mic_temp_count = 0;
    mic_used_ = true;   // triggers the frontend to open the capture device
  }
  dsi.mic_cnt = static_cast<u16>((v & 0xE00F) | (dsi.mic_cnt & (1 << 11)));
}

// Every read, of any width, takes a word; an empty FIFO repeats the last one.
u32 Io::dsi_mic_read_data() {
  nds_.spu.catch_up();
  if (dsi.mic_level == 0) return dsi.mic_fifo[(dsi.mic_rd + 15) & 15];
  const u32 v = dsi.mic_fifo[dsi.mic_rd];
  dsi.mic_rd = (dsi.mic_rd + 1) & 15;
  dsi.mic_level--;
  return v;
}

// Each I2S sample arrives twice. Format 0 keeps both, 1 and 2 keep one of the
// pair (making a word from two clocks), 3 keeps none; the rate field divides
// the clocks, format 2 taking the last of each divided run.
void Io::dsi_mic_clock(s16 sample) {
  DsiIo& d = dsi;
  if (!(d.mic_cnt & 0x8000) || (d.mic_cnt & (1 << 11))) return;
  const u8 mode = d.mic_cnt & 3, rate = (d.mic_cnt >> 2) & 3;
  if (mode == 3) return;
  const bool capture = d.mic_divider == (mode == 2 ? rate : 0);
  d.mic_divider = d.mic_divider >= rate ? 0 : d.mic_divider + 1;
  if (capture) {
    u32 v;
    if (mode == 0) {
      v = static_cast<u16>(sample);
      v |= v << 16;
    } else {
      if (!d.mic_temp_count) { d.mic_temp = sample; d.mic_temp_count = 1; return; }
      v = static_cast<u16>(d.mic_temp) | (static_cast<u32>(static_cast<u16>(sample)) << 16);
    }
    if (d.mic_level == 16) {
      d.mic_cnt |= 1 << 11;
    } else {
      d.mic_fifo[d.mic_wr] = v;
      d.mic_wr = (d.mic_wr + 1) & 15;
      d.mic_level++;
    }
    d.mic_temp_count = 0;
  }
  // The IRQ conditions are not exclusive; bit 14 is overrun, not full.
  if (d.mic_cnt & (1 << 11)) {
    if (d.mic_cnt & (1 << 14)) request_irq2(IRQ2_MIC_EXT);
  } else if (d.mic_level == 8) {
    if (d.mic_cnt & (1 << 13)) request_irq2(IRQ2_MIC_EXT);
    nds_.ndma.check(Cpu::ARM7, 0x2C);
  }
}

void Io::set_mic(const s16* samples, size_t count) {
  mic_ = samples; mic_count_ = count; mic_start_ = nds_.sched.now();
}

// AUX sits behind the PMIC mic amplifier: register 3 bits 0-1 pick the gain
// (x20/40/80/160); frontend samples assumed at x20, scaled/clipped like the
// ADC would. Register 2 bit 0 (amplifier enable) not honoured.
u16 Io::mic_sample() const {
  mic_used_ = true;
  static const bool log = std::getenv("DS_MIC_LOG") != nullptr;
  if (log) {
    static u64 reads = 0, last_frame = 0; static int peak = 0;
    ++reads;
    if (nds_.frame_count - last_frame >= 60) {
      std::fprintf(stderr, "[mic] %llu AUX reads in 60 frames, peak %d, buffer %zu samples, gain x%d (frame %llu)\n",
                   (unsigned long long)reads, peak, mic_count_, 20 << (spi_pm.regs[3] & 3), (unsigned long long)nds_.frame_count);
      reads = 0; peak = 0; last_frame = nds_.frame_count;
    }
    for (size_t k = 0; k < mic_count_; ++k) if (std::abs(int(mic_[k])) > peak) peak = std::abs(int(mic_[k]));
  }
  if (mic_count_ == 0) return 0x800;
  const u64 elapsed = nds_.sched.now() - mic_start_;
  size_t i = static_cast<size_t>((elapsed * mic_count_) / CYCLES_PER_FRAME);
  if (i >= mic_count_) i = mic_count_ - 1;
  const int gain = 1 << (spi_pm.regs[3] & 3);
  int v = 0x800 + ((static_cast<int>(mic_[i]) * gain) >> 4);
  return static_cast<u16>(v < 0 ? 0 : (v > 0xFFF ? 0xFFF : v));
}

// KEYCNT: bits 0-9 select keys, bit 14 enables the IRQ, bit 15 picks the
// condition (0 = any selected key held, 1 = all of them).
void Io::update_key_irq() {
  const u16 held = static_cast<u16>(~keyinput & 0x03FF);
  for (int c = 0; c < 2; ++c) {
    const u16 cnt = keycnt[c];
    if (!(cnt & 0x4000)) continue;
    const u16 sel = cnt & 0x03FF;
    const bool fire = (cnt & 0x8000) ? (sel != 0 && (held & sel) == sel) : ((held & sel) != 0);
    if (fire) request_irq(static_cast<Cpu>(c), IRQ_KEYPAD);
  }
}

void Io::spi_release() {
  spi_pm.hold = false; spi_fw.hold = false; spi_fw.addr = 0; spi_tsc.pos = 0; spi_tsc.dsi_pos = 0;
}

u16 Io::spicnt_read_arm7() {
  if (!spi_busy()) { spi_poll_streak_ = 0; return spicnt_read(); }
  if (++spi_poll_streak_ >= SPI_POLL_STREAK && nds_.sched.idle_skip_enabled()) {
    CpuContext& a7 = nds_.cpu(Cpu::ARM7);
    if (nds_.sched.running() == &a7 && a7.hot.cycle_budget > 0) {
      // ARM9 cycles left until ready, converted to ARM7 cycles, capped at the
      // slice: an underflowed remainder would hand back budget instead of
      // consuming it, spinning with time frozen.
      const u64 now = nds_.sched.now();
      if (spi_ready_at > now) {
        const u64 rem9 = spi_ready_at - now;
        s32 charge = static_cast<s32>((rem9 + 1) / 2);
        if (charge > a7.hot.cycle_budget) charge = a7.hot.cycle_budget;
        a7.hot.cycle_budget -= charge;
        prof::add(prof::C_CYC_A7_SPI_SLEPT, static_cast<u64>(charge) * 2);
        prof::add(prof::C_A7_SPI_SLEEP, 1);
      }
    }
  }
  return spicnt_read();
}

void Io::spi_write_data(u8 value) {
  if (!(spicnt & 0x8000)) return;
  if (spi_busy()) return;
  spidata = spi_transfer(value);
  if (!(spicnt & 0x0800)) spi_release();
  const u32 delay = 8 * (8u << (spicnt & 3));
  spi_ready_at = nds_.sched.now() + delay * 2;
  static const bool dbg = std::getenv("DS_DEBUG_SPI") != nullptr;   // DS_DEBUG_SPI=1: every transfer's ready time
  if (dbg) std::fprintf(stderr, "[spi] dev %u byte %02x -> %02x at %llu ready %llu\n", (spicnt >> 8) & 3, value, spidata, (unsigned long long)nds_.sched.now(), (unsigned long long)spi_ready_at);
  if (spi_flag_mode_) { spi_busy_ = true; nds_.sched.schedule(EventId::Spi, spi_ready_at, spi_event, 0); }
  else if (spicnt & 0x4000) nds_.sched.schedule(EventId::Spi, spi_ready_at, spi_event, 0);
}

// ---- RTC (ARM7, bit-banged on 0x04000138) ----
// The clock only free-runs once a frontend calls start_rtc_clock().
static u8 to_bcd(int v) { return static_cast<u8>(((v / 10) % 10) << 4 | (v % 10)); }
static int from_bcd(u8 v) { return (v >> 4) * 10 + (v & 0x0F); }

static void rtc_ev(NDS& nds, u32) { nds.io.rtc_event(); }

void Io::rtc_seed() {
  const std::time_t t = std::time(nullptr);
  std::tm lt{};
#if defined(_WIN32)
  localtime_s(&lt, &t);
#else
  localtime_r(&t, &lt);
#endif
  Rtc& r = rtc;
  r.datetime[0] = to_bcd(lt.tm_year % 100);     // two digits; 2000-2099
  r.datetime[1] = to_bcd(lt.tm_mon + 1);
  r.datetime[2] = to_bcd(lt.tm_mday);
  r.datetime[3] = static_cast<u8>(lt.tm_wday);  // 0 = Sunday
  r.datetime[4] = to_bcd(lt.tm_hour);   // always 24-hour (status1 bit 1)
  r.datetime[5] = to_bcd(lt.tm_min);
  r.datetime[6] = to_bcd(std::min(lt.tm_sec, 59));   // no leap seconds on this chip
  r.status1 = 0x02;      // 24-hour mode, not power-lost
  r.ticking = true;
  r.next_tick = nds_.sched.now() + ARM9_CLOCK_HZ;
  nds_.sched.schedule(EventId::Rtc, r.next_tick, rtc_ev, 0);
}

void Io::start_rtc_clock() {
  rtc_host_clock_ = true;
  rtc_seed();
}

// One second of carry, field-by-field rather than via time_t: a game's own
// (possibly invalid) date must still advance.
void Io::rtc_tick() {
  Rtc& r = rtc;
  int sec = from_bcd(r.datetime[6]) + 1;
  if (sec < 60) { r.datetime[6] = to_bcd(sec); return; }
  r.datetime[6] = 0;
  int min = from_bcd(r.datetime[5]) + 1;
  if (min < 60) { r.datetime[5] = to_bcd(min); return; }
  r.datetime[5] = 0;
  const u8 pm = r.datetime[4] & 0x40;
  int hour = from_bcd(static_cast<u8>(r.datetime[4] & 0x3F)) + 1;
  if (hour < 24) { r.datetime[4] = static_cast<u8>(to_bcd(hour) | pm); return; }
  r.datetime[4] = pm;
  r.datetime[3] = static_cast<u8>((r.datetime[3] + 1) % 7);
  const int year = from_bcd(r.datetime[0]), month = from_bcd(r.datetime[1]);
  // Two-digit years (2000-2099): every year divisible by 4 is a leap year.
  static const int len[13] = {31, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const int days = (month == 2 && (year % 4) == 0) ? 29 : len[month >= 1 && month <= 12 ? month : 1];
  int day = from_bcd(r.datetime[2]) + 1;
  if (day <= days) { r.datetime[2] = to_bcd(day); return; }
  r.datetime[2] = to_bcd(1);
  if (month < 12) { r.datetime[1] = to_bcd(month + 1); return; }
  r.datetime[1] = to_bcd(1);
  r.datetime[0] = to_bcd((year + 1) % 100);
}

void Io::rtc_event() {
  Rtc& r = rtc;
  if (!r.ticking) return;
  do {   // catch up whole seconds (a slice overrun, or a reloaded state left next_tick behind)
    rtc_tick();
    r.next_tick += ARM9_CLOCK_HZ;
  } while (r.next_tick <= nds_.sched.now());
  nds_.sched.schedule(EventId::Rtc, r.next_tick, rtc_ev, 0);
}

// Protocol per GBATEK.
void Io::rtc_cmd_read() {
  Rtc& r = rtc;
  if ((r.cmd & 0x0F) != 0x06) return;
  switch (r.cmd & 0x70) {
  case 0x00: r.output[0] = r.status1; if (r.status1 & 0x80) rtc_power_lost_seen_ = true; r.status1 &= 0x0F; break;
  case 0x40: r.output[0] = r.status2; break;
  case 0x20: std::memcpy(r.output, &r.datetime[0], 7); break;
  case 0x60: std::memcpy(r.output, &r.datetime[4], 3); break;
  case 0x10: if (r.status2 & 0x04) std::memcpy(r.output, r.alarm1, 3); else r.output[0] = r.alarm1[2]; break;
  case 0x50: std::memcpy(r.output, r.alarm2, 3); break;
  case 0x30: r.output[0] = r.clock_adjust; break;
  case 0x70: r.output[0] = r.free_reg; break;
  }
}

void Io::rtc_cmd_write(u8 v) {
  Rtc& r = rtc;
  if ((r.cmd & 0x0F) != 0x06) return;
  const u32 pos = r.input_pos;
  switch (r.cmd & 0x70) {
  case 0x00:
    if (pos == 1) {
      if (v & 1) { r.status1 = 0; r.status2 = 0; std::memset(r.datetime, 0, 7); r.datetime[1] = r.datetime[2] = 1; }
      r.status1 = (r.status1 & 0xF0) | (v & 0x0E);
    }
    break;
  case 0x40: if (pos == 1) r.status2 = v; break;
  case 0x20: if (pos >= 1 && pos <= 7) r.datetime[pos - 1] = v; break;
  case 0x60: if (pos >= 1 && pos <= 3) r.datetime[3 + pos] = v; break;
  case 0x10: if (r.status2 & 0x04) { if (pos >= 1 && pos <= 3) r.alarm1[pos - 1] = v; } else if (pos == 1) r.alarm1[2] = v; break;
  case 0x50: if (pos >= 1 && pos <= 3) r.alarm2[pos - 1] = v; break;
  case 0x30: if (pos == 1) r.clock_adjust = v; break;
  case 0x70: if (pos == 1) r.free_reg = v; break;
  }
}

void Io::rtc_byte_in(u8 v) {
  Rtc& r = rtc;
  if (r.input_pos == 0) {
    if ((v & 0xF0) == 0x60) {
      static const u8 rev[16] = {0x06, 0x86, 0x46, 0xC6, 0x26, 0xA6, 0x66, 0xE6, 0x16, 0x96, 0x56, 0xD6, 0x36, 0xB6, 0x76, 0xF6};
      r.cmd = rev[v & 0xF];
    } else r.cmd = v;
    if (r.cmd & 0x80) rtc_cmd_read();
    return;
  }
  rtc_cmd_write(v);
}

void Io::rtc_write(u16 value, bool byte) {
  Rtc& r = rtc;
  if (byte) value |= (r.io & 0xFF00);
  if (value & 0x0004) {
    if (!(r.io & 0x0004)) {                          // CS rising: start transfer
      r.input = 0; r.input_bit = 0; r.input_pos = 0;
      std::memset(r.output, 0, sizeof r.output); r.output_bit = 0; r.output_pos = 0;
    } else if (!(value & 0x0002)) {                  // clock low
      if (value & 0x0010) {                          // host drives SIO: write bit
        if (value & 1) r.input |= static_cast<u8>(1u << r.input_bit);
        if (++r.input_bit >= 8) { r.input_bit = 0; rtc_byte_in(r.input); r.input = 0; r.input_pos++; }
      } else {                                       // read bit
        if (r.output[r.output_pos] & (1u << r.output_bit)) r.io |= 1; else r.io &= 0xFFFE;
        if (++r.output_bit >= 8) { r.output_bit = 0; if (r.output_pos < 7) r.output_pos++; }
      }
    }
  }
  if (value & 0x0010) r.io = value; else r.io = (r.io & 0x0001) | (value & 0xFFFE);
}

// ---- cart bus ----
// Timing per GBATEK: the 8-bit command takes 8 clocks, each data word 4,
// gap1 before data, gap2 before each 0x200-byte block; clock is 5 or 8
// system cycles per transfer clock (ROMCTRL bit 27). System cycles are half
// ARM9 cycles.
static void cart_ev(NDS& nds, u32 param) { nds.io.cart_event(param); }

void Io::cart_write_romctrl(u32 value) {
  const bool start = (value & ~cart.romctrl) & 0x80000000;
  const bool release = (value & ~cart.romctrl) & (1u << 29);
  cart.romctrl = (cart.romctrl & 0x00800000) | (value & 0xFF7F7FFF) | (cart.romctrl & (1u << 29));
  if (value & (1u << 29)) cart.romctrl |= 1u << 29;
  if (release) update_cart_reset();
  if (!(cart.auxspicnt & 0x8000) || (cart.auxspicnt & 0x2000) || !start) return;
  if (nds_.cart) nds_.cart->command_start(cart.cmd.data());
  u32 size_code = (cart.romctrl >> 24) & 7;
  u32 bytes = size_code == 7 ? 4 : size_code ? (0x100u << size_code) : 0;
  cart.transfer_pos = 0; cart.transfer_len = bytes;
  cart.fifo_count = 0; cart.late = false;
  cart.romctrl &= ~0x00800000u;
  const u32 xfer = (cart.romctrl & (1u << 27)) ? 8 : 5;
  u32 cmddelay = 8 + (cart.romctrl & 0x1FFF);
  if (bytes) cmddelay += (cart.romctrl >> 16) & 0x3F;
  // The gap1/gap2 latency stays under the fast model too (DS_CART_GAPS=0
  // drops it to the command's 8 clocks, the former fast default). Without
  // it Tongari Boushi spins on its load screen forever -- the KEY2 gap
  // timing of mGBA's "holy grail bugs 2" -- and Sonic Chronicles' FMV
  // decoder runs off into unmapped memory. Games poll ROMCTRL through the
  // gap (a DS_IO_CENSUS found ROMCTRL reads outnumbering data words 7:1),
  // but on the device the gaps cost nothing on the hard tier and made
  // Animal Crossing faster (median 9.1 -> 8.0 ms); only Pokemon W2 pays
  // (+0.6 ms at a 3.6 ms median).
  static const bool drop_gaps = std::getenv("DS_CART_GAPS") && std::getenv("DS_CART_GAPS")[0] == '0';
  if (g_fast_timing && drop_gaps) cmddelay = 8;
  cart.event_armed = false;
  if (cart.romctrl & (1u << 30)) {            // write direction: not supported; end after the command
    nds_.sched.schedule(EventId::Cart, nds_.sched.now() + 2 * xfer * cmddelay, cart_ev, 0);
    cart.event_armed = true;
    return;
  }
  if (bytes == 0) {
    nds_.sched.schedule(EventId::Cart, nds_.sched.now() + 2 * xfer * cmddelay, cart_ev, 0);
    cart.event_armed = true;
    return;
  }
  cart.next_word_at = nds_.sched.now() + 2 * xfer * (cmddelay + 4);   // event only if a DMA is waiting for it
  if (cart_dma_armed()) {
    nds_.sched.schedule(EventId::Cart, cart.next_word_at, cart_ev, 1);
    cart.event_armed = true;
  }
}

// Cart words arrive on a fixed clock and nothing observes one arriving: the
// CPU learns of them by reading ROMCTRL's DRQ bit or ROMDATA, so words are
// produced lazily at the read (`cart_catch_up`). Exception: a cart-mode DMA
// channel is level-triggered on DRQ with nothing to poll it, so the
// per-word event stays while one is armed. Always on for DSi: the ARM9
// slice must end at every word to interleave correctly with the ARM7.
bool Io::cart_dma_armed() const { return nds_.dsi || nds_.dma.cart_armed(); }

// Fast timing: a word is ready as soon as the FIFO has room, and the command
// delay is cut to the command itself (cart_write_romctrl). A CPU copy loop
// finds DRQ on every poll instead of spinning on ROMCTRL, and a cart DMA's
// words chain within one scheduler pass instead of an event (and a split CPU
// slice) every 40 cycles.
u32 Io::cart_word_delay() const {
  // The per-word bus clock stays under the fast model too (DS_CART_CLOCK=0
  // drops it, the former fast default): with words arriving at once, Sonic
  // Chronicles' FMV decoder reads a corrupted stream, writes over its stack
  // and runs off into unmapped memory at frame ~1630 (see DS_JIT_WEEDS).
  static const bool drop_clock = std::getenv("DS_CART_CLOCK") && std::getenv("DS_CART_CLOCK")[0] == '0';
  if (g_fast_timing && drop_clock) return 0;
  const u32 xfer = (cart.romctrl & (1u << 27)) ? 8 : 5;
  u32 delay = 4;
  if (!(cart.transfer_pos & 0x1FF)) delay += (cart.romctrl >> 16) & 0x3F;
  return 2 * xfer * delay;
}

// `from` is the base the next word's delay counts from: the word's nominal
// arrival time on the lazy path, but now() on the event path (a nominal
// deadline there can land in the past and fire again in the same pass).
void Io::cart_schedule_receive(u64 from) {
  if (cart.transfer_pos >= cart.transfer_len) return;
  cart.next_word_at = from + cart_word_delay();
  static const bool dbg = std::getenv("DS_DEBUG_CART") != nullptr;   // DS_DEBUG_CART=1: every word deadline
  if (dbg) std::fprintf(stderr, "[cart] word %u at %llu (from %llu now %llu)\n", cart.transfer_pos / 4, (unsigned long long)cart.next_word_at, (unsigned long long)from, (unsigned long long)nds_.sched.now());
  if (cart_dma_armed()) {
    nds_.sched.schedule(EventId::Cart, cart.next_word_at, cart_ev, 1);
    cart.event_armed = true;
  }
}

// Materialise every word whose time has passed. FIFO holds two; while full,
// transfer stalls (`late`) and resumes from the read that makes room.
void Io::cart_catch_up_slow() {
  if (cart.event_armed || cart.late) return;
  while (cart.transfer_pos < cart.transfer_len && cart.fifo_count < 2 && nds_.sched.now() >= cart.next_word_at)
    cart_receive_word(cart.next_word_at);
  if (!cart.event_armed && !cart.late && cart.transfer_pos < cart.transfer_len && cart_dma_armed()) {   // a DMA armed mid-transfer takes over
    nds_.sched.schedule(EventId::Cart, cart.next_word_at, cart_ev, 1);
    cart.event_armed = true;
  }
}

void Io::cart_receive_word(u64 at) {
  cart.fifo[(cart.fifo_head + cart.fifo_count) & 1] = nds_.cart ? nds_.cart->command_receive() : 0;
  cart.fifo_count++;
  cart.transfer_pos += 4;
  cart.romctrl |= 0x00800000u;                 // DRQ
  // Only a cart-mode channel can be started by DRQ; skip the scan when none is armed.
  if (cart_dma_armed()) {
    nds_.dma.check(Cpu::ARM9, dma::MODE9_CART);
    nds_.dma.check(Cpu::ARM7, dma::MODE7_CART);
  }
  if (cart.fifo_count < 2) cart_schedule_receive(at); else cart.late = true;
}

void Io::cart_end_transfer() {
  cart.romctrl &= ~0x80000000u;
  cart.transfer_pos = cart.transfer_len = 0;
  if (cart.auxspicnt & 0x4000) request_irq((exmemcnt & 0x0800) ? Cpu::ARM7 : Cpu::ARM9, IRQ_CART_DONE);   // to slot owner (EXMEMCNT bit 11)
}

void Io::cart_event(u32 param) {
  cart.event_armed = false;
  if (param == 0) cart_end_transfer(); else cart_receive_word(nds_.dsi ? nds_.sched.event_base7() : nds_.sched.now());   // DSi: from ARM7's clock
}

u32 Io::cart_read_data() {
  cart_catch_up();
  const u32 v = cart.fifo[cart.fifo_head];
  if (cart.romctrl & (1u << 30)) return v;
  if (cart.fifo_count > 0) { cart.fifo_count--; cart.fifo_head ^= 1; }
  cart.romctrl &= ~0x00800000u;
  if (cart.transfer_pos < cart.transfer_len) {
    if (cart.late) { cart.late = false; cart_schedule_receive(nds_.sched.now()); }
  } else {
    if (cart.fifo_count == 0) cart_end_transfer();
    else if (cart.fifo_count) cart.romctrl |= 0x00800000u;
  }
  return v;
}

// ---- divider / square root ----
// Results appear after 18/34 (DIV, 32/64-bit) and 13 (SQRT) system cycles;
// busy bit set meanwhile. Division by zero yields +/-1 with the numerator
// as remainder; most-negative / -1 overflow wraps. No event: due at a
// recorded time, computed by the first read after it (see MathUnit).
static void ev_div(NDS& nds, u32)  { nds.io.div_settle(); }
static void ev_sqrt(NDS& nds, u32) { nds.io.sqrt_settle(); }

void Io::div_start() {
  math.div_pending = true;
  math.div_ready_at = nds_.sched.now() + (((math.divcnt & 3) == 0) ? 18 : 34) * 2;
  if (nds_.dsi) nds_.sched.schedule(EventId::Div, math.div_ready_at, ev_div, 0);   // cuts the slice there; result still settled lazily
}

void Io::div_done() {
  MathUnit& m = math;
  m.div_pending = false;
  m.divcnt &= ~0xC000;
  switch (m.divcnt & 3) {
  case 0: {
    const s32 num = static_cast<s32>(m.div_num), den = static_cast<s32>(m.div_den);
    if (den == 0) {
      m.div_quot = (num < 0) ? 0xFFFFFFFF00000001ull : 0x00000000FFFFFFFFull;
      m.div_rem = static_cast<u64>(static_cast<s64>(num));
    } else if (num == INT32_MIN && den == -1) {
      m.div_quot = 0x80000000ull;
    } else {
      m.div_quot = static_cast<u64>(static_cast<s64>(num / den));
      m.div_rem = static_cast<u64>(static_cast<s64>(num % den));
    }
    break;
  }
  default: {
    const s64 num = static_cast<s64>(m.div_num);
    const s64 den = ((m.divcnt & 3) == 2) ? static_cast<s64>(m.div_den) : static_cast<s64>(static_cast<s32>(m.div_den));
    if (den == 0) {
      m.div_quot = (num < 0) ? 1ull : ~0ull;
      m.div_rem = static_cast<u64>(num);
    } else if (num == INT64_MIN && den == -1) {
      m.div_quot = 0x8000000000000000ull; m.div_rem = 0;
    } else {
      m.div_quot = static_cast<u64>(num / den);
      m.div_rem = static_cast<u64>(num % den);
    }
    break;
  }
  }
  if (m.div_den == 0) m.divcnt |= 0x4000;
}

void Io::sqrt_start() {
  math.sqrt_pending = true;
  math.sqrt_ready_at = nds_.sched.now() + 13 * 2;
  if (nds_.dsi) nds_.sched.schedule(EventId::Sqrt, math.sqrt_ready_at, ev_sqrt, 0);
}

void Io::sqrt_done() {
  MathUnit& m = math;
  m.sqrt_pending = false;
  u64 val = (m.sqrtcnt & 1) ? m.sqrt_val : (m.sqrt_val & 0xFFFFFFFFull);
  u64 res = 0, rem = 0;   // digit-by-digit integer square root
  const int nbits = (m.sqrtcnt & 1) ? 32 : 16;
  const int topshift = (m.sqrtcnt & 1) ? 62 : 30;
  for (int i = 0; i < nbits; ++i) {
    rem = (rem << 2) + ((val >> topshift) & 3);
    val <<= 2;
    res <<= 1;
    const u64 prod = (res << 1) + 1;
    if (rem >= prod) { rem -= prod; ++res; }
  }
  m.sqrt_res = static_cast<u32>(res);
}

void Io::set_irq_line(Cpu cpu, u32 bit, bool on) {
  if (on) request_irq(cpu, bit);
  else { cpu_io[ci(cpu)].if_ &= ~(1u << bit); update_irq(cpu); }
}

// ---- register dispatch ----
// Gpu3D/Gpu/Spu own r < 0x70, [0x400,0x520) and up; [0x70,0x320) and
// r >= 0x1070 belong to no subsystem. One range test replaces three probes.
static inline bool io_unowned(u32 r) { return (r - 0x70 < 0x2B0) || r >= 0x1070; }

// DS_IO_CENSUS=1 (census builds): histogram of I/O accesses by address and CPU, printed at exit.
namespace {
struct IoCensus {
  bool on = prof::census_env("DS_IO_CENSUS");
  std::unordered_map<u32, u64> rd[2], wr[2];
  ~IoCensus() {
    if (!on) return;
    for (int c = 0; c < 2; ++c) {
      std::vector<std::pair<u64, u32>> v;
      for (auto& kv : rd[c]) v.push_back({kv.second, kv.first});
      std::sort(v.rbegin(), v.rend());
      std::fprintf(stderr, "[io] arm%d hottest READS:\n", c ? 7 : 9);
      for (size_t i = 0; i < v.size() && i < 12; ++i)
        std::fprintf(stderr, "[io]   %08x %12llu\n", v[i].second, (unsigned long long)v[i].first);
      v.clear();
      for (auto& kv : wr[c]) v.push_back({kv.second, kv.first});
      std::sort(v.rbegin(), v.rend());
      std::fprintf(stderr, "[io] arm%d hottest WRITES:\n", c ? 7 : 9);
      for (size_t i = 0; i < v.size() && i < 12; ++i)
        std::fprintf(stderr, "[io]   %08x %12llu\n", v[i].second, (unsigned long long)v[i].first);
    }
  }
};
IoCensus g_ioc;
}
bool Io::census_env_on() { return g_ioc.on; }

u32 Io::read(Cpu cpu, u32 addr, u32 width) {
  if (prof::census && g_ioc.on) g_ioc.rd[cpu == Cpu::ARM9 ? 0 : 1][addr]++;
  if (cpu == Cpu::ARM7 && (addr & ~3u) != 0x040001C0) spi_poll_streak_ = 0;
  if (nds_.dsi && (addr & 0xFFFFF000) == 0x04004000) return dsi_read(cpu, addr, width);
  if (!io_unowned(addr - 0x04000000)) {
    if (cpu == Cpu::ARM9 && gpu::Gpu3D::owns_reg(addr)) return nds_.gpu3d.read(addr, width);
    if (cpu == Cpu::ARM9 && gpu::Gpu::owns_reg(addr)) return nds_.gpu.reg_read(addr, width);
    if (cpu == Cpu::ARM7 && spu::Spu::owns_reg(addr)) return nds_.spu.read(addr, width);
  }
  if (width == 32) { const Special s = read32_special(cpu, addr); if (s.handled) return s.value; return read16(cpu, addr) | (static_cast<u32>(read16(cpu, addr + 2)) << 16); }
  if (width == 16) return read16(cpu, addr);
  return read8(cpu, addr);
}

void Io::write(Cpu cpu, u32 addr, u32 width, u32 value) {
  if (prof::census && g_ioc.on) g_ioc.wr[cpu == Cpu::ARM9 ? 0 : 1][addr]++;
  if (cpu == Cpu::ARM7) spi_poll_streak_ = 0;
  if (nds_.dsi && (addr & 0xFFFFF000) == 0x04004000) { dsi_write(cpu, addr, width, value); return; }
  if (!io_unowned(addr - 0x04000000)) {
    if (cpu == Cpu::ARM9 && gpu::Gpu3D::owns_reg(addr)) { nds_.gpu3d.write(addr, width, value); return; }
    if (cpu == Cpu::ARM9 && gpu::Gpu::owns_reg(addr)) {
      static const bool dbg_gpureg = std::getenv("DS_DEBUG_GPUREG") != nullptr;
      if (dbg_gpureg && (addr & 0xFF) >= 0x50) std::fprintf(stderr, "[gpureg] frame %llu line %u write%u %08x = %08x\n", (unsigned long long)nds_.frame_count, nds_.gpu.line(), width, addr, value);
      nds_.gpu.reg_write(addr, width, value); return;
    }
    if (cpu == Cpu::ARM7 && spu::Spu::owns_reg(addr)) { nds_.spu.write(addr, width, value); return; }
  }
  if (width == 32) {
    if (cpu == Cpu::ARM9 && addr >= 0x04000240 && addr < 0x0400024A) { vramcnt_store(addr, value, 4); return; }
    if (write32_special(cpu, addr, value).handled) return;
    write16(cpu, addr, static_cast<u16>(value)); write16(cpu, addr + 2, static_cast<u16>(value >> 16)); return;
  }
  if (width == 16) { write16(cpu, addr, static_cast<u16>(value)); return; }
  write8(cpu, addr, static_cast<u8>(value));
}

Io::Special Io::read32_special(Cpu cpu, u32 addr) {
  CpuIo& c = cpu_io[ci(cpu)];
  if (addr == 0x04100000) return {ipc_fifo_recv(cpu), true};   // the two 0x0410xxxx ports, kept out of the dense index below
  if (addr == 0x04100010) return {cart_read_data(), true};
  switch ((addr - 0x04000000u) >> 2) {   // dense index: GCC emits a jump table
  case 0x82: return {c.ime, true};
  case 0x84: return {c.ie, true};
  case 0x85: return {c.if_, true};
  case 0x69: cart_catch_up(); return {cart.romctrl, true};
  case 0xa0: return {divcnt_read(), true};
  case 0xa4: return {static_cast<u32>(math.div_num), true};
  case 0xa5: return {static_cast<u32>(math.div_num >> 32), true};
  case 0xa6: return {static_cast<u32>(math.div_den), true};
  case 0xa7: return {static_cast<u32>(math.div_den >> 32), true};
  case 0xa8: div_settle(); return {static_cast<u32>(math.div_quot), true};
  case 0xa9: div_settle(); return {static_cast<u32>(math.div_quot >> 32), true};
  case 0xaa: div_settle(); return {static_cast<u32>(math.div_rem), true};
  case 0xab: div_settle(); return {static_cast<u32>(math.div_rem >> 32), true};
  case 0xac: return {sqrtcnt_read(), true};
  case 0xad: sqrt_settle(); return {math.sqrt_res, true};
  case 0xae: return {static_cast<u32>(math.sqrt_val), true};
  case 0xaf: return {static_cast<u32>(math.sqrt_val >> 32), true};
  case 0x2c: case 0x2f: case 0x32: case 0x35: return {nds_.dma.read_src(cpu, (addr - 0x040000B0) / 12), true};
  case 0x2d: case 0x30: case 0x33: case 0x36: return {nds_.dma.read_dst(cpu, (addr - 0x040000B4) / 12), true};
  case 0x2e: case 0x31: case 0x34: case 0x37: return {nds_.dma.read_cnt(cpu, (addr - 0x040000B8) / 12), true};
  case 0x38: case 0x39: case 0x3a: case 0x3b: return {c.dma_fill[(addr - 0x040000E0) / 4], true};
  }
  return {0, false};
}

Io::Special Io::write32_special(Cpu cpu, u32 addr, u32 value) {
  CpuIo& c = cpu_io[ci(cpu)];
  switch ((addr - 0x04000000u) >> 2) {   // dense index: GCC emits a jump table
  case 0x82: c.ime = value & 1; update_irq(cpu); return {0, true};
  case 0x84: c.ie = value; update_irq(cpu); return {0, true};
  case 0x85: c.if_ &= ~value; update_irq(cpu); if (cpu == Cpu::ARM9) nds_.gpu3d.check_fifo_irq_fast(); return {0, true};
  case 0x62: ipc_fifo_send(cpu, value); return {0, true};
  case 0x69: cart_write_romctrl(value); return {0, true};
  case 0xa0: math.divcnt = value & 0x3; div_start(); return {0, true};
  case 0xa4: math.div_num = (math.div_num & 0xFFFFFFFF00000000ull) | value; div_start(); return {0, true};
  case 0xa5: math.div_num = (math.div_num & 0xFFFFFFFFull) | (static_cast<u64>(value) << 32); div_start(); return {0, true};
  case 0xa6: math.div_den = (math.div_den & 0xFFFFFFFF00000000ull) | value; div_start(); return {0, true};
  case 0xa7: math.div_den = (math.div_den & 0xFFFFFFFFull) | (static_cast<u64>(value) << 32); div_start(); return {0, true};
  case 0xac: math.sqrtcnt = value & 0x1; sqrt_start(); return {0, true};
  case 0xae: math.sqrt_val = (math.sqrt_val & 0xFFFFFFFF00000000ull) | value; sqrt_start(); return {0, true};
  case 0xaf: math.sqrt_val = (math.sqrt_val & 0xFFFFFFFFull) | (static_cast<u64>(value) << 32); sqrt_start(); return {0, true};
  case 0x2c: case 0x2f: case 0x32: case 0x35: nds_.dma.write_src(cpu, (addr - 0x040000B0) / 12, value); return {0, true};
  case 0x2d: case 0x30: case 0x33: case 0x36: nds_.dma.write_dst(cpu, (addr - 0x040000B4) / 12, value); return {0, true};
  case 0x2e: case 0x31: case 0x34: case 0x37: nds_.dma.write_cnt(cpu, (addr - 0x040000B8) / 12, value); return {0, true};
  case 0x38: case 0x39: case 0x3a: case 0x3b: c.dma_fill[(addr - 0x040000E0) / 4] = value; return {0, true};
  }
  return {0, false};
}

u32 Io::read16(Cpu cpu, u32 addr) {
  CpuIo& c = cpu_io[ci(cpu)];
  const bool a9 = cpu == Cpu::ARM9;
  if (!a9 && addr >= 0x04800000 && addr < 0x04810000) return (powcnt2 & 2) ? wifi.read16(addr) : 0;
  switch ((addr - 0x04000000u) >> 1) {   // dense index: GCC emits a jump table
  case 0x2: return dispstat[ci(cpu)];
  case 0x3: return vcount;
  case 0x80: case 0x82: case 0x84: case 0x86: return timer_value(cpu, (addr - 0x04000100) / 4);
  case 0x81: case 0x83: case 0x85: case 0x87: return c.timers[(addr - 0x04000102) / 4].control;
  case 0x98: return keyinput;
  case 0x99: return keycnt[ci(cpu)];
  case 0x9a: return a9 ? 0 : rcnt;
  case 0x9b: return a9 ? 0 : extkeyin;
  case 0x9c: return a9 ? 0 : rtc_read();
  case 0xc0: return c.ipc_sync;
  case 0xc2: return ipc_fifo_cnt_read(cpu);
  case 0xd0: return cart.auxspicnt;
  case 0xd1: return cart.auxspidata;
  case 0xd4: case 0xd5: case 0xd6: case 0xd7: {
    const u32 i = addr - 0x040001A8; return static_cast<u16>(cart.cmd[i] | (cart.cmd[i + 1] << 8));
  }
  case 0xe0: return a9 ? 0 : spicnt_read_arm7();
  case 0xe1: return a9 ? 0 : spidata;
  case 0x102: return exmemcnt;
  case 0x103: return (!a9 && (powcnt2 & 2)) ? wifiwaitcnt : 0;
  case 0x5d: case 0x63: case 0x69: case 0x6f: return static_cast<u16>(nds_.dma.read_cnt(cpu, (addr - 0x040000BA) / 12) >> 16);
  case 0x5c: case 0x62: case 0x68: case 0x6e: return static_cast<u16>(nds_.dma.read_cnt(cpu, (addr - 0x040000B8) / 12));
  case 0x104: return static_cast<u16>(c.ime);
  case 0x108: return static_cast<u16>(c.ie);
  case 0x109: return static_cast<u16>(c.ie >> 16);
  case 0x10a: return static_cast<u16>(c.if_);
  case 0x10b: return static_cast<u16>(c.if_ >> 16);
  case 0x10c: return (!a9 && nds_.dsi) ? static_cast<u16>(dsi.ie2) : 0;   // IE2/IF2 (DSi ARM7; 16 bits used)
  case 0x10e: return (!a9 && nds_.dsi) ? static_cast<u16>(dsi.if2) : 0;
  case 0x120: return a9 ? static_cast<u16>(vramcnt[0] | (vramcnt[1] << 8)) : static_cast<u16>(((vramcnt[2] >> 0) & 7) == 2 ? 1 : 0) | ((((vramcnt[3] >> 0) & 7) == 2 ? 2 : 0)) | (wramcnt << 8);
  case 0x180: return c.postflg;
  case 0x184: return a9 ? 0 : arm7_bios_prot;               // BIOSPROT (0x04000308)
  case 0x182: return a9 ? powcnt1 : powcnt2;
  case 0x140: return divcnt_read();
  case 0x158: return sqrtcnt_read();
  default: break;
  }
  if (a9 && addr >= 0x04000290 && addr < 0x040002C0) { const u32 v = read32_special(cpu, addr & ~3u).value; return static_cast<u16>((addr & 2) ? v >> 16 : v); }
  if (addr >= 0x04000240 && addr < 0x0400024A && a9) {
    // 0x240-0x246 VRAMCNT A-G, 0x247 WRAMCNT, 0x248-0x249 VRAMCNT H-I.
    const u32 i = addr - 0x04000240;
    auto get = [&](u32 k) -> u8 { if (k == 7) return wramcnt; if (k > 9) return 0; return vramcnt[k < 7 ? k : k - 1]; };
    return static_cast<u16>(get(i) | (get(i + 1) << 8));
  }
  return 0;
}

void Io::write16(Cpu cpu, u32 addr, u16 value) {
  CpuIo& c = cpu_io[ci(cpu)];
  const bool a9 = cpu == Cpu::ARM9;
  if (!a9 && addr >= 0x04800000 && addr < 0x04810000) { if (powcnt2 & 2) wifi.write16(addr, value); return; }
  switch ((addr - 0x04000000u) >> 1) {   // dense index: GCC emits a jump table
  case 0x2: dispstat[ci(cpu)] = (dispstat[ci(cpu)] & 0x0047) | (value & 0xFFB8); return;   // bit 6 (DSi LCD init flag) is read-only, never set on a DS
  case 0x80: case 0x82: case 0x84: case 0x86: c.timers[(addr - 0x04000100) / 4].reload = value; return;
  case 0x81: case 0x83: case 0x85: case 0x87: timer_write_control(cpu, (addr - 0x04000102) / 4, value); return;
  case 0x99: keycnt[ci(cpu)] = value; update_key_irq(); return;
  case 0x9a: if (!a9) rcnt = value; return;
  case 0x9c: if (!a9) rtc_write(value, false); return;
  case 0xc0: ipc_sync_write(cpu, value); return;
  case 0xc2: ipc_fifo_cnt_write(cpu, value); return;
  case 0xd0:
    if (nds_.cart) {
      if (cart.auxspicnt & ~value & 0x2000) nds_.cart->spi_release();
      else if (~cart.auxspicnt & value & 0x2000) nds_.cart->spi_select();
    }
    cart.auxspicnt = (cart.auxspicnt & 0x0080) | (value & 0xE043);
    return;
  case 0xd1: {
    if (!(cart.auxspicnt & 0x8000) || !(cart.auxspicnt & 0x2000)) return;
    const bool hold = cart.auxspicnt & 0x0040;
    if (nds_.cart) { cart.auxspidata = nds_.cart->spi_transfer(static_cast<u8>(value)); if (!hold) nds_.cart->spi_release(); }
    else cart.auxspidata = 0;
    return;
  }
  case 0xe0:
    if (a9) return;
    if ((spicnt & 0x8000) && !(value & 0x8000)) spi_release();
    spicnt = value & 0xCF03;
    if ((spicnt & 0x4000) && spi_busy()) nds_.sched.schedule(EventId::Spi, spi_ready_at, spi_event, 0);   // IRQ enabled mid-transfer
    return;
  case 0xe1: if (!a9) spi_write_data(static_cast<u8>(value)); return;
  case 0x102: {
    const u16 old = exmemcnt;
    // DSi has one more ARM9 bit, 10: which CPU the second card slot answers.
    const u16 rw9 = nds_.dsi ? 0x8CFF : 0x88FF;
    exmemcnt = a9 ? ((exmemcnt & 0x6000) | (value & rw9)) : ((exmemcnt & 0xFF80) | (value & 0x007F));
    if ((old ^ exmemcnt) & 0xFF) nds_.bus.update_gba_slot_timings();
    return;
  }
  case 0xd4: case 0xd5: case 0xd6: case 0xd7: {
    const u32 i = addr - 0x040001A8; cart.cmd[i] = static_cast<u8>(value); cart.cmd[i + 1] = static_cast<u8>(value >> 8); return;
  }
  case 0x103: if (!a9 && (powcnt2 & 2) && wifiwaitcnt != value) { wifiwaitcnt = value; nds_.bus.update_wifi_timings(); } return;
  case 0x140: if (a9) { math.divcnt = value & 3; div_start(); } return;
  case 0x158: if (a9) { math.sqrtcnt = value & 1; sqrt_start(); } return;
  case 0x5c: case 0x62: case 0x68: case 0x6e: {
    const int i = (addr - 0x040000B8) / 12; nds_.dma.write_cnt(cpu, i, (nds_.dma.read_cnt(cpu, i) & 0xFFFF0000) | value); return;
  }
  case 0x5d: case 0x63: case 0x69: case 0x6f: {
    const int i = (addr - 0x040000BA) / 12; nds_.dma.write_cnt(cpu, i, (nds_.dma.read_cnt(cpu, i) & 0x0000FFFF) | (static_cast<u32>(value) << 16)); return;
  }
  case 0x58: case 0x59: case 0x5e: case 0x5f: case 0x64: case 0x65: case 0x6a: case 0x6b: {
    const int i = (addr - 0x040000B0) / 12; const u32 old = nds_.dma.read_src(cpu, i);
    nds_.dma.write_src(cpu, i, ((addr - 0x040000B0) % 12) ? ((old & 0x0000FFFF) | (static_cast<u32>(value) << 16)) : ((old & 0xFFFF0000) | value)); return;
  }
  case 0x5a: case 0x5b: case 0x60: case 0x61: case 0x66: case 0x67: case 0x6c: case 0x6d: {
    const int i = (addr - 0x040000B4) / 12; const u32 old = nds_.dma.read_dst(cpu, i);
    nds_.dma.write_dst(cpu, i, ((addr - 0x040000B4) % 12) ? ((old & 0x0000FFFF) | (static_cast<u32>(value) << 16)) : ((old & 0xFFFF0000) | value)); return;
  }
  case 0x104: c.ime = value & 1; update_irq(cpu); return;
  case 0x108: c.ie = (c.ie & 0xFFFF0000) | value; update_irq(cpu); return;
  case 0x109: c.ie = (c.ie & 0x0000FFFF) | (static_cast<u32>(value) << 16); update_irq(cpu); return;
  case 0x10a: c.if_ &= ~static_cast<u32>(value); update_irq(cpu); if (a9) nds_.gpu3d.check_fifo_irq_fast(); return;
  case 0x10b: c.if_ &= ~(static_cast<u32>(value) << 16); update_irq(cpu); if (a9) nds_.gpu3d.check_fifo_irq_fast(); return;
  case 0x10c: if (!a9 && nds_.dsi) { dsi.ie2 = value & 0x7FF7; update_irq(cpu); } return;
  case 0x10e: if (!a9 && nds_.dsi) { dsi.if2 &= ~static_cast<u32>(value & 0x7FF7); update_irq(cpu); } return;
  case 0x180:
    c.postflg |= value & 1; if (a9) c.postflg = (c.postflg & 1) | (value & 2);
    if (!a9 && (value >> 8)) write8(cpu, 0x04000301, static_cast<u8>(value >> 8));
    return;
  case 0x184: if (!a9 && arm7_bios_prot == 0) arm7_bios_prot = value & 0xFFFE; return;   // BIOSPROT: write-once
  case 0x182:
    if (a9) { powcnt1 = value & 0x820F; nds_.gpu.set_powcnt(powcnt1); }
    else { const u16 old = powcnt2; powcnt2 = value & 0x0003; nds_.spu.set_powcnt2(powcnt2); if ((old ^ powcnt2) & 2) { nds_.bus.update_wifi_timings(); wifi.update_power_on(); } }
    return;
  default: break;
  }
  if (addr >= 0x04000240 && addr < 0x0400024A && a9) { vramcnt_store(addr, value, 2); return; }
  if (a9 && ((addr >= 0x04000290 && addr < 0x040002A0) || addr == 0x040002B8 || addr == 0x040002BA || addr == 0x040002BC || addr == 0x040002BE)) {
    const u32 cur = read32_special(cpu, addr & ~3u).value;
    write32_special(cpu, addr & ~3u, (addr & 2) ? ((cur & 0x0000FFFF) | (static_cast<u32>(value) << 16)) : ((cur & 0xFFFF0000) | value));
    return;
  }
}

u8 Io::read8(Cpu cpu, u32 addr) {
  const bool a9 = cpu == Cpu::ARM9;
  if (a9 && addr >= 0x04000240 && addr < 0x0400024A) { const u32 k = addr - 0x04000240; return k == 7 ? wramcnt : vramcnt[k < 7 ? k : k - 1]; }
  if (!a9 && addr == 0x04000241) return wramcnt;
  if (!a9 && addr == 0x04000240) return static_cast<u8>((((vramcnt[2] & 7) == 2) ? 1 : 0) | (((vramcnt[3] & 7) == 2) ? 2 : 0));
  if (addr == 0x04000300) return static_cast<u8>(cpu_io[ci(cpu)].postflg);
  if (!a9 && addr == 0x04000138) return static_cast<u8>(rtc_read());
  if (!a9 && addr == 0x04000139) return static_cast<u8>(rtc_read() >> 8);
  if (!a9 && addr == 0x040001C2) return spidata;
  const u16 v = read16(cpu, addr & ~1u);
  return static_cast<u8>((addr & 1) ? (v >> 8) : v);
}

// VRAMCNT A-I and WRAMCNT, `n` bytes from `addr`. A 32-bit store to 0x240
// changes up to four banks at once; the remap runs once for the store.
void Io::vramcnt_store(u32 addr, u32 value, u32 n) {
  static const bool dbg_vramcnt = std::getenv("DS_DEBUG_VRAMCNT") != nullptr;
  bool vram_changed = false;
  for (u32 i = 0; i < n; ++i, ++addr, value >>= 8) {
    if (addr < 0x04000240 || addr >= 0x0400024A) continue;
    const u32 k = addr - 0x04000240;
    const u8 b = static_cast<u8>(value);
    // WRAMCNT. On DSi the NWRAM windows sit on top of this region, so
    // update_nwram must re-lay shared WRAM then the NWRAM overlay, in order.
    if (k == 7) { if (wramcnt != (b & 3)) { wramcnt = b & 3; if (nds_.dsi) nds_.bus.update_nwram(); else nds_.bus.update_wram(); } continue; }
    const u32 bank = k < 7 ? k : k - 1;                 // 0x248/0x249 are banks H/I
    if (dbg_vramcnt) std::fprintf(stderr, "[vramcnt] %c = %02x frame %llu line %u pc %08x\n", 'A' + bank, b, (unsigned long long)nds_.frame_count, nds_.gpu.line(), nds_.cpu(Cpu::ARM9).hot.regs[15]);
    if (vramcnt[bank] != b) { vramcnt[bank] = b; vram_changed = true; }
  }
  if (vram_changed) nds_.bus.update_vram();
}

void Io::write8(Cpu cpu, u32 addr, u8 value) {
  const bool a9 = cpu == Cpu::ARM9;
  if (a9 && addr >= 0x04000240 && addr < 0x0400024A) { vramcnt_store(addr, value, 1); return; }
  if (addr == 0x04000300) { write16(cpu, addr, value); return; }
  if (!a9 && addr == 0x04000301) {                       // HALTCNT
    const u8 v = value & 0xC0;
    if (v == 0x80 || v == 0xC0) {
      // Halting with an interrupt already pending is a no-op: `halted` only
      // clears on the IRQ edge in update_irq, so without this test the ARM7
      // would sleep through an already-pending interrupt.
      CpuIo& c7 = cpu_io[ci(Cpu::ARM7)];
      bool pending = (c7.ie & c7.if_) != 0;
      if (nds_.dsi && (dsi.ie2 & dsi.if2)) pending = true;
      if (!pending) nds_.cpu(Cpu::ARM7).halted = true;   // the run loop ends the slice
    }
    else if (v == 0x40) std::fprintf(stderr, "[io] GBA mode requested; not supported\n");
    return;
  }
  if (!a9 && addr == 0x04000138) { rtc_write(value, true); return; }
  if (!a9 && addr == 0x04000139) { rtc_write(static_cast<u16>((rtc.io & 0xFF) | (value << 8)), false); return; }
  if (!a9 && addr == 0x040001C2) { spi_write_data(value); return; }
  // Generic byte write: read-modify-write the halfword.
  const u32 base = addr & ~1u;
  u16 cur = read16(cpu, base);
  if (addr & 1) cur = static_cast<u16>((cur & 0x00FF) | (value << 8)); else cur = static_cast<u16>((cur & 0xFF00) | value);
  write16(cpu, base, cur);
}


// ---- DSi system registers (0x04004xxx) ----
// GPIO 0x4C00 is a plain register file; I2C 0x4500 talks to the BPTWL only.

// ---- I2C host + BPTWL ----
void Io::bptwl_reset() {
  dsi.i2c_cnt = dsi.i2c_data = dsi.i2c_device = 0;
  dsi.bptwl_pos = 0xFFFFFFFF;
  u8* r = dsi.bptwl_regs;
  std::memset(r, 0x5A, 0x100);
  r[0x00] = 0x33; r[0x01] = 0x00; r[0x02] = 0x50;
  r[0x10] = 0x00;                 // IRQ flags
  r[0x11] = 0x00;                 // reset
  r[0x12] = 0x00;                 // IRQ mode
  r[0x20] = 0x8F; r[0x21] = 0x07; // battery: charging, full
  r[0x30] = 0x13; r[0x31] = 0x00; // camera power
  r[0x40] = 0x1F;                 // volume
  r[0x41] = 0x04;                 // backlight
  r[0x60] = 0x00; r[0x61] = 0x01; r[0x62] = 0x50; r[0x63] = 0x00;
  for (u32 i = 0x70; i <= 0x77; ++i) r[i] = 0x00;   // 0x70 = boot flag
  r[0x80] = 0x10; r[0x81] = 0x64;
}

u8 Io::bptwl_read(bool last) {
  const u8 v = dsi.bptwl_regs[dsi.bptwl_pos & 0xFF];
  if ((dsi.bptwl_pos & 0xFF) == 0x10) dsi.bptwl_regs[0x10] = 0;   // IRQ flags clear on read
  dsi.bptwl_pos++;
  if (last) dsi.bptwl_pos = 0xFFFFFFFF;
  return v;
}

void Io::bptwl_write(u8 value, bool last) {
  if (last) { dsi.bptwl_pos = 0xFFFFFFFF; return; }
  if (dsi.bptwl_pos == 0xFFFFFFFF) { dsi.bptwl_pos = value; return; }
  const u32 p = dsi.bptwl_pos & 0xFF;
  if (p == 0x11 && value == 0x01) {
    // Soft reset: register file left alone, 0x70 is the warm-boot flag this
    // reset exists to carry.
    nds_.dsi_soft_reset_pending = true;
    nds_.cpu(Cpu::ARM7).halted = true;
    dsi.bptwl_pos = 0xFFFFFFFF;
    return;
  }
  if (p == 0x40) value &= 0x1F;
  if (p == 0x41 && value > 4) value = 4;
  if (p == 0x11 || p == 0x12 || p == 0x21 || p == 0x30 || p == 0x31 || p == 0x40 || p == 0x41 || p == 0x60 || p == 0x63 ||
      (p >= 0x70 && p <= 0x77) || p == 0x80 || p == 0x81)
    dsi.bptwl_regs[p] = value;
  dsi.bptwl_pos++;
}

void Io::i2c_write_cnt(u8 value) {
  if (value & 0x80) {
    const bool last = value & 0x01;
    const u8 dev = dsi.i2c_device;   // only BPTWL and the two cameras answer on this bus
    DsiCamera* camdev = dev == 0x78 ? &cam.camera(0) : dev == 0x7A ? &cam.camera(1) : nullptr;
    if (value & 0x20) {                       // read
      value &= 0xF7;
      dsi.i2c_data = dev == 0x4A ? bptwl_read(last) : camdev ? camdev->read(last) : 0xFF;
    } else {                                  // write
      value &= 0xE7;
      bool ack = true;
      if (value & 0x02) {                     // start: the byte is the device address
        dsi.i2c_device = dsi.i2c_data & 0xFE;
        const u8 d = dsi.i2c_device;
        if (d == 0x78) cam.camera(0).acquire();
        else if (d == 0x7A) cam.camera(1).acquire();
        else if (d != 0x4A) ack = false;
      } else if (dev == 0x4A) bptwl_write(dsi.i2c_data, last);
      else if (camdev) camdev->write(dsi.i2c_data, last);
      else ack = false;

      if (ack) value |= 0x10;
    }
    value &= 0x7F;
  }
  dsi.i2c_cnt = value;
}

void Io::grid_rtc_event(NDS& nds, u32) {
  Io& io = nds.io;
  const u32 sysclock = 33513982u + io.rtc.clock_err;
  const u32 delay = sysclock >> 15;
  io.rtc.clock_err = sysclock & 0x7FFF;
  nds.sched.schedule(EventId::RtcClock, nds.sched.event_time() + static_cast<u64>(delay) * 2, grid_rtc_event, 0);
}

void Io::dsi_reset() {
  rtc.clock_err = 33513982u & 0x7FFF;   // RTC's first tick is one 32768 Hz period out
  nds_.sched.schedule(EventId::RtcClock, static_cast<u64>(33513982u >> 15) * 2, grid_rtc_event, 0);
  // Half-BIOS-dump (boot2-from-NAND) reset shape; direct boot is the only
  // boot path here either way.
  dsi.scfg_bios = 0x0101;
  dsi.scfg_clock9 = 0x0187; dsi.scfg_clock7 = 0x0187;
  dsi.scfg_ext[0] = 0x8307F100; dsi.scfg_ext[1] = 0x93FFFB06;
  dsi.scfg_mc = static_cast<u16>(0x0010 | (nds_.cart ? 0 : 1));
  dsi.cart_insert_delay = dsi.cart_poweroff_delay = 0xFFFF;
  dsi.scfg_rst = 0;
  std::memset(dsi.mbk, 0, sizeof dsi.mbk);
  dsi.ie2 = dsi.if2 = 0; dsi.sndexcnt = 0;
  dsi.mic_cnt = 0; dsi.mic_rd = dsi.mic_wr = dsi.mic_level = 0; dsi.mic_divider = dsi.mic_temp_count = 0; dsi.mic_temp = 0;
  arm7_bios_prot = 0x20;
  bptwl_reset();
  cam.camera(0).reset(); cam.camera(1).reset();   // I2C host resets cameras first
  cam.reset();                                    // then the module (arms the camera IRQ)
  spi_flag_mode_ = true;
  dsi.console_id = nds_.dsi_nand.valid() ? nds_.dsi_nand.console_id() : 0;   // must precede aes.reset(): seeds its key slots
  sd.attach_nand(nds_.dsi_nand.valid() ? &nds_.dsi_nand : nullptr);
  sd.attach_sd(nds_.dsi_sd.valid() ? &nds_.dsi_sd : nullptr);
  sd.reset();
  sdio.reset();
  aes.reset();
  dsp.set_rst_line(false);
  dispstat[0] |= 0x40; dispstat[1] |= 0x40;   // LCD init flag
  extkeyin &= ~(1u << 6);                     // pen-down key bit unused on a DSi
  dsi_tsc_reset();
}

void Io::dsi_tsc_reset() {
  SpiTouch& t = spi_tsc;
  t.dsi_pos = 0; t.dsi_bank = 0; t.dsi_index = 0; t.dsi_mode = 1;
  t.dsi_bank3.fill(0);
  t.dsi_bank3[0x02] = 0x18; t.dsi_bank3[0x03] = 0x87; t.dsi_bank3[0x04] = 0x22; t.dsi_bank3[0x05] = 0x04;
  t.dsi_bank3[0x06] = 0x20; t.dsi_bank3[0x09] = 0x40; t.dsi_bank3[0x0E] = 0xAD; t.dsi_bank3[0x0F] = 0xA0;
  t.dsi_bank3[0x10] = 0x88; t.dsi_bank3[0x11] = 0x81;
  t.dsi_tx = t.dsi_ty = 0;
  t.dsi_data = 0;
}

bool Io::dsi_io_access(Cpu cpu, u32 addr) const {
  const u32 ext = dsi.scfg_ext[ci(cpu)];
  if (cpu == Cpu::ARM9) {
    switch (addr & 0xF00) {
    case 0x000: return ext & (1u << 31);
    case 0x100: return ext & (1u << 16);
    case 0x200: return ext & (1u << 17);
    case 0x300: return ext & (1u << 18);
    default: return true;
    }
  }
  switch (addr & 0xF00) {
  case 0x000: return ext & (1u << 31);
  case 0x100: return ext & (1u << 16);
  case 0x400: return ext & (1u << 17);
  case 0x500: return ext & (1u << 22);
  case 0x600: return ext & (1u << 20);
  case 0x700: return ext & (1u << 21);
  case 0x800: case 0x900: return ext & (1u << 18);
  case 0xA00: case 0xB00: return ext & (1u << 19);
  case 0xC00: return ext & (1u << 23);
  case 0xD00: return !(dsi.scfg_bios & (1u << 10));
  default: return true;
  }
}

// NDMA's ARM7 FIFO ends, reached without the read/write dispatch: a NAND
// title load moves every byte through these (SD data FIFO -> AES -> RAM).
u32 Io::ndma_read7(u32 addr) {
  spi_poll_streak_ = 0;
  return addr == 0x0400490C ? sd.read_fifo32() : aes.read_output_fifo();
}
void Io::ndma_write7_aes(u32 value) {
  spi_poll_streak_ = 0;
  aes.write_input_fifo(value);
}

u32 Io::dsi_read(Cpu cpu, u32 addr, u32 width) {
  if (!dsi_io_access(cpu, addr)) return 0;
  const bool a9 = cpu == Cpu::ARM9;
  const int c = ci(cpu);
  const u32 r = addr & 0xFFF;
  if (r >= 0x800 && r < 0xC00) {                     // SDMMC host (0x800) and SDIO host (0xA00), ARM7 only
    if (a9) return 0;
    SdHost& h = r < 0xA00 ? sd : sdio;
    if (width == 32) {
      if ((r & 0x1FF) == 0x10C) return h.read_fifo32();
      return h.read(addr) | (static_cast<u32>(h.read(addr + 2)) << 16);
    }
    const u32 v16 = h.read(addr & ~1u);
    return width == 16 ? v16 : ((v16 >> ((addr & 1) * 8)) & 0xFF);
  }
  if (r >= 0x400 && r < 0x500) {                     // AES (ARM7): 32-bit ports only, the FIFO read pops
    if (a9 || width != 32) return 0;
    if (r == 0x400) return aes.read_cnt();
    if (r == 0x40C) return aes.read_output_fifo();
    return 0;
  }
  if (r >= 0x200 && r < 0x300) {                     // camera module (ARM9); the image buffer read pops
    if (!a9) return 0;
    return width == 32 ? cam.read32(addr) : width == 16 ? cam.read16(addr) : cam.read8(addr);
  }
  if (r >= 0x300 && r < 0x400) {                     // DSP host interface (ARM9): PDATA reads pop its FIFO
    if (!a9) return 0;
    return width == 32 ? dsp.read32(r) : width == 16 ? dsp.read16(r) : dsp.read8(r);
  }
  if (r < 0x040) {
    // SCFG block: what's not listed reads 0 at that width; nothing composes
    // a wider view from narrower ones.
    const u16 bios = dsi.scfg_bios;
    if (a9) {
      if (width == 8)  return r == 0x000 ? (bios & 0xFF) : r == 0x006 ? (dsi.scfg_rst & 0xFF) : 0;
      if (width == 16) return r == 0x000 ? (bios & 0xFF) : r == 0x004 ? dsi.scfg_clock9 : r == 0x006 ? dsi.scfg_rst : r == 0x010 ? dsi.scfg_mc : 0;
      return r == 0x000 ? (bios & 0xFFu) : r == 0x004 ? (dsi.scfg_clock9 | (static_cast<u32>(dsi.scfg_rst) << 16)) : r == 0x008 ? dsi.scfg_ext[0] : r == 0x010 ? dsi.scfg_mc : 0u;
    }
    if (width == 8)  return r == 0x000 ? (bios & 0xFF) : r == 0x001 ? (bios >> 8) : 0;
    if (width == 16) return r == 0x000 ? bios : r == 0x004 ? dsi.scfg_clock7 : r == 0x010 ? dsi.scfg_mc : 0;
    return r == 0x000 ? static_cast<u32>(bios) : r == 0x008 ? dsi.scfg_ext[1] : r == 0x010 ? static_cast<u32>(dsi.scfg_mc) : 0u;
  }
  // Compose from the 32-bit view; every register here reads without side effects.
  auto word = [&](u32 base) -> u32 {
    switch (base) {
    case 0xD00: return static_cast<u32>(dsi.console_id);
    case 0xD04: return static_cast<u32>(dsi.console_id >> 32);
    case 0x000: return a9 ? (dsi.scfg_bios & 0xFF) : dsi.scfg_bios;               // ARM7 also sees 0x4002 SCFG_ROMWE as 0
    case 0x004: return a9 ? (dsi.scfg_clock9 | (static_cast<u32>(dsi.scfg_rst) << 16)) : dsi.scfg_clock7;   // ARM7 0x4006 (JTAG) reads 0
    case 0x008: return dsi.scfg_ext[c];
    case 0x010: return a9 ? (dsi.scfg_mc & 0xFFFF) : (dsi.scfg_mc | (static_cast<u32>(dsi.cart_insert_delay) << 16));
    case 0x014: return a9 ? 0 : dsi.cart_poweroff_delay;
    case 0x040: case 0x044: case 0x048: case 0x04C: case 0x050: case 0x054: case 0x058: case 0x05C: case 0x060:
      return dsi.mbk[c][(base - 0x040) >> 2];
    case 0x700: return a9 ? 0 : dsi.sndexcnt;
    case 0x600: return a9 ? 0 : dsi_mic_read_cnt();
    case 0xC00: return (a9 || width == 32) ? 0 : (dsi.gpio_data | (static_cast<u32>(dsi.gpio_dir) << 8) | (static_cast<u32>(dsi.gpio_iedgesel) << 16) | (static_cast<u32>(dsi.gpio_ie) << 24));
    case 0xC04: return (a9 || width == 32) ? 0 : dsi.gpio_wifi;   // melonDS has 8/16-bit GPIO handlers only
    case 0x500: return a9 ? 0 : (dsi.i2c_data | (static_cast<u32>(dsi.i2c_cnt) << 8));   // I2C_DATA / I2C_CNT
    default: break;
    }
    if (r >= 0x100 && r < 0x200) return nds_.ndma.read(cpu, addr & ~3u);
    return 0;
  };
  // MIC_DATA: every access takes a word from the FIFO, whatever its width.
  const u32 v = (!a9 && (r & ~3u) == 0x604) ? dsi_mic_read_data() : word(r & ~3u);
  if (width == 32) return v;
  if (width == 16) return (v >> ((addr & 2) * 8)) & 0xFFFF;
  return (v >> ((addr & 3) * 8)) & 0xFF;
}

void Io::dsi_write(Cpu cpu, u32 addr, u32 width, u32 value) {
  if (!dsi_io_access(cpu, addr)) return;
  const bool a9 = cpu == Cpu::ARM9;
  const u32 r = addr & 0xFFF;
  if (r >= 0x100 && r < 0x200) { if (width == 32) nds_.ndma.write(cpu, addr, value); return; }   // NDMA: 32-bit ports only
  if (r >= 0x800 && r < 0xC00) {                     // SDMMC host (0x800) and SDIO host (0xA00), ARM7 only
    if (a9) return;
    SdHost& h = r < 0xA00 ? sd : sdio;
    if (width == 32) {
      if ((r & 0x1FF) == 0x10C) { h.write_fifo32(value); return; }
      h.write(addr, static_cast<u16>(value & 0xFFFF));
      h.write(addr + 2, static_cast<u16>(value >> 16));
      return;
    }
    // No 8-bit SD handler; a byte write lands as the 16-bit one.
    h.write(addr & ~1u, static_cast<u16>(value));
    return;
  }
  if (r >= 0x400 && r < 0x500) {                     // AES: ARM7 only
    if (a9) return;
    const u32 shift = (r & (width == 8 ? 3 : width == 16 ? 2 : 0)) * 8;
    const u32 mask = (width == 32 ? 0xFFFFFFFFu : width == 16 ? 0xFFFFu : 0xFFu) << shift;
    const u32 v = value << shift, o = r & ~3u;
    if (o >= 0x420 && o < 0x430) { aes.write_iv(o - 0x420, v, mask); return; }
    if (o >= 0x430 && o < 0x440) { aes.write_mac(o - 0x430, v, mask); return; }
    if (o >= 0x440) {
      const u32 slot = (o - 0x440) / 0x30, k = (o - 0x440) % 0x30;
      if (k < 0x10) aes.write_key_normal(slot, k, v, mask);
      else if (k < 0x20) aes.write_key_x(slot, k - 0x10, v, mask);
      else aes.write_key_y(slot, k - 0x20, v, mask);
      return;
    }
    if (width == 32) {
      if (r == 0x400) aes.write_cnt(value);
      else if (r == 0x404) aes.write_blkcnt(value);
      else if (r == 0x408) aes.write_input_fifo(value);
    } else if (width == 16 && r == 0x406) aes.write_blkcnt(value << 16);
    return;
  }
  if (r >= 0x200 && r < 0x300) {                     // camera module (ARM9)
    if (!a9) return;
    if (width == 32) cam.write32(addr, value); else if (width == 16) cam.write16(addr, static_cast<u16>(value)); else cam.write8(addr, static_cast<u8>(value));
    return;
  }
  if (r >= 0x300 && r < 0x400) {                     // DSP host interface: ARM9, plus a 32-bit ARM7 path
    if (width == 32) dsp.write32(r, value);
    else if (a9) { if (width == 16) dsp.write16(r, static_cast<u16>(value)); else dsp.write8(r, static_cast<u8>(value)); }
    return;
  }
  if (r >= 0x040 && r < 0x054) {                     // MBK1-5: byte-granular slot maps, ARM9 only
    if (!a9) return;
    for (u32 i = 0; i < width / 8; ++i) {
      const u32 k = r + i - 0x040;                   // 0..3 A, 4..11 B, 12..19 C
      const u8 b = static_cast<u8>(value >> (i * 8));
      if (k < 4) mbk_map_slot(0, static_cast<int>(k), b);
      else if (k < 12) mbk_map_slot(1, static_cast<int>(k - 4), b);
      else mbk_map_slot(2, static_cast<int>(k - 12), b);
    }
    return;
  }
  if (r >= 0x500 && r < 0x502) {                     // I2C: byte registers, ARM7 only
    if (a9) return;
    for (u32 i = 0; i < width / 8 && r + i < 0x502; ++i) {
      const u8 b = static_cast<u8>(value >> (i * 8));
      if (r + i == 0x500) dsi.i2c_data = b; else i2c_write_cnt(b);
    }
    return;
  }
  if (r >= 0xC00 && r < 0xC06) {                     // GPIO: byte registers, ARM7 only
    if (a9 || width == 32) return;
    for (u32 i = 0; i < width / 8; ++i) {
      const u8 b = static_cast<u8>(value >> (i * 8));
      switch (r + i) {
      case 0xC00: dsi.gpio_data = b; break;
      case 0xC01: dsi.gpio_dir = b; break;
      case 0xC02: dsi.gpio_iedgesel = b; break;
      case 0xC03: dsi.gpio_ie = b; break;
      case 0xC04: dsi.gpio_wifi = b; break;                 // GPIO_WiFi is byte-wide: a 16-bit write truncates
      case 0xC05: break;                                    // no byte handler for the high byte
      default: break;
      }
    }
    return;
  }
  if (width == 8) {
    switch (r) {
    case 0x000: if (!a9) dsi.scfg_bios |= value & 0x03; break;
    case 0x001: if (!a9) dsi.scfg_bios |= (value & 0x07) << 8; break;
    case 0x006: if (a9) { dsi.scfg_rst = static_cast<u16>((dsi.scfg_rst & 0xFF00) | (value & 0xFF)); dsp.set_rst_line(value & 1); } break;
    case 0x010: if (!a9) dsi_write_scfg_mc(static_cast<u16>(value & 0xFF), 0x00FF); break;
    case 0x011: if (!a9) dsi_write_scfg_mc(static_cast<u16>((value & 0xFF) << 8), 0xFF00); break;
    case 0x060: case 0x061: case 0x062: case 0x063:
      if (!a9) { u32 t = dsi.mbk[0][8]; t &= ~(0xFFu << ((r & 3) * 8)); t |= (value & 0xFF) << ((r & 3) * 8); dsi.mbk[0][8] = dsi.mbk[1][8] = t & 0x00FFFF0F; }
      break;
    case 0x700: if (!a9) { const u16 nv = static_cast<u16>((dsi.sndexcnt & 0xFF00) | (value & 0xFF)); nds_.spu.write_sndexcnt(nv, 0x00FF); } break;
    case 0x600: if (!a9) dsi_mic_write_cnt(static_cast<u16>(value & 0xFF), 0x00FF); break;
    case 0x601: if (!a9) dsi_mic_write_cnt(static_cast<u16>((value & 0xFF) << 8), 0xFF00); break;
    case 0x701: if (!a9) { const u16 nv = static_cast<u16>((dsi.sndexcnt & 0x00FF) | ((value & 0xFF) << 8)); nds_.spu.write_sndexcnt(nv, 0xFF00); } break;
    default: break;
    }
    if (r <= 0x001 && !a9) nds_.bus.update_bios_map();
    return;
  }
  if (width == 16) {
    switch (r) {
    case 0x000: if (!a9) { dsi.scfg_bios |= value & 0x0703; nds_.bus.update_bios_map(); } break;
    case 0x004: if (a9) { nds_.sched.floor_arm9_clock(nds_.cpu(Cpu::ARM9)); dsi.scfg_clock9 = value & 0x0187; nds_.bus.set_clock9_shift((dsi.scfg_clock9 & 1) ? 2 : 1); reprice_clock9_store(1); } else dsi.scfg_clock7 = value & 0x0187; break;
    case 0x006: if (a9) { dsi.scfg_rst = static_cast<u16>(value); dsp.set_rst_line(value & 1); } break;
    case 0x010: if (!a9) dsi_write_scfg_mc(static_cast<u16>(value), 0xFFFF); break;
    case 0x012: if (!a9) dsi.cart_insert_delay = static_cast<u16>(value); break;
    case 0x014: if (!a9) dsi.cart_poweroff_delay = static_cast<u16>(value); break;
    case 0x060: case 0x062:
      if (!a9) { u32 t = dsi.mbk[0][8]; t &= ~(0xFFFFu << ((r & 3) * 8)); t |= (value & 0xFFFF) << ((r & 3) * 8); dsi.mbk[0][8] = dsi.mbk[1][8] = t & 0x00FFFF0F; }
      break;
    case 0x700: if (!a9) nds_.spu.write_sndexcnt(static_cast<u16>(value), 0xFFFF); break;
    case 0x600: if (!a9) dsi_mic_write_cnt(static_cast<u16>(value), 0xFFFF); break;
    default: break;
    }
    return;
  }
  switch (r) {
  case 0x000: if (!a9) { dsi.scfg_bios |= value & 0x0703; nds_.bus.update_bios_map(); } break;
  case 0x004: if (a9) { nds_.sched.floor_arm9_clock(nds_.cpu(Cpu::ARM9)); dsi.scfg_clock9 = value & 0x0187; dsi.scfg_rst = static_cast<u16>(value >> 16); nds_.bus.set_clock9_shift((dsi.scfg_clock9 & 1) ? 2 : 1); reprice_clock9_store(2); dsp.set_rst_line((value >> 16) & 1); } break;
  case 0x008: {
    const u32 old0 = dsi.scfg_ext[0], old1 = dsi.scfg_ext[1];
    if (a9) {
      dsi.scfg_ext[0] = (dsi.scfg_ext[0] & ~0x8007F19Fu) | (value & 0x8007F19Fu);
      dsi.scfg_ext[1] = (dsi.scfg_ext[1] & ~0x00003080u) | (value & 0x00003080u);
      if ((old0 ^ dsi.scfg_ext[0]) & (1u << 13)) nds_.bus.update_vram_timings();
      // Bits 14-15: main RAM, 4 MB (0-1) or 16 MB (2-3). Held until the ARM7
      // signals it's done clearing/moving main RAM (IPCSYNC 0, after 5 both ways).
      const bool handshake = (cpu_io[ci(Cpu::ARM9)].ipc_sync & 0x0F0F) == 0x0505;
      if ((old0 ^ dsi.scfg_ext[0]) & 0xC000u) {
        if (!handshake) dsi_apply_ram_size();
      }
    } else {
      dsi.scfg_ext[0] = (dsi.scfg_ext[0] & ~0x03000000u) | (value & 0x03000000u);
      dsi.scfg_ext[1] = (dsi.scfg_ext[1] & ~0x93FF0F07u) | (value & 0x93FF0F07u);
    }
    if (((old0 ^ dsi.scfg_ext[0]) | (old1 ^ dsi.scfg_ext[1])) & (1u << 25)) nds_.bus.update_nwram();
    break;
  }
  case 0x010: if (!a9) { dsi.cart_insert_delay = static_cast<u16>(value >> 16); dsi_write_scfg_mc(static_cast<u16>(value), 0xFFFF); } break;
  case 0x014: if (!a9) dsi.cart_poweroff_delay = static_cast<u16>(value); break;
  case 0x054: case 0x058: case 0x05C: mbk_map_range(cpu, static_cast<int>((r - 0x054) >> 2), value); break;
  case 0x060: if (!a9) dsi.mbk[0][8] = dsi.mbk[1][8] = value & 0x00FFFF0F; break;
  case 0x700: if (!a9) nds_.spu.write_sndexcnt(static_cast<u16>(value), 0xFFFF); break;
  case 0x600: if (!a9) dsi_mic_write_cnt(static_cast<u16>(value), 0xFFFF); break;
  default: break;
  }
}

// The store that wrote SCFG_CLK9 priced its data cycles from the old clock's
// table; re-read from the table this call just rebuilt. `idx`: 1 = 16-bit,
// 2 = 32-bit.
void Io::reprice_clock9_store(u32 idx) {
  CpuContext& a9 = nds_.cpu(Cpu::ARM9);
  if (nds_.sched.running() != &a9) return;
  a9.data_cycles = a9.timing9[0x04004004 >> 12][4 + idx];
}

// One MBK1-5 byte: slot `slot` of bank A (0), B (1) or C (2).
void Io::mbk_map_slot(int bank, int slot, u8 value) {
  if (getenv("DS_DEBUG_MBK")) std::fprintf(stderr, "[mbk] slot %c[%d] = %02x  t=%llu\n", "ABC"[bank], slot, value, (unsigned long long)nds_.sched.now());
  value &= bank == 0 ? ~0x72 : ~0x60;
  const u32 prot_bit = bank == 0 ? slot : bank == 1 ? 8 + slot : 16 + slot;
  if (dsi.mbk[0][8] & (1u << prot_bit)) return;
  const int reg = bank == 0 ? 0 : bank == 1 ? 1 + (slot >> 2) : 3 + (slot >> 2);
  const u32 sh = (bank == 0 ? slot : (slot & 3)) * 8;
  if (((dsi.mbk[0][reg] >> sh) & 0xFF) == value) return;
  dsi.mbk[0][reg] = (dsi.mbk[0][reg] & ~(0xFFu << sh)) | (static_cast<u32>(value) << sh);
  dsi.mbk[1][reg] = dsi.mbk[0][reg];
  nds_.bus.update_nwram(true);
}

// SCFG_EXT9's size becomes the machine's; SCFG_EXT7 mirrors it (read-only
// there). The mirror is what the bus maps from, so save state carries it.
void Io::dsi_apply_ram_size() {
  const u32 size = (dsi.scfg_ext[0] >> 14) & 3;
  const bool was16 = ((dsi.scfg_ext[1] >> 14) & 3) >= 2;
  dsi.scfg_ext[1] = (dsi.scfg_ext[1] & ~0xC000u) | (size << 14);
  if (was16 != (size >= 2)) nds_.bus.update_main_ram();
}

void Io::dsi_write_scfg_mc(u16 value, u16 mask) {
  const u16 old = dsi.scfg_mc;
  u16 mc = static_cast<u16>((old & (~mask | 0x0011)) | (value & mask & 0x80CC));
  for (int i = 0; i < 2; ++i) {
    const int shift = 2 + 4 * i;
    u16 oldpower = (old >> shift) & 3, newpower = (mc >> shift) & 3;
    if (newpower == oldpower) continue;
    const EventId ev = i == 0 ? EventId::CartPower1 : EventId::CartPower2;
    nds_.sched.cancel(ev);
    const bool inserted = i == 0 && nds_.cart != nullptr;   // no second slot to fill
    if ((newpower == 1 || newpower == 2) && !inserted) { oldpower = newpower; newpower = 3; }
    if (newpower == 3) nds_.sched.schedule(ev, nds_.sched.now() + (static_cast<u64>(dsi.cart_poweroff_delay) << 10), cart_power_event, static_cast<u32>(i));   // delay in ARM9 cycles
    if (newpower == 0 && i == 0) cart.romctrl &= ~(1u << 29);   // power state 0 releases the card's reset line
    if (i == 0 && (oldpower == 1 || oldpower == 2) && (newpower == 3 || newpower == 0)) {   // only slot 1 leaving powered states raises the card IRQ
      request_irq(Cpu::ARM9, IRQ_CART_IREQ);
      request_irq(Cpu::ARM7, IRQ_CART_IREQ);
    }
    mc = static_cast<u16>((mc & ~(3u << shift)) | (newpower << shift));
  }
  dsi.scfg_mc = mc;
  if ((old ^ mc) & 0x000C) update_cart_reset();
}

void Io::cart_power_event(NDS& nds, u32 slot) {
  const int shift = 2 + 4 * static_cast<int>(slot);
  nds.io.dsi.scfg_mc = static_cast<u16>(nds.io.dsi.scfg_mc & ~(3u << shift));
  if (slot == 0) { nds.io.cart.romctrl &= ~(1u << 29); nds.io.update_cart_reset(); }
}

void Io::update_cart_reset() {
  if (!nds_.cart) return;
  const bool powered = !nds_.dsi || ((dsi.scfg_mc >> 2) & 3) == 2;
  nds_.cart->set_reset(!powered || !(cart.romctrl & (1u << 29)));
}

// MBK6-8 for one CPU: its window over bank A/B/C.
void Io::mbk_map_range(Cpu cpu, int bank, u32 value) {
  value &= bank == 0 ? ~0xE00FC00Fu : ~0xE007C007u;
  u32& reg = dsi.mbk[ci(cpu)][5 + bank];
  if (reg == value) return;
  reg = value;
  if (getenv("DS_DEBUG_MBK")) {
    const u32 start = bank == 0 ? 0x03000000 + (((value >> 4) & 0xFF) << 16) : 0x03000000 + (((value >> 3) & 0x1FF) << 15);
    const u32 end   = bank == 0 ? 0x03000000 + (((value >> 20) & 0x1FF) << 16) : 0x03000000 + (((value >> 19) & 0x3FF) << 15);
    std::fprintf(stderr, "[mbk] range %c cpu%d val %08x -> %08x-%08x size %u  t=%llu\n",
                 "ABC"[bank], cpu == Cpu::ARM9 ? 9 : 7, value, start, end, (value >> 12) & 3,
                 (unsigned long long)nds_.sched.now());
  }
  nds_.bus.update_nwram(true);
}

// DSi CODEC SPI protocol: first byte of a transfer is the index (bit 0 =
// read), then one register per byte, the index advancing. Register 0 of
// every bank selects the bank; bank 3 holds the control/status file, bank
// 0xFC the coordinate FIFO, bank 0xFF the mode register (0 there drops back
// to DS-compatibility mode). SPIDATA reads back the output latch, updated
// only by a read of a modelled register.
u8 Io::dsi_tsc_transfer(u8 value) {
  SpiTouch& t = spi_tsc;
  if (t.dsi_pos == 0) { t.dsi_index = value; ++t.dsi_pos; return t.dsi_data; }
  const u8 id = t.dsi_index >> 1;
  const bool rd = t.dsi_index & 1;
  if (id == 0) { if (rd) t.dsi_data = t.dsi_bank; else t.dsi_bank = value; }
  else if (t.dsi_bank == 0x03) {
    if (rd) t.dsi_data = t.dsi_bank3[id];
    else if (id == 0x0D || id == 0x0E) t.dsi_bank3[id] = static_cast<u8>((t.dsi_bank3[id] & 0x03) | (value & 0xFC));
  } else if (t.dsi_bank == 0xFC && rd) {
    if (id < 0x0B) { t.dsi_data = (id & 1) ? static_cast<u8>(t.dsi_tx >> 8) : static_cast<u8>(t.dsi_tx); t.dsi_tx &= 0x7FFF; }
    else if (id < 0x15) { t.dsi_data = (id & 1) ? static_cast<u8>(t.dsi_ty >> 8) : static_cast<u8>(t.dsi_ty); t.dsi_ty &= 0x7FFF; }
    else t.dsi_data = 0;
  } else if (t.dsi_bank == 0xFF && id == 0x05) {
    if (rd) t.dsi_data = t.dsi_mode;
    else {
      t.dsi_mode = value;
      if (t.dsi_mode == 0) { t.dsi_pos = 0; extkeyin |= 1u << 6; return t.dsi_data; }   // DS mode: the pen-down key bit is live again (up until the next touch)
    }
  }
  t.dsi_index = static_cast<u8>(t.dsi_index + 2);
  ++t.dsi_pos;
  return t.dsi_data;
}

template <class S> void Io::sync_state(S& s) {
  s.begin("IO  ");
  s.fields(lcd_irq_pending, dispstat, vcount, wramcnt, vramcnt, powcnt1, powcnt2, keyinput, extkeyin, keycnt, exmemcnt, spicnt, spidata, arm7_bios_prot);
  for (CpuIo& c : cpu_io) {
    s.fields(c.ime, c.ie, c.if_, c.ipc_sync, c.ipc_fifo_cnt, c.fifo_out.data, c.fifo_out.head, c.fifo_out.count, c.fifo_out.last, c.postflg, c.dma_fill);
    for (Timer& t : c.timers) s.fields(t.reload, t.control, t.counter, t.start_time);
    if constexpr (S::reading) {
      static const bool dbg = std::getenv("DS_DEBUG_STATE") != nullptr;
      if (dbg) for (int i = 0; i < 4; ++i) { const Timer& t = c.timers[i];
        std::fprintf(stderr, "[state] arm%d timer%d reload %04x control %04x counter %04x start_time %llu (sched now %llu, delta %lld)\n",
                     (&c == &cpu_io[0]) ? 9 : 7, i, t.reload, t.control, t.counter, (unsigned long long)t.start_time,
                     (unsigned long long)nds_.sched.now(), (long long)(nds_.sched.now() - t.start_time)); }
    }
  }
  s.fields(spi_fw.hold, spi_fw.cmd, spi_fw.pos, spi_fw.addr, spi_fw.status, spi_fw.data);
  s.fields(spi_tsc.hold, spi_tsc.pos, spi_tsc.cmd, spi_tsc.sample, spi_tsc.data, spi_tsc.x, spi_tsc.y);
  s.fields(spi_pm.hold, spi_pm.pos, spi_pm.cmd, spi_pm.regs, spi_pm.data);
  s.fields(rtc.io, rtc.input, rtc.input_bit, rtc.input_pos, rtc.output, rtc.output_bit, rtc.output_pos, rtc.cmd,
           rtc.status1, rtc.status2, rtc.datetime, rtc.alarm1, rtc.alarm2, rtc.clock_adjust, rtc.free_reg);
  s.fields(cart.auxspicnt, cart.auxspidata, cart.romctrl, cart.cmd, cart.transfer_pos, cart.transfer_len, cart.fifo_count, cart.fifo, cart.fifo_head,
           cart.late, cart.next_word_at, cart.event_armed);
  s.fields(math.divcnt, math.sqrtcnt, math.div_num, math.div_den, math.div_quot, math.div_rem, math.sqrt_val, math.sqrt_res);
  wifi.sync_state_regs(s);
  bool fast_load_state = false;   // a fast-load cart transfer in flight, from older states
  s.fields(math.div_ready_at, math.sqrt_ready_at, math.div_pending, math.sqrt_pending, spi_ready_at, fast_load_state);   // appended: older states leave them at rest
  if constexpr (S::reading) {
    if (fast_load_state) {   // its pending Cart event is the transfer's end: resume per word instead
      nds_.sched.cancel(EventId::Cart); cart.event_armed = false;
      if (cart.transfer_pos < cart.transfer_len) { if (cart.fifo_count < 2) cart_schedule_receive(nds_.sched.now()); else cart.late = true; }
      else if (cart.fifo_count == 0) cart_end_transfer();
    }
  }
  s.fields(wifi_power_on_pending);
  s.fields(rcnt);   // appended
  s.fields(wifiwaitcnt);   // appended
  wifi.sync_state_timer(s);   // appended (DSi)
  wifi.sync_state_engine(s);  // appended (frames)
  if constexpr (S::reading) nds_.bus.update_wifi_timings();
  s.end();
  if (nds_.dsi) {
    s.begin("DSI ");
    s.fields(dsi.scfg_bios, dsi.scfg_clock9, dsi.scfg_clock7, dsi.scfg_rst, dsi.scfg_ext, dsi.scfg_mc, dsi.cart_insert_delay, dsi.cart_poweroff_delay,
             dsi.mbk, dsi.ie2, dsi.if2, dsi.sndexcnt, spi_busy_, dsi.gpio_data, dsi.gpio_dir, dsi.gpio_iedgesel, dsi.gpio_ie, dsi.gpio_wifi,
             dsi.i2c_cnt, dsi.i2c_data, dsi.i2c_device, dsi.bptwl_regs, dsi.bptwl_pos);
    s.fields(spi_tsc.dsi_mode, spi_tsc.dsi_bank, spi_tsc.dsi_index, spi_tsc.dsi_pos, spi_tsc.dsi_bank3, spi_tsc.dsi_tx, spi_tsc.dsi_ty, rtc.clock_err);
    nds_.ndma.sync_state(s);
    s.fields(dsi.console_id);   // appended
    aes.sync_state(s);          // appended
    sd.sync_state(s);           // appended
    dsp.sync_state(s);          // appended
    s.fields(spi_tsc.dsi_data); // appended
    sdio.sync_state(s);         // appended
    cam.sync_state(s);          // appended
    s.fields(dsi.mic_cnt, dsi.mic_fifo, dsi.mic_rd, dsi.mic_wr, dsi.mic_level, dsi.mic_divider, dsi.mic_temp_count, dsi.mic_temp);
    s.end();
    if constexpr (S::reading) { nds_.sched.rebind(EventId::RtcClock, grid_rtc_event); nds_.sched.rebind(EventId::CamIrq, DsiCamModule::irq_event); nds_.sched.rebind(EventId::CamTransfer, DsiCamModule::transfer_event);
                                 nds_.sched.rebind(EventId::SdMmc, SdHost::ev_transfer_mmc); nds_.sched.rebind(EventId::Sdio, SdHost::ev_transfer_sdio);
                                 nds_.sched.rebind(EventId::NWifi, NWifi::ms_timer_event);
                                 nds_.sched.rebind(EventId::CartPower1, cart_power_event); nds_.sched.rebind(EventId::CartPower2, cart_power_event); }
  }
  if constexpr (S::reading) {
    mic_ = nullptr; mic_count_ = 0; mic_start_ = 0;   // the frontend hands a new buffer every frame
    for (int i = 0; i < 4; ++i) {
      nds_.sched.rebind(static_cast<EventId>(static_cast<int>(EventId::Timer0) + i), timer_event);
      nds_.sched.rebind(static_cast<EventId>(static_cast<int>(EventId::Timer7_0) + i), timer_event);
    }
    nds_.sched.rebind(EventId::Spi, spi_event);
    nds_.sched.rebind(EventId::Cart, cart_ev);
    nds_.sched.rebind(EventId::Div, ev_div);
    nds_.sched.rebind(EventId::Sqrt, ev_sqrt);
    nds_.sched.rebind(EventId::LcdIrq, ev_lcd_irq);
    nds_.sched.rebind(EventId::Wifi, [](NDS& n, u32) { n.io.wifi.us_timer(); });
    // Clock is a session property, not state: this run's setting keeps
    // going, rebased onto the restored timeline.
    if (rtc.ticking) { rtc.next_tick = nds_.sched.now() + ARM9_CLOCK_HZ; nds_.sched.schedule(EventId::Rtc, rtc.next_tick, rtc_ev, 0); }
    else nds_.sched.cancel(EventId::Rtc);
  }
}
template void Io::sync_state<state::Writer>(state::Writer&);
template void Io::sync_state<state::Reader>(state::Reader&);

} // namespace ds::io
