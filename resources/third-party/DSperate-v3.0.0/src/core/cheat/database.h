// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"
#include "core/cheat/ar_engine.h"

#include <string>
#include <vector>

namespace ds::cheat {

// The R4 `usrcheat.dat` cheat database format.
//
//   header  "R4 CheatCode\0\1\0\0", a 0x3C-byte description, then the entry
//           list at 0x100: {game code, ROM header checksum, offset, 0} per
//           entry, terminated by a zero game code.
//   entry   the game's name, a flags word (low 24 bits = item count), eight
//           master-code words, then the items.
//   item    a flags word (low 24 bits length, bit 24 enabled/exclusive, bit
//           28 category), name and description as NUL-terminated strings,
//           padding to a word, then a word count + code words (or, for a
//           category, nothing -- the next `length` codes belong to it).
//
// Read whole and parsed from memory; entries scattered by absolute offset.
struct Group {
  std::string name, description;
  bool exclusive = false;   // only one code in the group may be enabled
};

// `codes` is flat; group -1 means outside any category.
struct GameCheats {
  u32 game_code = 0;
  u32 checksum = 0;        // of the ROM header; distinguishes revisions/regions
  std::string name;
  std::vector<Group> groups;
  std::vector<Code> codes;
};

u32 header_checksum(const u8* header, size_t n = 512);

// False with `err` set if the database or ROM cannot be read; false with
// `err` empty if the game simply is not in the database.
bool load_for_rom(const std::string& db_path, const std::string& rom_path, GameCheats& out, std::string& err);

// Same, from a header already in hand. Use once the cart is loaded, since
// the ROM path may name a zip.
bool load_for_header(const std::string& db_path, const u8 header[512], GameCheats& out, std::string& err);

class Database {
public:
  bool open(const std::string& path, std::string& err);

  const std::string& name() const { return name_; }
  bool has(u32 game_code) const;
  size_t game_count() const { return index_.size(); }

  // Usually one entry per game code, one per checksum for several revisions.
  // Entries that fail to parse are left out and named in `err`.
  std::vector<GameCheats> entries_for(u32 game_code, std::string& err) const;
  // The entry whose checksum matches, else the first one.
  bool best_entry(u32 game_code, u32 checksum, GameCheats& out, std::string& err) const;

  std::vector<u32> game_codes() const;

private:
  struct Entry { u32 game_code, checksum, offset; };
  std::vector<u8> file_;
  std::string name_;
  std::vector<Entry> index_;
  bool parse_entry(const Entry& e, GameCheats& out, std::string& err) const;
};

} // namespace ds::cheat
