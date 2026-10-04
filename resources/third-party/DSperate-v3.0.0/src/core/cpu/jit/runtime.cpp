// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Host-agnostic recompiler runtime: arena, block cache, linking, park-and-revive,
// SMC tracking by host page. Host code emission is in backend::.
#include "core/cpu/jit/jit_internal.h"
#include "core/cpu/wait_loop.h"
#include "core/cpu/timing_mode.h"
#include "core/mem/fastmem_census.h"
#include "core/mem/fastmem.h"
#include "core/profile.h"
#include "core/sched/scheduler.h"
#include "core/cpu/cpu_cycles.h"
#include "core/cpu/interp/interp.h"
#include "core/cpu/interp/interp_internal.h"
#include "core/nds.h"

#include <cstdio>
#include <map>
#include <vector>
#include <algorithm>
#include <type_traits>
#include <string>
#include <cstring>
#include <unistd.h>
#include <cstdlib>
#include <sys/mman.h>
#include <csignal>
#include <ucontext.h>
#include <iterator>
#include <algorithm>
#include <vector>

namespace ds::jit {

namespace {
// 32-bit: A32 reaches the stubs with `b`/`bl` (+-32 MB).
constexpr size_t ARENA_BYTES = sizeof(void*) >= 8 ? (64u << 20) : (32u << 20);
constexpr size_t BLOCK_MARGIN = 64u << 10;    // max bytes one block may emit
// Bounds Block metadata growth (reset forced when reached).
constexpr size_t MAX_BLOCKS = ARENA_BYTES / 4 * 3 / sizeof(Block);

Runtime g_rt;
} // namespace
static void invalidate_blocks(const u8* host_page, std::vector<Block*> victims);
namespace {

// DS_JIT_CHURN=1: who invalidates what, and what gets retranslated. Printed at exit.
namespace churn {
struct Key { u32 pc; u8 dma, cpu; bool operator<(const Key& o) const { return pc != o.pc ? pc < o.pc : dma != o.dma ? dma < o.dma : cpu < o.cpu; } };
static std::map<Key, u64> writers;
static std::map<u32, u64> victims_by_page;
static std::map<u32, u64> retrans;             // guest pc -> translations
static std::map<u64, u64> trans_by_frame;
static std::map<u64, u64> retrans_by_frame;
static std::map<u64, u64> store_sites;         // (victim pc << 32 | store guest addr) -> kills
static std::map<u32, u64> by_region;           // (cpu << 8 | pc >> 24) -> translations
static u64 sample_frame = ~0ull, sampled = 0;   // DS_CHURN_FRAME=N: print the first keys translated in frame N
static u64 revive_none = 0, revive_stamp = 0, revive_span = 0, revive_len = 0, revive_bytes = 0, revive_ok = 0;   // why a revive did not happen
static u64 inval = 0, killed = 0, trans = 0, frames_seen = 0, range_miss = 0, resets = 0;
static u64 retime_calls = 0, retime_killed = 0;
// Lead time: page's last invalidation -> retranslation, in ARM9 cycles.
static std::map<u32, u64> page_last_write;   // guest page -> sched.now()
static u64 lead_hist[6];
static u64 lead_n = 0, lead_sum = 0;
static bool on() { static const bool e = std::getenv("DS_JIT_CHURN") != nullptr; return e; }
static void report() {
  auto top = [](auto& m, const char* what, int n, auto print) {
    using K = std::remove_const_t<std::remove_reference_t<decltype(m.begin()->first)>>;
    std::vector<std::pair<u64, K>> v; for (auto& kv : m) v.push_back({kv.second, kv.first});
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.first > b.first; });
    std::fprintf(stderr, "[churn] top %s (%zu distinct):\n", what, m.size());
    for (int i = 0; i < n && i < (int)v.size(); ++i) print(v[i].first, v[i].second);
  };
  auto pk = [](u64 n, const Key& k) { std::fprintf(stderr, "   %10llu  pc %08x %s arm%d\n", (unsigned long long)n, k.pc, k.dma ? "DMA" : "cpu", k.cpu); };
  auto pa = [](u64 n, u32 a) { std::fprintf(stderr, "   %10llu  %08x\n", (unsigned long long)n, a); };
  std::fprintf(stderr, "[churn] frames %llu arena resets %llu invalidations %llu blocks killed %llu translations %llu | code-page stores changed-but-no-block %llu\n",
               (unsigned long long)frames_seen, (unsigned long long)resets, (unsigned long long)inval, (unsigned long long)killed, (unsigned long long)trans,
               (unsigned long long)range_miss);
  if (prof::census)
    std::fprintf(stderr, "[churn] code-page stores: silent %llu changed %llu\n",
                 (unsigned long long)mem::code_store_stats.silent, (unsigned long long)mem::code_store_stats.changed);
  std::fprintf(stderr, "[churn] retimes %llu (%.2f/frame) blocks killed by retimes %llu (%.1f/frame)\n",
               (unsigned long long)retime_calls, frames_seen ? static_cast<double>(retime_calls) / static_cast<double>(frames_seen) : 0.0,
               (unsigned long long)retime_killed, frames_seen ? static_cast<double>(retime_killed) / static_cast<double>(frames_seen) : 0.0);
  top(writers, "writers (pc of the store / DMA start)", 15, pk);
  top(victims_by_page, "invalidated guest pages (2 KB)", 15, pa);
  top(retrans, "retranslated block pcs", 20, pa);
  auto ps = [](u64 n, u64 k) { std::fprintf(stderr, "   %10llu  block %08x <- store at %08x\n", (unsigned long long)n, static_cast<u32>(k >> 32), static_cast<u32>(k)); };
  top(store_sites, "store address per victim block", 12, ps);
  auto pr = [](u64 n, u32 k) { std::fprintf(stderr, "   %10llu  arm%d region %02x\n", (unsigned long long)n, (k >> 8) ? 7 : 9, k & 0xFF); };
  top(by_region, "translations by cpu and region", 12, pr);
  std::fprintf(stderr, "[churn] revive: ok %llu, no parked %llu, stamp %llu, span %llu, len %llu, bytes differ %llu\n", (unsigned long long)revive_ok,
               (unsigned long long)revive_none, (unsigned long long)revive_stamp, (unsigned long long)revive_span, (unsigned long long)revive_len, (unsigned long long)revive_bytes);
  auto pf = [](u64 n, u64 f) { std::fprintf(stderr, "   %10llu  frame %llu (%llu re)\n",
      (unsigned long long)n, (unsigned long long)f, (unsigned long long)retrans_by_frame[f]); };
  top(trans_by_frame, "translation frames", 20, pf);
  if (lead_n) {
    static const char* const lb[6] = {"<0.1ms", "0.1-1ms", "1-4ms", "4-16ms", "16-64ms", ">64ms"};
    std::fprintf(stderr, "[churn] retranslate lead time (page's last kill -> translate, upper bound), %llu samples, mean %.1f ms:\n",
                 (unsigned long long)lead_n, static_cast<double>(lead_sum) / static_cast<double>(lead_n) / 1120380.0 * 16.7);
    for (int i = 0; i < 6; ++i) std::fprintf(stderr, "   %-8s %10llu\n", lb[i], (unsigned long long)lead_hist[i]);
  }
}
}  // namespace churn

// ---- code page tracking ----

const u8* host_page_of(const u8* p) { return reinterpret_cast<const u8*>(reinterpret_cast<u64>(p) & ~u64{mem::PAGE_SIZE - 1}); }

// Offsets of `b`'s bytes within `page` (of at most two, possibly non-adjacent);
// an unmapped end counts as the whole page.
static void page_span(const Block* b, const u8* page, u32& lo, u32& hi) {
  if (!b->host_lo || !b->host_hi) { lo = 0; hi = mem::PAGE_SIZE - 1; return; }
  const u8* seg_lo; const u8* seg_hi;
  if (page == b->host_pages[0]) { seg_lo = b->host_lo; seg_hi = (b->npages == 1) ? b->host_hi : page + mem::PAGE_SIZE - 1; }
  else                          { seg_lo = page; seg_hi = b->host_hi; }
  lo = static_cast<u32>(seg_lo - page); hi = static_cast<u32>(seg_hi - page);
}

bool code_query(const u8* host_page) { return g_rt.code_pages.count(host_page) != 0; }

void set_code_tag(const u8* host_page, bool on) {
  if (mem::fmc::on()) mem::fmc::code_tag(on);
  for (JitCpu& jc : g_rt.cpus)
    if (jc.ctx) jc.ctx->page_table.set_code_host(host_page, on);
}

void code_write_hook(u8* host, u32 len) {
  const u8* first = host_page_of(host);
  const u8* last = host_page_of(host + len - 1);
  invalidate_host_range(first, host, host + len - 1);
  if (last != first) invalidate_host_range(last, host, host + len - 1);
}

constexpr size_t PARKED_PER_KEY = 4;

void kill_block(JitCpu& jc, Block* b) {
  if (b->dead) return;
  b->dead = true;
  // Linked callers now land in the dispatcher, which revives or retranslates.
  std::memcpy(b->entry_words, b->entry, backend::ENTRY_PATCH);
  {
    std::vector<Block*>& v = jc.parked[b->key];
    if (v.size() >= PARKED_PER_KEY) v.erase(v.begin());
    v.push_back(b);
  }
  backend::write_entry_redirect(b->entry, b->key, jc.dispatch);
  sync_icache(b->entry, backend::ENTRY_PATCH);
  const u32 idx = (b->key >> 1) & (LUT_SIZE - 1);
  if (static_cast<u32>(jc.lut[idx]) == b->key) jc.lut[idx] = LUT_EMPTY_KEY;
  jc.blocks.erase(b->key);
  g_rt.stats.blocks_invalidated++;
}

void remove_from_page_lists(Block* b) {
  for (u32 i = 0; i < b->npages; ++i) {
    auto it = g_rt.code_pages.find(b->host_pages[i]);
    if (it == g_rt.code_pages.end()) continue;
    it->second.remove(b);
    if (it->second.empty()) { g_rt.code_pages.erase(it); set_code_tag(b->host_pages[i], false); }
  }
}

JitCpu& jc_of(Block* b) { return g_rt.cpus[b->owner]; }

void reset_arena() {
  if (churn::on()) ++churn::resets;
  Runtime& r = g_rt;
  for (JitCpu& jc : r.cpus) {
    for (Block* b : jc.all_blocks) if (!b->pooled) delete b;
    jc.all_blocks.clear();
    jc.blocks.clear();
    jc.parked.clear();
    if (jc.lut) for (u32 i = 0; i < LUT_SIZE; ++i) jc.lut[i] = LUT_EMPTY_KEY;
  }
  for (auto& kv : r.code_pages) set_code_tag(kv.first, false);
  r.code_pages.clear();
  r.fm_blocks.clear();   // fm_slow (guest sites) survives
  r.fm_rels.clear();
  r.block_pool.clear();
  r.blocks_live = 0;
  r.pos = r.stubs_end;
  r.need_reset = false;
  r.stats.flushes++;
}

// ARM9 timing table rebuilt (PU/TCM/EXMEMCNT write): kill only blocks whose
// Block::dep_* bytes changed; parked blocks are all dropped.
void on_timing_changed(CpuContext& cpu) {
  if (!cpu.jit) return;
  JitCpu& jc = *static_cast<JitCpu*>(cpu.jit);
  mem::Timing& t = cpu.nds->bus.timing();
  if (churn::on()) { static bool reg = (std::atexit(churn::report), true); (void)reg; }
  u64 killed = 0;
  if (t.retime_pending()) {
    for (Block* b : jc.all_blocks) {
      if (b->dead) continue;
      bool hit = b->dep_overflow;
      for (u32 i = 0; i < b->ndep && !hit; ++i) hit = (t.retime_flag(b->dep_page[i]) & b->dep_kind[i]) != 0;
      if (!hit) continue;
      remove_from_page_lists(b);
      kill_block(jc, b);
      ++killed;
    }
  }
  t.retime_clear();
  jc.parked.clear();
  if (killed) jc.ctx->hot.alerts |= ALERT_INVALIDATED;
  prof::add(prof::C_JIT_INVALIDATE_CPU, 1);
  prof::add(prof::C_JIT_INVALIDATE_CPU_KILLED, killed);
  if (churn::on()) { churn::retime_calls++; churn::retime_killed += killed; churn::frames_seen = cpu.nds->frame_count; }
}

} // namespace

Runtime& rt() { return g_rt; }

// ---- cache ----

void lut_insert(JitCpu& jc, Block* b) {
  const u32 idx = (b->key >> 1) & (LUT_SIZE - 1);
  jc.lut[idx] = (static_cast<u64>(b->entry - g_rt.arena) << 32) | b->key;
}

// DS_PERF_MAP=1: /tmp/perf-<pid>.map symbols jit9_<pc>/jit7_<pc>[t].
static FILE* perf_map_file() {
  static FILE* map = [] () -> FILE* {
    if (!std::getenv("DS_PERF_MAP")) return nullptr;
    char path[64];
    std::snprintf(path, sizeof path, "/tmp/perf-%d.map", static_cast<int>(getpid()));
    FILE* f = std::fopen(path, "w");
    if (f) std::setvbuf(f, nullptr, _IOLBF, 0);
    return f;
  }();
  return map;
}

static void perf_map_add(const Block* b, bool arm9) {
  FILE* map = perf_map_file();
  if (!map) return;
  std::fprintf(map, "%llx %x jit%c_%08x%s\n", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(b->entry)),
               b->size, arm9 ? '9' : '7', key_pc(b->key), key_thumb(b->key) ? "t" : "");
}

static void perf_map_stubs(const Runtime& r) {
  FILE* map = perf_map_file();
  if (!map) return;
  std::vector<std::pair<const u8*, std::string>> v;
  auto add = [&](const void* p, const char* name) { if (p) v.emplace_back(static_cast<const u8*>(p), name); };
  add(reinterpret_cast<const void*>(r.enter), "jit_stub_enter"); add(r.enter_light, "jit_stub_enter_light");
  add(reinterpret_cast<const void*>(r.run_loop), "jit_stub_run_loop");
  add(r.exit_key, "jit_stub_exit_key"); add(r.exit_key_lit, "jit_stub_exit_key_lit"); add(r.exit_r15, "jit_stub_exit_r15");
  add(r.call_pure, "jit_stub_call_pure"); add(r.call_full, "jit_stub_call_full"); add(r.call2, "jit_stub_call2");
  add(r.poll, "jit_stub_poll"); add(r.flush_exit, "jit_stub_flush_exit");
  static const char* const sz[3] = {"8", "16", "32"};
  for (int i = 0; i < 3; ++i) { add(r.slow_load[i], (std::string("jit_stub_slow_load") + sz[i]).c_str()); add(r.slow_store[i], (std::string("jit_stub_slow_store") + sz[i]).c_str()); }
  add(r.merge_keep_cv, "jit_stub_merge_keep_cv"); add(r.merge_set_c, "jit_stub_merge_set_c");
  for (int c = 0; c < 2; ++c) {
    const JitCpu& jc = r.cpus[c];
    const std::string pre = c == 0 ? "jit_stub9_" : "jit_stub7_";
    add(jc.dispatch, (pre + "dispatch").c_str()); add(jc.link, (pre + "link").c_str()); add(jc.fallback, (pre + "fallback").c_str());
    add(jc.branch_indirect, (pre + "branch_indirect").c_str()); add(jc.branch_indirect_cdi, (pre + "branch_indirect_cdi").c_str());
  }
  std::sort(v.begin(), v.end());
  for (size_t i = 0; i < v.size(); ++i) {
    const u8* end = i + 1 < v.size() ? v[i + 1].first : r.arena + r.stubs_end;
    std::fprintf(map, "%llx %llx %s\n", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(v[i].first)),
                 static_cast<unsigned long long>(end - v[i].first), v[i].second.c_str());
  }
}

// Unmapped reads as zero. Emulation thread only.
static void copy_guest_bytes(JitCpu& jc, u32 pc, u8* dst, u32 len) {
  u32 done = 0;
  while (done < len) {
    const u32 a = pc + done;
    const u32 room = std::min<u32>(len - done, mem::PAGE_SIZE - (a & (mem::PAGE_SIZE - 1)));
    if (const u8* src = jc.ctx->page_table.read_ptr(a)) std::memcpy(dst + done, src, room);
    else std::memset(dst + done, 0, room);
    done += room;
  }
}
// FNV-1a over the block's watched range, page by page (unmapped bytes as zero).
static u64 span_hash(JitCpu& jc, const Block* b) {
  u64 h = 1469598103934665603ull;
  u32 a = b->span_lo;
  while (a <= b->span_hi) {
    const u32 room = std::min<u32>(b->span_hi - a + 1, mem::PAGE_SIZE - (a & (mem::PAGE_SIZE - 1)));
    const u8* p = jc.ctx->page_table.read_ptr(a);
    for (u32 i = 0; i < room; ++i) h = (h ^ (p ? p[i] : 0)) * 1099511628211ull;
    a += room;
  }
  return h;
}
static bool guest_bytes_match(JitCpu& jc, const Block* b) {
  u8 cur[GUEST_COPY_MAX];
  copy_guest_bytes(jc, key_pc(b->key), cur, b->guest_copy_len);
  return std::memcmp(cur, b->guest_copy, b->guest_copy_len) == 0;
}

static void register_block(JitCpu& jc, Block* b) {
  Runtime& r = g_rt;
  const u32 pc = key_pc(b->key);
  const bool span = b->span_hi != 0;
  const u8* p0 = jc.ctx->page_table.read_ptr(span ? b->span_lo : pc);
  const u8* p1 = jc.ctx->page_table.read_ptr(span ? b->span_hi : pc + b->guest_len - 1);
  b->npages = 0;
  b->host_lo = p0; b->host_hi = p1;
  if (p0) b->host_pages[b->npages++] = host_page_of(p0);
  if (p1 && host_page_of(p1) != (p0 ? host_page_of(p0) : nullptr)) b->host_pages[b->npages++] = host_page_of(p1);
  for (u32 i = 0; i < b->npages; ++i) {
    auto& v = r.code_pages[b->host_pages[i]];
    if (v.empty()) set_code_tag(b->host_pages[i], true);
    u32 lo, hi;
    page_span(b, b->host_pages[i], lo, hi);
    v.add(b, lo, hi);
  }
  jc.blocks[b->key] = b;
  lut_insert(jc, b);
}

// Revive a parked translation whose guest bytes match again; same timing stamp only.
static Block* revive(JitCpu& jc, u32 key) {
  auto it = jc.parked.find(key);
  if (it == jc.parked.end()) { if (churn::on()) churn::revive_none++; return nullptr; }
  std::vector<Block*>& v = it->second;
  const u64 stamp = jc.nds->bus.timing().stamp;
  for (size_t k = v.size(); k-- > 0;) {
    Block* b = v[k];
    // A spanned block is checked by its range hash (the successor's bytes it
    // read decide the flags it keeps at the exit); others by their own bytes.
    const bool same = b->stamp == stamp && (b->span_hi ? span_hash(jc, b) == b->span_hash
                                                        : (b->guest_len == b->guest_copy_len && guest_bytes_match(jc, b)));
    if (churn::on()) {
      if (b->stamp != stamp) churn::revive_stamp++; else if (!b->span_hi && b->guest_len != b->guest_copy_len) churn::revive_len++;
      else if (!same) churn::revive_bytes++; else churn::revive_ok++;
    }
    if (!same) continue;
    v.erase(v.begin() + static_cast<std::ptrdiff_t>(k));
    if (v.empty()) jc.parked.erase(it);
    std::memcpy(b->entry, b->entry_words, backend::ENTRY_PATCH);
    sync_icache(b->entry, backend::ENTRY_PATCH);
    b->dead = false;
    register_block(jc, b);
    g_rt.stats.blocks_revived++;
    return b;
  }
  return nullptr;
}

static void install(JitCpu& jc, Block* b) {
  Runtime& r = g_rt;
  if (r.debug) {
    std::fprintf(stderr, "[jit] block %08x (%u bytes):", b->key, b->size);
    for (u32 i = 0; i < b->size; i += 4) { u32 w; std::memcpy(&w, b->entry + i, 4); std::fprintf(stderr, " %08x", w); }
    std::fputc('\n', stderr);
  }
  perf_map_add(b, jc.arm9);
  // Revival checks against these.
  b->stamp = jc.nds->bus.timing().stamp;
  b->guest_copy_len = std::min<u32>(b->guest_len, GUEST_COPY_MAX);
  copy_guest_bytes(jc, key_pc(b->key), b->guest_copy, b->guest_copy_len);
  if (b->span_hi) b->span_hash = span_hash(jc, b);
  register_block(jc, b);
  jc.all_blocks.push_back(b);
  r.blocks_live++;
  r.stats.blocks_translated++;
  r.stats.code_bytes += b->size;
  r.stats.hot_bytes += b->hot_size;
}

static void fm_drain(Runtime& r) {
  const u32 n = r.fm_ring_n;
  for (u32 i = 0; i < n && i < std::size(r.fm_ring); ++i) {
    r.fm_slow.insert(r.fm_ring[i]);
    const u32 b = Runtime::fm_bit(r.fm_ring[i]);
    r.fm_slow_bits[b >> 6] |= u64{1} << (b & 63);
  }
  r.fm_ring_n = 0;
}

Block* translate(JitCpu& jc, u32 key) {
  DS_PROF(JIT_TX);   // nested in the CPU9/CPU7 slice
  Runtime& r = g_rt;
  if (r.fm_ring_n) fm_drain(r);
  if (churn::on()) {
    churn::trans++;
    const u64 f = jc.ctx->nds->frame_count;
    churn::by_region[(jc.arm9 ? 0u : 0x100u) | (key_pc(key) >> 24)]++;
    if (churn::sample_frame == ~0ull) { const char* e = std::getenv("DS_CHURN_FRAME"); churn::sample_frame = e ? static_cast<u64>(std::atoll(e)) : 0; }
    if (f == churn::sample_frame && churn::sampled < 80) { ++churn::sampled; std::fprintf(stderr, "[churn] frame %llu arm%d key %08x%s r15 %08x\n", (unsigned long long)f, jc.arm9 ? 9 : 7, key_pc(key), key_thumb(key) ? "t" : "", jc.ctx->hot.regs[15]); }
    churn::trans_by_frame[f]++;
    if (churn::retrans[key_pc(key)]++ > 0) churn::retrans_by_frame[f]++;
    const auto pw = churn::page_last_write.find(key_pc(key) & ~(mem::PAGE_SIZE - 1));
    if (pw != churn::page_last_write.end()) {
      const u64 dt = jc.ctx->nds->sched.now() - pw->second;
      const u64 ms01 = 112038;    // ~0.1 ms of ARM9 cycles
      const int bucket = dt < ms01 ? 0 : dt < ms01 * 10 ? 1 : dt < ms01 * 40 ? 2 : dt < ms01 * 160 ? 3 : dt < ms01 * 640 ? 4 : 5;
      churn::lead_hist[bucket]++;
      churn::lead_n++; churn::lead_sum += dt;
    }
  }
  if (r.pos + BLOCK_MARGIN > r.cap || r.blocks_live >= MAX_BLOCKS) return nullptr;   // caller resets the arena
  Block* b = &r.block_pool.emplace_back(Block{});
  b->key = key;
  b->owner = jc.arm9 ? 0 : 1;
  b->pooled = true;
  u32 size = 0;
  r.fm_new.clear();
  {
    DS_PROF(JIT_TX_GEN);
    if (!backend::translate_block(jc, key, r.arena + r.pos, BLOCK_MARGIN, *b, size)) { r.block_pool.pop_back(); r.fm_new.clear(); return nullptr; }
  }
  b->entry = r.arena + r.pos;
  b->size = size;
  if (!r.fm_new.empty()) {
    r.fm_blocks.push_back({b->entry, b->size, static_cast<u32>(r.fm_rels.size()), static_cast<u32>(r.fm_new.size())});
    r.fm_rels.insert(r.fm_rels.end(), r.fm_new.begin(), r.fm_new.end());
    r.fm_new.clear();
  }
  r.pos += (b->size + 15) & ~size_t{15};
  { DS_PROF(JIT_TX_ICACHE); sync_icache(b->entry, b->size); }
  { DS_PROF(JIT_TX_INSTALL); install(jc, b); }
  return b;
}

const u8* find_native(JitCpu& jc, u32 key) {
  auto it = jc.blocks.find(key);
  if (it != jc.blocks.end()) { lut_insert(jc, it->second); return it->second->entry; }
  if (Block* b = revive(jc, key)) return b->entry;
  Block* b = translate(jc, key);
  return b ? b->entry : nullptr;
}

void invalidate_host_range(const u8* host_page, const u8* lo, const u8* hi) {
  auto it = g_rt.code_pages.find(host_page);
  if (it == g_rt.code_pages.end()) return;
  Runtime::PageBlocks& list = it->second;
  const u32 slo = static_cast<u32>(lo - host_page), shi = static_cast<u32>(hi - host_page);
  std::vector<Block*> victims;
  for (size_t k = 0; k < list.span.size();) {
    const u32 sp = list.span[k];
    if (slo <= (sp >> 16) && shi >= (sp & 0xFFFF)) { victims.push_back(list.blocks[k]); list.remove_at(k); }
    else ++k;
  }
  if (victims.empty()) { churn::range_miss++; return; }
  if (list.empty()) { g_rt.code_pages.erase(it); set_code_tag(host_page, false); }
  if (churn::on()) for (Block* b : victims) churn::store_sites[(static_cast<u64>(key_pc(b->key)) << 32) | ((key_pc(b->key) & ~(mem::PAGE_SIZE - 1)) + slo)]++;
  invalidate_blocks(host_page, std::move(victims));
}

// Victims are already unlinked from host_page's list.
static void invalidate_blocks(const u8* host_page, std::vector<Block*> victims) {
  if (churn::on()) {
    static bool reg = (std::atexit(churn::report), true); (void)reg;
    CpuContext* ctx = g_rt.cpus[0].ctx ? g_rt.cpus[0].ctx : g_rt.cpus[1].ctx;
    const CpuContext* run = ctx->nds->sched.running();
    const bool dma = ctx->nds->sched.in_dma();
    const u8 cpu = run ? (run->which == Cpu::ARM9 ? 9 : 7) : 0;
    const u32 pc = run ? run->hot.regs[15] : 0;
    churn::writers[churn::Key{pc, dma, cpu}]++;
    churn::inval++; churn::killed += victims.size(); churn::frames_seen = ctx->nds->frame_count;
    for (Block* b : victims) {
      churn::victims_by_page[key_pc(b->key) & ~(mem::PAGE_SIZE - 1)]++;
      churn::page_last_write[key_pc(b->key) & ~(mem::PAGE_SIZE - 1)] = ctx->nds->sched.now();
    }
  }
  for (Block* b : victims) {
    JitCpu& jc = jc_of(b);
    kill_block(jc, b);
    for (u32 i = 0; i < b->npages; ++i) if (b->host_pages[i] != host_page) {
      auto jt = g_rt.code_pages.find(b->host_pages[i]);
      if (jt == g_rt.code_pages.end()) continue;
      jt->second.remove(b);
      if (jt->second.empty()) { g_rt.code_pages.erase(jt); set_code_tag(b->host_pages[i], false); }
    }
  }
  for (JitCpu& jc : g_rt.cpus) if (jc.ctx) jc.ctx->hot.alerts |= ALERT_INVALIDATED;
}

void invalidate_cpu(JitCpu& jc) {
  for (Block* b : jc.all_blocks) if (!b->dead) { remove_from_page_lists(b); kill_block(jc, b); }
  jc.blocks.clear();
  jc.parked.clear();
  if (jc.ctx) jc.ctx->hot.alerts |= ALERT_INVALIDATED;
}

// ---- helpers called from translated code ----

extern "C" u32 jit_h_fallback(CpuContext* cpu, u32 instr, u32 key) {
  if (instr == BIOS_SHA1_MARKER) return bios_sha1_run(*cpu) ? 1 : 0;   // key is LOOP-4, so a poll resumes at LOOP
  if (instr == WAIT_LOOP_MARKER) {   // key is LOOP-2: a poll resumes at LOOP, and the block's own loop runs what is left
    cpu->hot.regs[15] = key_r15(key) + 2;
    if (cpu::wait_loop_run(*cpu)) g_rt.stats.wait_loop_runs++;
    return 0;
  }
  cpu->hot.regs[15] = key_r15(key);
  cpu->data_cycles = 0;
  cpu->jumped = false;
  if (cpu->which == Cpu::ARM9) prefetch_cost9(*cpu);
  else { cpu->code_cycles = key_pc(key) >> 15; cpu->code_region = key_pc(key) >> 24; }
  if (key_thumb(key)) {
    interp::exec_thumb(*cpu, static_cast<u16>(instr));
    if (!cpu->jumped) cpu->hot.regs[15] += 2;
  } else {
    interp::exec_arm(*cpu, instr);
    if (!cpu->jumped) cpu->hot.regs[15] += 4;
  }
  if (cpu->halted) cpu->hot.alerts |= ALERT_HALTED;
  g_rt.stats.instrs_fallback++;
  if (g_rt.hist) g_rt.fallback_hist[(static_cast<u64>(cpu->which) << 32) | key_pc(key)]++;
  if (g_rt.debug) std::fprintf(stderr, "[jit] fallback %08x %08x -> r15 %08x cpsr %08x budget %d halted %d jumped %d\n", key_pc(key), instr, cpu->hot.regs[15], cpu->hot.cpsr, cpu->hot.cycle_budget, cpu->halted, cpu->jumped);
  return cpu->jumped ? 1 : 0;
}

// DS_JIT_WARM=N: lookups of a key before it is translated; 1 (the default)
// translates on first sight. Counted on the C side (every cold execution
// reaches it: the native paths exit for cold code), so at 2 a key's first
// execution is interpreted and its second translated. Opt-in: at 2 it took
// GS:DD's load bursts down but cost ST 0.2 ms a frame, since the interpreter
// takes the whole rest of the slice, hot code after the cold block included.
// Interpreting just the cold block would need a one-block interpreter entry.
static u32 warm_threshold() { static const u32 v = std::getenv("DS_JIT_WARM") ? static_cast<u32>(std::atoi(std::getenv("DS_JIT_WARM"))) : 1; return v; }
static bool is_cold(JitCpu& jc, u32 key) {
  if (warm_threshold() <= 1) return false;
  if (jc.blocks.count(key)) return false;
  return static_cast<u32>(jc.warm[(key >> 1) & (LUT_SIZE - 1)]) + 1 < warm_threshold();
}
static bool cold_after_count(JitCpu& jc, u32 key) {
  if (warm_threshold() <= 1) return false;
  if (jc.blocks.count(key)) return false;
  u8& n = jc.warm[(key >> 1) & (LUT_SIZE - 1)];
  if (n < 255) ++n;
  return n < warm_threshold();
}

static void weeds_check(CpuContext& cpu, u32 pc);
extern "C" const void* jit_h_lookup(CpuContext* cpu, u32 key) {
  JitCpu& jc = *static_cast<JitCpu*>(cpu->jit);
  weeds_check(*cpu, key_pc(key));
  if (is_cold(jc, key)) { cpu->hot.regs[15] = key_r15(key); return g_rt.exit_r15; }   // the C side interprets it
  const u8* native = find_native(jc, key);
  if (g_rt.debug) std::fprintf(stderr, "[jit] lookup %08x -> %p (budget %d)\n", key, static_cast<const void*>(native), cpu->hot.cycle_budget);
  if (native) return native;
  cpu->hot.regs[15] = key_r15(key);
  return g_rt.flush_exit;   // arena full
}

extern "C" const void* jit_h_link(CpuContext* cpu, u32 key, u8* patch_site) {
  JitCpu& jc = *static_cast<JitCpu*>(cpu->jit);
  if (is_cold(jc, key)) { cpu->hot.regs[15] = key_r15(key); return g_rt.exit_r15; }   // unpatched: linked once the target is warm
  const u8* native = find_native(jc, key);
  if (g_rt.debug) std::fprintf(stderr, "[jit] link %08x -> %p at %p (budget %d)\n", key, static_cast<const void*>(native), static_cast<void*>(patch_site), cpu->hot.cycle_budget);
  if (!native) { cpu->hot.regs[15] = key_r15(key); return g_rt.flush_exit; }
  backend::patch_link(patch_site, native);
  sync_icache(patch_site, 4);
  return native;
}

extern "C" void jit_h_cyclog(CpuContext* cpu, u32 instr, u32 key) {
  (void)instr;
  std::fprintf(stderr, "[cyc%d] %08x %d\n", cpu->which == Cpu::ARM9 ? 9 : 7, key_r15(key), cpu->hot.cycle_budget);
}

extern "C" void jit_h_trace(CpuContext* cpu, u32 instr, u32 key) {
  cpu->hot.regs[15] = key_r15(key);
  if (cpu->nds->trace) cpu->nds->trace(*cpu, instr, cpu->nds->trace_user);
}

// call_pure has already stored host flags into hot.cpsr.
extern "C" void jit_h_msr_cpsr(CpuContext* cpu, u32 value, u32 mask) {
  if ((cpu->hot.cpsr & 0x1F) == 0x10) mask &= 0xFF000000;   // user mode: flags only
  mask &= ~0x00000020u;                                     // T not writable via MSR
  cpu->set_cpsr((cpu->hot.cpsr & ~mask) | (value & mask));
}

// SUBS/MOVS pc etc.: returns target with the restored T in bit 0.
extern "C" u32 jit_h_exc_return(CpuContext* cpu, u32 target) {
  cpu->restore_cpsr();
  return (target & ~1u) | ((cpu->hot.cpsr >> 5) & 1u);
}

// Slow path (MMIO, unmapped, read-only, code pages); the block charges cost itself.
extern "C" u32 jit_h_ld8(CpuContext* cpu, u32 addr) {
  g_rt.stats.slow_accesses++;
  if (u8* p = cpu->page_table.read_ptr(addr)) return *p;
  if ((addr & 0xFF000000) == 0x04000000) return cpu->nds->io.read(cpu->which, addr, 8);
  return cpu->nds->bus.read8(cpu->which, addr);
}
extern "C" u32 jit_h_ld16(CpuContext* cpu, u32 addr) {
  g_rt.stats.slow_accesses++;
  addr &= ~1u;
  if (u8* p = cpu->page_table.read_ptr(addr)) { u16 v; std::memcpy(&v, p, 2); return v; }
  if ((addr & 0xFF000000) == 0x04000000) return cpu->nds->io.read(cpu->which, addr, 16);
  return cpu->nds->bus.read16(cpu->which, addr);
}
extern "C" u32 jit_h_ld32(CpuContext* cpu, u32 addr) {
  g_rt.stats.slow_accesses++;
  addr &= ~3u;
  if (u8* p = cpu->page_table.read_ptr(addr)) { u32 v; std::memcpy(&v, p, 4); return v; }
  if ((addr & 0xFF000000) == 0x04000000) return cpu->nds->io.read(cpu->which, addr, 32);
  return cpu->nds->bus.read32(cpu->which, addr);
}
extern "C" void jit_h_st8(CpuContext* cpu, u32 addr, u32 v) {
  g_rt.stats.slow_accesses++;
  bool code = false;
  if (u8* p = cpu->page_table.write_ptr(addr, &code)) { const u8 b8 = static_cast<u8>(v); if (code) mem::store_code(p, &b8, 1); else *p = b8; }
  else if ((addr & 0xFF000000) == 0x04000000) cpu->nds->io.write(cpu->which, addr, 8, v);
  else cpu->nds->bus.write8(cpu->which, addr, static_cast<u8>(v));
  if (cpu->halted) cpu->hot.alerts |= ALERT_HALTED;
}
extern "C" void jit_h_st16(CpuContext* cpu, u32 addr, u32 v) {
  g_rt.stats.slow_accesses++;
  addr &= ~1u;
  bool code = false;
  const u16 h = static_cast<u16>(v);
  if (u8* p = cpu->page_table.write_ptr(addr, &code)) { if (code) mem::store_code(p, &h, 2); else std::memcpy(p, &h, 2); }
  else if ((addr & 0xFF000000) == 0x04000000) cpu->nds->io.write(cpu->which, addr, 16, v);
  else cpu->nds->bus.write16(cpu->which, addr, h);
  if (cpu->halted) cpu->hot.alerts |= ALERT_HALTED;
}
extern "C" void jit_h_st32(CpuContext* cpu, u32 addr, u32 v) {
  g_rt.stats.slow_accesses++;
  addr &= ~3u;
  bool code = false;
  if (u8* p = cpu->page_table.write_ptr(addr, &code)) { if (code) mem::store_code(p, &v, 4); else std::memcpy(p, &v, 4); }
  // Bus::io_write, not Io::write: it has the ARM9 GXFIFO fast path.
  else if ((addr & 0xFF000000) == 0x04000000) cpu->nds->bus.io_write(cpu->which, addr, 32, v);
  else cpu->nds->bus.write32(cpu->which, addr, v);
  if (cpu->halted) cpu->hot.alerts |= ALERT_HALTED;
}

// ---- fastmem faults ----
// Fault at a registered site: patch it to its cold walk and resume there.
// Anything else goes to the previous handler.
static struct sigaction g_old_segv, g_old_bus;

static void fm_fault(int sig, siginfo_t* si, void* uctx) {
  Runtime& r = g_rt;
  uintptr_t pc = 0;
  ucontext_t* uc = static_cast<ucontext_t*>(uctx);
#if defined(__aarch64__)
  pc = static_cast<uintptr_t>(uc->uc_mcontext.pc);
#elif defined(__arm__)
  pc = static_cast<uintptr_t>(uc->uc_mcontext.arm_pc);
#endif
  const Runtime::FmRel* site = nullptr;
  const u8* block = nullptr;
  {
    auto it = std::upper_bound(r.fm_blocks.begin(), r.fm_blocks.end(), pc,
                               [](uintptr_t p, const Runtime::FmBlock& b) { return p < reinterpret_cast<uintptr_t>(b.entry); });
    if (it != r.fm_blocks.begin()) {
      const Runtime::FmBlock& b = *(it - 1);
      const uintptr_t off = pc - reinterpret_cast<uintptr_t>(b.entry);
      if (off < b.size)
        for (u32 k = 0; k < b.count; ++k)
          if (r.fm_rels[b.first + k].fault == off) { site = &r.fm_rels[b.first + k]; block = b.entry; break; }
    }
  }
  bool in_view = false;
  const uintptr_t addr = reinterpret_cast<uintptr_t>(si->si_addr);
  for (int c = 0; c < 2; ++c) {
    const JitCpu& jc = r.cpus[c];
    if (!jc.fastmem) continue;
    const mem::GuestView* v = jc.nds->bus.view(c == 0 ? Cpu::ARM9 : Cpu::ARM7);
    if (v && v->contains(addr)) in_view = true;
  }
  if (r.fm_guard && addr >= reinterpret_cast<uintptr_t>(r.fm_guard) && addr - reinterpret_cast<uintptr_t>(r.fm_guard) < mem::GuestView::RESERVE) in_view = true;
  if (!site || !in_view) {
    const struct sigaction& old = sig == SIGBUS ? g_old_bus : g_old_segv;
    if (old.sa_flags & SA_SIGINFO) { if (old.sa_sigaction) { old.sa_sigaction(sig, si, uctx); return; } }
    else if (old.sa_handler != SIG_DFL && old.sa_handler != SIG_IGN) { old.sa_handler(sig); return; }
    signal(sig, SIG_DFL);   // re-executes and dies
    return;
  }
  // View pages are mapped on demand; if that was all, retry in place.
  for (int c = 0; c < 2; ++c) {
    const JitCpu& jc = r.cpus[c];
    if (!jc.fastmem) continue;
    mem::GuestView* v = jc.nds->bus.view(c == 0 ? Cpu::ARM9 : Cpu::ARM7);
    if (v && v->contains(addr) && v->fault(addr)) return;
  }
  u8* const patch = const_cast<u8*>(block) + site->patch;
  const u8* const resume = block + site->resume;
  backend::patch_link(patch, resume);
  sync_icache(patch, 4);
  if (r.fm_ring_n < std::size(r.fm_ring)) r.fm_ring[r.fm_ring_n++] = site->guest;
  ++r.fm_faults;
#if defined(__aarch64__)
  uc->uc_mcontext.pc = reinterpret_cast<u64>(resume);
#elif defined(__arm__)
  uc->uc_mcontext.arm_pc = static_cast<unsigned long>(reinterpret_cast<uintptr_t>(resume));
#endif
}

static void install_fault_handler() {
  static bool done = false;
  if (done) return;
  done = true;
  struct sigaction sa {};
  sa.sa_sigaction = fm_fault;
  sa.sa_flags = SA_SIGINFO | SA_NODEFER;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGSEGV, &sa, &g_old_segv);
  sigaction(SIGBUS, &sa, &g_old_bus);
}

bool attach(NDS& nds, bool arm9, bool arm7) {
  Runtime& r = g_rt;
  // The stubs bake in the timing model (refill); a change between attaches
  // (tests run both) drops every block and emits them again.
  if (r.arena && r.stubs_fast != g_fast_timing) {
    reset_arena();
    backend::emit_stubs(r);
    r.stubs_fast = g_fast_timing;
  }
  if (!r.arena) {
    void* p = mmap(nullptr, ARENA_BYTES, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) { std::fprintf(stderr, "jit: cannot map code arena\n"); return false; }
    r.arena = static_cast<u8*>(p);
    r.cap = ARENA_BYTES;
    for (int c = 0; c < 2; ++c) {
      r.cpus[c].lut = reinterpret_cast<u64*>(r.arena + c * LUT_STRIDE);
      for (u32 i = 0; i < LUT_SIZE; ++i) r.cpus[c].lut[i] = LUT_EMPTY_KEY;
    }
    backend::emit_stubs(r);
    r.stubs_fast = g_fast_timing;
    perf_map_stubs(r);
    r.strict = std::getenv("DS_JIT_STRICT") != nullptr;
    r.debug = std::getenv("DS_JIT_DEBUG") != nullptr;
    r.cyclog = std::getenv("DS_DEBUG_CYCLES") != nullptr;
    r.hist = std::getenv("DS_JIT_HIST") != nullptr;
    r.density = std::getenv("DS_JIT_DENSITY") != nullptr;
    r.census = std::getenv("DS_JIT_CENSUS") != nullptr;
    if (r.census) r.density = true;
    mem::PageTable::code_query = &code_query;
    mem::code_write_hook = &code_write_hook;
  }
  r.trace = nds.trace != nullptr;
  for (int c = 0; c < 2; ++c) {
    const bool on = c == 0 ? arm9 : arm7;
    if (!on) continue;
    JitCpu& jc = r.cpus[c];
    CpuContext& ctx = nds.cpu(c == 0 ? Cpu::ARM9 : Cpu::ARM7);
    jc.ctx = &ctx;
    jc.nds = &nds;
    jc.arm9 = c == 0;
    jc.hot.pt = ctx.page_table.raw();
    jc.hot.table = ctx.page_table.raw();
    jc.fastmem = false;
    if (const mem::GuestView* v = nds.bus.view(c == 0 ? Cpu::ARM9 : Cpu::ARM7); v && backend::fastmem_capable()) {
#if UINTPTR_MAX > 0xFFFFFFFFu
      jc.fastmem = true;
      jc.hot.pt = reinterpret_cast<mem::Entry*>(v->base());
#else
      if (!r.fm_guard) {
        void* g = mmap(nullptr, mem::GuestView::RESERVE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (g != MAP_FAILED) r.fm_guard = static_cast<u8*>(g);
      }
      if (r.fm_guard) {
        const u32 guard = static_cast<u32>(reinterpret_cast<uintptr_t>(r.fm_guard));
        for (u32 k = 0; k < 64; ++k) jc.fm_region[k] = k == 0 ? static_cast<u32>(reinterpret_cast<uintptr_t>(v->base())) : guard - (k << 26);
        jc.fastmem = true;
        jc.hot.pt = reinterpret_cast<mem::Entry*>(jc.fm_region);
      }
#endif
      if (jc.fastmem) install_fault_handler();
    }
    jc.hot.timing = c == 0 ? reinterpret_cast<const u8*>(ctx.timing9) : reinterpret_cast<const u8*>(ctx.timing7);
    jc.hot.arena = r.arena;
    jc.blocks.reserve(1u << 15);
    jc.all_blocks.reserve(1u << 15);
    r.code_pages.reserve(1u << 13);
    ctx.jit = &jc;
    ctx.jit_timing_changed = &on_timing_changed;
    (c == 0 ? nds.run_arm9 : nds.run_arm7) = &run;
  }
  return true;
}

void detach(NDS& nds) {
  for (int c = 0; c < 2; ++c) {
    JitCpu& jc = g_rt.cpus[c];
    if (!jc.ctx) continue;
    invalidate_cpu(jc);
    jc.ctx->jit = nullptr;
    jc.ctx->jit_timing_changed = nullptr;
    (c == 0 ? nds.run_arm9 : nds.run_arm7) = &interp::run;
    jc.ctx = nullptr;
  }
}

void flush(CpuContext& cpu) { if (cpu.jit) invalidate_cpu(*static_cast<JitCpu*>(cpu.jit)); }
void flush_all() { reset_arena(); }
void set_trace(bool on) { if (g_rt.trace != on) { g_rt.trace = on; for (JitCpu& jc : g_rt.cpus) if (jc.ctx) invalidate_cpu(jc); } }
void set_strict(bool on) { if (g_rt.strict != on) { g_rt.strict = on; for (JitCpu& jc : g_rt.cpus) if (jc.ctx) invalidate_cpu(jc); } }
const Stats& stats() { return g_rt.stats; }

static double ex_total_pct(unsigned long long v, unsigned long long tot) {
  return tot ? 100.0 * static_cast<double>(v) / static_cast<double>(tot) : 0.0;
}

void density_reset() { for (DensitySlot& d : g_rt.density_slots) d.execs = 0; }

DensitySlot* density_new_slot() {
  Runtime& r = g_rt;
  if (!r.density) return nullptr;
  r.density_slots.emplace_back();
  return &r.density_slots.back();
}

void report(std::FILE* out) {
  const Stats& s = g_rt.stats;
  // DS_JIT_DUMP=<path>: dump the arena at exit after a "<base> <size>" line.
  if (const char* dump = std::getenv("DS_JIT_DUMP")) {
    if (FILE* f = std::fopen(dump, "wb")) {
      std::fprintf(f, "%llx %llx\n", (unsigned long long)reinterpret_cast<uintptr_t>(g_rt.arena), (unsigned long long)g_rt.pos);
      std::fwrite(g_rt.arena, 1, g_rt.pos, f);
      std::fclose(f);
    }
  }
  std::fprintf(out, "[jit] blocks %llu, inline instrs %llu, fallback executions %llu, slow accesses %llu, entries %llu, invalidated %llu, revived %llu, flushes %llu\n",
               (unsigned long long)s.blocks_translated, (unsigned long long)s.instrs_translated, (unsigned long long)s.instrs_fallback,
               (unsigned long long)s.slow_accesses, (unsigned long long)s.entries, (unsigned long long)s.blocks_invalidated, (unsigned long long)s.blocks_revived, (unsigned long long)s.flushes);
  if (g_rt.cpus[0].fastmem || g_rt.cpus[1].fastmem)
    std::fprintf(out, "[jit] fastmem: %llu site faults (rewritten to the walk), %zu guest sites on the walk, %zu registered sites\n",
                 static_cast<unsigned long long>(g_rt.fm_faults), g_rt.fm_slow.size() + g_rt.fm_ring_n, g_rt.fm_rels.size());
  if (s.bios_sha1_blocks) std::fprintf(out, "[jit] DSi BIOS SHA-1 blocks run natively: %llu\n", (unsigned long long)s.bios_sha1_blocks);
  if (s.slices_interpreted) std::fprintf(out, "[jit] cold code: %llu slices interpreted (DS_JIT_WARM)\n", (unsigned long long)s.slices_interpreted);
  if (const cpu::WaitLoopStats& w = cpu::wait_loop_stats(); w.runs)
    std::fprintf(out, "[jit] ARM7 WaitByLoop fast-forwarded %llu times: %llu iterations, %llu ARM7 cycles\n", (unsigned long long)w.runs, (unsigned long long)w.iterations, (unsigned long long)w.cycles);
  std::fprintf(out, "[jit] code %llu KB (hot %llu KB): %.1f bytes per guest instruction, %.1f hot\n", (unsigned long long)(s.code_bytes >> 10), (unsigned long long)(s.hot_bytes >> 10),
               s.instrs_translated ? static_cast<double>(s.code_bytes) / static_cast<double>(s.instrs_translated) : 0.0,
               s.instrs_translated ? static_cast<double>(s.hot_bytes) / static_cast<double>(s.instrs_translated) : 0.0);
  if (g_rt.census) {
    // Execution-weighted.
    long double ex = 0, gi = 0, fb = 0, mem = 0, memfl = 0, memfli = 0, fl = 0, efl = 0;
    long double rr[16] = {}, rw[16] = {}, li[16] = {}, wr[16] = {};
    long double len_hist[8] = {};   // 1-2, 3-4, 5-8, 9-16, 17-32, 33-64
    for (const DensitySlot& d : g_rt.density_slots) {
      const long double e = static_cast<long double>(d.execs);
      ex += e; gi += e * d.n_instrs; fb += e * d.n_fallback; mem += e * d.n_mem; memfl += e * d.n_mem_flags_live; memfli += e * d.n_mem_flags_intra;
      fl += e * d.n_flags_live; efl += d.entry_flags_live ? e : 0;
      for (int r = 0; r < 16; ++r) {
        rr[r] += e * d.reg_reads[r]; rw[r] += e * d.reg_writes[r];
        if (d.live_in & (1u << r)) li[r] += e;
        if (d.written & (1u << r)) wr[r] += e;
      }
      const u32 n = d.n_instrs;
      len_hist[n <= 2 ? 0 : n <= 4 ? 1 : n <= 8 ? 2 : n <= 16 ? 3 : n <= 32 ? 4 : 5] += e;
    }
    std::fprintf(out, "[census] translations %zu entries %.0Lf guest_instrs %.0Lf fallback_instrs %.0Lf\n", g_rt.density_slots.size(), ex, gi, fb);
    std::fprintf(out, "[census] instrs_per_entry %.2Lf fallback_pct %.2Lf\n", ex ? gi / ex : 0, gi ? 100 * fb / gi : 0);
    std::fprintf(out, "[census] mem_instrs %.0Lf mem_nzcv_live %.0Lf mem_nzcv_live_pct %.2Lf mem_nzcv_intra %.0Lf mem_nzcv_intra_pct %.2Lf\n", mem, memfl, mem ? 100 * memfl / mem : 0, memfli, mem ? 100 * memfli / mem : 0);
    std::fprintf(out, "[census] instrs_nzcv_live %.0Lf instrs_nzcv_live_pct %.2Lf entries_nzcv_live_at_entry %.0Lf entry_nzcv_live_pct %.2Lf\n", fl, gi ? 100 * fl / gi : 0, efl, ex ? 100 * efl / ex : 0);
    static const char* const lens[6] = {"1-2", "3-4", "5-8", "9-16", "17-32", "33-64"};
    for (int k = 0; k < 6; ++k) std::fprintf(out, "[census] block_len %s entries %.0Lf entries_pct %.2Lf\n", lens[k], len_hist[k], ex ? 100 * len_hist[k] / ex : 0);
    for (int r = 0; r < 16; ++r)
      std::fprintf(out, "[census] reg r%d reads %.0Lf writes %.0Lf live_in_entries %.0Lf written_entries %.0Lf reads_per_entry %.3Lf writes_per_entry %.3Lf live_in_pct %.2Lf written_pct %.2Lf\n",
                   r, rr[r], rw[r], li[r], wr[r], ex ? rr[r] / ex : 0, ex ? rw[r] / ex : 0, ex ? 100 * li[r] / ex : 0, ex ? 100 * wr[r] / ex : 0);
  }
  if (g_rt.density) {
    u64 ex = 0;
    long double hb = 0.0L, gi = 0.0L;
    for (const DensitySlot& d : g_rt.density_slots) {
      ex += d.execs;
      hb += static_cast<long double>(d.execs) * d.hot_bytes;
      gi += static_cast<long double>(d.execs) * d.guest_instrs;
    }
    std::fprintf(out, "[jit] density: %zu translations, %llu block entries, %.0Lf guest instrs executed, %.0Lf hot bytes fetched\n",
                 g_rt.density_slots.size(), (unsigned long long)ex, gi, hb);
    // From slots, not Stats: stats.hot_bytes includes the counter code.
    long double shb = 0.0L, sgi = 0.0L;
    for (const DensitySlot& d : g_rt.density_slots) { shb += d.hot_bytes; sgi += d.guest_instrs; }
    std::fprintf(out, "[jit] density: %.2Lf hot bytes per guest instruction executed, against %.2Lf translated (static)\n",
                 gi > 0.0L ? hb / gi : 0.0L, sgi > 0.0L ? shb / sgi : 0.0L);
    static const int lo[9] = {1, 2, 3, 4, 5, 9, 17, 33, 65};
    u64 tx[8] = {}, bex[8] = {};
    long double gx[8] = {};
    for (const DensitySlot& d : g_rt.density_slots) {
      int k = 0;
      while (k < 7 && static_cast<int>(d.guest_instrs) >= lo[k + 1]) ++k;
      tx[k]++; bex[k] += d.execs;
      gx[k] += static_cast<long double>(d.execs) * d.guest_instrs;
    }
    std::fprintf(out, "[jit] density: block length    translations        entries   %% entries   %% guest instrs\n");
    for (int k = 0; k < 8; ++k) {
      if (!tx[k] && !bex[k]) continue;
      char lab[16];
      if (lo[k + 1] - lo[k] == 1) std::snprintf(lab, sizeof lab, "%d", lo[k]);
      else std::snprintf(lab, sizeof lab, "%d-%d", lo[k], lo[k + 1] - 1);
      std::fprintf(out, "[jit]         %12s %14llu %14llu %10.2f %%     %10.2Lf %%\n", lab,
                   (unsigned long long)tx[k], (unsigned long long)bex[k],
                   ex_total_pct(bex[k], ex), gi > 0.0L ? 100.0L * gx[k] / gi : 0.0L);
    }
  }
  if (!g_rt.hist) return;
  std::vector<std::pair<u64, u64>> v(g_rt.fallback_hist.begin(), g_rt.fallback_hist.end());
  std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
  std::fprintf(out, "[jit] hottest fallbacks (cpu pc instr count):\n");
  for (size_t i = 0; i < v.size() && i < 16; ++i) {
    const Cpu c = static_cast<Cpu>(v[i].first >> 32);
    const u32 pc = static_cast<u32>(v[i].first);
    CpuContext& ctx = *g_rt.cpus[c == Cpu::ARM9 ? 0 : 1].ctx;
    u32 instr = 0;
    if (u8* p = ctx.page_table.read_ptr(pc)) std::memcpy(&instr, p, 4);
    std::fprintf(out, "[jit]   arm%d %08x %08x %llu\n", c == Cpu::ARM9 ? 9 : 7, pc, instr, (unsigned long long)v[i].second);
  }
}

// DS_JIT_WEEDS=1: the first time a CPU's pc leaves every region that can
// hold code, print its registers (the branch that went wrong is in lr/sp).
static void weeds_check(CpuContext& cpu, u32 pc) {
  static const bool on = std::getenv("DS_JIT_WEEDS") != nullptr;
  static bool shown[2] = {false, false};
  if (!on) return;
  const u32 r = pc >> 24;
  const bool ok = r <= 0x03 || r == 0x06 || r == 0x08 || r == 0x09 || r == 0xFF || r == 0x0A;
  const int i = cpu.which == Cpu::ARM9 ? 0 : 1;
  if (ok || shown[i]) return;
  shown[i] = true;
  std::fprintf(stderr, "[weeds] arm%d frame %llu pc %08x cpsr %08x lr %08x sp %08x r0-r3 %08x %08x %08x %08x r4-r7 %08x %08x %08x %08x r12 %08x\n",
               i ? 7 : 9, (unsigned long long)cpu.nds->frame_count, pc, cpu.hot.cpsr, cpu.hot.regs[14], cpu.hot.regs[13],
               cpu.hot.regs[0], cpu.hot.regs[1], cpu.hot.regs[2], cpu.hot.regs[3], cpu.hot.regs[4], cpu.hot.regs[5], cpu.hot.regs[6], cpu.hot.regs[7], cpu.hot.regs[12]);
  // The stack's top words: return addresses of the callers.
  for (u32 k = 0; k < 16; ++k) { u32 w = 0; if (const u8* p = cpu.page_table.read_ptr(cpu.hot.regs[13] + k * 4)) std::memcpy(&w, p, 4); std::fprintf(stderr, "[weeds]   [sp+%02x] %08x\n", k * 4, w); }
}

const void* lookup(CpuContext& cpu) {
  JitCpu& jc = *static_cast<JitCpu*>(cpu.jit);
  Runtime& r = g_rt;
  for (;;) {
    if (r.need_reset) reset_arena();
    const bool thumb = cpu.thumb();
    const u32 key = make_key(cpu.hot.regs[15] - (thumb ? 4 : 8), thumb);
    weeds_check(cpu, key_pc(key));
    const u64 lut = jc.lut[(key >> 1) & (LUT_SIZE - 1)];
    if (static_cast<u32>(lut) != key && cold_after_count(jc, key)) {
      // Cold: the interpreter takes the rest of the slice, and translated
      // code is entered only to leave (the state is already in ctx).
      interp::run(cpu);
      r.stats.slices_interpreted++;
      return r.exit_r15;
    }
    const u8* native = static_cast<u32>(lut) == key ? r.arena + (lut >> 32) : find_native(jc, key);
    if (!native) { reset_arena(); continue; }
    cpu.hot.alerts = 0;
    r.stats.entries++;
    return native;
  }
}

bool has_runtime() { return g_rt.arena != nullptr; }
void run_loop(void* scheduler) { g_rt.run_loop(scheduler); }

void run(CpuContext& cpu) {
  Runtime& r = g_rt;
  cpu.check_irq();
  if (cpu.halted) { cpu.hot.cycle_budget = -1; return; }
  if (cpu.step_limit) { interp::run(cpu); return; }
  while (cpu.hot.cycle_budget > 0) {
    r.enter(&cpu, lookup(cpu));
    if (cpu.halted) { cpu.budget_at_halt = cpu.hot.cycle_budget; cpu.hot.cycle_budget = -1; return; }
    if (cpu.hot.irq_pending) cpu.check_irq();
  }
}

} // namespace ds::jit
