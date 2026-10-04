// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// DS Wi-Fi block (ARM7, 0x04800000-0x0480FFFF): register file, 8 KB frame
// RAM, baseband/RF register indirection, transceiver power state machine,
// 8 us timer, TX/RX frame engine with Nintendo's local-multiplayer CMD/
// reply/ack sequencing. Port of melonDS's Wifi.cpp/WifiAP.cpp
// (GPL-3.0-or-later, melonDS team).
//
// Frames leave through an MpTransport (other emulators) or, outside
// multiplayer, the emulated access point to a NetDriver (host network).
#pragma once

#include "core/types.h"
#include "core/io/wifi_transport.h"
#include <array>
#include <string>

namespace ds { struct NDS; }

namespace ds::io {

class Wifi {
public:
  explicit Wifi(NDS& nds) : nds_(nds) {}
  void reset();

  // The bus side. Io gates both on POWCNT2 bit 1.
  u16  read16(u32 addr);
  void write16(u32 addr, u16 value);

  // POWCNT2 bit 1 or W_POWER_US changed: start or stop the 8 us timer.
  void update_power_on();
  void us_timer();

  // Outside world. Null means nobody is listening.
  void set_transport(MpTransport* mp) { mp_ = mp; }
  void set_net_driver(NetDriver* net) { net_ = net; }
  // Frames actually put on the air, not just radio power-on. Diagnostic
  // only; not in save state.
  u32 tx_frames() const { return tx_frames_; }
  MpTransport* transport() const { return mp_; }

  u32 host_syncs() const { return host_syncs_; }   // times this console, as MP host, synced a client
  // In a local-wireless exchange now, as host (armed beacon slot) or client
  // (has an association). Distinguishes Download Play from a Slot-1 launch.
  bool mp_active() const { return is_mp_ || is_mp_client_; }
  void trace_frame(int frame);
  const u8* mac() const { return reinterpret_cast<const u8*>(&io_[W_MACAddr0 / 2]); }
  const u8* bssid() const { return reinterpret_cast<const u8*>(&io_[W_BSSID0 / 2]); }

  template <class S> void sync_state_regs(S& s);
  template <class S> void sync_state_timer(S& s);
  template <class S> void sync_state_engine(S& s);

  // Register offsets.
  enum : u32 {
    W_ID = 0x000, W_ModeReset = 0x004, W_ModeWEP = 0x006, W_TXStatCnt = 0x008, W_IF = 0x010, W_IE = 0x012,
    W_MACAddr0 = 0x018, W_MACAddr1 = 0x01A, W_MACAddr2 = 0x01C, W_BSSID0 = 0x020, W_BSSID1 = 0x022, W_BSSID2 = 0x024,
    W_AIDLow = 0x028, W_AIDFull = 0x02A, W_TXRetryLimit = 0x02C, W_RXCnt = 0x030, W_WEPCnt = 0x032,
    W_TRXPower = 0x034, W_PowerUS = 0x036, W_PowerTX = 0x038, W_PowerState = 0x03C, W_PowerForce = 0x040,
    W_Random = 0x044, W_PowerDownCtrl = 0x048,
    W_RXBufBegin = 0x050, W_RXBufEnd = 0x052, W_RXBufWriteCursor = 0x054, W_RXBufWriteAddr = 0x056,
    W_RXBufReadAddr = 0x058, W_RXBufReadCursor = 0x05A, W_RXBufCount = 0x05C, W_RXBufDataRead = 0x060,
    W_RXBufGapAddr = 0x062, W_RXBufGapSize = 0x064,
    W_TXBufWriteAddr = 0x068, W_TXBufCount = 0x06C, W_TXBufDataWrite = 0x070, W_TXBufGapAddr = 0x074, W_TXBufGapSize = 0x076,
    W_TXSlotBeacon = 0x080, W_TXBeaconTIM = 0x084, W_ListenCount = 0x088, W_BeaconInterval = 0x08C, W_ListenInterval = 0x08E,
    W_TXSlotCmd = 0x090, W_TXSlotReply1 = 0x094, W_TXSlotReply2 = 0x098,
    W_TXSlotLoc1 = 0x0A0, W_TXSlotLoc2 = 0x0A4, W_TXSlotLoc3 = 0x0A8,
    W_TXReqReset = 0x0AC, W_TXReqSet = 0x0AE, W_TXReqRead = 0x0B0, W_TXSlotReset = 0x0B4, W_TXBusy = 0x0B6, W_TXStat = 0x0B8,
    W_Preamble = 0x0BC, W_CmdTotalTime = 0x0C0, W_CmdReplyTime = 0x0C4,
    W_RXFilter = 0x0D0, W_RXLenCrop = 0x0DA, W_RXFilter2 = 0x0E0,
    W_USCountCnt = 0x0E8, W_USCompareCnt = 0x0EA, W_CmdCountCnt = 0x0EE,
    W_USCompare0 = 0x0F0, W_USCompare1 = 0x0F2, W_USCompare2 = 0x0F4, W_USCompare3 = 0x0F6,
    W_USCount0 = 0x0F8, W_USCount1 = 0x0FA, W_USCount2 = 0x0FC, W_USCount3 = 0x0FE,
    W_ContentFree = 0x10C, W_PreBeacon = 0x110, W_CmdCount = 0x118, W_BeaconCount1 = 0x11C, W_BeaconCount2 = 0x134,
    W_BBCnt = 0x158, W_BBWrite = 0x15A, W_BBRead = 0x15C, W_BBBusy = 0x15E, W_BBMode = 0x160, W_BBPower = 0x168,
    W_RFData2 = 0x17C, W_RFData1 = 0x17E, W_RFBusy = 0x180, W_RFCnt = 0x184, W_TXHeaderCnt = 0x194, W_RFPins = 0x19C,
    W_RXStatIncIF = 0x1A8, W_RXStatIncIE = 0x1AA, W_RXStatHalfIF = 0x1AC, W_RXStatHalfIE = 0x1AE,
    W_TXErrorCount = 0x1C0, W_RXCount = 0x1C4, W_CMDStat0 = 0x1D0,
    W_TXSeqNo = 0x210, W_RFStatus = 0x214, W_IFSet = 0x21C, W_RXTXAddr = 0x268,
  };

private:
  static constexpr int kTimerInterval = 8;                        // us per event
  static constexpr u32 kTimeCheckMask = ~u32(kTimerInterval - 1);

  NDS& nds_;
  MpTransport* mp_ = nullptr;
  NetDriver*   net_ = nullptr;

  // ---- registers, RAM, baseband, RF ----
  std::array<u8, 0x2000> ram_{};
  std::array<u16, 0x800> io_{};
  std::array<u8, 0x100> bb_{}, bb_ro_{};
  std::array<u32, 0x40> rf_{};
  u8  rf_version_ = 2;
  u16 random_ = 1;
  std::array<u32, 2> rf_channel_index_{};
  std::array<u32, 28> rf_channel_data_{};   // [channel * 2 + n]
  int cur_channel_ = 0;

  // ---- timer ----
  bool on_ = false;
  u32  tx_frames_ = 0;
  s32  timer_err_ = 0;
  u64  us_timestamp_ = 0, us_counter_ = 0, us_compare_ = 0;
  s32  us_until_power_on_ = 0;
  u32  cmd_counter_ = 0, rx_counter_ = 0;
  bool block_beacon_irq14_ = false;

  // ---- frame engine ----
  struct TxSlot {
    bool valid = false;
    u16 addr = 0, length = 0;
    u8  rate = 0, cur_phase = 0;
    s32 cur_phase_time = 0;
    u32 halfword_time_mask = 0;
  };
  std::array<TxSlot, 6> tx_slots_{};
  std::array<u8, 0x2000> tx_buffer_{};
  std::array<u8, 2048> rx_buffer_{};
  u32 rx_buffer_ptr_ = 0;
  s32 rx_time_ = 0;
  u32 rx_halfword_time_mask_ = 0xFFFFFFFF;
  u32 com_status_ = 0;          // 0 = waiting for packets, 1 = receiving, 2 = sending
  s32 tx_cur_slot_ = -1;
  s32 mp_reply_timer_ = 0;
  u16 mp_client_mask_ = 0, mp_client_fail_ = 0;
  std::array<u8, 15 * 1024> mp_client_replies_{};
  u16 mp_last_seqno_ = 0xFFFF;
  bool is_mp_ = false, is_mp_client_ = false;
  u32  host_syncs_ = 0;
  int  last_rx_type_ = 0;            // trace only: which check_rx delivered the frame
  u64 next_sync_ = 0, rx_timestamp_ = 0;

  // ---- the emulated access point (melonDS WifiAP) ----
  struct Ap {
    std::string name = "DSperate-AP";
    u64 us_counter = 0;
    u16 seq_no = 0;
    bool beacon_due = false;
    std::array<u8, 2048> packet{};
    int packet_len = 0;
    int rx_num = 0;
    std::array<u8, 2048> lan{};
    int client_status = 0;      // 0 = none, 1 = authenticated, 2 = associated
  } ap_;
  void ap_reset();
  void ap_ms_timer();
  int  ap_handle_management(const u8* data, int len);
  int  ap_send(const u8* data, int len);
  int  ap_recv(u8* data);

  // helpers
  u16& reg(u32 off) { return io_[off / 2]; }
  u16  reg(u32 off) const { return io_[off / 2]; }
  u16  read16_inner(u32 addr);
  u16  ram16(u32 a) const;
  void ram16(u32 a, u16 v);
  void schedule_timer(bool first);
  void check_irq(u16 old_flags);
  void set_irq(u32 irq);
  void set_irq13();
  void set_irq14(int source);
  void set_irq15();
  void set_status(u32 status);
  void update_power_status(int power);   // 1 = on, 0 = no change, -1 = off
  void ms_timer();
  int  preamble_len(int rate) const;
  u32  num_clients(u16 mask) const;
  void increment_tx_count(const TxSlot& slot);
  void report_mp_reply_errors(u16 clientfail);
  void tx_send_frame(const TxSlot& slot, int num);
  void start_tx_locn(int nslot, int loc);
  void start_tx_cmd();
  void start_tx_beacon();
  void fire_tx();
  void send_mp_default_reply();
  void send_mp_reply(u16 clienttime, u16 clientmask);
  void send_mp_ack(u16 cmdcount, u16 clientfail);
  bool process_tx(TxSlot& slot, int num);
  void increment_rx_addr(u16& addr, u16 inc = 2);
  void start_rx();
  void finish_rx();
  void mp_client_reply_rx(int client);
  bool check_rx(int type);
  void change_channel();
  void rf_transfer_type2();
  void rf_transfer_type3();
};

} // namespace ds::io
