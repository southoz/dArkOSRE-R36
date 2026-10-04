// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/gpu/line_regs.h"
#include "core/types.h"

#include <array>
#include <atomic>

namespace ds { struct NDS; }

namespace ds::gpu {

// A 2D engine's registers (A at 0x04000000, B at 0x04001000), without the
// drawing. Guest writes update a guest-visible mirror and are journaled with
// their line stamp; the render side only changes via replay_to(), so a
// deferred render sees each write on its line. What a line is drawn from is
// taken with capture() / begin_line().
class Engine2DRegs {
public:
  // L_SPRITES draws the sprites of a line from inside the journal replay.
  using SpriteHook = void (*)(void* ctx, u32 line);
  Engine2DRegs(NDS& nds, int num, SpriteHook hook, void* ctx) : nds_(nds), num_(num), sprite_hook_(hook), sprite_ctx_(ctx) { reset(); }
  void reset();
  template <class S> void sync_fields(S& s);   // journal must be drained
  void after_load();

  // Guest side; addr is the full address.
  u32  read(u32 addr, u32 width);
  void write(u32 addr, u32 width, u32 value);
  void powcnt_write(u16 value);                          // POWCNT1: enable (bit 1 A, 9 B), swap (15)
  void master_bright_write(u16 value);
  void palette_written(u32 off, u32 width, u32 value);   // off in this engine's 1 KB (OBJ at 0x200)
  void oam_written(u32 off, u32 width, u32 value);

  // Render side.
  void replay_to(u32 stamp);               // inclusive
  void apply_pending() { replay_to(0xFFFF); }
  void frame_done();                       // journal must be drained by now

  // Per-line hooks, in hardware order; journaled so they stay ordered with register writes.
  enum Latch : u8 { L_WINDOWS, L_PREDRAW, L_POSTDRAW, L_SPRITES };
  void latch(Latch k, u32 line, bool reset);
  void update_windows(u32 line);           // scanline start
  void pre_draw(u32 line, bool frame_reset);   // HBlank, before drawing `line`
  void post_draw(bool frame_reset);        // HBlank, after drawing

  // The current render-side state.
  LineRegs capture() const;
  // capture() for a line that is drawn: also carries the window x state into
  // the next line, which only a drawn line does.
  LineRegs begin_line();

  bool enabled() const { return enabled_; }
  u16  master_bright() const { return master_bright_; }
  int  screen() const { return screen_; }
  u32  dispcnt() const { return dispcnt_; }
  bool forced_blank() const { return forced_blank_; }
  u32  bldcnt() const { return bldcnt_; }
  u32  eva() const { return eva_; }
  u32  evb() const { return evb_; }
  u32  evy() const { return evy_; }
  const u16* palette() const { return pal_.data(); }
  const u16* oam() const { return oam_.data(); }
  u32  pal_gen() const { return pal_gen_; }
  u32  oam_geom_gen() const { return oam_geom_gen_; }
  void debug_dump() const;

private:
  NDS& nds_;
  const int num_;
  const SpriteHook sprite_hook_;
  void* const sprite_ctx_;

  // Guest-visible mirror.
  u32 g_dispcnt_ = 0;
  std::array<u16, 4> g_bgcnt_{};
  std::array<u8, 4> g_wincnt_{};
  u16 g_bldcnt_ = 0, g_bldalpha_ = 0;
  bool g_enabled_ = false;

  enum JKind : u8 { J_REG, J_PAL, J_OAM, J_POWCNT, J_MBRIGHT, J_LATCH };
  struct JEntry { u16 stamp; u8 kind; u8 width; u16 addr; u32 value; };
  // Fixed array + atomic count: main appends while the worker replays.
  // On overflow, join the worker (Gpu::journal_full) then apply directly.
  static constexpr size_t JOURNAL_CAP = 16384;
  std::array<JEntry, JOURNAL_CAP> journal_{};
  std::atomic<u32> jn_{0};
  size_t jpos_ = 0;
  void queue(u8 kind, u32 addr, u32 width, u32 value);   // applies now if nothing is pending
  void apply(u8 kind, u32 addr, u32 width, u32 value);
  void apply_write(u32 addr, u32 width, u32 value);

  // ---- render side ----
  bool enabled_ = false;
  int screen_ = 1;
  u16 master_bright_ = 0;
  alignas(16) std::array<u16, 512> pal_{};   // BG 0-255, OBJ 256-511
  alignas(16) std::array<u16, 512> oam_{};
  u32 pal_gen_ = 1, oam_gen_ = 1;            // bumped on every change
  u32 oam_geom_gen_ = 1;                     // only on fields the sprite lists depend on

  u32 dispcnt_ = 0;
  std::array<u16, 4> bgcnt_{};
  std::array<u16, 4> bghofs_{}, bgvofs_{};
  std::array<s16, 2> pa_{}, pb_{}, pc_{}, pd_{};
  std::array<s32, 2> ref_x_{}, ref_y_{};             // as written (28-bit signed)
  std::array<s32, 2> ref_x_int_{}, ref_y_int_{};     // current line
  std::array<s32, 2> ref_x_reload_{}, ref_y_reload_{};
  std::array<u8, 4> win0_{}, win1_{};                // x1, x2, y1, y2
  std::array<u8, 4> wincnt_{};                       // WININ lo/hi, WINOUT lo/hi
  u8 bg_mosaic_w_ = 0, bg_mosaic_h_ = 0, obj_mosaic_w_ = 0, obj_mosaic_h_ = 0;
  u16 bldcnt_ = 0, bldalpha_ = 0;
  u8 eva_ = 16, evb_ = 0, evy_ = 0;

  // Latches: enables take effect with a delay, disables immediately.
  std::array<u32, 3> dispcnt_hist_{};
  u8 layer_enable_ = 0, obj_enable_ = 0;
  bool forced_blank_ = false;
  u8 win0_active_ = 0, win1_active_ = 0;            // bit0 y-range, bit1 x-range
  u8 bg_mosaic_y_ = 0, bg_mosaic_ymax_ = 0, obj_mosaic_y_ = 0;
  bool bg_mosaic_latch_ = true, obj_mosaic_latch_ = true;
  u32 bg_mosaic_line_ = 0, obj_mosaic_line_ = 0;
};

} // namespace ds::gpu
