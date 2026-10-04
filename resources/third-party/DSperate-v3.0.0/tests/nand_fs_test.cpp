// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// The DSi NAND filesystem layer (io/dsi_nand_fs): SHA-1 against the FIPS
// vectors, FatVolume on a FAT12 image built here (directories, files across
// clusters, overwrites that grow and shrink, a re-open from the raw bytes),
// and -- when DS_TEST_DSI_NAND and DS_TEST_DSI_BIOS7I name a dump and its
// ARM7 BIOS, which cannot ship with the tests -- the real NAND: mount, a
// title's save and the FAT12 inside it, the dump's tickets through ES
// decryption, and a write that stays in the session's memory. With
// DS_TEST_DSI_NAND_COPY naming a scratch copy as well, that write goes into
// the copy, for an independent tool to read back. The same variables
// run the save/system-file persistence round trip (io/dsi_nand_persist).
#include "core/crypto/sha1.h"
#include "core/io/dsi_nand_fs.h"
#include "core/io/dsi_nand_persist.h"
#include "core/io/dsi_nand_synth.h"
#include "core/io/dsi_nand_launch.h"
#include "core/io/dsi_sd.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <unistd.h>

using namespace ds;
using ds::io::FatVolume;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static std::string hex(const u8* p, size_t n) {
  std::string s;
  char b[3];
  for (size_t i = 0; i < n; ++i) { std::snprintf(b, sizeof b, "%02x", p[i]); s += b; }
  return s;
}

static void test_sha1() {
  u8 d[20];
  crypto::sha1(reinterpret_cast<const u8*>("abc"), 3, d);
  CHECK(hex(d, 20) == "a9993e364706816aba3e25717850c26c9cd0d89d");
  crypto::sha1(nullptr, 0, d);
  CHECK(hex(d, 20) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
  // 56 bytes: the length no longer fits after the terminator, so two blocks.
  const char* m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  crypto::sha1(reinterpret_cast<const u8*>(m), std::strlen(m), d);
  CHECK(hex(d, 20) == "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
}

// A FAT12 volume the shape of a small floppy: 512-byte sectors, one sector
// per cluster, 1 reserved sector, 2 FATs of 3 sectors, 112 root entries.
static std::vector<u8> make_fat12(u32 total_sectors) {
  std::vector<u8> img(static_cast<size_t>(total_sectors) * 512, 0);
  u8* b = img.data();
  b[0x0B] = 0x00; b[0x0C] = 0x02;   // 512 bytes per sector
  b[0x0D] = 1;                      // sectors per cluster
  b[0x0E] = 1;                      // reserved
  b[0x10] = 2;                      // FATs
  b[0x11] = 112;                    // root entries
  b[0x13] = static_cast<u8>(total_sectors); b[0x14] = static_cast<u8>(total_sectors >> 8);
  b[0x16] = 3;                      // sectors per FAT
  b[0x1FE] = 0x55; b[0x1FF] = 0xAA;
  for (int f = 0; f < 2; ++f) { u8* fat = b + 512 * (1 + 3 * f); fat[0] = 0xF8; fat[1] = 0xFF; fat[2] = 0xFF; }
  return img;
}

static bool open_on(FatVolume& v, std::vector<u8>& img) {
  return v.open([&img](u64 o, u32 n, u8* out) { std::memcpy(out, img.data() + o, n); },
                [&img](u64 o, u32 n, const u8* in) { std::memcpy(img.data() + o, in, n); });
}

static void test_fat12() {
  std::vector<u8> img = make_fat12(1440);
  FatVolume v;
  CHECK(open_on(v, img));
  CHECK(v.fat_bits() == 12);
  const u32 free0 = v.free_clusters();

  std::string err;
  CHECK(v.mkdir("/title", &err));
  CHECK(v.mkdir("/TITLE/00030004", &err));   // case-insensitive
  CHECK(v.mkdir("/title/00030004", &err));   // already there: fine
  CHECK(!v.mkdir("/nope/deeper", &err));     // no parent

  std::vector<u8> data(5000);
  for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<u8>(i * 7 + 3);
  CHECK(v.write("/title/00030004/public.sav", data.data(), static_cast<u32>(data.size()), &err));
  CHECK(v.free_clusters() == free0 - 2 - 10);   // two directory clusters, ten data clusters

  // Enough files to spill a subdirectory past its first cluster (16 entries).
  for (int i = 0; i < 20; ++i) {
    const std::string p = "/title/00030004/F" + std::to_string(i) + ".BIN";
    const u8 byte = static_cast<u8>(i);
    CHECK(v.write(p, &byte, 1, &err));
  }

  // Grow, then shrink, an existing file.
  std::vector<u8> big(9000, 0xAB);
  CHECK(v.write("/title/00030004/public.sav", big.data(), static_cast<u32>(big.size()), &err));
  std::vector<u8> small(100, 0xCD);
  CHECK(v.write("/title/00030004/public.sav", small.data(), static_cast<u32>(small.size()), &err));

  // Everything again from the raw bytes alone.
  FatVolume w;
  CHECK(open_on(w, img));
  FatVolume::Entry e;
  std::vector<u8> got;
  CHECK(w.lookup("/title/00030004/PUBLIC.SAV", e) && w.read(e, got) && got == small);
  CHECK(w.lookup("/title/00030004/F19.BIN", e) && w.read(e, got) && got.size() == 1 && got[0] == 19);
  CHECK(w.lookup("/title/00030004/F0.BIN", e) && w.read(e, got) && got.size() == 1 && got[0] == 0);
  int files = 0;
  w.walk([&](const std::string&, const FatVolume::Entry& x) { files += !x.dir(); });
  CHECK(files == 21);
  // 2 directory clusters + 1 more for the spilled directory, 1 for public.sav, 20 one-byte files.
  CHECK(w.free_clusters() == free0 - 3 - 1 - 20);

  // A name that is not 8.3 gets NAME~N.EXT and long-name entries; it is found
  // by either name, case-insensitively, and survives a re-open.
  CHECK(w.write("/title/00030004/TWLFontTable.dat", small.data(), static_cast<u32>(small.size()), &err));
  CHECK(w.write("/title/00030004/TWLFontOther.dat", small.data(), 1, &err));
  FatVolume x;
  CHECK(open_on(x, img));
  CHECK(x.lookup("/title/00030004/twlfonttable.DAT", e) && e.name == "TWLFON~1.DAT" && e.long_name == "TWLFontTable.dat");
  CHECK(x.lookup("/title/00030004/TWLFON~1.DAT", e) && x.read(e, got) && got == small);
  CHECK(x.lookup("/title/00030004/TWLFontOther.dat", e) && e.name == "TWLFON~2.DAT" && e.size == 1);
  // Rewriting through the long name replaces that file, not a third one.
  CHECK(x.write("/title/00030004/TWLFontTable.dat", big.data(), static_cast<u32>(big.size()), &err));
  CHECK(x.lookup("/title/00030004/TWLFontTable.dat", e) && x.read(e, got) && got == big);
  CHECK(!x.lookup("/title/00030004/TWLFON~3.DAT", e));
}

// A NAND made from nothing (io/dsi_nand_synth): the retail layout, mountable
// under its made-up console, holding the title where the launcher's mount
// table points and settings files whose hashes check.
static void test_synthetic_nand() {
  std::vector<u8> srl(0x6000);
  for (size_t i = 0; i < srl.size(); ++i) srl[i] = static_cast<u8>(i * 7);
  const u32 lo = 0x4B535445, hi = 0x00030004;   // "ETSK"
  std::memcpy(&srl[0x230], &lo, 4); std::memcpy(&srl[0x234], &hi, 4);
  const u32 pub = 0x4000, prv = 0;
  std::memcpy(&srl[0x238], &pub, 4); std::memcpy(&srl[0x23C], &prv, 4);
  srl[0x1BF] = 0x04;                            // banner.sav
  ds::bios::UserSettings user;
  user.nickname = "Tester";
  const ds::io::DsiRegion region = ds::io::dsi_region_for(0x00000002, 1);
  ds::io::DsiConsoleFiles files = ds::io::make_dsi_console_files(user, region, ds::io::kSynthConsoleId);
  files.font = std::vector<u8>(0x300, 0xAB);

  ds::io::NandImage nand;
  std::string err;
  CHECK(ds::io::build_synthetic_nand(nand, nullptr, srl, files, &err));
  CHECK(nand.in_memory() && nand.console_id() == ds::io::kSynthConsoleId);
  nand.mark_baseline();
  CHECK(!nand.any_changed());

  ds::io::NandFs fs;
  CHECK(fs.mount(nand, nullptr, &err));
  CHECK(fs.main().fat_bits() == 16 && fs.main().cluster_bytes() == 0x4000);
  CHECK(fs.photo().valid());
  FatVolume::Entry e;
  std::vector<u8> got;
  CHECK(fs.main().lookup("/title/00030004/4b535445/content/00000000.app", e) && fs.main().read(e, got) && got == srl);
  CHECK(fs.main().lookup("/title/00030004/4b535445/data/public.sav", e) && e.size == pub);
  CHECK(!fs.main().lookup("/title/00030004/4b535445/data/private.sav", e));
  CHECK(fs.main().lookup("/title/00030004/4b535445/data/banner.sav", e) && e.size == 0x4000);
  CHECK(fs.main().lookup("/sys/TWLFontTable.dat", e) && e.name == "TWLFON~1.DAT" && e.size == 0x300);
  CHECK(fs.main().lookup("/shared1/TWLCFG0.dat", e) && fs.main().read(e, got) && got.size() == 0x4000);
  u8 digest[20];
  crypto::sha1(&got[0x88], 0x128, digest);
  CHECK(std::memcmp(digest, got.data(), 20) == 0);
  CHECK(got[0x8D] == 0x31 && got[0x8E] == 1 && got[0xD0] == 'T');   // USA, English, the nickname
  CHECK(fs.main().lookup("/sys/HWINFO_S.dat", e) && fs.main().read(e, got) && got[0x90] == 1 && std::memcmp(&got[0xA0], "EANH", 4) == 0);
  CHECK(files.boot_blobs().size() == 0x154);

  // A guest write after the baseline is the session's; the build's were not.
  u8 sec[512] = {};
  nand.write(0x10EE00ull + 0x200000, 512, sec);
  CHECK(nand.any_changed() && nand.changed((0x10EE00ull + 0x200000) / 512) && !nand.changed(0));
}

// DSperate's own TWLFontTable.dat (io/dsi_font): the table layout, every
// SHA-1 a title checks, and the signature marker the SWI 22h HLE keys on.
// The built-in tables, one per console layout: the TWL SDK's
// OS_LoadSharedFont takes a resource index under the entry count, and 3 or
// more only while header byte 0x86 is set (read from a Korean title's code).
static void test_builtin_font() {
  struct Layout { u8 region; u32 count, first; u8 region_byte; const char* names[3]; };
  const Layout layouts[] = {
    {1, 3, 0, 0, {"TBF1_l.NFTR", "TBF1_m.NFTR", "TBF1_s.NFTR"}},
    {4, 9, 3, 4, {"TBF1-cn_l.NFTR", "TBF1-cn_m.NFTR", "TBF1-cn_s.NFTR"}},
    {5, 9, 6, 5, {"TBF1-kr_l.NFTR", "TBF1-kr_m.NFTR", "TBF1-kr_s.NFTR"}},
  };
  CHECK(ds::io::builtin_dsi_font(0) == ds::io::builtin_dsi_font(1));   // Japan shares the normal table
  CHECK(ds::io::builtin_dsi_font(2) == ds::io::builtin_dsi_font(1));
  for (const Layout& l : layouts) {
  const std::vector<u8> f = ds::io::builtin_dsi_font(l.region);
  CHECK(f.size() > 0x100000 / 5);
  if (f.size() < 0xA0 + 9 * 0x40) continue;
  CHECK(ds::io::is_builtin_font_signature(f.data()));
  CHECK(ds::io::font_table_region(f) == (l.region == 1 ? 0 : l.region));
  std::vector<u8> other(f.begin(), f.begin() + 0x80);
  other[0x10] ^= 1;
  CHECK(!ds::io::is_builtin_font_signature(other.data()));
  const u32 n = f[0x84];
  CHECK(n == l.count && f[0x86] == l.region_byte);
  u8 d[20];
  crypto::sha1(&f[0xA0], n * 0x40, d);
  CHECK(std::memcmp(d, &f[0x8C], 20) == 0);
  for (u32 i = 0; i < n; ++i) {
    const u8* e = &f[0xA0 + i * 0x40];
    const bool used = i >= l.first && i < l.first + 3;
    bool zero = true;
    for (u32 k = 0; k < 0x40; ++k) zero = zero && e[k] == 0;
    CHECK(used != zero);
    if (!used) continue;
    CHECK(std::strcmp(reinterpret_cast<const char*>(e), l.names[i - l.first]) == 0);
    u32 csize, cstart, dsize;
    std::memcpy(&csize, e + 0x20, 4); std::memcpy(&cstart, e + 0x24, 4); std::memcpy(&dsize, e + 0x28, 4);
    CHECK(cstart % 16 == 0 && static_cast<u64>(cstart) + csize <= f.size() && dsize > csize);
    if (static_cast<u64>(cstart) + csize > f.size() || csize < 8) continue;
    crypto::sha1(&f[cstart], csize, d);
    CHECK(std::memcmp(d, e + 0x2C, 20) == 0);
    // The backwards-LZ footer: the decompressed size is the stored one.
    u32 extra;
    std::memcpy(&extra, &f[cstart + csize - 4], 4);
    CHECK(csize + extra == dsize);
  }
  }
}

// The region a title is run in, from its header and the user's language.
static void test_region() {
  auto r = ds::io::dsi_region_for(0x00000002, 3);          // USA only, German wanted
  CHECK(r.region == 1 && r.language == 1 && r.letter == 'E');
  r = ds::io::dsi_region_for(0x00000004, 3);               // Europe: German is there
  CHECK(r.region == 2 && r.language == 3 && r.letter == 'P');
  r = ds::io::dsi_region_for(0xFFFFFFFF, 0);               // region-free, Japanese
  CHECK(r.region == 0 && r.language == 0);
  r = ds::io::dsi_region_for(0xFFFFFFFF, 1);               // region-free, English: the USA console
  CHECK(r.region == 1);
  r = ds::io::dsi_region_for(0x00000020, 1);               // Korea only
  CHECK(r.region == 5 && r.language == 7 && r.letter == 'K');
}

static std::vector<u8> slurp(const char* path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<u8>(std::istreambuf_iterator<char>(f), {});
}

static void test_real_nand() {
  const char* path = std::getenv("DS_TEST_DSI_NAND");
  const char* b7path = std::getenv("DS_TEST_DSI_BIOS7I");
  if (!path || !b7path) { std::printf("nand_fs: DS_TEST_DSI_NAND / DS_TEST_DSI_BIOS7I not set, real-dump checks skipped\n"); return; }
  const std::vector<u8> bios7i = slurp(b7path);
  CHECK(bios7i.size() == 0x10000);
  if (bios7i.size() != 0x10000) return;

  const char* copy = std::getenv("DS_TEST_DSI_NAND_COPY");
  io::NandImage nand;
  CHECK(nand.open(copy ? copy : path, copy != nullptr));
  io::NandFs fs;
  std::string err;
  CHECK(fs.mount(nand, bios7i.data(), &err));
  if (!fs.valid()) { std::fprintf(stderr, "mount: %s\n", err.c_str()); return; }
  CHECK(fs.main().fat_bits() == 16);

  // A title's save, and the FAT12 volume the SDK formats inside it.
  FatVolume::Entry e;
  std::vector<u8> sav;
  CHECK(fs.main().lookup("/title/00030004/4b533345/data/public.sav", e) && fs.main().read(e, sav));
  CHECK(sav.size() == 16384);
  FatVolume inner;
  CHECK(inner.open([&sav](u64 o, u32 n, u8* out) { std::memcpy(out, sav.data() + o, n); },
                   [&sav](u64 o, u32 n, const u8* in) { std::memcpy(sav.data() + o, in, n); }, &err));
  CHECK(inner.fat_bits() == 12);

  // Every DSiWare ticket in the dump decrypts under the ES key with a good MAC.
  FatVolume::Entry tdir;
  int tickets = 0;
  if (fs.main().lookup("/ticket/00030004", tdir)) {
    for (const FatVolume::Entry& t : fs.main().list(tdir)) {
      if (t.dir()) continue;
      std::vector<u8> tik;
      CHECK(fs.main().read(t, tik) && tik.size() == 0x2C4);
      CHECK(fs.es_decrypt(tik.data(), 0x2A4));
      ++tickets;
    }
  }
  CHECK(tickets > 0);

  // A ticket the way melonDS's ImportTitle makes one survives the round trip.
  u8 tik[0x2C4] = {};
  tik[0x1DC] = 0x00; tik[0x1DD] = 0x03; tik[0x1DE] = 0x00; tik[0x1DF] = 0x04;
  const u8 nonce[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
  fs.es_encrypt(tik, 0x2A4, nonce);
  CHECK(fs.es_decrypt(tik, 0x2A4));
  CHECK(tik[0x1DD] == 0x03 && tik[0x1DF] == 0x04);

  // Write through the session: a directory and a file of several clusters.
  const u32 free0 = fs.main().free_clusters();
  std::vector<u8> data(3 * fs.main().cluster_bytes() + 77);
  for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<u8>(i ^ (i >> 9));
  CHECK(fs.main().mkdir("/dsperate", &err));
  CHECK(fs.main().write("/dsperate/probe.bin", data.data(), static_cast<u32>(data.size()), &err));
  CHECK(fs.main().free_clusters() == free0 - 1 - 4);
  if (!copy) CHECK(!nand.written_sectors().empty());

  io::NandFs again;
  CHECK(again.mount(nand, bios7i.data(), &err));
  std::vector<u8> back;
  CHECK(again.main().lookup("/DSPERATE/PROBE.BIN", e) && again.main().read(e, back) && back == data);
  CHECK(again.main().lookup("/title/00030004/4b533345/data/public.sav", e) && again.main().read(e, back) && back.size() == 16384);
  std::printf("nand_fs: real dump ok (%d tickets, %u clusters free before the probe)\n", tickets, free0);
}

// Saves and system files in and out of a session (io/dsi_nand_persist), on a
// fresh read-only session over the real dump: a host save goes in and is what
// the NAND then holds, an unchanged session exports nothing, a changed save
// and a changed system file come back out, and the dump is never written.
static void test_persist() {
  const char* path = std::getenv("DS_TEST_DSI_NAND");
  const char* b7path = std::getenv("DS_TEST_DSI_BIOS7I");
  if (!path || !b7path || std::getenv("DS_TEST_DSI_NAND_COPY")) return;
  const std::vector<u8> bios7i = slurp(b7path);
  if (bios7i.size() != 0x10000) return;
  namespace fsys = std::filesystem;
  const fsys::path dir = fsys::temp_directory_path() / ("dsperate_persist_test_" + std::to_string(::getpid()));
  fsys::remove_all(dir);
  fsys::create_directories(dir);
  const io::NandPersistPaths paths{dir.string(), (dir / "nand.ovr").string(), (dir / "photos").string()};

  std::vector<u8> save(16384);
  for (size_t i = 0; i < save.size(); ++i) save[i] = static_cast<u8>(i * 13 + 5);
  { std::ofstream f(dir / "KS3E.pub", std::ios::binary); f.write(reinterpret_cast<const char*>(save.data()), save.size()); }
  const u8 wrong[3] = {1, 2, 3};
  { std::ofstream f(dir / "KMGE.pub", std::ios::binary); f.write(reinterpret_cast<const char*>(wrong), 3); }

  io::NandImage nand;
  CHECK(nand.open(path));
  io::NandPersistReport in = io::nand_import(nand, bios7i.data(), paths);
  CHECK(in.saves == 1);                 // KS3E; KMGE's is the wrong size and is left alone
  CHECK(in.notes.size() == 1);

  io::NandFs fs;
  std::string err;
  CHECK(fs.mount(nand, bios7i.data(), &err));
  FatVolume::Entry e;
  std::vector<u8> got;
  CHECK(fs.main().lookup("/title/00030004/4b533345/data/public.sav", e) && fs.main().read(e, got) && got == save);

  // Nothing changed since the import: nothing to write.
  io::NandPersistReport out = io::nand_export(nand, bios7i.data(), paths);
  CHECK(out.saves == 0 && out.system_files == 0);

  // The session changes the save and a system file.
  save[100] ^= 0xFF;
  CHECK(fs.main().write("/title/00030004/4b533345/data/public.sav", save.data(), static_cast<u32>(save.size()), &err));
  CHECK(fs.main().lookup("/shared1/TWLCFG0.dat", e) && fs.main().read(e, got));
  got[0x100] ^= 0x5A;
  CHECK(fs.main().write("/shared1/TWLCFG0.dat", got.data(), static_cast<u32>(got.size()), &err));
  out = io::nand_export(nand, bios7i.data(), paths);
  CHECK(out.saves == 1 && out.system_files == 1);
  std::vector<u8> host = slurp((dir / "KS3E.pub").string().c_str());
  CHECK(host == save);

  // A new session over the untouched dump takes both back in.
  io::NandImage nand2;
  CHECK(nand2.open(path));
  in = io::nand_import(nand2, bios7i.data(), paths);
  CHECK(in.saves == 1 && in.system_files == 1);
  io::NandFs fs2;
  CHECK(fs2.mount(nand2, bios7i.data(), &err));
  std::vector<u8> cfg;
  CHECK(fs2.main().lookup("/shared1/TWLCFG0.dat", e) && fs2.main().read(e, cfg) && cfg == got);

  fsys::remove_all(dir);
  std::printf("nand_fs: persistence ok\n");
}

// Save states (NandImage::state_delta): the sectors written since the state
// base, applied back over a session that has moved on.
static void test_nand_state() {
  const u8 cid[16] = {1, 2, 3};
  auto sector = [](u8 fill) { std::vector<u8> s(512, fill); return s; };
  auto at = [](io::NandImage& n, u64 sec) { std::vector<u8> s(512); n.peek(sec * 512, 512, s.data()); return s; };
  auto build = [&](io::NandImage& n, u8 base_fill) {
    n.create_in_memory(64 << 20, cid, 0x1234);
    for (u64 s = 0; s < 4; ++s) n.poke(s * 512, 512, sector(base_fill).data());
    n.mark_state_base();
  };
  io::NandImage nand;
  build(nand, 0xAA);
  nand.write(2 * 512, 512, sector(0xB0).data());
  nand.write(10 * 512, 512, sector(0xC0).data());
  const io::NandImage::StateDelta d = nand.state_delta();
  CHECK(d.sectors == std::vector<u64>({2, 10}) && d.data.size() == 1024);
  nand.write(3 * 512, 512, sector(0xE0).data());
  nand.write(11 * 512, 512, sector(0xF0).data());
  nand.write(2 * 512, 512, sector(0xB1).data());

  io::NandImage same, other;
  build(same, 0xAA);
  build(other, 0xAB);
  CHECK(same.state_identity() == nand.state_identity());
  CHECK(other.state_identity() != nand.state_identity());

  nand.apply_state_delta(d);
  CHECK(at(nand, 2) == sector(0xB0));
  CHECK(at(nand, 3) == sector(0xAA));   // back to the base
  CHECK(at(nand, 10) == sector(0xC0));
  CHECK(at(nand, 11) == sector(0));
  CHECK(nand.state_delta().sectors == d.sectors && nand.state_delta().data == d.data);
  // Onto another session of the same base.
  same.write(1 * 512, 512, sector(0x11).data());
  same.apply_state_delta(d);
  CHECK(at(same, 1) == sector(0xAA) && at(same, 2) == sector(0xB0) && at(same, 10) == sector(0xC0));

  // Unmarked: the base is the image as created, and everything written is carried.
  io::NandImage plain;
  plain.create_in_memory(64 << 20, cid, 0x1234);
  plain.write(5 * 512, 512, sector(0x55).data());
  const io::NandImage::StateDelta p = plain.state_delta();
  plain.write(6 * 512, 512, sector(0x66).data());
  plain.apply_state_delta(p);
  CHECK(at(plain, 5) == sector(0x55) && at(plain, 6) == sector(0));
  CHECK(plain.state_identity() != nand.state_identity());
}

// NAND title shortcuts (io/dsi_nand_launch.h): the titles, their banner
// names, the .app and console data read back, and the .dspr.nds sync.
static void test_nand_shortcuts() {
  std::vector<u8> srl(0x6000, 0);
  const u32 lo = 0x4B535445, hi = 0x00030004;   // "ETSK"
  std::memcpy(&srl[0x230], &lo, 4); std::memcpy(&srl[0x234], &hi, 4);
  const u32 banner = 0x5000;
  std::memcpy(&srl[0x68], &banner, 4);
  const char16_t en[] = u"Test: Title\nPublisher";
  for (size_t i = 0; en[i]; ++i) { srl[banner + 0x340 + i * 2] = static_cast<u8>(en[i]); srl[banner + 0x341 + i * 2] = static_cast<u8>(en[i] >> 8); }
  ds::bios::UserSettings user;
  ds::io::DsiConsoleFiles files = ds::io::make_dsi_console_files(user, ds::io::dsi_region_for(0x00000002, 1), ds::io::kSynthConsoleId);
  files.font = std::vector<u8>(0x300, 0xAB);
  ds::io::NandImage nand;
  std::string err;
  CHECK(ds::io::build_synthetic_nand(nand, nullptr, srl, files, &err));

  const std::vector<ds::io::NandTitle> titles = ds::io::nand_dsiware_titles(nand, nullptr);
  CHECK(titles.size() == 1);
  if (titles.size() != 1) return;
  CHECK(titles[0].title_lo == lo && titles[0].code == "KSTE" && titles[0].name == "Test Title");   // the first line, made file-safe
  std::vector<u8> app, blobs;
  u32 cid = 99;
  CHECK(ds::io::nand_read_title_app(nand, nullptr, lo, app, cid, &err) && app == srl && cid == 0);
  CHECK(!ds::io::nand_read_title_app(nand, nullptr, 0x4B585858, app, cid, &err));
  CHECK(ds::io::nand_boot_blobs(nand, nullptr, blobs, &err) && blobs == files.boot_blobs());

  const std::filesystem::path dir = std::filesystem::temp_directory_path() / ("dsperate-shortcuts-" + std::to_string(getpid()));
  std::filesystem::create_directories(dir);
  { std::ofstream(dir / "game.nds") << "x"; std::ofstream(dir / "Gone Title.dspr.nds") << "stale"; }
  ds::io::ShortcutSync r = ds::io::sync_shortcuts(dir.string(), &nand, nullptr, true);
  CHECK(r.written == 1 && r.removed == 1);
  ds::io::NandShortcut sc;
  CHECK(ds::io::is_shortcut_name((dir / "Test Title.dspr.nds").string()) && ds::io::is_shortcut_name("A.DSPR.NDS") && !ds::io::is_shortcut_name("a.nds"));
  CHECK(ds::io::read_shortcut((dir / "Test Title.dspr.nds").string(), sc) && sc.title_lo == lo && sc.from(nand));
  ds::io::NandImage other;
  other.create_in_memory(64 << 20, files.hwinfo_s.data(), 0x1234);
  CHECK(!sc.from(other));
  r = ds::io::sync_shortcuts(dir.string(), &nand, nullptr, true);
  CHECK(r.written == 0 && r.removed == 0);   // already up to date
  r = ds::io::sync_shortcuts(dir.string(), nullptr, nullptr, false);
  CHECK(r.removed == 1 && !std::filesystem::exists(dir / "Test Title.dspr.nds") && std::filesystem::exists(dir / "game.nds"));
  std::filesystem::remove_all(dir);
}

// Shortcut names in plain ASCII: a FAT card mounted iocharset=ascii (ROCKNIX's
// games card) refuses every other byte, which is how "Shantae: Risky's
// Revenge(TM)" failed to be written on the RG DS Plus.
static void test_shortcut_names() {
  auto ascii = [](const std::u16string& s) {
    std::vector<ds::u8> b(0x100, 0);
    for (size_t i = 0; i < s.size() && i < 0x80; ++i) { b[i * 2] = static_cast<ds::u8>(s[i]); b[i * 2 + 1] = static_cast<ds::u8>(s[i] >> 8); }
    return ds::io::banner_title_ascii(b.data());
  };
  CHECK(ascii(u"Shantae: Risky\u2019s Revenge\u2122") == "Shantae: Risky's Revenge");
  CHECK(ascii(u"Dark Void\u2122 Zero") == "Dark Void Zero");
  CHECK(ascii(u"Pok\u00E9mon\u00AE Art Academy\u00A9") == "Pokemon Art Academy");
  CHECK(ascii(u"Na\u00EFve \u00C6ther \u0141\u00F3d\u017A \u00DFtra\u00DFe") == "Naive AEther Lodz sstrasse");
  CHECK(ascii(u"\uFF21\uFF22\uFF23\u3000\uFF11\uFF12") == "ABC 12");      // fullwidth ASCII
  CHECK(ascii(u"A \u2013 B\u2026") == "A - B...");
  CHECK(ascii(u"\uE000 Start\uE001") == " Start");                            // the DSi font's button glyphs
  CHECK(ascii(u"\u3074\u3053\u3063\u3068\u30A2\u30ED\u30FC") == "-");      // kana: only the long-vowel mark survives
  CHECK(ascii(u"Line one\nLine two") == "Line one");                            // the first line only
}

int main() {
  test_sha1();
  test_shortcut_names();
  test_nand_shortcuts();
  test_nand_state();
  test_fat12();
  test_synthetic_nand();
  test_region();
  test_builtin_font();
  test_real_nand();
  test_persist();
  if (failures) { std::fprintf(stderr, "nand_fs: %d failure(s)\n", failures); return 1; }
  std::printf("nand_fs: ok\n");
  return 0;
}
