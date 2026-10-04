// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include <functional>
#include <cstdio>
#include "core/types.h"
#include "core/gpu/texcache.h"
#include "core/gpu/vk/vk_layout.h"
#include <unordered_map>

#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <memory>
#include <string>
#include <vector>

namespace ds { struct NDS; }

namespace ds::gpu {
namespace vk { class Device; class Lean; }

class Gpu3D;
struct Polygon;
struct Vertex;
struct RenderState;
class VramMap;
struct VramView;

// Software rasteriser for the 3D engine's polygon list: hardware fixed-point
// edge stepping, two-stage perspective-correct interpolation, its fill rules,
// a two-deep AA pixel stack, and the final pass (edge marking, fog, AA blend).
//
// Working buffers are a ring of 258-pixel lines (1px border each side for
// edge marking); line y renders, then the final pass of y-1 reads y-2..y and
// copies it out. Render registers latch at swap (Gpu3D::vblank); the raster
// works from a copy (rs_frame_).
struct RenderState {
  u32 dispcnt = 0;
  u8  alpha_ref = 0;
  std::array<u16, 32> toon{};
  std::array<u16, 8> edge{};
  u32 fog_color = 0, fog_offset = 0, fog_shift = 0;
  std::array<u8, 34> fog_density{};
  u32 clear_attr1 = 0x3F000000, clear_attr2 = 0x00007FFF;
};

class Renderer3D {
public:
  explicit Renderer3D(NDS& nds);
  ~Renderer3D();
  void reset();
  // Save states: rendered output only (loading resets everything else, incl. texture cache).
  template <class S> void sync_output(S& s);

  // Rasterise the frame latched by the geometry engine.
  void render(const Gpu3D& gx);

  // Output lines are RGB666 in bits 0-21, 5-bit alpha in bits 24-28, read
  // through a FrameRef (below); double-buffered.

  // Portable pixel-pipeline pieces, exposed for the unit tests.
  static u32 alpha_blend(u32 dispcnt, u32 src, u32 dst, u32 alpha);

  // Anti-aliasing (DISP3DCNT bit 4). Off clears the bit frame-wide: no
  // coverage, pixel-stack push, under-layer depth test, or AA blend.
  // video.aa: the hardware's edge blend on the CPU raster (when the game asks
  // for it), 4x MSAA on the GPU raster. Coordinator only.
  void set_aa(bool on);
  bool aa() const { return aa_; }
  // --dump-gpu-frame: write NDS frame `frame`'s polygon list in the GPU raster's layout
  // (vk_dump.h) to `path`; the frame is drawn on the CPU as usual.
  void set_gpu_dump(const char* path, u64 frame) { gpu_dump_path_ = path; gpu_dump_frame_ = frame; }
  // The GPU 3D raster (vk_lean.h): the frame's polygon list drawn on the GPU
  // at 1x with MSAA, one frame of lag allowed under load; a refused frame
  // (texture arena full, list past the buffers) goes to the band workers.
  // May decline (no Vulkan); `why` takes the reason. Coordinator only.
  bool set_gpu(bool on, std::string* why = nullptr);
  bool gpu_active() const { return gpu_on_; }
private:
  NDS& nds_;
  // Holds a whole chunk of scanlines at once (render_chunk draws chunk at a
  // time): CHUNK lines plus a border line each side, power-of-two for masking.
  static constexpr int W = 258, RING = 8, CHUNK = RING - 2, RSIZE = W * RING;
  static_assert((RING & (RING - 1)) == 0, "RING must be a power of two");
  static_assert(CHUNK >= 2, "CHUNK must leave room for the final pass lag");
  // Ring row of frame line y (-1, 192 are border rows); pixel (x,y) is row_of(y)+1+x.
  static constexpr u32 row_of(s32 y) { return static_cast<u32>((y + 1) & (RING - 1)) * W; }

  // Colour: R 0-5, G 8-13, B 16-21, A 24-28.
  // Attr: bits 0-3 edge flags (L/R/T/B), bit 4 back-facing, bits 8-12 AA
  // coverage, bit 15 fog, bits 16-21 translucent polygon id, bit 22
  // translucent, bits 24-29 opaque polygon id.
  // Second half of each buffer: pixel underneath (AA). +8 words slack for
  // 8-lane resolve groups starting near the last ring row's end.
  std::array<u32, RSIZE * 2 + 8> color_{}, depth_{}, attr_{};
  // Finished lines, two frames' worth; render() draws into the buffer the
  // display isn't reading (display_ ^ 1), letting compositing of frame N
  // continue past line 215 of frame N+1.
  std::array<u32, 256 * 192> out_[2]{};
  u32 display_ = 0;
  std::array<u8, 256 * RING> stencil_{};   // one row per ring line
  // Per ring line: was the previous polygon drawn on it a shadow mask
  // (decides clear vs. add to the stencil row). Per-line: render_chunk draws
  // a polygon's whole run before the next.
  std::array<bool, RING> prev_shadow_mask_{};

  // Perspective-correct interpolation factor between two endpoints.
  template <int dir> struct Interp {
    s32 x0 = 0, x1 = 0, xdiff = 0, x = 0;
    int shift = 0; bool linear = false, wbuffer = false;
    s32 xrecip_z = 0; s32 w0n = 0, w0d = 0, w1d = 0; u32 yfactor = 0;
    u32 recip = 0;   // ceil(2^32 / xdiff) for xdiff >= 2: exact linear division by one multiply and a fix-up
    void setup(s32 x0_, s32 x1_, s32 w0, s32 w1, bool wbuf);
    void set_x(s32 xv);
    s32 interpolate(s32 y0, s32 y1) const;
    s32 interpolate_z(s32 z0, s32 z1) const;
  };

  // Everything the per-pixel work needs from the polygon and the render
  // state, decoded once per polygon.
  struct SpanBuf;
public:
  struct Shade;
private:
  struct SpanJob;
  // Batch resolve kernel; prologue/Shade-derived setup paid once per batch, not per span.
  using ResolveFn = void (Renderer3D::*)(const Shade&, const SpanJob*, u32);
public:
  // Everything a span needs from its polygon, decoded once (public: the NEON gather helpers take it).
  struct Shade {
    u32 blendmode, polyalpha, polyattr;
    bool highlight, textured, wireframe, shadow, polyattr_z;   // polyattr_z: translucent pixels update depth
    u32 dispcnt, alpha_ref;
    const u16* toon;
    // Texture: format, VRAM base, size, wrap/flip, transparent-colour-0 alpha, palette base.
    u32 fmt, base, texpal, alpha0;
    s32 width, height;
    bool srep, sflip, trep, tflip;
    // Direct host pointers when texture/palette lie in directly mapped, host-contiguous VRAM (else null, go through views).
    const u8* tex_ptr;
    const u16* pal_ptr;
    // Views for formats that address VRAM per texel (the compressed one).
    const VramView* texv; const VramView* palv; const VramMap* vm;
    const u32* texels;   // decoded texels (colour16 | alpha<<16), or null when cache is off
    // NEON: four-texel gather specialised for (format, S wrap, T wrap), or null for the per-lane sampler.
    const void* gather4;
    ResolveFn resolve;   // bound once here rather than re-derived per flush
    int mode;      // pick_depth_mode
    bool vec;      // NEON resolve applies (no shadow / wireframe / blend 2)
    // True when every alpha this polygon can emit is 0 or 31 (0-alpha lanes
    // never reach resolve), so translucent handling compiles out.
    bool opaque;
    // Edges all fill under AA, edge marking, blended translucency or wireframe; fixed per polygon.
    bool always_fill;
    bool attrs_constant, rgb_constant;   // uniform across the polygon; checked once
  };
private:

  // One polygon edge walked down the scanlines.
  template <int side> struct Slope {
    s32 increment = 0; bool negative = false, xmajor = false;
    Interp<1> interp;
    s32 x0 = 0, xmin = 0, xmax = 0, xlen = 0, ylen = 0, dx = 0, y = 0, xcov_incr = 0;
    s32 setup_dummy(s32 x0_, bool wbuf);
    s32 setup(s32 x0_, s32 x1_, s32 y0, s32 y1, s32 w0, s32 w1, s32 y_, bool wbuf);
    s32 step();
    s32 xval() const;
    // `aa` off: coverage is 0 and unread (resolve's AA path is off too).
    template <bool swapped> void edge_params(bool aa, s32* length, s32* coverage) const;
  };

  struct Edge {
    const Polygon* poly;
    Slope<0> left; Slope<1> right;
    s32 xl, xr;
    u32 cur_vl, cur_vr, next_vl, next_vr;
    Shade sh;
    // Cached per-scanline state, valid until refresh_edge_state recomputes it
    // at vertex boundaries (Slope::step only advances dx/y).
    const Vertex *vcl, *vnl, *vcr, *vnr;   // vertex(vtx[cur_vl]) and friends
    s32 wcl, wnl, wcr, wnr;                // p.w[cur_vl] and friends
    s32 zcl, znl, zcr, znr;                // p.z[cur_vl] and friends
    bool nx_l, nx_r;        // negative || !xmajor
    bool px_l, px_r;        // !negative && xmajor
    bool lneg_xm;           // left.negative && left.xmajor
    bool lxm, rxm;          // xmajor
    bool same_incr;         // left.increment == right.increment
    bool l_incr0, r_incr0;  // increment == 0
    bool next_sx_differ;    // vnl->sx != vnr->sx (symmetric, so swap-safe)
  };
  std::array<Edge, 2048> edges_{};
  // Built lazily (bins each draw a fraction of lines; setup_polygon for
  // everything would be wasted work for untouched bins). build_edges records
  // polygon + list index; built_edge does the rest.
  std::array<u16, 2048> edge_list_{};    // edge index -> polygon list index
  std::array<u64, 32> edge_built_{};     // one bit per edge
  bool edge_is_built(u32 i) const { return (edge_built_[i >> 6] >> (i & 63)) & 1; }
  Edge& built_edge(u32 i) {
    if (!edge_is_built(i)) { edge_built_[i >> 6] |= u64{1} << (i & 63); setup_poly_ = edge_list_[i]; setup_polygon(edges_[i], *edges_[i].poly); }
    return edges_[i];
  }
  // Per-line active set: polygons enter at their top line (bucketed by ytop),
  // leave after their last; kept in list order (blending rules depend on it).
  std::array<u16, 2048> order_{};        // edge indices sorted by ytop, stable
  std::array<u16, 194> bucket_{};        // order_ offset where ytop == y starts (193 = end)
  std::array<u16, 2048> active_buf_[2]{};
  std::array<u16, 2048> enter_{};   // chunk's entering polygons, re-sorted into list order
  std::array<u64, 32> enter_bits_{};   // dense bin-local membership, avoids an O(n log n) sort
  u16* active_ = nullptr; u16* active_next_ = nullptr;
  u32 active_count_ = 0;
  std::array<bool, 192> line_touched_{};   // a polygon was active on the line (final pass needed)

  const Gpu3D* gx_ = nullptr;
  const RenderState* rs_ = nullptr;
  bool game_aa_ = false;                  // AA blend runs (game asked and aa_ allows)
  bool aa_ = true, aa_rendered_ = true;   // aa_rendered_: the setting the kept frame was drawn with
  // rs_->dispcnt with bit 4 cleared when AA is off: kills the whole under
  // layer for the frame (AA blend, under-layer depth test, translucent plot,
  // fog, shadow stencil bit 2's under-layer writes).
  u32 dispcnt_ = 0;
  alignas(16) u8 toon6_[3][32] = {};   // toon table as 3 6-bit byte planes, expanded once per frame (vector toon/highlight)
  alignas(16) u8 edge6_[3][16] = {};   // edge-marking colours as 3 6-bit byte planes (final_pass)
  void expand_toon();
  const VramMap* vm_ = nullptr;
  mutable TextureCache texcache_;
  bool rendered_once_ = false;   // the colour buffer holds a rendered frame
  const VramView* texv_ = nullptr;
  const VramView* palv_ = nullptr;

  u8  tex8(u32 addr) const;
  u16 tex16(u32 addr) const;
  u16 pal16(u32 addr) const;
  // A batch is up to BATCH_PX pixels plus one more span (may be a full
  // scanline) always stageable before flush, so buffers hold both plus vector slack.
  static constexpr u32 BATCH_PX = 256;
  static constexpr u32 BATCH_CAP = BATCH_PX + 256 + 16;
  struct SpanBuf {
    s32 x0;
    // +16 slack: stages round span length up to vector width and write whole vectors.
    alignas(16) u32 fac[BATCH_CAP];
    alignas(16) s32 z[BATCH_CAP];
    // Colour as 6-bit channel, texcoords as s16 (a third the bytes, 8 pixels/vector not 4).
    alignas(16) u8  vr[BATCH_CAP], vg[BATCH_CAP], vb[BATCH_CAP];
    alignas(16) s16 sc[BATCH_CAP], tc[BATCH_CAP];
    // Bit 0: top layer, bit 1: pixel underneath, can still draw. Set by
    // depth_candidates from depth alone; span_shade then clears alpha-killed
    // lanes, so by resolve it means "will draw" and a zero group of 8 is skipped.
    alignas(16) u8 pass[BATCH_CAP];
    alignas(16) u32 tcol[BATCH_CAP];     // texels for the span (textured polygons), colour15 and
    alignas(16) u32 talp[BATCH_CAP];     // 5-bit alpha, gathered once per span
    alignas(16) u32 col[BATCH_CAP];      // shaded pixel records (18-bit colour, alpha 24-28)
  };

  SpanBuf spanbuf_;   // one per renderer; staged across calls before pixel stages run over it once
  // One staged span; resolve's remaining per-span needs: scanline, candidate
  // range, position in batch buffers, three-part edge/fill decisions.
  struct SpanJob {
    s32 y, ca, cb; u32 off;
    s32 xdraw, lim0, lim1, lim2;
    s32 l_cov, r_cov;
    int yedge;
    bool l_fill, r_fill, wf_skip;
  };
  std::array<SpanJob, 256> jobs_{};
  u32 njobs_ = 0, batch_px_ = 0;

  // One scanline's geometry/endpoint attributes, a pure function of polygon and y.
  // precompute_lines derives a whole run at once, one loop instead of a fresh
  // derivation per scanline.
  struct LineSpan {
    s32 xstart, xend;        // span endpoints, the right-edge push-left applied
    s32 wl, wr, zl, zr;      // endpoint w / z, swapped-edge order applied
    s32 al[5], ar[5];        // endpoint r g b s t, swapped-edge order applied
    s32 l_len, r_len, l_cov, r_cov;
    s32 xa, xb;              // the clipped screen range [xa, xb)
    int yedge;
    bool l_fill, r_fill, wf_skip;
  };
  std::array<LineSpan, CHUNK> lines_{};
  // Fills lines_[0..y1-y0) for scanlines [y0,y1); leaves edge cursors stepped
  // past y1-1 as the per-scanline path would.
  void precompute_lines(Edge& e, s32 y0, s32 y1);
#if DSPERATE_NEON && defined(__arm__)
  static bool edge_values_vec(const Interp<1>& in, s32 w0, s32 w1, const Vertex& vc, const Vertex& vn, s32* w, s32* a);
#endif
  void stage_line(Edge& e, s32 y, const LineSpan& ls);   // depth pre-pass, attribute staging, batch job
  u32  texture_sample(const Shade& sh, s32 s, s32 t, u32* alpha) const;
  template <bool textured> u32 shade_pixel(const Shade& sh, u32 vr, u32 vg, u32 vb, s32 s, s32 t) const;
  void plot_translucent(u32 addr, u32 color, u32 z, u32 polyattr, bool shadow);
  template <int mode> bool depth_pass(u32 addr, s32 z, u32 dstattr) const;
  // One contiguous range of a span; called only from batch kernels and the
  // vector kernel's scalar fallback so it inlines into them.
  template <int mode, bool textured, bool aa, bool shadow> void resolve_span(const Shade& sh, const SpanBuf& sb, s32 y, s32 xa, s32 xb, int part, int edge, s32 l_cov, s32 r_cov, s32& xcov);
  template <int mode, bool textured, bool aa, bool shadow> void resolve_batch(const Shade& sh, const SpanJob* jobs, u32 n);
#if DSPERATE_NEON
  // Four pixels per step, same results as resolve_span; polygons without shadow/wireframe/toon only.
  template <int mode, bool textured, bool aa, bool opq> [[gnu::always_inline]] inline void resolve_span_vec(const Shade& sh, const SpanBuf& sb, s32 y, s32 xa, s32 xb, int part, int edge, s32 l_cov, s32 r_cov, s32& xcov);
  template <int mode, bool textured, bool aa, bool opq> void resolve_batch_vec(const Shade& sh, const SpanJob* jobs, u32 n);
  // DS_PROFILE census: per 8-pixel group, is the resolve kind code uniform among drawing lanes?
  void rk_census(u64 kinds, u64 p8);
  u32 rk_or_ = 0, rk_groups_ = 0;
  bool rk_mixed_ = false;
  void texture_gather4(const Shade& sh, const s16* sa, const s16* ta, u32* colour, u32* alpha) const;
  void span_texels(const Shade& sh, SpanBuf& sb, s32 ca, s32 cb) const;   // gathers a span's texels once, ahead of resolve
  template <bool textured> void span_shade(const Shade& sh, SpanBuf& sb, s32 ca, s32 cb) const;   // shaded colours, blend mode decided once
  static const void* select_gather4(const Shade& sh);
#endif
  static u32 fac_bound(s32 xdiff, s32 wl, s32 wr);
  // Whether the perspective factor was staged for the whole span (w-buffered depth only); span_attrs' fac_ready.
  bool span_stage(SpanBuf& sb, s32 xstart, s32 xend, s32 xa, s32 xb, s32 wl, s32 wr, s32 zl, s32 zr, bool wbuffer,
                  const s32* al, const s32* ar, bool with_attrs, u32 off) const;
  void span_attrs(SpanBuf& sb, s32 xstart, s32 xend, s32 ca, s32 cb, s32 wl, s32 wr, const s32* al, const s32* ar, bool attrs_constant, bool rgb_constant, bool fac_ready) const;
  void setup_left_edge(Edge& e, s32 y) const;
  void setup_right_edge(Edge& e, s32 y) const;
  void setup_polygon(Edge& e, const Polygon& p);
  void rewind_edge(Edge& e);
  void refresh_edge_state(Edge& e) const;   // called from both edge setups and once per polygon for the flat case
  void setup_shade(Shade& sh, const Polygon& p);
  // Texture part of setup_shade; true when the polygon reads the decoded
  // cache (resolved on the emulation thread only, see render()).
  bool texture_fields(Shade& sh, const Polygon& p) const;
  static ResolveFn select_resolve(const Shade& sh);   // dispatch tables live with flush_batch in render3d.cpp
  // One span's three-part edge/fill walk (left run, interior, right run),
  // clipped to what the depth pre-pass left alive. `range` draws one clipped
  // run; batch kernels pass their own inlined body.
  // always_inline is load-bearing: GCC otherwise emits a real call, paid per span.
  template <typename Range> [[gnu::always_inline]] inline void walk_span(const SpanJob& j, Range&& range);
  void render_shadow_mask_line(Edge& e, s32 y);
  void flush_batch(const Shade& sh);
  // Rasterises [ya, yb) polygon at a time, not line at a time: active set
  // merged once per chunk, each polygon draws its whole covered run before
  // the next starts. Per-pixel order unchanged (list order), so blend/stencil
  // rules still hold.
  void render_chunk(s32 ya, s32 yb);

  // ---- banded parallel rasterising ---------------------------------------
  // Screen split into horizontal bands, one worker each, sharing nothing
  // mutable but the texture cache (resolved to plain pointers before any
  // worker starts). Band boundaries are fixed per band count, so output does
  // not depend on thread scheduling. A band rasterises one line above the
  // range it emits, since the final pass reads two neighbours.
  void build_edges();   // from list_polys_ / list_count_, latched by render() or prepare_worker
  void seed_active(s32 y);
  void render_band(s32 y0, s32 y1, u32* dst);
  void prepare_worker(const Gpu3D& gx, const Polygon* const* polys, u32 npoly, const std::vector<const u32*>* texels, const RenderState* rs);
  RenderState rs_frame_;   // coordinator's copy; the engine's own may be rewritten at the next VBlank mid-raster
  static u32 band_count(u32 polygons);
  // More bins than workers, each worker takes the next unclaimed one, so a
  // heavy bin is absorbed by others finishing early (a static split can't).
  static constexpr u32 MAX_BINS = 32;
  void compute_bins(u32 nbins, u32 workers);
  static u32 bin_count(u32 workers);
  std::array<s32, MAX_BINS + 1> bin_y_{};
  u32 nbins_ = 0;

public:
  // Raster runs on the workers while the emulation thread carries on.
  // sync_line waits for the band owning one display line; sync_all waits for
  // all, the escape hatch for anything that would change what workers read.
  //
  // What the display reads for one frame: output buffer, and the pool
  // generation's bands to wait on before reading a line. Taken at the start
  // of the display frame (Gpu::begin_frame); nbins is 0 when nothing is
  // outstanding (rendered inline or kept).
  struct FrameRef {
    // A GPU frame's record is picked on the first line read, not when the
    // ref is taken at the start of the display frame: the GPU gets the lines
    // before the compositor's batch as well (see sync_line).
    mutable const u32* out = nullptr;
    bool gpu = false, allow_lag = true;
    u64 gen = 0;
    u32 nbins = 0;
    std::array<s32, MAX_BINS + 1> bin_y{};
    const u32* line(u32 y) const { return out + y * 256; }
  };
  // `allow_lag`: a GPU frame not finished yet may be stood in for by the one
  // before it (one frame of 3D latency instead of a stall); a display-capture
  // frame must not (stale VRAM readback is wrong emulation).
  FrameRef frame_ref(bool allow_lag = true) const;
  void sync_line(const FrameRef& f, s32 y);   // any thread; each call waits for one band at most (a GPU frame: resolves the record once)
  void sync_all();
  u64 last_band_sum_ns() const { return band_sum_ns_[0]; }   // serial raster cost of the last synced frame (sum over bands)
private:
  std::function<void(u32)> job_fn_;   // outlives the dispatch, unlike a local
  u32 pending_bands_ = 0;             // bins in flight (0 = nothing running)
  u64 gen_ = 0;                       // pool generation of the bands in flight

  u32  edge_count_ = 0;
  u32* out_dst_ = nullptr;                              // where final_pass writes
  const std::vector<const u32*>* texels_in_ = nullptr;  // decoded textures per polygon (render() records them)
  // Latched once by the coordinator after sync_all and handed to every band;
  // workers never read the engine's live list (SWAP can finalise the next
  // list while this raster is still in flight).
  const Polygon* const* list_polys_ = nullptr;
  u32 list_count_ = 0;
  s32  rendered_upto_ = 0;    // lines this instance has already rasterised this frame
  u32  setup_poly_ = 0;                                 // polygon index during build_edges
  std::vector<const u32*> poly_texels_;
  const char* gpu_dump_path_ = nullptr; u64 gpu_dump_frame_ = ~u64{0};
  void gpu_dump(const Gpu3D& gx, const Polygon* const* polys, u32 npoly);
  std::shared_ptr<vk::Device> vk_dev_;
  std::unique_ptr<vk::Lean> lean_;
  // The GPU submit (command recording + queue call, ~0.9 ms) runs on its own
  // thread, off the emulation thread: gpu_submit() fills the staging buffers
  // and the texel arena, then posts the Lean::submit call. gpu_job_wait()
  // before anything that reads the result or rewrites the staging.
  struct GpuJob { std::thread thread; std::mutex m; std::condition_variable cv; bool pending = false, quit = false, ok = false; u32 np = 0, nv = 0, ntex = 0; vk::GpuFrame f{}; };
  std::unique_ptr<GpuJob> gpu_job_;
  void gpu_job_start();
  void gpu_job_post(u32 np, u32 nv, u32 ntex, const vk::GpuFrame& f);
  bool gpu_job_wait();   // the last posted submit's result (true: on the GPU)
  bool gpu_on_ = false;
  bool gpu_frame_ = false;   // the last drawn frame went to the GPU: lean_ holds the picture, not out_[]
  struct GpuResident { u32 off = 0, words = 0, version = 0; };
  std::unordered_map<u32, GpuResident> gpu_resident_;   // decoded texture (cache id) -> arena offset
  u32 gpu_arena_top_ = 0;
  bool gpu_submit(const Gpu3D& gx, const Polygon* const* polys, u32 npoly);   // false: draw on the CPU instead
  std::vector<std::unique_ptr<Renderer3D>> bands_;      // workers 1..n-1 (band 0 is this)
  u64 band_ns_[8] = {};                                 // last frame's per-band wall time (workers write their own slot)
  u64 band_sum_ns_[2] = {0, 0};                         // summed band time (serial raster cost) of the last two frames
  u32 last_nb_ = 0;                                     // workers given the last frame (slots of band_ns_ that are live)
  static constexpr u64 kLagBandThresholdNs = 40'000'000; // serial raster cost two workers can still hide; below it a hot compositor gets the third core
  struct Pool;
public:
  void debug_dump(FILE* f);   // DS_WATCHDOG: band hand-off state
private:
  std::unique_ptr<Pool> pool_;
  // Steal-on-wait: a thread that would block for a band instead draws an
  // unclaimed bin itself (never later than waiting, and the waiting core does
  // the work). ctx_ snapshots what a bin needs, two slots by generation
  // parity (frame N compositor may still read while frame N+1 dispatches);
  // wait_idle waits for thieves too, so a VRAM remap never overtakes one.
  struct DispatchCtx {
    const Gpu3D* gx = nullptr;
    const Polygon* const* polys = nullptr;
    u32 npoly = 0;
    std::vector<const u32*> texels;
    RenderState rs;
    std::array<s32, MAX_BINS + 1> bin_y{};
    u32 nbins = 0;
    u32* dst = nullptr;
    bool aa = false;
  };
  DispatchCtx ctx_[2];
  struct StealBand { std::unique_ptr<Renderer3D> band; std::atomic<bool> busy{false}; u64 gen = ~u64{0}; };
  StealBand steal_[2];
  std::thread::id owner_;                // the thread render() dispatched from
  bool steal_bins(u64 gen, u32 upto);    // claim and draw unclaimed bins until bin `upto` is done; true if it is

  u32  fog_density(u32 addr) const;
  void final_pass(s32 y);
  void final_pass_ref(s32 y);
public:
  // final_pass against final_pass_ref on random buffers; 0 when identical.
  u32  selftest_final_pass(u32 seed, u32 dispcnt);
private:
  void clear_border(s32 y);
  void clear_line(s32 y);
};

} // namespace ds::gpu
