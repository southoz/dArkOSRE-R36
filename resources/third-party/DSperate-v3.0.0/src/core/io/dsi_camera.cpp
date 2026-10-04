// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/io/dsi_camera.h"
#include "core/nds.h"
#include "core/io/io.h"
#include "core/dma/ndma.h"
#include "core/state/state.h"
#include <algorithm>
#include <cstring>

namespace ds::io {

namespace {
// The camera IRQ marks camera VBlank; each scanline takes ~3173 ARM7
// cycles. Values are scheduler ticks (x2).
constexpr u64 IRQ_INTERVAL = 2234248;
constexpr u64 SCANLINE_TIME = 3173;
constexpr u64 TRANSFER_START = IRQ_INTERVAL - SCANLINE_TIME * 480;
u16 get16(const u8* p) { u16 v; std::memcpy(&v, p, 2); return v; }
}  // namespace

// ---- sensor ------------------------------------------------------------------

void DsiCamera::reset() {
  reset_held_ = true;
  data_pos_ = reg_addr_ = 0;
  reg_data_ = 0;
  pll_div_ = 0x0366; pll_pdiv_ = 0x00F5; pll_cnt_ = 0x21F9;
  clocks_cnt_ = 0; standby_cnt_ = 0x4029; misc_cnt_ = 0;
  mcu_addr_ = 0;
  mcu_.fill(0);
  mcu_[0x2104] = 3;   // preview mode
  internal_y_ = transfer_y_ = 0;
  std::fill(frame_.begin(), frame_.end(), 0u);
}

bool DsiCamera::activated() const {
  if (standby_cnt_ & (1u << 14)) return false;   // standby
  if (!(misc_cnt_ & (1u << 9))) return false;     // data transfer not enabled
  return true;
}

void DsiCamera::start_transfer() {
  internal_y_ = transfer_y_ = 0;
  const u8 state = mcu_[0x2104];
  if (state == 3) {        // preview
    frame_width_ = get16(&mcu_[0x2703]); frame_height_ = get16(&mcu_[0x2705]);
    frame_read_mode_ = get16(&mcu_[0x2717]); frame_format_ = get16(&mcu_[0x2755]);
  } else if (state == 7) { // capture
    frame_width_ = get16(&mcu_[0x2707]); frame_height_ = get16(&mcu_[0x2709]);
    frame_read_mode_ = get16(&mcu_[0x272D]); frame_format_ = get16(&mcu_[0x2757]);
  } else {
    frame_width_ = frame_height_ = frame_read_mode_ = frame_format_ = 0;
  }
}

int DsiCamera::transfer_scanline(u32* buffer, int maxlen, int& nlines) {
  nlines = 0;
  if (transfer_y_ >= frame_height_ || internal_y_ >= 480) return 0;
  if (frame_width_ > 640 || frame_height_ > 480 || frame_width_ < 2 || frame_height_ < 2 || (frame_width_ & 1)) return 0;
  const int retlen = frame_width_ >> 1;
  int sy = internal_y_;
  if (frame_read_mode_ & 2) sy = 479 - sy;
  if (frame_read_mode_ & 1) {
    for (int dx = 0; dx < retlen && dx < maxlen; ++dx) buffer[dx] = frame_[sy * 320 + (dx * 640) / frame_width_];
  } else {
    for (int dx = 0; dx < retlen && dx < maxlen; ++dx) {
      const u32 val = frame_[sy * 320 + 319 - (dx * 640) / frame_width_];
      buffer[dx] = (val & 0xFF00FF00) | ((val >> 16) & 0xFF) | ((val & 0xFF) << 16);
    }
  }
  const int oldy = transfer_y_;
  do {
    ++internal_y_;
    transfer_y_ = (internal_y_ * frame_height_) / 480;
    ++nlines;
  } while (transfer_y_ == oldy && internal_y_ < 480);
  return retlen;
}

u8 DsiCamera::read(bool last) {
  if (reset_held_) return 0xFF;
  u8 ret;
  if (data_pos_ & 1) { ret = static_cast<u8>(reg_data_); reg_addr_ += 2; }
  else { reg_data_ = i2c_read_reg(static_cast<u16>(reg_addr_)); ret = static_cast<u8>(reg_data_ >> 8); }
  data_pos_ = last ? 0 : data_pos_ + 1;
  return ret;
}

void DsiCamera::write(u8 val, bool last) {
  if (reset_held_) return;
  if (data_pos_ < 2) {
    if (data_pos_ == 0) reg_addr_ = static_cast<u32>(val) << 8;
    else reg_addr_ |= val;
  } else if (data_pos_ & 1) {
    reg_data_ |= val;
    i2c_write_reg(static_cast<u16>(reg_addr_), reg_data_);
    reg_addr_ += 2;
  } else {
    reg_data_ = static_cast<u16>(val << 8);
  }
  data_pos_ = last ? 0 : data_pos_ + 1;
}

u16 DsiCamera::i2c_read_reg(u16 addr) const {
  switch (addr) {
  case 0x0000: return 0x2280;   // chip ID
  case 0x0010: return pll_div_;
  case 0x0012: return pll_pdiv_;
  case 0x0014: return pll_cnt_;
  case 0x0016: return clocks_cnt_;
  case 0x0018: return standby_cnt_;
  case 0x001A: return misc_cnt_;
  case 0x098C: return mcu_addr_;
  case 0x0990: case 0x0992: case 0x0994: case 0x0996:
  case 0x0998: case 0x099A: case 0x099C: case 0x099E: {
    const u16 base = static_cast<u16>((mcu_addr_ & 0x7FFF) + (addr - 0x0990));
    u16 ret = mcu_[base & 0x7FFF];
    if (!(mcu_addr_ & 0x8000)) ret |= static_cast<u16>(mcu_[(base + 1) & 0x7FFF] << 8);
    return ret;
  }
  case 0x301A: return static_cast<u16>(((~standby_cnt_) & 0x4000) >> 12);
  default: return 0;
  }
}

void DsiCamera::i2c_write_reg(u16 addr, u16 val) {
  switch (addr) {
  case 0x0010: pll_div_ = val & 0x3FFF; return;
  case 0x0012: pll_pdiv_ = val & 0xBFFF; return;
  case 0x0014: val &= 0x7FFF; val |= static_cast<u16>((val & 0x0002) << 14); pll_cnt_ = val; return;
  case 0x0016: clocks_cnt_ = val; return;
  case 0x0018: val &= 0x003F; val |= static_cast<u16>((val & 0x0001) << 14); standby_cnt_ = val; return;
  case 0x001A: misc_cnt_ = val & 0x0B7B; return;
  case 0x098C: mcu_addr_ = val; return;
  case 0x0990: case 0x0992: case 0x0994: case 0x0996:
  case 0x0998: case 0x099A: case 0x099C: case 0x099E: {
    const u16 base = static_cast<u16>((mcu_addr_ & 0x7FFF) + (addr - 0x0990));
    mcu_write(base, static_cast<u8>(val));
    if (!(mcu_addr_ & 0x8000)) mcu_write(static_cast<u16>(base + 1), static_cast<u8>(val >> 8));
    return;
  }
  default: return;
  }
}

void DsiCamera::mcu_write(u16 addr, u8 val) {
  addr &= 0x7FFF;
  if (addr == 0x2103) {        // SEQ_CMD
    mcu_[addr] = 0;
    if (val == 2) mcu_[0x2104] = 7;        // capture
    else if (val == 1) mcu_[0x2104] = 3;   // preview
    return;
  }
  if (addr == 0x2104) return;  // SEQ_STATE, read-only
  mcu_[addr] = val;
}

template <class S> void DsiCamera::sync_state(S& s) {
  s.fields(reset_held_, data_pos_, reg_addr_, reg_data_, pll_div_, pll_pdiv_, pll_cnt_, clocks_cnt_, standby_cnt_, misc_cnt_,
           mcu_addr_, mcu_, frame_width_, frame_height_, frame_read_mode_, frame_format_, internal_y_, transfer_y_);
}
template void DsiCamera::sync_state<state::Writer>(state::Writer&);
template void DsiCamera::sync_state<state::Reader>(state::Reader&);

// ---- module ------------------------------------------------------------------

void DsiCamModule::reset() {
  module_cnt_ = 0; cnt_ = 0;
  crop_start_ = crop_end_ = 0;
  transferring_ = false;
  std::memset(buf_, 0, sizeof buf_);
  cur_buf_ = 0;
  buffer_num_lines_ = 0;
  cur_cam_ = nullptr;
  // Not a camera reset; sensors are reset separately through the I2C host.
  nds_.sched.cancel(EventId::CamTransfer);
  nds_.sched.schedule(EventId::CamIrq, nds_.sched.now() + IRQ_INTERVAL * 2, irq_event, 0);
}

DsiCamera* DsiCamModule::active_camera() {
  if (cam_[0].activated()) return &cam_[0];
  if (cam_[1].activated()) return &cam_[1];
  return nullptr;
}

void DsiCamModule::irq_event(NDS& nds, u32) { nds.io.cam.irq(); }
void DsiCamModule::transfer_event(NDS& nds, u32 line) { nds.io.cam.transfer_scanline(line); }

void DsiCamModule::irq() {
  if (DsiCamera* cam = active_camera()) {
    cam->start_transfer();
    if (cnt_ & (1u << 11)) nds_.io.request_irq(Cpu::ARM9, IRQ_DSI_CAMERA);
    cur_cam_ = cam;
    nds_.sched.schedule(EventId::CamTransfer, nds_.sched.event_base7() + TRANSFER_START * 2, transfer_event, 0);
  }
  // Periodic: reschedule from the nominal time, not the actual firing time.
  nds_.sched.schedule(EventId::CamIrq, nds_.sched.event_time() + IRQ_INTERVAL * 2, irq_event, 0);
}

void DsiCamModule::transfer_scanline(u32 line) {
  if (cnt_ & (1u << 4)) { transferring_ = false; return; }
  if (line == 0) {
    if (!(cnt_ & (1u << 15))) return;
    buffer_num_lines_ = 0;
    transferring_ = true;
  }
  PixelBuffer& b = buf_[cur_buf_];
  u32* dst = &b.data[b.write_pos];
  const int maxlen = 512 - static_cast<int>(b.write_pos);
  u32 tmp[512];
  int lines_next;
  const int datalen = cur_cam_ ? cur_cam_->transfer_scanline(tmp, 512, lines_next) : (lines_next = 0, 0);
  const u64 delay = static_cast<u64>(lines_next) * SCANLINE_TIME;
  int copystart = 0, copylen = datalen;
  bool line_last = false;
  bool skip = false;
  if (cnt_ & (1u << 14)) {   // crop
    const int ystart = (crop_start_ >> 16) & 0x1FF, yend = (crop_end_ >> 16) & 0x1FF;
    if (static_cast<int>(line) < ystart || static_cast<int>(line) > yend) {
      if (static_cast<int>(line) == yend + 1) line_last = true;
      skip = true;
    } else {
      const int xstart = (crop_start_ >> 1) & 0x1FF, xend = (crop_end_ >> 1) & 0x1FF;
      copystart = xstart;
      copylen = xend + 1 - xstart;
      if (copystart + copylen > datalen) copylen = datalen - copystart;
      if (copylen < 0) copylen = 0;
    }
  }
  if (!skip) {
    if (copylen > maxlen) copylen = maxlen;
    if (cnt_ & (1u << 13)) {   // convert to RGB
      for (int i = 0; i < copylen; ++i) {
        const u32 val = tmp[copystart + i];
        int y1 = val & 0xFF, u = (val >> 8) & 0xFF, y2 = (val >> 16) & 0xFF, v = (val >> 24) & 0xFF;
        u -= 128; v -= 128;
        auto chan = [](int c) { return std::clamp(c, 0, 255); };
        const int r1 = chan(y1 + ((v * 91881) >> 16)), g1 = chan(y1 - ((v * 46793) >> 16) - ((u * 22544) >> 16)), b1 = chan(y1 + ((u * 116129) >> 16));
        const int r2 = chan(y2 + ((v * 91881) >> 16)), g2 = chan(y2 - ((v * 46793) >> 16) - ((u * 22544) >> 16)), b2 = chan(y2 + ((u * 116129) >> 16));
        const u32 c1 = static_cast<u32>((r1 >> 3) | ((g1 >> 3) << 5) | ((b1 >> 3) << 10) | 0x8000);
        const u32 c2 = static_cast<u32>((r2 >> 3) | ((g2 >> 3) << 5) | ((b2 >> 3) << 10) | 0x8000);
        dst[i] = c1 | (c2 << 16);
      }
    } else if (copylen > 0) {
      std::memcpy(dst, &tmp[copystart], static_cast<size_t>(copylen) * sizeof(u32));
    }
    b.write_pos += static_cast<u32>(copylen);
    if (b.write_pos > 512) b.write_pos = 512;
    const u32 numscan = cnt_ & 0x000F;
    if (buffer_num_lines_ >= numscan) { buffer_num_lines_ = 0; swap_pixel_buffers(); }
    else ++buffer_num_lines_;
  }
  const bool done = cur_cam_ ? cur_cam_->transfer_done() : true;
  if ((done || line_last) && buffer_num_lines_ > 0) { buffer_num_lines_ = 0; swap_pixel_buffers(); }
  if (done) { transferring_ = false; return; }
  // Non-periodic: based on the ARM7's clock, not this event's firing time.
  nds_.sched.schedule(EventId::CamTransfer, nds_.sched.event_base7() + delay * 2, transfer_event, line + 1);
}

void DsiCamModule::swap_pixel_buffers() {
  PixelBuffer& other = buf_[cur_buf_ ^ 1];
  if (other.read_pos < other.write_pos) {   // overrun
    cnt_ |= 1u << 4;
    transferring_ = false;
  } else {
    buf_[cur_buf_].read_pos = 0;
    other.write_pos = 0;
    cur_buf_ ^= 1;
    nds_.ndma.check(Cpu::ARM9, 0x0B);
  }
}

u8 DsiCamModule::read8(u32) { return 0; }

u16 DsiCamModule::read16(u32 addr) {
  switch (addr) {
  case 0x04004200: return static_cast<u16>(module_cnt_ | (1u << 7));
  case 0x04004202: return static_cast<u16>(cnt_ | (transferring_ ? (1u << 15) : 0));
  default: return 0;
  }
}

u32 DsiCamModule::read32(u32 addr) {
  switch (addr) {
  case 0x04004204: {
    PixelBuffer& b = buf_[cur_buf_ ^ 1];
    if (b.read_pos < b.write_pos) return b.data[b.read_pos++];
    if (b.read_pos > 0) return b.data[b.read_pos - 1];
    return b.data[0];
  }
  case 0x04004210: return crop_start_;
  case 0x04004214: return crop_end_;
  default: return 0;
  }
}

void DsiCamModule::write8(u32, u8) {}

void DsiCamModule::write16(u32 addr, u16 val) {
  switch (addr) {
  case 0x04004200: {
    if (transferring()) return;
    const u16 old = module_cnt_;
    module_cnt_ = val;
    // bit 5 holds the cameras in reset
    if (!(module_cnt_ & (1u << 5)) && (old & (1u << 5))) { cam_[0].reset(); cam_[1].reset(); }
    else if ((module_cnt_ & (1u << 5)) && !(old & (1u << 5))) { cam_[0].release_reset(); cam_[1].release_reset(); }
    return;
  }
  case 0x04004202: {
    u16 oldmask;
    if (transferring()) { val &= 0x8F20; oldmask = 0x601F; }
    else { val &= 0xEF2F; oldmask = 0x0010; }
    cnt_ = static_cast<u16>((cnt_ & oldmask) | (val & ~0x0020));
    if (val & (1u << 5)) {
      cnt_ &= static_cast<u16>(~(1u << 4));
      std::memset(buf_, 0, sizeof buf_);
      cur_buf_ = 0;
    }
    return;
  }
  case 0x04004210: if (!transferring()) crop_start_ = (crop_start_ & 0x01FF0000) | (val & 0x03FE); return;
  case 0x04004212: if (!transferring()) crop_start_ = (crop_start_ & 0x03FE) | (static_cast<u32>(val & 0x01FF) << 16); return;
  case 0x04004214: if (!transferring()) crop_end_ = (crop_end_ & 0x01FF0000) | (val & 0x03FE); return;
  case 0x04004216: if (!transferring()) crop_end_ = (crop_end_ & 0x03FE) | (static_cast<u32>(val & 0x01FF) << 16); return;
  default: return;
  }
}

void DsiCamModule::write32(u32 addr, u32 val) {
  if (transferring()) return;
  if (addr == 0x04004210) crop_start_ = val & 0x01FF03FE;
  else if (addr == 0x04004214) crop_end_ = val & 0x01FF03FE;
}

template <class S> void DsiCamModule::sync_state(S& s) {
  s.fields(module_cnt_, cnt_, crop_start_, crop_end_, transferring_,
           buf_[0].data, buf_[0].read_pos, buf_[0].write_pos, buf_[1].data, buf_[1].read_pos, buf_[1].write_pos,
           cur_buf_, buffer_num_lines_);
  cam_[0].sync_state(s);
  cam_[1].sync_state(s);
  if constexpr (S::reading) cur_cam_ = active_camera();
}
template void DsiCamModule::sync_state<state::Writer>(state::Writer&);
template void DsiCamModule::sync_state<state::Reader>(state::Reader&);

}  // namespace ds::io
