// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Follows melonDS's Wifi.cpp and WifiAP.cpp (GPL-3.0-or-later, melonDS team);
// the hardware-behaviour comments and measured values are theirs.
//
// RFSTATUS values:
//   0 initial, 1 waiting for incoming packets, 2 switching RX to TX, 3 TX,
//   4 switching TX to RX, 5 MP host data sent / waiting for replies,
//   6 RX, 7 switching RX reply to TX ack, 8 MP client sending reply /
//   MP host sending ack, 9 idle.
#include "core/io/wifi.h"
#include "core/io/io.h"
#include "core/nds.h"
#include "core/state/state.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ds::io {

namespace {
constexpr u8 kMpCmdMac[6]   = {0x03, 0x09, 0xBF, 0x00, 0x00, 0x00};
constexpr u8 kMpReplyMac[6] = {0x03, 0x09, 0xBF, 0x00, 0x00, 0x10};
constexpr u8 kMpAckMac[6]   = {0x03, 0x09, 0xBF, 0x00, 0x00, 0x03};
constexpr u8 kApMac[6]      = {0x00, 0xF0, 0x77, 0x77, 0x77, 0x77};
constexpr u8 kApChannel     = 6;

inline bool mac_equal(const u8* a, const u8* b) { return std::memcmp(a, b, 6) == 0; }
inline bool mac_broadcast(const u8* a) { static const u8 ff[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}; return mac_equal(a, ff); }
inline u16 ld16(const u8* p) { u16 v; std::memcpy(&v, p, 2); return v; }
inline u32 ld32(const u8* p) { u32 v; std::memcpy(&v, p, 4); return v; }
inline u64 ld64(const u8* p) { u64 v; std::memcpy(&v, p, 8); return v; }
inline void st16(u8* p, u16 v) { std::memcpy(p, &v, 2); }
inline void st32(u8* p, u32 v) { std::memcpy(p, &v, 4); }
inline void st64(u8* p, u64 v) { std::memcpy(p, &v, 8); }

// DS_WIFI_TRACE=<file>: log register accesses as "R|W addr value".
FILE* wifi_trace_file() {
  static FILE* f = [] { const char* p = std::getenv("DS_WIFI_TRACE"); return p ? std::fopen(p, "w") : nullptr; }();
  return f;
}
bool wifi_trace_time() { static const bool on = std::getenv("DS_WIFI_TRACE_TIME") != nullptr; return on; }   // DS_WIFI_TRACE_TIME: add the us timer
bool wifi_log_enabled() { static const bool on = std::getenv("DS_WIFI_LOG") != nullptr; return on; }
#define WIFI_LOG(...) do { if (wifi_log_enabled()) std::fprintf(stderr, "[wifi] " __VA_ARGS__); } while (0)
} // namespace

u16  Wifi::ram16(u32 a) const { return ld16(&ram_[a & 0x1FFE]); }
void Wifi::ram16(u32 a, u16 v) { st16(&ram_[a & 0x1FFE], v); }

// ---- reset -------------------------------------------------------------------
void Wifi::reset() {
  ram_.fill(0); io_.fill(0); bb_.fill(0); bb_ro_.fill(0); rf_.fill(0);
  on_ = false; random_ = 1;
  auto fixed = [&](u32 id, u8 v) { bb_[id] = v; bb_ro_[id] = 1; };
  fixed(0x00, 0x6D);
  for (u32 id : {0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x27, 0x4D, 0x5E, 0x5F, 0x60, 0x61, 0x66}) fixed(id, 0x00);
  fixed(0x5D, 0x01); fixed(0x64, 0xFF);
  for (u32 id = 0x69; id < 0x100; ++id) fixed(id, 0x00);

  // Chip ID, RF type and RF init values from the firmware header.
  const auto& fw = nds_.firmware;
  u8 console = 0xFF;
  rf_version_ = 2;
  rf_channel_index_.fill(0); rf_channel_data_.fill(0);
  if (fw.size() >= 0x200) {
    console = fw[0x1D];
    rf_version_ = fw[0x40];
    if (rf_version_ == 3) {
      rf_channel_index_[0] = fw[0x116]; rf_channel_index_[1] = fw[0x125];
      for (int i = 0; i < 14; ++i) { rf_channel_data_[i * 2] = fw[0x117 + i]; rf_channel_data_[i * 2 + 1] = fw[0x126 + i]; }
    } else {
      const u8* v = &fw[0xF2];   // Type2Config.InitialRF56Values
      rf_channel_index_[0] = v[2] >> 2; rf_channel_index_[1] = v[5] >> 2;
      for (int i = 0; i < 14; ++i) {
        rf_channel_data_[i * 2]     = v[i * 6 + 0] | (v[i * 6 + 1] << 8) | ((v[i * 6 + 2] & 3) << 16);
        rf_channel_data_[i * 2 + 1] = v[i * 6 + 3] | (v[i * 6 + 4] << 8) | ((v[i * 6 + 5] & 3) << 16);
      }
    }
  }
  cur_channel_ = 0;
  reg(W_ID) = (console == 0x20 || console == 0x35 || (nds_.dsi && console == 0x57)) ? 0xC340 : 0x1440;
  for (u32 a = 0x018; a < 0x01E; a += 2) reg(a) = 0xFFFF;   // MAC
  for (u32 a = 0x020; a < 0x026; a += 2) reg(a) = 0xFFFF;   // BSSID
  reg(W_PowerUS) = 0x0001;

  timer_err_ = 0; us_timestamp_ = us_counter_ = us_compare_ = 0;
  block_beacon_irq14_ = false;
  us_until_power_on_ = 0; cmd_counter_ = rx_counter_ = 0;

  tx_slots_.fill(TxSlot{}); tx_buffer_.fill(0);
  com_status_ = 0; tx_cur_slot_ = -1;
  rx_buffer_.fill(0); rx_buffer_ptr_ = 0; rx_time_ = 0; rx_halfword_time_mask_ = 0xFFFFFFFF;
  mp_reply_timer_ = 0; mp_client_mask_ = mp_client_fail_ = 0;
  mp_client_replies_.fill(0); mp_last_seqno_ = 0xFFFF;
  is_mp_ = is_mp_client_ = false; next_sync_ = rx_timestamp_ = 0;
  ap_reset();
}

// ---- timer -------------------------------------------------------------------
void Wifi::schedule_timer(bool first) {
  if (first) timer_err_ = 0;
  s32 cycles = 33513982 * kTimerInterval;   // ARM7 cycles per interval, error-diffused
  cycles -= timer_err_;
  const s32 delay = (cycles + 999999) / 1000000;
  timer_err_ = delay * 1000000 - cycles;
  const u64 base = first ? nds_.sched.now() : nds_.sched.event_time();
  nds_.sched.schedule(EventId::Wifi, base + static_cast<u64>(delay) * 2, [](NDS& n, u32) { n.io.wifi.us_timer(); }, 0);
}

void Wifi::update_power_on() {
  // POWCNT2 bit 1, and on DS W_POWER_US bit 0 clear (DSi DWM-W024 ignores it).
  bool on = (nds_.io.powcnt2 & 2) != 0;
  if (!nds_.dsi) on = on && (reg(W_PowerUS) & 1) == 0;
  if (on == on_) return;
  on_ = on;
  if (on) { schedule_timer(true); if (FILE* tf = wifi_trace_file()) std::fprintf(tf, "ON %llX\n", (unsigned long long)(nds_.sched.now() >> 1)); if (mp_) mp_->begin(); }
  else { nds_.sched.cancel(EventId::Wifi); if (mp_) mp_->end(); }
}

void Wifi::check_irq(u16 old_flags) {
  const u16 now = reg(W_IF) & reg(W_IE);
  if (old_flags == 0 && now != 0) nds_.io.request_irq(Cpu::ARM7, IRQ_WIFI);
}
void Wifi::set_irq(u32 irq) {
  const u16 old = reg(W_IF) & reg(W_IE);
  reg(W_IF) |= static_cast<u16>(1u << irq);
  check_irq(old);
}
void Wifi::set_irq13() {
  set_irq(13);
  if ((reg(W_ModeWEP) & 7) == 0 && !(reg(W_PowerTX) & 2)) update_power_status(-1);
}
void Wifi::set_irq14(int source) {   // 0 = USCOMPARE, 1 = BEACONCOUNT, 2 = forced
  if (source != 2) reg(W_BeaconCount1) = reg(W_BeaconInterval);
  if (block_beacon_irq14_ && source == 1) return;
  if (!(reg(W_USCompareCnt) & 1)) return;
  set_irq(14);
  reg(W_BeaconCount2) = 0xFFFF;
  reg(W_TXReqRead) &= 0xFFF2;
  if (reg(W_TXSlotBeacon) & 0x8000) start_tx_beacon();
  if (reg(W_ListenCount) == 0) reg(W_ListenCount) = reg(W_ListenInterval);
  reg(W_ListenCount)--;
}
void Wifi::set_irq15() {
  if (FILE* tf = wifi_trace_file()) std::fprintf(tf, "I15 %llX %04X %04X %llX\n", (unsigned long long)us_counter_, reg(W_BeaconCount1), reg(W_PreBeacon), (unsigned long long)us_timestamp_);
  set_irq(15);
  if (reg(W_PowerTX) & 1) update_power_status(1);
}

void Wifi::set_status(u32 status) {
  static const u16 rfpins[10] = {0x04, 0x84, 0, 0x46, 0, 0x84, 0x87, 0, 0x46, 0x04};
  reg(W_RFStatus) = static_cast<u16>(status);
  reg(W_RFPins) = rfpins[status < 10 ? status : 0];
}

void Wifi::update_power_status(int power) {
  // W_PowerForce overrides all else; W_ModeReset bit 0 clear forces off;
  // else IRQ13/15 or W_PowerState turn it on/off per W_ModeWEP's mode.
  int cur = 0;
  if (reg(W_TRXPower) == 1) cur |= 1;
  if (!(reg(W_PowerState) & 0x0200)) cur |= 2;
  int req = cur;
  if (reg(W_PowerForce) & 0x8000) req = (reg(W_PowerForce) & 1) ? 0 : 3;
  else if (!(reg(W_ModeReset) & 1)) req = 0;
  else {
    if (power == 0) {
      if ((reg(W_PowerState) & 0x0202) == 0x0202) power = 1;
      else if ((reg(W_PowerState) & 0x0201) == 0x0001) power = -1;
    }
    if (power == -1 && (reg(W_PowerDownCtrl) & 1)) power = 0;
    if (power == 1) req = 3;
    else if (power == -1) req = reg(W_PowerDownCtrl) ? 3 : 0;
    else if (reg(W_PowerDownCtrl) & 2) req = 3;
  }
  if (req == cur) return;
  if (req & 1) { if (!(cur & 1)) { reg(W_TRXPower) = 1; set_status(1); } }
  else {
    // transceiver turning off (checkme)
    reg(W_TRXPower) = 2;
    if (!com_status_) { reg(W_TRXPower) = 0; set_status(9); }
  }
  if (req & 2) {
    reg(W_PowerState) |= 0x0100;
    if (!(cur & 2) && us_until_power_on_ == 0) { us_until_power_on_ = -2048; set_irq(11); }   // 2048 us delay
  } else {
    reg(W_PowerState) &= static_cast<u16>(~0x0101);
    reg(W_PowerState) |= 0x0200;
    us_until_power_on_ = 0;
  }
}

void Wifi::ms_timer() {
  if (reg(W_USCompareCnt)) {
    if ((us_counter_ & ~u64{0x3FF}) == us_compare_) { block_beacon_irq14_ = false; set_irq14(0); }
  }
  if (reg(W_BeaconCount1) != 0) {
    reg(W_BeaconCount1)--;
    if (reg(W_BeaconCount1) == 0) set_irq14(1);
  }
  if (reg(W_BeaconCount1) == 0) reg(W_BeaconCount1) = reg(W_BeaconInterval);
  if (reg(W_BeaconCount2) != 0) {
    reg(W_BeaconCount2)--;
    if (reg(W_BeaconCount2) == 0) set_irq13();
  }
}

void Wifi::us_timer() {
  us_timestamp_ += kTimerInterval;

  if (is_mp_client_ && !com_status_) {
    if (rx_timestamp_ && us_timestamp_ >= rx_timestamp_) { rx_timestamp_ = 0; start_rx(); }
    if (us_timestamp_ >= next_sync_) check_rx(2);
    // Peek every 64 us, but never while our reply is out or right after
    // start_rx() (hence com_status_ re-checked): the fetch would overwrite the
    // ack being received with the next CMD, breaking Download Play.
    else if (!com_status_ && !rx_timestamp_ && !(reg(W_TXBusy) & 0x0080) && !(us_timestamp_ & 0x38)) check_rx(3);
  }

  if (!(us_timestamp_ & 0x3FF & kTimeCheckMask)) ap_ms_timer();

  if (us_until_power_on_ < 0) {
    us_until_power_on_ += kTimerInterval;
    if (us_until_power_on_ >= 0) {
      us_until_power_on_ = 0;
      reg(W_PowerState) = 0;
      set_status(1);
      update_power_status(0);
    }
  }

  if (reg(W_USCountCnt)) {
    us_counter_ += kTimerInterval;
    const u32 uspart = static_cast<u32>(us_counter_ & 0x3FF);
    if (reg(W_USCompareCnt)) {
      const u32 beaconus = (static_cast<u32>(reg(W_BeaconCount1)) << 10) | (0x3FF - uspart);
      if ((beaconus & kTimeCheckMask) == (reg(W_PreBeacon) & kTimeCheckMask)) set_irq15();
    }
    if (!(uspart & kTimeCheckMask)) ms_timer();
  }

  if (reg(W_CmdCountCnt) & 1) {
    if (cmd_counter_ > 0) cmd_counter_ = cmd_counter_ < static_cast<u32>(kTimerInterval) ? 0 : cmd_counter_ - kTimerInterval;
  }
  if (reg(W_ContentFree) != 0) {
    if (reg(W_ContentFree) < kTimerInterval) reg(W_ContentFree) = 0;
    else reg(W_ContentFree) -= kTimerInterval;
  }

  if (com_status_ == 0) {
    const u16 txbusy = reg(W_TXBusy);
    if (txbusy) {
      if (reg(W_PowerState) & 0x0200) { com_status_ = 0; tx_cur_slot_ = -1; }
      else {
        com_status_ = 2;
        if      (txbusy & 0x0080) tx_cur_slot_ = 5;
        else if (txbusy & 0x0010) tx_cur_slot_ = 4;
        else if (txbusy & 0x0008) tx_cur_slot_ = 3;
        else if (txbusy & 0x0004) tx_cur_slot_ = 2;
        else if (txbusy & 0x0002) tx_cur_slot_ = 1;
        else if (txbusy & 0x0001) tx_cur_slot_ = 0;
      }
    } else {
      if (!is_mp_client_ || us_timestamp_ > next_sync_) {
        if (!(rx_counter_ & 0x1FF & kTimeCheckMask) && !com_status_) check_rx(0);
      }
      rx_counter_ += kTimerInterval;
    }
  }

  if (com_status_ & 2) {
    const bool finished = process_tx(tx_slots_[tx_cur_slot_], tx_cur_slot_);
    if (finished) {
      if (reg(W_PowerState) & 0x0200) { reg(W_TXBusy) = 0; reg(W_TRXPower) = 0; set_status(9); }
      const u16 txbusy = reg(W_TXBusy);
      if      (txbusy & 0x0080) tx_cur_slot_ = 5;
      else if (txbusy & 0x0010) tx_cur_slot_ = 4;
      else if (txbusy & 0x0008) tx_cur_slot_ = 3;
      else if (txbusy & 0x0004) tx_cur_slot_ = 2;
      else if (txbusy & 0x0002) tx_cur_slot_ = 1;
      else if (txbusy & 0x0001) tx_cur_slot_ = 0;
      else { tx_cur_slot_ = -1; com_status_ = 0; rx_counter_ = 0; }
    }
  }

  if (com_status_ & 1) {
    rx_time_ -= kTimerInterval;
    if (!(rx_time_ & rx_halfword_time_mask_)) {
      u16 addr = reg(W_RXTXAddr) << 1;
      if (addr < 0x1FFF) ram16(addr, ld16(&rx_buffer_[rx_buffer_ptr_]));
      increment_rx_addr(addr);
      reg(W_RXTXAddr) = addr >> 1;
      rx_buffer_ptr_ += 2;
      if (rx_time_ <= 0) finish_rx();
      else if (addr == (reg(W_RXBufReadCursor) << 1)) {
        WIFI_LOG("RX buffer full (buf=%04X/%04X rd=%04X wr=%04X)\n", (reg(W_RXBufBegin) >> 1) & 0xFFF, (reg(W_RXBufEnd) >> 1) & 0xFFF,
                 reg(W_RXBufReadCursor), reg(W_RXBufWriteCursor));
        rx_time_ = 0;
        set_status(1);
        if (tx_cur_slot_ < 0) { com_status_ &= ~1u; rx_counter_ = 0; }
        if (!com_status_ && (reg(W_PowerState) & 0x0200)) { reg(W_TRXPower) = 0; set_status(9); }
      }
    }
  }

  schedule_timer(false);
}

// ---- TX ----------------------------------------------------------------------
int Wifi::preamble_len(int rate) const {
  if (rate == 1) return 192;
  if (reg(W_Preamble) & 0x0004) return 96;
  return 192;
}
u32 Wifi::num_clients(u16 mask) const {
  u32 n = 0;
  for (int i = 1; i < 16; ++i) if (mask & (1 << i)) n++;
  return n;
}
void Wifi::increment_tx_count(const TxSlot& slot) {
  u8 cnt = ram_[slot.addr + 4];
  if (cnt < 0xFF) cnt++;
  ram16(slot.addr + 4, cnt);
}
void Wifi::trace_frame(int frame) { if (FILE* tf = wifi_trace_file()) std::fprintf(tf, "# frame %d us %llX\n", frame, (unsigned long long)us_timestamp_); }
void Wifi::report_mp_reply_errors(u16 clientfail) {
  u8* stat = reinterpret_cast<u8*>(io_.data()) + W_CMDStat0;
  for (int i = 1; i < 16; ++i) if (clientfail & (1 << i)) stat[i]++;
}

void Wifi::tx_send_frame(const TxSlot& slot, int num) {
  u32 noseqno = 0;
  if (ram_[slot.addr + 4]) noseqno = 2;
  else if (num == 1) noseqno = (reg(W_TXSlotCmd) & 0x4000) ? 1 : 0;
  if (!noseqno) {
    if (!(reg(W_TXHeaderCnt) & 4)) ram16(slot.addr + 0xC + 22, reg(W_TXSeqNo) << 4);
    reg(W_TXSeqNo) = (reg(W_TXSeqNo) + 1) & 0x0FFF;
  }
  const u16 framectl = ram16(slot.addr + 0xC);
  if (framectl & 0x4000) {   // WEP not emulated; some games need a nonzero WEP FCS
    if (reg(W_WEPCnt) & 0x8000) {
      const u32 wep_fcs = (slot.addr + 0xC + slot.length - 7) & ~1u;
      st32(&ram_[wep_fcs & 0x1FFC], 0x22334466);
    }
  }
  int len = slot.length;
  if (slot.addr + len > 0x1FF4) len = 0x1FF4 - slot.addr;
  std::memcpy(tx_buffer_.data(), &ram_[slot.addr], 12 + len);
  if (noseqno == 2) st16(&tx_buffer_[0xC], ld16(&tx_buffer_[0xC]) | 0x0800);
  if (cur_channel_ == 0) return;
  tx_buffer_[9] = static_cast<u8>(cur_channel_);
  ++tx_frames_;
  WIFI_LOG("TX slot %d: FC:%04X len=%d ch=%d\n", num, ld16(&tx_buffer_[0xC]), len, cur_channel_);

  switch (num) {
  case 0: case 2: case 3:
    if (mp_) mp_->send_packet(tx_buffer_.data(), 12 + len, us_timestamp_);
    if (!is_mp_) ap_send(tx_buffer_.data(), 12 + len);
    break;
  case 1:
    st16(&tx_buffer_[12 + 24 + 2], mp_client_mask_);
    if (FILE* tf = wifi_trace_file()) std::fprintf(tf, "# host CMD out len %d body %04X %04X %04X %04X %04X %04X us %llX\n", len, ld16(&tx_buffer_[36]), ld16(&tx_buffer_[38]), ld16(&tx_buffer_[40]), ld16(&tx_buffer_[42]), ld16(&tx_buffer_[44]), ld16(&tx_buffer_[46]), (unsigned long long)us_timestamp_);
    if (mp_) mp_->send_cmd(tx_buffer_.data(), 12 + len, us_timestamp_);
    break;
  case 5:
    increment_tx_count(slot);
    // Stamped with the CMD's arrival time; transports drop replies >32 us before the CMD's end.
    if (mp_) mp_->send_reply(tx_buffer_.data(), 12 + len, us_timestamp_, reg(W_AIDLow));
    break;
  case 4:
    st64(&tx_buffer_[0xC + 24], us_counter_);
    if (mp_) mp_->send_packet(tx_buffer_.data(), 12 + len, us_timestamp_);
    break;
  }
}

void Wifi::start_tx_locn(int nslot, int loc) {
  TxSlot& slot = tx_slots_[nslot];
  slot.valid = true;
  slot.addr = (reg(W_TXSlotLoc1 + loc * 4) & 0x0FFF) << 1;
  slot.length = ram16(slot.addr + 0xA) & 0x3FFF;
  slot.rate = ram_[slot.addr + 8] == 0x14 ? 2 : 1;
  slot.cur_phase = 0;
  slot.cur_phase_time = preamble_len(slot.rate);
}

void Wifi::start_tx_cmd() {
  TxSlot& slot = tx_slots_[1];
  slot.valid = true;
  slot.addr = (reg(W_TXSlotCmd) & 0x0FFF) << 1;
  slot.length = ram16(slot.addr + 0xA) & 0x3FFF;
  slot.rate = ram_[slot.addr + 8] == 0x14 ? 2 : 1;
  mp_client_mask_ = ram16(slot.addr + 12 + 24 + 2) & mp_client_fail_;
  mp_client_fail_ &= mp_client_mask_;
  u32 duration = preamble_len(slot.rate) + slot.length * (slot.rate == 2 ? 4 : 8);
  duration += 112 + (10 + reg(W_CmdReplyTime)) * num_clients(mp_client_mask_);
  duration += 32 * (slot.rate == 2 ? 4 : 8);
  if (cmd_counter_ > duration + 100) { slot.cur_phase = 0; slot.cur_phase_time = preamble_len(slot.rate); }
  else { slot.cur_phase = 13; slot.cur_phase_time = static_cast<s32>(cmd_counter_) - 100; }
  // a CMD transfer wakes the transceiver
  update_power_status(1);
}

void Wifi::start_tx_beacon() {
  TxSlot& slot = tx_slots_[4];
  slot.valid = true;
  slot.addr = (reg(W_TXSlotBeacon) & 0x0FFF) << 1;
  slot.length = ram16(slot.addr + 0xA) & 0x3FFF;
  slot.rate = ram_[slot.addr + 8] == 0x14 ? 2 : 1;
  slot.cur_phase = 0;
  slot.cur_phase_time = preamble_len(slot.rate);
  reg(W_TXBusy) |= 0x0010;
}

void Wifi::fire_tx() {
  if (!(reg(W_RXCnt) & 0x8000)) return;
  const u16 txbusy = reg(W_TXBusy);
  u16 txstart = 0;
  if (reg(W_TXSlotLoc1) & 0x8000) txstart |= 1;
  if (reg(W_TXSlotCmd)  & 0x8000) txstart |= 2;
  if (reg(W_TXSlotLoc2) & 0x8000) txstart |= 4;
  if (reg(W_TXSlotLoc3) & 0x8000) txstart |= 8;
  txstart &= reg(W_TXReqRead);
  txstart &= ~txbusy;
  reg(W_TXBusy) = txbusy | txstart;
  if (txstart & 8) { start_tx_locn(3, 2); return; }
  if (txstart & 4) { start_tx_locn(2, 1); return; }
  if (txstart & 2) { mp_client_fail_ = 0xFFFE; start_tx_cmd(); return; }
  if (txstart & 1) { start_tx_locn(0, 0); return; }
}

void Wifi::send_mp_default_reply() {
  u8 reply[12 + 28] = {};
  st16(&reply[0xA], 28);
  reply[0x8] = 0x14;   // rate
  if (cur_channel_ == 0) return;
  reply[0x9] = static_cast<u8>(cur_channel_);
  st16(&reply[0xC + 0x00], 0x0158);
  st16(&reply[0xC + 0x02], 0x00F0);
  st16(&reply[0xC + 0x04], reg(W_BSSID0)); st16(&reply[0xC + 0x06], reg(W_BSSID1)); st16(&reply[0xC + 0x08], reg(W_BSSID2));
  st16(&reply[0xC + 0x0A], reg(W_MACAddr0)); st16(&reply[0xC + 0x0C], reg(W_MACAddr1)); st16(&reply[0xC + 0x0E], reg(W_MACAddr2));
  st16(&reply[0xC + 0x10], 0x0903);
  st16(&reply[0xC + 0x12], 0x00BF);
  st16(&reply[0xC + 0x14], 0x1000);
  st16(&reply[0xC + 0x16], reg(W_TXSeqNo) << 4);
  st32(&reply[0xC + 0x18], 0);
  if (mp_) mp_->send_reply(reply, 12 + 28, us_timestamp_, reg(W_AIDLow));
}

void Wifi::send_mp_reply(u16 clienttime, u16 clientmask) {
  TxSlot& slot = tx_slots_[5];
  if (reg(W_TXSlotReply2) & 0x8000) ram16(slot.addr, 0x0001);   // mark last packet success (MSB meaning unclear)
  slot.rate = 2;
  // Reply contents are read at transmit (phase 0), not here: firmware rewrites
  // its one buffer per CMD, so copying now would send each reply one CMD late.
  reg(W_TXSlotReply2) = reg(W_TXSlotReply1);
  reg(W_TXSlotReply1) = 0;
  if (!(reg(W_TXSlotReply2) & 0x8000)) slot.valid = false;
  else {
    slot.valid = true;
    slot.addr = (reg(W_TXSlotReply2) & 0x0FFF) << 1;
    slot.length = ram16(slot.addr + 0xA) & 0x3FFF;
    const u32 duration = preamble_len(slot.rate) + slot.length * (slot.rate == 2 ? 4 : 8);
    if (duration > clienttime) slot.valid = false;   // ignored entirely if longer than the max reply time
  }
  if (FILE* tf = wifi_trace_file()) std::fprintf(tf, "# reply %s len %u clienttime %u\n", slot.valid ? "data" : "empty", slot.valid ? slot.length : 0u, clienttime);
  // Transmit now: the host's reply wait keys on the CMD-arrival timestamp.
  if (slot.valid) { slot.cur_phase = 0; tx_send_frame(slot, 5); }
  else { slot.cur_phase = 10; send_mp_default_reply(); }
  u16 clientnum = 0;
  for (int i = 1; i < reg(W_AIDLow); ++i) if (clientmask & (1 << i)) clientnum++;
  slot.cur_phase_time = 16 + (clienttime + 10) * clientnum + preamble_len(slot.rate);
  reg(W_TXBusy) |= 0x0080;
}

void Wifi::send_mp_ack(u16 cmdcount, u16 clientfail) {
  u8 ack[12 + 32] = {};
  st16(&ack[0xA], 32);
  ack[0x8] = tx_slots_[1].rate == 2 ? 0x14 : 0xA;
  if (cur_channel_ == 0) return;
  ack[0x9] = static_cast<u8>(cur_channel_);
  st16(&ack[0xC + 0x00], 0x0218);
  st16(&ack[0xC + 0x02], 0);
  st16(&ack[0xC + 0x04], 0x0903); st16(&ack[0xC + 0x06], 0x00BF); st16(&ack[0xC + 0x08], 0x0300);
  st16(&ack[0xC + 0x0A], reg(W_BSSID0)); st16(&ack[0xC + 0x0C], reg(W_BSSID1)); st16(&ack[0xC + 0x0E], reg(W_BSSID2));
  st16(&ack[0xC + 0x10], reg(W_MACAddr0)); st16(&ack[0xC + 0x12], reg(W_MACAddr1)); st16(&ack[0xC + 0x14], reg(W_MACAddr2));
  st16(&ack[0xC + 0x16], reg(W_TXSeqNo) << 4);
  st16(&ack[0xC + 0x18], cmdcount);
  st16(&ack[0xC + 0x1A], clientfail);
  st32(&ack[0xC + 0x1C], 0);
  if (!clientfail) {
    u32 nextbeacon;
    if (reg(W_TXBusy) & 0x0010) nextbeacon = 0;
    else nextbeacon = ((reg(W_BeaconCount1) - 1) << 10) + (0x400 - static_cast<u32>(us_counter_ & 0x3FF));
    int runahead = static_cast<int>(std::min(cmd_counter_, nextbeacon));
    if (cmd_counter_ < 1000) runahead -= 210;
    st32(&ack[0], static_cast<u32>(std::max(runahead - 32 * (tx_slots_[1].rate == 2 ? 4 : 8), 0)));
    if (FILE* tf = wifi_trace_file()) std::fprintf(tf, "# host ack fail %04X cmdcount %u runahead %u us %llX\n", clientfail, cmdcount, ld32(&ack[0]), (unsigned long long)us_timestamp_);
  } else {
    if (FILE* tf = wifi_trace_file()) std::fprintf(tf, "# host ack fail %04X cmdcount %u runahead 0 us %llX\n", clientfail, cmdcount, (unsigned long long)us_timestamp_);
    st32(&ack[0], static_cast<u32>(preamble_len(tx_slots_[1].rate)));
  }
  if (mp_) mp_->send_ack(ack, 12 + 32, us_timestamp_);
}

bool Wifi::process_tx(TxSlot& slot, int num) {
  slot.cur_phase_time -= kTimerInterval;
  if (slot.cur_phase_time > 0) {
    if (slot.cur_phase == 1) {
      if (!(static_cast<u32>(slot.cur_phase_time) & slot.halfword_time_mask)) reg(W_RXTXAddr)++;
    } else if (slot.cur_phase == 2) {
      mp_reply_timer_ -= kTimerInterval;
      if (mp_reply_timer_ <= 0 && mp_client_mask_ != 0) {
        int nclient = 1;
        while (!(mp_client_mask_ & (1 << nclient))) nclient++;
        const u32 curclient = 1u << nclient;
        if (!(mp_client_fail_ & curclient)) mp_client_reply_rx(nclient);
        mp_reply_timer_ += 10 + reg(W_CmdReplyTime);
        mp_client_mask_ &= static_cast<u16>(~curclient);
      }
    }
    return false;
  }

  switch (slot.cur_phase) {
  case 0: {   // preamble done
    set_irq(7);
    set_status(num == 5 ? 8 : 3);
    u32 len = slot.length;
    if (slot.rate == 2) { len *= 4; slot.halfword_time_mask = 0x7 & kTimeCheckMask; }
    else { len *= 8; slot.halfword_time_mask = 0xF & kTimeCheckMask; }
    slot.cur_phase = 1;
    slot.cur_phase_time = static_cast<s32>(len);
    reg(W_RXTXAddr) = slot.addr >> 1;
    if (num != 5) tx_send_frame(slot, num);   // slot 5's went out above
    if (num == 0 || num == 2 || num == 3) {   // LOC1..3 also go to the AP; CMD/REPLY/BEACON stay local-MP only
      const u16 framectl = ram16(slot.addr + 0xC);
      if ((framectl & 0x00FF) == 0x0010) {
        const u16 aid = ram16(slot.addr + 0xC + 24 + 4);
        if (aid) { host_syncs_++; WIFI_LOG("[HOST] syncing client %04X, sync=%016llX\n", aid, static_cast<unsigned long long>(us_timestamp_)); }
      } else if ((framectl & 0x00FF) == 0x00C0) {
        if (is_mp_client_) { WIFI_LOG("[CLIENT] deauth\n"); is_mp_ = false; is_mp_client_ = false; }
      }
    }
    break;
  }
  case 10:   // preamble done (default empty MP reply)
    set_irq(7);
    set_status(8);
    slot.cur_phase = 11;
    slot.cur_phase_time = 28 * 4;
    slot.halfword_time_mask = 0xFFFFFFFF;
    break;

  case 1: {   // transmit done
    // for the MP CMD and reply slots, this is set later
    if (num != 1 && num != 5) ram16(slot.addr, 0x0001);
    ram_[slot.addr + 5] = 0;
    if (num == 1) {
      if (reg(W_TXStatCnt) & 0x4000) { reg(W_TXStat) = 0x0800; set_irq(1); }
      set_status(5);
      mp_reply_timer_ = 16 + preamble_len(slot.rate);
      u16 res = 0;
      if (mp_client_mask_ && mp_) res = mp_->recv_replies(mp_client_replies_.data(), us_timestamp_, mp_client_mask_);
      mp_client_fail_ &= static_cast<u16>(~res);
      WIFI_LOG("CMD done: clients %04X replied %04X fail %04X cmdcount %u\n", mp_client_mask_, res, mp_client_fail_, cmd_counter_);
      slot.cur_phase = 2;
      slot.cur_phase_time = 112 + (10 + reg(W_CmdReplyTime)) * static_cast<s32>(num_clients(mp_client_mask_));
      break;
    }
    if (num == 5) {   // reply frame already went out in send_mp_reply; this just completes its transfer
      if (reg(W_TXStatCnt) & 0x1000) { reg(W_TXStat) = 0x0401; set_irq(1); }
      set_status(1);
      reg(W_TXBusy) &= static_cast<u16>(~0x80);
      fire_tx();
      return true;
    }
    reg(W_TXBusy) &= static_cast<u16>(~(1 << num));
    switch (num) {
    case 0: case 2: case 3:
      reg(W_TXStat) = static_cast<u16>(0x0001 | ((num ? num - 1 : 0) << 12));
      set_irq(1);
      reg(W_TXSlotLoc1 + (num ? num - 1 : 0) * 4) &= 0x7FFF;
      break;
    case 4:   // beacon
      if (reg(W_TXStatCnt) & 0x8000) { reg(W_TXStat) = 0x0301; set_irq(1); }
      break;
    }
    set_status(1);
    fire_tx();
    return true;
  }
  case 11:   // MP default reply transfer finished
    reg(W_TXSeqNo) = (reg(W_TXSeqNo) + 1) & 0x0FFF;
    reg(W_TXBusy) &= static_cast<u16>(~0x80);
    set_status(1);
    fire_tx();
    return true;

  case 2: {   // MP host transfer done
    set_irq(7);
    set_status(8);
    reg(W_RXTXAddr) = 0xFC0;
    slot.cur_phase_time = slot.rate == 2 ? 32 * 4 : 32 * 8;
    report_mp_reply_errors(mp_client_fail_);
    send_mp_ack(static_cast<u16>((cmd_counter_ + 9) / 10), mp_client_fail_);
    slot.cur_phase = 3;
    break;
  }
  case 3: {   // MP host ack transfer (reply wait done)
    WIFI_LOG("CMD result: fail %04X\n", mp_client_fail_);
    ram16(slot.addr, mp_client_fail_ ? 0x0005 : 0x0001);
    // this is set to indicate which clients failed to reply
    ram16(slot.addr + 2, mp_client_fail_);
    if (!mp_client_fail_) increment_tx_count(slot);
    reg(W_TXSeqNo) = (reg(W_TXSeqNo) + 1) & 0x0FFF;
    if (reg(W_TXStatCnt) & 0x2000) { reg(W_TXStat) = 0x0B01; set_irq(1); }
    // Not retried for the clients that failed: causes instability.
    reg(W_TXBusy) &= static_cast<u16>(~2);
    reg(W_TXSlotCmd) &= 0x7FFF;
    set_status(1);
    set_irq(12);
    fire_tx();
    return true;
  }
  case 13:   // MP transfer failed (timeout)
    reg(W_TXBusy) &= static_cast<u16>(~2);
    reg(W_TXSlotCmd) &= 0x7FFF;
    ram16(slot.addr, 0x0005);
    reg(W_TXSeqNo) = (reg(W_TXSeqNo) + 1) & 0x0FFF;
    set_status(1);
    set_irq(12);
    fire_tx();
    return true;
  }
  return false;
}

// ---- RX ----------------------------------------------------------------------
void Wifi::increment_rx_addr(u16& addr, u16 inc) {
  for (u32 i = 0; i < inc; i += 2) {
    addr += 2;
    addr &= 0x1FFE;
    if (addr == (reg(W_RXBufEnd) & 0x1FFE)) addr = reg(W_RXBufBegin) & 0x1FFE;
  }
}

void Wifi::start_rx() {
  const u16 framelen = ld16(&rx_buffer_[8]);
  rx_time_ = framelen;
  if (ld16(&rx_buffer_[6]) == 0x14) { rx_time_ *= 4; rx_halfword_time_mask_ = 0x7 & kTimeCheckMask; }
  else { rx_time_ *= 8; rx_halfword_time_mask_ = 0xF & kTimeCheckMask; }
  u16 addr = reg(W_RXBufWriteCursor) << 1;
  increment_rx_addr(addr, 12);
  reg(W_RXTXAddr) = addr >> 1;
  rx_buffer_ptr_ = 12;
  set_irq(6);
  set_status(6);
  com_status_ |= 1;
}

void Wifi::finish_rx() {
  com_status_ &= ~1u;
  rx_counter_ = 0;
  if (!com_status_) {
    if (reg(W_PowerState) & 0x0200) { reg(W_TRXPower) = 0; set_status(9); }
    else set_status(1);
  }
  const u16 framectl = ld16(&rx_buffer_[12]);
  const u16 seqno = ld16(&rx_buffer_[12 + 22]);
  const u8* dstmac = &rx_buffer_[12 + 4];   // hardware always checks the first address field
  if (!(dstmac[0] & 1)) { if (!mac_equal(dstmac, mac())) return; }
  if (framectl & 0x4000) { if (!(reg(W_WEPCnt) & 0x8000)) return; }   // reject WEP frame if WEP is off

  // RXFILTER bits 0, 9, 10, 12 and port 0D8 not fully understood. Duplicate-seq MP CMDs are ignored.
  u16 rxflags = 0x0010;
  bool cmd_dupe = false;
  switch ((framectl >> 2) & 3) {
  case 0: {   // management
    if (mac_equal(&rx_buffer_[12 + 16], bssid())) rxflags |= 0x8000;
    const u16 subtype = (framectl >> 4) & 0xF;
    if (subtype == 0x8) {   // beacon
      if (!(rxflags & 0x8000)) { if (!(reg(W_RXFilter) & 1)) return; }
      rxflags |= 0x0001;
    } else if (subtype <= 0x5 || (subtype >= 0xA && subtype <= 0xC)) {
      if (!(rxflags & 0x8000)) { if (!(reg(W_RXFilter) & (3 << 9))) return; }   // CHECKME
    }
    break;
  }
  case 1: {   // control
    if ((framectl & 0xF0) == 0xA0) {   // PS-poll
      if (mac_equal(&rx_buffer_[12 + 4], bssid())) rxflags |= 0x8000;
      if (!(rxflags & 0x8000)) { if (!(reg(W_RXFilter) & (1 << 11))) return; }
      rxflags |= 0x0005;
    } else return;
    break;
  }
  case 2: {   // data
    const u16 fromto = (framectl >> 8) & 3;
    if (reg(W_RXFilter2) & (1 << fromto)) return;
    static const int bssidoffset[4] = {16, 4, 10, 0};
    if (bssidoffset[fromto]) { if (mac_equal(&rx_buffer_[12 + bssidoffset[fromto]], bssid())) rxflags |= 0x8000; }
    const u16 rxfilter = reg(W_RXFilter);
    if (!(rxflags & 0x8000)) { if (!(rxfilter & (1 << 11))) return; }
    if (framectl & 0x0800) { if (!(rxfilter & 1)) return; }   // retransmit
    // MP frames: checked by MAC address; reply check has priority over others.
    if (mac_equal(&rx_buffer_[12 + 16], kMpReplyMac)) rxflags |= ((framectl & 0xF0) == 0x50) ? 0x000F : 0x000E;
    else if (mac_equal(&rx_buffer_[12 + 4], kMpCmdMac)) { if (seqno == mp_last_seqno_) cmd_dupe = true; mp_last_seqno_ = seqno; rxflags |= 0x000C; }
    else if (mac_equal(&rx_buffer_[12 + 4], kMpAckMac)) {
      rxflags |= 0x000D;
      if (FILE* tf = wifi_trace_file()) std::fprintf(tf, "# ack rx fail %04X cmdcount %u seq %04X us %llX via %d\n", ld16(&rx_buffer_[12 + 0x1A]), ld16(&rx_buffer_[12 + 0x18]), seqno, (unsigned long long)us_timestamp_, last_rx_type_);
    }
    else rxflags |= 0x0008;
    switch ((framectl >> 4) & 0xF) {
    case 0x0: break;
    case 0x1:
      if ((rxflags & 0xF) == 0xD) { if (!(rxfilter & (1 << 7))) return; }
      else if ((rxflags & 0xF) != 0xE) { if (!(rxfilter & (1 << 1))) return; }
      break;
    case 0x2: if ((rxflags & 0xF) != 0xC) { if (!(rxfilter & (1 << 2))) return; } break;
    case 0x3: if (!(rxfilter & (1 << 3))) return; break;
    case 0x4: break;
    case 0x5:
      if ((rxflags & 0xF) == 0xF) { if (!(rxfilter & (1 << 8))) return; }
      else { if (!(rxfilter & (1 << 4))) return; }
      break;
    case 0x6: if (!(rxfilter & (1 << 5))) return; break;
    case 0x7: if (!(rxfilter & (1 << 6))) return; break;
    default: return;
    }
    break;
  }
  }

  if (!cmd_dupe) {   // build the RX header
    u16 headeraddr = reg(W_RXBufWriteCursor) << 1;
    ram16(headeraddr, rxflags);
    increment_rx_addr(headeraddr);
    ram16(headeraddr, 0x0040);
    increment_rx_addr(headeraddr, 4);
    ram16(headeraddr, ld16(&rx_buffer_[6]));   // TX rate
    increment_rx_addr(headeraddr);
    ram16(headeraddr, ld16(&rx_buffer_[8]));   // frame length
    increment_rx_addr(headeraddr);
    ram16(headeraddr, 0x4080);   // RSSI
    u16 addr = reg(W_RXTXAddr) << 1;   // signal successful reception
    if (addr & 2) increment_rx_addr(addr);
    reg(W_RXBufWriteCursor) = (addr & ~3u) >> 1;
    set_irq(0);
  }

  if ((rxflags & 0x800F) == 0x800C) {   // reply to CMD frames
    const u16 clientmask = ld16(&rx_buffer_[0xC + 26]);
    if (FILE* tf = wifi_trace_file()) std::fprintf(tf, "# CMD rx seq %04X len %u reply1 %04X reply2 %04X us %llX via %d\n", seqno, ld16(&rx_buffer_[8]), reg(W_TXSlotReply1), reg(W_TXSlotReply2), (unsigned long long)us_timestamp_, last_rx_type_);
    if (reg(W_AIDLow) && (clientmask & (1 << reg(W_AIDLow)))) send_mp_reply(ld16(&rx_buffer_[0xC + 24]), clientmask);
    else if (mp_) mp_->send_reply(nullptr, 0, us_timestamp_, 0);   // blank, so the host has something to receive instead of a timeout
  } else if ((rxflags & 0x800F) == 0x8001) {   // beacon with the right BSSID copies its timestamp to USCOUNTER
    u32 len = ld16(&rx_buffer_[8]);
    len *= (ld16(&rx_buffer_[6]) == 0x14) ? 4 : 8;
    len -= 76;   // CHECKME: is this offset fixed?
    us_counter_ = ld64(&rx_buffer_[12 + 24]) + len;
  }
}

void Wifi::mp_client_reply_rx(int client) {
  if (reg(W_PowerState) & 0x0200) return;
  if (!(reg(W_RXCnt) & 0x8000)) return;
  if (reg(W_RXBufBegin) == reg(W_RXBufEnd)) return;
  const u8* reply = &mp_client_replies_[(client - 1) * 1024];
  int framelen = ld16(&reply[10]);
  WIFI_LOG("MP reply from client %d: FC:%04X len=%d\n", client, ld16(&reply[12]), framelen);
  if (FILE* tf = wifi_trace_file()) std::fprintf(tf, "# host reply-in client %d len %d body %04X %04X %04X %04X %04X us %llX\n", client, framelen, ld16(&reply[36]), ld16(&reply[38]), ld16(&reply[40]), ld16(&reply[42]), ld16(&reply[44]), (unsigned long long)us_timestamp_);
  const u8 txrate = reply[8];
  const u16 framectl = ld16(&reply[12]);
  if (framectl & 0x4000) {
    framelen -= (reg(W_RXLenCrop) >> 7) & 0x1FE;
    if (framelen > 24) std::memmove(&rx_buffer_[12 + 24], &rx_buffer_[12 + 28], framelen);
  } else framelen -= (reg(W_RXLenCrop) << 1) & 0x1FE;
  if (framelen < 0) framelen = 0;
  std::memcpy(rx_buffer_.data(), reply, 12 + framelen);
  st16(&rx_buffer_[6], txrate);
  st16(&rx_buffer_[8], static_cast<u16>(framelen));
  rx_timestamp_ = 0;
  start_rx();
}

bool Wifi::check_rx(int type) {   // 0 = regular, 1 = MP replies, 2 = MP host frames (wait), 3 = MP host frames (no wait)
  if (reg(W_PowerState) & 0x0200) return false;
  if (!(reg(W_RXCnt) & 0x8000)) return false;
  if (reg(W_RXBufBegin) == reg(W_RXBufEnd)) return false;
  int rxlen, framelen;
  u16 framectl;
  u8 txrate;
  u64 timestamp;
  for (;;) {
    timestamp = 0;
    if (type == 0) {
      rxlen = mp_ ? mp_->recv_packet(rx_buffer_.data(), &timestamp) : 0;
      if (rxlen <= 0 && !is_mp_) rxlen = ap_recv(rx_buffer_.data());
    } else {
      rxlen = mp_ ? (type == 3 ? mp_->peek_host_packet(rx_buffer_.data(), &timestamp) : mp_->recv_host_packet(rx_buffer_.data(), &timestamp)) : 0;
      if (rxlen < 0) { is_mp_ = false; is_mp_client_ = false; }   // host is gone
    }
    if (rxlen <= 0) return false;
    last_rx_type_ = type;
    if (rxlen < 12 + 24) continue;
    framelen = ld16(&rx_buffer_[10]);
    if (framelen != rxlen - 12) { WIFI_LOG("bad frame length %d/%d\n", framelen, rxlen - 12); continue; }
    const u8 chan = rx_buffer_[9];
    if (chan != cur_channel_ || cur_channel_ == 0) { WIFI_LOG("frame on channel %d, expected %d\n", chan, cur_channel_); continue; }
    if (type == 0 && !is_mp_) {   // ignore MP frames if not engaged in an MP comm
      if (mac_equal(&rx_buffer_[12 + 16], kMpReplyMac) || mac_equal(&rx_buffer_[12 + 4], kMpCmdMac) || mac_equal(&rx_buffer_[12 + 4], kMpReplyMac)) continue;
    }
    framectl = ld16(&rx_buffer_[12]);
    txrate = rx_buffer_[8];
    if (framectl & 0x4000) {
      framelen -= (reg(W_RXLenCrop) >> 7) & 0x1FE;
      if (framelen > 24) std::memmove(&rx_buffer_[12 + 24], &rx_buffer_[12 + 28], framelen);
    } else framelen -= (reg(W_RXLenCrop) << 1) & 0x1FE;
    if (framelen < 0) framelen = 0;
    break;
  }
  WIFI_LOG("received packet FC:%04X SN:%04X len=%d\n", framectl, ld16(&rx_buffer_[12 + 22]), framelen);
  st16(&rx_buffer_[6], txrate);
  st16(&rx_buffer_[8], static_cast<u16>(framelen));

  const u16 frametype = framectl & 0x00FF;
  const bool macgood = (rx_buffer_[12 + 4] & 1) || mac_equal(&rx_buffer_[12 + 4], mac());
  // auth/assoc frames extend the post-beacon window for unsynced clients.
  if ((frametype == 0x00B0 || frametype == 0x0010 || frametype == 0x0000) && timestamp && macgood) {
    if (reg(W_BeaconCount2)) reg(W_BeaconCount2) += 10;
  }
  if (frametype == 0x0010 && timestamp && macgood) {
    // an association response carries the host's sync value
    const u16 aid = ld16(&rx_buffer_[12 + 24 + 4]);
    if (aid) {
      WIFI_LOG("[CLIENT %01X] host sync=%016llX\n", aid & 0xF, static_cast<unsigned long long>(timestamp));
      is_mp_ = true; is_mp_client_ = true;
      us_timestamp_ = timestamp;
      next_sync_ = rx_timestamp_ + framelen * (txrate == 0x14 ? 4 : 8);
    }
    rx_timestamp_ = 0;
    start_rx();
  } else if (frametype == 0x00C0 && timestamp && macgood && is_mp_client_) {
    if (FILE* tf = wifi_trace_file()) std::fprintf(tf, "# deauth rx us %llX\n", (unsigned long long)us_timestamp_);
    is_mp_ = false; is_mp_client_ = false; next_sync_ = 0;
    rx_timestamp_ = 0;
    start_rx();
  } else if (macgood && is_mp_client_) {
    // Client: hold until the frame's timestamp; next_sync_ bounds how far we may run.
    if (FILE* tf = wifi_trace_file()) std::fprintf(tf, "# hf via %d fc %04X len %d ts %llX us %llX\n", type, framectl, framelen, (unsigned long long)timestamp, (unsigned long long)us_timestamp_);
    rx_timestamp_ = std::max(timestamp, us_timestamp_);
    next_sync_ = rx_timestamp_ + framelen * (txrate == 0x14 ? 4 : 8);
    if (mac_equal(&rx_buffer_[12 + 4], kMpCmdMac)) {
      const u16 clienttime = ld16(&rx_buffer_[12 + 24]);
      const u16 clientmask = ld16(&rx_buffer_[12 + 26]);
      next_sync_ += 112 + (clienttime + 10) * num_clients(clientmask);   // include the MP reply window
    } else if (mac_equal(&rx_buffer_[12 + 4], kMpAckMac)) {
      next_sync_ += ld32(&rx_buffer_[0]);
    }
  } else {
    rx_timestamp_ = 0;
    start_rx();
  }
  return true;
}

// ---- RF ----------------------------------------------------------------------
void Wifi::change_channel() {
  const u32 v1 = rf_[rf_channel_index_[0] & 0x3F], v2 = rf_[rf_channel_index_[1] & 0x3F];
  cur_channel_ = 0;
  for (int i = 0; i < 14; ++i) {
    if (v1 == rf_channel_data_[i * 2] && v2 == rf_channel_data_[i * 2 + 1]) { cur_channel_ = i + 1; break; }
  }
  if (cur_channel_ > 0) WIFI_LOG("channel %d\n", cur_channel_);
  else WIFI_LOG("invalid channel values %05X:%05X\n", v1, v2);
}
void Wifi::rf_transfer_type2() {
  const u32 id = (reg(W_RFData2) >> 2) & 0x1F;
  if (reg(W_RFData2) & 0x0080) {
    const u32 data = rf_[id];
    reg(W_RFData1) = static_cast<u16>(data);
    reg(W_RFData2) = static_cast<u16>((reg(W_RFData2) & 0xFFFC) | ((data >> 16) & 3));
  } else {
    rf_[id] = reg(W_RFData1) | ((reg(W_RFData2) & 3) << 16);
    if (id == rf_channel_index_[0] || id == rf_channel_index_[1]) change_channel();
  }
}
void Wifi::rf_transfer_type3() {
  const u32 id = (reg(W_RFData1) >> 8) & 0x3F, cmd = reg(W_RFData2) & 0xF;
  if (cmd == 6) reg(W_RFData1) = static_cast<u16>((reg(W_RFData1) & 0xFF00) | (rf_[id] & 0xFF));
  else if (cmd == 5) {
    rf_[id] = reg(W_RFData1) & 0xFF;
    if (id == rf_channel_index_[0] || id == rf_channel_index_[1]) change_channel();
  }
}

// ---- bus ---------------------------------------------------------------------
u16 Wifi::read16(u32 addr) {
  if (FILE* tf = wifi_trace_file()) { const u16 v = read16_inner(addr); if (wifi_trace_time()) std::fprintf(tf, "R %03X %04X @%llX\n", addr & 0xFFF, v, (unsigned long long)us_timestamp_); else std::fprintf(tf, "R %03X %04X\n", addr & 0xFFF, v); return v; }
  return read16_inner(addr);
}
u16 Wifi::read16_inner(u32 addr) {
  const u32 a = addr & 0x7FFE;
  if (a >= 0x4000 && a < 0x6000) return ram16(a);
  if (a >= 0x2000 && a < 0x4000) return 0xFFFF;
  const bool activeread = a < 0x1000;
  const u32 r = a & 0xFFF;
  switch (r) {
  case W_Random:   // random generator. not accurate
    random_ = static_cast<u16>((random_ & 1) ^ (((random_ & 0x3FF) << 1) | (random_ >> 10)));
    return random_;
  case W_Preamble: return reg(r) & 3;
  case W_USCount0: case W_USCount1: case W_USCount2: case W_USCount3: return static_cast<u16>(us_counter_ >> ((r - W_USCount0) * 8));
  case W_USCompare0: case W_USCompare1: case W_USCompare2: case W_USCompare3: return static_cast<u16>(us_compare_ >> ((r - W_USCompare0) * 8));
  case W_CmdCount: return static_cast<u16>((cmd_counter_ + 9) / 10);
  case W_BBRead:
    if ((reg(W_BBCnt) & 0xF000) != 0x6000) return 0;
    return bb_[reg(W_BBCnt) & 0xFF];
  case W_BBBusy: case W_RFBusy: return 0;
  case W_RXBufDataRead:
    if (activeread) {
      u32 rdaddr = reg(W_RXBufReadAddr);
      const u16 ret = ram16(rdaddr);
      rdaddr += 2;
      if (rdaddr == (reg(W_RXBufEnd) & 0x1FFEu)) rdaddr = reg(W_RXBufBegin) & 0x1FFE;
      if (rdaddr == reg(W_RXBufGapAddr)) {
        rdaddr += reg(W_RXBufGapSize) << 1;
        if (rdaddr >= (reg(W_RXBufEnd) & 0x1FFEu)) rdaddr = rdaddr + (reg(W_RXBufBegin) & 0x1FFE) - (reg(W_RXBufEnd) & 0x1FFE);
        if (reg(W_ID) == 0xC340) reg(W_RXBufGapSize) = 0;
      }
      reg(W_RXBufReadAddr) = rdaddr & 0x1FFE;
      reg(W_RXBufDataRead) = ret;
      if (reg(W_RXBufCount) > 0) {
        reg(W_RXBufCount)--;
        if (reg(W_RXBufCount) == 0) set_irq(9);
      }
    }
    break;
  case W_TXBusy: return reg(r) & 0x001F;   // no bit for MP replies. odd
  case W_CMDStat0: case W_CMDStat0 + 2: case W_CMDStat0 + 4: case W_CMDStat0 + 6:
  case W_CMDStat0 + 8: case W_CMDStat0 + 10: case W_CMDStat0 + 12: case W_CMDStat0 + 14: {
    const u16 v = reg(r); reg(r) = 0; return v;
  }
  }
  return reg(r);
}

void Wifi::write16(u32 addr, u16 val) {
  if (FILE* tf = wifi_trace_file()) { if (wifi_trace_time()) std::fprintf(tf, "W %03X %04X @%llX\n", addr & 0xFFF, val, (unsigned long long)us_timestamp_); else std::fprintf(tf, "W %03X %04X\n", addr & 0xFFF, val); }
  const u32 a = addr & 0x7FFE;
  if (a >= 0x4000 && a < 0x6000) { ram16(a, val); return; }
  if (a >= 0x2000 && a < 0x4000) return;
  const u32 r = a & 0xFFF;
  switch (r) {
  case W_ModeReset: {
    const u16 old = reg(W_ModeReset);
    reg(W_ModeReset) = val & 1;
    if (!(old & 1) && (val & 1)) { reg(0x27C) = 0x0005; update_power_status(0); }
    else if ((old & 1) && !(val & 1)) { reg(0x27C) = 0x000A; update_power_status(0); }
    if (val & 0x2000) {
      reg(W_RXBufWriteAddr) = 0; reg(W_CmdTotalTime) = 0; reg(W_CmdReplyTime) = 0;
      reg(0x1A4) = 0; reg(0x278) = 0x000F;
    }
    if (val & 0x4000) {
      reg(W_ModeWEP) = 0; reg(W_TXStatCnt) = 0; reg(0x00A) = 0;
      reg(W_MACAddr0) = reg(W_MACAddr1) = reg(W_MACAddr2) = 0;
      reg(W_BSSID0) = reg(W_BSSID1) = reg(W_BSSID2) = 0;
      reg(W_AIDLow) = 0; reg(W_AIDFull) = 0;
      reg(W_TXRetryLimit) = 0x0707; reg(0x02E) = 0;
      reg(W_RXBufBegin) = 0x4000; reg(W_RXBufEnd) = 0x4800;
      reg(W_TXBeaconTIM) = 0; reg(W_Preamble) = 0x0001;
      reg(W_RXFilter) = 0x0401; reg(0x0D4) = 0x0001; reg(W_RXFilter2) = 0x0008;
      reg(0x0EC) = 0x3F03; reg(W_TXHeaderCnt) = 0; reg(0x198) = 0; reg(0x1A2) = 0x0001;
      reg(0x224) = 0x0003; reg(0x230) = 0x0047;
    }
    return;
  }
  case W_ModeWEP:
    val &= 0x007F;
    reg(W_ModeWEP) = val;
    if (reg(W_PowerTX) & 2) {
      if ((val & 7) == 1) reg(W_PowerDownCtrl) |= 2;
      else if ((val & 7) == 2) reg(W_PowerDownCtrl) = 3;
      if ((val & 7) != 3) reg(W_PowerState) &= 0x0300;
      update_power_status(0);
    }
    return;
  case W_IE: { const u16 old = reg(W_IF) & reg(W_IE); reg(W_IE) = val; check_irq(old); return; }
  case W_IF: reg(W_IF) &= static_cast<u16>(~val); return;
  case W_IFSet: { const u16 old = reg(W_IF) & reg(W_IE); reg(W_IF) |= val & 0xFBFF; check_irq(old); return; }
  case W_AIDLow: reg(W_AIDLow) = val & 0x000F; return;
  case W_AIDFull: reg(W_AIDFull) = val & 0x07FF; return;
  case W_PowerUS: reg(W_PowerUS) = val & 3; update_power_on(); return;
  case W_PowerTX:
    reg(W_PowerTX) = val & 3;
    if (val & 2) {
      if ((reg(W_ModeWEP) & 7) == 1) reg(W_PowerDownCtrl) |= 2;
      else if ((reg(W_ModeWEP) & 7) == 2) reg(W_PowerDownCtrl) = 3;
      update_power_status(0);
    }
    return;
  case W_PowerState: {
    if ((reg(W_ModeWEP) & 7) != 3) return;
    u16 v = static_cast<u16>((reg(W_PowerState) & 0x0300) | (val & 0x0003));
    if ((v & 0x0300) == 0x0200) v &= static_cast<u16>(~1); else v &= static_cast<u16>(~2);
    if (!(v & 0x0200)) v &= static_cast<u16>(~0x0100);
    reg(W_PowerState) = v;
    update_power_status(0);
    return;
  }
  case W_PowerForce: reg(W_PowerForce) = val & 0x8001; update_power_status(0); return;
  case W_PowerDownCtrl:
    reg(W_PowerDownCtrl) = val & 3;
    if (reg(W_PowerTX) & 2) {
      if ((reg(W_ModeWEP) & 7) == 1) reg(W_PowerDownCtrl) |= 2;
      else if ((reg(W_ModeWEP) & 7) == 2) reg(W_PowerDownCtrl) = 3;
    }
    update_power_status(0);
    return;
  case W_USCountCnt: val &= 1; break;
  case W_USCompareCnt: if (val & 2) set_irq14(2); val &= 1; break;
  case W_USCount0: case W_USCount1: case W_USCount2: case W_USCount3: {
    const u32 sh = (r - W_USCount0) * 8; us_counter_ = (us_counter_ & ~(u64{0xFFFF} << sh)) | (static_cast<u64>(val) << sh); return;
  }
  case W_USCompare0:
    us_compare_ = (us_compare_ & ~u64{0xFFFF}) | (val & 0xFC00);
    if (val & 1) block_beacon_irq14_ = true;
    return;
  case W_USCompare1: case W_USCompare2: case W_USCompare3: {
    const u32 sh = (r - W_USCompare0) * 8; us_compare_ = (us_compare_ & ~(u64{0xFFFF} << sh)) | (static_cast<u64>(val) << sh); return;
  }
  case W_CmdCount: cmd_counter_ = val * 10u; return;
  case W_BBCnt:
    reg(W_BBCnt) = val;
    if ((val & 0xF000) == 0x5000) { const u32 id = val & 0xFF; if (!bb_ro_[id]) bb_[id] = static_cast<u8>(reg(W_BBWrite)); }
    return;
  case W_RFData2:
    reg(W_RFData2) = val;
    if (rf_version_ == 3) rf_transfer_type3(); else rf_transfer_type2();
    return;
  case W_RFCnt: val &= 0x413F; break;
  case W_RXCnt:
    if (val & 0x0001) reg(W_RXBufWriteCursor) = reg(W_RXBufWriteAddr);
    if (val & 0x0080) { reg(W_TXSlotReply2) = reg(W_TXSlotReply1); reg(W_TXSlotReply1) = 0; }
    if (val & 0x8000) fire_tx();
    val &= 0xFF0E;
    break;
  case W_RXBufDataRead:
    if (reg(W_RXBufCount) > 0) { reg(W_RXBufCount)--; if (reg(W_RXBufCount) == 0) set_irq(9); }
    return;
  case W_RXBufReadAddr: case W_RXBufGapAddr: val &= 0x1FFE; break;
  case W_RXBufGapSize: case W_RXBufCount: case W_RXBufWriteAddr: case W_RXBufReadCursor: val &= 0x0FFF; break;
  case W_TXSlotReply1: if (FILE* tf = wifi_trace_file()) std::fprintf(tf, "# arm reply1 %04X us %llX\n", val, (unsigned long long)us_timestamp_); break;
  case W_TXReqReset: reg(W_TXReqRead) &= static_cast<u16>(~val); return;
  case W_TXReqSet: reg(W_TXReqRead) |= val; fire_tx(); return;
  case W_TXSlotReset:
    if (val & 0x0001) reg(W_TXSlotLoc1) &= 0x7FFF;
    if (val & 0x0002) reg(W_TXSlotCmd) &= 0x7FFF;
    if (val & 0x0004) reg(W_TXSlotLoc2) &= 0x7FFF;
    if (val & 0x0008) reg(W_TXSlotLoc3) &= 0x7FFF;
    if (val & 0x0040) reg(W_TXSlotReply2) &= 0x7FFF;
    if (val & 0x0080) reg(W_TXSlotReply1) &= 0x7FFF;
    val = 0;   // checkme (write-only port)
    break;
  case W_TXBufDataWrite: {
    u32 wraddr = reg(W_TXBufWriteAddr);
    ram16(wraddr, val);
    wraddr += 2;
    if (wraddr == reg(W_TXBufGapAddr)) wraddr += reg(W_TXBufGapSize) << 1;
    reg(W_TXBufWriteAddr) = wraddr & 0x1FFE;
    if (reg(W_TXBufCount) > 0) { reg(W_TXBufCount)--; if (reg(W_TXBufCount) == 0) set_irq(8); }
    return;
  }
  case W_TXBufWriteAddr: case W_TXBufGapAddr: val &= 0x1FFE; break;
  case W_TXBufGapSize: case W_TXBufCount: val &= 0x0FFF; break;
  case W_TXSlotBeacon: is_mp_ = (val & 0x8000) != 0; break;
  case W_TXSlotCmd:
    if (cmd_counter_ == 0) val = static_cast<u16>((val & 0x7FFF) | (reg(W_TXSlotCmd) & 0x8000));
    [[fallthrough]];
  case W_TXSlotLoc1: case W_TXSlotLoc2: case W_TXSlotLoc3:
    // checkme: can a queued transfer be cancelled by clearing bit 15 here?
    reg(r) = val;
    fire_tx();
    return;
  // read-only ports
  case W_ID: case W_TRXPower: case W_Random: case W_RXBufWriteCursor: case W_TXSlotReply2:
  case W_TXReqRead: case W_TXBusy: case W_TXStat: case W_BBRead: case W_BBBusy: case W_RFBusy:
  case W_RFPins: case W_RXStatIncIF: case W_RXStatHalfIF: case W_RXCount: case W_TXSeqNo: case W_RFStatus: case W_RXTXAddr:
    return;
  default: break;
  }
  reg(r) = val;
}

// ---- the emulated access point -----------------------------------------------
namespace {
struct PacketWriter {
  u8* base; u8* p;
  explicit PacketWriter(u8* b) : base(b), p(b) {}
  void u8_(u8 v) { *p++ = v; }
  void u16_(u16 v) { st16(p, v); p += 2; }
  void u32_(u32 v) { st32(p, v); p += 4; }
  void u64_(u64 v) { st64(p, v); p += 8; }
  void mac(const u8* m) { std::memcpy(p, m, 6); p += 6; }
  void bytes(const void* d, size_t n) { std::memcpy(p, d, n); p += n; }
  int len() const { return static_cast<int>(p - base); }
  void align4() { while (len() & 3) *p++ = 0xFF; }
};
// 12-byte TX header in front of a received frame.
void write_txh(u8* p, int len, u8 rate) { std::memset(p, 0, 8); p[8] = rate; p[9] = kApChannel; st16(p + 10, static_cast<u16>(len)); }
} // namespace

void Wifi::ap_reset() {
  ap_.us_counter = 0x428888000ULL;   // arbitrary
  ap_.seq_no = 0x0120;
  ap_.beacon_due = false;
  ap_.packet.fill(0); ap_.packet_len = 0; ap_.rx_num = 0;
  ap_.lan.fill(0);
  ap_.client_status = 0;
}

void Wifi::ap_ms_timer() {
  ap_.us_counter += 0x400;
  if (static_cast<u32>(ap_.us_counter) % (100 * 0x400) == 0) ap_.beacon_due = true;   // beacon every 100 TU (102.4 ms)
}

int Wifi::ap_handle_management(const u8* data, int len) {
  // BSSID only checked where the client must already know us.
  if (ap_.rx_num) { WIFI_LOG("AP: can't reply\n"); return 0; }
  const u16 framectl = ld16(&data[0]);
  PacketWriter w(ap_.packet.data());
  auto seqno = [&] { w.u16_(ap_.seq_no); ap_.seq_no += 0x10; };
  switch ((framectl >> 4) & 0xF) {
  case 0x0: {   // assoc request
    if (!mac_equal(&data[16], kApMac)) return 0;
    if (ap_.client_status != 1) { WIFI_LOG("AP: assoc request without auth\n"); return 0; }
    ap_.client_status = 2;
    WIFI_LOG("AP: client associated\n");
    w.u16_(0x0010); w.u16_(0x0000);
    w.mac(&data[10]); w.mac(kApMac); w.mac(kApMac);
    seqno();
    w.u16_(0x0021);   // capability
    w.u16_(0);        // status: success
    w.u16_(0xC001);   // assoc ID
    w.u8_(0x01); w.u8_(0x02); w.u8_(0x82); w.u8_(0x84);   // rates
    ap_.packet_len = w.len(); ap_.rx_num = 1;
    return len;
  }
  case 0x4: {   // probe request
    // Answer as whatever SSID is requested.
    std::string ssid = ap_.name;
    if (len >= 26 && data[24] == 0x00) {
      const int n = data[25];
      if (n > 0 && n <= 32 && 26 + n <= len) ssid.assign(reinterpret_cast<const char*>(&data[26]), static_cast<size_t>(n));
    }
    w.u16_(0x0050); w.u16_(0x0000);
    w.mac(&data[10]); w.mac(kApMac); w.mac(kApMac);
    seqno();
    w.u64_(ap_.us_counter);
    w.u16_(128);      // beacon interval
    w.u16_(0x0021);   // capability
    w.u8_(0x01); w.u8_(0x02); w.u8_(0x82); w.u8_(0x84);   // rates
    w.u8_(0x03); w.u8_(0x01); w.u8_(kApChannel);           // current channel
    w.u8_(0x00); w.u8_(static_cast<u8>(ssid.size())); w.bytes(ssid.data(), ssid.size());
    ap_.packet_len = w.len(); ap_.rx_num = 1;
    return len;
  }
  case 0xA: {   // deassoc
    if (!mac_equal(&data[16], kApMac)) return 0;
    ap_.client_status = 1;
    WIFI_LOG("AP: client deassociated\n");
    w.u16_(0x00A0); w.u16_(0x0000);
    w.mac(&data[10]); w.mac(kApMac); w.mac(kApMac);
    seqno();
    w.u16_(3);   // reason code
    ap_.packet_len = w.len(); ap_.rx_num = 1;
    return len;
  }
  case 0xB: {   // auth
    if (!mac_equal(&data[16], kApMac)) return 0;
    ap_.client_status = 1;
    WIFI_LOG("AP: client authenticated\n");
    w.u16_(0x00B0); w.u16_(0x0000);
    w.mac(&data[10]); w.mac(kApMac); w.mac(kApMac);
    seqno();
    w.u16_(0);   // auth algorithm (open)
    w.u16_(2);   // auth sequence
    w.u16_(0);   // status code (success)
    ap_.packet_len = w.len(); ap_.rx_num = 1;
    return len;
  }
  case 0xC: {   // deauth
    if (!mac_equal(&data[16], kApMac)) return 0;
    ap_.client_status = 0;
    WIFI_LOG("AP: client deauthenticated\n");
    w.u16_(0x00C0); w.u16_(0x0000);
    w.mac(&data[10]); w.mac(kApMac); w.mac(kApMac);
    seqno();
    w.u16_(3);   // reason code
    ap_.packet_len = w.len(); ap_.rx_num = 1;
    return len;
  }
  default:
    WIFI_LOG("AP: unknown management frame type %X\n", (framectl >> 4) & 0xF);
    return 0;
  }
}

int Wifi::ap_send(const u8* data, int len) {
  if (data[9] != kApChannel) return 0;
  data += 12;
  const u16 framectl = ld16(&data[0]);
  switch ((framectl >> 2) & 3) {
  case 0: return ap_handle_management(data, len);
  case 1: return 0;   // control: TODO (melonDS)
  case 2: {   // data
    if ((framectl & 0x0300) != 0x0100) { WIFI_LOG("AP: data frame with bad fromDS/toDS bits %04X\n", framectl); return 0; }
    if (ld32(&data[24]) == 0x0003AAAA && ld16(&data[28]) == 0x0000) {   // LLC/SNAP
      if (ap_.client_status != 2) { WIFI_LOG("AP: data before association\n"); return 0; }
      const int lan_len = (len - 30 - 4) + 14;
      if (lan_len < 14 || lan_len > static_cast<int>(ap_.lan.size())) return 0;
      std::memcpy(&ap_.lan[0], &data[16], 6);   // destination MAC
      std::memcpy(&ap_.lan[6], &data[10], 6);   // source MAC
      st16(&ap_.lan[12], ld16(&data[30]));      // type
      std::memcpy(&ap_.lan[14], &data[32], static_cast<size_t>(lan_len - 14));
      if (net_) net_->send(ap_.lan.data(), lan_len);
    }
    return len;
  }
  }
  return 0;
}

int Wifi::ap_recv(u8* data) {
  if (ap_.beacon_due) {
    ap_.beacon_due = false;
    PacketWriter w(data + 12);
    w.u16_(0x0080); w.u16_(0x0000);
    static const u8 bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    w.mac(bcast); w.mac(kApMac); w.mac(kApMac);
    w.u16_(ap_.seq_no); ap_.seq_no += 0x10;
    w.u64_(ap_.us_counter);
    w.u16_(100); w.u16_(0x0021);   // beacon interval (TU), capability
    w.u8_(0x01); w.u8_(0x02); w.u8_(0x82); w.u8_(0x84);
    w.u8_(0x03); w.u8_(0x01); w.u8_(kApChannel);
    w.u8_(0x05); w.u8_(0x04); w.u8_(0); w.u8_(0); w.u8_(0); w.u8_(0);   // TIM
    w.u8_(0x00); w.u8_(static_cast<u8>(ap_.name.size())); w.bytes(ap_.name.data(), ap_.name.size());
    w.align4();
    w.u32_(0xDEADBEEF);   // checksum; not checked
    const int len = w.len();
    write_txh(data, len, 20);
    return len + 12;
  }
  if (ap_.rx_num) {
    ap_.rx_num = 0;
    PacketWriter w(data + 12);
    w.bytes(ap_.packet.data(), static_cast<size_t>(ap_.packet_len));
    w.align4();
    w.u32_(0xDEADBEEF);
    const int len = w.len();
    write_txh(data, len, 20);
    return len + 12;
  }
  if (ap_.client_status < 2 || !net_) return 0;
  int rxlen = net_->recv(ap_.lan.data());
  while (rxlen > 0) {
    if (rxlen > 14 && rxlen - 14 + 32 + 8 <= 2048 - 12 &&
        (mac_broadcast(&ap_.lan[0]) || mac_equal(&ap_.lan[0], mac())) && !mac_equal(&ap_.lan[6], mac())) {
      PacketWriter w(data + 12);
      w.u16_(0x0208); w.u16_(0x0000);
      w.mac(&ap_.lan[0]); w.mac(kApMac); w.mac(&ap_.lan[6]);
      w.u16_(ap_.seq_no); ap_.seq_no += 0x10;
      w.u32_(0x0003AAAA); w.u16_(0x0000);
      w.u16_(ld16(&ap_.lan[12]));
      w.bytes(&ap_.lan[14], static_cast<size_t>(rxlen - 14));
      w.align4();
      w.u32_(0xDEADBEEF);
      const int len = w.len();
      write_txh(data, len, 20);
      return len + 12;
    }
    rxlen = net_->recv(ap_.lan.data());
  }
  return 0;
}

// ---- save state --------------------------------------------------------------
template <class S> void Wifi::sync_state_regs(S& s) {
  s.fields(ram_, io_, bb_, bb_ro_, rf_, rf_version_, random_);
}
template <class S> void Wifi::sync_state_timer(S& s) {
  s.fields(on_, timer_err_, us_timestamp_, us_counter_, us_compare_, us_until_power_on_, cmd_counter_, rx_counter_, block_beacon_irq14_);
}
template <class S> void Wifi::sync_state_engine(S& s) {
  s.fields(rf_channel_index_, rf_channel_data_, cur_channel_);
  for (auto& t : tx_slots_) s.fields(t.valid, t.addr, t.length, t.rate, t.cur_phase, t.cur_phase_time, t.halfword_time_mask);
  s.fields(tx_buffer_, rx_buffer_, rx_buffer_ptr_, rx_time_, rx_halfword_time_mask_, com_status_, tx_cur_slot_,
           mp_reply_timer_, mp_client_mask_, mp_client_fail_, mp_client_replies_, mp_last_seqno_,
           is_mp_, is_mp_client_, next_sync_, rx_timestamp_);
  s.fields(ap_.us_counter, ap_.seq_no, ap_.beacon_due, ap_.packet, ap_.packet_len, ap_.rx_num, ap_.client_status);
}
template void Wifi::sync_state_regs<state::Writer>(state::Writer&);
template void Wifi::sync_state_regs<state::Reader>(state::Reader&);
template void Wifi::sync_state_timer<state::Writer>(state::Writer&);
template void Wifi::sync_state_timer<state::Reader>(state::Reader&);
template void Wifi::sync_state_engine<state::Writer>(state::Writer&);
template void Wifi::sync_state_engine<state::Reader>(state::Reader&);

} // namespace ds::io
