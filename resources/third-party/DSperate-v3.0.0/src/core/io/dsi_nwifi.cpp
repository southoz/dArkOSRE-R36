// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/io/dsi_nwifi.h"
#include "core/bios/freebios.h"
#include "core/nds.h"
#include "core/state/state.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace ds::io {

namespace {
const u8 kCis0Head[] = {
  0x01, 0x03, 0xD9, 0x01, 0xFF,
  0x20, 0x04, 0x71, 0x02, 0x00, 0x02,
  0x21, 0x02, 0x0C, 0x00,
  0x22, 0x04, 0x00, 0x00, 0x08, 0x32,
  0x1A, 0x05, 0x01, 0x01, 0x00, 0x02, 0x07,
  0x1B, 0x08, 0xC1, 0x41, 0x30, 0x30, 0xFF, 0xFF, 0x32, 0x00,
  0x14, 0x00,
  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
  0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};
const u8 kCis1Head[] = {
  0x20, 0x04, 0x71, 0x02, 0x00, 0x02,
  0x21, 0x02, 0x0C, 0x00,
  0x22, 0x2A, 0x01,
  0x01, 0x11,
  0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x08,
  0x00, 0x00, 0xFF, 0x80,
  0x00, 0x00, 0x00,
  0x00, 0x01, 0x0A,
  0x00, 0x00, 0x00, 0x00,
  0x00, 0x00,
  0x00, 0x00, 0x00, 0x00,
  0x00, 0x01, 0x00, 0x01,
  0x00, 0x01, 0x00, 0x01,
  0x80, 0x01, 0x06,
  0x81, 0x01, 0x07,
  0x82, 0x01, 0xDF,
  0xFF,
  0x01,
};
const u8 kApMac[6] = {0x00, 0xF0, 0x77, 0x77, 0x77, 0x77};
constexpr u64 MS_TIMER_TICKS = 33611 * 2;                     // 1 ms, scheduler ticks

void put16(u8* p, u16 v) { std::memcpy(p, &v, 2); }
void put32(u8* p, u32 v) { std::memcpy(p, &v, 4); }
u16 get16(const u8* p) { u16 v; std::memcpy(&v, p, 2); return v; }
u32 get32(const u8* p) { u32 v; std::memcpy(&v, p, 4); return v; }
}  // namespace

NWifi::NWifi(NDS& nds, SdHost& host) : nds_(nds), host_(host) {
  // mb_[8] is an extra RX buffer.
  for (int i = 0; i < 8; ++i) mb_[i].e.assign(0x600, 0);
  mb_[8].e.assign(0x8000, 0);
  std::memcpy(cis0_, kCis0Head, sizeof kCis0Head);
  std::memcpy(cis1_, kCis1Head, sizeof kCis1Head);
}

void NWifi::reset() {
  transfer_cmd_ = 0xFFFFFFFF;
  rem_size_ = 0;
  f0_irq_enable_ = f0_irq_status_ = 0;
  f1_irq_enable_ = f1_irq_enable_cpu_ = f1_irq_enable_error_ = f1_irq_enable_counter_ = 0;
  f1_irq_status_ = f1_irq_status_cpu_ = f1_irq_status_error_ = f1_irq_status_counter_ = 0;
  window_data_ = window_read_addr_ = window_write_addr_ = 0;
  for (Fifo& f : mb_) f.clear();

  const auto& fw = nds_.firmware;
  const u8 board = fw.size() > 0x1FD ? fw[0x1FD] : 0;
  switch (board) {
  case 2:  rom_id_ = 0x23000024; chip_id_ = 0x0D000000; host_int_addr_ = 0x00520000; break;   // W024: AR6013
  case 3:  rom_id_ = 0x2300006F; chip_id_ = 0x0D000001; host_int_addr_ = 0x00520000; break;   // W028: AR6014 (3DS)
  default: rom_id_ = 0x20000188; chip_id_ = 0x02000001; host_int_addr_ = 0x00500400; break;   // W015: AR6002 (and unknown)
  }
  cis0_[9] = chip_id_ >= 0x0D000000;
  cis1_[4] = cis0_[9];

  std::memset(eeprom_, 0, sizeof eeprom_);
  put32(&eeprom_[0x000], 0x300);
  put16(&eeprom_[0x008], 0x8348);   // country code, hardcoded
  if (fw.size() >= 0x3C) std::memcpy(&eeprom_[0x00A], &fw[0x36], 6);
  put32(&eeprom_[0x010], 0x60000000);
  std::memset(&eeprom_[0x03C], 0xFF, 0x70);
  std::memset(&eeprom_[0x140], 0xFF, 0x8);
  u16 chk = 0xFFFF;
  for (int i = 0; i < 0x300; i += 2) chk ^= get16(&eeprom_[i]);
  put16(&eeprom_[0x004], chk);

  boot_phase_ = 0;
  error_mask_ = 0;
  scan_timer_ = 0;
  beacon_timer_ = 0x10A2220ULL;
  connection_status_ = 0;
  send_bss_info_ = true;
  probed_ssid_[0] = 0;
  nds_.sched.cancel(EventId::NWifi);
}

// ---- IRQs ----

void NWifi::update_irq() {
  f0_irq_status_ = 0;
  irq = false;
  if (f1_irq_status_ & f1_irq_enable_) f0_irq_status_ |= 1u << 1;
  if ((f0_irq_enable_ & 1) && (f0_irq_status_ & f0_irq_enable_)) irq = true;
  host_.set_card_irq();
}

void NWifi::update_irq_f1() {
  f1_irq_status_ = 0;
  if (!mb_[4].empty()) f1_irq_status_ |= 1u << 0;
  if (!mb_[5].empty()) f1_irq_status_ |= 1u << 1;
  if (!mb_[6].empty()) f1_irq_status_ |= 1u << 2;
  if (!mb_[7].empty()) f1_irq_status_ |= 1u << 3;
  if (f1_irq_status_counter_ & f1_irq_enable_counter_) f1_irq_status_ |= 1u << 4;
  if (f1_irq_status_cpu_ & f1_irq_enable_cpu_) f1_irq_status_ |= 1u << 6;
  if (f1_irq_status_error_ & f1_irq_enable_error_) f1_irq_status_ |= 1u << 7;
  update_irq();
}

void NWifi::set_irq_f1_counter(u32 n) { f1_irq_status_counter_ |= static_cast<u8>(1u << n); update_irq_f1(); }
void NWifi::clear_irq_f1_counter(u32 n) { f1_irq_status_counter_ &= static_cast<u8>(~(1u << n)); update_irq_f1(); }

// ---- SDIO functions ----

u8 NWifi::f0_read(u32 addr) {
  switch (addr) {
  case 0x00000: return 0x11;
  case 0x00001: return 0x00;
  case 0x00002: return 0x02;
  case 0x00003: return 0x02;
  case 0x00004: return f0_irq_enable_;
  case 0x00005: return f0_irq_status_;
  case 0x00008: return 0x17;
  case 0x00009: return 0x00;
  case 0x0000A: return 0x10;
  case 0x0000B: return 0x00;
  case 0x00012: return 0x03;
  case 0x00109: return 0x00;
  case 0x0010A: return 0x11;
  case 0x0010B: return 0x00;
  default: break;
  }
  if (addr >= 0x01000 && addr < 0x01100) return cis0_[addr & 0xFF];
  if (addr >= 0x01100 && addr < 0x01200) return cis1_[addr & 0xFF];
  return 0;
}

void NWifi::f0_write(u32 addr, u8 val) {
  if (addr == 0x00004) { f0_irq_enable_ = val; update_irq(); }
}

u8 NWifi::f1_read(u32 addr) {
  auto mbox = [&](int n, bool drain) { const u8 r = mb_[n].read(); if (drain) drain_rx_buffer(); update_irq_f1(); return r; };
  if (addr < 0x100) return mbox(4, addr == 0xFF);
  if (addr < 0x200) return mbox(5, false);
  if (addr < 0x300) return mbox(6, false);
  if (addr < 0x400) return mbox(7, false);
  if (addr < 0x800) {
    switch (addr) {
    case 0x00400: return f1_irq_status_;
    case 0x00401: return f1_irq_status_cpu_;
    case 0x00402: return f1_irq_status_error_;
    case 0x00403: return f1_irq_status_counter_;
    case 0x00405: {
      u8 r = 0;
      for (int i = 0; i < 4; ++i) if (mb_[4 + i].occupied >= 4) r |= static_cast<u8>(1u << i);
      return r;
    }
    case 0x00408: return mb_[4].peek(0);
    case 0x00409: return mb_[4].peek(1);
    case 0x0040A: return mb_[4].peek(2);
    case 0x0040B: return mb_[4].peek(3);
    case 0x00418: return f1_irq_enable_;
    case 0x00419: return f1_irq_enable_cpu_;
    case 0x0041A: return f1_irq_enable_error_;
    case 0x0041B: return f1_irq_enable_counter_;
    case 0x00440: clear_irq_f1_counter(0); return 0;
    case 0x00450: return 1;
    case 0x00474: return static_cast<u8>(window_data_);
    case 0x00475: return static_cast<u8>(window_data_ >> 8);
    case 0x00476: return static_cast<u8>(window_data_ >> 16);
    case 0x00477: return static_cast<u8>(window_data_ >> 24);
    default: return 0;
    }
  }
  if (addr < 0x1000) return mbox(4, addr == 0xFFF);
  if (addr < 0x1800) return mbox(5, false);
  if (addr < 0x2000) return mbox(6, false);
  if (addr < 0x2800) return mbox(7, false);
  return mbox(4, addr == 0x3FFF);
}

void NWifi::f1_write(u32 addr, u8 val) {
  auto mbox = [&](int n, bool cmd) { mb_[n].write(val); if (cmd) handle_command(); update_irq_f1(); };
  if (addr < 0x100) return mbox(0, addr == 0xFF);
  if (addr < 0x200) return mbox(1, false);
  if (addr < 0x300) return mbox(2, false);
  if (addr < 0x400) return mbox(3, false);
  if (addr < 0x800) {
    switch (addr) {
    case 0x00418: f1_irq_enable_ = val; update_irq_f1(); return;
    case 0x00419: f1_irq_enable_cpu_ = val; update_irq_f1(); return;
    case 0x0041A: f1_irq_enable_error_ = val; update_irq_f1(); return;
    case 0x0041B: f1_irq_enable_counter_ = val; update_irq_f1(); return;
    case 0x00440: clear_irq_f1_counter(0); return;
    case 0x00474: window_data_ = (window_data_ & 0xFFFFFF00) | val; return;
    case 0x00475: window_data_ = (window_data_ & 0xFFFF00FF) | (static_cast<u32>(val) << 8); return;
    case 0x00476: window_data_ = (window_data_ & 0xFF00FFFF) | (static_cast<u32>(val) << 16); return;
    case 0x00477: window_data_ = (window_data_ & 0x00FFFFFF) | (static_cast<u32>(val) << 24); return;
    case 0x00478: window_write_addr_ = (window_write_addr_ & 0xFFFFFF00) | val; window_write(window_write_addr_, window_data_); return;
    case 0x00479: window_write_addr_ = (window_write_addr_ & 0xFFFF00FF) | (static_cast<u32>(val) << 8); return;
    case 0x0047A: window_write_addr_ = (window_write_addr_ & 0xFF00FFFF) | (static_cast<u32>(val) << 16); return;
    case 0x0047B: window_write_addr_ = (window_write_addr_ & 0x00FFFFFF) | (static_cast<u32>(val) << 24); return;
    case 0x0047C: window_read_addr_ = (window_read_addr_ & 0xFFFFFF00) | val; window_data_ = window_read(window_read_addr_); return;
    case 0x0047D: window_read_addr_ = (window_read_addr_ & 0xFFFF00FF) | (static_cast<u32>(val) << 8); return;
    case 0x0047E: window_read_addr_ = (window_read_addr_ & 0xFF00FFFF) | (static_cast<u32>(val) << 16); return;
    case 0x0047F: window_read_addr_ = (window_read_addr_ & 0x00FFFFFF) | (static_cast<u32>(val) << 24); return;
    default: return;
    }
  }
  if (addr < 0x1000) return mbox(0, addr == 0xFFF);
  if (addr < 0x1800) return mbox(1, false);
  if (addr < 0x2000) return mbox(2, false);
  if (addr < 0x2800) return mbox(3, false);
  return mbox(0, addr == 0x3FFF);
}

u8 NWifi::sdio_read(u32 func, u32 addr) {
  if (func == 0) return f0_read(addr);
  if (func == 1) return f1_read(addr);
  return 0;
}

void NWifi::sdio_write(u32 func, u32 addr, u8 val) {
  if (func == 0) f0_write(addr, val);
  else if (func == 1) f1_write(addr, val);
}

// ---- commands and block transfers ----

void NWifi::send_cmd(MmcCmd cmd, u32 param) {
  switch (cmd) {
  case MmcCmd::StopTransmission: return;
  case MmcCmd::IoRwDirect: {
    const u32 func = (param >> 28) & 7, addr = (param >> 9) & 0x1FFFF;
    u8 val;
    if (param & (1u << 31)) {
      val = static_cast<u8>(param);
      sdio_write(func, addr, val);
      if (param & (1u << 27)) val = sdio_read(func, addr);
    } else {
      val = sdio_read(func, addr);
    }
    host_.send_response(val | 0x1000u, true);
    return;
  }
  case MmcCmd::IoRwExtended: {
    transfer_cmd_ = param;
    transfer_addr_ = (param >> 9) & 0x1FFFF;
    if (param & (1u << 27)) rem_size_ = (param & 0x1FF) << 9;
    else { rem_size_ = param & 0x1FF; if (!rem_size_) rem_size_ = 0x200; }
    if (param & (1u << 31)) write_block(); else read_block();
    host_.send_response(0x1000, true);
    return;
  }
  case MmcCmd::SdioOpCond: host_.send_response(0x80FFFF00, true); return;
  case MmcCmd::GetRca:
  case MmcCmd::Select: host_.send_response(0, true); return;
  default: return;
  }
}

void NWifi::send_acmd(MmcAcmd, u32) {}

void NWifi::continue_transfer() {
  if (transfer_cmd_ & (1u << 31)) write_block(); else read_block();
}

void NWifi::read_block() {
  const u32 func = (transfer_cmd_ >> 28) & 7;
  u32 len = (transfer_cmd_ & (1u << 27)) ? 0x200 : rem_size_;
  len = host_.transferrable_len(len);
  u8 data[0x200];
  for (u32 i = 0; i < len; ++i) {
    data[i] = sdio_read(func, transfer_addr_);
    if (transfer_cmd_ & (1u << 26)) transfer_addr_ = (transfer_addr_ + 1) & 0x1FFFF;
  }
  len = host_.data_rx(data, len);
  if (rem_size_ > 0) rem_size_ -= len;
}

void NWifi::write_block() {
  const u32 func = (transfer_cmd_ >> 28) & 7;
  u32 len = (transfer_cmd_ & (1u << 27)) ? 0x200 : rem_size_;
  len = host_.transferrable_len(len);
  u8 data[0x200];
  if ((len = host_.data_tx(data, len))) {
    for (u32 i = 0; i < len; ++i) {
      sdio_write(func, transfer_addr_, data[i]);
      if (transfer_cmd_ & (1u << 26)) transfer_addr_ = (transfer_addr_ + 1) & 0x1FFFF;
    }
    if (rem_size_ > 0) rem_size_ -= len;
  }
}

void NWifi::handle_command() {
  switch (boot_phase_) {
  case 0: bmi_command(); return;
  case 1: htc_command(); return;
  case 2: wmi_command(); return;
  default: return;
  }
}

void NWifi::bmi_command() {
  const u32 cmd = mb_read32(0);
  switch (cmd) {
  case 0x01: {   // BMI_DONE
    eeprom_ready_ = 1;
    static const u8 ready[6] = {0x0A, 0x00, 0x08, 0x06, 0x16, 0x00};
    send_wmi_event(0, 0x0001, ready, 6);
    boot_phase_ = 1;
    return;
  }
  case 0x03: {   // BMI_WRITE_MEMORY: accepted, discarded
    mb_read32(0);
    const u32 len = mb_read32(0);
    for (u32 i = 0; i < len; ++i) mb_[0].read();
    return;
  }
  case 0x04: mb_read32(0); mb_read32(0); mb_write32(4, 0); return;      // BMI_EXECUTE
  case 0x06: { const u32 a = mb_read32(0); mb_write32(4, window_read(a)); return; }   // BMI_READ_SOC_REGISTER
  case 0x07: { const u32 a = mb_read32(0); const u32 v = mb_read32(0); window_write(a, v); return; }
  case 0x08:     // BMI_GET_TARGET_ID
    mb_write32(4, 0xFFFFFFFF); mb_write32(4, 0x0000000C); mb_write32(4, rom_id_); mb_write32(4, 0x00000002);
    return;
  case 0x0D: mb_read32(0); return;                                       // BMI_LZ_STREAM_START
  case 0x0E: { const u32 len = mb_read32(0); for (u32 i = 0; i < len; ++i) mb_[0].read(); return; }   // BMI_LZ_DATA
  default: return;
  }
}

void NWifi::htc_command() {
  mb_read16(0);
  const u16 len = mb_read16(0);
  mb_read16(0);
  const u16 cmd = mb_read16(0);
  switch (cmd) {
  case 0x0002: {   // service connect
    const u16 svc = mb_read16(0);
    mb_read16(0); mb_read16(0);
    u8 resp[8];
    put16(&resp[0], svc);
    resp[2] = 0;
    resp[3] = static_cast<u8>((svc & 0xFF) + 1);
    put16(&resp[4], svc == 0x0100 ? 0x0602 : 0x0600);
    put16(&resp[6], 0);
    send_wmi_event(0, 0x0003, resp, 8);
    break;
  }
  case 0x0004: {   // setup complete
    u8 ready[12];
    std::memcpy(&ready[0], &eeprom_[0xA], 6);
    ready[6] = 0x02; ready[7] = 0;
    put32(&ready[8], 0x2300006C);
    send_wmi_event(1, 0x1001, ready, 12);
    u8 reg[4];
    put32(reg, 0x80000000u | (get16(&eeprom_[0x008]) & 0x0FFF));
    send_wmi_event(1, 0x1006, reg, 4);
    boot_phase_ = 2;
    nds_.sched.schedule(EventId::NWifi, nds_.sched.now() + MS_TIMER_TICKS, ms_timer_event, 0);
    break;
  }
  default:
    for (u32 i = 0; i < len; ++i) mb_[0].read();
    break;
  }
  mb_drain(0);
}

static bool nwifi_debug() { static const bool on = std::getenv("DS_DEBUG_NWIFI") != nullptr; return on; }

void NWifi::wmi_command() {
  const u16 h0 = mb_read16(0);
  const u16 len = mb_read16(0);
  mb_read16(0);
  const u8 ep = static_cast<u8>(h0);
  if (ep > 0x01) {
    wmi_send_packet(len);
  } else {
    const u16 cmd = mb_read16(0);
    if (nwifi_debug()) std::fprintf(stderr, "[nwifi] WMI cmd %04x len %u (frame %llu)\n", cmd, len, (unsigned long long)nds_.frame_count);
    switch (cmd) {
    case 0x0001: wmi_connect(); break;
    case 0x0003: {   // disconnect
      connection_status_ = 0;
      u8 reply[11];
      put16(&reply[0], 3);
      std::memcpy(&reply[2], kApMac, 6);
      reply[8] = 3; reply[9] = 0; reply[10] = 0;
      send_wmi_event(1, 0x1003, reply, 11);
      break;
    }
    case 0x0004: mb_[0].read(); break;   // synchronize
    case 0x0005: break;                  // create priority stream
    case 0x0007: {                       // start scan
      mb_read32(0); mb_read32(0);
      const u32 scantime = mb_read32(0);
      mb_read32(0); mb_[0].read(); mb_[0].read();
      scan_timer_ = scantime * 8;
      break;
    }
    case 0x0008: break;                  // set scan params
    case 0x0009: mb_[0].read(); mb_[0].read(); mb_[0].read(); mb_[0].read(); mb_read32(0); break;   // set BSS filter
    case 0x000A: {                       // set probed SSID
      mb_[0].read();
      const u8 flags = mb_[0].read();
      const u8 l = mb_[0].read();
      char ssid[33] = {};
      for (int i = 0; i < l && i < 32; ++i) ssid[i] = static_cast<char>(mb_[0].read());
      std::memcpy(probed_ssid_, ssid, sizeof probed_ssid_);
      if (flags == 0) probed_ssid_[0] = 0;
      break;
    }
    case 0x000D: mb_[0].read(); break;   // set disconnect timeout
    case 0x000E: {                       // get channel list
      constexpr int nchan = 11;
      u8 reply[2 + nchan * 2 + 2];
      reply[0] = 0; reply[1] = nchan;
      for (int i = 0; i < nchan; ++i) put16(&reply[2 + i * 2], static_cast<u16>(2412 + i * 5));
      put16(&reply[2 + nchan * 2], 0);
      send_wmi_event(1, 0x000E, reply, 4 + nchan * 2);
      break;
    }
    case 0x0011: {                       // set channel params
      mb_[0].read(); mb_[0].read(); mb_[0].read();
      const u8 l = mb_[0].read();
      for (int i = 0; i < l && i < 32; ++i) mb_read16(0);
      break;
    }
    case 0x0012: mb_[0].read(); break;   // set power mode
    case 0x0017: mb_[0].read(); break;
    case 0x0022: error_mask_ = mb_read32(0); break;
    case 0x002E: {                       // extension
      const u32 ext = mb_read32(0);
      if (ext == 0x2008) {
        const u32 cookie = mb_read32(0), source = mb_read32(0);
        u8 reply[12];
        put32(&reply[0], 0x3007); put32(&reply[4], cookie); put32(&reply[8], source);
        send_wmi_event(1, 0x1010, reply, 12);
      }
      break;
    }
    case 0x003D: mb_[0].read(); break;   // set keepalive interval
    case 0x0041: mb_[0].read(); break;   // WMI_SET_WSC_STATUS_CMD
    case 0x0047: break;
    case 0x0048: mb_read32(0); mb_read32(0); mb_[0].read(); mb_[0].read(); break;
    case 0x0049: break;                  // host exit notify
    case 0xF000: mb_[0].read(); mb_[0].read(); mb_[0].read(); break;   // set bitrate
    default:
      for (int i = 0; i < static_cast<int>(len) - 2; ++i) mb_[0].read();
      break;
    }
  }
  if (h0 & (1u << 8)) send_wmi_ack(ep);
  mb_drain(0);
}

void NWifi::wmi_connect() {
  const u8 type = mb_[0].read(), auth11 = mb_[0].read(), auth = mb_[0].read();
  const u8 pcrypto = mb_[0].read(); mb_[0].read();
  const u8 gcrypto = mb_[0].read(); mb_[0].read();
  mb_[0].read();                                  // SSID length
  for (int i = 0; i < 32; ++i) mb_[0].read();     // SSID
  mb_read16(0);                                   // channel
  u8 bssid[6];
  put32(&bssid[0], mb_read32(0));
  put16(&bssid[4], mb_read16(0));
  mb_read32(0);                                   // flags
  if (type != 1 || auth11 != 1 || auth != 1 || pcrypto != 1 || gcrypto != 1 || std::memcmp(bssid, kApMac, 6)) return;
  u8 reply[20];
  put16(&reply[0], 2437);
  std::memcpy(&reply[2], kApMac, 6);
  put16(&reply[8], 128); put16(&reply[10], 128);
  put32(&reply[12], 0x01);
  reply[16] = 0x16; reply[17] = 0x2F; reply[18] = 0x16; reply[19] = 0;
  send_wmi_event(1, 0x1002, reply, 20);
  connection_status_ = 1;
}

// WMI data -> Ethernet. Header type 2 = sync, other nonzero types dropped; 802.3 length is big-endian.
void NWifi::wmi_send_packet(u16 len) {
  if (connection_status_ != 1) return;
  u16 hdr = mb_read16(0);
  hdr = static_cast<u16>((hdr >> 8) | (hdr << 8));
  const u16 type = hdr & 3;
  if (type == 2) return;
  if (type) { for (int i = 0; i < static_cast<int>(len) - 2; ++i) mb_[0].read(); return; }
  u8 dst[6], src[6];
  put32(&dst[0], mb_read32(0)); put16(&dst[4], mb_read16(0));
  put32(&src[0], mb_read32(0)); put16(&src[4], mb_read16(0));
  u16 plen = mb_read16(0);
  plen = static_cast<u16>((plen >> 8) | (plen << 8));
  if (plen > len - 16) return;
  const u32 h0 = mb_read32(0);
  const u16 h1 = mb_read16(0);
  if (h0 != 0x0003AAAA || h1 != 0x0000) return;
  const u16 ethertype = mb_read16(0);
  const int lan_len = (plen - 8) + 14;
  if (lan_len < 14 || lan_len > static_cast<int>(sizeof lan_)) return;
  std::memcpy(&lan_[0], dst, 6);
  std::memcpy(&lan_[6], src, 6);
  put16(&lan_[12], ethertype);
  for (int i = 14; i < lan_len; ++i) lan_[i] = mb_[0].read();
  if (net_) net_->send(lan_, lan_len);
}

// One frame per tick: skips frames not for us and our own echoes.
void NWifi::check_rx() {
  if (!net_ || !mb_[8].can_fit(2048)) return;
  for (int rxlen = net_->recv(lan_); rxlen > 0; rxlen = net_->recv(lan_)) {
    const bool broadcast = get32(&lan_[0]) == 0xFFFFFFFF && get16(&lan_[4]) == 0xFFFF;
    if (!broadcast && std::memcmp(&lan_[0], &eeprom_[0x00A], 6)) continue;
    if (!std::memcmp(&lan_[6], &eeprom_[0x00A], 6)) continue;
    if (rxlen < 14) continue;
    const int datalen = rxlen - 14;
    Fifo& rx = mb_[8];
    rx.write(2);      // endpoint
    rx.write(0x00);
    mb_write16(8, static_cast<u16>(16 + 8 + datalen));
    rx.write(0); rx.write(0);
    mb_write16(8, 0x80);
    mb_write32(8, get32(&lan_[0])); mb_write16(8, get16(&lan_[4]));
    mb_write32(8, get32(&lan_[6])); mb_write16(8, get16(&lan_[10]));
    const u16 plen = static_cast<u16>(datalen + 8);
    mb_write16(8, static_cast<u16>((plen >> 8) | (plen << 8)));
    mb_write16(8, 0xAAAA); mb_write16(8, 0x0003); mb_write16(8, 0x0000);
    mb_write16(8, get16(&lan_[12]));
    for (int i = 0; i < datalen; ++i) rx.write(lan_[14 + i]);
    drain_rx_buffer();
    return;
  }
}

void NWifi::send_wmi_event(u8 ep, u16 id, const u8* data, u32 len) {
  Fifo& rx = mb_[8];
  if (!rx.can_fit(6 + len + 2 + 8)) return;
  rx.write(ep); rx.write(0x02);
  mb_write16(8, static_cast<u16>(len + 2 + 8));
  rx.write(8); rx.write(0);
  mb_write16(8, id);
  for (u32 i = 0; i < len; ++i) rx.write(data[i]);
  static const u8 trailer[8] = {0x02, 0x06, 0, 0, 0, 0, 0, 0};
  for (u8 b : trailer) rx.write(b);
  drain_rx_buffer();
}

void NWifi::send_wmi_ack(u8 ep) {
  Fifo& rx = mb_[8];
  if (!rx.can_fit(6 + 12)) return;
  rx.write(0); rx.write(0x02);
  mb_write16(8, 0xC);
  rx.write(0xC); rx.write(0);
  rx.write(0x01); rx.write(0x02); rx.write(ep); rx.write(0x01);   // credit report
  static const u8 lookahead[8] = {0x02, 0x06, 0, 0, 0, 0, 0, 0};
  for (u8 b : lookahead) rx.write(b);
  drain_rx_buffer();
}

void NWifi::send_wmi_bss_info(u8 type, const u8* data, u32 len) {
  if (nwifi_debug()) std::fprintf(stderr, "[nwifi] BSS info %s, RX mailbox %u/%u\n", send_bss_info_ ? "sent" : "filtered", mb_[8].occupied, mb_[8].size());
  if (!send_bss_info_) return;
  Fifo& rx = mb_[8];
  if (!rx.can_fit(6 + len + 2 + 16)) return;
  rx.write(1); rx.write(0x00);
  mb_write16(8, static_cast<u16>(len + 2 + 16));
  rx.write(0xFF); rx.write(0xFF);
  mb_write16(8, 0x1004);
  mb_write16(8, 2437);
  rx.write(type); rx.write(0x1B);
  mb_write16(8, 0xFFBC);
  mb_write32(8, get32(&kApMac[0]));
  mb_write16(8, get16(&kApMac[4]));
  mb_write32(8, 0);
  for (u32 i = 0; i < len; ++i) rx.write(data[i]);
  drain_rx_buffer();
}

u32 NWifi::window_read(u32 addr) {
  if ((addr & 0xFFFF00) == host_int_addr_) {
    switch (addr & 0xFF) {
    case 0x54: return 0x1FFC00;       // EEPROM data base
    case 0x58: return eeprom_ready_;
    default: return 0;
    }
  }
  if ((addr & 0x1FFC00) == 0x1FFC00) return get32(&eeprom_[addr & 0x3FF]);
  switch (addr) {
  case 0x40EC: return chip_id_;
  case 0x40C0: return 2;              // SOC_RESET_CAUSE
  default: return 0;
  }
}

void NWifi::window_write(u32, u32) {}

void NWifi::drain_rx_buffer() {
  while (mb_[8].occupied >= 6) {
    const u16 len = static_cast<u16>(mb_[8].peek(2) | (mb_[8].peek(3) << 8));
    const u32 total = len + 6u;
    const u32 required = (total + 0x7F) & ~0x7Fu;
    if (!mb_[4].can_fit(required)) break;
    u32 i = 0;
    for (; i < total; ++i) mb_[4].write(mb_[8].read());
    for (; i < required; ++i) mb_[4].write(0);
  }
  update_irq_f1();
}

void NWifi::ms_timer_event(NDS& nds, u32) { nds.io.sdio.nwifi()->ms_timer(); }

void NWifi::ms_timer() {
  ++beacon_timer_;
  if (scan_timer_ > 0) {
    --scan_timer_;
    // Emulated AP answers scans even without a network backend.
    if (!(beacon_timer_ & 0x7F)) {
      static const u8 head[] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   // timestamp
        0x80, 0x00,                                       // beacon interval
        0x21, 0x00,                                       // capability
        0x01, 0x08, 0x82, 0x84, 0x8B, 0x96, 0x0C, 0x12, 0x18, 0x24,   // rates
        0x03, 0x01, 0x06,                                 // channel
        0x05, 0x04, 0x00, 0x00, 0x00, 0x00,               // TIM
      };
      const char* name = probed_ssid_[0] ? probed_ssid_ : bios::kAccessPointSsid;
      const size_t n = std::min<size_t>(std::strlen(name), 32);
      u8 beacon[sizeof head + 2 + 32];
      std::memcpy(beacon, head, sizeof head);
      beacon[sizeof head] = 0x00;                         // SSID element
      beacon[sizeof head + 1] = static_cast<u8>(n);
      std::memcpy(beacon + sizeof head + 2, name, n);
      send_wmi_bss_info(0x01, beacon, static_cast<u32>(sizeof head + 2 + n));
    }
    if (scan_timer_ == 0) { u8 status[4] = {}; send_wmi_event(1, 0x100A, status, 4); }
  }
  if (connection_status_ == 1) check_rx();
  nds_.sched.schedule(EventId::NWifi, nds_.sched.event_time() + MS_TIMER_TICKS, ms_timer_event, 0);
}

template <class S> void NWifi::sync_state(S& s) {
  for (Fifo& f : mb_) { s.fields(f.occupied, f.rd, f.wr); s.vec(f.e); }
  s.fields(transfer_cmd_, transfer_addr_, rem_size_, irq,
           f0_irq_enable_, f0_irq_status_, f1_irq_enable_, f1_irq_enable_cpu_, f1_irq_enable_error_, f1_irq_enable_counter_,
           f1_irq_status_, f1_irq_status_cpu_, f1_irq_status_error_, f1_irq_status_counter_,
           window_data_, window_read_addr_, window_write_addr_, rom_id_, chip_id_, host_int_addr_,
           eeprom_, eeprom_ready_, boot_phase_, error_mask_, scan_timer_, beacon_timer_, connection_status_, send_bss_info_);
  s.fields(probed_ssid_);
}
template void NWifi::sync_state<state::Writer>(state::Writer&);
template void NWifi::sync_state<state::Reader>(state::Reader&);

}  // namespace ds::io
