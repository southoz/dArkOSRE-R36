// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/gpu/gpu3d.h"
#include "core/div64.h"
#include "core/host_cores.h"
#include "core/state/state.h"
#include "core/nds.h"
#include "core/profile.h"
#if DSPERATE_NEON
#include <arm_neon.h>
#include "core/gpu/neon_compat.h"
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ds::gpu {

#if DSPERATE_NEON
// The A64-only NEON intrinsics the kernels use, in both spellings.
namespace compat = kern::compat;
#endif

namespace {


// ---- census (DS_CENSUS_GX=1) -----------------------------------------------
// Measurement only: hashes the polygon+vertex list to see whether a swap's
// geometry differs from what was last rasterised. Hashes the vertex records
// Polygon::vtx points at, not the (bank-biased) indices themselves.
bool census_gx() { static const bool on = std::getenv("DS_CENSUS_GX") != nullptr; return on; }

inline void fnv(u64& h, u64 v) { h = (h ^ v) * 0x100000001B3ull; }

u64 census_list_hash(const Polygon* const* polys, u32 n, const Vertex* vram) {
  u64 h = 0xCBF29CE484222325ull;
  fnv(h, n);
  for (u32 i = 0; i < n; ++i) {
    const Polygon& p = *polys[i];
    fnv(h, p.nverts); fnv(h, p.attr); fnv(h, p.texparam); fnv(h, p.texpal);
    fnv(h, p.wbuffer); fnv(h, p.degenerate); fnv(h, p.facing); fnv(h, p.translucent);
    fnv(h, p.shadow_mask); fnv(h, p.shadow);
    fnv(h, p.vtop); fnv(h, p.vbot);
    fnv(h, static_cast<u32>(p.ytop)); fnv(h, static_cast<u32>(p.ybot));
    fnv(h, static_cast<u32>(p.xtop)); fnv(h, static_cast<u32>(p.xbot));
    fnv(h, p.sort_key);
    for (u32 v = 0; v < p.nverts && v < 10; ++v) {
      fnv(h, static_cast<u32>(p.z[v])); fnv(h, static_cast<u32>(p.w[v]));
      const Vertex& vt = vram[p.vtx[v]];
      for (int c = 0; c < 4; ++c) fnv(h, static_cast<u32>(vt.pos[c]));
      for (int c = 0; c < 3; ++c) fnv(h, static_cast<u32>(vt.col[c]));
      for (int c = 0; c < 3; ++c) fnv(h, static_cast<u32>(vt.fcol[c]));
      fnv(h, static_cast<u16>(vt.tex[0])); fnv(h, static_cast<u16>(vt.tex[1]));
      fnv(h, vt.clipped);
      fnv(h, static_cast<u32>(vt.sx)); fnv(h, static_cast<u32>(vt.sy));
    }
  }
  return h;
}

// Whether the new list matches the previous bank's (the last frame is then
// reused). Compares live fields only: vtx/z/w tails beyond nverts are stale,
// and vtx indices are bank-biased.
bool lists_equal(const Polygon* a, const Polygon* b, u32 npoly, u32 abase, u32 bbase, const Vertex* vram) {
  for (u32 i = 0; i < npoly; ++i) {
    const Polygon& p = a[i]; const Polygon& q = b[i];
    if (p.nverts != q.nverts) return false;
    if (std::memcmp(&p.attr, &q.attr, 12) != 0) return false;          // attr, texparam, texpal
    if (p.wbuffer != q.wbuffer) return false;
    if (std::memcmp(&p.degenerate, &q.degenerate, 5) != 0) return false;
    if (std::memcmp(&p.vtop, &q.vtop, 28) != 0) return false;          // vtop..sort_key
    const u32 nv = p.nverts <= 10 ? p.nverts : 10;
    if (std::memcmp(p.z, q.z, nv * 4) != 0) return false;
    if (std::memcmp(p.w, q.w, nv * 4) != 0) return false;
    for (u32 v = 0; v < nv; ++v) {
      if (p.vtx[v] - abase != q.vtx[v] - bbase) return false;
      const Vertex& s0 = vram[p.vtx[v]]; const Vertex& t0 = vram[q.vtx[v]];
      if (std::memcmp(s0.pos, t0.pos, 16) != 0) return false;
      if (std::memcmp(s0.col, t0.col, 12) != 0) return false;
      if (std::memcmp(s0.tex, t0.tex, 4) != 0) return false;
      if (s0.clipped != t0.clipped) return false;
      if (std::memcmp(&s0.sx, &t0.sx, 8) != 0) return false;
      if (std::memcmp(s0.fcol, t0.fcol, 12) != 0) return false;
    }
  }
  return true;
}

struct CmpModel { u64 early, full; };

// Measurement only: models a straight compare's cost (bytes to first
// mismatch vs. bytes in full) against hashing. Same field walk as lists_equal.
CmpModel census_compare(const Polygon* a, const Polygon* b, u32 npoly, u32 abase, u32 bbase,
                        const Vertex* va, const Vertex* vb) {
  CmpModel m{0, 0};
  bool done = false;
  auto eq = [&](const void* x, const void* y, u32 n) {
    m.full += n;
    if (done) return;
    m.early += n;
    if (std::memcmp(x, y, n) != 0) done = true;
  };
  for (u32 i = 0; i < npoly; ++i) {
    const Polygon& p = a[i]; const Polygon& q = b[i];
    eq(&p.nverts, &q.nverts, 4);
    eq(&p.attr, &q.attr, 12);                       // attr, texparam, texpal
    eq(&p.wbuffer, &q.wbuffer, 1);
    eq(&p.degenerate, &q.degenerate, 5);            // the five flag bytes
    eq(&p.vtop, &q.vtop, 28);                       // vtop..sort_key, contiguous
    const u32 nv = p.nverts == q.nverts && p.nverts <= 10 ? p.nverts : 0;
    eq(p.z, q.z, nv * 4);
    eq(p.w, q.w, nv * 4);
    for (u32 v = 0; v < nv; ++v) {
      m.full += 2;
      if (!done) { m.early += 2; if (p.vtx[v] - abase != q.vtx[v] - bbase) done = true; }
      const Vertex& s0 = va[p.vtx[v]]; const Vertex& t0 = vb[q.vtx[v]];
      eq(s0.pos, t0.pos, 16);
      eq(s0.col, t0.col, 12);
      eq(s0.tex, t0.tex, 4);
      eq(&s0.clipped, &t0.clipped, 1);
      eq(&s0.sx, &t0.sx, 8);                        // sx, sy contiguous
      eq(s0.fcol, t0.fcol, 12);
    }
  }
  return m;
}

inline void mtx_identity(s32* m) {
  for (int i = 0; i < 16; ++i) m[i] = 0;
  m[0] = m[5] = m[10] = m[15] = 0x1000;
}
inline void mtx_load_4x3(s32* m, const s32* s) {
  m[0] = s[0]; m[1] = s[1]; m[2] = s[2]; m[3] = 0;
  m[4] = s[3]; m[5] = s[4]; m[6] = s[5]; m[7] = 0;
  m[8] = s[6]; m[9] = s[7]; m[10] = s[8]; m[11] = 0;
  m[12] = s[9]; m[13] = s[10]; m[14] = s[11]; m[15] = 0x1000;
}
// m = s * m, with s a 4x4 / 4x3 (implicit last column 0,0,0,1) / 3x3 matrix.
inline void mtx_mult_4x4(s32* m, const s32* s) {
#if DSPERATE_NEON
  // Same 64-bit products/sums/shift as the scalar form, two columns per vector.
  const int32x2_t t0l = vld1_s32(m), t0h = vld1_s32(m + 2), t1l = vld1_s32(m + 4), t1h = vld1_s32(m + 6);
  const int32x2_t t2l = vld1_s32(m + 8), t2h = vld1_s32(m + 10), t3l = vld1_s32(m + 12), t3h = vld1_s32(m + 14);
  for (int r = 0; r < 4; ++r) {
    const int32x4_t sr = vld1q_s32(s + r * 4);
    int64x2_t lo = vmull_lane_s32(t0l, vget_low_s32(sr), 0), hi = vmull_lane_s32(t0h, vget_low_s32(sr), 0);
    lo = vmlal_lane_s32(lo, t1l, vget_low_s32(sr), 1);  hi = vmlal_lane_s32(hi, t1h, vget_low_s32(sr), 1);
    lo = vmlal_lane_s32(lo, t2l, vget_high_s32(sr), 0); hi = vmlal_lane_s32(hi, t2h, vget_high_s32(sr), 0);
    lo = vmlal_lane_s32(lo, t3l, vget_high_s32(sr), 1); hi = vmlal_lane_s32(hi, t3h, vget_high_s32(sr), 1);
    vst1_s32(m + r * 4, vmovn_s64(vshrq_n_s64(lo, 12)));
    vst1_s32(m + r * 4 + 2, vmovn_s64(vshrq_n_s64(hi, 12)));
  }
#else
  s32 t[16]; std::memcpy(t, m, sizeof t);
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c)
      m[r * 4 + c] = static_cast<s32>((static_cast<s64>(s[r * 4]) * t[c] + static_cast<s64>(s[r * 4 + 1]) * t[4 + c] +
                                       static_cast<s64>(s[r * 4 + 2]) * t[8 + c] + static_cast<s64>(s[r * 4 + 3]) * t[12 + c]) >> 12);
#endif
}
inline void mtx_mult_4x3(s32* m, const s32* s) {
  s32 t[16]; std::memcpy(t, m, sizeof t);
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 4; ++c)
      m[r * 4 + c] = static_cast<s32>((static_cast<s64>(s[r * 3]) * t[c] + static_cast<s64>(s[r * 3 + 1]) * t[4 + c] +
                                       static_cast<s64>(s[r * 3 + 2]) * t[8 + c]) >> 12);
  for (int c = 0; c < 4; ++c)
    m[12 + c] = static_cast<s32>((static_cast<s64>(s[9]) * t[c] + static_cast<s64>(s[10]) * t[4 + c] +
                                  static_cast<s64>(s[11]) * t[8 + c] + static_cast<s64>(0x1000) * t[12 + c]) >> 12);
}
inline void mtx_mult_3x3(s32* m, const s32* s) {
  s32 t[12]; std::memcpy(t, m, sizeof t);
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 4; ++c)
      m[r * 4 + c] = static_cast<s32>((static_cast<s64>(s[r * 3]) * t[c] + static_cast<s64>(s[r * 3 + 1]) * t[4 + c] +
                                       static_cast<s64>(s[r * 3 + 2]) * t[8 + c]) >> 12);
}
inline void mtx_scale(s32* m, const s32* s) {
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 4; ++c) m[r * 4 + c] = static_cast<s32>((static_cast<s64>(s[r]) * m[r * 4 + c]) >> 12);
}
inline void mtx_translate(s32* m, const s32* s) {
  for (int c = 0; c < 4; ++c)
    m[12 + c] += static_cast<s32>((static_cast<s64>(s[0]) * m[c] + static_cast<s64>(s[1]) * m[4 + c] + static_cast<s64>(s[2]) * m[8 + c]) >> 12);
}

inline s16 sext10(u32 v) { return static_cast<s16>(static_cast<s16>(v << 6) >> 6); }

// ---- clipping ----------------------------------------------------------------
// Sutherland-Hodgman against the six clip planes, order Z, Y, X. A clipped
// vertex's attributes are interpolated in 64-bit with truncation.

template <int comp, int plane, bool attribs>
void clip_segment(Vertex& out, const Vertex& vin, const Vertex& vout) {
  const s64 num = vin.pos[3] - static_cast<s64>(plane) * vin.pos[comp];
  const s32 den = static_cast<s32>(num - (vout.pos[3] - static_cast<s64>(plane) * vout.pos[comp]));
  auto lerp = [&](s32 a, s32 b) -> s32 { return static_cast<s32>(a + div_s64((static_cast<s64>(b) - a) * num, den)); };
  if (comp != 0) out.pos[0] = lerp(vin.pos[0], vout.pos[0]);
  if (comp != 1) out.pos[1] = lerp(vin.pos[1], vout.pos[1]);
  if (comp != 2) out.pos[2] = lerp(vin.pos[2], vout.pos[2]);
  out.pos[3] = lerp(vin.pos[3], vout.pos[3]);
  out.pos[comp] = plane * out.pos[3];
  if (attribs) {
    out.col[0] = lerp(vin.col[0], vout.col[0]);
    out.col[1] = lerp(vin.col[1], vout.col[1]);
    out.col[2] = lerp(vin.col[2], vout.col[2]);
    out.tex[0] = static_cast<s16>(lerp(vin.tex[0], vout.tex[0]));
    out.tex[1] = static_cast<s16>(lerp(vin.tex[1], vout.tex[1]));
  }
  out.clipped = true;
}

template <int comp, bool attribs>
int clip_against_plane(Vertex* v, int nverts, int clipstart, bool far_clip) {
  Vertex temp[10];
  int c = clipstart;
  if (clipstart == 2) { temp[0] = v[0]; temp[1] = v[1]; }
  // Each pass reads one array and writes the other, avoiding a per-vertex copy.
  for (int i = clipstart; i < nverts; ++i) {
    const int prev = i == 0 ? nverts - 1 : i - 1;
    const int next = i + 1 >= nverts ? 0 : i + 1;
    const Vertex& vtx = v[i];
    if (vtx.pos[comp] > vtx.pos[3]) {
      if (comp == 2 && !far_clip) return 0;       // polygons crossing the far plane are dropped unless bit 12 allows them
      if (v[prev].pos[comp] <= v[prev].pos[3]) clip_segment<comp, 1, attribs>(temp[c++], vtx, v[prev]);
      if (v[next].pos[comp] <= v[next].pos[3]) clip_segment<comp, 1, attribs>(temp[c++], vtx, v[next]);
    } else temp[c++] = vtx;
  }
  nverts = c; c = clipstart;
  for (int i = clipstart; i < nverts; ++i) {
    const int prev = i == 0 ? nverts - 1 : i - 1;
    const int next = i + 1 >= nverts ? 0 : i + 1;
    const Vertex& vtx = temp[i];
    if (vtx.pos[comp] < -vtx.pos[3]) {
      if (temp[prev].pos[comp] >= -temp[prev].pos[3]) clip_segment<comp, -1, attribs>(v[c++], vtx, temp[prev]);
      if (temp[next].pos[comp] >= -temp[next].pos[3]) clip_segment<comp, -1, attribs>(v[c++], vtx, temp[next]);
    } else v[c++] = vtx;
  }
  // Colours keep only their 5-bit integer part across a clip stage.
  for (int i = 0; i < c; ++i)
    for (int k = 0; k < 3; ++k) { v[i].col[k] &= ~0xFFF; v[i].col[k] += 0xFFF; }
  return c;
}

template <bool attribs>
int clip_polygon(Vertex* v, int nverts, int clipstart, bool far_clip) {
  // Trivial accept/reject from the outcodes, ahead of the three plane
  // passes. Sound because a vertex lerped between two vertices outside the
  // same plane stays outside it too. Only vertices at/after clipstart are
  // tested; reused strip vertices are always kept.
  unsigned oc_all = clipstart == 0 ? 0x3Fu : 0u;
  bool inside = true;
  for (int i = clipstart; i < nverts; ++i) {
    const Vertex& t = v[i];
    const s32 w = t.pos[3];
    unsigned oc = 0;
    if (t.pos[0] >  w) oc |= 1u << 0;
    if (t.pos[0] < -w) oc |= 1u << 1;
    if (t.pos[1] >  w) oc |= 1u << 2;
    if (t.pos[1] < -w) oc |= 1u << 3;
    if (t.pos[2] >  w) oc |= 1u << 4;
    if (t.pos[2] < -w) oc |= 1u << 5;
    oc_all &= oc;
    if (oc) { inside = false; if (!oc_all) break; }
  }
  if (oc_all) return 0;
  if (inside) {
    for (int i = 0; i < nverts; ++i)
      for (int k = 0; k < 3; ++k) { v[i].col[k] &= ~0xFFF; v[i].col[k] += 0xFFF; }
    return nverts;
  }
  nverts = clip_against_plane<2, attribs>(v, nverts, clipstart, far_clip);
  nverts = clip_against_plane<1, attribs>(v, nverts, clipstart, far_clip);
  nverts = clip_against_plane<0, attribs>(v, nverts, clipstart, far_clip);
  return nverts;
}

} // namespace

Gpu3D::~Gpu3D() { renderer_.sync_all(); }

Gpu3D::Gpu3D(NDS& nds) : nds_(nds), renderer_(nds) { reset(); }

void Gpu3D::reset_render_state() {
  render_count_.fill(0);
  rstate_ = RenderState{};
}

void Gpu3D::reset() {
  stack_err_ = 0; box_result_ = 0;
  cmd_n_ = par_n_ = inflight_n_ = 0; inflight_cmd_ = 0xFF;
  parse_ = GxParse{};
  gxstat_ = 0; geometry_on_ = rendering_on_ = false; render_on_.store(false, std::memory_order_relaxed);
  dispcnt_ = 0; alpha_ref_val_ = alpha_ref_ = 0;
  toon_.fill(0); edge_.fill(0);
  fog_color_ = fog_offset_ = 0; fog_density_.fill(0);
  clear_attr1_ = 0x3F000000; clear_attr2_ = 0x00007FFF;
  zero_dot_w_limit_ = 0xFFFFFF;
  reset_render_state();
  render_xpos_ = 0;
  matrix_mode_ = 0;
  mtx_identity(proj_.data()); mtx_identity(pos_.data()); mtx_identity(vec_.data()); mtx_identity(tex_.data());
  clip_dirty_ = true; update_clip_matrix();
  proj_stack_.fill(0); tex_stack_.fill(0);
  for (auto& m : pos_stack_) m.fill(0);
  for (auto& m : vec_stack_) m.fill(0);
  proj_sp_ = pos_sp_ = tex_sp_ = 0;
  viewport_.fill(0);
  poly_mode_ = 0;
  std::memset(cur_vertex_, 0, sizeof cur_vertex_); std::memset(vertex_color_, 0, sizeof vertex_color_);
  std::memset(texcoords_, 0, sizeof texcoords_); std::memset(raw_texcoords_, 0, sizeof raw_texcoords_);
  std::memset(normal_, 0, sizeof normal_);
  std::memset(light_dir_, 0, sizeof light_dir_); std::memset(spec_recip_, 0, sizeof spec_recip_);
  std::memset(light_color_, 0, sizeof light_color_);
  std::memset(mat_diffuse_, 0, sizeof mat_diffuse_); std::memset(mat_ambient_, 0, sizeof mat_ambient_);
  std::memset(mat_specular_, 0, sizeof mat_specular_); std::memset(mat_emission_, 0, sizeof mat_emission_);
  use_shininess_ = false; shininess_.fill(0);
  polygon_attr_ = cur_polygon_attr_ = 0; texparam_ = texpal_ = 0;
  std::memset(pos_test_, 0, sizeof pos_test_); std::memset(vec_test_, 0, sizeof vec_test_);
  std::memset(temp_vtx_, 0, sizeof temp_vtx_);
  reset_vptr();
  vertex_num_ = vertex_in_poly_ = consecutive_polys_ = 0;
  last_strip_poly_ = nullptr; num_opaque_ = 0;
  bank_ = 0; render_bank_ = 1; raster_bank_ = 1; pending_bank_ = 1; num_vertices_ = num_polygons_ = 0;
  swaps_ = 0; list_unconsumed_ = false;
  flush_request_ = flush_attr_ = 0; render_identical_ = false; swapped_ = false; swap_wait_ = false; swap_busy_until_ = 0; list_same_ = false;
  renderer_.reset();
}

void Gpu3D::set_powcnt(u16 value) {
  geometry_on_ = value & (1 << 3);
  rendering_on_ = value & (1 << 2); render_on_.store(rendering_on_, std::memory_order_relaxed);
  if (!rendering_on_) reset_render_state();
}

// ---- FIFO ---------------------------------------------------------------------

// Replay everything queued. Called at VBlank, on a read, and when the log
// nears full. No time passes, so read() can report busy bits clear unconditionally.
u64* gx_cmd_census() {
  static u64 hist[256];
  static bool reg = false;
  if (!reg) {
    reg = true;
    std::atexit([] {
      u64 tot = 0;
      for (u64 v : hist) tot += v;
      if (!tot) return;
      std::fprintf(stderr, "[gx] command census: %llu commands\n", (unsigned long long)tot);
      for (int pass = 0; pass < 12; ++pass) {   // the twelve most frequent, descending
        int best = -1;
        for (int k = 0; k < 256; ++k) if (hist[k] && (best < 0 || hist[k] > hist[best])) best = k;
        if (best < 0) break;
        std::fprintf(stderr, "[gx]   %02x %12llu %5.1f%%\n", best, (unsigned long long)hist[best], 100.0 * static_cast<double>(hist[best]) / static_cast<double>(tot));
        hist[best] = 0;
      }
    });
  }
  return hist;
}

void Gpu3D::drain_all() {
  if (!geometry_on_) return;
  if (!cmd_n_) return;
  DS_PROF(GX_RUN);
  // One pass: a command's parameters are already contiguous.
  const u8* const c = cmd_log_.get();
  const u32* const q = par_log_.get();
  u32 pi = 0;
  // DS_PROFILE: which commands the lists are made of (gx_cmd_census).
  static u64* const hist = prof::heavy ? gx_cmd_census() : nullptr;
  if (hist) for (u32 i = 0; i < cmd_n_; ++i) ++hist[c[i]];
  for (u32 i = 0; i < cmd_n_; ++i) {
    const u8 k = c[i];
    exec_single(k, q + pi);
    pi += CMD_PARAMS[k];
  }
  if (inflight_n_) std::memmove(par_log_.get(), par_log_.get() + par_n_, inflight_n_ * 4);
  cmd_n_ = 0; par_n_ = 0;
  // Only after something actually ran: firing on every empty-log observation
  // would re-arm GXFIFO DMA at a different point in the guest's polling loop.
  check_fifo_dma();
  check_fifo_irq_fast();
}

void Gpu3D::check_fifo_irq() {
  // FIFO always reads empty and under half full, so both IRQ modes are
  // satisfied whenever either is selected.
  nds_.io.set_irq_line(Cpu::ARM9, io::IRQ_GX_FIFO, (gxstat_ >> 30) != 0);
}

void Gpu3D::check_fifo_dma() {
  if (nds_.dma.gx_armed()) nds_.dma.check(Cpu::ARM9, dma::MODE9_GXFIFO);
}

// Packed command port: up to four command bytes followed by their parameters.
// Templated on its sink so the single-word port and the DMA burst below
// cannot drift apart.
template <class S>
inline void Gpu3D::gxfifo_word(u32 value, GxParse& p, S& sink) {
  if (p.num_cmds != 0) {
    // A parameter that does not complete its command: everything else takes
    // the packed-command walk below.
    if (++p.param_count < p.total_params) { sink.param(value); return; }
    sink.param(value); sink.commit(static_cast<u8>(p.cur_cmd));
    p.cur_cmd >>= 8; --p.num_cmds;
    if (p.cur_cmd == 0) { p.num_cmds = 0; return; }
    p.param_count = 0;
    p.total_params = CMD_PARAMS[p.cur_cmd & 0xFF];
    if (p.total_params > 0) return;
    // Zero-parameter commands packed behind it: the walk enqueues them.
  } else {
    p.num_cmds = 4; p.cur_cmd = value; p.param_count = 0;
    p.total_params = CMD_PARAMS[p.cur_cmd & 0xFF];
    if (p.total_params > 0) return;
  }
  for (;;) {
    // NOPs (command 0) are not logged at all.
    if (p.cur_cmd & 0xFF) sink.commit(static_cast<u8>(p.cur_cmd & 0xFF));
    if (p.param_count >= p.total_params) {
      p.cur_cmd >>= 8;
      if (--p.num_cmds == 0) break;
      p.param_count = 0;
      p.total_params = CMD_PARAMS[p.cur_cmd & 0xFF];
    }
    if (p.param_count < p.total_params) break;
  }
}

// The sink both GXFIFO paths share: parameters land beyond par_n_ and a
// commit is what makes them real.
struct Gpu3D::Sink {
  Gpu3D& g;
  void param(u32 v) { g.par_log_[g.par_n_ + g.inflight_n_++] = v; }
  void commit(u8 c) { g.log_commit(c); }
};

void Gpu3D::gxfifo_write(u32 value) {
  log_room();
  if (parse_.num_cmds == 0) inflight_cmd_ = 0xFF;   // a fresh word: no port command is half-written
  Sink s{*this};
  gxfifo_word(value, parse_, s);
}

// A GXFIFO DMA burst: `n` words from one direct-mapped source page. Nothing
// can observe the log mid-burst (CPU stopped for the DMA); caller keeps `n`
// under fifo_burst_room() so the log cannot fill.
void Gpu3D::gxfifo_dma_burst(const u8* src, u32 n) {
  if (!geometry_on_) return;
  // The cursors ride in registers for the whole run and are written back once.
  struct BurstSink {
    u8* c; u32* p; u32 cn, pn, inf;
    void param(u32 v) { p[pn + inf++] = v; }
    void commit(u8 k) { c[cn++] = k; pn += inf; inf = 0; }
    // A run of words certainly parameters of the in-flight command: straight
    // into the log, no state machine per word.
    void params(const u8* from, u32 k) { std::memcpy(p + pn + inf, from, k * 4); inf += k; }
  } sink{cmd_log_.get(), par_log_.get(), cmd_n_, par_n_, inflight_n_};
  GxParse p = parse_;   // local copy, written back once
  for (u32 i = 0; i < n; ) {
    // Bulk parameters: copy them in one go, leaving the LAST parameter to go
    // through the walk below so the commit happens exactly where it did before.
    if (p.num_cmds != 0) {
      const u32 rem = p.total_params - p.param_count;   // includes the completing word
      if (rem > 2) {
        const u32 avail = n - i, want = rem - 1;
        const u32 take = want < avail ? want : avail;
        if (take >= 2) {
          sink.params(src + i * 4, take);
          p.param_count += take;
          i += take;
          continue;
        }
      }
    }
    u32 v;
    std::memcpy(&v, src + i * 4, 4);
    gxfifo_word(v, p, sink);
    ++i;
  }
  parse_ = p;
  cmd_n_ = sink.cn; par_n_ = sink.pn; inflight_n_ = sink.inf;
}

// ---- command execution --------------------------------------------------------

// `p` points at this command's first parameter; a zero-parameter command may
// still read p[0], which is why par_log_ carries PAR_SLACK words of tail.
void Gpu3D::exec_single(u8 cmd, const u32* p) {
  const u32 param = p[0];
  switch (cmd) {
  case 0x10: matrix_mode_ = param & 3; break;
  case 0x11:   // push
    
    if (matrix_mode_ == 0) {
      if (proj_sp_ > 0) stack_err_ = 1u << 15;
      proj_stack_ = proj_; proj_sp_ = (proj_sp_ + 1) & 1;
    } else if (matrix_mode_ == 3) {
      if (tex_sp_ > 0) stack_err_ = 1u << 15;
      tex_stack_ = tex_; tex_sp_ = (tex_sp_ + 1) & 1;
    } else {
      if (pos_sp_ > 30) stack_err_ = 1u << 15;
      pos_stack_[pos_sp_ & 0x1F] = pos_; vec_stack_[pos_sp_ & 0x1F] = vec_;
      pos_sp_ = (pos_sp_ + 1) & 0x3F;
    }
    break;
  case 0x12:   // pop
    
    if (matrix_mode_ == 0) {
      if (proj_sp_ == 0) stack_err_ = 1u << 15;
      proj_sp_ = (proj_sp_ - 1) & 1; proj_ = proj_stack_; clip_dirty_ = true;
    } else if (matrix_mode_ == 3) {
      if (tex_sp_ == 0) stack_err_ = 1u << 15;
      tex_sp_ = (tex_sp_ - 1) & 1; tex_ = tex_stack_;
    } else {
      const s32 off = static_cast<s32>(param << 26) >> 26;
      pos_sp_ = (pos_sp_ - off) & 0x3F;
      if (pos_sp_ > 30) stack_err_ = 1u << 15;
      pos_ = pos_stack_[pos_sp_ & 0x1F]; vec_ = vec_stack_[pos_sp_ & 0x1F]; clip_dirty_ = true;
    }
    break;
  case 0x13:   // store
    if (matrix_mode_ == 0) proj_stack_ = proj_;
    else if (matrix_mode_ == 3) tex_stack_ = tex_;
    else {
      const u32 a = param & 0x1F;
      if (a > 30) stack_err_ = 1u << 15;
      pos_stack_[a] = pos_; vec_stack_[a] = vec_;
    }
    break;
  case 0x14:   // restore
    if (matrix_mode_ == 0) { proj_ = proj_stack_; clip_dirty_ = true; }
    else if (matrix_mode_ == 3) { tex_ = tex_stack_; }
    else {
      const u32 a = param & 0x1F;
      if (a > 30) stack_err_ = 1u << 15;
      pos_ = pos_stack_[a]; vec_ = vec_stack_[a]; clip_dirty_ = true;
    }
    break;
  case 0x15:   // identity
    if (matrix_mode_ == 0) { mtx_identity(proj_.data()); clip_dirty_ = true; }
    else if (matrix_mode_ == 3) mtx_identity(tex_.data());
    else { mtx_identity(pos_.data()); if (matrix_mode_ == 2) mtx_identity(vec_.data()); clip_dirty_ = true; }
    break;
  case 0x20:   // colour
    vertex_color_[0] = param & 0x1F; vertex_color_[1] = (param >> 5) & 0x1F; vertex_color_[2] = (param >> 10) & 0x1F;
    break;
  case 0x21:   // normal
    normal_[0] = sext10(param & 0x3FF); normal_[1] = sext10((param >> 10) & 0x3FF); normal_[2] = sext10((param >> 20) & 0x3FF);
    calculate_lighting();
    break;
  case 0x22:   // texcoord
    raw_texcoords_[0] = static_cast<s16>(param & 0xFFFF); raw_texcoords_[1] = static_cast<s16>(param >> 16);
    if ((texparam_ >> 30) == 1) {
      texcoords_[0] = static_cast<s16>((raw_texcoords_[0] * tex_[0] + raw_texcoords_[1] * tex_[4] + tex_[8] + tex_[12]) >> 12);
      texcoords_[1] = static_cast<s16>((raw_texcoords_[0] * tex_[1] + raw_texcoords_[1] * tex_[5] + tex_[9] + tex_[13]) >> 12);
    } else { texcoords_[0] = raw_texcoords_[0]; texcoords_[1] = raw_texcoords_[1]; }
    break;
  case 0x24:   // 10-bit vertex
    cur_vertex_[0] = static_cast<s16>((param & 0x3FF) << 6); cur_vertex_[1] = static_cast<s16>((param & 0xFFC00) >> 4); cur_vertex_[2] = static_cast<s16>((param & 0x3FF00000) >> 14);
    submit_vertex();
    break;
  case 0x25: cur_vertex_[0] = static_cast<s16>(param & 0xFFFF); cur_vertex_[1] = static_cast<s16>(param >> 16); submit_vertex(); break;
  case 0x26: cur_vertex_[0] = static_cast<s16>(param & 0xFFFF); cur_vertex_[2] = static_cast<s16>(param >> 16); submit_vertex(); break;
  case 0x27: cur_vertex_[1] = static_cast<s16>(param & 0xFFFF); cur_vertex_[2] = static_cast<s16>(param >> 16); submit_vertex(); break;
  case 0x28:   // delta vertex
    cur_vertex_[0] = static_cast<s16>(cur_vertex_[0] + sext10(param & 0x3FF));
    cur_vertex_[1] = static_cast<s16>(cur_vertex_[1] + sext10((param >> 10) & 0x3FF));
    cur_vertex_[2] = static_cast<s16>(cur_vertex_[2] + sext10((param >> 20) & 0x3FF));
    submit_vertex();
    break;
  case 0x29: polygon_attr_ = param; break;
  case 0x2A: texparam_ = param; break;
  case 0x2B: texpal_ = param & 0x1FFF; break;
  case 0x30:   // diffuse / ambient
    mat_diffuse_[0] = param & 0x1F; mat_diffuse_[1] = (param >> 5) & 0x1F; mat_diffuse_[2] = (param >> 10) & 0x1F;
    mat_ambient_[0] = (param >> 16) & 0x1F; mat_ambient_[1] = (param >> 21) & 0x1F; mat_ambient_[2] = (param >> 26) & 0x1F;
    if (param & 0x8000) { vertex_color_[0] = mat_diffuse_[0]; vertex_color_[1] = mat_diffuse_[1]; vertex_color_[2] = mat_diffuse_[2]; }
    break;
  case 0x31:   // specular / emission
    mat_specular_[0] = param & 0x1F; mat_specular_[1] = (param >> 5) & 0x1F; mat_specular_[2] = (param >> 10) & 0x1F;
    mat_emission_[0] = (param >> 16) & 0x1F; mat_emission_[1] = (param >> 21) & 0x1F; mat_emission_[2] = (param >> 26) & 0x1F;
    use_shininess_ = (param & 0x8000) != 0;
    break;
  case 0x32: {  // light vector
    const u32 l = param >> 30;
    const s16 d0 = sext10(param & 0x3FF), d1 = sext10((param >> 10) & 0x3FF), d2 = sext10((param >> 20) & 0x3FF);
    // Transformed by the vector matrix, kept as a signed 11-bit value.
    auto s11 = [](s32 v) { return static_cast<s16>(static_cast<s32>(static_cast<u32>(v) << 21) >> 21); };
    light_dir_[l][0] = s11(-((d0 * vec_[0] + d1 * vec_[4] + d2 * vec_[8]) >> 12));
    light_dir_[l][1] = s11(-((d0 * vec_[1] + d1 * vec_[5] + d2 * vec_[9]) >> 12));
    light_dir_[l][2] = s11(-((d0 * vec_[2] + d1 * vec_[6] + d2 * vec_[10]) >> 12));
    const s32 den = -((static_cast<s32>(static_cast<u32>(d0 * vec_[2] + d1 * vec_[6] + d2 * vec_[10]) << 9)) >> 21) + (1 << 9);
    spec_recip_[l] = den == 0 ? 0 : (1 << 18) / den;
    break;
  }
  case 0x33: {  // light colour
    const u32 l = param >> 30;
    light_color_[l][0] = param & 0x1F; light_color_[l][1] = (param >> 5) & 0x1F; light_color_[l][2] = (param >> 10) & 0x1F;
    break;
  }
  case 0x40:   // begin
    poly_mode_ = param & 3;
    vertex_num_ = 0; vertex_in_poly_ = 0; consecutive_polys_ = 0;
    last_strip_poly_ = nullptr;
    cur_polygon_attr_ = polygon_attr_;
    break;
  case 0x41: break;   // end: no effect
  case 0x50:   // swap buffers
    flush_attr_ = param & 3;   // finalise_list's sort mode
    // Takes effect now: list finalised and bank flipped immediately; render
    // happens at VBlank. swap_wait_ carries the busy bit until then.
    if (rendering_on_) finalise_list();
    swapped_ = true;
    swap_wait_ = true;
    { std::lock_guard<std::mutex> lk(bank_mu_); render_bank_ = bank_; bank_ = next_write_bank(); }
    num_vertices_ = num_polygons_ = num_opaque_ = 0;
    break;
  case 0x60:   // viewport (Y is upside down)
    viewport_[0] = param & 0xFF;
    viewport_[1] = (191 - ((param >> 8) & 0xFF)) & 0xFF;
    viewport_[2] = (param >> 16) & 0xFF;
    viewport_[3] = (191 - (param >> 24)) & 0xFF;
    viewport_[4] = (viewport_[2] - viewport_[0] + 1) & 0x1FF;
    viewport_[5] = (viewport_[1] - viewport_[3] + 1) & 0xFF;
    break;
  case 0x72: vec_test(param); break;
  case 0x16: case 0x17: case 0x18: case 0x19: case 0x1A: case 0x1B: case 0x1C: {
    const s32* m = reinterpret_cast<const s32*>(p);
    auto on_matrix = [&](auto&& op) {
      if (matrix_mode_ == 0) { op(proj_.data()); clip_dirty_ = true; }
      else if (matrix_mode_ == 3) { op(tex_.data()); }
      else {
        op(pos_.data());
        if (matrix_mode_ == 2) op(vec_.data()); else clip_dirty_ = true;
      }
    };
    switch (cmd) {
    case 0x16: on_matrix([&](s32* d) { std::memcpy(d, m, 64); }); break;
    case 0x17: on_matrix([&](s32* d) { mtx_load_4x3(d, m); }); break;
    case 0x18: on_matrix([&](s32* d) { mtx_mult_4x4(d, m); }); break;
    case 0x19: on_matrix([&](s32* d) { mtx_mult_4x3(d, m); }); break;
    case 0x1A: on_matrix([&](s32* d) { mtx_mult_3x3(d, m); }); break;
    // Scale never touches the vector matrix, in any mode.
    case 0x1B:
      if (matrix_mode_ == 0) { mtx_scale(proj_.data(), m); clip_dirty_ = true; }
      else if (matrix_mode_ == 3) { mtx_scale(tex_.data(), m); }
      else { mtx_scale(pos_.data(), m); clip_dirty_ = true; }
      break;
    case 0x1C: on_matrix([&](s32* d) { mtx_translate(d, m); }); break;
    default: break;
    }
    break;
  }
  case 0x23:   // full vertex
    cur_vertex_[0] = static_cast<s16>(p[0] & 0xFFFF); cur_vertex_[1] = static_cast<s16>(p[0] >> 16);
    cur_vertex_[2] = static_cast<s16>(p[1] & 0xFFFF);
    submit_vertex();
    break;
  case 0x34:   // shininess table
    for (int i = 0; i < 128; i += 4) {
      const u32 v = p[i >> 2];
      shininess_[i] = v & 0xFF; shininess_[i + 1] = (v >> 8) & 0xFF; shininess_[i + 2] = (v >> 16) & 0xFF; shininess_[i + 3] = v >> 24;
    }
    break;
  case 0x71:   // position test
    cur_vertex_[0] = static_cast<s16>(p[0] & 0xFFFF); cur_vertex_[1] = static_cast<s16>(p[0] >> 16);
    cur_vertex_[2] = static_cast<s16>(p[1] & 0xFFFF);
    pos_test();
    break;
  case 0x70: box_test(p); break;
  default: break;
  }
}


// ---- geometry -----------------------------------------------------------------

void Gpu3D::update_clip_matrix() {
  if (!clip_dirty_) return;
  clip_dirty_ = false;
  clip_ = proj_;
  mtx_mult_4x4(clip_.data(), pos_.data());
}

// Reorder temp_vtx_ so that position i is slot i again (only the state writer
// needs this; an equivalent representation otherwise).
void Gpu3D::normalise_temp_vtx() {
  Vertex tmp[4];
  for (int i = 0; i < 4; ++i) tmp[i] = *vptr_[i];
  for (int i = 0; i < 4; ++i) temp_vtx_[i] = tmp[i];
  reset_vptr();
}

void Gpu3D::submit_vertex() {
  const s64 v[4] = {cur_vertex_[0], cur_vertex_[1], cur_vertex_[2], 0x1000};
  Vertex& vt = *vptr_[vertex_in_poly_];
  update_clip_matrix();
#if DSPERATE_NEON
  // The four clip-space coordinates are four dot products against the same
  // vertex: one 4-lane pass, bit-exact with the scalar arm.
  {
    const int32x4_t r0 = vld1q_s32(clip_.data() + 0), r1 = vld1q_s32(clip_.data() + 4);
    const int32x4_t r2 = vld1q_s32(clip_.data() + 8), r3 = vld1q_s32(clip_.data() + 12);
    const int32x4_t q0 = vdupq_n_s32(cur_vertex_[0]), q1 = vdupq_n_s32(cur_vertex_[1]);
    const int32x4_t q2 = vdupq_n_s32(cur_vertex_[2]), q3 = vdupq_n_s32(0x1000);
    int64x2_t lo = vmull_s32(vget_low_s32(r0), vget_low_s32(q0));
    int64x2_t hi = compat::mull_high_s32(r0, q0);
    lo = vmlal_s32(lo, vget_low_s32(r1), vget_low_s32(q1)); hi = compat::mlal_high_s32(hi, r1, q1);
    lo = vmlal_s32(lo, vget_low_s32(r2), vget_low_s32(q2)); hi = compat::mlal_high_s32(hi, r2, q2);
    lo = vmlal_s32(lo, vget_low_s32(r3), vget_low_s32(q3)); hi = compat::mlal_high_s32(hi, r3, q3);
    const int32x4_t p = vcombine_s32(vmovn_s64(vshrq_n_s64(lo, 12)), vmovn_s64(vshrq_n_s64(hi, 12)));
    vst1q_s32(vt.pos, p);
    // Six frustum tests while position is still in a register, packed to the
    // same bits outcode() produces; lane 3 (W) is discarded via zero bit constants.
    const int32x4_t w = compat::dup_laneq_s32<3>(p);
    static const uint32x4_t gt_bits = {1, 4, 16, 0}, lt_bits = {2, 8, 32, 0};
    vt.oc = static_cast<u8>(compat::addv_u32(vorrq_u32(vandq_u32(vcgtq_s32(p, w), gt_bits), vandq_u32(vcltq_s32(p, vnegq_s32(w)), lt_bits))));
  }
#else
  for (int c = 0; c < 4; ++c)
    vt.pos[c] = static_cast<s32>((v[0] * clip_[c] + v[1] * clip_[4 + c] + v[2] * clip_[8 + c] + v[3] * clip_[12 + c]) >> 12);
  vt.oc = outcode(vt.pos);
#endif
  for (int c = 0; c < 3; ++c) vt.col[c] = (vertex_color_[c] << 12) + 0xFFF;
  if ((texparam_ >> 30) == 3) {
    vt.tex[0] = static_cast<s16>(((v[0] * tex_[0] + v[1] * tex_[4] + v[2] * tex_[8]) >> 24) + raw_texcoords_[0]);
    vt.tex[1] = static_cast<s16>(((v[0] * tex_[1] + v[1] * tex_[5] + v[2] * tex_[9]) >> 24) + raw_texcoords_[1]);
  } else { vt.tex[0] = texcoords_[0]; vt.tex[1] = texcoords_[1]; }
  vt.clipped = false;

  ++vertex_num_; ++vertex_in_poly_;
  switch (poly_mode_) {
  case 0: if (vertex_in_poly_ == 3) { vertex_in_poly_ = 0; submit_polygon(); ++consecutive_polys_; } break;
  case 1: if (vertex_in_poly_ == 4) { vertex_in_poly_ = 0; submit_polygon(); ++consecutive_polys_; } break;
  case 2:   // triangle strip
    if (consecutive_polys_ & 1) {
      // swap(temp_vtx_[0], temp_vtx_[1]), then temp_vtx_[1] = temp_vtx_[2].
      { Vertex* t = vptr_[0]; vptr_[0] = vptr_[1]; vptr_[1] = t; }
      vertex_in_poly_ = 2; submit_polygon(); ++consecutive_polys_;
      { Vertex* t = vptr_[1]; vptr_[1] = vptr_[2]; vptr_[2] = t; }
    } else if (vertex_in_poly_ == 3) {
      vertex_in_poly_ = 2; submit_polygon(); ++consecutive_polys_;
      // temp_vtx_[0] = temp_vtx_[1]; temp_vtx_[1] = temp_vtx_[2];
      { Vertex* t = vptr_[0]; vptr_[0] = vptr_[1]; vptr_[1] = vptr_[2]; vptr_[2] = t; }
    }
    break;
  case 3:   // quad strip
    if (vertex_in_poly_ == 4) {
      // swap(temp_vtx_[2], temp_vtx_[3]), then temp_vtx_[0]=[3], temp_vtx_[1]=[2].
      { Vertex* t = vptr_[2]; vptr_[2] = vptr_[3]; vptr_[3] = t; }
      vertex_in_poly_ = 2; submit_polygon(); ++consecutive_polys_;
      { Vertex* x = vptr_[0]; Vertex* y = vptr_[1];
        vptr_[0] = vptr_[3]; vptr_[1] = vptr_[2]; vptr_[2] = x; vptr_[3] = y; }
    }
    break;
  }
}

namespace {
// Viewport transform of one vertex in place. W is truncated to 24 bits; the
// 32-bit divider loses a bit of precision when W exceeds 16 bits.
inline void viewport_vertex(Vertex& vt, const std::array<u32, 6>& viewport) {
  vt.pos[3] &= 0x00FFFFFF;
  u32 px, py;
  const u32 w = static_cast<u32>(vt.pos[3]);
  if (w == 0) { px = 0; py = 0; }
  else {
    px = static_cast<u32>(vt.pos[0]) + w;
    py = static_cast<u32>(-vt.pos[1]) + w;
    u32 den = w;
    if (w > 0xFFFF) { px >>= 1; py >>= 1; den >>= 1; }
    den <<= 1;
    px = ((px * viewport[4]) / den) + viewport[0];
    py = ((py * viewport[5]) / den) + viewport[3];
  }
  vt.sx = px & 0x1FF;
  vt.sy = py & 0xFF;
}

// 5-bit colour to 9 bits: (c << 4) + 0xF for non-zero components.
inline void final_colour(Vertex& vt) {
  for (int c = 0; c < 3; ++c) { vt.fcol[c] = vt.col[c] >> 12; if (vt.fcol[c]) vt.fcol[c] = (vt.fcol[c] << 4) + 0xF; }
}
} // namespace

// The clipper's six plane tests as a bit set, one bit per plane.
u8 Gpu3D::outcode(const s32* pos) {
  const s32 w = pos[3];
  unsigned oc = 0;
  if (pos[0] >  w) oc |= 1u << 0;
  if (pos[0] < -w) oc |= 1u << 1;
  if (pos[1] >  w) oc |= 1u << 2;
  if (pos[1] < -w) oc |= 1u << 3;
  if (pos[2] >  w) oc |= 1u << 4;
  if (pos[2] < -w) oc |= 1u << 5;
  return static_cast<u8>(oc);
}

// This entry does only the trivial reject and back-face cull, reading source
// vertices in place. Survivors go to one of two emitters: the common one
// writes straight into vertex RAM, the rare one runs the clipper's plane
// passes over a scratch copy. Strip-reuse must be decided before the reject
// test, since it decides which vertices the test covers.
void Gpu3D::submit_polygon() {
  const Vertex* src[4];
  u16 reused_idx[2] = {0, 0};
  int clipstart = 0, lastpolyverts = 0;
  const int nverts = (poly_mode_ & 1) ? 4 : 3;

  // Strips share two unclipped vertices with the previous polygon. Decided
  // first because it determines which vertices the reject test covers.
  if (poly_mode_ >= 2 && last_strip_poly_) {
    int id0, id1;
    if (poly_mode_ == 2) {
      if (consecutive_polys_ & 1) { id0 = 2; id1 = 1; } else { id0 = 0; id1 = 2; }
      lastpolyverts = 3;
    } else { id0 = 3; id1 = 2; lastpolyverts = 4; }
    if (static_cast<int>(last_strip_poly_->nverts) == lastpolyverts &&
        !vram_[last_strip_poly_->vtx[id0]].clipped && !vram_[last_strip_poly_->vtx[id1]].clipped) {
      reused_idx[0] = last_strip_poly_->vtx[id0]; reused_idx[1] = last_strip_poly_->vtx[id1];
      src[0] = &vram_[reused_idx[0]]; src[1] = &vram_[reused_idx[1]];
      clipstart = 2;
    }
  }
  for (int i = clipstart; i < nverts; ++i) src[i] = vptr_[i];

  // Trivial reject: every new vertex outside the same plane. Reused strip
  // vertices are untested and forbid the reject, as in the clipper.
  unsigned oc_all = clipstart == 0 ? 0x3Fu : 0u, oc_any = 0;
  for (int i = clipstart; i < nverts; ++i) { oc_all &= src[i]->oc; oc_any |= src[i]->oc; }
  if (oc_all) { last_strip_poly_ = nullptr; return; }

  // Culling from the first three vertices' clip-space positions.
  const Vertex &v0 = *vptr_[0], &v1 = *vptr_[1], &v2 = *vptr_[2];
  s64 nx = static_cast<s64>(v0.pos[1] - v1.pos[1]) * (v2.pos[3] - v1.pos[3]) - static_cast<s64>(v0.pos[3] - v1.pos[3]) * (v2.pos[1] - v1.pos[1]);
  s64 ny = static_cast<s64>(v0.pos[3] - v1.pos[3]) * (v2.pos[0] - v1.pos[0]) - static_cast<s64>(v0.pos[0] - v1.pos[0]) * (v2.pos[3] - v1.pos[3]);
  s64 nz = static_cast<s64>(v0.pos[0] - v1.pos[0]) * (v2.pos[1] - v1.pos[1]) - static_cast<s64>(v0.pos[1] - v1.pos[1]) * (v2.pos[0] - v1.pos[0]);
  while ((((nx >> 31) ^ (nx >> 63)) != 0) || (((ny >> 31) ^ (ny >> 63)) != 0) || (((nz >> 31) ^ (nz >> 63)) != 0)) { nx >>= 4; ny >>= 4; nz >>= 4; }
  const s64 dot = static_cast<s64>(v1.pos[0]) * nx + static_cast<s64>(v1.pos[1]) * ny + static_cast<s64>(v1.pos[3]) * nz;
  const bool facing = dot <= 0;
  if (dot < 0) { if (!(cur_polygon_attr_ & (1 << 7))) { last_strip_poly_ = nullptr; return; } }
  else if (dot > 0) { if (!(cur_polygon_attr_ & (1 << 6))) { last_strip_poly_ = nullptr; return; } }

  if (oc_any == 0) emit_polygon_unclipped(src, nverts, clipstart, reused_idx, facing);
  else emit_polygon_clipped(src, nverts, clipstart, reused_idx, lastpolyverts, facing);
}

// A polygon accepted whole: new vertices write directly to vertex RAM slots,
// which stay beyond num_vertices_ until the zero-dot test passes.
void Gpu3D::emit_polygon_unclipped(const Vertex* const* src, int nverts, int clipstart, const u16* reused_idx, bool facing) {
  if (num_polygons_ >= PRAM_BANK || num_vertices_ + nverts > VRAM_BANK) {
    last_strip_poly_ = nullptr;
    dispcnt_ |= (1 << 13);                     // RAM overflow flag
    return;
  }
  Vertex* vr = cur_vram();
  Vertex* fresh = vr + num_vertices_;
  const Vertex* vv[4];
  for (int i = 0; i < clipstart; ++i) vv[i] = src[i];
  for (int i = clipstart; i < nverts; ++i) {
    Vertex& vt = fresh[i - clipstart];
    vt = *src[i];
    // Colours keep only their 5-bit integer part across the clip stage.
    for (int k = 0; k < 3; ++k) { vt.col[k] &= ~0xFFF; vt.col[k] += 0xFFF; }
    viewport_vertex(vt, viewport_);
    vv[i] = &vt;
  }

  // Zero-dot polygons (all vertices on one pixel) beyond the W limit are dropped.
  if (!(cur_polygon_attr_ & (1 << 13))) {
    bool zerodot = true, allbehind = true;
    for (int i = 0; i < nverts; ++i) {
      if (vv[i]->sx != vv[0]->sx || vv[i]->sy != vv[0]->sy) { zerodot = false; break; }
      if (static_cast<u32>(vv[i]->pos[3]) <= zero_dot_w_limit_) { allbehind = false; break; }
    }
    if (zerodot && allbehind) { last_strip_poly_ = nullptr; return; }
  }


  Polygon* poly = new_polygon(facing);
  for (int i = 0; i < clipstart; ++i) poly->vtx[i] = reused_idx[i];
  for (int i = clipstart; i < nverts; ++i) {
    final_colour(fresh[i - clipstart]);
    poly->vtx[i] = static_cast<u16>(vram_base() + num_vertices_++);
  }
  poly->nverts = static_cast<u32>(nverts);
  finish_polygon(poly, nverts);
}

// A polygon that crosses the frustum: plane passes run over a scratch copy,
// which may change the vertex count and so whether reused strip vertices can
// still be shared by index.
void Gpu3D::emit_polygon_clipped(const Vertex* const* src, int nverts, int clipstart, const u16* reused_idx, int lastpolyverts, bool facing) {
  Vertex clipped[10];
  for (int i = 0; i < nverts; ++i) clipped[i] = *src[i];

  nverts = clip_polygon<true>(clipped, nverts, clipstart, cur_polygon_attr_ & (1 << 12));
  if (nverts == 0) { last_strip_poly_ = nullptr; return; }

  if (num_polygons_ >= PRAM_BANK || num_vertices_ + nverts > VRAM_BANK) {
    last_strip_poly_ = nullptr;
    dispcnt_ |= (1 << 13);                     // RAM overflow flag
    return;
  }

  for (int i = clipstart; i < nverts; ++i) viewport_vertex(clipped[i], viewport_);

  // Zero-dot polygons (all vertices on one pixel) beyond the W limit are dropped.
  if (!(cur_polygon_attr_ & (1 << 13))) {
    bool zerodot = true, allbehind = true;
    for (int i = 0; i < nverts; ++i) {
      if (clipped[i].sx != clipped[0].sx || clipped[i].sy != clipped[0].sy) { zerodot = false; break; }
      if (static_cast<u32>(clipped[i].pos[3]) <= zero_dot_w_limit_) { allbehind = false; break; }
    }
    if (zerodot && allbehind) { last_strip_poly_ = nullptr; return; }
  }


  Polygon* poly = new_polygon(facing);
  Vertex* vr = cur_vram();
  if (clipstart > 0) {
    if (nverts == lastpolyverts) { poly->vtx[0] = reused_idx[0]; poly->vtx[1] = reused_idx[1]; }
    else {
      vr[num_vertices_] = *src[0]; poly->vtx[0] = static_cast<u16>(vram_base() + num_vertices_);
      vr[num_vertices_ + 1] = *src[1]; poly->vtx[1] = static_cast<u16>(vram_base() + num_vertices_ + 1);
      num_vertices_ += 2;
    }
    poly->nverts += 2;
  }
  for (int i = clipstart; i < nverts; ++i) {
    Vertex& vt = vr[num_vertices_];
    vt = clipped[i];
    poly->vtx[i] = static_cast<u16>(vram_base() + num_vertices_);
    ++num_vertices_; ++poly->nverts;
    final_colour(vt);
  }
  finish_polygon(poly, nverts);
}

// Allocate the next polygon RAM entry and fill the attributes that do not
// depend on its vertices.
Polygon* Gpu3D::new_polygon(bool facing) {
  Polygon* poly = &cur_pram()[num_polygons_++];
  poly->nverts = 0;
  poly->attr = cur_polygon_attr_; poly->texparam = texparam_; poly->texpal = texpal_;
  poly->degenerate = false;
  poly->facing = facing;
  const u32 texfmt = (texparam_ >> 26) & 7, polyalpha = (cur_polygon_attr_ >> 16) & 0x1F;
  poly->translucent = (texfmt == 1 || texfmt == 6) || (polyalpha > 0 && polyalpha < 31);
  poly->shadow_mask = (cur_polygon_attr_ & 0x3F000030) == 0x00000030;
  poly->shadow = ((cur_polygon_attr_ & 0x30) == 0x30) && !poly->shadow_mask;
  if (!poly->translucent) ++num_opaque_;
  return poly;
}

// Bounds, sort key and per-vertex depth from the polygon's vertex RAM entries.
void Gpu3D::finish_polygon(Polygon* poly, int nverts) {
  // Bounds, and the W range used to normalise W to 16 bits.
  u32 vtop = 0, vbot = 0; s32 ytop = 192, ybot = 0, xtop = 256, xbot = 0; u32 wsize = 0;
  for (int i = 0; i < nverts; ++i) {
    const Vertex& vt = vram_[poly->vtx[i]];
    if (vt.sy < ytop) { xtop = vt.sx; ytop = vt.sy; vtop = i; }
    if (vt.sy > ybot || (vt.sy == ybot && vt.sx > xbot)) { xbot = vt.sx; ybot = vt.sy; vbot = i; }
    const u32 w = static_cast<u32>(vt.pos[3]);
    if (w == 0) poly->degenerate = true;
    // Smallest multiple of 4 that shifts w to zero, capped at 32.
    if (w) { const u32 need = (35 - static_cast<u32>(__builtin_clz(w))) & ~3u; if (need > wsize) wsize = need; }
  }
  poly->vtop = vtop; poly->vbot = vbot; poly->ytop = ytop; poly->ybot = ybot; poly->xtop = xtop; poly->xbot = xbot;
  if (ybot > 192) poly->degenerate = true;
  poly->sort_key = (ybot << 8) | ytop;
  if (poly->translucent) poly->sort_key |= 0x10000;
  poly->wbuffer = flush_attr_ & 2;

  for (int i = 0; i < nverts; ++i) {
    const Vertex& vt = vram_[poly->vtx[i]];
    s32 w, wshifted;
    if (wsize < 16) { w = vt.pos[3] << (16 - wsize); wshifted = w >> (16 - wsize); }
    else { w = vt.pos[3] >> (wsize - 16); wshifted = w << (wsize - 16); }
    s32 z;
    if (flush_attr_ & 2) z = wshifted;
    else if (vt.pos[3]) z = static_cast<s32>((div_s64(static_cast<s64>(vt.pos[2]) * 0x4000, vt.pos[3]) + 0x3FFF) * 0x200);
    else z = 0x7FFE00;
    if (z < 0) z = 0; else if (z > 0xFFFFFF) z = 0xFFFFFF;
    poly->z[i] = z; poly->w[i] = w;
  }
  last_strip_poly_ = poly_mode_ >= 2 ? poly : nullptr;
}

void Gpu3D::calculate_lighting() {
  if ((texparam_ >> 30) == 2) {
    texcoords_[0] = static_cast<s16>(raw_texcoords_[0] + ((static_cast<s64>(normal_[0]) * tex_[0] + static_cast<s64>(normal_[1]) * tex_[4] + static_cast<s64>(normal_[2]) * tex_[8]) >> 21));
    texcoords_[1] = static_cast<s16>(raw_texcoords_[1] + ((static_cast<s64>(normal_[0]) * tex_[1] + static_cast<s64>(normal_[1]) * tex_[5] + static_cast<s64>(normal_[2]) * tex_[9]) >> 21));
  }
  // Normal through the vector matrix, kept as 1.10 signed.
  s32 n[3];
  for (int c = 0; c < 3; ++c)
    n[c] = static_cast<s32>(static_cast<u32>(normal_[0] * vec_[c] + normal_[1] * vec_[4 + c] + normal_[2] * vec_[8 + c]) << 9) >> 21;

  s32 count = 0;
  u32 acc[3] = {static_cast<u32>(mat_emission_[0]) << 14, static_cast<u32>(mat_emission_[1]) << 14, static_cast<u32>(mat_emission_[2]) << 14};
  for (int i = 0; i < 4; ++i) {
    if (!(cur_polygon_attr_ & (1 << i))) continue;
    s32 dot = ((light_dir_[i][0] * n[0]) >> 9) + ((light_dir_[i][1] * n[1]) >> 9) + ((light_dir_[i][2] * n[2]) >> 9);
    s32 shine;
    if (dot > 0) {
      // Diffuse: dot as signed 11-bit, products truncated to 20 bits.
      const s32 diffdot = static_cast<s32>(static_cast<u32>(dot) << 21) >> 21;
      for (int c = 0; c < 3; ++c) acc[c] += static_cast<u32>(mat_diffuse_[c] * light_color_[i][c] * diffdot) & 0xFFFFF;
      // Specular: half-vector approximation reusing the dot product.
      dot += n[2];
      dot = static_cast<s32>(static_cast<u32>(dot) << 21) >> 21;
      dot = ((dot * dot) >> 10) & 0x3FF;
      shine = ((dot * spec_recip_[i]) >> 8) - (1 << 9);
      if (shine < 0) shine = 0;
      else {
        shine = static_cast<s32>(static_cast<u32>(shine) << 18) >> 18;
        if (shine < 0) shine = 0; else if (shine > 0x1FF) shine = 0x1FF;
      }
    } else shine = 0;
    if (use_shininess_) { shine >>= 2; shine = shininess_[shine]; shine <<= 1; }
    for (int c = 0; c < 3; ++c) acc[c] += ((mat_specular_[c] * shine) + (mat_ambient_[c] << 9)) * light_color_[i][c];
    ++count;
  }
  for (int c = 0; c < 3; ++c) vertex_color_[c] = (acc[c] >> 14) > 31 ? 31 : static_cast<u8>(acc[c] >> 14);
  if (count < 1) count = 1;
}

void Gpu3D::box_test(const u32* params) {
  box_result_ = 0;
  const s16 x0 = static_cast<s16>(params[0] & 0xFFFF), y0 = static_cast<s16>(static_cast<s32>(params[0]) >> 16);
  const s16 z0 = static_cast<s16>(params[1] & 0xFFFF);
  const s16 x1 = static_cast<s16>(x0 + static_cast<s16>(static_cast<s32>(params[1]) >> 16));
  const s16 y1 = static_cast<s16>(y0 + static_cast<s16>(params[2] & 0xFFFF));
  const s16 z1 = static_cast<s16>(z0 + static_cast<s16>(static_cast<s32>(params[2]) >> 16));
  Vertex cube[8] = {};
  const s16 corners[8][3] = {{x0, y0, z0}, {x1, y0, z0}, {x1, y1, z0}, {x0, y1, z0}, {x0, y1, z1}, {x0, y0, z1}, {x1, y0, z1}, {x1, y1, z1}};
  update_clip_matrix();
  for (int i = 0; i < 8; ++i) {
    const s64 x = corners[i][0], y = corners[i][1], z = corners[i][2];
    for (int c = 0; c < 4; ++c)
      cube[i].pos[c] = static_cast<s32>((x * clip_[c] + y * clip_[4 + c] + z * clip_[8 + c] + static_cast<s64>(0x1000) * clip_[12 + c]) >> 12);
  }
  static const int faces[6][4] = {{0, 1, 2, 3}, {4, 5, 6, 7}, {0, 3, 4, 5}, {1, 2, 7, 6}, {0, 1, 6, 5}, {2, 3, 4, 7}};
  for (const auto& f : faces) {
    Vertex face[10];
    for (int i = 0; i < 4; ++i) face[i] = cube[f[i]];
    if (clip_polygon<false>(face, 4, 0, cur_polygon_attr_ & (1 << 12)) > 0) { box_result_ = 1u << 1; return; }
  }
}

void Gpu3D::pos_test() {
  const s64 v[4] = {cur_vertex_[0], cur_vertex_[1], cur_vertex_[2], 0x1000};
  update_clip_matrix();
  for (int c = 0; c < 4; ++c)
    pos_test_[c] = static_cast<s32>((v[0] * clip_[c] + v[1] * clip_[4 + c] + v[2] * clip_[8 + c] + v[3] * clip_[12 + c]) >> 12);
}

void Gpu3D::vec_test(u32 param) {
  const s16 n[3] = {sext10(param & 0x3FF), sext10((param >> 10) & 0x3FF), sext10((param >> 20) & 0x3FF)};
  for (int c = 0; c < 3; ++c) {
    vec_test_[c] = static_cast<s16>((n[0] * vec_[c] + n[1] * vec_[4 + c] + n[2] * vec_[8 + c]) >> 9);
    if (vec_test_[c] & 0x1000) vec_test_[c] |= static_cast<s16>(0xF000);
  }
}

// ---- frame --------------------------------------------------------------------

void Gpu3D::finalise_list() {
    if (num_polygons_) {
      // Opaque polygons first, then translucent; each group stable-sorted by
      // sort_key unless the flush asked for manual translucent ordering.
      u32 io = 0, it = num_opaque_;
      const Polygon* pr = cur_pram();
      auto& rp = render_polys_[bank_];
      for (u32 i = 0; i < num_polygons_; ++i) { const Polygon* p = &pr[i]; if (p->translucent) rp[it++] = p; else rp[io++] = p; }
      std::stable_sort(rp.begin(), rp.begin() + ((flush_attr_ & 1) ? num_opaque_ : num_polygons_),
                       [](const Polygon* a, const Polygon* b) { return a->sort_key < b->sort_key; });
    }
    render_count_[bank_] = num_polygons_;
    ++swaps_;
    if (prof::enabled && list_unconsumed_) prof::add(prof::C_GX_LIST_DROPPED, 1);
    list_unconsumed_ = true;
    // A swap that resubmits the same geometry with the same render state
    // produces the same picture: keep the previous output.
    list_same_ = rendered_before_
      && num_polygons_ == prev_swap_polys_ && num_vertices_ == prev_swap_verts_
      && lists_equal(&pram_[bank_ * PRAM_BANK], &pram_[render_bank_ * PRAM_BANK], num_polygons_,
                     bank_ * VRAM_BANK, render_bank_ * VRAM_BANK, vram_.data());
    prev_swap_polys_ = num_polygons_; prev_swap_verts_ = num_vertices_; rendered_before_ = true;
    if (prof::enabled && list_same_) prof::add(prof::C_GX_LIST_SAME, 1);
    if (prof::enabled) {
      prof::add(prof::C_GX_SWAP, 1);
      prof::add(prof::C_GX_SWAP_POLYS, num_polygons_);
      prof::add(prof::C_GX_SWAP_VERTS, num_vertices_);
      if (num_polygons_ > prof::count(prof::C_GX_SWAP_MAXPOLYS))
        prof::add(prof::C_GX_SWAP_MAXPOLYS, num_polygons_ - prof::count(prof::C_GX_SWAP_MAXPOLYS));
      if (num_vertices_ > prof::count(prof::C_GX_SWAP_MAXVERTS))
        prof::add(prof::C_GX_SWAP_MAXVERTS, num_vertices_ - prof::count(prof::C_GX_SWAP_MAXVERTS));
      if (census_gx()) {
        const u64 h = census_list_hash(render_polys_[bank_].data(), render_count_[bank_], vram_.data());
        prof::census_same_list = census_have_prev_ && h == census_prev_hash_;
        if (prof::census_same_list) {
          prof::add(prof::C_GX_SWAP_SAME_CONTENT, 1);
          prof::add(prof::C_GX_SAME_POLYS, num_polygons_);
          prof::add(prof::C_GX_SAME_VERTS, num_vertices_);
        }
        census_prev_hash_ = h; census_have_prev_ = true;
        if (census_have_prev_counts_ && num_polygons_ == census_prev_polys_ && num_vertices_ == census_prev_verts_) {
          const u32 other = render_bank_;
          const CmpModel m = census_compare(&pram_[bank_ * PRAM_BANK], &pram_[other * PRAM_BANK], num_polygons_,
                                            bank_ * VRAM_BANK, other * VRAM_BANK, vram_.data(), vram_.data());
          prof::add(prof::C_GX_CMP_RUNS, 1);
          prof::add(prof::C_GX_CMP_FULL, m.full);
          prof::add(prof::C_GX_CMP_EARLY, m.early);
          // Split by outcome: identical is scanned in full, differing stops early.
          if (m.early == m.full) { prof::add(prof::C_GX_CMP_RUNS_SAME, 1); prof::add(prof::C_GX_CMP_FULL_SAME, m.full); }
          else { prof::add(prof::C_GX_CMP_RUNS_DIFF, 1); prof::add(prof::C_GX_CMP_FULL_DIFF, m.full); prof::add(prof::C_GX_CMP_EARLY_DIFF, m.early); }
        }
        census_prev_polys_ = num_polygons_; census_prev_verts_ = num_vertices_; census_have_prev_counts_ = true;
      }
    }
}

void Gpu3D::vblank() {
  static const bool debug_gx = std::getenv("DS_DEBUG_GX") != nullptr;
  if (debug_gx)
    std::fprintf(stderr, "[gx] frame %llu geom %d rend %d flush %u attr %u polys %u verts %u disp3dcnt %04x alpharef %u clear %08x/%08x fifo %u gxstat %08x ie %08x if %08x\n",
                 static_cast<unsigned long long>(nds_.frame_count), geometry_on_, rendering_on_, flush_request_, flush_attr_, num_polygons_, num_vertices_,
                 dispcnt_, alpha_ref_, clear_attr1_, clear_attr2_, cmd_n_, gxstat_, nds_.io.cpu_io[0].ie, nds_.io.cpu_io[0].if_);
  if (!geometry_on_) return;
  drain_all();
  // The raster of the frame being displayed may still be running: it reads
  // its own copy of the render state and a bank the swap below leaves alone.
  if (rendering_on_) {
    const bool same_disp  = rstate_.dispcnt == dispcnt_ && rstate_.alpha_ref == alpha_ref_;
    const bool same_clear = rstate_.clear_attr1 == clear_attr1_ && rstate_.clear_attr2 == clear_attr2_;
    const bool same_fog   = rstate_.fog_color == fog_color_ && rstate_.fog_offset == fog_offset_ * 0x200u
      && std::equal(fog_density_.begin(), fog_density_.end(), rstate_.fog_density.begin() + 1);
    const bool same_et    = rstate_.edge == edge_ && rstate_.toon == toon_;
    const bool same_regs  = same_disp && same_clear && same_fog && same_et;
    if (swapped_) {   // the list was finalised at the SWAP command
      render_identical_ = same_regs && list_same_;
    } else {
      render_identical_ = same_regs;
      if (prof::enabled) {
        prof::add(prof::C_GX_NOSWAP, 1);
        if (!render_identical_) {
          prof::add(prof::C_GX_NOSWAP_REGS_DIFFER, 1);
          if (!same_disp)  prof::add(prof::C_GX_RD_DISPCNT, 1);
          if (!same_clear) prof::add(prof::C_GX_RD_CLEAR, 1);
          if (!same_fog)   prof::add(prof::C_GX_RD_FOG, 1);
          if (!same_et)    prof::add(prof::C_GX_RD_EDGETOON, 1);
        }
      }
    }
    // A raster frameskip never produced the picture rstate_/list_same_
    // describe, so it must not count as identical.
    if (render_stale_) render_identical_ = false;
    rstate_.dispcnt = dispcnt_;
    rstate_.alpha_ref = alpha_ref_;
    rstate_.edge = edge_; rstate_.toon = toon_;
    rstate_.fog_color = fog_color_;
    rstate_.fog_offset = fog_offset_ * 0x200;
    rstate_.fog_shift = (dispcnt_ >> 8) & 0xF;
    rstate_.fog_density[0] = fog_density_[0];
    for (int i = 0; i < 32; ++i) rstate_.fog_density[i + 1] = fog_density_[i];
    rstate_.fog_density[33] = fog_density_[31];
    rstate_.clear_attr1 = clear_attr1_; rstate_.clear_attr2 = clear_attr2_;
  }
  swapped_ = false;
  if (swap_wait_) { swap_busy_until_ = nds_.sched.now() + 650; swap_wait_ = false; }
  // No lock strictly needed here, but taken anyway so every write to a bank
  // role goes through it.
  { std::lock_guard<std::mutex> lk(bank_mu_); pending_bank_ = render_bank_; }
}

void Gpu3D::render_frame() {
  if (list_unconsumed_) { if (prof::enabled) prof::add(prof::C_GX_LIST_CONSUMED, 1); list_unconsumed_ = false; }
  // The previous raster must finish before its bank is released: the next
  // SWAP may take any bank that is not raster/pending/render.
  renderer_.sync_all();
  { std::lock_guard<std::mutex> lk(bank_mu_); raster_bank_ = pending_bank_; }
  render_stale_ = false;
  renderer_.render(*this);
}

void Gpu3D::set_render_xpos(u16 value, u16 mask) {
  if (!render_on_.load(std::memory_order_relaxed)) return;
  render_xpos_ = (render_xpos_ & ~mask) | (value & mask & 0x1FF);
}

void Gpu3D::sync_raster() { renderer_.sync_all(); }

const u32* Gpu3D::line(const Renderer3D::FrameRef& f, u32 y) {
  renderer_.sync_line(f, static_cast<s32>(y));
  const u32* raw = f.line(y);
  const u32 xpos = render_xpos_;
  if (xpos == 0) return raw;
  u32* const scrolled = scrolled_;
  if (xpos & 0x100) {
    u32 i = 0, j = xpos;
    for (; j < 512; ++i, ++j) scrolled[i] = 0;
    for (j = 0; i < 256; ++i, ++j) scrolled[i] = raw[j];
  } else {
    u32 i = 0, j = xpos;
    for (; j < 256; ++i, ++j) scrolled[i] = raw[j];
    for (; i < 256; ++i) scrolled[i] = 0;
  }
  return scrolled;
}

// ---- geometry worker ----------------------------------------------------------

void Gpu3D::stack_reset() {
  // Applied at once, ahead of anything queued: the write does not drain first.
  stack_err_ = 0; proj_sp_ = 0; tex_sp_ = 0;
}

u32 Gpu3D::read(u32 addr, u32 width) {
  const u32 r = addr - 0x04000000;
  // Sync on observation: a poll needs to see the busy->idle edge, which
  // only advances by draining here.
  drain_all();
  if ((r & ~3u) == 0x600) {
    if (prof::enabled) {
      prof::add(prof::C_GX_READ, 1); prof::add(prof::C_GX_READ_GXSTAT, 1);
      if (swap_wait_) prof::add(prof::C_GX_READ_GXSTAT_BUSY, 1);
    }
    // Synthesised: FIFO always reads empty and under half full. The busy bit
    // is held from SWAP_BUFFERS until 650 cycles past the swap so a poll
    // sees a plausible busy->idle edge instead of an immediate idle.
    const u32 sp = stack_err_ | ((pos_sp_ & 0x1F) << 8) | ((proj_sp_ & 1) << 13);
    const u32 v = gxstat_ | (swap_wait_ || nds_.sched.now() < swap_busy_until_ ? (1u << 27) : 0)
                | box_result_ | sp | (1u << 25) | (1u << 26);
    return width == 32 ? v : width == 16 ? (v >> ((addr & 2) * 8)) & 0xFFFF : (v >> ((addr & 3) * 8)) & 0xFF;
  }
  if (width == 8) { const u32 v = read(addr & ~3u, 32); return (v >> ((addr & 3) * 8)) & 0xFF; }
  if (width == 16) { const u32 v = read(addr & ~3u, 32); return (v >> ((addr & 2) * 8)) & 0xFFFF; }
  prof::add(prof::C_GX_READ, 1);
  switch (r) {
  case 0x60: return dispcnt_;
  case 0x320: return 46;                         // RDLINES_COUNT: rendering keeps up
  case 0x604: return num_polygons_ | (num_vertices_ << 16);
  case 0x620: return static_cast<u32>(pos_test_[0]);
  case 0x624: return static_cast<u32>(pos_test_[1]);
  case 0x628: return static_cast<u32>(pos_test_[2]);
  case 0x62C: return static_cast<u32>(pos_test_[3]);
  case 0x630: return static_cast<u16>(vec_test_[0]) | (static_cast<u32>(static_cast<u16>(vec_test_[1])) << 16);
  case 0x634: return static_cast<u16>(vec_test_[2]);
  case 0x680: return static_cast<u32>(vec_[0]); case 0x684: return static_cast<u32>(vec_[1]); case 0x688: return static_cast<u32>(vec_[2]);
  case 0x68C: return static_cast<u32>(vec_[4]); case 0x690: return static_cast<u32>(vec_[5]); case 0x694: return static_cast<u32>(vec_[6]);
  case 0x698: return static_cast<u32>(vec_[8]); case 0x69C: return static_cast<u32>(vec_[9]); case 0x6A0: return static_cast<u32>(vec_[10]);
  default: break;
  }
  if (r >= 0x640 && r < 0x680) { update_clip_matrix(); return static_cast<u32>(clip_[(r & 0x3C) >> 2]); }
  return 0;
}

void Gpu3D::write(u32 addr, u32 width, u32 value) {
  const u32 r = addr - 0x04000000;
  // Command ports checked first: almost all 3D-frame writes go here.
  if (width == 32 && r - 0x400 < 0x1CC) {
    if (!geometry_on_) return;
    if (r < 0x440) gxfifo_write(value);
    else log_port(static_cast<u8>((r & 0x1FC) >> 2), value);
    return;
  }
  if (!rendering_on_ && r >= 0x320 && r < 0x400) return;
  if (!geometry_on_ && r >= 0x400 && r < 0x700) return;

  if (width == 8) {
    switch (r) {
    case 0x60:
      dispcnt_ = (dispcnt_ & 0xFF00) | (value & 0xFF);
      alpha_ref_ = (dispcnt_ & 4) ? alpha_ref_val_ : 0;
      return;
    case 0x61:   // bits 12/13 are sticky error flags, cleared by writing 1
      dispcnt_ = (dispcnt_ & 0x30FF) | ((value & 0x4F) << 8);
      if (value & 0x10) dispcnt_ &= ~(1u << 12);
      if (value & 0x20) dispcnt_ &= ~(1u << 13);
      return;
    case 0x62: case 0x63: return;
    case 0x340: alpha_ref_val_ = value & 0x1F; alpha_ref_ = (dispcnt_ & 4) ? alpha_ref_val_ : 0; return;
    case 0x601: if (value & 0x80) stack_reset(); return;
    case 0x603: gxstat_ = (gxstat_ & 0x3FFFFFFF) | ((value & 0xC0) << 24); check_fifo_irq(); return;
    default: break;
    }
    if (r >= 0x330 && r < 0x340) { const u32 i = (r - 0x330) >> 1; edge_[i] = (r & 1) ? static_cast<u16>((edge_[i] & 0x00FF) | (value << 8)) : static_cast<u16>((edge_[i] & 0xFF00) | (value & 0xFF)); return; }
    if (r >= 0x360 && r < 0x380) { fog_density_[r - 0x360] = value & 0x7F; return; }
    if (r >= 0x380 && r < 0x3C0) { const u32 i = (r - 0x380) >> 1; toon_[i] = (r & 1) ? static_cast<u16>((toon_[i] & 0x00FF) | (value << 8)) : static_cast<u16>((toon_[i] & 0xFF00) | (value & 0xFF)); return; }
    if (r >= 0x350 && r < 0x360) {               // clear attributes / fog colour / fog offset bytes
      const u32 cur = read(addr & ~1u, 16);
      write(addr & ~1u, 16, (addr & 1) ? ((cur & 0x00FF) | ((value & 0xFF) << 8)) : ((cur & 0xFF00) | (value & 0xFF)));
      return;
    }
    return;
  }

  if (width == 16) {
    switch (r) {
    case 0x60:
      dispcnt_ = (value & 0x4FFF) | (dispcnt_ & 0x3000);
      if (value & (1 << 12)) dispcnt_ &= ~(1u << 12);
      if (value & (1 << 13)) dispcnt_ &= ~(1u << 13);
      alpha_ref_ = (dispcnt_ & 4) ? alpha_ref_val_ : 0;
      return;
    case 0x62: return;
    case 0x340: alpha_ref_val_ = value & 0x1F; alpha_ref_ = (dispcnt_ & 4) ? alpha_ref_val_ : 0; return;
    case 0x350: clear_attr1_ = (clear_attr1_ & 0xFFFF0000) | (value & 0xFFFF); return;
    case 0x352: clear_attr1_ = (clear_attr1_ & 0x0000FFFF) | ((value & 0xFFFF) << 16); return;
    case 0x354: clear_attr2_ = (clear_attr2_ & 0xFFFF0000) | (value & 0xFFFF); return;
    case 0x356: clear_attr2_ = (clear_attr2_ & 0x0000FFFF) | ((value & 0xFFFF) << 16); return;
    case 0x358: fog_color_ = (fog_color_ & 0xFFFF0000) | (value & 0xFFFF); return;
    case 0x35A: fog_color_ = (fog_color_ & 0x0000FFFF) | ((value & 0xFFFF) << 16); return;
    case 0x35C: fog_offset_ = value & 0x7FFF; return;
    case 0x600: if (value & 0x8000) stack_reset(); return;
    case 0x602: gxstat_ = (gxstat_ & 0x3FFFFFFF) | ((value & 0xC000) << 16); check_fifo_irq(); return;
    case 0x610: zero_dot_w_limit_ = ((value & 0x7FFF) * 0x200) + 0x1FF; return;
    default: break;
    }
    if (r >= 0x330 && r < 0x340) { edge_[(r - 0x330) >> 1] = static_cast<u16>(value); return; }
    if (r >= 0x360 && r < 0x380) { fog_density_[r - 0x360] = value & 0x7F; fog_density_[r - 0x360 + 1] = (value >> 8) & 0x7F; return; }
    if (r >= 0x380 && r < 0x3C0) { toon_[(r - 0x380) >> 1] = static_cast<u16>(value); return; }
    return;
  }

  switch (r) {
  case 0x60:
    dispcnt_ = (value & 0x4FFF) | (dispcnt_ & 0x3000);
    if (value & (1 << 12)) dispcnt_ &= ~(1u << 12);
    if (value & (1 << 13)) dispcnt_ &= ~(1u << 13);
    alpha_ref_ = (dispcnt_ & 4) ? alpha_ref_val_ : 0;
    return;
  case 0x340: alpha_ref_val_ = value & 0x1F; alpha_ref_ = (dispcnt_ & 4) ? alpha_ref_val_ : 0; return;
  case 0x350: clear_attr1_ = value; return;
  case 0x354: clear_attr2_ = value; return;
  case 0x358: fog_color_ = value; return;
  case 0x35C: fog_offset_ = value & 0x7FFF; return;
  case 0x600:
    if (value & 0x8000) stack_reset();
    gxstat_ = (gxstat_ & 0x3FFFFFFF) | (value & 0xC0000000);
    check_fifo_irq();
    return;
  case 0x610: zero_dot_w_limit_ = ((value & 0x7FFF) * 0x200) + 0x1FF; return;
  default: break;
  }
  if (r >= 0x400 && r < 0x440) { gxfifo_write(value); return; }
  if (r >= 0x440 && r < 0x5CC) { log_port(static_cast<u8>((r & 0x1FC) >> 2), value); return; }
  if (r >= 0x330 && r < 0x340) { const u32 i = (r - 0x330) >> 1; edge_[i] = value & 0xFFFF; edge_[i + 1] = value >> 16; return; }
  if (r >= 0x360 && r < 0x380) { const u32 i = r - 0x360; for (int k = 0; k < 4; ++k) fog_density_[i + k] = (value >> (8 * k)) & 0x7F; return; }
  if (r >= 0x380 && r < 0x3C0) { const u32 i = (r - 0x380) >> 1; toon_[i] = value & 0xFFFF; toon_[i + 1] = value >> 16; return; }
}


namespace {
template <class S> void sync_vertex(S& s, Vertex& v) { s.fields(v.pos, v.col, v.tex, v.clipped, v.sx, v.sy, v.fcol); }
template <class S> void sync_polygon(S& s, Polygon& p) {
  s.fields(p.vtx, p.nverts, p.z, p.w, p.wbuffer, p.attr, p.texparam, p.texpal, p.degenerate, p.facing, p.translucent, p.shadow_mask, p.shadow,
           p.vtop, p.vbot, p.ytop, p.ybot, p.xtop, p.xbot, p.sort_key);
}
} // namespace

template <class S> void Gpu3D::sync_state(S& s) {
  if constexpr (!S::reading) drain_all();   // the log does not travel; it is replayed out first
  // On disk the overflow flag is GXSTAT bit 15 and the box result bit 1.
  if constexpr (!S::reading) gxstat_ |= stack_err_ | box_result_;
  s.begin("GX3D");
  // Only a half-assembled command remains after drain_all: its parameters
  // (inflight_n_ of them, at most 32) and the parser state to finish it.
  u32 inflight = inflight_n_;
  s.put(inflight);
  if constexpr (S::reading) { if (inflight > 32) { s.fail("gx in-flight parameters"); return; } inflight_n_ = inflight; cmd_n_ = par_n_ = 0; }
  for (u32 i = 0; i < inflight; ++i) s.put(par_log_[i]);
  s.fields(inflight_cmd_, parse_.num_cmds, parse_.cur_cmd, parse_.param_count, parse_.total_params,
           gxstat_, geometry_on_, rendering_on_, dispcnt_, alpha_ref_val_, alpha_ref_, toon_, edge_, fog_color_, fog_offset_, fog_density_,
           clear_attr1_, clear_attr2_, zero_dot_w_limit_,
           rstate_.dispcnt, rstate_.alpha_ref, rstate_.toon, rstate_.edge, rstate_.fog_color, rstate_.fog_offset, rstate_.fog_shift, rstate_.fog_density,
           rstate_.clear_attr1, rstate_.clear_attr2, render_xpos_,
           matrix_mode_, proj_, pos_, vec_, tex_, clip_, clip_dirty_, proj_stack_, tex_stack_, pos_stack_, vec_stack_, proj_sp_, pos_sp_, tex_sp_, viewport_,
           poly_mode_, cur_vertex_, vertex_color_, texcoords_, raw_texcoords_, normal_, light_dir_, spec_recip_, light_color_,
           mat_diffuse_, mat_ambient_, mat_specular_, mat_emission_, use_shininess_, shininess_,
           polygon_attr_, cur_polygon_attr_, texparam_, texpal_, pos_test_, vec_test_);
  // temp_vtx_ travels in position order, so the slot permutation never reaches
  // the file.
  if constexpr (S::reading) reset_vptr();
  else normalise_temp_vtx();
  for (Vertex& v : temp_vtx_) sync_vertex(s, v);
  // Outcode is derived from pos; rebuilt on load rather than stored.
  if constexpr (S::reading) for (Vertex& v : temp_vtx_) v.oc = outcode(v.pos);
  // On disk, polygon RAM is two slots: slot 0 the bank being written, slot 1
  // the finalised list. The third bank is not saved; a file always loads
  // back as bank 0 written, bank 1 finalised.
  u32 slot_of[BANKS]; for (u32 b = 0; b < BANKS; ++b) slot_of[b] = b == bank_ ? 0 : b == render_bank_ ? 1 : 2;
  auto remap = [&](u32 idx, u32 per) -> u32 { return S::reading ? idx : slot_of[idx / per] * per + idx % per; };
  u32 bank_disk = 0;
  u32 rcount = S::reading ? 0 : render_count_[render_bank_];
  s.fields(vertex_num_, vertex_in_poly_, consecutive_polys_, num_opaque_, bank_disk, num_vertices_, num_polygons_,
           rcount, render_identical_, flush_request_, flush_attr_, prev_swap_polys_, prev_swap_verts_, rendered_before_);
  if constexpr (S::reading) { bank_ = bank_disk & 1; render_bank_ = bank_ ^ 1; raster_bank_ = pending_bank_ = render_bank_; }
  const Polygon** rlist = render_polys_[render_bank_].data();
  // Pointers into polygon RAM travel as indices.
  s32 strip = last_strip_poly_ ? static_cast<s32>(remap(static_cast<u32>(last_strip_poly_ - pram_.data()), PRAM_BANK)) : -1;
  s.put(strip);
  if constexpr (S::reading) last_strip_poly_ = strip >= 0 && strip < static_cast<s32>(PRAM_BANK * 2) ? &pram_[static_cast<size_t>(strip)] : nullptr;
  if constexpr (S::reading) { if (rcount > PRAM_BANK) { s.fail("render list"); return; } render_count_[render_bank_] = rcount; }
  for (u32 i = 0; i < rcount; ++i) {
    u16 k = static_cast<u16>(S::reading ? 0 : remap(static_cast<u32>(rlist[i] - pram_.data()), PRAM_BANK));
    s.put(k);
    if constexpr (S::reading) rlist[i] = &pram_[k & (PRAM_BANK * 2 - 1)];
  }
  // Vertex/polygon RAM: current bank up to its counts, finalised bank up to
  // what the last swap left there or the render list references.
  u32 nv[2] = {0, 0}, np[2] = {0, 0};
  if constexpr (!S::reading) {
    nv[0] = num_vertices_; np[0] = num_polygons_;
    nv[1] = std::min(prev_swap_verts_, VRAM_BANK); np[1] = std::min(prev_swap_polys_, PRAM_BANK);
    for (u32 i = 0; i < rcount; ++i) {
      const u32 k = remap(static_cast<u32>(rlist[i] - pram_.data()), PRAM_BANK);
      if (k < PRAM_BANK * 2) np[k / PRAM_BANK] = std::max(np[k / PRAM_BANK], k % PRAM_BANK + 1);
      const Polygon& p = *rlist[i];
      for (u32 j = 0; j < p.nverts && j < 10; ++j) { const u32 vi = remap(p.vtx[j], VRAM_BANK); if (vi < VRAM_BANK * 2) nv[vi / VRAM_BANK] = std::max(nv[vi / VRAM_BANK], vi % VRAM_BANK + 1); }
    }
  }
  s.fields(nv, np);
  for (u32 slot = 0; slot < 2; ++slot) {
    const u32 b = S::reading ? slot : (slot == 0 ? bank_ : render_bank_);
    if constexpr (S::reading) { if (nv[slot] > VRAM_BANK || np[slot] > PRAM_BANK) { s.fail("polygon RAM counts"); return; } }
    for (u32 i = 0; i < nv[slot]; ++i) sync_vertex(s, vram_[b * VRAM_BANK + i]);
    for (u32 i = 0; i < np[slot]; ++i) {
      if constexpr (S::reading) sync_polygon(s, pram_[b * PRAM_BANK + i]);
      else { Polygon copy = pram_[b * PRAM_BANK + i]; for (u32 j = 0; j < 10; ++j) copy.vtx[j] = static_cast<u16>(remap(copy.vtx[j], VRAM_BANK)); sync_polygon(s, copy); }
    }
  }
  s.end();
  renderer_.sync_output(s);
  stack_err_ = gxstat_ & 0x8000u; box_result_ = gxstat_ & 2u; gxstat_ &= ~0x8002u;
}
template void Gpu3D::sync_state<state::Writer>(state::Writer&);
template void Gpu3D::sync_state<state::Reader>(state::Reader&);

} // namespace ds::gpu
