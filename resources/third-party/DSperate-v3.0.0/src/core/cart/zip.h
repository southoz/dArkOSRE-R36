// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Reading a ROM out of a zip, as a RomSource (rom_source.h): a stored entry
// is mapped where it lies, a deflated one is inflated once to a file beside
// the archive (zip_cache.h) and mapped.
//
// Only what a ROM archive needs: stored and deflated entries, no zip64, no
// encryption, no writing. DEFLATE core is vendored miniz (miniz/); container
// parsing is ours, with every offset bounds-checked.
#pragma once

#include <string>
#include <vector>

#include "core/types.h"

namespace ds::cart {

// "PK\x03\x04". Sniffed rather than trusting the extension.
bool is_zip(const u8* data, size_t size);

struct ZipEntry {
  std::string name;
  u16  method = 0;          // 0 stored, 8 deflated
  u64  data_off = 0;        // payload offset in the archive
  u64  csize = 0, usize = 0;
  u32  crc32 = 0;           // of the uncompressed bytes, as declared
  // A DSiWare title wrapped in a CIA container. Not a DS header, so the cart
  // path must not map it directly -- io/dsi_title_install.h unwraps it first.
  bool cia = false;
  bool stored() const { return method == 0; }
};

// Picks the image entry out of `zip`. False with a reason in `err`.
// Extensions taken to hold an image: .nds, .dsi, .srl (bare ROM/SRL), .cia
// (SRL in a CIA container). A bare entry is always preferred over a .cia.
bool find_rom(const u8* zip, size_t size, ZipEntry& entry, std::string& err);

// The first `n` uncompressed bytes of `entry` (fewer if shorter). CRC not
// checked -- this is a peek; only inflate_entry vouches for a complete stream.
size_t peek_entry(const u8* zip, size_t size, const ZipEntry& entry, u8* out, size_t n);

// Produces the entry's uncompressed bytes through `sink` (false aborts),
// calling `progress` every megabyte or so. Verifies CRC32 at the end.
using ZipSink = bool (*)(void* user, const u8* data, size_t n);
using ZipProgress = void (*)(void* user, u64 done, u64 total);
bool inflate_entry(const u8* zip, size_t size, const ZipEntry& entry, ZipSink sink, void* sink_user,
                   ZipProgress progress, void* progress_user, std::string& err);

u32 crc32_update(u32 crc, const u8* data, size_t n);

// Extracts the image from `zip` into `out` (find_rom + inflate_entry into a
// vector). The in-memory path, for a filesystem that cannot map, and tests.
//
// With several candidate entries: game codes listed in save_list.inc beat
// unlisted ones, then highest header revision wins, then archive order. A
// .cia is only picked when there's no bare entry, since its header isn't a DS
// one and can't join that comparison. Listed does not mean "good dump".
//
// False on any failure, with a reason in `err`. A CIA is refused unless
// `allow_cia`; io/dsi_title_install.h is the caller that unwraps one.
bool extract_rom(const u8* zip, size_t size, std::vector<u8>& out, std::string& err,
                 std::string* chosen = nullptr, bool allow_cia = false, bool* was_cia = nullptr);

std::string cia_refusal(const std::string& name);

} // namespace ds::cart
