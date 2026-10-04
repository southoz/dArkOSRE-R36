// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// The DSi SD card (io/dsi_sd_card) and the FAT32 half of FatVolume: a FAT32
// volume formatted in memory; long, UTF-8 and case-preserved names; the bulk
// populate; and a card built from a scratch folder, changed the way a guest
// changes it (through the card's own sector interface), then synced back:
// written, new, removed and conflicting files, the backing rebuilt after the
// sync, and a FAT32 card from a sparse 1 GB file.
#include "core/io/dsi_nand_fs.h"
#include "core/io/dsi_sd_card.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <unistd.h>
#include <vector>

using namespace ds;
using ds::io::FatVolume;
using ds::io::SdCard;
namespace fs = std::filesystem;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static std::vector<u8> pattern(size_t n, u32 seed) {
  std::vector<u8> v(n);
  for (size_t i = 0; i < n; ++i) { seed = seed * 1103515245u + 12345u; v[i] = static_cast<u8>(seed >> 16); }
  return v;
}

static void put_file(const fs::path& p, const std::vector<u8>& data) {
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

static std::vector<u8> get_file(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::vector<u8>(std::istreambuf_iterator<char>(f), {});
}

static std::vector<u8> bytes(const std::string& s) { return std::vector<u8>(s.begin(), s.end()); }

// A sparse in-memory device.
struct MemDev {
  std::map<u64, std::vector<u8>> sec;
  void read(u64 o, u32 n, u8* out) {
    for (u32 i = 0; i < n; i += 512) {
      auto it = sec.find((o + i) / 512);
      if (it == sec.end()) std::memset(out + i, 0, 512); else std::memcpy(out + i, it->second.data(), 512);
    }
  }
  void write(u64 o, u32 n, const u8* in) { for (u32 i = 0; i < n; i += 512) sec[(o + i) / 512].assign(in + i, in + i + 512); }
};

static void test_fat32() {
  MemDev dev;
  FatVolume::FormatSpec spec;
  spec.sectors = (1ull << 30) / 512;   // 1 GB
  spec.fat_bits = 32;
  spec.sectors_per_cluster = 8;
  spec.zeroed = true;
  std::string err;
  CHECK(FatVolume::format([&](u64 o, u32 n, const u8* in) { dev.write(o, n, in); }, spec, &err));
  FatVolume v;
  CHECK(v.open([&](u64 o, u32 n, u8* out) { dev.read(o, n, out); }, [&](u64 o, u32 n, const u8* in) { dev.write(o, n, in); }, &err));
  CHECK(v.fat_bits() == 32);
  CHECK(v.cluster_bytes() == 4096);
  const u32 free0 = v.free_clusters();
  CHECK(free0 == v.cluster_count() - 1);   // the root's cluster
  v.set_preserve_case(true);

  CHECK(v.mkdir("/private"));
  CHECK(v.mkdir("/private/ds"));
  const std::vector<u8> big = pattern(100000, 7);
  CHECK(v.write("/private/ds/save data.bin", big.data(), static_cast<u32>(big.size())));
  CHECK(v.write("/readme.txt", reinterpret_cast<const u8*>("hi"), 2));
  const std::string jp = "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E.txt";   // 日本語.txt
  CHECK(v.write("/" + jp, reinterpret_cast<const u8*>("jp"), 2));
  CHECK(v.free_clusters() == free0 - 2 - 25 - 1 - 1);

  FatVolume::Entry e;
  CHECK(v.lookup("/PRIVATE/DS/SAVE DATA.BIN", e));
  std::vector<u8> back;
  CHECK(v.read(e, back) && back == big);
  CHECK(v.lookup("/readme.txt", e) && e.long_name == "readme.txt" && e.name == "README.TXT");
  CHECK(v.lookup("/private", e) && e.display_name() == "private");
  CHECK(v.lookup("/" + jp, e) && e.long_name == jp);

  // A re-open reads what was written, with the same free count.
  FatVolume v2;
  CHECK(v2.open([&](u64 o, u32 n, u8* out) { dev.read(o, n, out); }, [&](u64 o, u32 n, const u8* in) { dev.write(o, n, in); }));
  CHECK(v2.free_clusters() == v.free_clusters());
  CHECK(v2.lookup("/private/ds/save data.bin", e) && v2.read(e, back) && back == big);
  // ".." in a first-level directory names the root as cluster 0.
  CHECK(v2.lookup("/private", e));
  bool dotdot = false;
  for (const FatVolume::Entry& c : v2.list(e)) if (c.name == ".." && c.cluster == 0) dotdot = true;
  CHECK(dotdot);

  // Many names that share a short-name stem, in one populate.
  CHECK(v2.lookup("/private/ds", e));
  std::vector<FatVolume::NewEntry> items;
  for (int i = 0; i < 40; ++i) {
    FatVolume::NewEntry n;
    n.name = "Pokemon Version " + std::to_string(i) + ".nds";
    n.size = 5000;
    items.push_back(n);
  }
  CHECK(v2.populate(e, items, &err));
  for (const auto& n : items) CHECK(n.ok);
  CHECK(v2.lookup("/private/ds/pokemon version 39.nds", e) && e.size == 5000 && e.cluster == items[39].out.cluster);
  CHECK(v2.lookup("/private/ds/save data.bin", e) && v2.read(e, back) && back == big);

  CHECK(v2.remove("/private"));
  CHECK(!v2.lookup("/private", e));
}

// The guest's view of a card: FatVolume over its sector interface, so every
// write counts as the guest's.
static bool guest_mount(SdCard& card, FatVolume& v) {
  u8 mbr[512];
  card.read(0, 512, mbr);
  const u64 base = (static_cast<u64>(mbr[0x1C6]) | (mbr[0x1C7] << 8) | (mbr[0x1C8] << 16) | (static_cast<u64>(mbr[0x1C9]) << 24)) * 512;
  return v.open([&card, base](u64 o, u32 n, u8* out) { card.read(base + o, n, out); },
                [&card, base](u64 o, u32 n, const u8* in) { card.write(base + o, n, in); });
}

static void test_card(const fs::path& dir) {
  const std::vector<u8> photo = pattern(100000, 1), big = pattern(3 << 20, 2);
  put_file(dir / "readme.txt", bytes("host readme"));
  put_file(dir / "Photos" / "IMG_0001.JPG", photo);
  put_file(dir / "big.bin", big);
  put_file(dir / "keep.txt", bytes("keep"));
  put_file(dir / "gone.txt", bytes("gone"));
  put_file(dir / "Empty" / "Nested" / "x.txt", bytes("x"));
  put_file(dir / "\xE6\x97\xA5\xE6\x9C\xAC.txt", bytes("jp"));

  SdCard card;
  SdCard::Report rep;
  std::string err;
  CHECK(card.open(dir.string(), &rep, &err));
  CHECK(rep.files == 7 && rep.dirs == 3 && rep.notes.empty());
  CHECK(card.length() == 256ull << 20 && card.fat_bits() == 16);
  CHECK(card.sync().files == 0);   // nothing written yet

  FatVolume v;
  CHECK(guest_mount(card, v));
  FatVolume::Entry e;
  std::vector<u8> back;
  CHECK(v.lookup("/big.bin", e) && v.read(e, back) && back == big);
  CHECK(v.lookup("/photos/img_0001.jpg", e) && v.read(e, back) && back == photo);
  CHECK(v.lookup("/readme.txt", e) && e.display_name() == "readme.txt");
  CHECK(v.lookup("/\xE6\x97\xA5\xE6\x9C\xAC.txt", e) && v.read(e, back) && back == bytes("jp"));

  // The guest: rewrites a file, changes one byte in the middle of another,
  // adds a directory and a file, removes a file; meanwhile the host changes
  // keep.txt, which the guest rewrites too.
  CHECK(v.write("/readme.txt", reinterpret_cast<const u8*>("card readme!"), 12));
  std::vector<u8> big2 = big;
  big2[1500000] ^= 0xFF;
  CHECK(v.write("/big.bin", big2.data(), static_cast<u32>(big2.size())));
  CHECK(v.mkdir("/New Dir"));
  CHECK(v.write("/New Dir/data file.dat", reinterpret_cast<const u8*>("new"), 3));
  CHECK(v.remove("/gone.txt"));
  put_file(dir / "keep.txt", bytes("changed on the host"));
  CHECK(v.write("/keep.txt", reinterpret_cast<const u8*>("card keep"), 9));
  CHECK(card.any_changed());

  rep = card.sync();
  CHECK(rep.files == 3);     // readme, big, data file
  CHECK(rep.dirs == 1);
  CHECK(rep.removed == 1);
  CHECK(rep.notes.size() == 1 && rep.notes[0].find("keep.txt") != std::string::npos);
  CHECK(get_file(dir / "readme.txt") == bytes("card readme!"));
  CHECK(get_file(dir / "big.bin") == big2);
  CHECK(get_file(dir / "New Dir" / "data file.dat") == bytes("new"));
  CHECK(!fs::exists(dir / "gone.txt"));
  CHECK(get_file(dir / "keep.txt") == bytes("changed on the host"));
  CHECK(get_file(dir / "Photos" / "IMG_0001.JPG") == photo);
  for (const auto& p : fs::recursive_directory_iterator(dir)) CHECK(p.path().string().find(".dsperate-sync") == std::string::npos);

  // After the sync the card reads its files from the host again, and keeps
  // its own keep.txt.
  CHECK(guest_mount(card, v));
  CHECK(v.lookup("/big.bin", e) && v.read(e, back) && back == big2);
  CHECK(v.lookup("/readme.txt", e) && v.read(e, back) && back == bytes("card readme!"));
  CHECK(v.lookup("/keep.txt", e) && v.read(e, back) && back == bytes("card keep"));
  CHECK(v.lookup("/photos/IMG_0001.JPG", e) && v.read(e, back) && back == photo);
  CHECK(card.sync().files == 0);

  // A second round: a directory tree removed, a file grown past its chain.
  CHECK(v.remove("/Empty"));
  const std::vector<u8> photo2 = pattern(300000, 3);
  CHECK(v.write("/Photos/IMG_0001.JPG", photo2.data(), static_cast<u32>(photo2.size())));
  rep = card.sync();
  CHECK(rep.files == 1 && rep.removed == 3);   // x.txt, Nested, Empty
  CHECK(!fs::exists(dir / "Empty"));
  CHECK(get_file(dir / "Photos" / "IMG_0001.JPG") == photo2);
  CHECK(guest_mount(card, v));
  CHECK(v.lookup("/photos/img_0001.jpg", e) && v.read(e, back) && back == photo2);

  // A fresh build of the synced folder sees the same tree.
  SdCard again;
  CHECK(again.open(dir.string(), &rep, &err));
  CHECK(rep.files == 6 && rep.dirs == 2);
  FatVolume w;
  CHECK(guest_mount(again, w));
  CHECK(w.lookup("/New Dir/data file.dat", e) && w.read(e, back) && back == bytes("new"));
  CHECK(w.lookup("/keep.txt", e) && w.read(e, back) && back == bytes("changed on the host"));
}

static void test_card_fat32(const fs::path& dir) {
  // A sparse 1 GB file: a 2 GB FAT32 card without writing a gigabyte.
  fs::create_directories(dir);
  { std::ofstream(dir / "huge.bin", std::ios::binary); }
  fs::resize_file(dir / "huge.bin", 1ull << 30);
  {
    std::fstream f(dir / "huge.bin", std::ios::binary | std::ios::in | std::ios::out);
    f.seekp((1ll << 30) - 4);
    f.write("TAIL", 4);
  }
  SdCard card;
  std::string err;
  CHECK(card.open(dir.string(), nullptr, &err));
  CHECK(card.length() == 2ull << 30 && card.fat_bits() == 32);
  FatVolume v;
  CHECK(guest_mount(card, v));
  CHECK(v.fat_bits() == 32);
  FatVolume::Entry e;
  CHECK(v.lookup("/HUGE.BIN", e) && e.size == (1u << 30));
  // The last cluster, read the way the guest reads it.
  const std::vector<u64> ex = v.extents(e);
  CHECK(ex.size() == (1u << 30) / v.cluster_bytes());
  u8 mbr[512], sec[512];
  card.read(0, 512, mbr);
  const u64 base = static_cast<u64>(mbr[0x1C6] | (mbr[0x1C7] << 8)) * 512;
  card.read(base + ex.back() + v.cluster_bytes() - 512, 512, sec);
  CHECK(std::memcmp(sec + 508, "TAIL", 4) == 0);
  // A new file on the FAT32 card syncs out.
  CHECK(v.write("/new.txt", reinterpret_cast<const u8*>("32"), 2));
  CHECK(card.sync().files == 1);
  // The guest writes a plain 8.3 entry, so the host file is named as stored.
  CHECK(get_file(dir / "NEW.TXT") == bytes("32"));
}

// Save states: a snapshot brings back the card's contents as they were, onto
// the same folder, and stops matching once a file it reads from changes.
static void test_card_state(const fs::path& dir) {
  fs::create_directories(dir);
  const std::vector<u8> big = pattern(700000, 9);
  put_file(dir / "a.txt", bytes("host a"));
  put_file(dir / "b.bin", big);
  SdCard card;
  std::string err;
  CHECK(card.open(dir.string(), nullptr, &err));
  FatVolume v;
  CHECK(guest_mount(card, v));
  CHECK(v.write("/a.txt", reinterpret_cast<const u8*>("one"), 3));
  const SdCard::StateSnapshot snap = card.state_snapshot();
  CHECK(snap.present && snap.files.size() == 2);
  CHECK(v.write("/a.txt", reinterpret_cast<const u8*>("two!"), 4));
  CHECK(v.write("/c.txt", reinterpret_cast<const u8*>("c"), 1));

  std::string why;
  CHECK(card.state_matches(snap, &why));
  card.apply_state_snapshot(snap);
  CHECK(guest_mount(card, v));
  FatVolume::Entry e;
  std::vector<u8> back;
  CHECK(v.lookup("/a.txt", e) && v.read(e, back) && back == bytes("one"));
  CHECK(!v.lookup("/c.txt", e));
  CHECK(v.lookup("/b.bin", e) && v.read(e, back) && back == big);
  // Saved again straight away, it is the same snapshot.
  const SdCard::StateSnapshot again = card.state_snapshot();
  CHECK(again.sectors == snap.sectors && again.data == snap.data && again.changed == snap.changed && again.known_key == snap.known_key);

  // The loaded card's unsynced write goes out like any other; a.txt then
  // no longer backs the snapshot as it was.
  CHECK(card.sync().files == 1);
  CHECK(get_file(dir / "a.txt") == bytes("one"));
  SdCard fresh;
  CHECK(fresh.open(dir.string(), nullptr, &err));
  CHECK(!fresh.state_matches(snap, &why) && why.find("a.txt") != std::string::npos);
  SdCard none;
  CHECK(!none.state_matches(snap, &why));
  CHECK(!card.state_matches(SdCard::StateSnapshot{}, &why));
}

int main() {
  test_fat32();
  const fs::path tmp = fs::temp_directory_path() / ("dsperate-sd-test-" + std::to_string(getpid()));
  fs::remove_all(tmp);
  test_card(tmp / "card");
  test_card_fat32(tmp / "card32");
  test_card_state(tmp / "cardstate");
  fs::remove_all(tmp);
  if (failures) { std::fprintf(stderr, "sd_card: %d failures\n", failures); return 1; }
  std::fprintf(stderr, "sd_card: ok\n");
  return 0;
}
