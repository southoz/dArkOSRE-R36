// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// 2D engine registers: the guest mirror, the write journal and the per-line
// latches. Hardware behaviour per GBATEK.
#include "core/gpu/engine2d_regs.h"
#include "core/state/state.h"
#include "core/nds.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ds::gpu {

void Engine2DRegs::reset() {
  g_dispcnt_ = 0; g_bgcnt_.fill(0); g_wincnt_.fill(0); g_bldcnt_ = g_bldalpha_ = 0; g_enabled_ = false;
  jn_.store(0, std::memory_order_relaxed); jpos_ = 0;
  enabled_ = false; screen_ = 1 - num_; master_bright_ = 0;
  pal_.fill(0); oam_.fill(0); pal_gen_ = oam_gen_ = oam_geom_gen_ = 1;
  dispcnt_ = 0; dispcnt_hist_.fill(0);
  bgcnt_.fill(0); bghofs_.fill(0); bgvofs_.fill(0);
  pa_.fill(0); pb_.fill(0); pc_.fill(0); pd_.fill(0);
  ref_x_.fill(0); ref_y_.fill(0); ref_x_int_.fill(0); ref_y_int_.fill(0); ref_x_reload_.fill(0); ref_y_reload_.fill(0);
  win0_.fill(0); win1_.fill(0); wincnt_.fill(0);
  bg_mosaic_w_ = bg_mosaic_h_ = obj_mosaic_w_ = obj_mosaic_h_ = 0;
  bldcnt_ = 0; bldalpha_ = 0; eva_ = 16; evb_ = 0; evy_ = 0;
  layer_enable_ = obj_enable_ = 0; forced_blank_ = false;
  win0_active_ = win1_active_ = 0;
  bg_mosaic_y_ = bg_mosaic_ymax_ = obj_mosaic_y_ = 0;
  bg_mosaic_latch_ = obj_mosaic_latch_ = true;
  bg_mosaic_line_ = obj_mosaic_line_ = 0;
}

// ---- registers --------------------------------------------------------------

u32 Engine2DRegs::read(u32 addr, u32 width) {
  const u32 r = addr & 0xFFF;
  if (width == 32) return read(addr, 16) | (read(addr + 2, 16) << 16);
  if (width == 8) { const u32 v = read(addr & ~1u, 16); return (addr & 1) ? (v >> 8) & 0xFF : v & 0xFF; }
  switch (r) {
  case 0x00: return g_dispcnt_ & 0xFFFF;
  case 0x02: return g_dispcnt_ >> 16;
  case 0x08: case 0x0A: case 0x0C: case 0x0E: return g_bgcnt_[(r - 8) / 2];
  case 0x48: return g_wincnt_[0] | (g_wincnt_[1] << 8);
  case 0x4A: return g_wincnt_[2] | (g_wincnt_[3] << 8);
  case 0x50: return g_bldcnt_;
  case 0x52: return g_bldalpha_;
  default: return 0;      // write-only registers read as zero
  }
}

// Guest side: keep the read mirror current, then hand the write to the
// journal. Byte writes outside byte-addressed registers merge into a
// halfword here, so the journal only ever carries what apply_write handles.
void Engine2DRegs::write(u32 addr, u32 width, u32 value) {
  const u32 r = addr & 0xFFF;
  if (width == 8) {
    if (r < 4) {                                   // DISPCNT bytes
      const u32 shift = r * 8;
      g_dispcnt_ = (g_dispcnt_ & ~(0xFFu << shift)) | ((value & 0xFF) << shift);
      if (num_) g_dispcnt_ &= 0xC0B1FFF7;
      queue(J_REG, r, 8, value & 0xFF);
      return;
    }
    // BG0HOFS on engine A also scrolls the 3D layer even when powered down;
    // journaled byte-wise since the write-only register can't be merged.
    if (!num_ && (r == 0x10 || r == 0x11)) queue(J_REG, r, 8, value & 0xFF);
    if (!g_enabled_) return;
    switch (r) {
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
    case 0x4C: case 0x4D: case 0x52: case 0x53: case 0x54:
      queue(J_REG, r, 8, value & 0xFF); return;
    case 0x48: case 0x49: case 0x4A: case 0x4B:
      g_wincnt_[r - 0x48] = static_cast<u8>(value); queue(J_REG, r, 8, value & 0xFF); return;
    default: break;
    }
    const u32 cur = read(addr & ~1u, 16);
    const u16 merged = (addr & 1) ? static_cast<u16>((cur & 0x00FF) | (value << 8)) : static_cast<u16>((cur & 0xFF00) | (value & 0xFF));
    write(addr & ~1u, 16, merged);
    return;
  }
  if (width == 32) {
    switch (r) {
    case 0x00: g_dispcnt_ = num_ ? (value & 0xC0B1FFF7) : value; queue(J_REG, 0, 32, value); return;
    case 0x28: case 0x2C: case 0x38: case 0x3C: if (g_enabled_) queue(J_REG, r, 32, value); return;
    default: write(addr, 16, value & 0xFFFF); write(addr + 2, 16, value >> 16); return;
    }
  }
  value &= 0xFFFF;
  switch (r) {
  case 0x00: g_dispcnt_ = (g_dispcnt_ & 0xFFFF0000) | value; if (num_) g_dispcnt_ &= 0xC0B1FFF7; queue(J_REG, r, 16, value); return;
  case 0x02: g_dispcnt_ = (g_dispcnt_ & 0x0000FFFF) | (value << 16); if (num_) g_dispcnt_ &= 0xC0B1FFF7; queue(J_REG, r, 16, value); return;
  default: break;
  }
  // BG0HOFS on engine A scrolls the 3D layer even when powered down;
  // journaled and applied in display-line order like the rest.
  if (!g_enabled_) { if (!num_ && r == 0x10) queue(J_REG, r, 16, value); return; }
  switch (r) {
  case 0x08: case 0x0A: case 0x0C: case 0x0E: g_bgcnt_[(r - 8) / 2] = static_cast<u16>(value); break;
  case 0x48: g_wincnt_[0] = value & 0xFF; g_wincnt_[1] = value >> 8; break;
  case 0x4A: g_wincnt_[2] = value & 0xFF; g_wincnt_[3] = value >> 8; break;
  case 0x50: g_bldcnt_ = value & 0x3FFF; break;
  case 0x52: g_bldalpha_ = value & 0x1F1F; break;
  default: if (r >= 0x56) return; break;      // nothing there: not worth a journal entry
  }
  queue(J_REG, r, 16, value);
}

void Engine2DRegs::powcnt_write(u16 value) {
  g_enabled_ = value & (num_ ? 1 << 9 : 1 << 1);
  queue(J_POWCNT, 0, 16, value);
}
void Engine2DRegs::master_bright_write(u16 value) { queue(J_MBRIGHT, 0, 16, value); }
void Engine2DRegs::palette_written(u32 off, u32 width, u32 value) { queue(J_PAL, off, width, value); }
void Engine2DRegs::oam_written(u32 off, u32 width, u32 value) { queue(J_OAM, off, width, value); }

void Engine2DRegs::latch(Latch k, u32 line, bool reset) { queue(J_LATCH, k, 0, line | (reset ? 0x10000u : 0u)); }

void Engine2DRegs::queue(u8 kind, u32 addr, u32 width, u32 value) {
  const u32 stamp = nds_.gpu.journal_stamp(num_);
  if (stamp == Gpu::NO_STAMP) { apply(kind, addr, width, value); return; }
  u32 n = jn_.load(std::memory_order_relaxed);
  if (n == JOURNAL_CAP) { nds_.gpu.journal_full(); n = jn_.load(std::memory_order_relaxed); if (n == JOURNAL_CAP) { apply_pending(); jn_.store(0, std::memory_order_relaxed); jpos_ = 0; n = 0; } }
  journal_[n] = JEntry{static_cast<u16>(stamp), kind, static_cast<u8>(width), static_cast<u16>(addr), value};
  jn_.store(n + 1, std::memory_order_release);
}

void Engine2DRegs::replay_to(u32 stamp) {
  const u32 n = jn_.load(std::memory_order_acquire);
  while (jpos_ < n && journal_[jpos_].stamp <= stamp) {
    const JEntry& e = journal_[jpos_++];
    apply(e.kind, e.addr, e.width, e.value);
  }
}
void Engine2DRegs::frame_done() {
  // Nothing may be left: a stamp the render never reached would be a write
  // the frame silently lost.
  if (jpos_ != jn_.load(std::memory_order_relaxed)) { std::fprintf(stderr, "[eng%d] journal not drained: %zu of %u\n", num_, jpos_, jn_.load(std::memory_order_relaxed)); std::abort(); }
  jn_.store(0, std::memory_order_relaxed); jpos_ = 0;
}

void Engine2DRegs::apply(u8 kind, u32 addr, u32 width, u32 value) {
  switch (kind) {
  case J_REG: apply_write(addr, width, value); return;
  case J_PAL: std::memcpy(reinterpret_cast<u8*>(pal_.data()) + addr, &value, width / 8); ++pal_gen_; return;
  case J_OAM: {
    // Sprite lists read attr0's y/type/shape and attr1's size only; other
    // bits (most of what a game animates) don't bump oam_geom_gen_.
    u8* dst = reinterpret_cast<u8*>(oam_.data()) + addr;
    const u32 n = width / 8;
    u32 old = 0; std::memcpy(&old, dst, n);
    std::memcpy(dst, &value, n); ++oam_gen_;
    static constexpr u16 kGeom[4] = {0xC3FF, 0xC000, 0, 0};   // per halfword of an entry
    u32 changed = 0;
    for (u32 b = 0; b < n; ++b) changed |= ((old ^ value) >> (b * 8)) & (kGeom[((addr + b) >> 1) & 3] >> (((addr + b) & 1) * 8)) & 0xFF;
    if (changed) ++oam_geom_gen_;
    return;
  }
  case J_POWCNT:
    enabled_ = value & (num_ ? 1 << 9 : 1 << 1);
    screen_ = (value & (1 << 15)) ? num_ : 1 - num_;   // bit 15: engine A on the top screen
    return;
  case J_MBRIGHT: master_bright_ = static_cast<u16>(value); return;
  case J_LATCH: {
    const u32 line = value & 0xFFFF, reset = value & 0x10000;
    switch (static_cast<Latch>(addr)) {
    case L_WINDOWS: update_windows(line); return;
    case L_PREDRAW: pre_draw(line, reset != 0); return;
    case L_POSTDRAW: post_draw(reset != 0); return;
    case L_SPRITES: sprite_hook_(sprite_ctx_, line); return;
    }
    return;
  }
  }
}

// Render side. `addr` here is the register offset; byte writes only reach
// the byte-addressed registers (write() merged the rest).
void Engine2DRegs::apply_write(u32 addr, u32 width, u32 value) {
  const u32 r = addr & 0xFFF;
  if (width == 8) {
    if (r < 4) {                                   // DISPCNT bytes
      const u32 shift = r * 8;
      dispcnt_ = (dispcnt_ & ~(0xFFu << shift)) | ((value & 0xFF) << shift);
      if (num_) dispcnt_ &= 0xC0B1FFF7;
      return;
    }
    if (!num_ && r == 0x10) { nds_.gpu3d.set_render_xpos(static_cast<u16>(value & 0xFF), 0x00FF); return; }
    if (!num_ && r == 0x11) { nds_.gpu3d.set_render_xpos(static_cast<u16>(value << 8), 0xFF00); return; }
    if (!enabled_) return;
    switch (r) {
    case 0x40: win0_[1] = value; return; case 0x41: win0_[0] = value; return;
    case 0x42: win1_[1] = value; return; case 0x43: win1_[0] = value; return;
    case 0x44: win0_[3] = value; return; case 0x45: win0_[2] = value; return;
    case 0x46: win1_[3] = value; return; case 0x47: win1_[2] = value; return;
    case 0x48: case 0x49: case 0x4A: case 0x4B: wincnt_[r - 0x48] = value; return;
    case 0x4C: bg_mosaic_w_ = value & 0xF; bg_mosaic_h_ = value >> 4; return;
    case 0x4D: obj_mosaic_w_ = value & 0xF; obj_mosaic_h_ = value >> 4; return;
    case 0x52: bldalpha_ = (bldalpha_ & 0x1F00) | (value & 0x1F); eva_ = value & 0x1F; if (eva_ > 16) eva_ = 16; return;
    case 0x53: bldalpha_ = (bldalpha_ & 0x001F) | ((value & 0x1F) << 8); evb_ = value & 0x1F; if (evb_ > 16) evb_ = 16; return;
    case 0x54: evy_ = value & 0x1F; if (evy_ > 16) evy_ = 16; return;
    default: return;
    }
  }
  if (width == 32) {
    switch (r) {
    case 0x00: dispcnt_ = value; if (num_) dispcnt_ &= 0xC0B1FFF7; return;
    case 0x28: case 0x2C: case 0x38: case 0x3C:
      if (!enabled_) return;
      { const int i = r >= 0x38; s32 v = static_cast<s32>(value << 4) >> 4;
        if ((r & 0xF) == 0x8) { ref_x_[i] = v; ref_x_reload_[i] = v; } else { ref_y_[i] = v; ref_y_reload_[i] = v; } }
      return;
    default: apply_write(addr, 16, value & 0xFFFF); apply_write(addr + 2, 16, value >> 16); return;
    }
  }
  // 16-bit.
  switch (r) {
  case 0x00: dispcnt_ = (dispcnt_ & 0xFFFF0000) | value; if (num_) dispcnt_ &= 0xC0B1FFF7; return;
  case 0x02: dispcnt_ = (dispcnt_ & 0x0000FFFF) | (value << 16); if (num_) dispcnt_ &= 0xC0B1FFF7; return;
  default: break;
  }
  // The 3D layer's X scroll, engine powered or not (see write()).
  if (!num_ && r == 0x10) nds_.gpu3d.set_render_xpos(static_cast<u16>(value), 0xFFFF);
  // Everything below is ignored while the engine is powered down (POWCNT1),
  // which is the behaviour the reference implementation models.
  if (!enabled_) return;
  switch (r) {
  case 0x08: case 0x0A: case 0x0C: case 0x0E: bgcnt_[(r - 8) / 2] = value; return;
  case 0x10: case 0x14: case 0x18: case 0x1C: bghofs_[(r - 0x10) / 4] = value & 0x1FF; return;
  case 0x12: case 0x16: case 0x1A: case 0x1E: bgvofs_[(r - 0x12) / 4] = value & 0x1FF; return;
  case 0x20: pa_[0] = static_cast<s16>(value); return;
  case 0x22: pb_[0] = static_cast<s16>(value); return;
  case 0x24: pc_[0] = static_cast<s16>(value); return;
  case 0x26: pd_[0] = static_cast<s16>(value); return;
  case 0x30: pa_[1] = static_cast<s16>(value); return;
  case 0x32: pb_[1] = static_cast<s16>(value); return;
  case 0x34: pc_[1] = static_cast<s16>(value); return;
  case 0x36: pd_[1] = static_cast<s16>(value); return;
  case 0x28: case 0x2A: case 0x2C: case 0x2E: case 0x38: case 0x3A: case 0x3C: case 0x3E: {
    const int i = r >= 0x38;
    s32& ref = ((r & 0xF) < 0xC) ? ref_x_[i] : ref_y_[i];
    s32& reload = ((r & 0xF) < 0xC) ? ref_x_reload_[i] : ref_y_reload_[i];
    if (r & 2) { u32 hi = value & 0x0FFF; if (hi & 0x0800) hi |= 0xF000; ref = static_cast<s32>((static_cast<u32>(ref) & 0xFFFF) | (hi << 16)); }
    else ref = static_cast<s32>((static_cast<u32>(ref) & 0xFFFF0000) | value);
    reload = ref;
    return;
  }
  case 0x40: win0_[1] = value & 0xFF; win0_[0] = value >> 8; return;
  case 0x42: win1_[1] = value & 0xFF; win1_[0] = value >> 8; return;
  case 0x44: win0_[3] = value & 0xFF; win0_[2] = value >> 8; return;
  case 0x46: win1_[3] = value & 0xFF; win1_[2] = value >> 8; return;
  case 0x48: wincnt_[0] = value & 0xFF; wincnt_[1] = value >> 8; return;
  case 0x4A: wincnt_[2] = value & 0xFF; wincnt_[3] = value >> 8; return;
  case 0x4C: bg_mosaic_w_ = value & 0xF; bg_mosaic_h_ = (value >> 4) & 0xF; obj_mosaic_w_ = (value >> 8) & 0xF; obj_mosaic_h_ = value >> 12; return;
  case 0x50: bldcnt_ = value & 0x3FFF; return;
  case 0x52: bldalpha_ = value & 0x1F1F; eva_ = value & 0x1F; if (eva_ > 16) eva_ = 16; evb_ = (value >> 8) & 0x1F; if (evb_ > 16) evb_ = 16; return;
  case 0x54: evy_ = value & 0x1F; if (evy_ > 16) evy_ = 16; return;
  default: return;
  }
}

// ---- per-line state ---------------------------------------------------------

void Engine2DRegs::update_windows(u32 line) {
  if (!enabled_) return;
  // Vertical window edges are evaluated every line, windows enabled or not.
  const u8 y = line & 0xFF;
  if (y == win0_[3]) win0_active_ &= ~1; else if (y == win0_[2]) win0_active_ |= 1;
  if (y == win1_[3]) win1_active_ &= ~1; else if (y == win1_[2]) win1_active_ |= 1;
}

void Engine2DRegs::pre_draw(u32 line, bool frame_reset) {
  if (!enabled_) return;
  // Turning a layer on takes two lines (sprites: one); off/forced-blank is immediate.
  dispcnt_hist_[2] = dispcnt_hist_[1]; dispcnt_hist_[1] = dispcnt_hist_[0]; dispcnt_hist_[0] = dispcnt_;
  layer_enable_ = ((dispcnt_hist_[2] & dispcnt_) >> 8) & 0x1F;
  obj_enable_ = ((dispcnt_hist_[1] & dispcnt_) >> 12) & 1;
  forced_blank_ = ((dispcnt_hist_[2] | dispcnt_) >> 7) & 1;

  if (bg_mosaic_latch_) bg_mosaic_line_ = line;
  for (int i = 0; i < 2; ++i) {
    if (!(bgcnt_[2 + i] & (1 << 6)) || bg_mosaic_latch_) { ref_x_int_[i] = ref_x_[i]; ref_y_int_[i] = ref_y_[i]; }
  }
  if (dispcnt_ & (1 << 12)) {
    if (frame_reset || obj_mosaic_y_ == obj_mosaic_h_) { obj_mosaic_y_ = 0; obj_mosaic_latch_ = true; }
    else { obj_mosaic_y_ = (obj_mosaic_y_ + 1) & 0xF; obj_mosaic_latch_ = false; }
  }
  if (obj_mosaic_latch_) obj_mosaic_line_ = frame_reset ? 0 : (line + 1);
}

void Engine2DRegs::post_draw(bool frame_reset) {
  if (!enabled_) return;
  // BG mosaic height latches into an internal counter; OBJ mosaic compares live.
  if (frame_reset) { bg_mosaic_ymax_ = bg_mosaic_h_; bg_mosaic_y_ = 0; bg_mosaic_latch_ = true; }
  else if (bg_mosaic_y_ == bg_mosaic_ymax_) { bg_mosaic_ymax_ = bg_mosaic_h_; bg_mosaic_y_ = 0; bg_mosaic_latch_ = true; }
  else { bg_mosaic_y_ = (bg_mosaic_y_ + 1) & 0xF; bg_mosaic_latch_ = false; }
  for (int i = 0; i < 2; ++i) {
    if (!(layer_enable_ & (4 << i))) continue;        // reference points only advance for enabled layers
    if (frame_reset) { ref_x_[i] = ref_x_reload_[i]; ref_y_[i] = ref_y_reload_[i]; }
    else { ref_x_[i] += pb_[i]; ref_y_[i] += pd_[i]; }
  }
}

LineRegs Engine2DRegs::capture() const {
  LineRegs r;
  r.dispcnt = dispcnt_;
  for (int i = 0; i < 4; ++i) { r.bgcnt[i] = bgcnt_[i]; r.bghofs[i] = bghofs_[i]; r.bgvofs[i] = bgvofs_[i]; }
  for (int i = 0; i < 2; ++i) { r.ref_x[i] = ref_x_int_[i]; r.ref_y[i] = ref_y_int_[i]; r.pa[i] = pa_[i]; r.pc[i] = pc_[i]; }
  r.bldcnt = bldcnt_;
  r.master_bright = master_bright_;
  r.bg_mosaic_line = bg_mosaic_line_; r.obj_mosaic_line = obj_mosaic_line_;
  for (int i = 0; i < 4; ++i) { r.win0[i] = win0_[i]; r.win1[i] = win1_[i]; r.wincnt[i] = wincnt_[i]; }
  r.win0_active = win0_active_; r.win1_active = win1_active_;
  r.bg_mosaic_w = bg_mosaic_w_; r.obj_mosaic_w = obj_mosaic_w_;
  r.eva = eva_; r.evb = evb_; r.evy = evy_;
  r.layer_enable = layer_enable_; r.obj_enable = obj_enable_;
  r.enabled = enabled_; r.forced_blank = forced_blank_;
  r.screen = static_cast<u8>(screen_);
  r.pal_ver = pal_gen_; r.oam_ver = oam_gen_; r.oam_geom_ver = oam_geom_gen_;
  return r;
}

LineRegs Engine2DRegs::begin_line() {
  const LineRegs r = capture();
  // A drawn line with a window enabled leaves its x state (inside at x = 255,
  // i.e. x2 < x1 wrapping) for the next line to start from.
  if (enabled_ && !forced_blank_) {
    if (dispcnt_ & (1 << 14)) win1_active_ = static_cast<u8>((win1_active_ & 1) | (win1_[0] > win1_[1] ? 2 : 0));
    if (dispcnt_ & (1 << 13)) win0_active_ = static_cast<u8>((win0_active_ & 1) | (win0_[0] > win0_[1] ? 2 : 0));
  }
  return r;
}

void Engine2DRegs::debug_dump() const {
  std::fprintf(stderr, "[eng%d] dispcnt %08x enabled %d layer_en %02x obj_en %d fb %d bgcnt %04x %04x %04x %04x hofs %u %u %u %u vofs %u %u %u %u wincnt %02x %02x %02x %02x bldcnt %04x eva %u evb %u evy %u mos %u %u\n",
               num_, dispcnt_, enabled_, layer_enable_, obj_enable_, forced_blank_, bgcnt_[0], bgcnt_[1], bgcnt_[2], bgcnt_[3],
               bghofs_[0], bghofs_[1], bghofs_[2], bghofs_[3], bgvofs_[0], bgvofs_[1], bgvofs_[2], bgvofs_[3], wincnt_[0], wincnt_[1], wincnt_[2], wincnt_[3], bldcnt_, eva_, evb_, evy_, bg_mosaic_w_, bg_mosaic_h_);
}

template <class S> void Engine2DRegs::sync_fields(S& s) {
  s.fields(g_dispcnt_, g_bgcnt_, g_wincnt_, g_bldcnt_, g_bldalpha_, g_enabled_,
           enabled_, screen_, master_bright_, pal_, oam_,
           dispcnt_, bgcnt_, bghofs_, bgvofs_, pa_, pb_, pc_, pd_, ref_x_, ref_y_, ref_x_int_, ref_y_int_, ref_x_reload_, ref_y_reload_,
           win0_, win1_, wincnt_, bg_mosaic_w_, bg_mosaic_h_, obj_mosaic_w_, obj_mosaic_h_, bldcnt_, bldalpha_, eva_, evb_, evy_,
           dispcnt_hist_, layer_enable_, obj_enable_, forced_blank_, win0_active_, win1_active_,
           bg_mosaic_y_, bg_mosaic_ymax_, obj_mosaic_y_, bg_mosaic_latch_, obj_mosaic_latch_, bg_mosaic_line_, obj_mosaic_line_);
}
template void Engine2DRegs::sync_fields<state::Writer>(state::Writer&);
template void Engine2DRegs::sync_fields<state::Reader>(state::Reader&);

// Every generation moves on, so derived tables revalidate.
void Engine2DRegs::after_load() {
  jn_.store(0, std::memory_order_relaxed); jpos_ = 0;
  ++pal_gen_; ++oam_gen_; ++oam_geom_gen_;
}

} // namespace ds::gpu
