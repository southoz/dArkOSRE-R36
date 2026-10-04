// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// DSiWare without a NAND dump. NDS::dsi_hle_launch starts a title as the DSi Launcher would,
// and the title then mounts nand:/. This builds that NAND in memory: a formatted, encrypted
// filesystem under a made-up console, holding the title and settings files from [user] config.
//
// Nothing Nintendo-owned is on it: no launcher, no system titles, no certificates. System font
// is DSperate's own (builtin_dsi_font) unless the user supplies the console's.
#pragma once
#include "core/types.h"
#include "core/bios/freebios.h"

#include <string>
#include <vector>

namespace ds::io {

class NandImage;

// Console region as a DSi title sees it: HWINFO_S's region/language mask/launcher title ID,
// TWLCFG's country/language.
struct DsiRegion {
  u8   region = 1;        // 0 JPN, 1 USA, 2 EUR, 3 AUS, 4 CHN, 5 KOR
  u8   country = 0x31;    // TWLCFG country code (USA)
  u8   language = 1;      // 0 ja, 1 en, 2 fr, 3 de, 4 it, 5 es, 6 zh, 7 ko
  u8   language_mask = 0x26;
  char letter = 'E';      // the launcher's title ID ends in it (HNAE)
};

// A region the title's header allows (0x1B0), preferring one whose languages include the
// user's; language is the user's when the region has it, else the region's own.
DsiRegion dsi_region_for(u32 header_region_flags, u8 user_language);

// The console files, whole (0x4000 bytes, 0xFF padded, as on a NAND).
struct DsiConsoleFiles {
  std::vector<u8> twlcfg;     // /shared1/TWLCFG0.dat and TWLCFG1.dat
  std::vector<u8> hwinfo_n;   // /sys/HWINFO_N.dat
  std::vector<u8> hwinfo_s;   // /sys/HWINFO_S.dat (unsigned: only the launcher checks)
  // /sys/TWLFontTable.dat when non-empty. Titles check its RSA signature, so the built-in one
  // needs NDS::dsi_hle_swi.
  std::vector<u8> font;
  // What a DSi direct boot copies into main RAM from them (0x154 bytes).
  std::vector<u8> boot_blobs() const;
};
DsiConsoleFiles make_dsi_console_files(const bios::UserSettings& user, const DsiRegion& region, u64 console_id);

// DSperate's own TWLFontTable.dat for a console of `region` (DsiRegion::region). China (4) and
// Korea (5) have tables of their own (resource entries at 3-5/6-8); other regions get the
// normal three. First 0x80 bytes are a marker where Nintendo's RSA signature would be, checked
// through DSi BIOS SWI 22h, which NDS::dsi_hle_swi answers for the marker.
std::vector<u8> builtin_dsi_font(u8 region = 1);
bool is_builtin_font_signature(const u8 sig[0x80]);
// Which console a TWLFontTable.dat is for: 0 normal, 4 China, 5 Korea, -1 neither.
int font_table_region(const std::vector<u8>& table);

// The made-up console the synthesised NAND belongs to.
extern const u64 kSynthConsoleId;
extern const u8  kSynthEmmcCid[16];

// Replaces `nand` with an in-memory NAND holding the console files and the DSiWare title `srl`
// (content 00000000, empty saves sized from its header). `bios7i` is the DSi ARM7 BIOS (64 KB).
bool build_synthetic_nand(NandImage& nand, const u8* bios7i, const std::vector<u8>& srl,
                          const DsiConsoleFiles& files, std::string* err);

}  // namespace ds::io
