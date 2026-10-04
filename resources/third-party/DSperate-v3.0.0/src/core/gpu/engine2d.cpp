// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// 2D engine: registers and the portable line renderer. Hardware behaviour per
// GBATEK.
#include "core/gpu/engine2d.h"
#include "core/state/state.h"
#include "core/gpu/vram_map.h"
#include "core/gpu/kernels.h"
#include "core/nds.h"
#include "core/profile.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ds::gpu {

namespace {

// Extended palettes live in VRAM, outside the write journal, so they're
// validated against the copy taken at conversion time, once per VRAMCNT
// remap. Standard palettes and OAM are journaled and compare a generation word.
inline bool cmp_differs(const void* a, const void* b, size_t n, prof::Counter calls, prof::Counter diff) {
  prof::add(calls, 1);
  const bool d = std::memcmp(a, b, n) != 0;
  if (d) prof::add(diff, 1);
  return d;
}

// BGR555 palette entry -> 18-bit record. Bit 15 of a palette entry is the low
// green bit on paletted graphics (not on direct colour, VRAM or FIFO display).
inline Pixel rgb15_to_18(u16 c) {
  const u32 r = (c & 0x001F) << 1;
  const u32 g = ((c & 0x03E0) >> 4) | ((c & 0x8000) >> 15);
  const u32 b = (c & 0x7C00) >> 9;
  return r | (g << 8) | (b << 16);
}


} // namespace

Engine2D::Engine2D(NDS& nds, int num)
    : nds_(nds), num_(num), regs_(nds, num, [](void* e, u32 line) { static_cast<Engine2D*>(e)->render_sprites(line); }, this) { reset(); }

void Engine2D::reset() {
  regs_.reset();
  cur_ = spr_ = LineRegs{};
  for (auto& p : bg_) { p.vs.fill(0); p.any = false; p.table = nullptr; }
  pal18_gen_ = objpal18_gen_ = 0; extpal_checked_ = extpal_have_ = 0; objext_checked_ = objext_have_ = 0; obj_prio_mask_ = 0;
  obj_v_.fill(0); obj_attr_.fill(0); obj_alpha_.fill(0); obj_win_.fill(0); num_sprites_ = 0;
  oam_lists_gen_ = 0;
  out_.fill(0);
  line3d_ = nullptr;
}

// ---- memory helpers ---------------------------------------------------------

const VramMap& Engine2D::vram() const { return vm_override_ ? *vm_override_ : nds_.bus.vram_map(); }
const VramView& Engine2D::bg_vram() const { return num_ ? vram().bbg : vram().abg; }
const VramView& Engine2D::obj_vram() const { return num_ ? vram().bobj : vram().aobj; }
u16 Engine2D::bg_extpal(u32 slot, u32 pal, u32 idx) const {
  return vram().read16(num_ ? vram().bbg_extpal : vram().abg_extpal, slot * 0x2000 + pal * 0x200 + idx * 2);
}
u16 Engine2D::obj_extpal(u32 idx) const {
  return vram().read16(num_ ? vram().bobj_extpal : vram().aobj_extpal, idx * 2);
}

const Pixel* Engine2D::std_pal18() {
  if (pal18_gen_ != regs_.pal_gen()) {
    pal18_gen_ = regs_.pal_gen();
    kern::active::palette_to_18(palette(), pal18_.data(), 256);
  }
  return pal18_.data();
}

const Pixel* Engine2D::ext_pal18(u32 slot, u32 pal) {
  const u32 n = slot * 16 + pal;
  Pixel* dst = extpal18_.data() + n * 256;
  if (!(extpal_checked_ & (1ull << n))) {
    extpal_checked_ |= 1ull << n;
    u16* copy = extpal_copy_.data() + n * 256;
    const VramView& v = num_ ? vram().bbg_extpal : vram().abg_extpal;
    const u32 addr = slot * 0x2000 + pal * 0x200;
    alignas(16) u16 tmp[256];
    const u16* src = reinterpret_cast<const u16*>(v.direct(addr, 512));
    if (!src) { for (u32 i = 0; i < 256; ++i) tmp[i] = vram().read16(v, addr + i * 2); src = tmp; }
    if (!(extpal_have_ & (1ull << n)) || cmp_differs(src, copy, 512, prof::C_2D_CMP_BGEXT, prof::C_2D_CMPD_BGEXT)) {
      std::memcpy(copy, src, 512);
      kern::active::palette_to_18(copy, dst, 256);
      extpal_have_ |= 1ull << n;
    }
  }
  return dst;
}

void Engine2D::debug_dump(u32 line) {
  regs_.debug_dump();
  const VramView& vv = bg_vram();
  std::fprintf(stderr, "[eng%d] bgvram blocks:", num_);
  for (u32 b = 0; b < vv.blocks(); ++b) std::fprintf(stderr, " %x", vv.mask[b]);
  std::fprintf(stderr, "\n[eng%d] map[0..8] ", num_);
  for (u32 i = 0; i < 8; ++i) std::fprintf(stderr, "%04x ", vram().read16(vv, ((regs_.capture().bgcnt[0] & 0x1F00) << 3) + i * 2));
  std::fprintf(stderr, " extpal(slot0..3, pal0, idx1) %04x %04x %04x %04x", bg_extpal(0, 0, 1), bg_extpal(1, 0, 1), bg_extpal(2, 0, 1), bg_extpal(3, 0, 1));
  std::fprintf(stderr, " pal[0..8] ");
  for (u32 i = 0; i < 8; ++i) std::fprintf(stderr, "%04x ", palette()[i]);
  render_sprites(line);
  u32 objop = 0; for (u32 i = 0; i < 256; ++i) objop += (obj_attr_[i] & 0x80) != 0;
  std::fprintf(stderr, "\n[eng%d] sprites on line %u: %u, opaque obj pixels %u, objvram blocks:", num_, line, num_sprites_, objop);
  for (u32 b = 0; b < obj_vram().blocks(); ++b) std::fprintf(stderr, " %x", obj_vram().mask[b]);
  const u16* oam = regs_.oam();
  std::fprintf(stderr, "\n[eng%d] oam[0..3]:", num_);
  for (int n = 0; n < 4; ++n) std::fprintf(stderr, " %04x/%04x/%04x", oam[n * 4], oam[n * 4 + 1], oam[n * 4 + 2]);
  render_line(line);
  if (std::getenv("DS_DEBUG_DUMP_LINEOUT")) { std::fprintf(stderr, "[lineout] eng%d", num_); for (u32 i = 0; i < 256; ++i) std::fprintf(stderr, " %06x", out_[i] & 0xFFFFFF); std::fprintf(stderr, "\n"); }
  if (const char* xs = std::getenv("DS_DEBUG_DUMP_X")) {
    const u32 x = std::atoi(xs);
    std::fprintf(stderr, "[eng%d] X=%u out %08x top16 %04x tid %u top %08x id %02x kind %u alpha %u second16 %04x stid %u second %08x sid %02x win %02x objattr %02x objv %04x objalpha %u flat %d bgany %d%d%d%d bgv %04x %04x %04x %04x tables %p %p %p %p objstd %p objext %p\n", num_, x, out_[x], top16_[x], top_tid_[x], top_[x], top_id_[x], top_kind_[x], top_alpha_[x], second16_[x], second_tid_[x], second_[x], second_id_[x], win_[x], obj_attr_[x], obj_v_[x], obj_alpha_[x], !effect_possible(),
                 bg_[0].any, bg_[1].any, bg_[2].any, bg_[3].any, bg_[0].v()[x], bg_[1].v()[x], bg_[2].v()[x], bg_[3].v()[x], (const void*)tables_[0], (const void*)tables_[1], (const void*)tables_[2], (const void*)tables_[3], (const void*)tables_[4], (const void*)tables_[5]);
  }
  u32 op = 0; for (int bg = 0; bg < 4; ++bg) if (bg_[bg].any) for (u32 i = 0; i < 256; ++i) op += (bg_[bg].v()[i] & LV_OPAQUE) != 0;
  u32 nonbd = 0; for (u32 i = 0; i < 256; ++i) nonbd += top_id_[i] != L_BACKDROP;
  std::fprintf(stderr, "\n[eng%d] line %u: opaque bg pixels %u, non-backdrop top %u, out[128] %08x top %08x id %02x kind %u win %02x second %08x\n", num_, line, op, nonbd, out_[128], top_[128], top_id_[128], top_kind_[128], win_[128], second_[128]);
}

// ---- line rendering ---------------------------------------------------------

const Pixel Engine2D::zero_table_[256] = {};

namespace {
// DS_DEBUG_OUTHASH=1: hash of each engine's composite output per frame
// (and per line of frame DS_DEBUG_OUTHASH_FRAME).
u64 g_outhash[2];
const bool g_outhash_on = std::getenv("DS_DEBUG_OUTHASH") != nullptr;
const long g_outhash_frame = std::getenv("DS_DEBUG_OUTHASH_FRAME") ? std::atol(std::getenv("DS_DEBUG_OUTHASH_FRAME")) : -1;
}
void Engine2D::debug_outhash(u32 line) {
  u64 lh = 0;
  for (u32 i = 0; i < 256; ++i) { lh ^= out_[i]; lh *= 0x100000001b3ull; }
  u64& h = g_outhash[num_];
  h ^= lh; h *= 0x100000001b3ull;
  if (static_cast<long>(nds_.frame_count) == g_outhash_frame) std::fprintf(stderr, "[linehash] eng%d line %u %016llx\n", num_, line, static_cast<unsigned long long>(lh));
  if (line == 191) { std::fprintf(stderr, "[outhash] eng%d frame %llu %016llx\n", num_, static_cast<unsigned long long>(nds_.frame_count), static_cast<unsigned long long>(h)); h = 0; }
}

void Engine2D::render_line(u32 line) {
  struct AtExit { Engine2D* e; u32 line; ~AtExit() { if (g_outhash_on) e->debug_outhash(line); } } at_exit{this, line};
  cur_ = regs_.begin_line();
  if (!cur_.enabled) {
    // Powered-down engines output a fixed colour: black for A, white for B.
    out_.fill(num_ ? 0xFF3F3F3F : 0xFF000000);
    return;
  }
  if (cur_.forced_blank) { out_.fill(0xFF3F3F3F); return; }

  for (auto& p : bg_) p.any = false;
  if (prof::heavy) {
    prof::add(prof::C_2D_LINES, 1);
    if ((cur_.layer_enable & 0x10) && num_sprites_) prof::add(prof::C_2D_OBJ_LINES, 1);
    if (cur_.dispcnt & 0xE000) prof::add(prof::C_2D_WINDOW_LINES, 1);
    if (cur_.bldcnt & 0xC0) prof::add(prof::C_2D_EFFECT_LINES, 1);
  }
  const int mode = cur_.dispcnt & 7;
  auto bg_on = [&](int n) { return (cur_.layer_enable >> n) & 1; };

  // 1. Rasterise each enabled background into its plane.
  {
  prof::Scope sc(prof::BG_DRAW);
  switch (mode) {
  case 0: for (int n = 0; n < 4; ++n) if (bg_on(n)) draw_bg_text(line, n); break;
  case 1: if (bg_on(0)) draw_bg_text(line, 0); if (bg_on(1)) draw_bg_text(line, 1); if (bg_on(2)) draw_bg_text(line, 2); if (bg_on(3)) draw_bg_affine(line, 3); break;
  case 2: if (bg_on(0)) draw_bg_text(line, 0); if (bg_on(1)) draw_bg_text(line, 1); if (bg_on(2)) draw_bg_affine(line, 2); if (bg_on(3)) draw_bg_affine(line, 3); break;
  case 3: if (bg_on(0)) draw_bg_text(line, 0); if (bg_on(1)) draw_bg_text(line, 1); if (bg_on(2)) draw_bg_text(line, 2); if (bg_on(3)) draw_bg_extended(line, 3); break;
  case 4: if (bg_on(0)) draw_bg_text(line, 0); if (bg_on(1)) draw_bg_text(line, 1); if (bg_on(2)) draw_bg_affine(line, 2); if (bg_on(3)) draw_bg_extended(line, 3); break;
  case 5: if (bg_on(0)) draw_bg_text(line, 0); if (bg_on(1)) draw_bg_text(line, 1); if (bg_on(2)) draw_bg_extended(line, 2); if (bg_on(3)) draw_bg_extended(line, 3); break;
  case 6: if (bg_on(2)) draw_bg_large(line); break;
  case 7: if (bg_on(0)) draw_bg_text(line, 0); if (bg_on(1)) draw_bg_text(line, 1); break;
  }
  // BG0 is the 3D layer on engine A when DISPCNT bit 3 is set (modes 0-5, 7; mode 6 has no text BG0).
  if (!num_ && (cur_.dispcnt & 8) && bg_on(0)) draw_bg_3d();
  }

  // 2. Window plane, 3. sprite X mosaic, 4. priority select, 5. colour effects.
  if (prof::heavy) {
    u32 layers = 0; int only = -1;
    for (int n = 0; n < 4; ++n) if (bg_[n].any) { ++layers; only = n; }
    bool objpx = false;
    if ((cur_.layer_enable & 0x10) && num_sprites_) { u64 acc = 0; for (u32 i = 0; i < 256; i += 8) { u64 v; std::memcpy(&v, &obj_attr_[i], 8); acc |= v; } objpx = acc & 0x8080808080808080ull; }
    if (objpx) ++layers;
    prof::add(layers == 0 ? prof::C_2D_L0 : layers == 1 ? prof::C_2D_L1 : layers == 2 ? prof::C_2D_L2 : layers == 3 ? prof::C_2D_L3 : prof::C_2D_L4P, 1);
    if (layers == 1 && only >= 0) { u64 acc = ~0ull; for (u32 i = 0; i < 256; i += 4) { u64 v; std::memcpy(&v, bg_[only].v() + i, 8); acc &= v; } if ((acc & 0x8000800080008000ull) == 0x8000800080008000ull) prof::add(prof::C_2D_L1_FULL, 1); }
    if (objpx) prof::add(prof::C_2D_OBJ_PRESENT, 1);
    if (!num_ && (cur_.dispcnt & 8) && bg_[0].any) prof::add(prof::C_2D_3D_PRESENT, 1);
    if (cur_.dispcnt & 0xE000) prof::add(prof::C_2D_WIN_PRESENT, 1);
  }
  { DS_PROF(WINDOW); build_window_plane(); apply_sprite_mosaic_x(); }
  if (!effect_possible()) {
    // No colour effect touches this line: skip second-layer bookkeeping and
    // pick the cheapest applicable pass below.
    prof::add(prof::C_2D_FLAT_LINES, 1);
    DS_PROF(SELECT);
    u32 nlayers = 0;
    for (int n = 0; n < 4; ++n) if (bg_[n].any) ++nlayers;
    const bool objs = (cur_.layer_enable & 0x10) && num_sprites_ && obj_prio_mask_;
    if (!objs) {
      // Nothing on the line: the backdrop shows everywhere.
      if (nlayers == 0) { prof::add(prof::C_2D_FAST_BACKDROP, 1); out_.fill(std_pal18()[0] | 0xFF000000); return; }
      // With no windows and no OBJ pixels, a fully opaque BG that is the
      // topmost layer with content hides everything below it and can be
      // resolved directly. Walk planes topmost first (priority 0..3, BG0
      // before BG3 within a priority) and let the first with content decide.
      if (!(cur_.dispcnt & 0xE000) && !obj_prio_mask_) {
        int top = -1;
        for (int prio = 0; prio < 4 && top < 0; ++prio)
          for (int bg = 0; bg < 4; ++bg)
            if ((cur_.layer_enable & (1 << bg)) && bg_[bg].any && (cur_.bgcnt[bg] & 3) == prio) { top = bg; break; }
        if (top >= 0 && line_all_opaque(bg_[top])) {
          prof::add(prof::C_2D_FAST_ONE, 1);
          kern::active::resolve16_one(bg_[top].v(), bg_[top].table, out_.data());
          return;
        }
      }
    }
    select_layers_flat();
    return;
  }
  prof::add(prof::C_2D_EFFECT_LIVE, 1);
  // A fade over plain layers never reads beneath the top pixel, so the
  // second select/gather/kind-alpha records are unneeded.
  if (!needs_second()) {
    prof::add(prof::C_2D_FULL_FADE, 1);
    { DS_PROF(SELECT); select_layers_top(); }
    { DS_PROF(EFFECTS); kern::active::composite_line_fade(cur_.bldcnt, cur_.evy, top_.data(), top_id_.data(), win_.data(), out_.data()); }
    return;
  }
  prof::add(prof::C_2D_FULL_SECOND, 1);
  { DS_PROF(SELECT); select_layers(); }
  { DS_PROF(EFFECTS); colour_effects(); }
}

// Whether any pixel of the line could be changed by the colour-effects pass.
// Conservative: presence is checked per line, not per pixel.
bool Engine2D::effect_possible() const {
  u32 present = L_BACKDROP;
  for (int n = 0; n < 4; ++n) if (bg_[n].any) present |= 1u << n;
  const bool objs = (cur_.layer_enable & 0x10) && num_sprites_;
  if (objs) present |= L_OBJ;
  const u32 mode = (cur_.bldcnt >> 6) & 3;
  const bool second = ((cur_.bldcnt >> 8) & present) != 0;
  // Identity coefficients make the effect a no-op: EVY 0, or EVA 16/EVB 0.
  const bool blend_identity = cur_.eva == 16 && cur_.evb == 0;
  const bool mode_noop = (mode == 1 && blend_identity) || (mode >= 2 && cur_.evy == 0);
  if (mode != 0 && !mode_noop && (cur_.bldcnt & 0x3F & present) && (mode != 1 || second)) { prof::add(prof::C_2D_FULL_MODE, 1); return true; }
  if (!second) return false;
  if (!num_ && (cur_.dispcnt & 8) && bg_[0].any) {
    // 3D blends only where alpha is strictly between 0 and 31.
    if (kern::active::line_has_translucent_3d(line3d_)) { prof::add(prof::C_2D_FULL_3D, 1); return true; }
  }
  if (objs) {
    u64 acc = 0;
    for (u32 i = 0; i < 256; i += 8) { u64 v; std::memcpy(&v, &obj_attr_[i], 8); acc |= v; }
    // Bitmap sprites blend on their own alpha; semi-transparent ones with EVA/EVB.
    if ((acc & 0x0808080808080808ull) || (!blend_identity && (acc & 0x0404040404040404ull))) { prof::add(prof::C_2D_FULL_OBJ, 1); return true; }
  }
  return false;
}

// Text (tiled) background: gather the 33 tile rows covering the line into
// index rows, then one kernel call resolves them through the palettes and
// writes the row at the scroll offset into the padded plane.
void Engine2D::draw_bg_text(u32 line, int bg) {
  prof::add(prof::C_2D_BG_TEXT, 1);
  prof::add((cur_.bgcnt[bg] & (1 << 7)) ? prof::C_2D_BG_PAL256 : prof::C_2D_BG_PAL16, 1);
  const u16 cnt = cur_.bgcnt[bg];
  Layer& plane = bg_[bg];
  const VramView& vv = bg_vram();
  const VramMap& vm = vram();

  u32 xoff = cur_.bghofs[bg];
  u32 yoff = cur_.bgvofs[bg] + ((cnt & (1 << 6)) ? cur_.bg_mosaic_line : line);
  const bool wide = cnt & (1 << 14), tall = cnt & (1 << 15);
  u32 tileset = (cnt & 0x003C) << 12, tilemap = (cnt & 0x1F00) << 3;
  if (!num_) { tileset += (cur_.dispcnt & 0x07000000) >> 8; tilemap += (cur_.dispcnt & 0x38000000) >> 11; }
  if (tall) { tilemap += (yoff & 0x1F8) << 3; if (wide) tilemap += (yoff & 0x100) << 3; }
  else tilemap += (yoff & 0xF8) << 3;
  const u32 widexmask = wide ? 0x100 : 0;
  const bool c256 = cnt & (1 << 7);
  const bool extpal = cur_.dispcnt & (1u << 30);
  const u32 extslot = (bg < 2 && (cnt & 0x2000)) ? (2 + bg) : bg;
  const bool mosaic = (cnt & (1 << 6)) && cur_.bg_mosaic_w > 0;
  const u32 mw = cur_.bg_mosaic_w + 1;
  const u32 ty0 = yoff & 7;

  if (!mosaic) {
    const u8* map[2] = {vv.direct(tilemap, 64), nullptr};
    map[1] = wide ? vv.direct(tilemap + 0x800, 64) : map[0];
    alignas(16) u8 rows[33 * 8]; alignas(16) u16 tiles[33];
    u32 palmask = 0;
    plane.table = std_pal18();
    // Map entries first: an all-transparent row needs neither tile rows nor
    // the kernel. A map row is contiguous (32 entries per screen block,
    // wrapping into the next on a wide map), so two memcpys suffice.
    {
      u32 done = 0, mx = (xoff & 0xF8) >> 2, blk = ((xoff & ~7u) & widexmask) ? 1 : 0;
      while (done < 33) {
        const u32 avail = (64 - mx) / 2, take = avail < 33 - done ? avail : 33 - done;
        if (map[blk]) std::memcpy(tiles + done, map[blk] + mx, take * 2);
        else for (u32 k = 0; k < take; ++k) tiles[done + k] = vm.read16(vv, tilemap + mx + k * 2 + (blk << 11));
        done += take;
        mx = 0;
        if (widexmask) blk ^= 1;
      }
    }
    // The control bytes for the kernel and the uniformity test, over the
    // whole map row at once: ctl = palette (bits 12-15) | hflip (bit 10 -> 4).
    // The previous per-tile form was 32 scalar compares plus a dependent
    // chain inside the gather loop below.
    u8 ctl[33];
    const bool uniform = kern::active::text_ctl(tiles, ctl);
    if (uniform) {
      const u32 ty = (tiles[0] & (1 << 11)) ? 7 - ty0 : ty0;
      const u32 a = c256 ? tileset + ((tiles[0] & 0x3FF) << 6) + (ty << 3) : tileset + ((tiles[0] & 0x3FF) << 5) + (ty << 2);
      const u32 len = c256 ? 8 : 4;
      u64 row = 0;
      if (const u8* p = vv.direct(a, len)) std::memcpy(&row, p, len); else for (u32 i = 0; i < len; ++i) row |= static_cast<u64>(vm.read8(vv, a + i)) << (8 * i);
      if (row == 0) { plane.any = false; prof::add(prof::C_2D_BG_EMPTY, 1); return; }   // repeated blank tile: not even the rows are needed
    }
    // Rows are accumulated as gathered: an all-blank row needs neither the
    // kernel nor extended palettes. A row never crosses a 16 KB block, so
    // lookup is an index without direct()'s per-tile crossing test.
    u64 rowacc = 0;
    const u32 amask = vv.addr_mask();
    // Block pointer looked up only when the block changes: one map row's
    // tiles almost always share a block.
    u32 last_blk = ~0u; const u8* last_ptr = nullptr;
    auto block_ptr = [&](u32 am) { const u32 b = am / VramView::BLOCK; if (b != last_blk) { last_blk = b; last_ptr = vv.ptr[b]; } return last_ptr; };
    if (c256) {
      for (u32 t = 0; t < 33; ++t) {
        const u16 tile = tiles[t];
        const u32 ty = (tile & (1 << 11)) ? 7 - ty0 : ty0;
        palmask |= 1u << (tile >> 12);
        const u32 a = tileset + ((tile & 0x3FF) << 6) + (ty << 3), am = a & amask;
        u64 row;
        if (const u8* p = block_ptr(am)) std::memcpy(&row, p + (am & (VramView::BLOCK - 1)), 8);
        else { row = 0; for (u32 i = 0; i < 8; ++i) row |= static_cast<u64>(vm.read8(vv, a + i)) << (8 * i); }
        std::memcpy(rows + t * 8, &row, 8);
        rowacc |= row;
      }
    } else {
      for (u32 t = 0; t < 33; ++t) {
        const u16 tile = tiles[t];
        const u32 ty = (tile & (1 << 11)) ? 7 - ty0 : ty0;
        const u32 a = tileset + ((tile & 0x3FF) << 5) + (ty << 2), am = a & amask;
        u32 row;
        if (const u8* p = block_ptr(am)) std::memcpy(&row, p + (am & (VramView::BLOCK - 1)), 4);
        else { row = 0; for (u32 i = 0; i < 4; ++i) row |= static_cast<u32>(vm.read8(vv, a + i)) << (8 * i); }
        std::memcpy(rows + t * 4, &row, 4);
        rowacc |= row;
      }
    }
    if (!rowacc) { plane.any = false; prof::add(prof::C_2D_BG_EMPTY, 1); return; }
    const u32 shift = xoff & 7;
    u16* v = plane.v() - shift;
    if (!c256) plane.any = kern::active::text_row_16(rows, ctl, 33, v);
    else {
      if (extpal) {
        // Validate the extended palettes the row uses.
        for (u32 i = 0; i < 16; ++i) if (palmask & (1u << i)) ext_pal18(extslot, i);
        plane.table = extpal18_.data() + extslot * 4096;
      }
      plane.any = kern::active::text_row_256(rows, ctl, 33, extpal, v);
    }
    return;
  }

  // Mosaic: pixel by pixel.
  std::memset(plane.v(), 0, 512);
  plane.table = std_pal18();
  if (c256 && extpal) { for (u32 i = 0; i < 16; ++i) ext_pal18(extslot, i); plane.table = extpal18_.data() + extslot * 4096; }
  u32 cur_tile = 0, cur_x = ~0u; u8 row[8] = {};
  u32 cur_pal_hi = 0;
  auto load_tile = [&](u32 x) {
    const u32 map_addr = tilemap + ((x & 0xF8) >> 2) + ((x & widexmask) << 3);
    cur_tile = vm.read16(vv, map_addr);
    const u32 ty = (cur_tile & (1 << 11)) ? (7 - ty0) : ty0;
    if (c256) {
      const u32 a = tileset + ((cur_tile & 0x3FF) << 6) + (ty << 3);
      if (const u8* p = vv.direct(a, 8)) std::memcpy(row, p, 8); else for (int i = 0; i < 8; ++i) row[i] = vm.read8(vv, a + i);
      cur_pal_hi = cur_tile >> 12;
    } else {
      const u32 a = tileset + ((cur_tile & 0x3FF) << 5) + (ty << 2);
      u8 packed[4];
      if (const u8* p = vv.direct(a, 4)) std::memcpy(packed, p, 4); else for (int i = 0; i < 4; ++i) packed[i] = vm.read8(vv, a + i);
      for (int i = 0; i < 4; ++i) { row[i * 2] = packed[i] & 0xF; row[i * 2 + 1] = packed[i] >> 4; }
      cur_pal_hi = (cur_tile & 0xF000) >> 8;
    }
    if (cur_tile & (1 << 10)) { for (int i = 0; i < 4; ++i) { const u8 t = row[i]; row[i] = row[7 - i]; row[7 - i] = t; } }
  };
  for (u32 i = 0; i < 256; ++i) {
    const u32 x = xoff + i - (i % mw);
    if ((x >> 3) != cur_x) { cur_x = x >> 3; load_tile(x); }
    const u8 idx = row[x & 7];
    if (!idx) continue;
    plane.v()[i] = static_cast<u16>(LV_OPAQUE | (c256 ? ((extpal ? cur_pal_hi << 8 : 0) | idx) : (cur_pal_hi | idx)));
    plane.any = true;
  }
}

// Affine (rotation/scaling) background: 8-bit map entries, 256-colour tiles.
void Engine2D::draw_bg_affine(u32 line, int bg) {
  prof::add(prof::C_2D_BG_AFFINE, 1);
  (void)line;
  const u16 cnt = cur_.bgcnt[bg];
  Layer& plane = bg_[bg];
  const VramView& vv = bg_vram();
  const VramMap& vm = vram();
  std::memset(plane.v(), 0, 512); plane.any = false; plane.table = std_pal18();
  static const u32 coord_masks[4] = {0x07800, 0x0F800, 0x1F800, 0x3F800};
  const u32 coordmask = coord_masks[(cnt >> 14) & 3], yshift = 7 + ((cnt >> 14) & 3) - 3;
  const u32 overflow = (cnt & (1 << 13)) ? 0 : ~(coordmask | 0x7FF);
  u32 tileset = (cnt & 0x003C) << 12, tilemap = (cnt & 0x1F00) << 3;
  if (!num_) { tileset += (cur_.dispcnt & 0x07000000) >> 8; tilemap += (cur_.dispcnt & 0x38000000) >> 11; }
  const s32 dx = cur_.pa[bg - 2], dy = cur_.pc[bg - 2];
  s32 rx = cur_.ref_x[bg - 2], ry = cur_.ref_y[bg - 2];
  const bool mosaic = (cnt & (1 << 6)) && cur_.bg_mosaic_w > 0;
  const u32 mw = cur_.bg_mosaic_w + 1;
  if (!mosaic && dx == 0x100 && dy == 0) { tile_row_degenerate(plane, tilemap, tileset, coordmask, yshift, overflow == 0, false, false, bg, rx, ry); return; }

  u32 mosaic_phase = 0;
  s32 mosaic_rx = rx, mosaic_ry = ry;
  for (u32 i = 0; i < 256; ++i, rx += dx, ry += dy,
       mosaic_phase = mosaic ? ((mosaic_phase + 1 == mw) ? 0 : mosaic_phase + 1) : 0) {
    if (mosaic && mosaic_phase == 0) { mosaic_rx = rx; mosaic_ry = ry; }
    const s32 fx = mosaic ? mosaic_rx : rx, fy = mosaic ? mosaic_ry : ry;
    if ((fx | fy) & overflow) continue;
    const u32 tile = vram_fetch8(vm, vv, tilemap + (((fy & coordmask) >> 11) << yshift) + ((fx & coordmask) >> 11));
    const u8 idx = vram_fetch8(vm, vv, tileset + (tile << 6) + (((fy >> 8) & 7) << 3) + ((fx >> 8) & 7));
    if (!idx) continue;
    plane.v()[i] = LV_OPAQUE | idx; plane.any = true;
  }
}

// Extended background: 16-bit-map affine, 8-bit bitmap or direct-colour bitmap.
void Engine2D::draw_bg_extended(u32 line, int bg) {
  prof::add(prof::C_2D_BG_EXT, 1);
  (void)line;
  const u16 cnt = cur_.bgcnt[bg];
  Layer& plane = bg_[bg];
  const VramView& vv = bg_vram();
  const VramMap& vm = vram();
  std::memset(plane.v(), 0, 512); plane.any = false; plane.table = std_pal18();
  const s32 dx = cur_.pa[bg - 2], dy = cur_.pc[bg - 2];
  s32 rx = cur_.ref_x[bg - 2], ry = cur_.ref_y[bg - 2];
  const bool mosaic = (cnt & (1 << 6)) && cur_.bg_mosaic_w > 0;
  const u32 mw = cur_.bg_mosaic_w + 1;
  const bool extpal = cur_.dispcnt & (1u << 30);

  if (cnt & (1 << 7)) {
    static const u32 xm[4] = {0x07FFF, 0x0FFFF, 0x1FFFF, 0x1FFFF}, ym[4] = {0x07FFF, 0x0FFFF, 0x0FFFF, 0x1FFFF}, ys[4] = {7, 8, 9, 9};
    const u32 sz = (cnt >> 14) & 3, xmask = xm[sz], ymask = ym[sz], yshift = ys[sz];
    const u32 ofx = (cnt & (1 << 13)) ? 0 : ~xmask, ofy = (cnt & (1 << 13)) ? 0 : ~ymask;
    const u32 base = (cnt & 0x1F00) << 6;
    const bool direct = cnt & (1 << 2);
    if (direct) plane.table = kern::direct_table();
    if (!mosaic && dx == 0x100 && dy == 0) { bitmap_row_degenerate(plane, base, xmask, ymask, yshift, ofx == 0, direct, rx, ry); return; }
    u32 mosaic_phase = 0;
    s32 mosaic_rx = rx, mosaic_ry = ry;
    for (u32 i = 0; i < 256; ++i, rx += dx, ry += dy,
         mosaic_phase = mosaic ? ((mosaic_phase + 1 == mw) ? 0 : mosaic_phase + 1) : 0) {
      if (mosaic && mosaic_phase == 0) { mosaic_rx = rx; mosaic_ry = ry; }
      const s32 fx = mosaic ? mosaic_rx : rx, fy = mosaic ? mosaic_ry : ry;
      if ((fx & ofx) || (fy & ofy)) continue;
      const u32 off = (((fy & ymask) >> 8) << yshift) + ((fx & xmask) >> 8);
      if (direct) {
        const u16 c = vram_fetch16(vm, vv, base + (off << 1));
        if (!(c & 0x8000)) continue;
        plane.v()[i] = c;
      } else {
        const u8 idx = vram_fetch8(vm, vv, base + off);
        if (!idx) continue;
        plane.v()[i] = LV_OPAQUE | idx;
      }
      plane.any = true;
    }
  } else {
    static const u32 coord_masks[4] = {0x07800, 0x0F800, 0x1F800, 0x3F800};
    const u32 coordmask = coord_masks[(cnt >> 14) & 3], yshift = 7 + ((cnt >> 14) & 3) - 3;
    const u32 overflow = (cnt & (1 << 13)) ? 0 : ~(coordmask | 0x7FF);
    u32 tileset = (cnt & 0x003C) << 12, tilemap = (cnt & 0x1F00) << 3;
    if (!num_) { tileset += (cur_.dispcnt & 0x07000000) >> 8; tilemap += (cur_.dispcnt & 0x38000000) >> 11; }
    if (!mosaic && dx == 0x100 && dy == 0) { tile_row_degenerate(plane, tilemap, tileset, coordmask, yshift, overflow == 0, true, extpal, bg, rx, ry); if (extpal) plane.table = extpal18_.data() + bg * 4096; return; }
    u32 palmask = 0;
    u32 mosaic_phase = 0;
    s32 mosaic_rx = rx, mosaic_ry = ry;
    for (u32 i = 0; i < 256; ++i, rx += dx, ry += dy,
         mosaic_phase = mosaic ? ((mosaic_phase + 1 == mw) ? 0 : mosaic_phase + 1) : 0) {
      if (mosaic && mosaic_phase == 0) { mosaic_rx = rx; mosaic_ry = ry; }
      const s32 fx = mosaic ? mosaic_rx : rx, fy = mosaic ? mosaic_ry : ry;
      if ((fx | fy) & overflow) continue;
      const u16 tile = vram_fetch16(vm, vv, tilemap + ((((fy & coordmask) >> 11) << yshift) + ((fx & coordmask) >> 11)) * 2);
      u32 tx = (fx >> 8) & 7, ty = (fy >> 8) & 7;
      if (tile & (1 << 10)) tx = 7 - tx;
      if (tile & (1 << 11)) ty = 7 - ty;
      const u8 idx = vram_fetch8(vm, vv, tileset + ((tile & 0x3FF) << 6) + (ty << 3) + tx);
      if (!idx) continue;
      plane.v()[i] = static_cast<u16>(LV_OPAQUE | (extpal ? ((tile >> 12) << 8) : 0) | idx); plane.any = true;
      palmask |= 1u << (tile >> 12);
    }
    if (extpal) {
      for (u32 i = 0; i < 16; ++i) if (palmask & (1u << i)) ext_pal18(bg, i);
      plane.table = extpal18_.data() + bg * 4096;
    }
  }
}

// Mode 6: one large 8-bit bitmap on BG2.
void Engine2D::draw_bg_large(u32 line) {
  (void)line;
  const u16 cnt = cur_.bgcnt[2];
  Layer& plane = bg_[2];
  const VramView& vv = bg_vram();
  const VramMap& vm = vram();
  std::memset(plane.v(), 0, 512); plane.any = false; plane.table = std_pal18();
  static const u32 xm[4] = {0x1FFFF, 0x3FFFF, 0x1FFFF, 0x1FFFF}, ym[4] = {0x3FFFF, 0x1FFFF, 0x0FFFF, 0x1FFFF}, ys[4] = {9, 10, 9, 9};
  const u32 sz = (cnt >> 14) & 3, xmask = xm[sz], ymask = ym[sz], yshift = ys[sz];
  const u32 ofx = (cnt & (1 << 13)) ? 0 : ~xmask, ofy = (cnt & (1 << 13)) ? 0 : ~ymask;
  const s32 dx = cur_.pa[0], dy = cur_.pc[0];
  s32 rx = cur_.ref_x[0], ry = cur_.ref_y[0];
  const bool mosaic = (cnt & (1 << 6)) && cur_.bg_mosaic_w > 0;
  const u32 mw = cur_.bg_mosaic_w + 1;
  if (!mosaic && dx == 0x100 && dy == 0) { bitmap_row_degenerate(plane, 0, xmask, ymask, yshift, ofx == 0, false, rx, ry); return; }
  u32 mosaic_phase = 0;
  s32 mosaic_rx = rx, mosaic_ry = ry;
  for (u32 i = 0; i < 256; ++i, rx += dx, ry += dy,
       mosaic_phase = mosaic ? ((mosaic_phase + 1 == mw) ? 0 : mosaic_phase + 1) : 0) {
    if (mosaic && mosaic_phase == 0) { mosaic_rx = rx; mosaic_ry = ry; }
    const s32 fx = mosaic ? mosaic_rx : rx, fy = mosaic ? mosaic_ry : ry;
    if ((fx & ofx) || (fy & ofy)) continue;
    const u8 idx = vram_fetch8(vm, vv, (((fy & ymask) >> 8) << yshift) + ((fx & xmask) >> 8));
    if (!idx) continue;
    plane.v()[i] = LV_OPAQUE | idx; plane.any = true;
  }
}

// The per-pixel loops compute, for pixel i, fx = rx + i * pa and fy = ry +
// i * pc; with pa = 0x100 and pc = 0 the row is fixed and the texel column is
// (rx >> 8) + i, so the coordinate masks reduce to a wrap at the bitmap's
// width (or, with overflow transparent, to the span 0 <= column < width).
// Each run is read a block at a time through the view's direct pointers
// (an overlapping-bank block falls back to the OR read).
void Engine2D::bitmap_row_degenerate(Layer& plane, u32 base, u32 xmask, u32 ymask, u32 yshift, bool wrap, bool direct, s32 rx, s32 ry) {
  const VramView& vv = bg_vram();
  const VramMap& vm = vram();
  if (!wrap && (static_cast<u32>(ry) & ~ymask)) return;
  const u32 rowoff = ((static_cast<u32>(ry) & ymask) >> 8) << yshift;
  const u32 width = (xmask >> 8) + 1;
  const s32 x0 = rx >> 8;
  bool any = false;
  auto emit = [&](u32 texel, u32 i, u32 n) {
    // [i, i + n) of the line from texel offset `texel` of the row.
    u32 addr = base + (direct ? texel << 1 : texel);
    while (n) {
      const u32 a = addr & vv.addr_mask();
      const u32 in_block = (VramView::BLOCK - (a & (VramView::BLOCK - 1))) >> (direct ? 1 : 0);
      const u32 m = n < in_block ? n : in_block;
      if (const u8* p = vv.ptr[a / VramView::BLOCK]) {
        const u8* src = p + (a & (VramView::BLOCK - 1));
        any |= direct ? kern::active::bmp_row_16(reinterpret_cast<const u16*>(src), m, plane.v() + i) : kern::active::bmp_row_8(src, m, plane.v() + i);
      } else if (direct) {
        for (u32 k = 0; k < m; ++k) { const u16 c = vm.read16(vv, addr + k * 2); if (c & 0x8000) { plane.v()[i + k] = c; any = true; } }
      } else {
        for (u32 k = 0; k < m; ++k) { const u8 c = vm.read8(vv, addr + k); if (c) { plane.v()[i + k] = static_cast<u16>(LV_OPAQUE | c); any = true; } }
      }
      addr += direct ? m << 1 : m; i += m; n -= m;
    }
  };
  if (wrap) {
    u32 i = 0, x = static_cast<u32>(x0) & (width - 1);
    while (i < 256) { const u32 n = 256 - i < width - x ? 256 - i : width - x; emit(rowoff + x, i, n); i += n; x = 0; }
  } else {
    const s32 i0 = x0 < 0 ? -x0 : 0, i1 = static_cast<s32>(width) - x0 < 256 ? static_cast<s32>(width) - x0 : 256;
    if (i0 < i1 && i0 < 256) emit(rowoff + static_cast<u32>(x0 + i0), static_cast<u32>(i0), static_cast<u32>(i1 - i0));
  }
  plane.any = any;
}

// Tiled rotscale layers on the identity matrix: gathered like the text
// layer and run through the same kernel; a tile past a non-wrapping map's
// edge is a blank row.
void Engine2D::tile_row_degenerate(Layer& plane, u32 tilemap, u32 tileset, u32 coordmask, u32 yshift, bool wrap, bool map16, bool ext, int bg, s32 rx, s32 ry) {
  const VramView& vv = bg_vram();
  const VramMap& vm = vram();
  const u32 tiles = (coordmask >> 11) + 1, size = tiles << 3;   // per axis, in tiles and pixels
  if (!wrap && (static_cast<u32>(ry) & ~(coordmask | 0x7FF))) return;
  const u32 maprow = (static_cast<u32>(ry) & coordmask) >> 11 << yshift, tyrow = (static_cast<u32>(ry) >> 8) & 7;
  const s32 x0 = rx >> 8;
  const u32 shift = static_cast<u32>(x0) & 7;
  const s32 tpx0 = x0 - static_cast<s32>(shift);
  alignas(16) u8 rows[33 * 8]; u8 ctl[33];
  u32 palmask = 0;
  const u32 amask = vv.addr_mask();
  for (u32 t = 0; t < 33; ++t) {
    const s32 tpx = tpx0 + static_cast<s32>(t << 3);
    ctl[t] = 0;
    if (!wrap && (tpx < 0 || tpx >= static_cast<s32>(size))) { std::memset(rows + t * 8, 0, 8); continue; }
    const u32 tx = (static_cast<u32>(tpx) >> 3) & (tiles - 1);
    u32 tile, ty = tyrow;
    if (map16) {
      tile = vram_fetch16(vm, vv, tilemap + (maprow + tx) * 2);
      ctl[t] = static_cast<u8>((tile >> 12) | ((tile >> 6) & 0x10));
      if (tile & (1 << 11)) ty = 7 - ty;
      palmask |= 1u << (tile >> 12);
      tile &= 0x3FF;
    } else tile = vram_fetch8(vm, vv, tilemap + maprow + tx);
    const u32 a = tileset + (tile << 6) + (ty << 3), am = a & amask;
    if (const u8* p = vv.ptr[am / VramView::BLOCK]) std::memcpy(rows + t * 8, p + (am & (VramView::BLOCK - 1)), 8);
    else for (u32 i = 0; i < 8; ++i) rows[t * 8 + i] = vm.read8(vv, a + i);
  }
  if (ext) for (u32 i = 0; i < 16; ++i) if (palmask & (1u << i)) ext_pal18(static_cast<u32>(bg), i);
  plane.any = kern::active::text_row_256(rows, ctl, 33, ext, plane.v() - shift);
}

void Engine2D::draw_bg_3d() {
  prof::add(prof::C_2D_BG_3D, 1);
  Layer& plane = bg_[0];
  if (!line3d_) { plane.any = false; return; }
  // Report emptiness like other layers so an empty 3D line still gets the
  // fast paths in select_layers.
  plane.any = kern::active::layer16_3d(line3d_, plane.v());
  plane.table = line3d_;
  if (!plane.any) prof::add(prof::C_2D_BG_3D_EMPTY, 1);
}

// ---- sprites ----------------------------------------------------------------

inline void Engine2D::put_sprite_pixel(s32 x, u16 value, bool opaque, u8 attr, u8 alpha, bool window) {
  if (window) { if (opaque) obj_win_[x] = 1; return; }
  const u8 old = obj_attr_[x];
  if (opaque && (!(old & OA_OPAQUE) || (attr & OA_PRIO) < (old & OA_PRIO))) {
    obj_v_[x] = value; obj_attr_[x] = attr | OA_OPAQUE; obj_alpha_[x] = alpha;
    obj_prio_mask_ |= static_cast<u8>(1 << (attr & OA_PRIO));
  } else if (!opaque && !(old & OA_OPAQUE)) {
    // A transparent pixel still stamps its priority and mosaic flag.
    obj_attr_[x] = (old & ~(OA_MOSAIC | OA_PRIO)) | (attr & (OA_TOUCHED | OA_MOSAIC | OA_PRIO));
  }
}

void Engine2D::render_sprites(u32 line) {
  spr_ = regs_.capture();
  if (!spr_.enabled) return;      // the OBJ planes are left as they are
  num_sprites_ = 0; obj_prio_mask_ = 0;
  // Only the attribute plane needs clearing: obj_v_/obj_alpha_ readers are
  // gated on OA_OPAQUE.
  obj_attr_.fill(0); obj_win_.fill(0);
  if (!spr_.obj_enable) return;

  const u16* oam = regs_.oam();
  static const s32 widths[16]  = {8, 16, 8, 8, 16, 32, 8, 8, 32, 32, 16, 8, 64, 64, 32, 8};
  static const s32 heights[16] = {8, 8, 16, 8, 16, 8, 32, 8, 32, 16, 32, 8, 64, 32, 64, 8};

  if (oam_lists_gen_ != regs_.oam_geom_gen()) { oam_lists_gen_ = regs_.oam_geom_gen(); rebuild_sprite_lists(oam); }
  const LineSprites& ls = line_sprites_[line & 0xFF];
  for (u32 k = 0; k < ls.count; ++k) {
    const int n = ls.idx[k];
    const u16* attr = &oam[n * 4];
    const u32 type = (attr[0] >> 8) & 3;      // bit 8 rotscale, bit 9 double-size / disable
    const bool window = ((attr[0] >> 10) & 3) == 2;
    const u32 shape_size = (attr[0] >> 14) | ((attr[1] & 0xC000) >> 12);
    const s32 w = widths[shape_size], h = heights[shape_size];
    s32 bw = w, bh = h;
    if (type == 3) { bw <<= 1; bh <<= 1; }
    s32 y = attr[0] & 0xFF;
    if (((line - y) & 0xFF) >= static_cast<u32>(bh)) continue;
    const s32 x = static_cast<s32>(static_cast<u32>(attr[1]) << 23) >> 23;
    if (x <= -bw) continue;
    if ((attr[0] & (1 << 12)) && !window) {
      // Mosaic: the line used is the one latched at the top of the mosaic row.
      y = (spr_.obj_mosaic_line - y) & 0xFF;
      if (y >= bh) y = 0;
    } else y = (line - y) & 0xFF;
    if (type & 1) draw_sprite_rotscale(attr, oam, bw, bh, w, h, x, y, window);
    else draw_sprite_normal(attr, w, h, x, y, window);
    ++num_sprites_;
  }
}

// Candidate lists: every enabled sprite on every line of its box, in OAM
// order; render_sprites still does the exact per-line test.
void Engine2D::rebuild_sprite_lists(const u16* oam) {
  static const s32 heights[16] = {8, 8, 16, 8, 16, 8, 32, 8, 32, 16, 32, 8, 64, 32, 64, 8};
  for (auto& l : line_sprites_) l.count = 0;
  for (int n = 0; n < 128; ++n) {
    const u16* attr = &oam[n * 4];
    const u32 type = (attr[0] >> 8) & 3;
    if (type == 2) continue;
    const u32 shape_size = (attr[0] >> 14) | ((attr[1] & 0xC000) >> 12);
    s32 bh = heights[shape_size];
    if (type == 3) bh <<= 1;
    const u32 y = attr[0] & 0xFF;
    for (s32 i = 0; i < bh; ++i) { LineSprites& l = line_sprites_[(y + i) & 0xFF]; l.idx[l.count++] = static_cast<u8>(n); }
  }
}

void Engine2D::draw_sprite_normal(const u16* attr, int w, int h, s32 x, s32 y, bool window) {
  const VramView& vv = obj_vram();
  const VramMap& vm = vram();
  const u32 tile = attr[2] & 0x3FF;
  const u32 mode = window ? 0 : ((attr[0] >> 10) & 3);
  u8 a = static_cast<u8>((attr[2] >> 10) & 3) | OA_TOUCHED;
  if ((attr[0] & (1 << 12)) && !window) a |= OA_MOSAIC;
  if (attr[1] & (1 << 13)) y = h - 1 - y;                   // vertical flip
  const bool hflip = attr[1] & (1 << 12);
  u32 xoff = 0, xend = w;
  if (x >= 0) { if (x + xend > 256) xend = 256 - x; } else { xoff = -x; x = 0; }

  if (mode == 3) {
    // Bitmap sprite: direct colour, alpha in the palette field.
    const u32 alpha = attr[2] >> 12;
    if (!alpha) return;
    a |= OA_BITMAP;
    u32 addr;
    if (spr_.dispcnt & 0x40) {
      if (spr_.dispcnt & 0x20) return;                           // reserved mapping: draws nothing
      addr = (tile << (7 + ((spr_.dispcnt >> 22) & 1))) + y * w * 2;
    } else if (spr_.dispcnt & 0x20) addr = ((tile & 0x1F) << 4) + ((tile & 0x3E0) << 7) + y * 256 * 2;
    else addr = ((tile & 0x0F) << 4) + ((tile & 0x3F0) << 7) + y * 128 * 2;
    const u8* row = vv.direct(addr, w * 2);
    alignas(16) u16 col[64 + 16];
    for (u32 i = xoff; i < xend; ++i) {
      const u32 sx = hflip ? (w - 1 - i) : i;
      if (row) std::memcpy(col + (i - xoff), row + sx * 2, 2); else col[i - xoff] = vm.read16(vv, addr + sx * 2);
    }
    if (window) { for (u32 i = 0; i < xend - xoff; ++i) if (col[i] & 0x8000) obj_win_[x + i] = 1; return; }
    kern::active::obj_row_bmp16(col, xend - xoff, a, static_cast<u8>(alpha + 1), obj_v_.data() + x, obj_attr_.data() + x, obj_alpha_.data() + x);
    obj_prio_mask_ |= static_cast<u8>(1 << (a & OA_PRIO));
    return;
  }

  if (mode == 1) a |= OA_SEMI;
  const bool c256 = attr[0] & (1 << 13);
  u32 base = tile;
  u32 row_stride;                                              // bytes between tile rows
  if (spr_.dispcnt & (1 << 4)) { base <<= (spr_.dispcnt >> 20) & 3; row_stride = (w >> 3) << (c256 ? 6 : 5); }
  else row_stride = 0x400;
  base = (base << 5) + (y >> 3) * row_stride;
  // Decoded tile by tile into indices first, then placed per pixel.
  u8 idx[64];
  u16 pal_base = 0;
  if (c256) {
    base += (y & 7) << 3;
    if (spr_.dispcnt & (1u << 31)) pal_base = static_cast<u16>((attr[2] & 0xF000) >> 4); else a |= OA_STDPAL;   // ext palette number | index, or the standard palette
    for (u32 t = 0; t < static_cast<u32>(w >> 3); ++t) {
      const u32 addr = base + t * 64;
      if (const u8* p = vv.direct(addr, 8)) std::memcpy(idx + t * 8, p, 8);
      else for (u32 i = 0; i < 8; ++i) idx[t * 8 + i] = vm.read8(vv, addr + i);
    }
  } else {
    base += (y & 7) << 2;
    pal_base = static_cast<u16>((attr[2] & 0xF000) >> 8); a |= OA_STDPAL;
    for (u32 t = 0; t < static_cast<u32>(w >> 3); ++t) {
      const u32 addr = base + t * 32;
      u8 packed[4];
      if (const u8* p = vv.direct(addr, 4)) std::memcpy(packed, p, 4);
      else for (u32 i = 0; i < 4; ++i) packed[i] = vm.read8(vv, addr + i);
      for (u32 i = 0; i < 4; ++i) { idx[t * 8 + i * 2] = packed[i] & 0xF; idx[t * 8 + i * 2 + 1] = packed[i] >> 4; }
    }
  }
  alignas(16) u8 row[64 + 16];
  for (u32 i = xoff; i < xend; ++i) row[i - xoff] = idx[hflip ? (w - 1 - i) : i];
  if (window) { for (u32 i = 0; i < xend - xoff; ++i) if (row[i]) obj_win_[x + i] = 1; return; }
  kern::active::obj_row_idx16(row, xend - xoff, pal_base, a, obj_v_.data() + x, obj_attr_.data() + x, obj_alpha_.data() + x);
  obj_prio_mask_ |= static_cast<u8>(1 << (a & OA_PRIO));
}

void Engine2D::draw_sprite_rotscale(const u16* attr, const u16* oam, int bw, int bh, int w, int h, s32 x, s32 y, bool window) {
  const VramView& vv = obj_vram();
  const VramMap& vm = vram();
  const u16* rot = &oam[(((attr[1] >> 9) & 0x1F) * 16) + 3];
  const s16 pa = static_cast<s16>(rot[0]), pb = static_cast<s16>(rot[4]), pc = static_cast<s16>(rot[8]), pd = static_cast<s16>(rot[12]);
  const u32 tile = attr[2] & 0x3FF;
  const u32 mode = window ? 0 : ((attr[0] >> 10) & 3);
  u8 a = static_cast<u8>((attr[2] >> 10) & 3) | OA_TOUCHED;
  if ((attr[0] & (1 << 12)) && !window) a |= OA_MOSAIC;
  const s32 cx = bw >> 1, cy = bh >> 1;
  u32 xoff = 0;
  if (x >= 0) { if (x + bw > 256) bw = 256 - x; } else { xoff = -x; x = 0; }
  s32 rx = (static_cast<s32>(xoff) - cx) * pa + (y - cy) * pb + (w << 7);
  s32 ry = (static_cast<s32>(xoff) - cx) * pc + (y - cy) * pd + (h << 7);
  const u32 fw = w << 8, fh = h << 8;

  if (mode == 3) {
    const u32 alpha = attr[2] >> 12;
    if (!alpha) return;
    a |= OA_BITMAP;
    u32 addr, stride;
    if (spr_.dispcnt & 0x40) {
      if (spr_.dispcnt & 0x20) return;
      addr = tile << (7 + ((spr_.dispcnt >> 22) & 1)); stride = w * 2;
    } else if (spr_.dispcnt & 0x20) { addr = ((tile & 0x1F) << 4) + ((tile & 0x3E0) << 7); stride = 256 * 2; }
    else { addr = ((tile & 0x0F) << 4) + ((tile & 0x3F0) << 7); stride = 128 * 2; }
    for (; xoff < static_cast<u32>(bw); ++xoff, ++x, rx += pa, ry += pc) {
      if (static_cast<u32>(rx) >= fw || static_cast<u32>(ry) >= fh) continue;
      const u16 c = vram_fetch16(vm, vv, addr + (ry >> 8) * stride + ((rx >> 8) << 1));
      put_sprite_pixel(x, c, c & 0x8000, a, static_cast<u8>(alpha + 1), window);
    }
    return;
  }

  if (mode == 1) a |= OA_SEMI;
  const bool c256 = attr[0] & (1 << 13);
  u32 base = tile, row_stride;
  if (spr_.dispcnt & (1 << 4)) { base <<= (spr_.dispcnt >> 20) & 3; row_stride = (w >> 3) << (c256 ? 6 : 5); }
  else row_stride = 0x400;
  base <<= 5;
  if (c256) {
    u16 pal_base = 0;
    if (spr_.dispcnt & (1u << 31)) pal_base = static_cast<u16>((attr[2] & 0xF000) >> 4); else a |= OA_STDPAL;
    for (; xoff < static_cast<u32>(bw); ++xoff, ++x, rx += pa, ry += pc) {
      if (static_cast<u32>(rx) >= fw || static_cast<u32>(ry) >= fh) continue;
      const u8 idx = vram_fetch8(vm, vv, base + (ry >> 11) * row_stride + ((ry & 0x700) >> 5) + (rx >> 11) * 64 + ((rx & 0x700) >> 8));
      put_sprite_pixel(x, static_cast<u16>(LV_OPAQUE | pal_base | idx), idx != 0, a, 0, window);
    }
  } else {
    const u16 pal_base = static_cast<u16>((attr[2] & 0xF000) >> 8); a |= OA_STDPAL;
    for (; xoff < static_cast<u32>(bw); ++xoff, ++x, rx += pa, ry += pc) {
      if (static_cast<u32>(rx) >= fw || static_cast<u32>(ry) >= fh) continue;
      u8 idx = vram_fetch8(vm, vv, base + (ry >> 11) * row_stride + ((ry & 0x700) >> 6) + (rx >> 11) * 32 + ((rx & 0x700) >> 9));
      idx = (rx & 0x100) ? (idx >> 4) : (idx & 0xF);
      put_sprite_pixel(x, static_cast<u16>(LV_OPAQUE | pal_base | idx), idx != 0, a, 0, window);
    }
  }
}

// Sprite X mosaic runs over the finished OBJ plane, left to right: re-latch
// at each mosaic cell, on a mosaic-ness change, or a higher-priority pixel.
void Engine2D::apply_sprite_mosaic_x() {
  const u32 mw = cur_.obj_mosaic_w;
  if (!mw) return;
  u32 mx = 0; u16 lpx = 0; u8 lattr = 0, lalpha = 0;
  for (u32 i = 0; i < 256; ++i) {
    const u8 cur = obj_attr_[i];
    bool latch = mx == 0 || !(cur & OA_MOSAIC) || !(lattr & OA_MOSAIC) || (cur & OA_PRIO) < (lattr & OA_PRIO);
    if (latch) { lpx = obj_v_[i]; lattr = cur; lalpha = obj_alpha_[i]; }
    obj_v_[i] = lpx; obj_attr_[i] = lattr; obj_alpha_[i] = lalpha;
    mx = (mx == mw) ? 0 : mx + 1;
  }
}

// ---- compositing ------------------------------------------------------------

void Engine2D::build_window_plane() {
  if (!(cur_.dispcnt & 0xE000)) { win_.fill(0xFF); return; }
  win_.fill(cur_.wincnt[2]);                                     // outside all windows
  if (cur_.dispcnt & (1 << 15)) for (u32 i = 0; i < 256; ++i) if (obj_win_[i]) win_[i] = cur_.wincnt[3];
  // Same edge rule as the vertical windows: x2 < x1 wraps, x1 == x2 covers
  // nothing. A line is at most three runs, carrying the x state in from
  // the previous line before the first edge (Engine2DRegs::begin_line
  // carries it on).
  auto span = [&](u8 x1, u8 x2, u8 active, u8 val) {
    const u32 e1 = x1 < x2 ? x1 : x2, e2 = x1 < x2 ? x2 : x1;
    const bool st[3] = {(active & 2) != 0, x1 < x2, x1 > x2};
    const u32 edge[4] = {0, e1, e2, 256};
    if (active & 1)
      for (int k = 0; k < 3; ++k) if (st[k] && edge[k + 1] > edge[k]) std::memset(win_.data() + edge[k], val, edge[k + 1] - edge[k]);
  };
  if (cur_.dispcnt & (1 << 14)) span(cur_.win1[0], cur_.win1[1], cur_.win1_active, cur_.wincnt[1]);
  if (cur_.dispcnt & (1 << 13)) span(cur_.win0[0], cur_.win0[1], cur_.win0_active, cur_.wincnt[0]);
}

// OBJ palettes as 18-bit records: the standard one (palette RAM + 0x200) and
// the extended one (8 KB of VRAM), reconverted when the bytes changed.
const Pixel* Engine2D::obj_std_pal18() {
  if (objpal18_gen_ != regs_.pal_gen()) {
    objpal18_gen_ = regs_.pal_gen();
    kern::active::palette_to_18(palette() + 0x100, objpal18_.data(), 256);
  }
  return objpal18_.data();
}
void Engine2D::obj_ext_pal18(u32 pal) {
  if (objext_checked_ & (1u << pal)) return;
  objext_checked_ |= 1u << pal;
  const VramView& v = num_ ? vram().bobj_extpal : vram().aobj_extpal;
  const u32 addr = pal * 0x200;
  u16* copy = objext_copy_.data() + pal * 256;
  alignas(16) u16 tmp[256];
  const u16* src = reinterpret_cast<const u16*>(v.direct(addr, 512));
  if (!src) { for (u32 i = 0; i < 256; ++i) tmp[i] = vram().read16(v, addr + i * 2); src = tmp; }
  if (!(objext_have_ & (1u << pal)) || cmp_differs(src, copy, 512, prof::C_2D_CMP_OBJEXT, prof::C_2D_CMPD_OBJEXT)) {
    std::memcpy(copy, src, 512);
    kern::active::palette_to_18(copy, objext18_.data() + pal * 256, 256);
    objext_have_ |= 1u << pal;
  }
}

// The resolve tables for this line, by TableId. The OBJ palettes are only
// validated when an opaque sprite pixel on the line uses them.
void Engine2D::setup_tables() {
  for (int n = 0; n < 4; ++n) tables_[n] = bg_[n].table;
  tables_[T_BACKDROP] = std_pal18();
  tables_[T_NONE] = zero_table_;
  tables_[T_OBJ_DIRECT] = kern::direct_table();
  tables_[T_OBJ_STD] = tables_[T_OBJ_EXT] = tables_[T_BACKDROP];
  if ((cur_.layer_enable & 0x10) && num_sprites_) {
    // Which OBJ palettes the line's opaque paletted sprite pixels use, eight
    // attribute bytes at a time.
    bool any_std = false, any_ext = false;
    {
      constexpr u64 OP = 0x8080808080808080ull, BM = 0x0808080808080808ull, SP = 0x4040404040404040ull;
      u64 std_acc = 0, ext_acc = 0;
      for (u32 i = 0; i < 256; i += 8) {
        u64 v;
        std::memcpy(&v, &obj_attr_[i], 8);
        const u64 paletted = v & OP & ~((v & BM) << 4);   // opaque and not a bitmap sprite
        const u64 stdpal = (v & SP) << 1;
        std_acc |= paletted & stdpal;
        ext_acc |= paletted & ~stdpal;
      }
      any_std = std_acc != 0;
      any_ext = ext_acc != 0;
    }
    if (any_std) tables_[T_OBJ_STD] = obj_std_pal18();
    if (any_ext) {
      u32 extmask = 0;
      for (u32 i = 0; i < 256; ++i) { const u8 a = obj_attr_[i]; if ((a & (OA_OPAQUE | OA_BITMAP | OA_STDPAL)) == OA_OPAQUE) extmask |= 1u << ((obj_v_[i] >> 8) & 0xF); }
      for (u32 pal = 0; pal < 16; ++pal) if (extmask & (1u << pal)) obj_ext_pal18(pal);
      tables_[T_OBJ_EXT] = objext18_.data();
    }
  }
}

// Top and second records for the composite: colours through the tables,
// layer ids as BLDCNT masks, the kind and alpha of the winning pixel. The
// per-pixel derivations are kernels; the two palette gathers stay scalar.
void Engine2D::select_layers() {
  setup_tables();
  top16_.fill(LV_OPAQUE); top_tid_.fill(T_BACKDROP);
  second16_.fill(0); second_tid_.fill(T_NONE);   // nothing beneath: never a blend target
  const bool objs = (cur_.layer_enable & 0x10) && num_sprites_;
  const bool no_windows = !(cur_.dispcnt & 0xE000);
  // Lowest priority first; within a priority BG3..BG0 then OBJ, later wins.
  for (int prio = 3; prio >= 0; --prio) {
    for (int bg = 3; bg >= 0; --bg) {
      if (!(cur_.layer_enable & (1 << bg))) continue;
      if ((cur_.bgcnt[bg] & 3) != prio) continue;
      const Layer& p = bg_[bg];
      if (!p.any) continue;
      prof::add(prof::C_2D_SELECTS, 1);
      if (no_windows) kern::active::select16_nowin(p.v(), static_cast<u8>(bg), top16_.data(), top_tid_.data(), second16_.data(), second_tid_.data());
      else kern::active::select16(p.v(), win_.data(), 1 << bg, static_cast<u8>(bg), top16_.data(), top_tid_.data(), second16_.data(), second_tid_.data());
    }
    if (objs && (obj_prio_mask_ & (1 << prio))) {
      if (no_windows) kern::active::select16_obj_nowin(obj_v_.data(), obj_attr_.data(), prio, top16_.data(), top_tid_.data(), second16_.data(), second_tid_.data());
      else kern::active::select16_obj(obj_v_.data(), obj_attr_.data(), win_.data(), prio, top16_.data(), top_tid_.data(), second16_.data(), second_tid_.data());
    }
  }
  resolve_full();
}

// Whether the composite can read the second target at all on this line. Only
// the three blending paths do: a blend effect, the 3D layer over something,
// and semi-transparent or bitmap sprites. A fade (brighten/darken) over plain
// layers never looks below the top pixel, so nothing under it need be
// resolved.
bool Engine2D::needs_second() const {
  if (((cur_.bldcnt >> 6) & 3) == 1) return true;
  if (!num_ && (cur_.dispcnt & 8) && bg_[0].any && kern::active::line_has_translucent_3d(line3d_)) return true;
  if ((cur_.layer_enable & 0x10) && num_sprites_) {
    u64 acc = 0;
    for (u32 i = 0; i < 256; i += 8) { u64 v; std::memcpy(&v, &obj_attr_[i], 8); acc |= v; }
    if (acc & 0x0C0C0C0C0C0C0C0Cull) return true;   // OA_SEMI | OA_BITMAP
  }
  return false;
}

void Engine2D::resolve_full() {
  const bool is3d = !num_ && (cur_.dispcnt & 8);
  kern::active::resolve16_full(top16_.data(), top_tid_.data(), second16_.data(), second_tid_.data(), tables_, obj_attr_.data(), obj_alpha_.data(),
                               is3d ? line3d_ : nullptr, top_.data(), second_.data(), top_id_.data(), top_kind_.data(), top_alpha_.data(), second_id_.data());
}

// Every pixel of the line opaque (bit 15 set): four vectors' worth of AND.
bool Engine2D::line_all_opaque(const Layer& p) {
  const u16* v = p.v();
  u64 acc = ~0ull;
  for (u32 i = 0; i < 256; i += 4) { u64 w; std::memcpy(&w, v + i, 8); acc &= w; }
  return (acc & 0x8000800080008000ull) == 0x8000800080008000ull;
}

// The select without the second layer: priority order as select_layers, but
// only the winner is kept. Shared by the flat path (which resolves straight
// to the output) and the fade path (which still needs the layer ids).
void Engine2D::select_top_only() {
  setup_tables();
  top16_.fill(LV_OPAQUE); top_tid_.fill(T_BACKDROP);
  const bool objs = (cur_.layer_enable & 0x10) && num_sprites_;
  const bool no_windows = !(cur_.dispcnt & 0xE000);
  for (int prio = 3; prio >= 0; --prio) {
    for (int bg = 3; bg >= 0; --bg) {
      if (!(cur_.layer_enable & (1 << bg))) continue;
      if ((cur_.bgcnt[bg] & 3) != prio) continue;
      const Layer& p = bg_[bg];
      if (!p.any) continue;
      prof::add(prof::C_2D_SELECTS, 1);
      if (no_windows) kern::active::select16_flat_nowin(p.v(), static_cast<u8>(bg), top16_.data(), top_tid_.data());
      else kern::active::select16_flat(p.v(), win_.data(), 1 << bg, static_cast<u8>(bg), top16_.data(), top_tid_.data());
    }
    if (objs && (obj_prio_mask_ & (1 << prio))) {
      if (no_windows) kern::active::select16_obj_flat_nowin(obj_v_.data(), obj_attr_.data(), prio, top16_.data(), top_tid_.data());
      else kern::active::select16_obj_flat(obj_v_.data(), obj_attr_.data(), win_.data(), prio, top16_.data(), top_tid_.data());
    }
  }
}

void Engine2D::select_layers_flat() {
  select_top_only();
  kern::active::resolve16(top16_.data(), top_tid_.data(), tables_, out_.data());
}

void Engine2D::select_layers_top() {
  select_top_only();
  const bool is3d = !num_ && (cur_.dispcnt & 8);
  kern::active::resolve16_top(top16_.data(), top_tid_.data(), tables_, is3d ? line3d_ : nullptr, top_.data(), top_id_.data());
}

void Engine2D::colour_effects() {
  kern::active::composite_line(cur_.bldcnt, cur_.eva, cur_.evb, cur_.evy, top_.data(), second_.data(), top_id_.data(), top_kind_.data(),
                               top_alpha_.data(), second_id_.data(), win_.data(), out_.data());
}


template <class S> void Engine2D::sync_state(S& s) {
  s.begin(num_ == 0 ? "ENGA" : "ENGB");
  regs_.sync_fields(s);
  // Sprites are rendered one line ahead: at the frame boundary the planes
  // hold line 0's, drawn during line 262.
  s.fields(obj_v_, obj_attr_, obj_alpha_, obj_win_, obj_prio_mask_, num_sprites_);
  s.end();
  if constexpr (S::reading) {
    // Every derived table revalidates against a generation it cannot match.
    regs_.after_load();
    pal18_gen_ = objpal18_gen_ = 0; extpal_checked_ = extpal_have_ = 0; objext_checked_ = objext_have_ = 0;
    oam_lists_gen_ = 0;
  }
}
template void Engine2D::sync_state<state::Writer>(state::Writer&);
template void Engine2D::sync_state<state::Reader>(state::Reader&);

} // namespace ds::gpu
