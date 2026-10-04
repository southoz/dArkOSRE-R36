// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"
#include "core/cpu/cpu.h"
#include "core/mem/bus.h"
#include "core/sched/scheduler.h"
#include "core/gpu/gpu.h"
#include "core/gpu/gpu3d.h"
#include "core/spu/spu.h"
#include "core/io/io.h"
#include "core/io/dsi_sd_card.h"
#include "core/dma/dma.h"
#include "core/dma/ndma.h"
#include "core/cart/cart.h"
#include "core/cart/zip.h"
#include "core/cheat/ar_engine.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>
#include "core/bios/freebios.h"

namespace ds {
namespace state { class Writer; class Reader; }

// Per-instruction trace callback: called with r15 already pipeline-adjusted
// (instruction address + 8 / + 4) before the instruction executes.
using TraceFn = void (*)(CpuContext& cpu, u32 instr, void* user);

struct NDS {
  NDS();
  ~NDS();

  void reset();
  // Loads the boot images. An empty/missing path is substituted: BIOS by
  // FreeBIOS, firmware by one generated from `user`. A present-but-bad file
  // is an error (`err` says which); substitution is reported via
  // bios_native/firmware_synthetic below (substitutes only support direct boot).
  bool load_bios(const std::string& bios9, const std::string& bios7, const std::string& firmware,
                 const bios::UserSettings& user = {}, std::string* err = nullptr);
  bool bios_native = false;          // both BIOS images came from dumps
  // DSi BIOS pair (64 KB each, dumps only). Loaded separately: a DS game doesn't need them; both or neither.
  bool load_dsi_bios(const std::string& bios9i, const std::string& bios7i, std::string* err = nullptr);
  bool bios_native_dsi = false;      // the DSi pair is loaded
  // Console type (core/nds_dsi.cpp for the DSi machine). Set by the frontend
  // before reset(); every DS-mode path is unchanged when false.
  bool dsi = false;
  void set_dsi(bool on);             // before reset(); needs load_dsi_bios for true
  // What the launcher leaves in main RAM for a title: TWLCFG (0x128 @
  // 0x02000400), HWINFO_N (0x14 @ 0x02000600), HWINFO_S (0x18 @ 0x02FFFD68).
  // Absent, the areas stay zero.
  bool load_dsi_boot_blobs(const std::string& path, std::string* err = nullptr);
  std::vector<u8> dsi_boot_blobs;    // 0x154 bytes when loaded, else empty
  // DSi NAND image; backs the eMMC on SD/MMC port 1 and seeds AES key slots
  // 1/3 from the console ID. Loaded before reset(), like the BIOS pair.
  bool load_dsi_nand(const std::string& path, std::string* err = nullptr, bool write_through = false);   // see NandImage
  io::NandImage dsi_nand;
  // SD card in the DSi's slot, from a host folder (io/dsi_sd_card.h). Opened
  // before reset() onto SD/MMC port 0; changes sync back via dsi_sd.sync().
  io::SdCard dsi_sd;
  bool firmware_synthetic = false;   // the firmware was generated, not dumped (set by load_bios)
  bool can_boot_firmware() const { return bios_native && !firmware_synthetic; }
  bool load_rom(const std::string& path);
  // A ROM already in memory (frontend's built-in loader cart); same identity
  // hash as the path form, so saves/states key the same way.
  bool load_rom_image(std::vector<u8> image);
  bool load_rom_source(std::unique_ptr<cart::RomSource> src);
  std::string rom_zip_entry;   // which zip entry a zipped ROM came from, empty for a loose .nds; reporting only
  // Loading a zip: extraction target when the archive's own directory can't
  // be written (cart/zip_cache.h), and a progress callback. Set before load_rom.
  std::string rom_cache_dir;
  u64 rom_cache_max_bytes = 0;                 // 0: no limit
  cart::ZipProgress rom_progress = nullptr;
  void* rom_progress_user = nullptr;
  std::atomic<bool>* rom_cancel = nullptr;     // set from another thread to abandon the extraction
  std::string rom_cache_path;   // extracted image the current cart was mapped from, empty otherwise
  void normalise_touch_calibration();   // see nds.cpp; called by load_bios
  void normalise_boot_mode();           // see nds.cpp; called by load_bios
  // Which of the firmware's three Wi-Fi slots holds DSperate's access point, -1 if none.
  int firmware_ap_slot = -1;

  // Console settings as the firmware holds them (dump's pages, or what
  // `user` built a generated firmware from). Reading gives back what a game
  // would see.
  //
  // Written a field at a time: a dump's nickname may hold UTF-16 characters
  // these one-byte-per-unit strings can't represent, and rewriting every
  // field to change one would turn an untouched name into mojibake.
  enum class UserField : u8 { Nickname, Message, Colour, BirthdayMonth, BirthdayDay, Language };
  bool read_user_settings(bios::UserSettings& out) const;
  u32  user_settings_offset() const;   // where the two settings copies live, or 0 if none
  bool write_user_settings(UserField field, const bios::UserSettings& in);
  void setup_direct_boot();          // skip the firmware: load the ROM's binaries and jump to them
  // Gives this console its own Wi-Fi MAC (firmware OUI + `suffix` as the low
  // three bytes, checksum fixed), so local-wireless titles can tell
  // instances of the same dump apart. Call before boot; in-memory only.
  void set_wifi_mac_suffix(u32 suffix);
  void setup_direct_boot_dsi();      // the DSi machine's version (nds_dsi.cpp)
  // DSiWare without the launcher: stages what the DSi Launcher leaves a
  // title at its entry point (nds_dsi.cpp) instead of card-mode direct boot.
  // Starts as if launched from the menu: nand:/ mounted, image read back
  // from nand:/title/.../content/<dsi_hle_content_id>.app. Set before setup_direct_boot.
  bool dsi_hle_launch = false;
  u32  dsi_hle_content_id = 0;
  // What the launcher hand-off needs beyond the BIOS pair, synthesised where
  // missing (io/dsi_nand_synth.h): an in-memory NAND if none loaded, console
  // settings from `user` if no --dsi-boot data, firmware settings if
  // generated. Call after load_rom and load_dsi_bios, before setup_direct_boot.
  // `report` gets one line per substitution.
  bool prepare_dsi_hle(const bios::UserSettings& user, std::string* err, std::vector<std::string>* report = nullptr);
  // Installs a DSiWare title on the loaded NAND for the hand-off
  // (io/dsi_nand_launch.h): its .app, content ID, and (unless supplied) its
  // console data from TWLCFG/HWINFO. Marks the NAND's state base so later
  // saves are carried by a state. Call after load_dsi_nand/load_dsi_bios, before prepare_dsi_hle.
  bool load_dsi_nand_title(u32 title_lo, std::string* err);
  bool dsi_nand_synthetic = false;   // dsi_nand was built by prepare_dsi_hle, not loaded
  // User's /sys/TWLFontTable.dat for a synthesised NAND; empty uses DSperate's own font.
  std::string dsi_font_path;
  // The built-in font's signature check (DSi BIOS SWI 22h) is answered with
  // its SHA-1 when the passed signature is the built-in font's marker.
  // Called by the interpreter/recompiler fallback for every SWI while set; true if handled.
  bool dsi_font_hle = false;
  u8   dsi_font_digest[20] = {};
  // Set by the frontend when its loader cart sits in the slot of a DSi
  // booted to its launcher. The launcher fades to white at several points,
  // so only a card launch's header hash (SWI 27h over the first 0x160
  // bytes) sets dsi_loader_launched, before the launcher refuses the
  // unwhitelisted card.
  bool dsi_loader_watch = false;
  bool dsi_loader_launched = false;
  bool dsi_hle_swi(CpuContext& cpu, u32 number);
  // Boots the DSi from its NAND (boot2 -> launcher) instead of a direct
  // boot; needs a NAND image.
  bool boot_dsi_nand();
  bool dsi_nand_boot = false;        // set before reset() to take that path
  // DSi autoload hand-off (GBATEK "DSi Autoload"): a "TLNC" block at
  // 0x02000300 plus the BPTWL boot flag, making the launcher start the
  // installed title instead of the menu.
  void dsi_autoload(u32 title_lo, u32 title_hi = 0x00030004);
  bool dsi_soft_reset_pending = false;   // BPTWL soft reset (reg 0x11 <- 1); scheduler calls dsi_soft_reset() as the ARM7's run returns
  void dsi_soft_reset();
  // Guest started a DSP program (unemulated): it will hang waiting for
  // replies. Sticky until reset()/soft reset; frontends warn once per start.
  bool dsi_dsp_started = false;

  // Firmware settings persistence: the firmware writes its own flash over
  // SPI, but changed 256-byte pages go to a sidecar file re-applied over the
  // pristine dump at load, rather than into firmware.bin itself. Deleting
  // the sidecar restores the dump's original settings.
  //
  // Both take the sidecar's path and report the reason on failure. save_
  // returns true and writes nothing when no page has changed.
  static constexpr u32 FW_PAGE = 256;   // the flash's page, and the sidecar's granularity
  bool load_firmware_override(const std::string& path, std::string& err);
  bool save_firmware_override(const std::string& path, std::string& err);
  bool firmware_override_dirty() const { return fw_dirty_pages > 0; }
  void firmware_written(u32 offset);   // called from the SPI page-write path
  void run_frame();
  // run_frame() in pieces: runs up to `cycles` of the current frame, true
  // when it completed. Lets local wireless answer a peer's CMD within a
  // slice rather than after the frame's sleep.
  bool run_frame_slice(u64 cycles);

  // Whole-machine snapshots, taken only where run_frame() returns.
  // save_state does not disturb the run; a failed load_state leaves the
  // machine unusable (reset it). `err` gets the reason on failure.
  bool save_state(state::Writer& w, std::string& err);
  bool load_state(state::Reader& r, std::string& err);
  // Set by a DSi load_state without the state's SD card (folder changed, or
  // this session has none); empty otherwise. Frontends surface it.
  std::string sd_card_note;

  CpuContext& cpu(Cpu which) { return which == Cpu::ARM9 ? *arm9 : *arm7; }

  // Execution engines, swappable per CPU (interpreter / JIT).
  RunFn run_arm9;
  RunFn run_arm7;

  // Heap-allocated: each owns a 16 MiB page table mapping and the JIT wants a stable address.
  std::unique_ptr<CpuContext> arm9, arm7;

  // Loaded images (declared before the subsystems, which read them in
  // reset()). The ROM image itself lives only in the Cart; what survives
  // here is the identity a save state checks against.
  u64 rom_id = 0;
  std::vector<u8> firmware;
  // Which 256-byte pages of `firmware` differ from the dump on disk, set by
  // the SPI write path and by a loaded override.
  std::vector<u8> fw_page_dirty;
  u32 fw_dirty_pages = 0;
  u64 firmware_id = 0;           // identity of the pristine dump; an override names it
  // Identity of the two BIOS images in bus.bios9/bios7. A save state usually
  // restores the ARM7 mid-BIOS-routine, but the BIOS isn't part of the
  // state, so the header carries this to refuse a load against a different pair.
  u64 bios_id = 0;

  mem::Bus   bus;
  Scheduler  sched;
  gpu::Gpu   gpu;
  gpu::Gpu3D gpu3d;
  spu::Spu   spu;
  io::Io     io;
  dma::Dma   dma;
  dma::Ndma  ndma;                 // DSi only; idle on a DS
  std::unique_ptr<cart::Cart> cart;

  // Action Replay codes, run from the ARM7's VBlank IRQ when enabled. Not
  // part of a save state: which cheats are on is the frontend's business.
  cheat::Engine cheats;

  u64  frame_count = 0;
  bool frame_ready = false;
  bool frame_in_slices = false;   // run_frame_slice has begun a frame not yet complete
  // ARM7 pulled the power line down (PMIC reg 0 bit 6): console switched
  // itself off. Cleared by reset(); what it means is the frontend's call (SDL saves settings and reboots).
  bool power_off = false;
  // Guest asked for something only the DSi Launcher could do (soft reset,
  // app jump) on a synthesised NAND with no launcher to reset into. ARM7
  // stays halted; frontend ends the session. Cleared by reset().
  bool exit_requested = false;

  TraceFn trace = nullptr;
  void*   trace_user = nullptr;
};

} // namespace ds
