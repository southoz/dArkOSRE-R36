// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Port of melonDS's DSi_NWifi.cpp (GPL-3.0-or-later, melonDS team).
//
// The DSi's Atheros Wi-Fi module on the SDIO host's port 0: function 0 (CCCR + CIS), function 1
// (mailboxes, IRQ registers, diagnostic window), and the firmware protocol the ARM7 driver
// speaks through mailbox 0 -- BMI, then HTC service setup, then WMI. Everything answers
// instantly. A scan reports one built-in AP; connected, data frames go out as Ethernet frames
// through the NetDriver and come back on the 1 ms timer.
#pragma once
#include "core/io/dsi_sd.h"
#include "core/io/wifi_transport.h"
#include <vector>

namespace ds::io {

class NWifi : public SdDevice {
 public:
  NWifi(NDS& nds, SdHost& host);

  void reset() override;
  void send_cmd(MmcCmd cmd, u32 param) override;
  void send_acmd(MmcAcmd cmd, u32 param) override;
  void continue_transfer() override;

  static void ms_timer_event(NDS& nds, u32 param);
  void set_net_driver(NetDriver* net) { net_ = net; }
  template <class S> void sync_state(S& s);

 private:
  // A read of an empty FIFO returns the stale entry at the read position;
  // clear() zeroes only that entry.
  struct Fifo {
    std::vector<u8> e;   // heap-allocated: NDS lives on the stack in unit tests
    u32 occupied = 0, rd = 0, wr = 0;
    u32  size() const { return static_cast<u32>(e.size()); }
    void clear() { occupied = rd = wr = 0; e[0] = 0; }
    bool empty() const { return occupied == 0; }
    bool full() const { return occupied >= size(); }
    bool can_fit(u32 n) const { return occupied + n <= size(); }
    void write(u8 v) { if (full()) return; e[wr] = v; if (++wr >= size()) wr = 0; ++occupied; }
    u8   read() { const u8 v = e[rd]; if (empty()) return v; if (++rd >= size()) rd = 0; --occupied; return v; }
    u8   peek(u32 off) const { u32 p = rd + off; if (p >= size()) p -= size(); return e[p]; }
  };

  void update_irq();
  void update_irq_f1();
  void set_irq_f1_counter(u32 n);
  void clear_irq_f1_counter(u32 n);

  u8   f0_read(u32 addr);
  void f0_write(u32 addr, u8 val);
  u8   f1_read(u32 addr);
  void f1_write(u32 addr, u8 val);
  u8   sdio_read(u32 func, u32 addr);
  void sdio_write(u32 func, u32 addr, u8 val);

  void read_block();
  void write_block();

  void handle_command();
  void bmi_command();
  void htc_command();
  void wmi_command();
  void wmi_connect();
  void wmi_send_packet(u16 len);
  void send_wmi_event(u8 ep, u16 id, const u8* data, u32 len);
  void send_wmi_ack(u8 ep);
  void send_wmi_bss_info(u8 type, const u8* data, u32 len);
  void drain_rx_buffer();
  void ms_timer();
  void check_rx();

  u32  window_read(u32 addr);
  void window_write(u32 addr, u32 val);

  u16 mb_read16(int n) { u16 r = mb_[n].read(); r |= static_cast<u16>(mb_[n].read() << 8); return r; }
  u32 mb_read32(int n) { u32 r = mb_[n].read(); r |= static_cast<u32>(mb_[n].read()) << 8; r |= static_cast<u32>(mb_[n].read()) << 16; r |= static_cast<u32>(mb_[n].read()) << 24; return r; }
  void mb_write16(int n, u16 v) { mb_[n].write(static_cast<u8>(v)); mb_[n].write(static_cast<u8>(v >> 8)); }
  void mb_write32(int n, u32 v) { for (int i = 0; i < 4; ++i) mb_[n].write(static_cast<u8>(v >> (i * 8))); }
  void mb_drain(int n) { while (!mb_[n].empty()) mb_[n].read(); }

  NDS& nds_;
  SdHost& host_;
  NetDriver* net_ = nullptr;
  u8 lan_[2048] = {};
  u32 transfer_cmd_ = 0xFFFFFFFF, transfer_addr_ = 0, rem_size_ = 0;
  Fifo mb_[9];

  u8 f0_irq_enable_ = 0, f0_irq_status_ = 0;
  u8 f1_irq_enable_ = 0, f1_irq_enable_cpu_ = 0, f1_irq_enable_error_ = 0, f1_irq_enable_counter_ = 0;
  u8 f1_irq_status_ = 0, f1_irq_status_cpu_ = 0, f1_irq_status_error_ = 0, f1_irq_status_counter_ = 0;
  u32 window_data_ = 0, window_read_addr_ = 0, window_write_addr_ = 0;
  u32 rom_id_ = 0, chip_id_ = 0, host_int_addr_ = 0;
  u8  eeprom_[0x400] = {};
  u32 eeprom_ready_ = 0;   // set at BMI_DONE, never cleared on reset
  u32 boot_phase_ = 0, error_mask_ = 0, scan_timer_ = 0;
  u64 beacon_timer_ = 0;
  u32 connection_status_ = 0;
  bool send_bss_info_ = true;   // a probe always gets an answer under its own name
  char probed_ssid_[33] = {};   // network a directed scan asks for; empty: any
  u8  cis0_[256] = {}, cis1_[256] = {};
};

}  // namespace ds::io
