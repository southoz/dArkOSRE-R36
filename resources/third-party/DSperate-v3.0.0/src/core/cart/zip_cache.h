// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// A zipped ROM as a RomSource without holding it in memory.
//
// A stored entry is mapped where it lies. A deflated one is inflated once to
// a file beside the archive (`<dir>/.dsperate/<zip stem>.nds`) and kept, then
// mapped. `/tmp` is never used: on target handhelds it is a RAM-backed tmpfs.
//
// A tag file beside the cached image records the archive's size/mtime and
// the entry's name/CRC/size; a mismatch rebuilds it. The image is written to
// `.part` and renamed into place, so a power cut mid-extraction leaves
// nothing mistakable for a ROM, and the CRC is checked as bytes go out.
#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "core/cart/rom_source.h"
#include "core/cart/zip.h"

namespace ds::cart {

struct ZipOpen {
  std::string fallback_dir;   // used when the archive's own directory isn't writable; empty = fail
  u64 max_bytes = 0;          // cache directory cap before LRU eviction (0: no limit); image being opened is never evicted
  ZipProgress progress = nullptr;   // called during extraction with bytes done/total; null for none
  void* progress_user = nullptr;
  std::atomic<bool>* cancel = nullptr;   // set to abandon extraction; open_zip fails with "cancelled"
  // Filled in on success.
  std::string chosen;         // the entry's name
  std::string cache_path;     // the extracted image, empty when mapped in place
  bool extracted = false;     // true when the image was (re)built this time
};

// Opens `path` (an archive) as a RomSource. Null with a reason in `err`.
std::unique_ptr<RomSource> open_zip(const std::string& path, ZipOpen& how, std::string& err);

// Where open_zip would put (or find) the extracted image for `path`.
std::string zip_cache_path(const std::string& zip_path, const std::string& dir_override = {});

// The cache directory for archives in `dir` (its `.dsperate`).
std::string zip_cache_dir(const std::string& dir);

// `stamp` is when the image was last launched (the tag's mtime).
struct CacheEntry { std::string image, tag, archive; u64 bytes = 0; long stamp = 0; };
std::vector<CacheEntry> list_cache(const std::string& dir);

// Removes images whose archive no longer exists, images with no tag, and
// leftover `.part` files. Returns bytes freed.
u64 sweep_cache(const std::string& dir);

// Removes every image in `dir` except `keep_image` (may be empty). Returns bytes freed.
u64 clear_cache(const std::string& dir, const std::string& keep_image);

void remove_cached(const std::string& image);

} // namespace ds::cart
