// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"
#include "core/profile.h"
#include "core/io/dsi_aes.h"
#include "core/io/dsi_dsp.h"
#include "core/io/dsi_camera.h"
#include "core/io/dsi_sd.h"
#include "core/io/wifi.h"

#include <array>
#include <vector>

namespace ds { struct NDS; struct CpuContext; }

namespace ds::io {

// IE/IF bits.
enum Irq : u32 {
  IRQ_VBLANK = 0, IRQ_HBLANK = 1, IRQ_VCOUNT = 2,
  IRQ_TIMER0 = 3, IRQ_TIMER1 = 4, IRQ_TIMER2 = 5, IRQ_TIMER3 = 6,
  IRQ_RTC = 7, IRQ_DMA0 = 8, IRQ_DMA1 = 9, IRQ_DMA2 = 10, IRQ_DMA3 = 11,
  IRQ_KEYPAD = 12, IRQ_GBA_SLOT = 13,
  IRQ_IPC_SYNC = 16, IRQ_IPC_SEND_EMPTY = 17, IRQ_IPC_RECV = 18,
  IRQ_CART_DONE = 19, IRQ_CART_IREQ = 20, IRQ_GX_FIFO = 21,
  IRQ_LID = 22, IRQ_SPI = 23, IRQ_WIFI = 24,
};

struct Timer {
  u16 reload = 0;
  u16 control = 0;
  u16 counter = 0;
  u64 start_time = 0;     // when `counter` was last sampled
  bool running() const { return control & 0x80; }
  bool count_up() const { return control & 0x04; }
  u32 prescaler_shift() const { static const u8 s[4] = {0, 6, 8, 10}; return s[control & 3]; }
};

struct IpcFifo {
  std::array<u32, 16> data{};
  u32 head = 0, count = 0;
  u32 last = 0;             // returned on empty read
  bool empty() const { return count == 0; }
  bool full() const { return count == 16; }
  void push(u32 v) { data[(head + count) & 15] = v; ++count; }
  u32 pop() { u32 v = data[head]; head = (head + 1) & 15; --count; last = v; return v; }
  void clear() { head = count = 0; }
};

struct CpuIo {
  u32 ime = 0, ie = 0, if_ = 0;
  u16 ipc_sync = 0;       // bits 11:8 = our output
  u16 ipc_fifo_cnt = 0;   // bits 2 (send irq), 10 (recv irq), 14 (error), 15 (enable)
  IpcFifo fifo_out;
  std::array<Timer, 4> timers;
  u16 postflg = 0;
  std::array<u32, 4> dma_fill{};
};

struct SpiFirmware {
  bool hold = false; u8 cmd = 0; u32 pos = 0; u32 addr = 0; u8 status = 0; u8 data = 0;
};
struct SpiTouch { bool hold = false; u32 pos = 0; u8 cmd = 0; u16 sample = 0; u8 data = 0;
  u16 x = 0, y = 0xFFF;   // 12-bit ADC units (pixel << 4)
  // DSi CODEC (TSC2117-class) on the same select; dsi_mode 0 = DS-compatible.
  u8  dsi_mode = 0, dsi_bank = 0, dsi_index = 0; u32 dsi_pos = 0;
  std::array<u8, 0x80> dsi_bank3{};
  u16 dsi_tx = 0, dsi_ty = 0;
  u8  dsi_data = 0;   // output latch: only a handled read changes it
};
struct SpiPower { bool hold = false; u32 pos = 0; u8 cmd = 0; std::array<u8, 8> regs{}; u8 data = 0; };

struct Rtc {
  u16 io = 0;                   // RTC register (bit0 SIO, bit1 SCK, bit2 CS, bits 4-6 direction)
  u8  input = 0, input_bit = 0, input_pos = 0;
  u8  output[8] = {}; u8 output_bit = 0, output_pos = 0;
  u8  cmd = 0;
  u8  status1 = 0x82, status2 = 0;   // bit7: power was lost; bit1: 24-hour mode
  u8  datetime[7] = {0, 1, 1, 0, 0, 0, 0};   // yy mm dd dow hh mm ss (BCD)
  u8  alarm1[3] = {}, alarm2[3] = {};
  u8  clock_adjust = 0, free_reg = 0;
  // Off by default for determinism; see Io::start_rtc_clock.
  bool ticking = false;
  u64  next_tick = 0;           // next one-second carry
  u32  clock_err = 0;           // DSi: 33513982/32768 cycles per tick, error-diffused
};

// Slot-1 bus state (ROMCTRL, transfer timing, FIFO); the card is cart::Cart.
struct Cart {
  u16 auxspicnt = 0; u8 auxspidata = 0;
  u32 romctrl = 0;
  std::array<u8, 8> cmd{};
  u32 transfer_pos = 0, transfer_len = 0;   // bytes
  u32 fifo_count = 0;                       // 0..2
  u32 fifo[2] = {0, 0}; u32 fifo_head = 0;
  bool late = false;                        // FIFO was full; receive paused
  u64 next_word_at = 0;                     // nominal, not a slice end
  bool event_armed = false;                 // per-word event (DMA only)
};

// 0x04000280-0x040002BF.
struct MathUnit {
  u16 divcnt = 0, sqrtcnt = 0;              // bit 15 (busy) is derived, never stored; DIVCNT bit 14 = div by zero
  u64 div_num = 0, div_den = 0, div_quot = 0, div_rem = 0;
  u64 sqrt_val = 0; u32 sqrt_res = 0;
  // Result computed on first read at/after ready_at, not by event (events
  // wait for slice end, so a poll loop would spin).
  u64  div_ready_at = 0, sqrt_ready_at = 0;
  bool div_pending = false, sqrt_pending = false;
};

// DSi 0x04004000 page and ARM7 IE2/IF2.
struct DsiIo {
  u16 scfg_bios = 0;        // 0x04004000, set-only bits: 0/8 hide the upper BIOS halves, 1/9 select the DS images, 10 hides the console ID
  u16 scfg_clock9 = 0;      // 0x04004004 (ARM9): bit 0 = ARM9 at 134 MHz
  u16 scfg_clock7 = 0;      // 0x04004004 (ARM7)
  u16 scfg_rst = 0;         // 0x04004006 (ARM9): bit 0 = DSP reset line
  u32 scfg_ext[2] = {0, 0}; // 0x04004008 per CPU: I/O page gates (bits 16-24, 31), NWRAM enable (25), RAM size (14-15)
  u16 scfg_mc = 0;          // 0x04004010: cart slot power state
  u16 cart_insert_delay = 0, cart_poweroff_delay = 0;   // 0x04004012/14
  u32 mbk[2][9] = {};       // 0x04004040-60 per CPU view: [0..4] slot maps (shared), [5..7] this CPU's windows, [8] write protect (shared)
  u32 ie2 = 0, if2 = 0;     // 0x04000218/1C (ARM7): the DSi IRQ sources, mask 0x7FF7
  u16 sndexcnt = 0;         // 0x04004700 (ARM7): I2S enable (15), mute (14), 47.6 kHz (13), NITRO/DSP ratio (0-3)
  // MIC_CNT 0x04004600 (ARM7): 15 run, 0-1 sample of pair, 2-3 rate divider,
  // 12 clear FIFO, 13/14 half-full/overrun IRQ, 11 overrun, 8-10 FIFO status.
  u16 mic_cnt = 0;
  u32 mic_fifo[16] = {};
  u8  mic_rd = 0, mic_wr = 0, mic_level = 0;
  u8  mic_divider = 0, mic_temp_count = 0;
  s16 mic_temp = 0;
  // 0x04004C00-05 (ARM7) GPIO: plain registers.
  u8  gpio_data = 0xFF, gpio_dir = 0x80, gpio_iedgesel = 0, gpio_ie = 0;
  u16 gpio_wifi = 0;
  // 0x04004500/01 (ARM7) I2C; BPTWL at 0x4A (no delay, no IRQ, always ACK).
  u8  i2c_cnt = 0, i2c_data = 0, i2c_device = 0;
  u8  bptwl_regs[0x100] = {};
  u32 bptwl_pos = 0xFFFFFFFF;
  u64 console_id = 0;   // 0x04004D00: seeds AES key slots 1/3; 0 without NAND
};

enum Irq2 : u32 {
  IRQ2_GPIO18_0 = 0, IRQ2_GPIO18_1, IRQ2_GPIO18_2, IRQ2_UNUSED3, IRQ2_GPIO33_0, IRQ2_HEADPHONE, IRQ2_BPTWL,
  IRQ2_GPIO33_3, IRQ2_SDMMC, IRQ2_SD_DATA1, IRQ2_SDIO, IRQ2_SDIO_DATA1, IRQ2_AES, IRQ2_I2C, IRQ2_MIC_EXT,
};
enum IrqDsi : u32 { IRQ_DSI_DSP = 24, IRQ_DSI_CAMERA = 25, IRQ_DSI_CART2_DONE = 26, IRQ_DSI_CART2_IREQ = 27, IRQ_DSI_NDMA0 = 28 };

class Io {
public:
  explicit Io(NDS& nds);
  void reset();
  template <class S> void sync_state(S& s);

  u32  read (Cpu cpu, u32 addr, u32 width);
  void write(Cpu cpu, u32 addr, u32 width, u32 value);
  // DS_IO_CENSUS (census builds): paths bypassing write() must count too.
  static bool census_on() { return prof::census && census_env_on(); }
  u32  ndma_read7(u32 addr);         // 0x0400490C (SD FIFO) or 0x0400440C (AES out FIFO)
  void ndma_write7_aes(u32 value);  // 0x04004408

  void request_irq(Cpu cpu, u32 bit);
  // LCD IRQs lag their DISPSTAT flag by lcd_irq_delay ARM9 cycles so a
  // VCOUNT poll can see the match before the IRQ.
  void lcd_irq(Cpu cpu, u32 bit);
  void flush_lcd_irq();
  u32  lcd_irq_delay = 4;
  u32  lcd_irq_pending[2] = {0, 0};
  void update_irq(Cpu cpu);

  u16 dispstat[2] = {0, 0};
  u16 vcount = 0;
  void set_vcount(u16 line);
  void set_hblank(bool on);
  void set_vblank(bool on);

  CpuIo cpu_io[2];
  DsiIo dsi;               // only while NDS::dsi
  DsiAes aes;              // 0x04004400 (ARM7)
  DsiDsp dsp;              // 0x04004300 (ARM9) host interface only, no core
  SdHost sd;               // 0x04004800 (ARM7); NAND on port 1
  SdHost sdio;             // 0x04004A00 (ARM7); Wi-Fi on port 0
  DsiCamModule cam;        // 0x04004200 (ARM9); sensors on I2C 0x78/0x7A
  void request_irq2(u32 bit);
  void bptwl_reset();
  void reprice_clock9_store(u32 idx);
  void i2c_write_cnt(u8 value);
  u8   bptwl_read(bool last);
  void bptwl_write(u8 value, bool last);
  u8  wramcnt = 0;         // 0x04000247
  u8  vramcnt[9] = {};     // 0x04000240-0x04000249 (skipping 0x247)
  u16 powcnt1 = 0;         // 0x04000304 (ARM9)
  u16 powcnt2 = 0;         // 0x04000304 (ARM7)
  u16 keyinput = 0x03FF, extkeyin = 0x007F;   // 0 = held
  u16 keycnt[2] = {0, 0};                     // 0x04000132, per CPU

  // Touch position in screen pixels.
  enum Button : u32 {
    BTN_A = 0, BTN_B, BTN_SELECT, BTN_START, BTN_RIGHT, BTN_LEFT, BTN_UP, BTN_DOWN,
    BTN_R, BTN_L, BTN_X, BTN_Y, BTN_COUNT
  };
  void set_buttons(u32 pressed);
  void set_touch(int x, int y, bool down);
  void set_lid(bool closed);   // opening raises ARM7 IRQ 22, ending firmware sleep
  bool lid_closed() const { return extkeyin & 0x80; }
  // One frame of s16 mono mic samples, any rate, mapped across the frame by
  // time. Pointer must stay valid until the next call.
  void set_mic(const s16* samples, size_t count);
  u16  mic_sample() const;          // 12-bit ADC value after the PMIC amplifier
  s16  mic_at(u64 t) const;         // 0 without input
  // I2S sample clock is driven from the SPU mixer while SNDEXCNT enables it.
  u16  dsi_mic_read_cnt() const;
  void dsi_mic_write_cnt(u16 value, u16 mask);
  // SCFG_MC slot power: 0 off, 1 on/reset held, 2 on, 3 powering off.
  // An empty slot goes to 3 from any on state.
  void dsi_write_scfg_mc(u16 value, u16 mask);
  // Card /RES held while ROMCTRL bit 29 clear, or (DSi) slot not in power state 2.
  void update_cart_reset();
  void dsi_apply_ram_size();   // SCFG_EXT9 bits 14-15
  static void cart_power_event(NDS& nds, u32 slot);
  u32  dsi_mic_read_data();
  void dsi_mic_clock(s16 sample);
  // Game has sampled AUX at least once.
  bool mic_used() const { return mic_used_; }
  void update_key_irq();
  u16 exmemcnt = 0;
  u16 wifiwaitcnt = 0;       // 0x04000206 (ARM7): live only while POWCNT2 bit 1
  u16 rcnt = 0;                     // 0x04000134 (ARM7), readback only
  u16 spicnt = 0; u8 spidata = 0;   // bit 7 never stored, see spi_busy()
  u64 spi_ready_at = 0;
  SpiFirmware spi_fw; SpiTouch spi_tsc; SpiPower spi_pm;
  const s16* mic_ = nullptr; size_t mic_count_ = 0; u64 mic_start_ = 0;
  mutable bool mic_used_ = false;
  Rtc rtc;
  Cart cart;
  u16 arm7_bios_prot = 0;    // reads below this from outside the BIOS return garbage
  bool cart_drq() { cart_catch_up(); return (cart.romctrl & 0x00800000) != 0; }
  // Hot: every ROMCTRL/ROMDATA read.
  void cart_catch_up() { if (cart.transfer_pos < cart.transfer_len) cart_catch_up_slow(); }
  // ARM7 0x04800000-0x0480FFFF, gated on POWCNT2 bit 1.
  Wifi wifi;
  void set_net_driver(NetDriver* net);   // both radios; null detaches
  bool wifi_power_on_pending = false;    // unused; savestate layout
  void set_irq_line(Cpu cpu, u32 bit, bool on);   // level-sensitive (GX FIFO)
  MathUnit math;
  void div_start(); void div_done();
  void sqrt_start(); void sqrt_done();
  void div_settle()  { if (math.div_pending  && nds_sched_now() >= math.div_ready_at)  div_done(); }
  void sqrt_settle() { if (math.sqrt_pending && nds_sched_now() >= math.sqrt_ready_at) sqrt_done(); }
  u16  divcnt_read()  { div_settle();  return static_cast<u16>(math.divcnt  | (math.div_pending  ? 0x8000 : 0)); }
  u16  sqrtcnt_read() { sqrt_settle(); return static_cast<u16>(math.sqrtcnt | (math.sqrt_pending ? 0x8000 : 0)); }
  u64  nds_sched_now() const;

  u16 timer_value(Cpu cpu, int idx);
  void timer_overflow(Cpu cpu, int idx);

  void spi_done();
  // SPICNT bit 7: DSi clears it at the completion event; DS compares ready time.
  bool spi_busy() const { return spi_flag_mode_ ? spi_busy_ : nds_sched_now() < spi_ready_at; }
  bool spi_flag_mode_ = false, spi_busy_ = false;
  u16  spicnt_read() const { return static_cast<u16>(spicnt | (spi_busy() ? 0x0080 : 0)); }
  // After this many back-to-back busy reads the rest of the wait is charged to
  // the ARM7 budget instead of spun (DS_IDLE_SKIP=0 disables).
  static constexpr u32 SPI_POLL_STREAK = 4;
  u32 spi_poll_streak_ = 0;
  u16  spicnt_read_arm7();

  // DSi 0x04004xxx page.
  u32  dsi_read(Cpu cpu, u32 addr, u32 width);
  void dsi_write(Cpu cpu, u32 addr, u32 width, u32 value);
  void dsi_reset();
  void dsi_tsc_reset();
  void mbk_map_slot(int bank, int slot, u8 value);      // MBK1-5 byte
  void mbk_map_range(Cpu cpu, int bank, u32 value);     // MBK6-8
  bool dsi_io_access(Cpu cpu, u32 addr) const;   // SCFG_EXT gate: disabled pages read 0, drop writes
private:
  static bool census_env_on();
  u8   dsi_tsc_transfer(u8 value);
  NDS& nds_;
  u32  read16(Cpu cpu, u32 addr);
  void write16(Cpu cpu, u32 addr, u16 value);
  u8   read8(Cpu cpu, u32 addr);
  void write8(Cpu cpu, u32 addr, u8 value);
  void vramcnt_store(u32 addr, u32 value, u32 n);   // one remap per store
  // By value, not bool&: an address-taken local adds stack-protector cost to every Io access.
  struct Special { u32 value; bool handled; };
  Special write32_special(Cpu cpu, u32 addr, u32 value);
  Special read32_special(Cpu cpu, u32 addr);

  void ipc_sync_write(Cpu cpu, u16 value);
  void ipc_fifo_cnt_write(Cpu cpu, u16 value);
  void ipc_fifo_send(Cpu cpu, u32 value);
  u32  ipc_fifo_recv(Cpu cpu);
  u16  ipc_fifo_cnt_read(Cpu cpu);

  void timer_write_control(Cpu cpu, int idx, u16 value);
  void timer_schedule(Cpu cpu, int idx);

  void spi_write_data(u8 value);
  u8   spi_transfer(u8 value);
  void spi_release();

  void rtc_write(u16 value, bool byte);
  u16  rtc_read() const { return rtc.io; }
  void rtc_byte_in(u8 value);
  void rtc_cmd_read();
  void rtc_cmd_write(u8 value);
  void rtc_tick();
  void rtc_seed();              // from host local time
  bool rtc_host_clock_ = false; // survives reset()
  // status1 bit 7 (power lost) clears on guest read and stays clear across
  // reset(); otherwise every reboot re-enters the setup wizard.
  bool rtc_power_lost_seen_ = false;
public:
  void start_rtc_clock();   // host time; also clears power-lost; survives reset()
  bool rtc_host_clock() const { return rtc_host_clock_; }
  void rtc_event();
  static void grid_rtc_event(NDS& nds, u32);   // DSi interleave grid
private:

  void cart_write_romctrl(u32 value);
  u32  cart_read_data();
  void cart_end_transfer();
  void cart_receive_word(u64 at);
  void cart_schedule_receive(u64 from);
  void cart_catch_up_slow();
  bool cart_dma_armed() const;
  u32  cart_word_delay() const;
public:
  void cart_event(u32 param);
};

} // namespace ds::io
