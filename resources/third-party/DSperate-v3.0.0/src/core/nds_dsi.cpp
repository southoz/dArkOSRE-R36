// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// The DSi machine's boot: BIOS pair, launcher main-RAM leftovers, NWRAM
// mapping, ARM9i/ARM7i binaries and modcrypt, CP15/SCFG state for a DSiWare
// title. Mirrors melonDS DSi::SetupDirectBoot (DSi-mode branch).
#include "core/nds.h"
#include "core/io/dsi_nand_synth.h"
#include "core/io/dsi_nand_launch.h"
#include "core/crypto/sha1.h"
#include "core/cpu/cp15.h"
#if DSPERATE_JIT
#include "core/cpu/jit/jit.h"
#endif

extern "C" {
#include "core/crypto/aes.h"
#include "core/bios/freebios.h"
}

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <fstream>

namespace ds {

namespace {

std::vector<u8> slurp_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<u8>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

void bswap128(u8* dst, const u8* src) { for (int i = 0; i < 16; ++i) dst[i] = src[15 - i]; }

// melonDS DSi::DecryptModcryptArea: AES-CTR over the binary the area covers,
// in place in main RAM. Byte-reversed 128-bit blocks: the DSi's AES engine is big-endian.
void decrypt_modcrypt_area(NDS& nds, u32 offset, u32 size, const u8* iv, bool trim) {
  if (!offset || !size) return;
  const cart::Header& h = nds.cart->header();
  const cart::TwlHeader& t = nds.cart->twl();
  u8 key[16], tmp[16];
  if ((h.reserved2 & (1u << 4)) || (t.app_flags & (1u << 7))) {
    nds.cart->rom_read(0, tmp, 16);                      // dev key: the first 16 header bytes
  } else {
    u8 kx[16], ky[16];
    std::memcpy(kx, "Nintendo", 8);
    kx[8] = static_cast<u8>(h.game_code[0]); kx[9] = static_cast<u8>(h.game_code[1]); kx[10] = static_cast<u8>(h.game_code[2]); kx[11] = static_cast<u8>(h.game_code[3]);
    kx[12] = static_cast<u8>(h.game_code[3]); kx[13] = static_cast<u8>(h.game_code[2]); kx[14] = static_cast<u8>(h.game_code[1]); kx[15] = static_cast<u8>(h.game_code[0]);
    std::memcpy(ky, t.arm9i_hash, 16);
    DsiAes::derive_normal_key(kx, ky, tmp);
  }
  bswap128(key, tmp);
  u8 ivr[16]; bswap128(ivr, iv);
  AES_ctx ctx;
  AES_init_ctx_iv(&ctx, key, ivr);

  const u32 rounded = (size + 0xF) & ~0xFu;
  auto covers = [&](u32 rom_off, u32 bin_size) { return offset >= rom_off && offset + rounded <= rom_off + ((bin_size + 0xF) & ~0xFu); };
  u32 addr, bin;
  if (covers(h.arm9_rom_offset, h.arm9_size)) addr = h.arm9_ram_address, bin = h.arm9_size;
  else if (covers(h.arm7_rom_offset, h.arm7_size)) addr = h.arm7_ram_address, bin = h.arm7_size;
  else if (covers(t.arm9i_rom_offset, t.arm9i_size)) addr = t.arm9i_ram_address, bin = t.arm9i_size;
  else if (covers(t.arm7i_rom_offset, t.arm7i_size)) addr = t.arm7i_ram_address, bin = t.arm7i_size;
  else return;
  // Header's area size is already rounded, so its last block can run past
  // the binary into keystream; `trim` (launcher only) decrypts just the binary.
  const u32 keep = std::min(size, bin);
  u8 tail[32];
  for (u32 k = 0; k < rounded - keep && k < sizeof tail; ++k) tail[k] = nds.bus.dma_read8(Cpu::ARM9, addr + keep + k);
  for (u32 i = 0; i < size; i += 16) {
    u32 data[4];
    for (int k = 0; k < 4; ++k) data[k] = nds.bus.dma_read32(Cpu::ARM9, addr + i + k * 4);
    bswap128(tmp, reinterpret_cast<const u8*>(data));
    AES_CTR_xcrypt_buffer(&ctx, tmp, 16);
    bswap128(reinterpret_cast<u8*>(data), tmp);
    for (int k = 0; k < 4; ++k) nds.bus.dma_write32(Cpu::ARM9, addr + i + k * 4, data[k]);
  }
  if (trim) for (u32 k = 0; k < rounded - keep && k < sizeof tail; ++k) nds.bus.dma_write8(Cpu::ARM9, addr + keep + k, tail[k]);
}

} // namespace

void NDS::set_dsi(bool on) { dsi = on; }

bool NDS::load_dsi_bios(const std::string& p9i, const std::string& p7i, std::string* err) {
  auto fail = [&](const std::string& m) { if (err) *err = m; return false; };
  auto exists = [](const std::string& p) { return !p.empty() && std::ifstream(p).good(); };
  const bool have9 = exists(p9i), have7 = exists(p7i);
  bios_native_dsi = false;
  if (!have9 && !have7) return true;   // no DSi: fine for a DS game
  if (have9 != have7) return fail(std::string(have9 ? p7i : p9i) + ": the other DSi BIOS half is present; both or neither");
  std::vector<u8> b9 = slurp_file(p9i), b7 = slurp_file(p7i);
  if (b9.size() != mem::Bus::BIOS9I_SIZE) return fail(p9i + ": not a 64 KB DSi ARM9 BIOS");
  if (b7.size() != mem::Bus::BIOS7I_SIZE) return fail(p7i + ": not a 64 KB DSi ARM7 BIOS");
  std::memcpy(bus.bios9i.get(), b9.data(), b9.size());
  std::memcpy(bus.bios7i.get(), b7.data(), b7.size());
  bios_native_dsi = true;
  // Fold into the BIOS identity a save state checks.
  for (u32 i = 0; i < mem::Bus::BIOS9I_SIZE; ++i) bios_id = (bios_id ^ bus.bios9i.get()[i]) * 1099511628211ull;
  for (u32 i = 0; i < mem::Bus::BIOS7I_SIZE; ++i) bios_id = (bios_id ^ bus.bios7i.get()[i]) * 1099511628211ull;
  return true;
}

bool NDS::load_dsi_boot_blobs(const std::string& path, std::string* err) {
  std::vector<u8> b = slurp_file(path);
  if (b.size() != 0x128 + 0x14 + 0x18) { if (err) *err = path + ": not a 0x154-byte DSi boot blob"; return false; }
  dsi_boot_blobs = std::move(b);
  return true;
}

bool NDS::load_dsi_nand(const std::string& path, std::string* err, bool write_through) {
  if (!dsi_nand.open(path, write_through)) {
    if (err) *err = path + ": not a DSi NAND image (needs the nocash footer holding the eMMC CID and console ID)";
    return false;
  }
  return true;
}

// Boots the DSi from the NAND instead of staging a direct boot: reads
// boot2's location from the NAND's boot info, applies its NWRAM mapping,
// decrypts boot2 into place, seeds the state the missing BIOS code would
// have left, and enters boot2 directly (melonDS DSi.cpp LoadNAND,
// !FullBIOSBoot branch). boot2 brings up the launcher, which autoloads the title.
bool NDS::boot_dsi_nand() {
  if (!dsi_nand.valid()) return false;
  io::DsiIo& d = io.dsi;

  // reset() sets CP15 up for a direct boot; boot2 is real BIOS code and
  // programs CP15 itself, so give it the ARM9's true reset value instead
  // (melonDS CP15::Reset) -- boot2 reads the control register back and branches on it.
  {
    CpuContext& a9 = cpu(Cpu::ARM9);
    a9.cp15_control = 0x00002078;
    a9.cp15_itcm = 0;
    a9.cp15_dtcm = 0;
    a9.update_tcm_windows();
    bus.update_tcm(a9);
  }

  // NWRAM must be reachable before the mapping below means anything.
  d.scfg_ext[0] |= 1u << 25;
  d.scfg_ext[1] |= 1u << 25;
  // Card slot powered off so it's back in reset (else still in KEY2 mode from the last boot).
  d.scfg_mc = static_cast<u16>(0x0010 | (cart ? 0 : 1));
  io.cart.romctrl &= ~(1u << 29);
  io.update_cart_reset();
  for (int i = 0; i < 3; ++i) std::memset(bus.nwram[i].get(), 0, mem::Bus::NWRAM_BANK_SIZE);

  // Boot info block: where boot2 lives and where it goes. Raw NAND bytes, outside the AES-CTR'd filesystem.
  u32 bp[8], mbk[12];
  dsi_nand.read(0x220, sizeof bp, reinterpret_cast<u8*>(bp));
  dsi_nand.read(0x380, sizeof mbk, reinterpret_cast<u8*>(mbk));

  // NWRAM mapping boot2 expects: slots 0-4 shared, 5-7 each CPU's own windows, 8 the write protect.
  for (int c = 0; c < 2; ++c) {
    for (int i = 0; i < 5; ++i) d.mbk[c][i] = mbk[i];
    for (int i = 0; i < 3; ++i) d.mbk[c][5 + i] = mbk[(c == 0 ? 5 : 8) + i];
    d.mbk[c][8] = mbk[11] & 0x00FFFF0F;
  }
  // A DSi resets with all shared WRAM on the ARM7 (melonDS DSi::Reset:
  // MapSharedWRAM(3)); under the DS default of 0 boot2's ARM7 memset would
  // wrap and wipe 0x0380FFC8.
  io.wramcnt = 3;
  bus.update_nwram();

  // boot2: AES-CTR, fixed key, IV derived from the aligned size, byte-reversed 16-byte blocks.
  auto load_boot2 = [&](u32 offset, u32 size_aligned, u32 dst, Cpu cpu) {
    static const u8 key[16] = {0xAD, 0x34, 0xEC, 0xF9, 0x62, 0x6E, 0xC2, 0x3A,
                               0xF6, 0xB4, 0x6C, 0x00, 0x80, 0x80, 0xEE, 0x98};
    u8 tmp[16], iv[16];
    const u32 sz = size_aligned;
    std::memcpy(&tmp[0], &sz, 4);
    const u32 neg = ~sz + 1, inv = ~sz, zero = 0;
    std::memcpy(&tmp[4], &neg, 4);
    std::memcpy(&tmp[8], &inv, 4);
    std::memcpy(&tmp[12], &zero, 4);
    bswap128(iv, tmp);

    AES_ctx ctx;
    AES_init_ctx_iv(&ctx, key, iv);
    for (u32 i = 0; i < size_aligned; i += 16) {
      u8 blk[16];
      dsi_nand.read(offset + i, 16, blk);
      bswap128(tmp, blk);
      AES_CTR_xcrypt_buffer(&ctx, tmp, 16);
      bswap128(blk, tmp);
      for (u32 k = 0; k < 16; k += 4) {
        u32 v;
        std::memcpy(&v, &blk[k], 4);
        bus.dma_write32(cpu, dst, v);
        dst += 4;
      }
    }
  };
  load_boot2(bp[0], bp[3], bp[2], Cpu::ARM9);
  load_boot2(bp[4], bp[7], bp[6], Cpu::ARM7);

  if (getenv("DS_DEBUG_MBK")) {
    for (u32 a : {0x037B8000u, 0x037C0000u, 0x037D0000u, 0x037D5190u, 0x037D8000u, 0x037DF000u})
      std::fprintf(stderr, "[boot2] arm7 view %08x = %08x\n", a, bus.dma_read32(Cpu::ARM7, a));
  }

  // What the missing boot ROM would have left: eMMC CID and constants the
  // ARM7 reads back, plus BIOS routines boot2 calls, copied into ITCM/ARM7
  // WRAM at melonDS's addresses.
  const u8* cid = dsi_nand.emmc_cid();
  auto w7 = [&](u32 a, u32 v) { bus.dma_write32(Cpu::ARM7, a, v); };
  auto w7h = [&](u32 a, u16 v) { bus.dma_write16(Cpu::ARM7, a, v); };
  const u32 e = 0x03FFE6E4;
  for (u32 i = 0; i < 16; i += 4) { u32 v; std::memcpy(&v, cid + i, 4); w7(e + i, v); }
  w7h(e + 0x2C, 0x0001); w7h(e + 0x2E, 0x0001);
  w7h(e + 0x3C, 0x0100); w7h(e + 0x3E, 0x40E0); w7h(e + 0x42, 0x0001);

  const u8* b9 = bus.bios9i.get();
  u8* itcm = bus.itcm.get();
  std::memcpy(itcm + 0x4400, b9 + 0x87F4, 0x400);
  std::memcpy(itcm + 0x4800, b9 + 0x9920, 0x80);
  std::memcpy(itcm + 0x4894, b9 + 0x99A0, 0x1048);
  std::memcpy(itcm + 0x58DC, b9 + 0xA9E8, 0x1048);

  const u8* b7 = bus.bios7i.get();
  std::vector<u8> init(0x3C00, 0);
  std::memcpy(&init[0x0000], b7 + 0x8188, 0x200);
  std::memcpy(&init[0x0200], b7 + 0xB5D8, 0x40);
  std::memcpy(&init[0x0254], b7 + 0xC6D0, 0x1048);
  std::memcpy(&init[0x129C], b7 + 0xD718, 0x1048);
  for (u32 i = 0; i < init.size(); i += 4) { u32 v; std::memcpy(&v, &init[i], 4); w7(0x03FFC400 + i, v); }

  cpu(Cpu::ARM9).jump(bp[2], false);
  cpu(Cpu::ARM7).jump(bp[6], false);
  // Pipeline-fill cost charged after the first instruction (melonDS's
  // JumpTo); defer_cost, not boot_stall, so the timeline isn't shifted.
  cpu(Cpu::ARM9).defer_cost += 64;
  cpu(Cpu::ARM7).defer_cost += 4;
  std::fprintf(stderr, "dsi: boot2 from NAND -- ARM9 %08X (%u bytes), ARM7 %08X (%u bytes)\n",
               bp[2], bp[3], bp[6], bp[7]);
  return true;
}

// melonDS DSi::SoftReset order. Leaves main RAM, BPTWL registers, GPU, SPU,
// timers, IRQ, DMA and armed events alone; re-runs the boot ROM (boot2 from the NAND).
void NDS::dsi_soft_reset() {
  dsi_soft_reset_pending = false;
  dsi_loader_launched = false;
  dsi_dsp_started = false;
  if (dsi_nand_synthetic) {
    // Nothing to reset into; ARM7 stays halted by the exit request.
    std::fprintf(stderr, "dsi: soft reset on a synthesised NAND: the title is leaving\n");
    exit_requested = true;
    return;
  }
  std::fprintf(stderr, "dsi: soft reset\n");

  // Keep what the recompiler/sibling link hang off the hot block; reset() clears it whole.
  for (CpuContext* c : {arm9.get(), arm7.get()}) {
    const u64 exit_native = c->hot.exit_native, other = c->hot.other_cpu;
    c->reset(c->which, this);
    c->hot.exit_native = exit_native;
    c->hot.other_cpu = other;
  }
  io.dsp.reset();                // melonDS: NWRAM is remapped by the boot ROM, so the DSP presumably resets too
  boot_dsi_nand();               // LoadNAND: NWRAM, MBK, WRAMCNT 3, boot2, the CPUs' entry points
  io.sd.reset();
  io.sdio.reset();
  io.aes.reset();                // the console ID is unchanged, so key slots 1 and 3 come back the same

  io::DsiIo& d = io.dsi;
  d.scfg_bios = 0x0101;
  d.scfg_clock9 = 0x0187; d.scfg_clock7 = 0x0187;
  bus.set_clock9_shift(2);       // the launcher may have dropped the ARM9 to 67 MHz
  d.scfg_ext[0] = 0x8307F100; d.scfg_ext[1] = 0x93FFFB06;
  d.scfg_mc = static_cast<u16>(0x0010 | (cart ? 0 : 1));   // boot_dsi_nand put the card back in reset
  d.scfg_rst = 0;
  io.dsp.set_rst_line(false);
  io.dispstat[0] |= 0x40; io.dispstat[1] |= 0x40;   // LCD init flag
  bus.update_nwram();            // SCFG_EXT bit 25 (NWRAM) was just rewritten
  bus.update_vram_timings();
  bus.update_main_ram();         // 16 MB again: a DS title the menu started ran at 4 MB (melonDS keeps its mask here; the register says 16)

#if DSPERATE_JIT
  if (jit::has_runtime()) jit::flush_all();   // boot2 went over NWRAM and ITCM
#endif
}

bool NDS::load_dsi_nand_title(u32 title_lo, std::string* err) {
  if (!dsi_nand.valid()) { if (err) *err = "no NAND is loaded"; return false; }
  if (!bios_native_dsi) { if (err) *err = "the DSi BIOS pair is not loaded"; return false; }
  std::vector<u8> srl;
  u32 content_id = 0;
  if (!io::nand_read_title_app(dsi_nand, bus.bios7i.get(), title_lo, srl, content_id, err)) return false;
  if (dsi_boot_blobs.empty() && !io::nand_boot_blobs(dsi_nand, bus.bios7i.get(), dsi_boot_blobs, err)) return false;
  if (!load_rom_image(std::move(srl))) { if (err) *err = "the title's .app is not a ROM this can load"; return false; }
  dsi_hle_content_id = content_id;
  dsi_nand.mark_state_base();
  return true;
}

bool NDS::prepare_dsi_hle(const bios::UserSettings& user, std::string* err, std::vector<std::string>* report) {
  auto fail = [&](const std::string& m) { if (err) *err = m; return false; };
  auto note = [&](const std::string& m) { if (report) report->push_back(m); };
  if (!cart) return fail("the launcher hand-off needs a DSiWare ROM");
  if (!bios_native_dsi) return fail("the launcher hand-off needs the DSi BIOS pair");
  const cart::TwlHeader& t = cart->twl();
  const io::DsiRegion region = io::dsi_region_for(t.region_flags, user.language);
  u64 console_id = dsi_nand.valid() ? dsi_nand.console_id() : io::kSynthConsoleId;
  io::DsiConsoleFiles files = io::make_dsi_console_files(user, region, console_id);
  dsi_font_hle = false;
  if (!dsi_nand.valid()) {
    if (!dsi_font_path.empty()) {
      files.font = slurp_file(dsi_font_path);
      if (files.font.size() < 0x100) return fail(dsi_font_path + ": not a TWLFontTable.dat");
      // A title finds fonts only in its own region's layout.
      const int want = region.region == 4 || region.region == 5 ? region.region : 0;
      const int have = io::font_table_region(files.font);
      static const char* kLayout[6] = {"normal", "", "", "", "Chinese", "Korean"};
      if (have >= 0 && have != want)
        note(dsi_font_path + " is a " + kLayout[have] + " font table; this title needs the " + kLayout[want] + " one and will not find its fonts");
    } else {
      files.font = io::builtin_dsi_font(region.region);
    }
    if (io::is_builtin_font_signature(files.font.data())) {
      crypto::sha1(&files.font[0x80], 0x20, dsi_font_digest);
      dsi_font_hle = true;
    }
  }
  static const char* kRegionNames[6] = {"Japan", "USA", "Europe", "Australia", "China", "Korea"};

  if (firmware_synthetic && firmware.size() != 0x20000) {
    firmware = bios::generate_firmware_dsi(user, region.language, region.language_mask);
    firmware_ap_slot = bios::stamp_access_point(firmware);
    firmware_id = 1469598103934665603ull;
    for (u8 b : firmware) firmware_id = (firmware_id ^ b) * 1099511628211ull;
    fw_page_dirty.assign((firmware.size() + FW_PAGE - 1) / FW_PAGE, 0);
    fw_dirty_pages = 0;
    note("firmware: a generated DSi firmware carrying the user settings");
  }
  if (dsi_boot_blobs.empty() && !dsi_nand.valid()) {
    dsi_boot_blobs = files.boot_blobs();
    note(std::string("console settings: generated for ") + kRegionNames[region.region] + ", language " + std::to_string(region.language));
  }
  if (!dsi_nand.valid()) {
    std::vector<u8> srl(cart->rom_size());
    cart->rom_read(0, srl.data(), static_cast<u32>(srl.size()));
    std::string why;
    if (!io::build_synthetic_nand(dsi_nand, bus.bios7i.get(), srl, files, &why)) return fail("synthesising the NAND: " + why);
    dsi_nand_synthetic = true;
    dsi_nand.mark_baseline();
    dsi_nand.mark_state_base();
    dsi_hle_content_id = 0;
    // DS_NAND_DUMP=<file>: write the synthesised image out with a nocash footer.
    if (const char* dump = getenv("DS_NAND_DUMP")) {
      if (std::FILE* f = std::fopen(dump, "wb")) {
        std::vector<u8> sec(512);
        for (u64 a = 0; a < dsi_nand.length(); a += 512) { dsi_nand.peek(a, 512, sec.data()); std::fwrite(sec.data(), 1, 512, f); }
        u8 footer[0x40] = {};
        std::memcpy(footer, "DSi eMMC CID/CPU", 16);
        std::memcpy(footer + 16, dsi_nand.emmc_cid(), 16);
        const u64 id = dsi_nand.console_id();
        std::memcpy(footer + 32, &id, 8);
        std::fwrite(footer, 1, sizeof footer, f);
        std::fclose(f);
      }
    }
    note("nand: synthesised in memory (" + std::to_string(srl.size() >> 10) + " KB title)" +
         (dsi_font_hle ? std::string("; DSperate's own ") + (io::font_table_region(files.font) == 4 ? "Chinese " : io::font_table_region(files.font) == 5 ? "Korean " : "") + "system font"
                       : "; system font from " + dsi_font_path));
  }
  // Redo what Io::dsi_reset derives from the NAND and firmware, so this also
  // works on a machine the frontend has already reset.
  normalise_touch_calibration();
  io.dsi.console_id = dsi_nand.console_id();
  io.sd.attach_nand(&dsi_nand);
  io.sd.reset();
  io.aes.reset();
  return true;
}

bool NDS::dsi_hle_swi(CpuContext& cpu, u32 number) {
  // SWI 27h SHA1_Calc(r0 = digest out, r1 = data, r2 = length), watched only:
  // the launcher hashing the loader cart's header is its launch signal.
  if (number == 0x27) {
    if (dsi_loader_watch && cart && cpu.which == Cpu::ARM9 && cpu.hot.regs[2] == 0x160 && (cpu.hot.regs[1] >> 24) == 0x02) {
      u8 want[0x160];
      cart->source().read(0, want, sizeof want);
      u32 i = 0;
      while (i < sizeof want && bus.dma_read8(cpu.which, cpu.hot.regs[1] + i) == want[i]) ++i;
      if (i == sizeof want) dsi_loader_launched = true;
    }
    return false;
  }
  // SWI 22h RSA_Decrypt_Unpad(r0 = key heap, r1 = digest out, r2 = signature):
  // returns 1 and the 20-byte digest on success.
  if (number != 0x22 || !dsi_font_hle) return false;
  const u32 sig = cpu.hot.regs[2], dst = cpu.hot.regs[1];
  if ((sig >> 24) != 0x02 || (dst >> 24) != 0x02) return false;   // main RAM only: no reads with side effects
  u8 buf[0x80];
  for (u32 i = 0; i < sizeof buf; ++i) buf[i] = bus.dma_read8(cpu.which, sig + i);
  if (!io::is_builtin_font_signature(buf)) return false;
  for (u32 i = 0; i < 20; ++i) bus.dma_write8(cpu.which, dst + i, dsi_font_digest[i]);
  cpu.hot.regs[0] = 1;
  return true;
}

void NDS::dsi_autoload(u32 title_lo, u32 title_hi) {
  u8 tlnc[0x100] = {};
  tlnc[0] = 'T'; tlnc[1] = 'L'; tlnc[2] = 'N'; tlnc[3] = 'C';
  tlnc[4] = 0x01;
  tlnc[5] = 0x18;
  // PrevTitleID stays zero ("anonymous": nothing launched us).
  std::memcpy(&tlnc[0x10], &title_lo, 4);
  std::memcpy(&tlnc[0x14], &title_hi, 4);
  const u32 flags = 0x01u | (0x03u << 1) | (1u << 4);
  std::memcpy(&tlnc[0x18], &flags, 4);
  const u16 crc = bios::crc16(&tlnc[0x08], 0x18, 0xFFFF);
  std::memcpy(&tlnc[0x06], &crc, 2);
  for (u32 i = 0; i < sizeof tlnc; ++i) bus.dma_write8(Cpu::ARM9, 0x02000300 + i, tlnc[i]);
  io.dsi.bptwl_regs[0x70] = 1;   // the BPTWL boot flag (melonDS SetBootFlag)
}

void NDS::setup_direct_boot_dsi() {
  const cart::Header& h = cart->header();
  const cart::TwlHeader& t = cart->twl();
  auto w32 = [&](u32 a, u32 v) { bus.dma_write32(Cpu::ARM9, a, v); };
  auto w16 = [&](u32 a, u16 v) { bus.dma_write16(Cpu::ARM9, a, v); };
  auto w8  = [&](u32 a, u8 v)  { bus.dma_write8(Cpu::ARM9, a, v); };
  auto rd32 = [&](const u8* p) { u32 v; std::memcpy(&v, p, 4); return v; };
  auto rom32 = [&](u32 off) { return cart->rom_read32_at(off); };
  io::DsiIo& d = io.dsi;

  // ---- SCFG_BIOS and the NWRAM mapping from the header's MBK block ----
  d.scfg_bios = 0x0101;
  d.mbk[0][8] = d.mbk[1][8] = 0;                     // protection off while the header's map goes in
  for (int i = 0; i < 4; ++i) io.mbk_map_slot(0, i, static_cast<u8>(t.mbk[0] >> (i * 8)));
  for (int i = 0; i < 8; ++i) io.mbk_map_slot(1, i, static_cast<u8>(t.mbk[1 + (i >> 2)] >> ((i & 3) * 8)));
  for (int i = 0; i < 8; ++i) io.mbk_map_slot(2, i, static_cast<u8>(t.mbk[3 + (i >> 2)] >> ((i & 3) * 8)));
  for (int i = 0; i < 3; ++i) { io.mbk_map_range(Cpu::ARM9, i, t.mbk[5 + i]); io.mbk_map_range(Cpu::ARM7, i, t.mbk[8 + i]); }
  d.mbk[0][8] = d.mbk[1][8] = t.mbk[11] & 0x00FFFF0F;
  io.wramcnt = static_cast<u8>((t.mbk[11] >> 24) & 3);
  bus.update_nwram();
  if (!(t.app_flags & 1)) io.spi_tsc.dsi_mode = 0;    // DS-compatibility touchscreen unless the title asks for the CODEC

  // Launcher hand-off (dsi_hle_launch) differs from card-mode direct boot at
  // the places marked `hle` below; leftovers titles re-initialise anyway
  // (NDMA, SD/SDIO host, DS Wi-Fi state, AES slots 0/2, loader jump code) aren't reproduced.
  const bool hle = dsi_hle_launch;

  // ---- main RAM: header copies, launcher leftovers ----
  // The launcher leaves only the 0x02FFE000/0x02FFFE00 copies.
  for (u32 i = 0; i < 0x160; i += 4) { const u32 v = rom32(i); if (!hle) w32(0x02FFFA80 + i, v); w32(0x02FFFE00 + i, v); }
  for (u32 i = 0; i < 0x1000; i += 4) { const u32 v = rom32(i); if (!hle) w32(0x02FFC000 + i, v); w32(0x02FFE000 + i, v); }
  if (dsi_boot_blobs.size() == 0x154) {
    const u8* b = dsi_boot_blobs.data();
    for (u32 i = 0; i < 0x128; i += 4) w32(0x02000400 + i, rd32(b + i));            // TWLCFG user data, bytes 0x88..
    for (u32 i = 0; i < 0x14; i += 4) w32(0x02000600 + i, rd32(b + 0x128 + i));    // HWINFO_N
    for (u32 i = 0; i < 0x18; i += 4) w32(0x02FFFD68 + i, rd32(b + 0x13C + i));    // HWINFO_S
    if (hle) {
      // The launcher clears two stretches of TWLCFG; the second held the last-launched title ID.
      for (u32 a = 0x02000407; a <= 0x0200040B; ++a) w8(a, 0);
      for (u32 a = 0x02000420; a <= 0x0200042F; ++a) w8(a, 0);
    }
  }
  // Wi-Fi board words, from the firmware header's board byte (0x1FD).
  const u8 board = firmware.size() > 0x1FD ? firmware[0x1FD] : 0;
  w8(0x020005E0, board);
  if (board == 0x01) { w16(0x020005E2, 0xB57E); w32(0x020005E4, 0x00500400); w32(0x020005E8, 0x00500000); w32(0x020005EC, 0x0002E000); }
  else               { w16(0x020005E2, 0x5BCA); w32(0x020005E4, 0x00520000); w32(0x020005E8, 0x00520000); w32(0x020005EC, 0x00020000); }
  if (!hle) w32(0x02FFFC00, cart->chip_id());       // the launcher leaves no card, so no chip ID
  // Boot indicator: 1 = from the card, 3 = handed over by the DSi launcher.
  w16(0x02FFFC40, hle ? 0x0003 : 0x0001);
  w8(0x02FFFDFA, hle ? 0x81 : 0x80);                 // BPTWL boot flag | 0x80
  w8(0x02FFFDFB, 0x01);

  // ---- the binaries ----
  u32 arm9_start = 0;
  if (h.arm9_rom_offset >= 0x4000 && h.arm9_rom_offset < 0x8000) {
    u8 secure[0x800];
    cart->decrypt_secure_area(secure);
    for (u32 i = 0; i < 0x800; i += 4) w32(h.arm9_ram_address + i, rd32(secure + i));
    arm9_start = 0x800;
  }
  for (u32 i = arm9_start; i < h.arm9_size; i += 4) w32(h.arm9_ram_address + i, rom32(h.arm9_rom_offset + i));
  for (u32 i = 0; i < h.arm7_size; i += 4) bus.dma_write32(Cpu::ARM7, h.arm7_ram_address + i, rom32(h.arm7_rom_offset + i));
  if (h.reserved2 & 1) {                             // DSi crypto flags bit 0: the ARM9i/ARM7i binaries exist
    for (u32 i = 0; i < t.arm9i_size; i += 4) w32(t.arm9i_ram_address + i, rom32(t.arm9i_rom_offset + i));
    for (u32 i = 0; i < t.arm7i_size; i += 4) bus.dma_write32(Cpu::ARM7, t.arm7i_ram_address + i, rom32(t.arm7i_rom_offset + i));
    if (h.reserved2 & 2) {                           // bit 1: modcrypted
      decrypt_modcrypt_area(*this, t.modcrypt1_offset, t.modcrypt1_size, t.arm9_hash, hle);
      decrypt_modcrypt_area(*this, t.modcrypt2_offset, t.modcrypt2_size, t.arm7_hash, hle);
    }
  }
  io.arm7_bios_prot = 0x20;

  // ---- what the firmware leaves for a DSi (melonDS FirmwareMem::SetupDirectBoot) ----
  if (firmware.size() >= 0x20000) {   // the DSi firmware image is 128 KB
    if (hle) {
      // The launcher writes the whole MAC, and its own channel mask (1, 7, 13).
      for (u32 i = 0; i < 6; ++i) w8(0x02FFFCF4 + i, firmware[0x36 + i]);
      w16(0x02FFFCFA, 0x1041);
    } else {
      // melonDS writes all three MAC halves to the same address (no stride), so only the last two bytes stick.
      for (u32 i = 0; i < 6; i += 2) w16(0x02FFFCF4, static_cast<u16>(firmware[0x36 + i] | (firmware[0x37 + i] << 8)));
      w16(0x02FFFCFA, static_cast<u16>(firmware[0x3C] | (firmware[0x3D] << 8)));   // enabled channels
    }
    const u32 u0 = static_cast<u32>(firmware[0x20] | (firmware[0x21] << 8)) << 3, u1 = u0 + 0x100;
    auto counter = [&](u32 off) { return (off + 0x72 <= firmware.size()) ? (firmware[off + 0x70] | (firmware[off + 0x71] << 8)) & 0x7F : -1; };
    const int c0 = counter(u0), c1 = counter(u1);
    const u32 sel = (c1 > c0 && ((c1 - c0) & 0x7F) == 1) ? u1 : u0;
    for (u32 i = 0; i < 0x70; i += 4) w32(0x02FFFC80 + i, rd32(firmware.data() + sel + i));
  }

  d.cart_insert_delay = 0x1988; d.cart_poweroff_delay = 0x264C;
  d.scfg_ext[0] = 0x8307F100;
  if (!hle) {
    d.scfg_mc = 0x0018;
    spu.write_sndexcnt(0x8008, 0xFFFF);
    d.scfg_ext[1] = 0x93FBFB06;
  } else {
    d.scfg_mc = 0x0001;                              // slot empty, powered off
    spu.write_sndexcnt(0x800F, 0xFFFF);
    d.scfg_ext[1] = (0x93FBFB06 | t.scfg_ext7) & 0x7FFFFFFF;   // header's ARM7 SCFG_EXT bits over the launcher's base, then locked
    d.scfg_bios = 0x0501;                            // bit 10: the console ID is hidden
    d.scfg_clock9 = 0x0084;                          // launcher hands over at 67 MHz, DSP/camera clocks off
    bus.set_clock9_shift(1);
  }

  // CP15, in melonDS's order (control first, so the DTCM window is placed
  // twice; only the end state matters).
  auto cp = [&](u32 crn, u32 crm, u32 opc2, u32 v) { cp15_write(*arm9, 0, crn, crm, opc2, v); };
  cp(1, 0, 0, 0x00056078);
  cp(2, 0, 0, 0x0000004A); cp(2, 0, 1, 0x0000004A); cp(3, 0, 0, 0x0000000A);
  cp(5, 0, 2, 0x15111011); cp(5, 0, 3, 0x05101011);
  cp(6, 0, 0, 0x04000033); cp(6, 1, 0, 0x02000031); cp(6, 2, 0, 0x00000000); cp(6, 3, 0, 0x08000033);
  cp(6, 4, 0, 0x0E00001B); cp(6, 5, 0, 0x00000000); cp(6, 6, 0, 0xFFFF001D); cp(6, 7, 0, 0x02FFC01B);
  cp(9, 1, 0, 0x0E00000A); cp(9, 1, 1, 0x00000020);
  bus.update_vram_timings();

  if (!hle) {
    // Register state as for a DS direct boot (melonDS NDS::SetupDirectBoot).
    arm9->set_cpsr(0xD3); arm7->set_cpsr(0xD3);
    arm9->hot.regs[12] = h.arm9_entry; arm9->hot.regs[13] = 0x03002F7C; arm9->hot.regs[14] = h.arm9_entry;
    arm9->bank_r13[0] = 0x03003FC0; arm9->bank_r13[2] = 0x03003F80;
    arm7->hot.regs[12] = h.arm7_entry; arm7->hot.regs[13] = 0x0380FD80; arm7->hot.regs[14] = h.arm7_entry;
    arm7->bank_r13[0] = 0x0380FFC0; arm7->bank_r13[2] = 0x0380FF80;
  } else {
    // Launcher's loader jumps from System mode with IRQs masked, on its own
    // stacks (bank order USR/SYS, FIQ, IRQ, SVC, ABT, UND).
    arm9->set_cpsr(0x9F); arm7->set_cpsr(0x9F);
    arm9->hot.regs[13] = arm9->bank_r13[0] = 0x0E003F80; arm9->bank_r13[2] = 0x0E003F7C; arm9->bank_r13[3] = 0x0E003FC0;
    arm9->hot.regs[14] = h.arm9_entry;
    arm7->hot.regs[13] = arm7->bank_r13[0] = 0x03FFFF80; arm7->bank_r13[2] = 0x0380FF7C; arm7->bank_r13[3] = 0x0380FFC0;
    arm7->hot.regs[14] = h.arm7_entry;

    // Launcher's mount table at the header's parameter block address
    // (0x1D4): five 0x54-byte entries, then the title's image path at +0x3C0.
    auto w32_7 = [&](u32 a, u32 v) { bus.dma_write32(Cpu::ARM7, a, v); };
    auto w8_7  = [&](u32 a, u8 v)  { bus.dma_write8(Cpu::ARM7, a, v); };
    const u32 tbl = t.param_block_address;
    // Either ARM7 WRAM or the ARM7's NWRAM window from the header's MBK map, applied above.
    if (tbl >= 0x03000000 && tbl + 0x500 <= 0x0380FC00) {
      for (u32 z = 0; z < 0x500; z += 4) w32_7(tbl + z, 0);
      auto put_str = [&](u32 addr, const char* str) { for (const char* c = str; *c; ++c) w8_7(addr++, static_cast<u8>(*c)); w8_7(addr, 0); };
      char title[64];
      std::snprintf(title, sizeof title, "nand:/title/%08x/%08x", t.title_id_hi, t.title_id_lo);
      char pub[96];
      std::snprintf(pub, sizeof pub, "%s/data/public.sav", title);
      struct Mount { u32 hdr; const char* name; const char* path; };
      std::vector<Mount> entries = {
        {0x00008141, "nand",    "/"},
        {0x0000A142, "nand2",   "/"},
        {0x00041144, "shared1", "nand:/shared1"},
        {0x00063146, "photo",   "nand2:/photo"},
        {0x00060948, "dataPub", pub},
      };
      // A title with SD card access (header bit 3) gets the card's root too, regardless of slot state.
      if (t.access_control & (1u << 3)) entries.push_back({0x00060049, "sdmc", "/"});
      u32 o = tbl;
      for (const auto& e : entries) { w32_7(o, e.hdr); put_str(o + 4, e.name); put_str(o + 20, e.path); o += 0x54; }
      char app[96];
      std::snprintf(app, sizeof app, "%s/content/%08x.app", title, dsi_hle_content_id);
      put_str(tbl + 0x3C0, app);
    } else {
      std::fprintf(stderr, "dsi: parameter block address %08x is outside the ARM7's RAM; no mount table\n", tbl);
    }
    // Locked SCFG_EXT7 and two flag bytes at the top of ARM7 WRAM.
    w32_7(0x0380FFC4, d.scfg_ext[1]);
    w8_7(0x0380FFC8, 0x44);
    w8_7(0x0380FFC9, 0xF8);

    // Devices the launcher left configured.
    io.dsi.bptwl_regs[0x12] = 0x03;
    io.dsi.bptwl_regs[0x70] = 0x01;                  // the boot flag (0x02FFFDFA above)
    io.dsi.gpio_ie = 0x40;
    io.spi_pm.regs[0] = 0x0C;
    io.rtc.status1 &= 0x7F;                          // the power-lost flag, cleared when the launcher read it
  }
  arm9->jump(h.arm9_entry, true);
  arm7->jump(h.arm7_entry, true);
  arm9->hot.cycle_budget = 0; arm7->hot.cycle_budget = 0;
  // melonDS's pending pipeline-fill cycles at reset/direct-boot JumpTo: the
  // ARM9 starts 34 bus cycles late and the ARM7 15.
  arm9->boot_stall = 136; arm7->boot_stall = 15;

  io.exmemcnt = hle ? 0xE88C : 0xE880; bus.update_gba_slot_timings();
  io.cpu_io[0].postflg = 1; io.cpu_io[1].postflg = 1;
  io.powcnt1 = 0x820F; gpu.set_powcnt(0x820F);
  io.powcnt2 = 0x0001; spu.set_powcnt2(0x0001);
  spu.write(0x04000504, 16, 0x200);
  if (hle) { io.rcnt = 0x0005; return; }             // no card: nothing of the slot is set up
  io.cart.romctrl |= 1u << 29;
  io.cart.auxspicnt = 0x8000;   // melonDS NDS::SetupDirectBoot: WriteSPICnt(0x8000) on both slots (the DS path leaves it to the game)
  io.rcnt = 0x8000;             // melonDS NDS::SetupDirectBoot ("checkme"); the DS path leaves 0
  cart->setup_direct_boot();
}

} // namespace ds
