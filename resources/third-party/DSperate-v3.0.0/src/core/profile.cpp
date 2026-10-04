// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/profile.h"

#include <cstring>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>

#include "core/cpu/idle_loop.h"

namespace ds::prof {

bool enabled = false, heavy = false;
void set_level(const char* env) {
  enabled = env != nullptr;
  heavy = enabled && std::atoi(env) >= 2;
}
bool async_window = false;
bool census_same_list = false;

bool census_env(const char* var) {
  if (!std::getenv(var)) return false;
  if (!census) std::fprintf(stderr, "[profile] %s needs a -DDSPERATE_CENSUS=1 build; ignored\n", var);
  return census;
}

// "Of which" stages nest inside another scope; excluded from the sum. See profile.h.
inline bool of_which(Stage s) { return s == JIT_TX || s == JIT_TX_GEN || s == JIT_TX_INSTALL || s == JIT_TX_ICACHE || s == JIT_TX_DECODE || s == JIT_TX_LIVE || s == JIT_TX_EMIT || s == JIT_TX_FINISH || s == GX_RUN || s == W2D_JOIN || s == R3D_STEAL; }

const char* const names[COUNT] = {
  "cpu arm9", "cpu arm7", "dma", "gx geometry (of which)",
  "2d bg draw", "2d obj draw", "2d window", "2d select", "2d effects", "output", "capture",
  "3d clear", "3d spans", "3d final pass", "3d band wait", "spu",
  "jit translate", "jit tx codegen", "jit tx install", "jit tx icache", "jit tx decode", "jit tx liveness", "jit tx emit", "jit tx finish",
  "2d worker join (of which)",
  "sched slice loop", "events (timers, dma, fifo; not spu)", "gpu line hooks", "line-0 worker join", "begin_frame", "gx vblank (sort, join)",
  "2d journal/latches", "3d line wait", "3d prep (texcache)", "3d bins stolen (of which)",
};

// Stable machine keys for DS_PROFILE_LINE; unlike `names`, must not change on rename.
const char* const stage_keys[COUNT] = {
  "cpu9", "cpu7", "dma", "gx_geom",
  "bg_draw", "obj_draw", "window", "select", "effects", "output", "capture",
  "r3d_clear", "r3d_spans", "r3d_final", "r3d_wait", "spu",
  "jit_tx",
  "w2d_join",
  "sched", "events", "gpu_line", "join0", "begin_frame", "gx_vblank",
  "journal", "r3d_line", "r3d_prep", "r3d_steal",
};

const char* const count_names[] = {"3d polygon lines", "3d span pixels", "3d resolved pixels",
  "3d texel gathers (cached/direct)", "3d texel gathers (compressed)", "3d texel gathers (via views)",
  "3d texcache validated", "3d texcache decoded", "3d texcache bytes compared", "3d frames kept (no new swap)",
  "slices", "slices arm9 halted", "slices arm7 halted", "slices both halted", "slices with dma", "slices run to the deadline (both halted)",
  "cycles total", "cycles both halted", "cycles arm9 halted only", "cycles arm7 halted only", "cycles neither halted",
  "cycles arm9 awake+spinning", "cycles arm7 awake+spinning", "cycles one spinning, other halted", "cycles all idle (halt or spin)",
  "host ns arm9 in spin slices", "host ns arm7 in spin slices", "host ns arm9 working", "host ns arm7 working", "cycles skipped by idle-loop detect", "idle veto: dma", "idle veto: gx busy", "idle veto: irq pending", "idle veto: pc filter", "idle veto: arm9 not a loop", "idle veto: arm7 not a loop", "idle skip allowed", "arm7 spi poll: slices slept", "arm7 spi poll: cycles slept",
  "idle survey: slices vetoed by dma", "idle survey: WOULD have skipped", "idle survey: irq pending", "idle survey: pc filter", "idle survey: not a loop",
  "2d lines rendered", "2d text bg lines", "2d affine bg lines", "2d extended bg lines", "2d 3d-layer lines", "2d lines with sprites", "2d lines with windows", "2d lines with colour effect", "2d lines where an effect can apply", "2d flat lines (no effect possible)", "2d plane selects",
  "2d lines: 0 layers", "2d lines: 1 layer", "2d lines: 2 layers", "2d lines: 3 layers", "2d lines: 4+ layers", "2d lines: 1 layer, fully opaque", "2d lines with obj pixels", "2d lines with 3d pixels", "2d lines with a window", "2d bg lines 16-colour text", "2d bg lines 256-colour text", "2d bg lines empty (transparent row)", "2d 3d-layer lines with nothing visible", "2d fast lines: backdrop only", "2d fast lines: one opaque layer", "2d full lines: effect mode live", "2d full lines: translucent 3d", "2d full lines: semi/bitmap sprites", "2d full lines: second target needed", "2d full lines: fade only", "3d spans: constant colour", "3d spans: interpolated colour", "3d band 0 ns", "3d band 1 ns", "3d band 2 ns", "3d band 3 ns", "3d band phase ns (slowest band)", "3d band ns summed (all bands)", "async probe: frames measured", "async probe: texture vram changed by next line 0", "async probe: texture vram changed by next swap", "async probe: vramcnt rewritten by next line 0", "async probe: vramcnt rewritten by next swap", "3d raster force-joined (vram touched)", "3d bins drawn by the thread that would have waited", "3d batches flushed", "3d spans batched", "3d pixels batched",
  "gx swap_buffers (new list)", "gx swaps whose list is unchanged", "gx vblanks with no swap", "gx no-swap frames rejected by the register compare",
  "gx reject cause: dispcnt/alpha_ref", "gx reject cause: clear attrs", "gx reject cause: fog", "gx reject cause: edge/toon",
  "gx polygons submitted (summed over swaps)", "gx vertices submitted (summed over swaps)", "gx polygons in the largest swap", "gx vertices in the largest swap",
  "gx compare bytes if scanned in full", "gx compare bytes until the first difference", "gx compares run (counts matched)",
  "gx compare bytes, identical lists", "gx compares, identical lists", "gx compare bytes in full, differing lists", "gx compare bytes to first difference, differing lists", "gx compares, differing lists",
  "gx polygons in identical-list swaps", "gx vertices in identical-list swaps",
  "3d polygon lines in identical-list frames", "3d span pixels in identical-list frames",
  "3d spans len 1-4", "3d spans len 5-8", "3d spans len 9-16", "3d spans len 17-32", "3d spans len 33-64", "3d spans len 65-128", "3d spans len 129-256",
  "3d span pixels in len 1-4", "3d span pixels in len 5-8", "3d span pixels in len 9-16", "3d span pixels in len 17-32", "3d span pixels in len 33-64", "3d span pixels in len 65-128", "3d span pixels in len 129-256",
  "stores into palette space", "stores into oam space",
  "2d lazy frames", "2d frames engine A starts per line (futile)", "2d frames engine B starts per line (futile)", "2d trap windows re-armed (engine mode change)", "2d vram trap hits", "2d lag frames", "2d lag: trapped stores", "2d lag: stores that joined a line", "2d lag: frames that hit the trap limit", "2d lag: lines left in flight", "2d engine A batch: stores that joined it", "2d engine A batch: capture-bank reads that joined it",
  "2d join calls", "render_ranges calls", "rr: engine A deferred", "rr: lag hand-off", "rr: engine B handed", "rr: nothing handed",
  "rr: worker parked at the decision", "rr: worker awake at the decision",
  "rr: short run handed ONLY because awake", "rr: short run inline ONLY because parked",
  "rr: lines drawn inline", "rr: lines handed over",
  "vramcnt remaps", "vramcnt remaps: a 2d view moved", "vramcnt remaps: no 2d view moved", "vramcnt remaps with lines pending",
  "vramcnt remaps while a 2d job is in flight",
  "  ...of those, on an alternating-phase frame", "  ...of those, the in-flight job is a capture", "  ...of those, on a clean frame (neither)", "remap in flight: banks stay in their engine", "remap in flight: a bank moves to the other engine", "remap in flight: a bank moves to lcdc", "remap in flight: a bank moves to arm7", "remap in flight: a bank moves to texture only", "vramcnt remaps without a join (job on its snapshot)",
  "frames with an alternating display phase", "frames with capture on", "frames total",
  "2d join ns: catch_up", "2d join ns: vram trap", "2d join ns: journal full", "2d join ns: line 0", "2d join ns: vram remap", "2d join ns: render_ranges pre", "2d join ns: render_ranges post", "2d join ns: other",
  "3d polygon-chunk entries",
  "3d spans empty (no pixels)", "3d spans fully occluded by depth", "3d spans that draw",
  "gx reg reads", "gx reads of GXSTAT", "gx GXSTAT reads while busy (bit27)",
  "3d drawn spans: plain", "3d drawn pixels: plain", "3d drawn spans: toon/highlight", "3d drawn pixels: toon/highlight", "3d drawn spans: shadow", "3d drawn pixels: shadow", "3d drawn spans: wireframe (scalar)", "3d drawn pixels: wireframe (scalar)",
  "2d compares: bg ext palette (512B)", "2d compares: obj ext palette (512B)",
  "2d compares that differed: bg ext palette", "2d compares that differed: obj ext palette",
  "dma transfers started", "dma dispatch loop entries (a run or one unit)",
  "dma gxfifo words (run)", "dma gxfifo words (per word)", "dma gxfifo bulk runs",
  "dma page-to-page runs", "dma units in word runs", "dma units in halfword runs", "dma words through the bus", "dma halfwords through the bus",
  "dma vram traps taken for a run",
  "dma units -> main ram", "dma units -> wram", "dma units -> palette", "dma units -> oam", "dma units -> i/o", "dma units -> elsewhere",
  "dma units -> vram engine A bg", "dma units -> vram engine B bg", "dma units -> vram engine A obj", "dma units -> vram engine B obj", "dma units -> vram lcdc",
  "dma vram traps: engine A bg", "dma vram traps: engine B bg", "dma vram traps: engine A obj", "dma vram traps: engine B obj", "dma vram traps: lcdc",
  "2d render ranges: engine A", "2d render ranges: engine B",
  "dma starts: immediate", "dma starts: vblank", "dma starts: hblank", "dma starts: display start", "dma starts: display fifo", "dma starts: cart", "dma starts: gba", "dma starts: gxfifo", "dma starts: arm7",
  "3d resolve groups (8px)", "3d resolve groups: nothing drawn", "3d resolve groups: one kind", "3d resolve groups: mixed kinds", "3d resolve groups: opaque only", "3d resolve groups: translucent only", "3d resolve groups: touching the under layer",
  "3d resolve batches", "3d resolve batches: nothing drawn", "3d resolve batches: one kind", "3d resolve batches: mixed kinds", "3d resolve batches: opaque only",
  "3d resolve: drawing groups in batches", "3d resolve: drawing groups in one-kind batches", "3d resolve: drawing groups in mixed batches",
  "3d resolve empty groups: under-layer candidates only", "3d resolve empty groups: a top lane passed depth",
  "3d resolve groups: every lane opaque and drawing", "3d resolve pixels in every-lane-opaque groups",
  "jit retime invalidations", "jit blocks killed by retimes",
  "bus vram remaps", "bus tcm updates", "bus gba slot retimes", "timing cpu9 range rebuilds",
  "gx lists dropped unrendered", "gx lists identical to the last", "gx lists that reached a render",
  "cycles arm9 exact idle (same pc, same regs)", "cycles arm7 exact idle (same pc, same regs)",
  "host ns arm9 in exact-idle slices", "host ns arm7 in exact-idle slices",
  "cycles arm9 drift idle (same regs, pc within 256 bytes)", "cycles arm7 drift idle (same regs, pc within 256 bytes)",
  "host ns arm9 in drift-idle slices", "host ns arm7 in drift-idle slices",
  "idle cut: probes", "idle cut: probes still idle", "idle cut: arm9 cycles skipped", "idle cut: arm7 cycles skipped"};

static_assert(sizeof(count_names) / sizeof(*count_names) == C_COUNT,
              "count_names must have exactly one entry per Counter enumerator");

#if defined(__aarch64__)
namespace detail {
u64 cnt_mul = [] {
  u64 f;
  asm volatile("mrs %0, cntfrq_el0" : "=r"(f));
  return f ? static_cast<u64>((static_cast<unsigned __int128>(1000000000ull) << 32) / f) : 0;
}();
}
#endif
double probe_ns = 0.0;

namespace {
// The cost of one probe, measured on a scratch accumulator so the stages
// stay clean. The clock governor decides what a probe costs: measured once
// at startup it varied 70-110 ns on the RK3566 with where the clock stood,
// so it is re-measured every few hundred frames and the minimum kept -- the
// cost at full clock, which is what the loaded emulation core runs at.
void calibrate_probe(u32 n) {
  Accum scratch;
  Accum* const saved = detail::acc;
  detail::acc = &scratch;
  for (u32 i = 0; i < n / 10; ++i) { Scope s(CPU9); }   // warm
  const u64 t0 = now_ns();
  for (u32 i = 0; i < n; ++i) { Scope s(CPU9); }
  const u64 t1 = now_ns();
  detail::acc = saved;
  const double v = static_cast<double>(t1 - t0) / n;
  if (probe_ns == 0.0 || v < probe_ns) probe_ns = v;
}
}

namespace detail {
thread_local Accum* acc = nullptr;

std::mutex& accs_mutex() { static std::mutex m; return m; }
std::vector<Accum*>& accs() { static std::vector<Accum*> v; return v; }

// One accumulator per thread, owned by the registry and never freed, so
// totals survive a band worker outliving or predeceasing `report`.
Accum* make_acc() {
  Accum* a = new Accum();
  { std::lock_guard<std::mutex> lk(accs_mutex()); a->tid = static_cast<u32>(accs().size()); accs().push_back(a); }
  acc = a;
  static std::once_flag once;
  if (enabled) std::call_once(once, [] { calibrate_probe(200000); });
  return a;
}
// A stage's time less what its own probes cost.
inline u64 net_ns(const Accum& a, u32 i) {
  const u64 cost = static_cast<u64>(static_cast<double>(a.probes[i]) * probe_ns);
  return a.ns[i] > cost ? a.ns[i] - cost : 0;
}
} // namespace detail

// Per-frame stage series. Only the emulation thread's stages are kept per
// frame (it waits for the slowest band, so they sum to the critical path);
// band worker time is folded into one "band workers" column for context.
namespace {
// cyc/slices distinguish a STALLED frame (lost time outside emulation) from
// a CATCH-UP one (ran several frames' worth of cycles).
struct FrameNs { u64 ns[COUNT]; u64 workers; u64 cyc; u64 slices; u64 probe_emu; };   // probe_emu: the emulation thread's probe cost this frame, ns
std::vector<FrameNs> frame_series;
u64 frame_last_ns[COUNT];
u64 frame_last_workers, frame_last_probes;
u64 frame_last_cyc, frame_last_slices;
} // namespace

void frame_mark() {
  if (!enabled) return;
  { static u32 frames = 0; if (++frames % 256 == 0) calibrate_probe(5000); }   // ~0.4 ms, once every 256 frames
  u64 now[COUNT] = {}; u64 workers = 0, probes = 0;
  {
    std::lock_guard<std::mutex> lk(detail::accs_mutex());
    for (const Accum* a : detail::accs()) {
      if (a->tid == 0) for (u32 i = 0; i < COUNT; ++i) { now[i] = detail::net_ns(*a, i); probes += a->probes[i]; }
      else             for (u32 i = 0; i < COUNT; ++i) workers += detail::net_ns(*a, i);
    }
  }
  FrameNs d{};
  for (u32 i = 0; i < COUNT; ++i) { d.ns[i] = now[i] > frame_last_ns[i] ? now[i] - frame_last_ns[i] : 0; frame_last_ns[i] = now[i]; }
  d.workers = workers > frame_last_workers ? workers - frame_last_workers : 0;
  frame_last_workers = workers;
  d.probe_emu = static_cast<u64>(static_cast<double>(probes - frame_last_probes) * probe_ns);
  frame_last_probes = probes;
  {
    u64 cyc = 0, slices = 0;
    std::lock_guard<std::mutex> lk(detail::accs_mutex());
    for (const Accum* a : detail::accs()) { cyc += a->count[C_CYC_TOTAL]; slices += a->count[C_SLICES]; }
    d.cyc = cyc - frame_last_cyc; frame_last_cyc = cyc;
    d.slices = slices - frame_last_slices; frame_last_slices = slices;
  }
  frame_series.push_back(d);
}

void deduct_probe_overhead(std::vector<double>& frame_ms, std::vector<double>* work_ms) {
  if (!enabled || frame_series.size() < frame_ms.size()) return;
  const size_t off = frame_series.size() - frame_ms.size();
  for (size_t i = 0; i < frame_ms.size(); ++i) {
    const double p = static_cast<double>(frame_series[off + i].probe_emu) / 1e6;
    frame_ms[i] = std::max(0.0, frame_ms[i] - p);
    if (work_ms && i < work_ms->size()) (*work_ms)[i] = std::max(0.0, (*work_ms)[i] - p);
  }
}

void frame_breakdown(const std::vector<double>& frame_ms) {
  // frame_mark runs every frame; frame_ms may start later (--stats-from), so align to the tail.
  if (!enabled || frame_series.empty() || frame_series.size() < frame_ms.size()) return;
  if (frame_series.size() > frame_ms.size())
    frame_series.erase(frame_series.begin(), frame_series.begin() + static_cast<long>(frame_series.size() - frame_ms.size()));
  const size_t n = frame_ms.size();
  std::vector<size_t> order(n);
  for (size_t i = 0; i < n; ++i) order[i] = i;
  std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return frame_ms[a] < frame_ms[b]; });
  // Tail: the slowest (100 - DS_PROFILE_TAIL)% (default 99: the slowest 1%).
  // typical: middle fifth, keeping boot transients and spikes out of the baseline.
  static const int tail_pct = [] { const char* e = std::getenv("DS_PROFILE_TAIL"); const int v = e ? std::atoi(e) : 99; return v >= 50 && v <= 99 ? v : 99; }();
  const size_t n99 = std::max<size_t>(1, n * static_cast<size_t>(100 - tail_pct) / 100);
  const std::vector<size_t> tail(order.end() - static_cast<long>(n99), order.end());
  const std::vector<size_t> mid(order.begin() + static_cast<long>(n * 2 / 5),
                                order.begin() + static_cast<long>(n * 3 / 5));
  struct Group { double ms = 0, stage[COUNT] = {}, workers = 0, untimed = 0; };
  auto mean = [&](const std::vector<size_t>& idx) {
    Group g;
    for (size_t i : idx) {
      double timed = 0;
      // "of which" stages are reported as rows but excluded here (else `untimed` goes negative).
      for (u32 s = 0; s < COUNT; ++s) { const double v = frame_series[i].ns[s] / 1e6; g.stage[s] += v; if (!of_which(static_cast<Stage>(s))) timed += v; }
      g.workers += frame_series[i].workers / 1e6;
      g.ms += frame_ms[i];
      g.untimed += frame_ms[i] - timed;
    }
    const double k = 1.0 / static_cast<double>(idx.size());
    g.ms *= k; g.workers *= k; g.untimed *= k;
    for (u32 s = 0; s < COUNT; ++s) g.stage[s] *= k;
    return g;
  };
  // Fastest fifth too: for titles alternating heavy/light frames, the delta
  // between this and the middle column is what the heavy frame adds.
  const std::vector<size_t> low(order.begin(), order.begin() + static_cast<long>(n / 5));
  const Group m = mean(mid), t = mean(tail), l = mean(low);
  std::fprintf(stderr, "[frames] stage breakdown, mean of fastest fifth (%zu frames), typical (middle 20%%, %zu frames) and p%d tail (%zu frames), sorted by what the tail adds:\n",
               low.size(), mid.size(), tail_pct, tail.size());
  std::fprintf(stderr, "[frames] %-16s %9s %9s %6s%d ms %9s\n", "stage", "fast ms", "typ ms", "p", tail_pct, "delta");
  std::vector<u32> rows(COUNT);
  for (u32 s = 0; s < COUNT; ++s) rows[s] = s;
  std::sort(rows.begin(), rows.end(), [&](u32 a, u32 b) { return t.stage[a] - m.stage[a] > t.stage[b] - m.stage[b]; });
  for (u32 s : rows)
    if (m.stage[s] >= 0.0005 || t.stage[s] >= 0.0005)
      std::fprintf(stderr, "[frames] %-16s %9.3f %9.3f %9.3f %+9.3f\n", names[s], l.stage[s], m.stage[s], t.stage[s], t.stage[s] - m.stage[s]);
  std::fprintf(stderr, "[frames] %-16s %9.3f %9.3f %9.3f %+9.3f   (frame_ms minus the timed stages, excluding the \"of which\" rows)\n",
               "untimed", l.untimed, m.untimed, t.untimed, t.untimed - m.untimed);
  std::fprintf(stderr, "[frames] %-16s %9.3f %9.3f %9.3f %+9.3f   (overlaps the band wait; not part of the wall time)\n",
               "band workers", l.workers, m.workers, t.workers, t.workers - m.workers);
  std::fprintf(stderr, "[frames] %-16s %9.3f %9.3f %9.3f %+9.3f\n", "frame total", l.ms, m.ms, t.ms, t.ms - m.ms);
  const size_t worst_n = std::min<size_t>(6, n);
  {
    // Median frame as calibration: 1.00x = one frame of emulated cycles (1,120,380); a spike above 1.00x was catching up.
    const size_t md = order[n / 2];
    std::fprintf(stderr, "[frames] median frame #%-5zu %7.3f ms  [cyc %.2fx slices %llu]\n",
                 md, frame_ms[md], static_cast<double>(frame_series[md].cyc) / 1120380.0,
                 static_cast<unsigned long long>(frame_series[md].slices));
  }
  std::fprintf(stderr, "[frames] worst %zu frames:\n", worst_n);
  for (size_t k = 0; k < worst_n; ++k) {
    const size_t i = order[n - 1 - k];
    double timed = 0;
    std::vector<u32> top(COUNT);
    for (u32 s = 0; s < COUNT; ++s) { top[s] = s; timed += frame_series[i].ns[s] / 1e6; }
    std::sort(top.begin(), top.end(), [&](u32 a, u32 b) { return frame_series[i].ns[a] > frame_series[i].ns[b]; });
    std::fprintf(stderr, "[frames]   #%-5zu %7.3f ms:", i, frame_ms[i]);
    for (size_t s = 0; s < 3 && frame_series[i].ns[top[s]]; ++s)
      std::fprintf(stderr, " %s %.3f", names[top[s]], frame_series[i].ns[top[s]] / 1e6);
    std::fprintf(stderr, " untimed %.3f", frame_ms[i] - timed);
    std::fprintf(stderr, "  [cyc %.2fx slices %llu]\n",
                 static_cast<double>(frame_series[i].cyc) / 1120380.0,
                 static_cast<unsigned long long>(frame_series[i].slices));
  }

  // DS_PROFILE_LINE: this run as one JSON line. "-"/"1" -> stderr; else a
  // path, appended to. DS_PROFILE_LABEL names the run.
  const char* line_to = std::getenv("DS_PROFILE_LINE");
  if (!line_to || !*line_to) return;
  std::FILE* out = stderr;
  bool close_out = false;
  if (std::strcmp(line_to, "-") != 0 && std::strcmp(line_to, "1") != 0) {
    out = std::fopen(line_to, "a");
    if (!out) { std::fprintf(stderr, "[profile] cannot append to %s\n", line_to); return; }
    close_out = true;
  }
  u64 counts[C_COUNT] = {};
  {
    std::lock_guard<std::mutex> lk(detail::accs_mutex());
    for (const Accum* a : detail::accs())
      for (u32 i = 0; i < C_COUNT; ++i) counts[i] += a->count[i];
  }
  auto pct = [&](double q) {
    const size_t k = std::min(n - 1, static_cast<size_t>(q * static_cast<double>(n - 1) + 0.5));
    return frame_ms[order[k]];
  };
  const char* label = std::getenv("DS_PROFILE_LABEL");
  if (close_out) std::fprintf(out, "%s", "");
  std::fprintf(out, "{\"label\":\"%s\",\"frames\":%zu", label ? label : "", n);
  std::fprintf(out, ",\"ms\":{\"mean\":%.4f,\"median\":%.4f,\"p90\":%.4f,\"p95\":%.4f,\"p99\":%.4f,\"max\":%.4f}",
               m.ms, pct(0.50), pct(0.90), pct(0.95), pct(0.99), frame_ms[order[n - 1]]);
  std::fprintf(out, ",\"typ\":{");
  bool first = true;
  for (u32 i = 0; i < COUNT; ++i) {
    if (m.stage[i] < 5e-5) continue;   // below a twentieth of a microsecond a frame, not signal
    std::fprintf(out, "%s\"%s\":%.4f", first ? "" : ",", stage_keys[i], m.stage[i]);
    first = false;
  }
  std::fprintf(out, "},\"typ_untimed\":%.4f,\"typ_workers\":%.4f", m.untimed, m.workers);
  std::fprintf(out, ",\"p99\":{");
  first = true;
  for (u32 i = 0; i < COUNT; ++i) {
    if (t.stage[i] < 5e-5) continue;
    std::fprintf(out, "%s\"%s\":%.4f", first ? "" : ",", stage_keys[i], t.stage[i]);
    first = false;
  }
  std::fprintf(out, "},\"p99_untimed\":%.4f,\"p99_workers\":%.4f", t.untimed, t.workers);
  // Fixed set of workload counters so two runs can be diffed.
  struct { const char* k; Counter c; } kCounts[] = {
    {"slices", C_SLICES}, {"gx_swap", C_GX_SWAP}, {"gx_read_gxstat", C_GX_READ_GXSTAT},
    {"poly_lines", C_POLY_LINES}, {"span_px", C_SPAN_PIXELS}, {"resolved_px", C_RESOLVED_PIXELS},
    {"2d_lines", C_2D_LINES}, {"2d_lazy_frames", C_2D_LAZY_FRAMES}, {"2d_trap_hits", C_2D_TRAP_HITS},
    {"dma_starts", C_DMA_STARTS}, {"cyc_total", C_CYC_TOTAL}, {"cyc_idle_skipped", C_CYC_IDLE_SKIPPED},
  };
  std::fprintf(out, ",\"counts\":{");
  for (size_t i = 0; i < sizeof(kCounts) / sizeof(*kCounts); ++i)
    std::fprintf(out, "%s\"%s\":%llu", i ? "" : "", kCounts[i].k, (unsigned long long)counts[kCounts[i].c]),
    std::fprintf(out, "%s", i + 1 < sizeof(kCounts) / sizeof(*kCounts) ? "," : "");
  std::fprintf(out, "}}\n");
  if (close_out) std::fclose(out);
  else std::fflush(out);
}

// Called from the emulation thread after band workers are idle; a plain lock is enough.
void report() {
  Accum t;
  {
    std::lock_guard<std::mutex> lk(detail::accs_mutex());
    for (const Accum* a : detail::accs()) {
      for (u32 i = 0; i < COUNT; ++i) { t.ns[i] += detail::net_ns(*a, i); t.probes[i] += a->probes[i]; }
      for (u32 i = 0; i < C_COUNT; ++i) t.count[i] += a->count[i];
    }
  }
  {
    u64 probes = 0;
    for (u32 i = 0; i < COUNT; ++i) probes += t.probes[i];
    std::fprintf(stderr, "[profile] probe cost %.0f ns each, %llu probes: %.1f ms removed from the stages below (and from frame ms, per frame)\n",
                 probe_ns, static_cast<unsigned long long>(probes), static_cast<double>(probes) * probe_ns / 1e6);
  }
  const u64* ns = t.ns;
  const u64* count = t.count;
  u64 total = 0;
  for (u32 i = 0; i < COUNT; ++i) if (!of_which(static_cast<Stage>(i))) total += ns[i];
  if (!total) return;
  for (u32 i = 0; i < C_COUNT; ++i)
    if (count[i]) std::fprintf(stderr, "[profile] %-20s %12llu\n", count_names[i], static_cast<unsigned long long>(count[i]));
  {
    const auto& il = ds::cpu::idle_loop_stats();
    std::fprintf(stderr, "[profile] idle-loop: queries %llu hits %llu analyses %llu\n",
                 (unsigned long long)il.queries, (unsigned long long)il.hits, (unsigned long long)il.analyses);
    for (u32 r = 1; r < 12; ++r)
      if (il.by_reason[r])
        std::fprintf(stderr, "[profile]   reject %-14s %10llu\n",
                     ds::cpu::idle_reject_name(static_cast<ds::cpu::IdleReject>(r)),
                     (unsigned long long)il.by_reason[r]);
  }
  // DS_PROFILE_THREADS=1: the same stages, per thread.
  if (std::getenv("DS_PROFILE_THREADS")) {
    std::lock_guard<std::mutex> lk(detail::accs_mutex());
    for (const Accum* a : detail::accs()) {
      u64 t = 0;
      for (u32 i = 0; i < COUNT; ++i) t += a->ns[i];
      if (!t) continue;
      std::fprintf(stderr, "[profile] --- thread %u (%s), %.1f ms of stage time\n", a->tid,
                   a->tid == 0 ? "emulation" : "band worker", t / 1e6);
      for (u32 i = 0; i < COUNT; ++i)
        if (a->ns[i]) std::fprintf(stderr, "[profile]     %-14s %9.1f %5.1f%%\n", names[i], a->ns[i] / 1e6, 100.0 * a->ns[i] / t);
      for (u32 i = 0; i < C_COUNT; ++i)
        if (a->count[i] && (i == C_POLY_LINES || i == C_SPAN_PIXELS || i == C_RESOLVED_PIXELS || i == C_2D_LINES))
          std::fprintf(stderr, "[profile]     %-24s %12llu\n", count_names[i], (unsigned long long)a->count[i]);
    }
  }
  std::fprintf(stderr, "[profile] %-14s %9s %6s %12s\n", "stage", "ms", "%", "probes");
  for (u32 i = 0; i < COUNT; ++i)
    if (ns[i] || t.probes[i]) std::fprintf(stderr, "[profile] %-14s %9.1f %5.1f%% %12llu\n", names[i], ns[i] / 1e6, 100.0 * ns[i] / total, static_cast<unsigned long long>(t.probes[i]));
  std::fprintf(stderr, "[profile] %-14s %9.1f  (excludes the \"of which\" rows)\n", "sum", total / 1e6);
}

} // namespace ds::prof
