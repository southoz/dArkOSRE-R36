// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Headless frontend: boots the BIOS/firmware (and optionally a ROM), runs N
// frames, optionally tracing instructions or dumping frames/audio.
#include "core/nds.h"
#include "core/cpu/timing_mode.h"
#include "core/pc_sampler.h"
#include "core/mem/fastmem_census.h"
#include "core/host_cores.h"
#include "core/state/state.h"
#include "core/io/dsi_nand_persist.h"
#include "core/io/dsi_title_install.h"
#include "core/io/dsi_nand_launch.h"
#if DSPERATE_CHEEVOS
#include "cheevos/cheevos_http.h"
#endif
#include "core/cpu/interp/interp.h"
#include "core/input/input_log.h"
#if DSPERATE_JIT
#include "core/cpu/jit/jit.h"
#endif
#include "core/profile.h"
#include "core/frame_report.h"
#include "core/cheat/database.h"

#include <cstdio>
#include <thread>
#include <random>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <chrono>
#include <thread>
#if DSPERATE_NET
#include "net/lan_mp.h"
#include "net/slirp_driver.h"
#include <arpa/inet.h>
#endif

namespace {


// DS_STATE_DEBUG=1: dump per-CPU cycle-accounting state around state save/load.
void dump_cpu_timing(ds::NDS& nds, const char* when) {
  if (!std::getenv("DS_STATE_DEBUG")) return;
  for (ds::Cpu w : {ds::Cpu::ARM9, ds::Cpu::ARM7}) {
    const ds::CpuContext& c = nds.cpu(w);
    const ds::u32 pc = c.hot.regs[15];
    std::fprintf(stderr, "[state %s] %s irq_pending %u ime %u ie %08x if %08x cpsr %08x\n", when, w == ds::Cpu::ARM9 ? "a9" : "a7", c.hot.irq_pending,
                 nds.io.cpu_io[w == ds::Cpu::ARM9 ? 0 : 1].ime, nds.io.cpu_io[w == ds::Cpu::ARM9 ? 0 : 1].ie, nds.io.cpu_io[w == ds::Cpu::ARM9 ? 0 : 1].if_, c.hot.cpsr);
    std::fprintf(stderr, "[state %s] %s pc %08x budget %d resid %d halted %d code_cycles %u data_cycles %u code_rgn %u data_rgn %u branch_fetch %d t9[",
                 when, w == ds::Cpu::ARM9 ? "a9" : "a7", pc, c.hot.cycle_budget, c.preempt_residual, c.halted, c.code_cycles, c.data_cycles, c.code_region, c.data_region, c.branch_fetch);
    for (int i = 0; i < 8; ++i) std::fprintf(stderr, "%s%u", i ? " " : "", c.timing9[pc >> 12][i]);
    std::fprintf(stderr, "] t7[");
    for (int i = 0; i < 4; ++i) std::fprintf(stderr, "%s%u", i ? " " : "", c.timing7[pc >> 15][i]);
    std::fprintf(stderr, "] itcm %u dtcm %08x/%08x\n", c.itcm_size, c.dtcm_base, c.dtcm_mask);
  }
  { ds::u64 h = 1469598103934665603ull; const ds::u32* l = nds.gpu3d.line(nds.gpu3d.frame_ref(), 0); for (int x = 0; x < 256; ++x) h = (h ^ l[x]) * 1099511628211ull;
    std::fprintf(stderr, "[state %s] 3d line0 hash %016llx\n", when, (unsigned long long)h); }
  std::fprintf(stderr, "[state %s] now %llu next %llu a7debt? gx idle %d\n", when, (unsigned long long)nds.sched.now(), (unsigned long long)nds.sched.next_deadline(), nds.gpu3d.idle());
}

std::vector<ds::u8> slurp_file(const char* path) {
  std::vector<ds::u8> v;
  FILE* f = std::fopen(path, "rb");
  if (!f) return v;
  std::fseek(f, 0, SEEK_END); const long n = std::ftell(f); std::fseek(f, 0, SEEK_SET);
  if (n > 0) { v.resize(static_cast<size_t>(n)); if (std::fread(v.data(), 1, v.size(), f) != v.size()) v.clear(); }
  std::fclose(f);
  return v;
}
// Spin-loop collapse: a line identical to one of the last 32 emitted lines is
// dropped, so a poll loop's repeated state doesn't bloat the trace.
struct TraceState {
  FILE* out[2] = {nullptr, nullptr};
  unsigned long long count[2] = {0, 0};
  unsigned long long executed[2] = {0, 0};
  unsigned long long max = 1'000'000;
  ds::u64 recent[2][32] = {};
  unsigned recent_pos[2] = {0, 0};
  bool pc_hist = false;                       // TRACE_PC_HIST=1: uncollapsed PC histogram on stderr at exit
  const ds::NDS* nds = nullptr;
  bool stamp = false;                         // TRACE_TIME=1: prefix each line with the scheduler time (not the shared format)
  std::unordered_map<ds::u32, unsigned long long> hist[2];
};

void trace_cb(ds::CpuContext& cpu, ds::u32 instr, void* user) {
  auto* t = static_cast<TraceState*>(user);
  const int i = static_cast<int>(cpu.which);
  // DS_WATCH7=<hex offset into ARM7 WRAM>: polled per traced instruction, so it
  // catches a byte change made any way (not just the slow path DS_WATCH traps).
  {
    static const char* w7 = std::getenv("DS_WATCH7");
    static const ds::u32 woff = w7 ? (ds::u32)std::strtoul(w7, nullptr, 16) : 0;
    static int wlast = -1;
    if (w7) {
      const int now = t->nds->bus.arm7_wram.get()[woff & 0xFFFF];
      if (now != wlast) {
        std::fprintf(stderr, "[watch7] %04x: %02x -> %02x  after %s pc %08x t=%llu wramcnt=%u\n", woff & 0xFFFF,
                     wlast < 0 ? 0 : wlast, now, i ? "arm7" : "arm9",
                     cpu.hot.regs[15] - (cpu.thumb() ? 4 : 8), (unsigned long long)t->nds->sched.now(), t->nds->io.wramcnt);
        wlast = now;
      }
    }
  }
  t->executed[i]++;
  if (t->pc_hist) t->hist[i][cpu.hot.regs[15] - (cpu.thumb() ? 4 : 8)]++;
  if (t->count[i] >= t->max) return;
  const bool thumb = cpu.thumb();
  const ds::u32 pc = cpu.hot.regs[15] - (thumb ? 4 : 8);
  ds::u64 h = 1469598103934665603ull;
  auto mix = [&](ds::u32 v) { h ^= v; h *= 1099511628211ull; h ^= h >> 29; };
  mix(pc); mix(instr); mix(cpu.hot.cpsr);
  for (int r = 0; r < 15; ++r) mix(cpu.hot.regs[r]);
  static const bool nodedup = std::getenv("DS_TRACE_NODEDUP") != nullptr;   // every instruction, repeats included (poll loops)
  if (!nodedup) {
    for (int k = 0; k < 32; ++k) if (t->recent[i][k] == h) return;
    t->recent[i][t->recent_pos[i]++ & 31] = h;
  }
  t->count[i]++;
  if (t->stamp) {
    static const bool fine = std::getenv("TRACE_TIME_FINE") != nullptr;   // DSi ARM9 in raw core cycles (quarter system cycles at 134 MHz, half at 67)
    if (fine && i == 0 && t->nds->dsi && t->nds->sched.running() == t->nds->arm9.get()) std::fprintf(t->out[i], "%llu ", (unsigned long long)t->nds->sched.now_fine9());
    else std::fprintf(t->out[i], "%llu ", (unsigned long long)t->nds->sched.now());
  }
  std::fprintf(t->out[i], "%08x %08x %08x", pc, instr, cpu.hot.cpsr);
  for (int r = 0; r < 15; ++r) std::fprintf(t->out[i], " %08x", cpu.hot.regs[r]);
  std::fputc('\n', t->out[i]);
}
// DS_ENTRY_SNAP=<dir>:<arm9 pc>:<arm7 pc> (hex): on hitting each PC (at most 4
// times each), dump machine state to <dir>/<cpu>_<n>/.
struct EntrySnap {
  std::string dir;
  ds::u32 pc[2] = {0, 0};
  int hits[2] = {0, 0};
  ds::NDS* nds = nullptr;
};

// DS_SWI_LOG=1: every SWI the CPUs take outside the BIOS, with r0-r3 (a
// diagnostic trace hook, like DS_ENTRY_SNAP).
void swi_log_cb(ds::CpuContext& cpu, ds::u32 instr, void* user) {
  auto* n = static_cast<ds::NDS*>(user);
  const bool thumb = cpu.thumb();
  const ds::u32 pc = cpu.hot.regs[15] - (thumb ? 4 : 8);
  if (pc < 0x01000000) return;   // the BIOS's own
  static ds::u32 ret_at[2] = {0, 0}, ret_dst[2] = {0, 0};
  const int ci = cpu.which == ds::Cpu::ARM9 ? 0 : 1;
  if (ret_at[ci] && pc == ret_at[ci]) {
    std::fprintf(stderr, "        returned: r0 %08x r1 %08x r2 %08x r3 %08x; dst:", cpu.hot.regs[0], cpu.hot.regs[1], cpu.hot.regs[2], cpu.hot.regs[3]);
    for (int k = 0; k < 0x30; ++k) std::fprintf(stderr, "%s%02x", k % 4 ? "" : " ", n->bus.dma_read8(cpu.which, ret_dst[ci] + k));
    std::fputc('\n', stderr);
    ret_at[ci] = 0;
  }
  ds::u32 num;
  if (thumb) { if ((instr & 0xFF00) != 0xDF00) return; num = instr & 0xFF; }
  else { if ((instr & 0x0F000000) != 0x0F000000) return; num = (instr >> 16) & 0xFF; }
  std::fprintf(stderr, "[swi] frame %llu arm%d pc %08x swi %02x r0 %08x r1 %08x r2 %08x r3 %08x\n", (unsigned long long)n->frame_count,
               cpu.which == ds::Cpu::ARM9 ? 9 : 7, pc, num, cpu.hot.regs[0], cpu.hot.regs[1], cpu.hot.regs[2], cpu.hot.regs[3]);
  if (num >= 0x20 && num <= 0x2F) { ret_at[ci] = pc + (thumb ? 2 : 4); ret_dst[ci] = cpu.hot.regs[1]; }
  if (num >= 0x20 && num <= 0x2F) {
    for (int r = 0; r < 3; ++r) {
      const ds::u32 a = cpu.hot.regs[r];
      if ((a >> 24) != 0x02) continue;
      std::fprintf(stderr, "        r%d@%08x:", r, a);
      for (int k = 0; k < 0x30; ++k) std::fprintf(stderr, "%s%02x", k % 4 ? "" : " ", n->bus.dma_read8(cpu.which, a + k));
      std::fputc('\n', stderr);
    }
  }
}

void write_blob(const std::string& path, const ds::u8* p, size_t n) {
  if (FILE* f = std::fopen(path.c_str(), "wb")) { std::fwrite(p, 1, n, f); std::fclose(f); }
}

void entry_snap_cb(ds::CpuContext& cpu, ds::u32, void* user) {
  auto* s = static_cast<EntrySnap*>(user);
  const int i = static_cast<int>(cpu.which);
  if (cpu.hot.regs[15] - (cpu.thumb() ? 4 : 8) != s->pc[i] || s->hits[i] >= 4) return;
  ds::NDS& n = *s->nds;
  const std::string d = s->dir + "/" + (i ? "arm7_" : "arm9_") + std::to_string(s->hits[i]++);
  std::string mk = "mkdir -p '" + d + "'";
  if (std::system(mk.c_str()) != 0) return;
  write_blob(d + "/main.bin", n.bus.main_ram.get(), n.bus.main_ram_size());
  write_blob(d + "/swram.bin", n.bus.shared_wram.get(), ds::mem::Bus::SHARED_WRAM_SIZE);
  write_blob(d + "/wram7.bin", n.bus.arm7_wram.get(), ds::mem::Bus::ARM7_WRAM_SIZE);
  write_blob(d + "/itcm.bin", n.bus.itcm.get(), ds::mem::Bus::ITCM_SIZE);
  write_blob(d + "/dtcm.bin", n.bus.dtcm.get(), ds::mem::Bus::DTCM_SIZE);
  for (int b = 0; b < 3; ++b) write_blob(d + "/nwram" + char('a' + b) + ".bin", n.bus.nwram[b].get(), ds::mem::Bus::NWRAM_BANK_SIZE);
  // Bus chunk left out: its memory buffers are already in the files above.
  { ds::state::Writer w; n.sched.sync_state(w); n.io.sync_state(w); n.arm9->sync_state(w); n.arm7->sync_state(w); n.dma.sync_state(w);
    write_blob(d + "/devices.bin", w.data().data(), w.data().size()); }
  { ds::state::Writer w; n.io.aes.sync_state(w); write_blob(d + "/aes.bin", w.data().data(), w.data().size()); }
  FILE* f = std::fopen((d + "/summary.txt").c_str(), "w");
  if (!f) return;
  std::fprintf(f, "frame %llu now %llu\n", (unsigned long long)n.frame_count, (unsigned long long)n.sched.now());
  for (ds::Cpu w : {ds::Cpu::ARM9, ds::Cpu::ARM7}) {
    const ds::CpuContext& c = n.cpu(w);
    const int k = w == ds::Cpu::ARM9 ? 0 : 1;
    std::fprintf(f, "%s cpsr %08x spsr %08x halted %d r:", k ? "arm7" : "arm9", c.hot.cpsr, c.hot.spsr, c.halted);
    for (int r = 0; r < 16; ++r) std::fprintf(f, " %08x", c.hot.regs[r]);
    std::fprintf(f, "\n%s bank_spsr:", k ? "arm7" : "arm9");
    for (int b = 0; b < 6; ++b) std::fprintf(f, " %08x", c.bank_spsr[b]);
    std::fprintf(f, "\n%s bank_r13:", k ? "arm7" : "arm9");
    for (int b = 0; b < 6; ++b) std::fprintf(f, " %08x", c.bank_r13[b]);
    std::fprintf(f, "\n%s bank_r14:", k ? "arm7" : "arm9");
    for (int b = 0; b < 6; ++b) std::fprintf(f, " %08x", c.bank_r14[b]);
    if (!k) {
      std::fprintf(f, "\narm9 cp15 control %08x dtcm %08x itcm %08x code_cache %08x data_cache %08x data_buf %08x code_perm %08x data_perm %08x pu:",
                   c.cp15_control, c.cp15_dtcm, c.cp15_itcm, c.pu_code_cacheable, c.pu_data_cacheable, c.pu_data_bufferable, c.pu_code_perm, c.pu_data_perm);
      for (ds::u32 v : c.pu_region) std::fprintf(f, " %08x", v);
    }
    const ds::io::CpuIo& io = n.io.cpu_io[k];
    std::fprintf(f, "\n%s ime %u ie %08x if %08x ipcsync %04x ipcfifocnt %04x postflg %u\n", k ? "arm7" : "arm9", io.ime, io.ie, io.if_, io.ipc_sync, io.ipc_fifo_cnt, io.postflg);
  }
  std::fprintf(f, "arm9 itcm %u dtcm %08x/%08x\n", n.arm9->itcm_size, n.arm9->dtcm_base, n.arm9->dtcm_mask);
  const auto& x = n.io.dsi;
  std::fprintf(f, "wramcnt %02x powcnt1 %04x powcnt2 %04x exmemcnt %04x rcnt %04x spicnt %04x keycnt %04x/%04x\n",
               n.io.wramcnt, n.io.powcnt1, n.io.powcnt2, n.io.exmemcnt, n.io.rcnt, n.io.spicnt, n.io.keycnt[0], n.io.keycnt[1]);
  std::fprintf(f, "vramcnt"); for (ds::u8 v : n.io.vramcnt) std::fprintf(f, " %02x", v);
  std::fprintf(f, "\nscfg_bios %04x clock9 %04x clock7 %04x rst %04x ext9 %08x ext7 %08x mc %04x ie2 %08x if2 %08x sndexcnt %04x console_id %016llx\n",
               x.scfg_bios, x.scfg_clock9, x.scfg_clock7, x.scfg_rst, x.scfg_ext[0], x.scfg_ext[1], x.scfg_mc, x.ie2, x.if2, x.sndexcnt, (unsigned long long)x.console_id);
  for (int c = 0; c < 2; ++c) { std::fprintf(f, "mbk[%s]", c ? "arm7" : "arm9"); for (ds::u32 v : x.mbk[c]) std::fprintf(f, " %08x", v); std::fputc('\n', f); }
  std::fprintf(f, "gpio data %02x dir %02x iedgesel %02x ie %02x wifi %04x\n", x.gpio_data, x.gpio_dir, x.gpio_iedgesel, x.gpio_ie, x.gpio_wifi);
  std::fprintf(f, "bptwl"); for (int r = 0; r < 0x100; ++r) std::fprintf(f, "%s%02x", r % 16 ? " " : "\n  ", x.bptwl_regs[r]);
  std::fprintf(f, "\npm"); for (ds::u8 v : n.io.spi_pm.regs) std::fprintf(f, " %02x", v);
  std::fputc('\n', f);
  std::fclose(f);
  std::fprintf(stderr, "entry snap: %s at frame %llu\n", d.c_str(), (unsigned long long)n.frame_count);
}

} // namespace

int main(int argc, char** argv) {
  ds::mem::fmc::init();   // before any Bus exists: counts page-table churn from reset on
  int scaled_n = 0; const char* scaled_path = nullptr;
  const char *rom = nullptr, *bios9 = nullptr, *bios7 = nullptr, *fw = nullptr, *trace = nullptr, *dump = nullptr, *hash_frames = nullptr, *dump_audio = nullptr, *replay = nullptr, *save = nullptr;
  const char* load_state = nullptr; const char* save_state_path = nullptr; int save_state_at = -1;
  const char* hide_screen = nullptr;
  int frameskip = 0;    // --frameskip N: skip drawing N of every N+1 frames (fixed; the SDL frontend also has the adaptive mode)
  bool frameskip_capture = false;
  int stats_from = 0;   // --stats-from N: first frame counted in the timing statistics
  const char* pc_profile = nullptr;
  int dump_from = 0, dump_count = 0;   // --dump-from/--dump-count: window of a large dump
  long gpu_dump_frame = -1; const char* gpu_dump_path = nullptr;   // --dump-gpu-frame
  const char* rec3d_path = nullptr;   // --dump-3d
  bool gpu3d = false;   // --gpu3d
  int frames = 60; bool direct = false;
#if DSPERATE_JIT
  bool jit = true;                    // both CPUs; --interp clears it
#else
  bool jit = false;
#endif
  TraceState ts;
  bool rtc_host = false;              // --rtc-host: free-running clock seeded from the wall
  const char* fw_override = nullptr;  // --firmware-override: sidecar of changed firmware pages
  bool no_aa = false;
  bool frames_given = false;
  const char* cheat_db = nullptr;      // a usrcheat.dat to load this ROM's codes from
  const char* bios9i = nullptr; const char* bios7i = nullptr; const char* dsi_boot = nullptr; const char* dsi_nand = nullptr; bool dsi_nand_boot = false; bool dsi_nand_write = false; const char* dsi_persist = nullptr; const char* dsi_install = nullptr; bool dsi_hide_installed = false; const char* dsi_tmd = nullptr; bool dsi_offline = false; bool dsi_autoload = false; bool dsi_hle = false; ds::u32 dsi_title_lo = 0; ds::bios::UserSettings user; const char* dsi_font = nullptr; const char* dsi_sd = nullptr; ds::u64 dsi_autoload_id = 0; const char* dsi_shortcuts = nullptr; bool dsi_shortcuts_on = true;
  int dsi_mode = -1;                   // -1 auto
  bool list_cheats = false;
  std::vector<std::string> enable_cheats;   // names (or #index) to switch on
  const char* lan_host = nullptr;      // --lan-host NAME: host as NAME
  const char* lan_join = nullptr;      // --lan-join ADDR: join the session at ADDR
  const char* lan_name = "DSperate";   // --lan-name NAME: our player name when joining
  bool netplay = false;                // --netplay: join a session heard on the LAN within 2.5 s, else host one
  // --internet [WHERE]: DNS host, wiimmfi (default) or address; excludes local wireless.
  bool internet = false;
  const char* dns_arg = nullptr;
  // --tap-after-sync F:x,y[:N[:R]]: as MP host, F frames after client sync,
  // touch (x,y) for N frames, R times 60 frames apart (Download Play "Cut Off").
  int tas_after = -1, tas_x = 0, tas_y = 0, tas_n = 8, tas_rep = 1; long tas_frame = -1; ds::u32 tas_seen = 0;
  int lan_players = 16;
  struct Touch { int frame, x, y, n; };
  double mic_tone_hz = 0;
  std::vector<Touch> touches;          // --touch F:x,y[:N]: press (x,y) from frame F for N frames (default 10)
  bool pace = false;                   // --pace: sleep to 60 frames a second
  for (int i = 1; i < argc; ++i) {
    auto arg = [&](const char* name) { return !std::strcmp(argv[i], name) && i + 1 < argc; };
    auto flag = [&](const char* name) { return !std::strcmp(argv[i], name); };
    if (arg("--frames")) { frames = std::atoi(argv[++i]); frames_given = true; }
    else if (arg("--lan-host")) { lan_host = argv[++i]; pace = true; }
    else if (arg("--lan-join")) { lan_join = argv[++i]; pace = true; }
    else if (arg("--lan-name")) lan_name = argv[++i];
    else if (flag("--netplay")) { netplay = true; pace = true; }
    else if (flag("--internet")) { internet = true; pace = true; }   // real sockets want real time
    else if (arg("--dns")) dns_arg = argv[++i];
    else if (arg("--tap-after-sync")) { std::sscanf(argv[++i], "%d:%d,%d:%d:%d", &tas_after, &tas_x, &tas_y, &tas_n, &tas_rep); }
    else if (arg("--lan-players")) lan_players = std::atoi(argv[++i]);
    else if (arg("--mic-tone")) { mic_tone_hz = std::atof(argv[++i]); }   // a sine at HZ (amplitude DS_MIC_TONE_AMP, default 8000) as the microphone input, every frame
    else if (arg("--touch")) { Touch t{0, 0, 0, 10}; std::sscanf(argv[++i], "%d:%d,%d:%d", &t.frame, &t.x, &t.y, &t.n); touches.push_back(t); }
    else if (flag("--pace")) pace = true;
    else if (arg("--bios9")) bios9 = argv[++i];
    else if (arg("--bios7")) bios7 = argv[++i];
    else if (arg("--firmware")) fw = argv[++i];
    else if (arg("--bios9i")) bios9i = argv[++i];            // the DSi BIOS pair (64 KB each): needed for DSi mode
    else if (arg("--bios7i")) bios7i = argv[++i];
    else if (arg("--dsi-boot")) dsi_boot = argv[++i];        // console data a DSi title starts with
    else if (flag("--dsi-nand-boot")) dsi_nand_boot = true;   // boot the NAND (boot2 -> launcher) instead of direct-booting the ROM
    else if (arg("--dsi-tmd")) dsi_tmd = argv[++i];           // the title's signed DSi TMD for --dsi-install (default: <file>.tmd beside it)
    else if (flag("--dsi-autoload")) dsi_autoload = true;
    else if (arg("--dsi-autoload-id")) { dsi_autoload_id = std::strtoull(argv[++i], nullptr, 16); dsi_autoload = true; }   // 16 hex digits, e.g. 00030005484E4B45: TLNC-launch any installed title (system apps too)
    else if (flag("--dsi-hle-launch")) dsi_hle = true;         // with --direct: start the DSiWare ROM as the DSi launcher hands a title over, not in card mode
    else if (arg("--dsi-font")) dsi_font = argv[++i];         // with --dsi-hle-launch and no --dsi-nand: the console's /sys/TWLFontTable.dat instead of DSperate's own font
    else if (arg("--user-name")) user.nickname = argv[++i];   // generated firmware / DSi settings: the owner's nickname
    else if (arg("--user-language")) user.language = static_cast<ds::u8>(std::atoi(argv[++i]));   // 0 ja 1 en 2 fr 3 de 4 it 5 es 6 zh 7 ko
    else if (flag("--dsi-offline")) dsi_offline = true;        // --dsi-install never downloads the TMD from Nintendo's update CDN
    else if (flag("--dsi-hide-installed")) dsi_hide_installed = true;   // hide the dump's own DSiWare for this session (the dump is untouched)
    else if (arg("--dsi-install")) dsi_install = argv[++i];   // a DSiWare .nds/.cia put into the session's NAND (not the dump) unless its title ID is already installed
    else if (arg("--dsi-persist")) dsi_persist = argv[++i];   // carry DSi saves (<CODE>.pub/.prv/.bnr), the system sidecar (nand.ovr) and photos (photos/) in and out of DIR
    else if (flag("--dsi-nand-write")) dsi_nand_write = true;   // write the guest's NAND writes into the file (for diffing against melonDS; use a copy). Default: held in memory
    else if (arg("--dsi-nand")) dsi_nand = argv[++i];        // a real nand.bin (nocash footer): the eMMC behind the SD/MMC host, and the console ID
    else if (arg("--dsi-sd")) dsi_sd = argv[++i];   // host folder as the DSi's SD card; synced back at exit
    else if (arg("--dsi-shortcuts")) dsi_shortcuts = argv[++i];                                         // with --dsi-nand: a .dspr.nds shortcut in DIR for each installed DSiWare title, then exit
    else if (arg("--dsi-shortcuts-clear")) { dsi_shortcuts = argv[++i]; dsi_shortcuts_on = false; }   // remove every .dspr.nds in DIR, then exit
    else if (flag("--dsi")) dsi_mode = 1;                    // force the DSi machine (default: a DSi-capable header with the DSi BIOS loaded)
    else if (flag("--no-dsi")) dsi_mode = 0;
    else if (arg("--trace")) trace = argv[++i];
    else if (arg("--max")) ts.max = std::strtoull(argv[++i], nullptr, 0);
    else if (arg("--dump-frames")) dump = argv[++i];
    else if (arg("--dump-3d")) rec3d_path = argv[++i];   // FILE: the 3D layer record as the compositor reads it (256x192 words: RGB666 + 5-bit alpha in 24-28), for the --dump-from/--dump-count frames
    else if (arg("--dump-gpu-frame")) { gpu_dump_frame = std::atol(argv[++i]); gpu_dump_path = argv[++i]; }   // N FILE: frame N's polygon list in the GPU raster's layout (vk_dump.h)

    else if (arg("--hash-frames")) hash_frames = argv[++i];   // FILE: "frame top bottom" per frame, FNV-1a 64 of each screen's 0xAARRGGBB (the video golden hashes)
    else if (arg("--dump-scaled")) { scaled_n = std::atoi(argv[++i]); scaled_path = argv[++i]; }   // N FILE: both screens through the scanline scaler at Nx, raw BGRA
    else if (arg("--dump-from")) dump_from = std::atoi(argv[++i]);    // first frame to dump
    else if (arg("--dump-count")) dump_count = std::atoi(argv[++i]);  // how many (0 = to the end)
    else if (arg("--dump-audio")) dump_audio = argv[++i];   // raw s16 stereo, 32768 Hz
    else if (arg("--replay")) replay = argv[++i];           // inputs recorded by dsperate-sdl --record; sets --frames to its length unless given
    else if (arg("--save")) save = argv[++i];               // battery save to start from; loaded read-only, never written back
    else if (arg("--cheats")) cheat_db = argv[++i];         // usrcheat.dat; the entry matching this ROM is loaded
    else if (flag("--list-cheats")) list_cheats = true;     // print them (with their index) and exit
    else if (arg("--cheat")) enable_cheats.push_back(argv[++i]);   // enable one by name, or by "#N" from --list-cheats
    else if (!std::strcmp(argv[i], "--direct")) direct = true;
    else if (!std::strcmp(argv[i], "--interp")) jit = false;
    else if (arg("--timing")) { const char* m = argv[++i]; if (!std::getenv("DS_TIMING")) ds::g_fast_timing = std::strcmp(m, "exact") != 0; }   // CPU cycle model (timing_mode.h): fast (default) | exact; DS_TIMING wins

    else if (flag("--rtc-host")) rtc_host = true;                            // INEXACT by construction: runs stop being reproducible
    else if (arg("--firmware-override")) fw_override = argv[++i];            // load it, and write back what the firmware changed
    else if (flag("--no-aa")) no_aa = true;                                  // 3D anti-aliasing off (Renderer3D::set_aa); inexact, for measurement
    else if (flag("--gpu3d")) gpu3d = true;   // the 3D layer on the GPU (Renderer3D::set_gpu, vk_lean.h); declines to the CPU without Vulkan
    else if (arg("--load-state")) load_state = argv[++i];                   // restore a save state before running
    else if (arg("--frameskip")) frameskip = std::atoi(argv[++i]);          // skip drawing N of every N+1 frames (Gpu::set_frame_skip); a dump of a skipped frame is stale
    else if (flag("--frameskip-capture")) frameskip_capture = true;          // INEXACT: skip frames that display-capture too
    else if (arg("--hide-screen")) hide_screen = argv[++i];                 // top | bottom: the engine on it skips its drawing (Gpu::set_screen_visible); its half of the dump goes stale
    // Frames before N run but are excluded from frame_ms/work_ms stats (cold
    // caches after --load-state); DS_PROFILE counters accumulate from frame 0.
    else if (arg("--stats-from")) stats_from = std::atoi(argv[++i]);
    else if (arg("--pc-profile")) pc_profile = argv[++i];                   // a sampling profile of every thread from --stats-from on
    else if (arg("--save-state-at")) { save_state_at = std::atoi(argv[++i]); save_state_path = std::strchr(argv[i], ':'); if (save_state_path) ++save_state_path; }   // N:path -- write after N frames (0 = at once)
    else rom = argv[i];
  }
  // Without --direct, replay inputs land in the firmware setup wizard instead.
  if (replay && !direct)
    std::fprintf(stderr, "warning: --replay without --direct boots the firmware, not the ROM;"
                         " the replay will not reproduce the recorded session\n");
  std::fprintf(stderr, "DSperate 1.0.0 (%s%s)\n",
#if DSPERATE_JIT
              "jit",
#else
              "interp",
#endif
#if DSPERATE_NEON
              "+neon");
#else
              "");
#endif
  ds::NDS nds;
  if (frameskip_capture) nds.gpu.set_frameskip_capture(true);
  if (hide_screen) nds.gpu.set_screen_visible(!std::strcmp(hide_screen, "bottom") ? 1 : 0, false);
  {
    std::string err;
    if (!nds.load_bios(bios9 ? bios9 : "", bios7 ? bios7 : "", fw ? fw : "", user, &err)) { std::fprintf(stderr, "bios: %s\n", err.c_str()); return 1; }
    if (lan_host || lan_join || netplay) { std::random_device rd; nds.set_wifi_mac_suffix(rd() & 0xFFFFFF); }
  }
  if (!nds.bios_native) std::fprintf(stderr, "note: --bios9/--bios7 %s; using the built-in FreeBIOS (direct boot only, timing is not Nintendo's)\n", bios9 ? "not found" : "not given");
  {
    std::string err;
    if (!nds.load_dsi_bios(bios9i ? bios9i : "", bios7i ? bios7i : "", &err)) { std::fprintf(stderr, "dsi bios: %s\n", err.c_str()); return 1; }
    if (dsi_boot && !nds.load_dsi_boot_blobs(dsi_boot, &err)) { std::fprintf(stderr, "dsi boot: %s\n", err.c_str()); return 1; }
    if (dsi_nand && !nds.load_dsi_nand(dsi_nand, &err, dsi_nand_write)) { std::fprintf(stderr, "dsi nand: %s\n", err.c_str()); return 1; }
    if (dsi_shortcuts) {
      if (dsi_shortcuts_on && (!nds.dsi_nand.valid() || !nds.bios_native_dsi)) { std::fprintf(stderr, "--dsi-shortcuts needs --dsi-nand and --bios9i/--bios7i\n"); return 1; }
      const ds::io::ShortcutSync r = ds::io::sync_shortcuts(dsi_shortcuts, &nds.dsi_nand, nds.bus.bios7i.get(), dsi_shortcuts_on);
      std::fprintf(stderr, "dsi shortcuts: %d written, %d removed in %s\n", r.written, r.removed, dsi_shortcuts);
      for (const std::string& n : r.notes) std::fprintf(stderr, "dsi shortcuts: %s\n", n.c_str());
      return 0;
    }
    if (dsi_sd) {
      ds::io::SdCard::Report r;
      if (!nds.dsi_sd.open(dsi_sd, &r, &err)) { std::fprintf(stderr, "dsi sd: %s\n", err.c_str()); return 1; }
      std::fprintf(stderr, "dsi sd: %s, %d files and %d folders on a %llu MB FAT%d card\n", dsi_sd, r.files, r.dirs,
                   static_cast<unsigned long long>(nds.dsi_sd.length() >> 20), nds.dsi_sd.fat_bits());
      for (const std::string& n : r.notes) std::fprintf(stderr, "dsi sd: %s\n", n.c_str());
      // DS_SD_DUMP=<file>: the card as built, as an image.
      if (const char* d = std::getenv("DS_SD_DUMP"); d && !nds.dsi_sd.dump(d)) { std::fprintf(stderr, "dsi sd: cannot write %s\n", d); return 1; }
    }
    if (dsi_hide_installed && nds.dsi_nand.valid()) {
      const int n = ds::io::nand_hide_installed_dsiware(nds.dsi_nand, nds.bios_native_dsi ? nds.bus.bios7i.get() : nullptr, &err);
      if (n < 0) { std::fprintf(stderr, "dsi: hiding installed titles: %s\n", err.c_str()); return 1; }
      std::fprintf(stderr, "dsi: %d installed titles hidden for this session\n", n);
    }
    if (dsi_install && nds.dsi_nand.valid()) {
      std::vector<ds::u8> srl, embedded;
      if (!ds::io::read_dsiware(dsi_install, srl, &err, &embedded)) { std::fprintf(stderr, "dsi install: %s\n", err.c_str()); return 1; }
      const ds::u32 lo = static_cast<ds::u32>(srl[0x230] | (srl[0x231] << 8) | (srl[0x232] << 16) | (srl[0x233] << 24));
      if (ds::io::nand_has_title(nds.dsi_nand, nds.bios_native_dsi ? nds.bus.bios7i.get() : nullptr, lo)) {
        std::fprintf(stderr, "dsi install: %.4s: already installed on the NAND\n", reinterpret_cast<const char*>(&srl[0x0C]));
        dsi_title_lo = lo;
      } else {
        // The signed TMD, tried in order: the CIA's own, the --dsi-persist cache, the update CDN, or a file beside the ROM.
        std::string beside = dsi_tmd ? dsi_tmd : std::string(dsi_install);
        if (!dsi_tmd) { const size_t dot = beside.find_last_of('.'), slash = beside.find_last_of('/'); beside = (dot != std::string::npos && (slash == std::string::npos || dot > slash) ? beside.substr(0, dot) : beside) + ".tmd"; }
        const std::string cache = dsi_persist ? std::string(dsi_persist) + "/" + std::string(reinterpret_cast<const char*>(&srl[0x0C]), 4) + ".tmd" : std::string();
        ds::io::TmdFetch fetch;
#if DSPERATE_CHEEVOS
        std::string http_err;
        std::shared_ptr<ds::cheevos::Backend> http = dsi_offline ? nullptr : std::shared_ptr<ds::cheevos::Backend>(ds::cheevos::make_curl_backend(http_err));
        if (http) fetch = [http](const std::string& url, std::vector<ds::u8>& body) {
          const ds::cheevos::Response resp = http->perform({url, {}, {}}, "DSperate");
          if (resp.status != 200) return false;
          body.assign(resp.body.begin(), resp.body.end());
          return true;
        };
#endif
        std::vector<std::string> tmd_log;
        const std::vector<ds::u8> tmd = ds::io::find_signed_tmd(srl, embedded, cache, beside, fetch, tmd_log);
        for (const std::string& l : tmd_log) std::fprintf(stderr, "dsi install: %s\n", l.c_str());
        if (tmd.empty()) { std::fprintf(stderr, "dsi install: no signed DSi TMD for this title; the launcher will not start it without one (pass --dsi-tmd, or allow the download)\n"); return 1; }
        const auto t0 = std::chrono::steady_clock::now();
        const ds::io::TitleInstall r = ds::io::nand_install_title(nds.dsi_nand, nds.bios_native_dsi ? nds.bus.bios7i.get() : nullptr, srl, tmd);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::fprintf(stderr, "dsi install: %.4s: %s (%.0f ms)\n", reinterpret_cast<const char*>(&srl[0x0C]), r.message.c_str(), ms);
        if (r.result == ds::io::TitleInstall::Result::Failed) return 1;
        dsi_title_lo = r.title_lo;
      }
    }
    // A save state carries the NAND's sectors from here on (NandImage::mark_state_base).
    if (nds.dsi_nand.valid() && !nds.dsi_nand.write_through()) nds.dsi_nand.mark_state_base();
    if (dsi_persist && nds.dsi_nand.valid()) {
      if (dsi_nand_write) { std::fprintf(stderr, "--dsi-persist and --dsi-nand-write do not mix: the written image is the record there\n"); return 1; }
      const std::string d = dsi_persist;
      const ds::io::NandPersistReport r = ds::io::nand_import(nds.dsi_nand, nds.bios_native_dsi ? nds.bus.bios7i.get() : nullptr, {d, d + "/nand.ovr", d + "/photos"});
      std::fprintf(stderr, "dsi persist: in %d saves, %d system files, %d photos\n", r.saves, r.system_files, r.photos);
      for (const std::string& n : r.notes) std::fprintf(stderr, "dsi persist: %s\n", n.c_str());
    }
  }
  if (nds.firmware_synthetic) std::fprintf(stderr, "note: --firmware %s; using a generated firmware\n", fw ? "not found" : "not given");
  if (!direct && !nds.can_boot_firmware()) {
    // FreeBIOS's reset vector is an idle loop, so a firmware boot draws nothing.
    if (rom) { std::fprintf(stderr, "booting the firmware needs real BIOS and firmware dumps; pass --direct to run the ROM\n"); return 1; }
    std::fprintf(stderr, "warning: no ROM and no real dumps: nothing boots (FreeBIOS has no boot code)\n");
  }
  if (fw_override && !nds.firmware_synthetic) {
    std::string err;
    if (!nds.load_firmware_override(fw_override, err)) std::fprintf(stderr, "firmware override: %s\n", err.c_str());
    else if (!err.empty()) std::fprintf(stderr, "firmware override: warning: %s\n", err.c_str());
  }
  nds.reset();
  if (rtc_host) nds.io.start_rtc_clock();   // after reset(), which clears the RTC
  // A zipped ROM may be inflated to disk on first use (slow on an SD card).
  nds.rom_progress = [](void*, ds::u64 done, ds::u64 total) {
    static ds::u64 last = ~0ull;
    const ds::u64 pct = total ? done * 100 / total : 100;
    if (pct / 10 != last / 10 || done == total) { std::fprintf(stderr, "\rzip: extracting %llu%%", static_cast<unsigned long long>(pct)); last = pct; }
    if (done == total) std::fputc('\n', stderr);
  };
  if (rom && ds::io::is_shortcut_name(rom)) {
    // NAND title shortcut: title comes from --dsi-nand, handed over as --dsi-hle-launch does for a ROM file.
    ds::io::NandShortcut sc;
    std::string err;
    if (!ds::io::read_shortcut(rom, sc)) { std::fprintf(stderr, "%s: not a NAND title shortcut\n", rom); return 1; }
    if (!sc.from(nds.dsi_nand)) { std::fprintf(stderr, "%s: made from another NAND than --dsi-nand\n", rom); return 1; }
    if (!nds.load_dsi_nand_title(sc.title_lo, &err)) { std::fprintf(stderr, "%s: %s\n", rom, err.c_str()); return 1; }
    dsi_hle = true;
    dsi_mode = 1;
  } else if (rom && !nds.load_rom(rom)) { std::fprintf(stderr, "could not read %s\n", rom); return 1; }
  {
    const bool capable = nds.cart && nds.cart->dsi_capable();
    const bool want = dsi_mode == 1 || (dsi_mode == -1 && capable && nds.bios_native_dsi);
    if (want && !nds.bios_native_dsi) { std::fprintf(stderr, "DSi mode needs --bios9i/--bios7i\n"); return 1; }
    if (want && !capable) std::fprintf(stderr, "warning: --dsi with a DS-only header (unit code %02x)\n", nds.cart ? nds.cart->header().unit_code : 0);
    if (want) {
      nds.set_dsi(true);
      nds.dsi_nand_boot = dsi_nand_boot;
      if (dsi_hle && rom && !nds.dsi_nand_boot) {
        std::string why; std::vector<std::string> made;
        if (dsi_font) nds.dsi_font_path = dsi_font;
        if (!nds.prepare_dsi_hle(user, &why, &made)) { std::fprintf(stderr, "dsi: %s\n", why.c_str()); return 1; }
        for (const std::string& m : made) std::fprintf(stderr, "dsi: %s\n", m.c_str());
        if (dsi_persist && nds.dsi_nand_synthetic) {
          const std::string d = dsi_persist;
          const ds::io::NandPersistReport r = ds::io::nand_import(nds.dsi_nand, nds.bus.bios7i.get(), {d, "", d + "/photos"});
          nds.dsi_nand.mark_baseline();
          std::fprintf(stderr, "dsi persist: in %d saves, %d photos\n", r.saves, r.photos);
          for (const std::string& n : r.notes) std::fprintf(stderr, "dsi persist: %s\n", n.c_str());
        }
      }
      nds.reset();
      if (rtc_host) nds.io.start_rtc_clock();
      std::fprintf(stderr, "console: DSi (16 MB, ARM9 at 134 MHz)%s%s\n",
                   (dsi_boot || !nds.dsi_boot_blobs.empty()) ? "" : "; no --dsi-boot: the console data areas stay zero",
                   nds.dsi_nand_synthetic ? "; synthesised NAND" : nds.dsi_nand.valid() ? "; NAND attached" : "; no NAND (card mode)");
    } else if (capable && !nds.bios_native_dsi && dsi_mode == -1) {
      std::fprintf(stderr, "note: DSi-capable ROM without --bios9i/--bios7i: running as a DS\n");
    }
  }
  nds.gpu3d.renderer().set_aa(!no_aa);
  std::vector<ds::u32> scaled_px[2]; std::vector<ds::u16> scaled_xrun; FILE* scaled_out = nullptr;
  if (scaled_n > 0 && scaled_path) {
    const ds::u32 W = ds::SCREEN_W * scaled_n, H = ds::SCREEN_H * scaled_n;
    scaled_xrun.resize(ds::SCREEN_W + 1);
    for (ds::u32 x = 0; x <= ds::SCREEN_W; ++x) scaled_xrun[x] = static_cast<ds::u16>(x * scaled_n);
    for (int k = 0; k < 2; ++k) {
      scaled_px[k].assign(static_cast<size_t>(W) * H, 0xFF000000u);
      ds::gpu::Gpu::ScaleTarget t; t.px = scaled_px[k].data(); t.pitch = W; t.h = H; t.xrun = scaled_xrun.data();
      nds.gpu.set_scale_target(k, t);
    }
    scaled_out = std::fopen(scaled_path, "wb");
    if (!scaled_out) { std::fprintf(stderr, "could not open %s\n", scaled_path); return 1; }
  }

  if (rom && cheat_db) {
    ds::cheat::GameCheats found;
    std::string err;
    // From the loaded cart, so a zipped ROM is looked up by the game's header.
    ds::u8 header[512] = {};
    if (nds.cart) nds.cart->rom_read(0, header, sizeof header);
    if (!ds::cheat::load_for_header(cheat_db, header, found, err)) {
      if (!err.empty()) { std::fprintf(stderr, "cheats: %s\n", err.c_str()); return 1; }
      std::fprintf(stderr, "cheats: this ROM is not in %s\n", cheat_db);
      if (list_cheats) return 0;
    } else {
      if (!err.empty()) std::fprintf(stderr, "cheats: %s\n", err.c_str());
      std::fprintf(stderr, "cheats: %s -- %zu codes in %zu groups\n",
                   found.name.c_str(), found.codes.size(), found.groups.size());
      if (list_cheats) {
        for (size_t i = 0; i < found.codes.size(); ++i) {
          const ds::cheat::Code& c = found.codes[i];
          const char* g = c.group >= 0 ? found.groups[static_cast<size_t>(c.group)].name.c_str() : "";
          std::printf("#%-5zu %-6s %-52s %s\n", i, c.is_note() ? "note" : "code", c.name.c_str(), g);
        }
        return 0;
      }
      nds.cheats.codes = std::move(found.codes);
      // "#N" is the listing's index -- needed since names can repeat within a game.
      for (const std::string& want : enable_cheats) {
        bool hit = false;
        if (want.size() > 1 && want[0] == '#') {
          const size_t at = std::strtoul(want.c_str() + 1, nullptr, 10);
          if (at < nds.cheats.codes.size()) { nds.cheats.codes[at].enabled = true; hit = true; }
        } else {
          for (ds::cheat::Code& c : nds.cheats.codes) if (c.name == want) { c.enabled = true; hit = true; }
        }
        if (!hit) std::fprintf(stderr, "cheats: no code called \"%s\"\n", want.c_str());
      }
      size_t on = 0;
      for (const ds::cheat::Code& c : nds.cheats.codes) if (c.enabled) ++on;
      std::fprintf(stderr, "cheats: %zu enabled\n", on);
    }
  }
  if (dsi_hle) {
    if (!(nds.dsi && rom && direct && !nds.dsi_nand_boot)) { std::fprintf(stderr, "--dsi-hle-launch needs --dsi, --direct and a DSiWare ROM (not a NAND boot)\n"); return 1; }
    nds.dsi_hle_launch = true;
    const ds::u32 lo = nds.cart->twl().title_id_lo;
    if (nds.dsi_nand.valid() && !ds::io::nand_title_content_id(nds.dsi_nand, nds.bus.bios7i.get(), lo, nds.dsi_hle_content_id))
      std::fprintf(stderr, "dsi: %08x is not installed on the NAND; its image path names content 00000000\n", lo);
    std::fprintf(stderr, "dsi: launcher hand-off for %08x, content %08x\n", lo, nds.dsi_hle_content_id);
  }
  if ((rom && direct) || (nds.dsi && nds.dsi_nand_boot)) nds.setup_direct_boot();
  if (dsi_autoload) {
    const ds::u32 lo = dsi_autoload_id ? static_cast<ds::u32>(dsi_autoload_id) : dsi_title_lo;
    const ds::u32 hi = dsi_autoload_id ? static_cast<ds::u32>(dsi_autoload_id >> 32) : 0x00030004;
    if (!(nds.dsi && nds.dsi_nand_boot && lo)) { std::fprintf(stderr, "--dsi-autoload needs a NAND boot and --dsi-install or --dsi-autoload-id\n"); return 1; }
    nds.dsi_autoload(lo, hi);
    std::fprintf(stderr, "dsi: autoload %08x%08x (TLNC)\n", hi, lo);
  }
  // Never written back -- this is a harness.
  if (save && nds.cart) {
    if (FILE* f = std::fopen(save, "rb")) {
      std::vector<ds::u8> data;
      ds::u8 buf[65536];
      for (size_t got; (got = std::fread(buf, 1, sizeof buf, f)) > 0;) data.insert(data.end(), buf, buf + got);
      std::fclose(f);
      const ds::cart::Cart::SaveLoad r = nds.cart->load_save(data.data(), data.size());
      std::fprintf(stderr, "save: loaded %zu bytes from %s%s\n", data.size(), save, r.fitted ? "" : " (not the save chip's size)");
    } else {
      std::fprintf(stderr, "save: cannot read %s\n", save);
      return 1;
    }
  }
#if DSPERATE_JIT
  if (jit && !ds::jit::attach(nds, true, true)) return 1;
#else
  (void)jit;
#endif
  ds::prof::set_level(std::getenv("DS_PROFILE"));
  std::fprintf(stderr, "host: %u cores\n", ds::host_cores());
  if (const char* w = std::getenv("DS_WATCH")) nds.bus.enable_watch(static_cast<ds::u32>(std::strtoul(w, nullptr, 16)));
  if (trace) {
    ts.out[0] = std::fopen((std::string(trace) + ".arm9.trace").c_str(), "w");
    ts.out[1] = std::fopen((std::string(trace) + ".arm7.trace").c_str(), "w");
  }
  const char* per_frame = std::getenv("TRACE_PER_FRAME");
  unsigned long long last9 = 0, last7 = 0;
  FILE* dump_out = dump ? std::fopen(dump, "wb") : nullptr;
  FILE* rec3d_out = rec3d_path ? std::fopen(rec3d_path, "wb") : nullptr;
  if (dump && !dump_out) { std::fprintf(stderr, "could not open %s\n", dump); return 1; }
  if (gpu_dump_path && gpu_dump_frame >= 0) nds.gpu3d.renderer().set_gpu_dump(gpu_dump_path, static_cast<ds::u64>(gpu_dump_frame));
  if (gpu3d) { std::string why; if (!nds.gpu3d.renderer().set_gpu(true, &why)) std::fprintf(stderr, "gpu3d: unavailable (%s), drawing on the CPU\n", why.c_str()); }
  FILE* hash_out = hash_frames ? std::fopen(hash_frames, "w") : nullptr;
  if (hash_frames && !hash_out) { std::fprintf(stderr, "could not open %s\n", hash_frames); return 1; }
  ds::input::Log log;
  if (replay) {
    if (!log.open_read(replay)) { std::fprintf(stderr, "cannot read %s\n", replay); return 1; }
    if (!frames_given) frames = static_cast<int>(log.frames());
    std::fprintf(stderr, "replay: %u frames from %s\n", log.frames(), replay);
  }
  FILE* audio_out = dump_audio ? std::fopen(dump_audio, "wb") : nullptr;
  if (dump_audio && !audio_out) { std::fprintf(stderr, "could not open %s\n", dump_audio); return 1; }
  ts.pc_hist = std::getenv("TRACE_PC_HIST") != nullptr;
  ts.stamp = std::getenv("TRACE_TIME") != nullptr; ts.nds = &nds;
  const char* trace_start = std::getenv("TRACE_START_FRAME");   // suppress trace output before this frame
  const int trace_from = trace_start ? std::atoi(trace_start) : 0;
  const char* trace_end_s = std::getenv("TRACE_END_FRAME");     // stop tracing at this frame (0 = never)
  const int trace_end = trace_end_s ? std::atoi(trace_end_s) : 0;
  // DS_WATCHDOG=<seconds>: no progress for that long dumps state and aborts.
  std::atomic<bool> wd_stop{false};
  std::thread wd;
  if (const char* w = std::getenv("DS_WATCHDOG")) {
    const int limit = std::atoi(w);
    wd = std::thread([&nds, &wd_stop, limit] {
      ds::u64 last = ~ds::u64{0}; int still = 0;
      while (!wd_stop.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const ds::u64 fc = nds.frame_count;
        if (fc == last) { if (++still >= limit) {
          std::fprintf(stderr, "[watchdog] no progress for %d s at frame %llu:\n", limit, (unsigned long long)fc);
          nds.gpu.debug_dump(stderr); std::fflush(stderr); std::abort(); } }
        else { last = fc; still = 0; }
      }
    });
  }
  std::vector<double> frame_ms;
  frame_ms.reserve(static_cast<size_t>(frames));
  if (load_state) {
    std::vector<ds::u8> bytes = slurp_file(load_state);
    ds::state::Reader r(bytes.data(), bytes.size());
    std::string err;
    if (bytes.empty() || !nds.load_state(r, err)) { std::fprintf(stderr, "cannot load state %s: %s\n", load_state, bytes.empty() ? "unreadable" : err.c_str()); return 1; }
    std::fprintf(stderr, "state: loaded %s (frame %llu)\n", load_state, static_cast<unsigned long long>(nds.frame_count));
    dump_cpu_timing(nds, "load");
    // replay continues from the state's frame, not the log's start
    if (log.reading()) { ds::input::Frame f; for (ds::u64 k = 0; k < nds.frame_count && log.read(f); ++k) {} }
  }
  auto write_state = [&](int after) {
    if (!save_state_path || save_state_at != after) return true;
    ds::state::Writer w; std::string err;
    if (!nds.save_state(w, err)) { std::fprintf(stderr, "cannot save state: %s\n", err.c_str()); return false; }
    dump_cpu_timing(nds, "save");
    FILE* f = std::fopen(save_state_path, "wb");
    if (!f) { std::fprintf(stderr, "cannot write %s\n", save_state_path); return false; }
    std::fwrite(w.data().data(), 1, w.data().size(), f); std::fclose(f);
    std::fprintf(stderr, "state: wrote %s after %d frames (%zu bytes)\n", save_state_path, after, w.data().size());
    return true;
  };
  if (!write_state(0)) return 1;
  const char* reboot_at = std::getenv("DS_REBOOT_AT");
  const int reboot_frame = reboot_at ? std::atoi(reboot_at) : -1;
  const char* reboot_rom = reboot_at ? std::strchr(reboot_at, ':') : nullptr;
  if (reboot_rom) ++reboot_rom;

#if DSPERATE_NET
  std::unique_ptr<ds::net::LanMp> lan;
  if (lan_host || lan_join || netplay) {
    lan = std::make_unique<ds::net::LanMp>();
    if (!lan->ok()) { std::fprintf(stderr, "lan: %s\n", lan->error().c_str()); return 1; }
    bool up;
    if (netplay) {
      const auto role = lan->start_auto(lan_name, 2500, lan_players);
      up = role != ds::net::LanMp::Role::None;
      if (up) std::fprintf(stderr, "netplay: %s\n", role == ds::net::LanMp::Role::Host ? "no session heard, hosting" : ("joined " + lan->peer_name()).c_str());
    } else up = lan_host ? lan->start_host(lan_host, lan_players) : lan->start_client(lan_name, lan_join);
    if (!up) { std::fprintf(stderr, "lan: %s\n", lan->error().c_str()); return 1; }
    std::fprintf(stderr, "lan: %s, player %d\n", lan->is_host() ? "hosting" : "joined", lan->my_id());
    nds.io.wifi.set_transport(lan.get());
  }
  std::unique_ptr<ds::net::SlirpDriver> slirp;
  if (internet) {
    if (lan_host || lan_join || netplay) { std::fprintf(stderr, "net: --internet is not local wireless; pick one\n"); return 1; }
    auto dns = ds::net::SlirpDriver::Dns::Custom;
    ds::u32 dns_addr = 0xB23E2BD4;   // 178.62.43.212, Wiimmfi's resolver
    const std::string where = dns_arg ? dns_arg : "wiimmfi";
    if (where == "host") { dns = ds::net::SlirpDriver::Dns::Host; dns_addr = 0; }
    else if (where != "wiimmfi") {
      in_addr parsed{};
      if (inet_pton(AF_INET, where.c_str(), &parsed) == 1) dns_addr = ntohl(parsed.s_addr);
      else { std::fprintf(stderr, "--dns: \"%s\" is not host, wiimmfi or an address\n", where.c_str()); return 1; }
    }
    slirp = std::make_unique<ds::net::SlirpDriver>();
    if (!slirp->start(dns, dns_addr)) { std::fprintf(stderr, "internet: %s\n", slirp->error().c_str()); return 1; }
    std::fprintf(stderr, "internet: up, DNS %s\n", where.c_str());
    nds.io.set_net_driver(slirp.get());
  }
#else
  if (lan_host || lan_join || netplay) { std::fprintf(stderr, "lan: built without DSPERATE_NET\n"); return 1; }
  if (internet) { std::fprintf(stderr, "internet: built without DSPERATE_NET\n"); return 1; }
#endif
  if (ds::mem::fmc::on()) ds::mem::fmc::set_counting(stats_from == 0);
  if (pc_profile && !ds::pcsample::start()) { std::fprintf(stderr, "--pc-profile: no sampler on this platform\n"); return 1; }
  const auto pace_start = std::chrono::steady_clock::now();
  for (int i = 0; i < frames; ++i) {
    if (pc_profile && i == stats_from) ds::pcsample::set_active(true);
    if (i == stats_from && ds::mem::fmc::on()) ds::mem::fmc::set_counting(true);
    static const bool fm_verify = std::getenv("DS_FASTMEM_VERIFY") != nullptr;
    if (fm_verify && nds.bus.arena_ && i > 0 && !nds.bus.fastmem_verify(nds.frame_count)) return 1;
    if (pace) {   // the DS's 59.83 Hz, timed from the run's start so sleep jitter doesn't accumulate
      const auto due = pace_start + std::chrono::microseconds(static_cast<long long>(i * 1000000.0 / 59.8261));
      std::this_thread::sleep_until(due);
    }
#if DSPERATE_NET
    if (lan) lan->process();
    if (slirp) slirp->process();
#endif
    if (reboot_rom && i == reboot_frame) {
      nds.reset();
      if (!nds.load_rom(reboot_rom)) std::fprintf(stderr, "reboot: could not read %s\n", reboot_rom);
      else { nds.setup_direct_boot(); std::fprintf(stderr, "reboot: direct boot at frame %d\n", i); }
    }
    const auto t0 = std::chrono::steady_clock::now();
    // Armed from frame 0 regardless of TRACE_START_FRAME: arming later drops
    // every translated block at that frame, changing the run.
#if DSPERATE_JIT
    if (trace && i == 0) ds::jit::set_trace(true);
#endif
    if (i == 0 && !trace && std::getenv("DS_SWI_LOG")) { nds.trace = swi_log_cb; nds.trace_user = &nds; }
    if (i == 0 && !trace) if (const char* e = std::getenv("DS_ENTRY_SNAP")) {
      static EntrySnap snap;
      std::string spec = e; const size_t c1 = spec.rfind(':'), c0 = spec.rfind(':', c1 - 1);
      if (c1 == std::string::npos || c0 == std::string::npos) { std::fprintf(stderr, "DS_ENTRY_SNAP=<dir>:<arm9 pc>:<arm7 pc>\n"); return 1; }
      snap.dir = spec.substr(0, c0); snap.nds = &nds;
      snap.pc[0] = static_cast<ds::u32>(std::strtoul(spec.c_str() + c0 + 1, nullptr, 16));
      snap.pc[1] = static_cast<ds::u32>(std::strtoul(spec.c_str() + c1 + 1, nullptr, 16));
      nds.trace = entry_snap_cb; nds.trace_user = &snap;
    }
    if (trace && i == trace_from) { nds.trace = trace_cb; nds.trace_user = &ts; }
    if (trace && trace_end && i == trace_end) { nds.trace = nullptr; nds.trace_user = nullptr; }
    nds.io.wifi.trace_frame(i);   // "# frame N" in the Wi-Fi trace, aligns it with --trace
    if (log.reading()) { ds::input::Frame in; if (log.read(in)) ds::input::apply(nds, in); }
    if (mic_tone_hz > 0) {
      static std::vector<ds::s16> tone(1600);   // ~95.7 kHz, finer than the DSi's 47.6 kHz mic clock
      static const double amp = std::getenv("DS_MIC_TONE_AMP") ? std::atof(std::getenv("DS_MIC_TONE_AMP")) : 8000.0;
      static double phase = 0;
      const double step = 2 * 3.14159265358979 * mic_tone_hz / (tone.size() * 59.8261);
      for (ds::s16& v : tone) { v = static_cast<ds::s16>(amp * std::sin(phase)); phase += step; }
      nds.io.set_mic(tone.data(), tone.size());
    }
    if (!touches.empty()) {
      const Touch* on = nullptr;
      for (const Touch& t : touches) if (i >= t.frame && i < t.frame + t.n) on = &t;
      if (on) nds.io.set_touch(on->x, on->y, true); else nds.io.set_touch(0, 0, false);
    }
    if (tas_after >= 0) {
      if (nds.io.wifi.host_syncs() > tas_seen) { tas_seen = nds.io.wifi.host_syncs(); tas_frame = i + tas_after; std::fprintf(stderr, "tap-after-sync: client synced at frame %d, tapping from %ld\n", i, tas_frame); }
      if (tas_frame >= 0 && i >= tas_frame) {
        const long k = i - tas_frame; const long cycle = 60;
        if (k / cycle < tas_rep && k % cycle < tas_n) nds.io.set_touch(static_cast<ds::u8>(tas_x), static_cast<ds::u8>(tas_y), true);
        else if (k / cycle < tas_rep) nds.io.set_touch(0, 0, false);
      }
    }
    // The 3D raster for frame i+1 runs during frame i, so this decides ahead.
    if (frameskip > 0) {
      // Skip/draw in whole display periods (display_phase_period), as a fixed pattern.
      const int period = nds.gpu.display_phase_period();
      const int skip = frameskip * period;
      const int cycle = skip + period;
      nds.gpu.set_frame_skip(skip > 0 && static_cast<int>((i + 1) % cycle) < skip);
    }
    // DS_DSI_SOFT_RESET_AT=<frame>: BPTWL soft reset (like a guest write of 1 to reg 0x11).
    if (static const int sr = std::getenv("DS_DSI_SOFT_RESET_AT") ? std::atoi(std::getenv("DS_DSI_SOFT_RESET_AT")) : -1; nds.dsi && i == sr) {
      nds.dsi_soft_reset_pending = true;
      nds.cpu(ds::Cpu::ARM7).halted = true;
    }
#if DSPERATE_NET
    if (lan) {   // --lan-* implies --pace
      // Spread the frame in 1 ms slices so a peer's CMD is answered promptly.
      const auto frame_end = pace_start + std::chrono::microseconds(static_cast<long long>((i + 1) * 1000000.0 / 59.8261));
      constexpr ds::u64 slice = ds::ARM9_CLOCK_HZ / 1000;
      constexpr int slices = static_cast<int>(ds::CYCLES_PER_FRAME / slice) + 1;
      for (int k = 1; !nds.run_frame_slice(slice); ++k)
        std::this_thread::sleep_until(frame_end - std::chrono::microseconds(static_cast<long long>(16700.0 * (slices - k) / slices)));
    } else
#endif
    nds.run_frame();
    if (!write_state(i + 1)) return 1;
    if (static const bool fh = std::getenv("DS_FRAME_HASH") != nullptr; fh) {
      auto fnv = [](const ds::u8* p, size_t n, ds::u64 h) { for (size_t k = 0; k < n; ++k) h = (h ^ p[k]) * 1099511628211ull; return h; };
      ds::u64 h = 1469598103934665603ull;
      h = fnv(nds.bus.main_ram.get(), 4u << 20, h);
      h = fnv(nds.bus.shared_wram.get(), 32u << 10, h);
      h = fnv(nds.bus.arm7_wram.get(), 64u << 10, h);
      h = fnv(nds.bus.dtcm.get(), 16u << 10, h);
      const auto& a9 = nds.cpu(ds::Cpu::ARM9).hot; const auto& a7 = nds.cpu(ds::Cpu::ARM7).hot;
      std::fprintf(stderr, "[fh] %d mem %016llx a9", i, (unsigned long long)h);
      for (int r = 0; r < 16; ++r) std::fprintf(stderr, " %08x", a9.regs[r]);
      std::fprintf(stderr, " %08x a7", a9.cpsr);
      for (int r = 0; r < 16; ++r) std::fprintf(stderr, " %08x", a7.regs[r]);
      std::fprintf(stderr, " %08x\n", a7.cpsr);
      static const char* dumpf = std::getenv("DS_FRAME_DUMP");   // "<frame>:<path>": write main RAM + WRAM + DTCM after that frame
      if (dumpf && std::atoi(dumpf) == i) {
        FILE* f = std::fopen(std::strchr(dumpf, ':') + 1, "wb");
        std::fwrite(nds.bus.main_ram.get(), 1, nds.bus.main_ram_size(), f); std::fwrite(nds.bus.shared_wram.get(), 1, 32u << 10, f);
        std::fwrite(nds.bus.arm7_wram.get(), 1, 64u << 10, f); std::fwrite(nds.bus.dtcm.get(), 1, 16u << 10, f); std::fclose(f);
      }
    }
    if (scaled_out && i >= dump_from && (dump_count <= 0 || i < dump_from + dump_count))
      for (int k = 0; k < 2; ++k) std::fwrite(scaled_px[k].data(), 4, scaled_px[k].size(), scaled_out);
    if (dump_out && i >= dump_from && (dump_count <= 0 || i < dump_from + dump_count)) {
      // raw 0xAARRGGBB, top screen then bottom, 256x192 each, one record per frame
      std::fwrite(nds.gpu.framebuffer(0), 4, ds::SCREEN_W * ds::SCREEN_H, dump_out);
      std::fwrite(nds.gpu.framebuffer(1), 4, ds::SCREEN_W * ds::SCREEN_H, dump_out);
    }
    if (rec3d_out && i >= dump_from && (dump_count <= 0 || i < dump_from + dump_count)) {
      const auto ref = nds.gpu3d.frame_ref(false);
      for (int y = 0; y < static_cast<int>(ds::SCREEN_H); ++y) std::fwrite(nds.gpu3d.line(ref, static_cast<ds::u32>(y)), 4, ds::SCREEN_W, rec3d_out);
    }
    if (hash_out) {
      auto fnv = [](const ds::u32* p) { ds::u64 h = 1469598103934665603ull; for (ds::u32 k = 0; k < ds::SCREEN_W * ds::SCREEN_H; ++k) h = (h ^ p[k]) * 1099511628211ull; return h; };
      std::fprintf(hash_out, "%d %016llx %016llx\n", i, (unsigned long long)fnv(nds.gpu.framebuffer(0)), (unsigned long long)fnv(nds.gpu.framebuffer(1)));
    }
    if (audio_out) {
      ds::s16 buf[2048 * 2]; size_t n;
      while ((n = nds.spu.take(buf, 2048)) != 0) std::fwrite(buf, 4, n, audio_out);
    } else nds.spu.drain();
#if DSPERATE_JIT
    if (i == stats_from && jit) ds::jit::density_reset();
#endif
    if (i >= stats_from) {
      frame_ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
      // DS_FRAME_SERIES=<path>: run-order series, "ms polygons raster_ns" per line.
      static FILE* series = [] { const char* p = std::getenv("DS_FRAME_SERIES"); return p ? std::fopen(p, "w") : nullptr; }();
      if (series) std::fprintf(series, "%.3f %u %llu\n", frame_ms.back(), nds.gpu3d.render_polygon_count(),
                               (unsigned long long)nds.gpu3d.last_raster_ns());
    }
    if (nds.exit_requested) { std::fprintf(stderr, "exit requested at frame %d: ending the run\n", i); break; }
    if (nds.power_off) {
      // No cart: firmware settings-pages exit, save and reboot as power would.
      std::fprintf(stderr, "power off at frame %d%s\n", i, nds.cart ? "" : "; saving settings and rebooting");
      if (!nds.cart) {
        if (fw_override) {
          std::string err;
          if (!nds.save_firmware_override(fw_override, err)) std::fprintf(stderr, "firmware override: cannot save: %s\n", err.c_str());
        }
#if DSPERATE_JIT
        if (jit) ds::jit::flush_all();
#endif
        nds.reset();   // clears power_off, re-seeds the clock if --rtc-host
      } else {
        nds.power_off = false;
      }
    }
    ds::prof::frame_mark();
    if (per_frame && trace) { std::fprintf(stderr, "frame %d arm9 %llu arm7 %llu\n", i, ts.executed[0] - last9, ts.executed[1] - last7); last9 = ts.executed[0]; last7 = ts.executed[1]; }
  }
  wd_stop.store(true); if (wd.joinable()) wd.join();
  if (pc_profile && !ds::pcsample::write(pc_profile)) std::fprintf(stderr, "--pc-profile: cannot write %s\n", pc_profile);
  if (fw_override && nds.firmware_override_dirty()) {
    std::string err;
    if (!nds.save_firmware_override(fw_override, err)) std::fprintf(stderr, "firmware override: cannot save: %s\n", err.c_str());
    else std::fprintf(stderr, "firmware override: saved %s\n", fw_override);
  }
  if (dump_out) std::fclose(dump_out);
  if (hash_out) std::fclose(hash_out);
  if (scaled_out) std::fclose(scaled_out);
  if (audio_out) std::fclose(audio_out);
  if (ts.pc_hist) {
    for (int c = 0; c < 2; ++c) {
      std::vector<std::pair<ds::u32, unsigned long long>> v(ts.hist[c].begin(), ts.hist[c].end());
      std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
      std::fprintf(stderr, "arm%d hottest pcs:", c ? 7 : 9);
      for (size_t k = 0; k < v.size() && k < 12; ++k) std::fprintf(stderr, " %08x:%llu", v[k].first, v[k].second);
      std::fputc('\n', stderr);
    }
  }
  if (trace) { std::fclose(ts.out[0]); std::fclose(ts.out[1]);
    std::fprintf(stderr, "arm9: %llu lines (%llu instrs), arm7: %llu lines (%llu instrs), cap %llu lines each\n",
                 ts.count[0], ts.executed[0], ts.count[1], ts.executed[1], ts.max); }
  ds::prof::report();
#if DSPERATE_JIT
  if (ds::prof::enabled && jit) ds::jit::report(stderr);
#endif
#if DSPERATE_NET
  if (lan) std::fprintf(stderr, "lan: reply/host waits %u, total %.1f ms, max %.1f ms, timeouts %u\n", lan->wait_count(), lan->wait_total_ms(), lan->wait_max_ms(), lan->wait_timeouts());
#endif
  ds::interp::census_report(nds.frame_count);
  if (std::getenv("DS_STATE_DUMP")) {
    for (int c = 0; c < 2; ++c) {
      const ds::Cpu cpu = c == 0 ? ds::Cpu::ARM9 : ds::Cpu::ARM7;
      const auto& ci = nds.io.cpu_io[c];
      std::fprintf(stderr, "[state] %s pc %08x halted %d IME %x IE %08x IF %08x IE&IF %08x fifocnt %04x romctrl %08x auxspicnt %04x\n",
                   c == 0 ? "arm9" : "arm7", nds.cpu(cpu).hot.regs[15], nds.cpu(cpu).halted, ci.ime, ci.ie, ci.if_, ci.ie & ci.if_,
                   nds.io.read(cpu, 0x04000184, 16), nds.io.read(cpu, 0x040001A4, 32), nds.io.read(cpu, 0x040001A0, 16));
      for (int d = 0; d < 4; ++d) std::fprintf(stderr, "[state]   dma%d cnt %08x src %08x dst %08x\n", d, nds.io.read(cpu, 0x040000B8 + d * 12, 32), nds.io.read(cpu, 0x040000B0 + d * 12, 32), nds.io.read(cpu, 0x040000B4 + d * 12, 32));
    }
  }
  if (ds::mem::fmc::on()) ds::mem::fmc::report(frame_ms.size());
  nds.bus.fastmem_report();
  ds::prof::deduct_probe_overhead(frame_ms, nullptr);
  ds::frame_report(frame_ms);
  ds::prof::frame_breakdown(frame_ms);
  if (nds.dsi_nand.valid())
    std::fprintf(stderr, "nand: %llu block reads, %llu block writes\n",
                 (unsigned long long)nds.dsi_nand.reads, (unsigned long long)nds.dsi_nand.writes);
  if (nds.dsi_sd.valid()) {
    std::fprintf(stderr, "sd: %llu block reads, %llu block writes\n",
                 (unsigned long long)nds.dsi_sd.reads, (unsigned long long)nds.dsi_sd.writes);
    const ds::io::SdCard::Report r = nds.dsi_sd.sync();
    if (r.files || r.dirs || r.removed) std::fprintf(stderr, "dsi sd: synced %d files, %d new folders, %d removed\n", r.files, r.dirs, r.removed);
    for (const std::string& n : r.notes) std::fprintf(stderr, "dsi sd: %s\n", n.c_str());
  }
  if (dsi_persist && nds.dsi_nand.valid()) {
    const std::string d = dsi_persist;
    const ds::io::NandPersistReport r = ds::io::nand_export(nds.dsi_nand, nds.bios_native_dsi ? nds.bus.bios7i.get() : nullptr,
                                                            {d, nds.dsi_nand_synthetic ? std::string() : d + "/nand.ovr", d + "/photos"});
    std::fprintf(stderr, "dsi persist: out %d saves, %d system files, %d photos\n", r.saves, r.system_files, r.photos);
    for (const std::string& n : r.notes) std::fprintf(stderr, "dsi persist: %s\n", n.c_str());
  }
  std::fprintf(stderr, "ran %llu frames, %llu cycles\n",
              static_cast<unsigned long long>(nds.frame_count),
              static_cast<unsigned long long>(nds.sched.now()));
  return 0;
}
