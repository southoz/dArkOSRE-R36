// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Reading a ROM out of a zip, against archives built here so that every case
// -- including the malformed ones -- can be produced on purpose. This is the
// component where a bug hands back a silently corrupt ROM rather than an
// error, so the failure cases carry as much weight as the happy ones.
#include "core/cart/cart.h"
#include "core/cart/zip.h"
#include "core/cart/zip_cache.h"

#include <atomic>
#include <sys/stat.h>
#include <unistd.h>
#include "check.h"

#include <cstring>
#include <string>
#include <vector>

using namespace ds;

namespace {

// A game code that is in save_list.inc, and one that is not. The selection
// rule turns on exactly this distinction, so the test asserts the premise
// rather than trusting it -- a database regeneration that dropped the code
// would otherwise silently turn those cases into no-ops.
constexpr u32 kKnownCode   = 0x41464141;   // first entry of save_list.inc
constexpr u32 kUnknownCode = 0x5A5A5A5A;   // "ZZZZ"

// 96 KB of text, matching what tests/zip_deflate_blob.inc was compressed
// from. Regenerate both together:
//   data = b"".join(b"DSperate deflate window test block %06d\n" % i
//                   for i in range(4000))[:96*1024]
//   comp = zlib.compressobj(9, zlib.DEFLATED, -15)  # raw, no zlib wrapper
std::vector<u8> window_text() {
  std::vector<u8> v;
  char line[64];
  for (int i = 0; v.size() < 96u * 1024u; ++i) {
    const int n = std::snprintf(line, sizeof line, "DSperate deflate window test block %06d\n", i);
    v.insert(v.end(), line, line + n);
  }
  v.resize(96 * 1024);
  return v;
}

#include "zip_deflate_blob.inc"

// A synthetic ROM: a real 0x160 header with the fields the picker reads, then
// filler so the image is a plausible size.
std::vector<u8> make_rom(u32 game_code, u8 revision, size_t size = 0x400, u8 fill = 0xA5) {
  std::vector<u8> rom(size, fill);
  std::memset(rom.data(), 0, sizeof(cart::Header));
  std::memcpy(rom.data(), "TESTROM     ", 12);
  std::memcpy(rom.data() + 12, &game_code, 4);
  rom[30] = revision;                       // Header::rom_version
  return rom;
}

// Builds archives byte by byte, so a field can be made wrong on purpose.
struct Zip {
  std::vector<u8> buf;
  struct Rec { std::string name; u16 method, flags; u32 crc, csize, usize, local; };
  // A wrong CRC on purpose, for the test that wants the check to fire.
  bool break_crc = false;
  std::vector<Rec> recs;

  void u16le(u32 v) { buf.push_back(u8(v)); buf.push_back(u8(v >> 8)); }
  void u32le(u32 v) { for (int i = 0; i < 4; ++i) buf.push_back(u8(v >> (8 * i))); }
  void raw(const u8* p, size_t n) { buf.insert(buf.end(), p, p + n); }

  // DEFLATE's uncompressed block form: BFINAL|BTYPE=00, LEN, ~LEN, bytes.
  // A genuine method-8 stream that needs no compressor to produce, so the
  // container tests do not depend on the Huffman blob.
  static std::vector<u8> stored_blocks(const std::vector<u8>& in) {
    std::vector<u8> out;
    size_t off = 0;
    do {
      const size_t n = in.size() - off < 0xFFFF ? in.size() - off : 0xFFFF;
      const bool last = off + n == in.size();
      out.push_back(last ? 1 : 0);
      out.push_back(u8(n)); out.push_back(u8(n >> 8));
      out.push_back(u8(~n)); out.push_back(u8(~n >> 8));
      out.insert(out.end(), in.begin() + long(off), in.begin() + long(off + n));
      off += n;
    } while (off < in.size());
    return out;
  }

  // `usize_override` lets a test declare a size the stream does not produce.
  void add(const std::string& name, const std::vector<u8>& data, u16 method,
           u16 flags = 0, long usize_override = -1, const std::vector<u8>* payload = nullptr) {
    std::vector<u8> body = payload ? *payload
                         : method == 8 ? stored_blocks(data) : data;
    const u32 crc = cart::crc32_update(0, data.data(), data.size()) ^ (break_crc ? 1 : 0);
    Rec r{name, method, flags, crc, u32(body.size()),
          u32(usize_override >= 0 ? usize_override : long(data.size())), u32(buf.size())};
    u32le(0x04034B50); u16le(20); u16le(flags); u16le(method);
    u16le(0); u16le(0); u32le(crc);
    u32le(r.csize); u32le(r.usize);
    u16le(u32(name.size())); u16le(0);
    raw(reinterpret_cast<const u8*>(name.data()), name.size());
    raw(body.data(), body.size());
    recs.push_back(r);
  }

  // Finishes the archive. `break_central` corrupts the first central-directory
  // signature; `drop_eocd` leaves the record off entirely.
  void finish(bool break_central = false, bool drop_eocd = false) {
    const u32 cd_off = u32(buf.size());
    for (const Rec& r : recs) {
      u32le(break_central && &r == &recs[0] ? 0xDEADBEEF : 0x02014B50);
      u16le(20); u16le(20); u16le(r.flags); u16le(r.method);
      u16le(0); u16le(0); u32le(r.crc);
      u32le(r.csize); u32le(r.usize);
      u16le(u32(r.name.size())); u16le(0); u16le(0);
      u16le(0); u16le(0); u32le(0);
      u32le(r.local);
      raw(reinterpret_cast<const u8*>(r.name.data()), r.name.size());
    }
    const u32 cd_size = u32(buf.size()) - cd_off;
    if (drop_eocd) return;
    u32le(0x06054B50); u16le(0); u16le(0);
    u16le(u32(recs.size())); u16le(u32(recs.size()));
    u32le(cd_size); u32le(cd_off); u16le(0);
  }

  bool extract(std::vector<u8>& out, std::string& err, std::string* chosen = nullptr) const {
    return cart::extract_rom(buf.data(), buf.size(), out, err, chosen);
  }
  // The DSi path's form: a CIA is an image too, once the caller has said it
  // knows what to do with the container.
  bool extract_cia(std::vector<u8>& out, std::string& err, std::string* chosen = nullptr,
                   bool* was_cia = nullptr) const {
    return cart::extract_rom(buf.data(), buf.size(), out, err, chosen, true, was_cia);
  }
};

void test_sniffing() {
  const u8 pk[] = {'P', 'K', 3, 4, 0};
  const u8 nds[] = {0x12, 0x34, 0, 0xEA};
  CHECK(cart::is_zip(pk, sizeof pk));
  CHECK(!cart::is_zip(nds, sizeof nds));
  CHECK(!cart::is_zip(pk, 3));            // too short to tell
  CHECK(!cart::is_zip(nullptr, 0));
}

// Both storage methods must reproduce the ROM byte for byte.
void test_roundtrip() {
  for (u16 method : {u16(0), u16(8)}) {
    const std::vector<u8> rom = make_rom(kKnownCode, 0, 0x4000, 0x5C);
    Zip z;
    z.add("game.nds", rom, method);
    z.finish();
    std::vector<u8> out; std::string err, chosen;
    CHECK(z.extract(out, err, &chosen));
    CHECK(err.empty());
    CHECK(out == rom);
    CHECK(chosen == "game.nds");
  }
}

// Dynamic Huffman codes and back-references reaching across the 32 KB
// dictionary window -- the streaming loop this project wrote.
void test_deflate_window() {
  const std::vector<u8> text = window_text();
  const std::vector<u8> comp(kDeflated, kDeflated + sizeof kDeflated);
  Zip z;
  z.add("big.nds", text, 8, 0, -1, &comp);
  z.finish();
  std::vector<u8> out; std::string err;
  CHECK(z.extract(out, err));
  CHECK(out.size() == text.size());
  CHECK(out == text);
}

// A name is matched case-insensitively, and non-.nds entries are ignored.
void test_entry_matching() {
  Zip z;
  z.add("readme.txt", std::vector<u8>(100, 'x'), 0);
  z.add("GAME.NDS", make_rom(kKnownCode, 0), 0);
  z.finish();
  std::vector<u8> out; std::string err, chosen;
  CHECK(z.extract(out, err, &chosen));
  CHECK(chosen == "GAME.NDS");
}

// Which extensions are taken to hold an image, and the CIA rules: a bare
// entry always wins, and a container is only handed over to a caller that
// asked for one.
void test_entry_kinds() {
  for (const char* name : {"game.nds", "game.dsi", "game.srl", "GAME.SRL"}) {
    Zip z;
    z.add("readme.txt", std::vector<u8>(100, 'x'), 0);
    z.add(name, make_rom(kKnownCode, 0), 0);
    z.finish();
    std::vector<u8> out; std::string err, chosen;
    CHECK(z.extract(out, err, &chosen));
    CHECK(chosen == name);
  }
  {   // a .cia is found, but only extract_cia may have it
    Zip z;
    z.add("title.cia", make_rom(kKnownCode, 0), 0);
    z.finish();
    std::vector<u8> out; std::string err, chosen;
    CHECK(!z.extract(out, err, &chosen));
    CHECK(err.find("title.cia") != std::string::npos && err.find("DSi mode") != std::string::npos);
    bool was_cia = false;
    err.clear();
    CHECK(z.extract_cia(out, err, &chosen, &was_cia));
    CHECK(err.empty() && chosen == "title.cia" && was_cia);
  }
  {   // a bare image beats a container, whatever the archive order
    Zip z;
    z.add("title.cia", make_rom(kUnknownCode, 9), 0);
    z.add("title.nds", make_rom(kUnknownCode, 0), 0);
    z.finish();
    std::vector<u8> out; std::string err, chosen;
    bool was_cia = true;
    CHECK(z.extract_cia(out, err, &chosen, &was_cia));
    CHECK(chosen == "title.nds" && !was_cia);
  }
  {   // nothing usable names all four extensions, so the reason is actionable
    Zip z; z.add("cover.png", std::vector<u8>(100, 'p'), 0); z.finish();
    std::vector<u8> out; std::string err;
    CHECK(!z.extract_cia(out, err));
    CHECK(err.find(".cia") != std::string::npos);
  }
}

// peek_entry: a header without inflating the rest, stored and deflated alike.
void test_peek() {
  for (u16 method : {u16(0), u16(8)}) {
    Zip z;
    z.add("game.nds", make_rom(kKnownCode, 7, 0x4000), method);
    z.finish();
    cart::ZipEntry e; std::string err;
    CHECK(cart::find_rom(z.buf.data(), z.buf.size(), e, err));
    u8 head[0x160] = {};
    CHECK(cart::peek_entry(z.buf.data(), z.buf.size(), e, head, sizeof head) == sizeof head);
    CHECK(std::memcmp(head, "TESTROM", 7) == 0);
    CHECK(head[30] == 7);                       // the revision, not filler
    // Short of the whole entry, and never more than the entry holds.
    u8 big[8] = {};
    cart::ZipEntry tiny = e;
    tiny.usize = 4;
    CHECK(cart::peek_entry(z.buf.data(), z.buf.size(), tiny, big, sizeof big) == 4);
  }
}

// The selection rule: database membership first, then the highest revision,
// then archive order.
void test_selection() {
  {   // a listed game code beats an unlisted one, whatever the order
    Zip z;
    z.add("hack.nds", make_rom(kUnknownCode, 9), 0);
    z.add("retail.nds", make_rom(kKnownCode, 0), 0);
    z.finish();
    std::vector<u8> out; std::string err, chosen;
    CHECK(z.extract(out, err, &chosen));
    CHECK(chosen == "retail.nds");   // beats revision 9: the database wins first
  }
  {   // among listed codes, the latest revision
    Zip z;
    z.add("rev0.nds", make_rom(kKnownCode, 0), 0);
    z.add("rev2.nds", make_rom(kKnownCode, 2), 0);
    z.add("rev1.nds", make_rom(kKnownCode, 1), 0);
    z.finish();
    std::vector<u8> out; std::string err, chosen;
    CHECK(z.extract(out, err, &chosen));
    CHECK(chosen == "rev2.nds");
  }
  {   // a tie falls back to archive order
    Zip z;
    z.add("first.nds", make_rom(kKnownCode, 3, 0x400, 0x11), 0);
    z.add("second.nds", make_rom(kKnownCode, 3, 0x400, 0x22), 0);
    z.finish();
    std::vector<u8> out; std::string err, chosen;
    CHECK(z.extract(out, err, &chosen));
    CHECK(chosen == "first.nds");
    CHECK(out[0x300] == 0x11);       // really the first one's bytes
  }
  {   // nothing in the database: the same rule with that term dropped
    Zip z;
    z.add("a.nds", make_rom(kUnknownCode, 1), 0);
    z.add("b.nds", make_rom(kUnknownCode, 3), 0);
    z.add("c.nds", make_rom(kUnknownCode, 2), 0);
    z.finish();
    std::vector<u8> out; std::string err, chosen;
    CHECK(z.extract(out, err, &chosen));
    CHECK(chosen == "b.nds");
  }
  {   // the picker must read each candidate's header through its own
      // compression, not just the first entry's
    Zip z;
    z.add("old.nds", make_rom(kKnownCode, 1), 8);
    z.add("new.nds", make_rom(kKnownCode, 5), 8);
    z.finish();
    std::vector<u8> out; std::string err, chosen;
    CHECK(z.extract(out, err, &chosen));
    CHECK(chosen == "new.nds");
  }
}

// Every one of these must fail, and must not leave a partial image behind.
void test_failures() {
  // `expect` is a substring of the reason. Asserting only "it failed" is too
  // weak: several of these would still fail for an unrelated reason if the
  // check they are aimed at were removed, so the test would not notice.
  auto fails = [](const Zip& z, const char* expect) {
    std::vector<u8> out(999, 0xFF);
    std::string err;
    CHECK(!z.extract(out, err, nullptr));
    CHECK(err.find(expect) != std::string::npos);
    CHECK(out.empty() || out.size() == 999);   // never a half-written ROM
  };
  {   // no ROM in the archive
    Zip z; z.add("notes.txt", std::vector<u8>(50, 'q'), 0); z.finish();
    fails(z, "no .nds, .dsi, .srl or .cia file");
  }
  {   // an encrypted entry is refused, not silently decompressed as garbage
    Zip z; z.add("game.nds", make_rom(kKnownCode, 0), 0, 1); z.finish();
    fails(z, "encrypted");
  }
  {   // a compression method we do not implement
    Zip z; z.add("game.nds", make_rom(kKnownCode, 0), 12); z.finish();
    fails(z, "unsupported compression method");
  }
  {   // smaller than a DS header
    Zip z; z.add("game.nds", std::vector<u8>(0x40, 0), 0); z.finish();
    fails(z, "too small to be a ROM");
  }
  {   // truncated: the declared size is larger than the stream produces
    Zip z; z.add("game.nds", make_rom(kKnownCode, 0, 0x400), 8, 0, 0x8000); z.finish();
    fails(z, "ended early");
  }
  {   // stored, but the two sizes disagree
    Zip z; z.add("game.nds", make_rom(kKnownCode, 0, 0x400), 0, 0, 0x800); z.finish();
    fails(z, "sizes disagree");
  }
  {   // an absurd declared size must be refused before anything is allocated
    Zip z; z.add("game.nds", make_rom(kKnownCode, 0, 0x400), 8, 0, 0x7FFFFFFF); z.finish();
    fails(z, "larger than any DS card");
  }
  {   // corrupt central directory
    Zip z; z.add("game.nds", make_rom(kKnownCode, 0), 0); z.finish(true);
    fails(z, "bad central directory entry");
  }
  {   // no end-of-central-directory record
    Zip z; z.add("game.nds", make_rom(kKnownCode, 0), 0); z.finish(false, true);
    fails(z, "no end-of-central-directory");
  }
  {   // garbage that happens to start with the magic
    Zip z;
    z.buf = {'P', 'K', 3, 4, 0, 0, 0, 0, 0, 0};
    fails(z, "");
  }
  {   // a deflate stream of pure noise
    Zip z;
    std::vector<u8> junk(500);
    for (size_t i = 0; i < junk.size(); ++i) junk[i] = u8(i * 37 + 11);
    z.add("game.nds", std::vector<u8>(0x400, 0), 8, 0, 0x400, &junk);
    z.finish();
    fails(z, "ended early");
  }
}

// An offset that points outside the buffer must be caught, not followed.
void test_out_of_range() {
  Zip z;
  z.add("game.nds", make_rom(kKnownCode, 0), 0);
  z.finish();
  // Point the central directory's local-header offset past the end. It sits
  // at the last 4 bytes before the name in the central record.
  std::vector<u8> bad = z.buf;
  const size_t eocd = bad.size() - 22;
  const u32 cd_off = u32(bad[eocd + 16]) | (u32(bad[eocd + 17]) << 8) |
                     (u32(bad[eocd + 18]) << 16) | (u32(bad[eocd + 19]) << 24);
  for (int i = 0; i < 4; ++i) bad[cd_off + 42 + size_t(i)] = 0xFF;
  std::vector<u8> out; std::string err;
  CHECK(!cart::extract_rom(bad.data(), bad.size(), out, err));
  CHECK(!err.empty());

  // A local header that is present and well-signed, but whose extra-field
  // length pushes the payload past the end of the buffer. This reaches the
  // second bounds check in data_offset(); the case above stops at the first.
  bad = z.buf;
  bad[26 + 2] = 0xFF; bad[26 + 3] = 0xFF;   // local header extra_len = 65535
  out.clear(); err.clear();
  CHECK(!cart::extract_rom(bad.data(), bad.size(), out, err));
  CHECK(!err.empty());

  // And one whose compressed size claims more bytes than remain after it.
  bad = z.buf;
  for (int i = 0; i < 4; ++i) bad[18 + size_t(i)] = 0xFF;   // local csize
  const size_t eocd2 = bad.size() - 22;
  const u32 cd2 = u32(bad[eocd2 + 16]) | (u32(bad[eocd2 + 17]) << 8) |
                  (u32(bad[eocd2 + 18]) << 16) | (u32(bad[eocd2 + 19]) << 24);
  for (int i = 0; i < 4; ++i) bad[cd2 + 20 + size_t(i)] = 0xFE;   // central csize, not zip64
  out.clear(); err.clear();
  CHECK(!cart::extract_rom(bad.data(), bad.size(), out, err));
  CHECK(!err.empty());
}

// The premise the selection rule rests on.
void test_database_premise() {
  CHECK(cart::known_game_code(kKnownCode));
  CHECK(!cart::known_game_code(kUnknownCode));
}

} // namespace

// The CRC is checked as the bytes come out: a stream that inflates cleanly
// to the right length but the wrong bytes is still a corrupt archive.
void test_crc() {
  Zip z; z.break_crc = true;
  z.add("game.nds", make_rom(kKnownCode, 0, 0x4000), 8);
  z.finish();
  std::vector<u8> out; std::string err;
  CHECK(!z.extract(out, err));
  CHECK(err.find("CRC") != std::string::npos);
  CHECK(out.empty());
  CHECK(cart::crc32_update(0, reinterpret_cast<const u8*>("123456789"), 9) == 0xCBF43926u);   // the check value
}

// open_zip: stored entries mapped in place, deflated ones through the cache
// beside the archive, with the tag deciding whether the cache is reused.
static std::string write_temp_zip(const std::string& dir, const std::string& name, const Zip& z) {
  const std::string path = dir + "/" + name;
  FILE* f = std::fopen(path.c_str(), "wb");
  CHECK(f && std::fwrite(z.buf.data(), 1, z.buf.size(), f) == z.buf.size());
  std::fclose(f);
  return path;
}
static bool exists(const std::string& p) { struct stat st{}; return stat(p.c_str(), &st) == 0; }
static long mtime_of(const std::string& p) { struct stat st{}; return stat(p.c_str(), &st) == 0 ? long(st.st_mtime) : -1; }

void test_cache() {
  char tmpl[] = "/tmp/dsperate-zipcache-XXXXXX";
  const std::string dir = mkdtemp(tmpl);
  const std::vector<u8> rom = make_rom(kKnownCode, 0, 0x4321, 0x5C);   // not a page multiple
  std::string err;

  // Stored: no cache, the mapping is a range of the archive.
  {
    Zip z; z.add("stored.nds", rom, 0); z.finish();
    const std::string path = write_temp_zip(dir, "stored.zip", z);
    cart::ZipOpen how;
    auto src = cart::open_zip(path, how, err);
    CHECK(src && err.empty());
    CHECK(how.chosen == "stored.nds" && how.cache_path.empty() && !how.extracted);
    CHECK(src->size() == rom.size() && src->mapped());
    std::vector<u8> got(rom.size()); src->read(0, got.data(), u32(got.size()));
    CHECK(got == rom);
    CHECK(src->page(0x4000)[0x321] == 0xFF);
    CHECK(!exists(dir + "/.dsperate"));
  }
  // Deflated: extracted once beside the archive, reused after.
  {
    Zip z; z.add("deflated.nds", rom, 8); z.finish();
    const std::string path = write_temp_zip(dir, "game.zip", z);
    cart::ZipOpen how;
    long progress_calls = 0;
    how.progress = [](void* u, u64 done, u64 total) { ++*static_cast<long*>(u); CHECK(done <= total); };
    how.progress_user = &progress_calls;
    auto src = cart::open_zip(path, how, err);
    CHECK(src && err.empty());
    CHECK(how.extracted && how.cache_path == dir + "/.dsperate/game.nds");
    CHECK(how.cache_path == cart::zip_cache_path(path));
    CHECK(progress_calls >= 1);
    CHECK(exists(how.cache_path) && exists(how.cache_path + ".tag") && !exists(how.cache_path + ".part"));
    std::vector<u8> got(rom.size()); src->read(0, got.data(), u32(got.size()));
    CHECK(got == rom);
    src.reset();

    // Same archive again: the image is not rebuilt.
    const long stamp = mtime_of(how.cache_path);
    sleep(1);   // mtime resolution
    cart::ZipOpen again;
    auto src2 = cart::open_zip(path, again, err);
    CHECK(src2 && !again.extracted && again.cache_path == how.cache_path);
    CHECK(mtime_of(how.cache_path) == stamp);
    src2.reset();

    // A different archive under the same name (new entry, new CRC): rebuilt.
    const std::vector<u8> rom2 = make_rom(kKnownCode, 1, 0x4321, 0x77);
    Zip z2; z2.add("deflated.nds", rom2, 8); z2.finish();
    write_temp_zip(dir, "game.zip", z2);
    cart::ZipOpen third;
    auto src3 = cart::open_zip(path, third, err);
    CHECK(src3 && third.extracted);
    std::vector<u8> got3(rom2.size()); src3->read(0, got3.data(), u32(got3.size()));
    CHECK(got3 == rom2);
    src3.reset();

    // A corrupt archive fails loudly and leaves neither a .part nor a tag
    // that would vouch for the old image.
    Zip z3; z3.break_crc = true; z3.add("deflated.nds", rom, 8); z3.finish();
    write_temp_zip(dir, "game.zip", z3);
    cart::ZipOpen bad;
    CHECK(!cart::open_zip(path, bad, err) && err.find("CRC") != std::string::npos);
    CHECK(!exists(how.cache_path + ".part") && !exists(how.cache_path + ".tag"));
  }
  // Sweep: an image whose archive is gone is removed on the next open of
  // any archive in that directory; a stray .part goes with it.
  {
    const std::string keep_zip = dir + "/keep.zip", gone_zip = dir + "/gone.zip";
    Zip a; a.add("keep.nds", rom, 8); a.finish(); write_temp_zip(dir, "keep.zip", a);
    Zip b; b.add("gone.nds", make_rom(kKnownCode, 2, 0x4321, 0x33), 8); b.finish(); write_temp_zip(dir, "gone.zip", b);
    cart::ZipOpen o1, o2;
    CHECK(cart::open_zip(keep_zip, o1, err) && cart::open_zip(gone_zip, o2, err));
    CHECK(exists(o1.cache_path) && exists(o2.cache_path));
    const std::string cache_dir = dir + "/.dsperate";
    CHECK(cart::list_cache(cache_dir).size() >= 2);
    { FILE* f = std::fopen((cache_dir + "/junk.nds.part").c_str(), "wb"); CHECK(f); std::fclose(f); }
    CHECK(unlink(gone_zip.c_str()) == 0);
    cart::ZipOpen o3;
    CHECK(cart::open_zip(keep_zip, o3, err) && !o3.extracted);
    CHECK(exists(o1.cache_path) && !exists(o2.cache_path) && !exists(o2.cache_path + ".tag"));
    CHECK(!exists(cache_dir + "/junk.nds.part"));
    // clear_cache keeps only what it is told to.
    cart::clear_cache(cache_dir, o1.cache_path);
    CHECK(exists(o1.cache_path));
    CHECK(cart::list_cache(cache_dir).size() == 1);
    CHECK(cart::clear_cache(cache_dir, "") == rom.size());
    CHECK(cart::list_cache(cache_dir).empty());
    unlink(keep_zip.c_str());
  }
  // Size cap: the least recently launched image is evicted to make room,
  // never the one being opened.
  {
    const std::string cache_dir = dir + "/.dsperate";
    for (const char* n : {"c1.zip", "c2.zip", "c3.zip"}) {
      Zip z; z.add(std::string(n).substr(0, 2) + ".nds", make_rom(kKnownCode, 0, 0x4321, u8(n[1])), 8); z.finish();
      write_temp_zip(dir, n, z);
    }
    cart::ZipOpen o;
    o.max_bytes = rom.size() * 2 + 100;   // room for two images
    CHECK(cart::open_zip(dir + "/c1.zip", o, err));
    sleep(1);
    CHECK(cart::open_zip(dir + "/c2.zip", o, err));
    sleep(1);
    CHECK(cart::open_zip(dir + "/c1.zip", o, err) && !o.extracted);   // c1 launched again: now the newest
    sleep(1);
    CHECK(cart::open_zip(dir + "/c3.zip", o, err) && o.extracted);    // needs room: c2 goes, c1 stays
    CHECK(exists(cache_dir + "/c1.nds") && !exists(cache_dir + "/c2.nds") && exists(cache_dir + "/c3.nds"));
    cart::clear_cache(cache_dir, "");
    for (const char* n : {"c1.zip", "c2.zip", "c3.zip"}) unlink((dir + "/" + n).c_str());
  }
  // Cancel: the extraction stops, fails as "cancelled", and leaves nothing.
  {
    Zip z; z.add("big.nds", make_rom(kKnownCode, 0, 0x40000), 8); z.finish();
    const std::string path = write_temp_zip(dir, "big.zip", z);
    std::atomic<bool> cancel{true};
    cart::ZipOpen o; o.cancel = &cancel;
    CHECK(!cart::open_zip(path, o, err) && err == "cancelled");
    CHECK(!exists(o.cache_path) && !exists(o.cache_path + ".part") && !exists(o.cache_path + ".tag"));
    unlink(path.c_str());
  }
  // An unwritable directory falls back to the configured one, then fails.
  {
    const std::string ro = dir + "/ro";
    mkdir(ro.c_str(), 0555);
    Zip z; z.add("d.nds", rom, 8); z.finish();
    // The archive has to be written before the directory is read-only.
    chmod(ro.c_str(), 0755);
    const std::string path = write_temp_zip(ro, "g.zip", z);
    chmod(ro.c_str(), 0555);
    if (access(ro.c_str(), W_OK) != 0) {   // not root
      cart::ZipOpen none;
      CHECK(!cart::open_zip(path, none, err) && !err.empty());
      cart::ZipOpen fb; fb.fallback_dir = dir + "/cache";
      auto src = cart::open_zip(path, fb, err);
      CHECK(src && fb.cache_path == dir + "/cache/g.nds");
    }
    chmod(ro.c_str(), 0755);
  }
  // Leave nothing behind.
  const int rc = std::system(("rm -rf '" + dir + "'").c_str());
  CHECK(rc == 0);
}

int main() {
  test_crc();
  test_cache();
  test_database_premise();
  test_sniffing();
  test_roundtrip();
  test_deflate_window();
  test_entry_matching();
  test_entry_kinds();
  test_peek();
  test_selection();
  test_failures();
  test_out_of_range();
  std::printf("zip tests passed\n");
  return 0;
}
