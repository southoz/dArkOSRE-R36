// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// The usrcheat.dat loader, against databases built here so that every field
// -- including the malformed ones -- can be produced on purpose.
#include "core/cheat/database.h"
#include "check.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace ds;

namespace {

const char* kPath = "cheat_db_test.tmp";

// Builds a database file the way r4cce writes one.
struct Builder {
  std::vector<u8> buf;

  void u32le(u32 v) { for (int i = 0; i < 4; ++i) buf.push_back(static_cast<u8>(v >> (8 * i))); }
  void str(const std::string& s) { buf.insert(buf.end(), s.begin(), s.end()); buf.push_back(0); }
  void align4() { while (buf.size() & 3) buf.push_back(0); }
  size_t here() const { return buf.size(); }

  Builder() {
    const char magic[16] = {'R','4',' ','C','h','e','a','t','C','o','d','e', 0, 1, 0, 0};
    buf.insert(buf.end(), magic, magic + 16);
    const std::string name = "test database";
    buf.insert(buf.end(), name.begin(), name.end());
    // The entry list lives at 0x100 and is patched in by write(), so room for
    // it is reserved before any payload goes down.
    buf.resize(0x100 + kMaxEntries * 16, 0);
  }
  static constexpr size_t kMaxEntries = 8;

  // The entry list is written at 0x100 once the payloads are placed, so the
  // builder collects them and patches the list at the end.
  struct Ent { u32 code, checksum; size_t offset; };
  std::vector<Ent> ents;

  void entry(u32 code, u32 checksum, size_t offset) { CHECK(ents.size() + 1 < kMaxEntries); ents.push_back({code, checksum, offset}); }

  // A game's payload: name, the item count, eight master words, then items.
  size_t begin_game(const std::string& name, u32 items) {
    align4();
    const size_t off = here();
    str(name);
    align4();
    u32le(items);
    for (int i = 0; i < 8; ++i) u32le(0);
    return off;
  }

  void code_item(const std::string& name, const std::string& desc, bool enabled,
                 const std::vector<u32>& words, int total_override = -1) {
    const u32 strlen4 = (static_cast<u32>(name.size()) + 1 + static_cast<u32>(desc.size()) + 1 + 3) >> 2;
    const u32 total = total_override >= 0 ? static_cast<u32>(total_override)
                                          : strlen4 + 1 + static_cast<u32>(words.size());
    u32le(total | (enabled ? (1u << 24) : 0u));
    str(name); str(desc); align4();
    u32le(static_cast<u32>(words.size()));
    for (u32 v : words) u32le(v);
  }

  void category(const std::string& name, const std::string& desc, u32 count, bool exclusive) {
    u32le(count | (exclusive ? (1u << 24) : 0u) | (1u << 28));
    str(name); str(desc); align4();
  }

  void write() {
    for (size_t i = 0; i < ents.size(); ++i) {
      u8* e = buf.data() + 0x100 + i * 16;
      const u32 vals[4] = {ents[i].code, ents[i].checksum, static_cast<u32>(ents[i].offset), 0};
      for (int k = 0; k < 4; ++k) for (int j = 0; j < 4; ++j) e[k * 4 + j] = static_cast<u8>(vals[k] >> (8 * j));
    }
    FILE* f = std::fopen(kPath, "wb");
    CHECK(f != nullptr);
    CHECK(std::fwrite(buf.data(), 1, buf.size(), f) == buf.size());
    std::fclose(f);
  }
};

constexpr u32 game(const char (&s)[5]) {
  return static_cast<u32>(s[0]) | (static_cast<u32>(s[1]) << 8) | (static_cast<u32>(s[2]) << 16) | (static_cast<u32>(s[3]) << 24);
}

void test_header() {
  cheat::Database db;
  std::string err;
  // Not a database at all.
  { FILE* f = std::fopen(kPath, "wb"); std::fputs("not a cheat database at all, but long enough to read", f); std::fclose(f); }
  CHECK(!db.open(kPath, err));
  CHECK(!err.empty());
  // Right magic, but nothing after the header.
  { FILE* f = std::fopen(kPath, "wb");
    const char magic[16] = {'R','4',' ','C','h','e','a','t','C','o','d','e', 0, 1, 0, 0};
    std::fwrite(magic, 1, 16, f); std::fclose(f); }
  CHECK(!db.open(kPath, err));
  // A missing file.
  CHECK(!db.open("no_such_database.tmp", err));
}

void test_basic_entry() {
  Builder b;
  const size_t off = b.begin_game("Test Game (U)", 2);
  b.code_item("Infinite Lives", "does what it says", false, {0x02000000, 0x00000063});
  b.code_item("Max Money", "", true, {0x020000F0, 0x000F423F});
  b.entry(game("ABCD"), 0x11223344, off);
  b.write();

  cheat::Database db;
  std::string err;
  CHECK(db.open(kPath, err));
  CHECK(db.name() == "test database");
  CHECK(db.game_count() == 1);
  CHECK(db.has(game("ABCD")));
  CHECK(!db.has(game("ZZZZ")));

  auto list = db.entries_for(game("ABCD"), err);
  CHECK(err.empty());
  CHECK(list.size() == 1);
  const cheat::GameCheats& g = list[0];
  CHECK(g.name == "Test Game (U)");
  CHECK(g.checksum == 0x11223344);
  CHECK(g.codes.size() == 2);
  CHECK(g.codes[0].name == "Infinite Lives");
  CHECK(g.codes[0].description == "does what it says");
  CHECK(!g.codes[0].enabled);
  CHECK(g.codes[0].words.size() == 2);
  CHECK(g.codes[0].words[0] == 0x02000000);
  CHECK(g.codes[0].words[1] == 0x00000063);
  CHECK(g.codes[1].name == "Max Money");
  CHECK(g.codes[1].description.empty());
  CHECK(g.codes[1].enabled);
  CHECK(g.codes[0].group == -1 && g.codes[1].group == -1);
}

// A category owns the next `count` codes; the ones after it are loose again.
void test_categories() {
  Builder b;
  const size_t off = b.begin_game("Grouped", 5);
  b.category("Characters", "pick one", 2, true);
  b.code_item("Mario", "", true, {0x02000000, 1});
  b.code_item("Luigi", "", true, {0x02000000, 2});   // both enabled in an exclusive group
  b.category("Extras", "", 1, false);
  b.code_item("Widescreen", "", true, {0x02000000, 3});
  b.entry(game("GRUP"), 0, off);
  b.write();

  cheat::Database db;
  std::string err;
  CHECK(db.open(kPath, err));
  auto list = db.entries_for(game("GRUP"), err);
  CHECK(err.empty());
  CHECK(list.size() == 1);
  const cheat::GameCheats& g = list[0];
  CHECK(g.groups.size() == 2);
  CHECK(g.groups[0].name == "Characters");
  CHECK(g.groups[0].description == "pick one");
  CHECK(g.groups[0].exclusive);
  CHECK(!g.groups[1].exclusive);
  CHECK(g.codes.size() == 3);
  CHECK(g.codes[0].group == 0 && g.codes[1].group == 0);
  CHECK(g.codes[2].group == 1);
  // The exclusive group keeps only the first enabled code.
  CHECK(g.codes[0].enabled);
  CHECK(!g.codes[1].enabled);
  CHECK(g.codes[2].enabled);
}

// A category's count running out puts later codes back outside any group.
void test_category_runs_out() {
  Builder b;
  const size_t off = b.begin_game("Loose", 3);
  b.category("Only one", "", 1, false);
  b.code_item("Inside", "", false, {0x02000000, 1});
  b.code_item("Outside", "", false, {0x02000000, 2});
  b.entry(game("LOOS"), 0, off);
  b.write();

  cheat::Database db;
  std::string err;
  CHECK(db.open(kPath, err));
  auto list = db.entries_for(game("LOOS"), err);
  CHECK(list.size() == 1);
  CHECK(list[0].codes[0].group == 0);
  CHECK(list[0].codes[1].group == -1);
}

// The published database carries notes and credits as items with no code at
// all, and a heading as a category of zero items. Both must survive: they
// were nearly half the file when a stricter reader dropped their games.
void test_notes_and_empty_categories() {
  Builder b;
  const size_t off = b.begin_game("Noted", 3);
  b.category("http://example.invalid", "", 0, false);
  b.code_item("(M) must be on", "the master code note", false, {});
  b.code_item("Real Cheat", "", false, {0x02000000, 7});
  b.entry(game("NOTE"), 0, off);
  b.write();

  cheat::Database db;
  std::string err;
  CHECK(db.open(kPath, err));
  auto list = db.entries_for(game("NOTE"), err);
  CHECK(err.empty());
  CHECK(list.size() == 1);
  const cheat::GameCheats& g = list[0];
  CHECK(g.groups.size() == 1);
  CHECK(g.codes.size() == 2);
  CHECK(g.codes[0].is_note());
  CHECK(g.codes[0].name == "(M) must be on");
  CHECK(!g.codes[1].is_note());
}

// An odd word count is a truncated last instruction, kept rather than fatal.
void test_odd_word_count() {
  Builder b;
  const size_t off = b.begin_game("Odd", 1);
  b.code_item("Half an instruction", "", false, {0x02000000, 1, 0x02000004});
  b.entry(game("ODDD"), 0, off);
  b.write();

  cheat::Database db;
  std::string err;
  CHECK(db.open(kPath, err));
  auto list = db.entries_for(game("ODDD"), err);
  CHECK(err.empty());
  CHECK(list.size() == 1);
  CHECK(list[0].codes.size() == 1);
  CHECK(list[0].codes[0].words.size() == 3);
}

// A length that does not match its own contents means the file is being read
// at the wrong offset, which must stop the entry rather than carry on.
void test_bad_item_length() {
  Builder b;
  const size_t off = b.begin_game("Broken", 1);
  b.code_item("Wrong length", "", false, {0x02000000, 1}, /*total_override=*/99);
  b.entry(game("BRKN"), 0, off);
  b.write();

  cheat::Database db;
  std::string err;
  CHECK(db.open(kPath, err));
  auto list = db.entries_for(game("BRKN"), err);
  CHECK(list.empty());
  CHECK(!err.empty());
}

// An offset inside the header or past the end is skipped, and the rest of
// the entry list still loads.
void test_bad_offsets() {
  Builder b;
  const size_t off = b.begin_game("Good", 1);
  b.code_item("Fine", "", false, {0x02000000, 1});
  b.entry(game("BAD1"), 0, 0x20);          // inside the header
  b.entry(game("BAD2"), 0, 0x7FFFFFF0);    // past the end
  b.entry(game("GOOD"), 0, off);
  b.write();

  cheat::Database db;
  std::string err;
  CHECK(db.open(kPath, err));
  CHECK(db.game_count() == 1);
  CHECK(db.has(game("GOOD")));
  CHECK(!db.has(game("BAD1")));
  CHECK(!db.has(game("BAD2")));
}

// Several revisions share a game code and are told apart by the checksum.
void test_revisions() {
  Builder b;
  const size_t a = b.begin_game("Game (E)", 1);
  b.code_item("Euro", "", false, {0x02000000, 1});
  const size_t c = b.begin_game("Game (U)", 1);
  b.code_item("US", "", false, {0x02000000, 2});
  b.entry(game("REVS"), 0xAAAAAAAA, a);
  b.entry(game("REVS"), 0xBBBBBBBB, c);
  b.write();

  cheat::Database db;
  std::string err;
  CHECK(db.open(kPath, err));
  auto list = db.entries_for(game("REVS"), err);
  CHECK(list.size() == 2);

  cheat::GameCheats g;
  CHECK(db.best_entry(game("REVS"), 0xBBBBBBBB, g, err));
  CHECK(g.name == "Game (U)");
  // An unknown checksum still returns something usable: the first entry.
  CHECK(db.best_entry(game("REVS"), 0x12345678, g, err));
  CHECK(g.name == "Game (E)");
  // A game that is not there at all is a miss, not a fallback.
  CHECK(!db.best_entry(game("ZZZZ"), 0, g, err));
}

// The complement of a CRC-32 over the ROM's first 512 bytes.
void test_header_checksum() {
  std::vector<u8> zeros(512, 0);
  CHECK(cheat::header_checksum(zeros.data()) == 0x4D558A87u);
  std::vector<u8> ramp(512);
  for (size_t i = 0; i < ramp.size(); ++i) ramp[i] = static_cast<u8>(i & 0xFF);
  CHECK(cheat::header_checksum(ramp.data()) == 0xE39ECA89u);
}

// A zipped game is looked up by the header inside the archive, which is what
// load_for_header takes: pointing the path form at the .zip reads the
// archive's own first 512 bytes and misses every entry.
void test_zipped_lookup() {
  Builder b;
  const size_t off = b.begin_game("Zipped Game", 1);
  b.code_item("Code", "", false, {0x02000000, 1});
  std::vector<u8> header(512, 0);
  std::memcpy(&header[0x0C], "ZIPD", 4);
  b.entry(game("ZIPD"), cheat::header_checksum(header.data()), off);
  b.write();

  cheat::GameCheats g;
  std::string err;
  CHECK(cheat::load_for_header(kPath, header.data(), g, err));
  CHECK(g.name == "Zipped Game");
  CHECK(g.codes.size() == 1);

  // The same game inside a zip: the file starts "PK\3\4" and its bytes at
  // 0x0C are the archive's, not the game's.
  const char* kZip = "cheat_db_test_zip.tmp";
  std::vector<u8> archive(512, 0);
  const u8 magic[4] = {'P', 'K', 3, 4};
  std::memcpy(archive.data(), magic, 4);
  FILE* f = std::fopen(kZip, "wb");
  CHECK(f != nullptr);
  CHECK(std::fwrite(archive.data(), 1, archive.size(), f) == archive.size());
  std::fclose(f);
  cheat::GameCheats miss;
  err.clear();
  CHECK(!cheat::load_for_rom(kPath, kZip, miss, err));
  std::remove(kZip);
}

} // namespace

int main() {
  test_header();
  test_basic_entry();
  test_categories();
  test_category_runs_out();
  test_notes_and_empty_categories();
  test_odd_word_count();
  test_bad_item_length();
  test_bad_offsets();
  test_revisions();
  test_header_checksum();
  test_zipped_lookup();
  std::remove(kPath);
  std::printf("cheat db: ok\n");
  return 0;
}
