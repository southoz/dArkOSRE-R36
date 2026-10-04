// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/cheat/database.h"

#include <cstdio>
#include <cstring>

namespace ds::cheat {
namespace {

constexpr size_t ENTRY_LIST = 0x100;     // where the entry list starts
constexpr size_t NAME_LEN   = 0x3C;      // the database description, after the 16-byte magic
constexpr u32 MAX_CODE_WORDS = 0x100000; // melonDS's sanity bound, and far above any real code
constexpr u32 MAX_CATEGORY   = 0x10000;

// Bounds-checked cursor: every read either succeeds or sets `bad`.
struct Reader {
  const u8* p;
  size_t n, at = 0;
  bool bad = false;

  bool seek(size_t to) { if (to > n) { bad = true; return false; } at = to; return true; }
  size_t left() const { return at <= n ? n - at : 0; }
  u32 u32le() {
    if (left() < 4) { bad = true; return 0; }
    const u32 v = static_cast<u32>(p[at]) | (static_cast<u32>(p[at + 1]) << 8) |
                  (static_cast<u32>(p[at + 2]) << 16) | (static_cast<u32>(p[at + 3]) << 24);
    at += 4;
    return v;
  }
  std::string ntstring() {
    const size_t start = at;
    while (at < n && p[at]) ++at;
    if (at >= n) { bad = true; return {}; }
    std::string s(reinterpret_cast<const char*>(p + start), at - start);
    ++at;   // the terminator
    return s;
  }
  void align4() { if (at & 3) seek((at + 3) & ~size_t{3}); }
};

u32 crc32(const u8* data, size_t n) {
  static u32 table[256];
  static bool built = false;
  if (!built) {
    for (u32 i = 0; i < 256; ++i) {
      u32 c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      table[i] = c;
    }
    built = true;
  }
  u32 c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; ++i) c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

} // namespace

u32 header_checksum(const u8* header, size_t n) { return ~crc32(header, n); }

bool Database::open(const std::string& path, std::string& err) {
  file_.clear();
  index_.clear();
  name_.clear();

  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) { err = "cannot open " + path; return false; }
  std::fseek(f, 0, SEEK_END);
  const long len = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (len <= 0) { std::fclose(f); err = path + " is empty"; return false; }
  file_.resize(static_cast<size_t>(len));
  const bool read_ok = std::fread(file_.data(), 1, file_.size(), f) == file_.size();
  std::fclose(f);
  if (!read_ok) { file_.clear(); err = "cannot read " + path; return false; }

  if (file_.size() < ENTRY_LIST) { err = path + " is too short to be a cheat database"; return false; }
  static const u8 magic[16] = {'R','4',' ','C','h','e','a','t','C','o','d','e', 0, 1, 0, 0};
  if (std::memcmp(file_.data(), magic, sizeof magic) != 0) {
    err = path + " is not a usrcheat database (bad magic)";
    return false;
  }
  const char* desc = reinterpret_cast<const char*>(file_.data() + 16);
  name_.assign(desc, strnlen(desc, NAME_LEN));

  // Entry list: 16 bytes each, ended by a zero game code or the file.
  Reader r{file_.data(), file_.size()};
  r.seek(ENTRY_LIST);
  while (r.left() >= 16) {
    const u32 code = r.u32le(), checksum = r.u32le(), offset = r.u32le();
    r.u32le();                       // reserved
    if (code == 0) break;
    if (offset < ENTRY_LIST || offset >= file_.size()) continue;
    index_.push_back({code, checksum, offset});
  }
  if (index_.empty()) { err = path + " has no usable entries"; return false; }
  return true;
}

bool load_for_rom(const std::string& db_path, const std::string& rom_path, GameCheats& out, std::string& err) {
  u8 header[512];
  FILE* rom = std::fopen(rom_path.c_str(), "rb");
  if (!rom) { err = "cannot open " + rom_path; return false; }
  const bool got = std::fread(header, 1, sizeof header, rom) == sizeof header;
  std::fclose(rom);
  if (!got) { err = rom_path + " is too short to be a DS ROM"; return false; }

  return load_for_header(db_path, header, out, err);
}

bool load_for_header(const std::string& db_path, const u8 header[512], GameCheats& out, std::string& err) {
  u32 game_code = 0;
  for (int i = 3; i >= 0; --i) game_code = (game_code << 8) | header[0x0C + static_cast<size_t>(i)];

  Database db;
  if (!db.open(db_path, err)) return false;
  err.clear();
  return db.best_entry(game_code, header_checksum(header), out, err);
}

bool Database::has(u32 game_code) const {
  for (const Entry& e : index_) if (e.game_code == game_code) return true;
  return false;
}

std::vector<u32> Database::game_codes() const {
  std::vector<u32> out;
  out.reserve(index_.size());
  for (const Entry& e : index_) out.push_back(e.game_code);
  return out;
}

std::vector<GameCheats> Database::entries_for(u32 game_code, std::string& err) const {
  std::vector<GameCheats> out;
  for (const Entry& e : index_) {
    if (e.game_code != game_code) continue;
    GameCheats g;
    std::string one;
    if (parse_entry(e, g, one)) out.push_back(std::move(g));
    else if (err.empty()) err = one;
  }
  return out;
}

bool Database::best_entry(u32 game_code, u32 checksum, GameCheats& out, std::string& err) const {
  std::vector<GameCheats> all = entries_for(game_code, err);
  if (all.empty()) return false;
  for (GameCheats& g : all) {
    if (g.checksum == checksum) { out = std::move(g); return true; }
  }
  // No revision matched; the first entry is the useful fallback.
  out = std::move(all.front());
  return true;
}

bool Database::parse_entry(const Entry& e, GameCheats& out, std::string& err) const {
  Reader r{file_.data(), file_.size()};
  if (!r.seek(e.offset)) { err = "entry offset past the end of the file"; return false; }

  out.game_code = e.game_code;
  out.checksum = e.checksum;
  out.name = r.ntstring();
  r.align4();

  const u32 flags = r.u32le();
  for (int i = 0; i < 8; ++i) r.u32le();   // master codes; undocumented
  if (r.bad) { err = "entry header runs past the end of the file"; return false; }
  const u32 items = flags & 0xFFFFFF;

  // Codes belong to the most recent category until its count is used up.
  int group = -1;
  u32 group_left = 0;
  for (u32 i = 0; i < items; ++i) {
    const u32 item = r.u32le();
    const u32 total = item & 0xFFFFFF;
    const std::string item_name = r.ntstring();
    const std::string item_desc = r.ntstring();
    r.align4();
    if (r.bad) { err = "item " + std::to_string(i) + " runs past the end of the file"; return false; }

    if (item & (1u << 28)) {
      if (total >= MAX_CATEGORY) { err = "category \"" + item_name + "\" has an unreasonable length"; return false; }
      out.groups.push_back({item_name, item_desc, (item & (1u << 24)) != 0});
      group = static_cast<int>(out.groups.size()) - 1;
      group_left = total;
      continue;
    }

    const u32 words = r.u32le();
    if (r.bad) { err = "code \"" + item_name + "\" has no length"; return false; }
    // Declared length must account for strings, word count and code.
    const u32 expect = ((static_cast<u32>(item_name.size()) + 1 + static_cast<u32>(item_desc.size()) + 1 + 3) >> 2) + 1 + words;
    if (expect != total) { err = "code \"" + item_name + "\" has a length of " + std::to_string(total) + ", expected " + std::to_string(expect); return false; }
    // Zero words: a note (credit line, heading), not a cheat; kept for the menu.
    if (words >= MAX_CODE_WORDS) { err = "code \"" + item_name + "\" has an unreasonable word count (" + std::to_string(words) + ")"; return false; }
    if (r.left() < static_cast<size_t>(words) * 4) { err = "code \"" + item_name + "\" runs past the end of the file"; return false; }

    Code c;
    c.name = item_name;
    c.description = item_desc;
    c.enabled = (item & (1u << 24)) != 0;
    c.group = group_left > 0 ? group : -1;
    c.words.reserve(words);
    for (u32 w = 0; w < words; ++w) c.words.push_back(r.u32le());
    out.codes.push_back(std::move(c));
    if (group_left > 0 && --group_left == 0) group = -1;
  }

  // An exclusive category may arrive with several codes marked enabled; first wins.
  for (size_t g = 0; g < out.groups.size(); ++g) {
    if (!out.groups[g].exclusive) continue;
    bool seen = false;
    for (Code& c : out.codes) {
      if (c.group != static_cast<int>(g) || !c.enabled) continue;
      if (seen) c.enabled = false;
      seen = true;
    }
  }
  return true;
}

} // namespace ds::cheat
