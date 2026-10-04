// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// The emulator's own view of the DSi NAND filesystem, for reading dump files and writing into
// the session's in-memory sectors. The guest never goes through here; it reads raw sectors from
// the SD host.
//
// Two layers: FatVolume is FAT12/16/32 over any byte-addressed device (NAND partitions, a
// title's public.sav, or the SD card). NandFs is the NAND itself: the MBR, the AES-CTR every
// sector is encrypted with (key from the console ID, counter from the eMMC CID's SHA-1), and
// the ES encryption tickets carry.
#pragma once
#include "core/types.h"

#include <array>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

extern "C" {
#include "core/crypto/aes.h"
}

namespace ds::io {

class NandImage;

class FatVolume {
 public:
  using ReadFn = std::function<void(u64 offset, u32 len, u8* out)>;
  using WriteFn = std::function<void(u64 offset, u32 len, const u8* in)>;

  struct Entry {
    std::string name;      // "PUBLIC.SAV", as stored (8.3, upper case)
    std::string long_name; // "TWLFontTable.dat" (UTF-8) when a long-name entry precedes it, else empty
    u8  attr = 0;          // 0x10 directory
    u32 cluster = 0;       // first cluster; 0 for an empty file, and for the root except on FAT32
    u32 size = 0;
    u64 dirent = 0;        // byte offset of the 32-byte entry (0 for the root)
    u16 mdate = 0, mtime = 0;   // last modified, FAT encoding
    bool dir() const { return attr & 0x10; }
    const std::string& display_name() const { return long_name.empty() ? name : long_name; }
  };

  // FAT timestamp for entries a write creates/updates; default 2000-01-01 00:00.
  struct Stamp { u16 date = 0, time = 0; };

  // `read`/`write` address the device in bytes from the volume's boot sector, whole sectors only.
  bool open(ReadFn read, WriteFn write, std::string* err = nullptr);
  bool valid() const { return bps_ != 0; }
  int  fat_bits() const { return fat_bits_; }
  u32  cluster_bytes() const { return bps_ * spc_; }
  u32  free_clusters() const { return free_count_; }
  u32  cluster_count() const { return clusters_; }
  Entry root() const;

  // Writes an empty volume through `write`; layout computed from the size, `fat_bits` must suit
  // the resulting cluster count. `zeroed`: device already reads zero, so all-zero sectors are
  // skipped.
  struct FormatSpec {
    u64 sectors = 0;          // the volume's length in 512-byte sectors
    u32 hidden = 0;           // sectors before it (its partition's start)
    int fat_bits = 16;        // 16 or 32
    u32 sectors_per_cluster = 32;
    const char* oem = "DSPERATE";
    const char* label = "NO NAME";
    u32 serial = 0x12345678;
    bool zeroed = false;
  };
  static bool format(const WriteFn& write, const FormatSpec& spec, std::string* err = nullptr);

  // Names created on this volume keep their case, getting long-name entries when not 8.3. Off
  // for the NAND, whose files are all upper-case 8.3.
  void set_preserve_case(bool on) { preserve_case_ = on; }

  // Paths are '/'-separated from the root, matched case-insensitively against short and long
  // names. A non-8.3 write gets a NAME~N.EXT short name plus long-name entries.
  bool lookup(const std::string& path, Entry& out) const;
  std::vector<Entry> list(const Entry& dir) const;
  bool read(const Entry& file, std::vector<u8>& out) const;
  // `len` bytes of a file from `offset`, reading only the sectors that hold them; false past end.
  bool read_part(const Entry& file, u64 offset, u32 len, u8* out) const;

  // Replaces a file's contents (chain grown/trimmed), or creates it in its parent (must exist).
  bool write(const std::string& path, const u8* data, u32 len, std::string* err = nullptr, const Stamp* stamp = nullptr);
  // Same without the data: entry plus a `len`-byte chain, clusters left as the device has them
  // (for contents living elsewhere, e.g. the SD card's host files); `out` is the new entry.
  bool create(const std::string& path, u32 len, Entry* out, std::string* err = nullptr, const Stamp* stamp = nullptr);
  // Creates a directory (parent must exist); true if it already exists.
  bool mkdir(const std::string& path, std::string* err = nullptr, const Stamp* stamp = nullptr);

  // Bulk-fills a directory with new entries in one pass (avoids re-reading the directory per
  // entry). Files get a `size`-byte chain, clusters as the device has them; directories get one
  // cluster holding "." and "..". An item that cannot be made is left with ok false and why set;
  // the call fails only if entries don't fit the directory.
  struct NewEntry {
    std::string name;
    bool dir = false;
    u32 size = 0;
    Stamp stamp;
    bool ok = false;
    std::string why;
    Entry out;
  };
  bool populate(const Entry& dir, std::vector<NewEntry>& items, std::string* err = nullptr);
  // Set when unused clusters already read zero (freshly formatted onto a zeroed device): new
  // directory clusters then skip their zero fill.
  void set_fresh(bool on) { fresh_ = on; }
  u64 data_offset() const { return data_off_; }   // cluster 2's byte offset in the volume
  // Deletes a file, or a directory with everything in it; true if already absent.
  bool remove(const std::string& path, std::string* err = nullptr);

  // Every file and directory below the root, parents before children.
  void walk(const std::function<void(const std::string& path, const Entry&)>& fn) const;
  // Volume-relative byte offset of each of a file's clusters (cluster_bytes() long).
  std::vector<u64> extents(const Entry& file) const;

 private:
  u32  fat_get(u32 c) const;
  void fat_set(u32 c, u32 v);
  void fat_flush();
  u32  eoc_min() const { return fat_bits_ == 12 ? 0xFF8 : fat_bits_ == 16 ? 0xFFF8 : 0x0FFFFFF8; }
  u32  eoc_mark() const { return fat_bits_ == 12 ? 0xFFF : fat_bits_ == 16 ? 0xFFFF : 0x0FFFFFFF; }
  bool alloc_chain(u32 count, u32& first);
  void free_chain(u32 first);
  std::vector<u32> chain(u32 first) const;
  u64  cluster_offset(u32 c) const { return data_off_ + static_cast<u64>(c - 2) * bps_ * spc_; }
  u32  entry_cluster(const u8* e) const;
  void set_entry_cluster(u8* e, u32 c) const;
  std::vector<u8> dir_bytes(const Entry& dir) const;
  // Stores `count` consecutive 32-byte entries (long-name, then short); `dirent_out` is the
  // last's offset.
  bool add_entry(const Entry& parent, const u8* raw, u32 count, u64& dirent_out);
  // Short name plus long-name entries for `name` in `parent`; 1 entry if already 8.3.
  bool make_entries(const Entry& parent, const std::string& name, std::vector<u8>& raws, std::string* why) const;
  bool make_entries_in(const std::unordered_set<std::string>& taken, const std::string& name, std::vector<u8>& raws, std::string* why) const;
  void write_dir_cluster(u32 cluster, u32 parent_cluster, const Stamp* stamp);
  void write_entry(u64 dirent, const u8 raw[32]);
  bool split(const std::string& path, Entry& parent, std::string& name, std::string* err) const;
  bool put(const std::string& path, const u8* data, u32 len, bool with_data, Entry* out, std::string* err, const Stamp* stamp);
  static bool to_83(const std::string& name, u8 out[11]);

  ReadFn read_;
  WriteFn write_;
  u32 bps_ = 0, spc_ = 0, clusters_ = 0;
  int fat_bits_ = 0;
  u32 nfats_ = 0, fat_sectors_ = 0, root_entries_ = 0, root_cluster_ = 0;
  u64 fat_off_ = 0, root_off_ = 0, data_off_ = 0;
  std::vector<u8> fat_;             // the first FAT, cached; written to every copy
  std::vector<bool> fat_dirty_;     // per FAT sector
  u32 free_count_ = 0;              // clusters whose FAT entry is 0
  u32 next_free_ = 2;               // where the next allocation starts looking
  bool preserve_case_ = false;
  bool fresh_ = false;
};

class NandFs {
 public:
  // `bios7i` (64 KB) supplies the ES key's KeyY at 0x8308; without it, tickets cannot be made
  // or read, but everything else works.
  bool mount(NandImage& nand, const u8* bios7i, std::string* err = nullptr);
  // Writes an empty DSi filesystem over `nand` and mounts it: MBR plus two FAT16 partitions
  // with a retail DSi's geometry, encrypted under the image's console ID and CID. `nand` must
  // be at least kImageBytes long.
  static constexpr u64 kImageBytes = 0xF000000;
  bool format(NandImage& nand, const u8* bios7i, std::string* err = nullptr);
  bool valid() const { return main_.valid(); }

  FatVolume& main() { return main_; }     // partition 0: title/, ticket/, shared1/, sys/
  FatVolume& photo() { return photo_; }   // partition 1: the camera's photos (may be invalid)
  u64 main_base() const { return main_base_; }     // each partition's byte offset in the image
  u64 photo_base() const { return photo_base_; }

  // `data` holds `len` bytes followed by a 0x20-byte MAC + footer area; decrypt fails on a bad MAC.
  bool has_es_key() const { return es_key_ok_; }
  void es_encrypt(u8* data, u32 len, const u8 nonce[12]) const;
  bool es_decrypt(u8* data, u32 len) const;

 private:
  void setup_crypto(NandImage& nand, const u8* bios7i);
  void crypt_read(u64 offset, u32 len, u8* out);
  void crypt_write(u64 offset, u32 len, const u8* in);
  void xcrypt(u64 offset, u8* buf, u32 len) const;

  NandImage* nand_ = nullptr;
  AES_ctx fat_ctx_{};
  u8 fat_iv_[16] = {};              // counter base, big-endian
  u8 es_key_[16] = {};
  bool es_key_ok_ = false;
  FatVolume main_, photo_;
  u64 main_base_ = 0, photo_base_ = 0;
};

}  // namespace ds::io
