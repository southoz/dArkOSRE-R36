// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// One DSiWare title put into the session's NAND: at most one per boot, only when its title ID
// is not already installed. Writes a ticket, the title/content directories, SDK-formatted save
// files, title.tmd and the .app -- into NandImage's memory, never the dump.
#pragma once
#include "core/types.h"

#include <functional>
#include <string>
#include <vector>

namespace ds::io {

class NandImage;
class FatVolume;

// What each DSiWare title on the NAND occupies (files under its title directory, cluster-rounded).
struct DsiWareUsage { std::string id; std::string code; u64 bytes = 0; };   // id "4b533345", code "KS3E"
std::vector<DsiWareUsage> dsiware_usage(const FatVolume& vol);

// Takes a DSiWare title out of the session (title directory + ticket); dump untouched, so it's
// back next boot.
bool hide_dsiware_title(FatVolume& vol, const std::string& id, std::string* err);

// Hides every installed DSiWare title. Returns how many; call before nand_install_title/nand_import.
int nand_hide_installed_dsiware(NandImage& nand, const u8* bios7i, std::string* err);

// A DSiWare SRL from a .nds/.app/.srl, a zip holding one, or a CIA's one content if it is not
// title-key encrypted (encrypted ones need a console key DSperate lacks, and are refused).
// False, with reason, for anything not DSiWare. `embedded_tmd` gets the CIA's DSi-signed TMD
// when it carries one, else left empty.
bool read_dsiware(const std::string& path, std::vector<u8>& srl, std::string* err, std::vector<u8>* embedded_tmd = nullptr);

// The launcher checks the TMD's RSA signature at launch, so this needs the title's real
// Nintendo-signed DSi TMD, matching this title and exact SRL (size and SHA-1).
bool check_signed_tmd(const std::vector<u8>& tmd, const std::vector<u8>& srl, std::string* err);

// Where that TMD comes from, in order: embedded in the CIA; `cache_path`; the Nintendo update
// CDN through `fetch` (stored to `cache_path` when it works); `beside_rom`. Empty when none is
// usable; `log` says what was tried. `fetch` may be empty (no network client).
using TmdFetch = std::function<bool(const std::string& url, std::vector<u8>& body)>;
std::string nus_tmd_url(u32 title_lo);
std::vector<u8> find_signed_tmd(const std::vector<u8>& srl, const std::vector<u8>& embedded, const std::string& cache_path,
                                const std::string& beside_rom, const TmdFetch& fetch, std::vector<std::string>& log);

bool nand_has_title(NandImage& nand, const u8* bios7i, u32 title_lo);

// Cheap enough for every file in a game list: .nds/.dsi/.srl by header, .cia by extension alone
// (read_dsiware checks it properly when opened), .zip by the entry it holds.
bool file_is_dsiware(const std::string& path);
bool zip_is_dsiware(const std::string& path);
// Whether a .zip's chosen entry is a .cia (needs read_dsiware to unwrap) rather than a direct image.
bool zip_holds_cia(const std::string& path);
// Content ID an installed title's .app is named by (CONTENT/xxxxxxxx.APP).
bool nand_title_content_id(NandImage& nand, const u8* bios7i, u32 title_lo, u32& content_id);

struct TitleInstall {
  enum class Result { Installed, AlreadyInstalled, Failed } result = Result::Failed;
  u32 title_lo = 0;          // e.g. 0x4B443945 ("KD9E")
  std::string message;       // why it failed, or what was done
  std::vector<std::string> hidden;   // dump titles hidden this session to fit the quota, by game code
};

// `bios7i` required: the ticket is ES-encrypted with a key from it. `signed_tmd` must pass check_signed_tmd.
TitleInstall nand_install_title(NandImage& nand, const u8* bios7i, const std::vector<u8>& srl, const std::vector<u8>& signed_tmd);

// An empty FAT12 volume `len` bytes long. Empty for len 0; exposed for tests.
std::vector<u8> make_dsi_save(u32 len);

}  // namespace ds::io
