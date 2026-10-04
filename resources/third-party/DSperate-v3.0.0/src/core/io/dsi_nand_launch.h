// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// DSiWare already installed on a NAND dump, started without the DSi Menu: the title's .app is
// read out of the NAND once, put in the slot, and handed over with the real NAND behind it.
//
// A "shortcut" is how a game list reaches one: a small file named "<banner title>.dspr.nds" in
// the games folder holding a DSPR marker, the title ID, and the identity of its NAND. The
// suffix marks files DSperate made, so they're the only ones it ever removes.
#pragma once
#include "core/types.h"

#include <string>
#include <vector>

namespace ds::io {

class NandImage;

struct NandTitle {
  u32 title_lo = 0;          // 0x4B533345
  std::string code;          // "KS3E"
  std::string name;          // the banner's title (English where there is one), UTF-8, one line
  u32 content_id = 0;        // its CONTENT/xxxxxxxx.APP
};

// The DSiWare titles (00030004) installed on `nand`, with their banner names.
std::vector<NandTitle> nand_dsiware_titles(NandImage& nand, const u8* bios7i);

// A title's .app, whole.
bool nand_read_title_app(NandImage& nand, const u8* bios7i, u32 title_lo, std::vector<u8>& srl, u32& content_id, std::string* err);

// What a DSi direct boot copies into main RAM from the NAND (0x154 bytes): TWLCFG's
// 0x88..0x1AF, HWINFO_N's 0x88..0x9B, HWINFO_S's 0x88..0x9F.
bool nand_boot_blobs(NandImage& nand, const u8* bios7i, std::vector<u8>& out, std::string* err);

// ---- shortcuts ----------------------------------------------------------------

inline constexpr const char* kShortcutSuffix = ".dspr.nds";

struct NandShortcut {
  u32 title_lo = 0, title_hi = 0x00030004;
  u8  cid[16] = {};
  u64 console_id = 0;
  // Whether it names a title on this NAND (the same eMMC CID and console ID).
  bool from(const NandImage& nand) const;
};

// First line of a banner title (UTF-16LE, 0x80 chars max), folded to plain ASCII for a
// shortcut's file name: accents dropped, typographic/fullwidth punctuation folded, anything
// without an ASCII spelling left out.
std::string banner_title_ascii(const u8* utf16le);

// Whether `path` ends in .dspr.nds (any case).
bool is_shortcut_name(const std::string& path);
// Reads one; false when the file is not a shortcut.
bool read_shortcut(const std::string& path, NandShortcut& out);

struct ShortcutSync {
  int written = 0, removed = 0;
  std::vector<std::string> notes;
};

// `enabled`: a shortcut in `dir` for every title on `nand`, none for titles it lacks; else all
// .dspr.nds in `dir` are removed. Files named from the banner (filesystem-safe); name clashes
// get game codes appended.
ShortcutSync sync_shortcuts(const std::string& dir, NandImage* nand, const u8* bios7i, bool enabled);

}  // namespace ds::io
