// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// DSi's cameras: two Aptina sensors on I2C (0x78 outer, 0x7A inner) plus the camera module at
// 0x04004200 (ARM9), framing at ~15 fps via an IRQ-started frame and a per-scanline event
// feeding two 512-word pixel buffers the NDMA (mode 0x0B) drains. No image source; frames black.
#pragma once
#include "core/types.h"
#include <array>
#include <vector>

namespace ds {
struct NDS;
}

namespace ds::io {

class DsiCamera {
 public:
  explicit DsiCamera(u32 num) : num_(num), frame_(640 * 480 / 2) {}
  void reset();
  void release_reset() { reset_held_ = false; }
  bool activated() const;
  void start_transfer();
  bool transfer_done() const { return transfer_y_ >= frame_height_; }
  int  transfer_scanline(u32* buffer, int maxlen, int& nlines);   // lengths in words

  // I2C device.
  void acquire() { data_pos_ = 0; }
  u8   read(bool last);
  void write(u8 val, bool last);

  template <class S> void sync_state(S& s);

 private:
  u16  i2c_read_reg(u16 addr) const;
  void i2c_write_reg(u16 addr, u16 val);
  void mcu_write(u16 addr, u8 val);

  u32 num_;
  bool reset_held_ = true;
  u32 data_pos_ = 0, reg_addr_ = 0;
  u16 reg_data_ = 0;
  u16 pll_div_ = 0, pll_pdiv_ = 0, pll_cnt_ = 0, clocks_cnt_ = 0, standby_cnt_ = 0, misc_cnt_ = 0;
  u16 mcu_addr_ = 0;
  std::array<u8, 0x8000> mcu_{};
  u16 frame_width_ = 0, frame_height_ = 0, frame_read_mode_ = 0, frame_format_ = 0;
  int internal_y_ = 0, transfer_y_ = 0;
  // YUYV, two pixels per word; stays black. Heap-allocated (NDS lives on the stack in unit
  // tests); not saved to state.
  std::vector<u32> frame_;
};

class DsiCamModule {
 public:
  explicit DsiCamModule(NDS& nds) : nds_(nds), cam_{DsiCamera(0), DsiCamera(1)} {}
  void reset();                       // schedules the first camera IRQ
  DsiCamera& camera(u32 i) { return cam_[i]; }

  static void irq_event(NDS& nds, u32 param);        // EventId::CamIrq
  static void transfer_event(NDS& nds, u32 line);    // EventId::CamTransfer

  u8   read8(u32 addr);
  u16  read16(u32 addr);
  u32  read32(u32 addr);
  void write8(u32 addr, u8 val);
  void write16(u32 addr, u16 val);
  void write32(u32 addr, u32 val);

  template <class S> void sync_state(S& s);

 private:
  struct PixelBuffer { u32 data[512]; u32 read_pos, write_pos; };
  void irq();
  void transfer_scanline(u32 line);
  void swap_pixel_buffers();
  bool transferring() const { return (cnt_ & (1u << 15)) || transferring_; }
  DsiCamera* active_camera();

  NDS& nds_;
  DsiCamera cam_[2];   // 0: 0x78 outer, 1: 0x7A inner
  u16 module_cnt_ = 0, cnt_ = 0;
  u32 crop_start_ = 0, crop_end_ = 0;
  bool transferring_ = false;
  PixelBuffer buf_[2] = {};
  u8  cur_buf_ = 0;
  u32 buffer_num_lines_ = 0;
  DsiCamera* cur_cam_ = nullptr;
};

}  // namespace ds::io
