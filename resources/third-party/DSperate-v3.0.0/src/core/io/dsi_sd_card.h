// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// The DSi's SD card slot, backed by a folder on the host (`--dsi-sd DIR` / `paths.dsi_sd`).
//
// The folder is never copied into an image. At boot the card is built in memory: an MBR, one
// FAT16/32 partition, and the folder's directory tree, with clusters allocated but unfilled. An
// unwritten data sector reads from the host file that owns it; guest writes are held in memory.
//
// sync() carries guest changes back: new/changed files are written (temp name, renamed over),
// removed files/dirs are removed. A host file that changed on the host since the card was built
// is never overwritten or deleted; it is reported and the card keeps its own copy.
//
// Card size: folder size + 128 MB, rounded to a power of two; FAT32 from 1 GB. At most 32 GB.
#pragma once
#include "core/types.h"
#include "core/io/dsi_nand_fs.h"
#include "core/io/dsi_sd.h"

#include <array>
#include <istream>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ds::io {

class SdCard : public BlockStorage {
 public:
  static constexpr u8 kCid[16] = {0xBD, 0x12, 0x34, 0x56, 0x78, 0x03, 0x4D, 0x30, 0x30, 0x46, 0x50, 0x41, 0x00, 0x00, 0x15, 0x00};
  static constexpr u64 kMaxBytes = 32ull << 30;
  static constexpr u64 kPartitionStart = 8192;   // sectors before the partition

  struct Report {
    int files = 0, dirs = 0;        // open: imported; sync: written / created
    int removed = 0;                // sync: host files and directories removed
    std::vector<std::string> notes; // skipped entries and conflicts, one line each
  };

  SdCard() = default;
  ~SdCard() override;
  SdCard(const SdCard&) = delete;
  SdCard& operator=(const SdCard&) = delete;

  // Builds the card from `dir` (created if missing).
  bool open(const std::string& dir, Report* report = nullptr, std::string* err = nullptr);
  void close();
  bool valid() const { return length_ != 0; }
  const std::string& folder() const { return dir_; }
  u64 length() const { return length_; }
  int fat_bits() const { return fat_bits_; }
  bool any_changed() const { return !changed_.empty(); }

  // Carries guest changes out to the folder. Cheap when nothing was written.
  Report sync();

  // BlockStorage: the guest's view. read/write count and (DS_SD_LOG=<file>) log.
  void read(u64 addr, u32 len, u8* out) override;
  void write(u64 addr, u32 len, const u8* in) override;
  const u8* cid() const override { return kCid; }
  // Same, for the emulator's own filesystem work: not counted, not logged, no guest change.
  void peek(u64 addr, u32 len, u8* out);
  void poke(u64 addr, u32 len, const u8* in);

  // The card's file data stays in the host folder, so a state holds only what the card keeps in
  // memory plus which host files back the rest, as they were then. Loads onto this session's
  // folder only while those files are unchanged (state_matches); else loaded without a card.
  struct StateSnapshot {
    bool present = false;
    u64 length = 0, part_base = 0;
    s32 fat_bits = 0;
    std::vector<u64> sectors;                 // ascending
    std::vector<u8> data;                     // 512 bytes per sector
    std::vector<u64> changed;                 // ascending
    std::vector<u64> ext_start, ext_len, ext_file_off;
    std::vector<u32> ext_file;
    std::vector<std::string> files;           // relative to the folder
    std::vector<std::string> known_key, known_host;
    std::vector<u8> known_dir;
    std::vector<u64> known_size;
    std::vector<s64> known_mtime, known_mtime_ns;
  };
  StateSnapshot state_snapshot() const;
  // Whether this card's folder still holds the host files `snap` reads from;
  // `why` names the first that does not.
  bool state_matches(const StateSnapshot& snap, std::string* why) const;
  void apply_state_snapshot(const StateSnapshot& snap);

  // The whole card as an image file (for inspection).
  bool dump(const std::string& path);

  u64 reads = 0, writes = 0;

 private:
  // A run of the card backed by a host file.
  struct Extent {
    u64 start = 0, len = 0;   // card byte range
    u32 file = 0;             // index into files_
    u64 file_off = 0;
  };
  // What the folder held for a card path, as of the last build or sync.
  struct Known {
    std::string host;         // the host path relative to the folder, '/'-separated, the host's spelling
    bool dir = false;
    u64 size = 0;
    s64 mtime = 0;            // host modification time, seconds
    s64 mtime_ns = 0;
  };

  void read_sector(u64 sector, u8* out);
  void read_backing(u64 addr, u8* out);   // one sector, from the host file backing it (zero if none)
  const Extent* extent_at(u64 addr) const;
  bool is_backed(u64 addr) const { return extent_at(addr) != nullptr; }
  std::istream* host_file(u32 index);
  bool mount(FatVolume& v, std::string* why);   // the partition the MBR names; sets part_base_
  void map_file(const FatVolume& v, const std::string& host_rel, const FatVolume::Entry& e);
  bool host_unchanged(const Known& k) const;

  std::string dir_;
  u64 length_ = 0;
  u64 part_base_ = 0;
  int fat_bits_ = 0;
  std::unordered_map<u64, std::array<u8, 512>> sectors_;   // sector index -> contents
  std::unordered_set<u64> changed_;                         // guest writes since the last sync
  std::vector<Extent> extents_;                             // sorted by start, not overlapping
  std::vector<std::string> files_;                          // host paths (absolute)
  std::map<std::string, Known> known_;                      // key: card path, upper case
  u32 open_index_ = ~0u;
  std::unique_ptr<std::istream> host_in_;   // the last host file read
};

}  // namespace ds::io
