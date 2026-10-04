// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/cart/zip_cache.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

namespace ds::cart {
namespace {

std::string dir_of(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? "." : (slash == 0 ? "/" : path.substr(0, slash));
}
std::string stem_of(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  std::string base = slash == std::string::npos ? path : path.substr(slash + 1);
  const size_t dot = base.find_last_of('.');
  return dot == std::string::npos || dot == 0 ? base : base.substr(0, dot);
}

// Canonical path for the tag: the sweep must find the archive an image came
// from, and the fallback directory holds images from anywhere.
std::string canonical(const std::string& path) {
  char buf[PATH_MAX];
  if (const char* r = realpath(path.c_str(), buf)) return r;
  return path;
}

// What the cached image was built from; a match means the file beside it is
// still that entry's bytes.
std::string make_tag(const std::string& archive, const struct stat& zip_st, const ZipEntry& e) {
  std::ostringstream o;
  o << "dsperate zip cache 2\n"
    << "archive " << archive << '\n'
    << "zip " << static_cast<long long>(zip_st.st_size) << ' ' << static_cast<long long>(zip_st.st_mtime) << '\n'
    << "entry " << e.name << '\n'
    << "crc " << e.crc32 << " size " << e.usize << '\n';
  return o.str();
}

// The "archive" line of a tag, or empty.
std::string tag_archive(const std::string& tag) {
  const size_t at = tag.find("\narchive ");
  if (at == std::string::npos) return {};
  const size_t start = at + 9, end = tag.find('\n', start);
  return tag.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

bool exists(const std::string& p) { struct stat st{}; return stat(p.c_str(), &st) == 0; }

bool read_file(const std::string& path, std::string& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  std::ostringstream o; o << f.rdbuf();
  out = o.str();
  return true;
}

bool write_file(const std::string& path, const std::string& text) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) return false;
  f << text;
  return static_cast<bool>(f);
}

bool file_size(const std::string& path, u64& size) {
  struct stat st{};
  if (stat(path.c_str(), &st) != 0) return false;
  size = static_cast<u64>(st.st_size);
  return true;
}

// Beside the archive if writable, else the override. `path` comes back as the image path.
bool choose_cache(const std::string& zip_path, const std::string& fallback, std::string& path, std::string& err) {
  for (const std::string& dir : {dir_of(zip_path) + "/.dsperate", fallback}) {
    if (dir.empty()) continue;
    mkdir(dir.c_str(), 0755);   // may exist; the access check below is the test
    if (access(dir.c_str(), W_OK | X_OK) == 0) { path = dir + "/" + stem_of(zip_path) + ".nds"; return true; }
  }
  err = "cannot write beside " + zip_path + " and no cache directory is configured";
  return false;
}

struct FileSink { FILE* f; std::atomic<bool>* cancel; };
bool file_sink(void* user, const u8* p, size_t n) {
  FileSink& s = *static_cast<FileSink*>(user);
  if (s.cancel && s.cancel->load()) return false;
  return std::fwrite(p, 1, n, s.f) == n;
}

u64 remove_entry(const CacheEntry& e) {
  std::remove(e.image.c_str());
  if (!e.tag.empty()) std::remove(e.tag.c_str());
  return e.bytes;
}

// Evicts least-recently-launched images until `incoming` bytes fit under
// `max_bytes`. `keep` is never evicted.
void enforce_cap(const std::string& dir, u64 max_bytes, u64 incoming, const std::string& keep) {
  if (!max_bytes) return;
  std::vector<CacheEntry> all = list_cache(dir);
  u64 total = incoming;
  for (const CacheEntry& e : all) total += e.bytes;
  std::sort(all.begin(), all.end(), [](const CacheEntry& a, const CacheEntry& b) { return a.stamp < b.stamp; });
  for (const CacheEntry& e : all) {
    if (total <= max_bytes) break;
    if (e.image == keep) continue;
    total -= remove_entry(e);
  }
}

} // namespace

std::string zip_cache_dir(const std::string& dir) { return dir + "/.dsperate"; }

std::vector<CacheEntry> list_cache(const std::string& dir) {
  std::vector<CacheEntry> out;
  DIR* d = opendir(dir.c_str());
  if (!d) return out;
  while (dirent* ent = readdir(d)) {
    const std::string name = ent->d_name;
    if (name.size() < 5 || name.compare(name.size() - 4, 4, ".nds") != 0) continue;
    CacheEntry e;
    e.image = dir + "/" + name;
    struct stat st{};
    if (stat(e.image.c_str(), &st) != 0) continue;
    e.bytes = static_cast<u64>(st.st_size);
    e.stamp = static_cast<long>(st.st_mtime);
    const std::string tag_path = e.image + ".tag";
    std::string tag;
    if (read_file(tag_path, tag)) { e.tag = tag_path; e.archive = tag_archive(tag); if (stat(tag_path.c_str(), &st) == 0) e.stamp = static_cast<long>(st.st_mtime); }
    out.push_back(std::move(e));
  }
  closedir(d);
  return out;
}

u64 sweep_cache(const std::string& dir) {
  u64 freed = 0;
  for (const CacheEntry& e : list_cache(dir)) {
    // No tag, or no archive recorded in it: not vouched for.
    if (e.tag.empty() || e.archive.empty() || !exists(e.archive)) freed += remove_entry(e);
  }
  if (DIR* d = opendir(dir.c_str())) {
    std::vector<std::string> parts;
    while (dirent* ent = readdir(d)) {
      const std::string name = ent->d_name;
      if (name.size() > 5 && name.compare(name.size() - 5, 5, ".part") == 0) parts.push_back(dir + "/" + name);
    }
    closedir(d);
    for (const std::string& p : parts) std::remove(p.c_str());
  }
  return freed;
}

u64 clear_cache(const std::string& dir, const std::string& keep_image) {
  u64 freed = 0;
  for (const CacheEntry& e : list_cache(dir)) if (e.image != keep_image) freed += remove_entry(e);
  return freed;
}

void remove_cached(const std::string& image) {
  if (image.empty()) return;
  std::remove(image.c_str());
  std::remove((image + ".tag").c_str());
}

std::string zip_cache_path(const std::string& zip_path, const std::string& dir_override) {
  const std::string dir = dir_override.empty() ? dir_of(zip_path) + "/.dsperate" : dir_override;
  return dir + "/" + stem_of(zip_path) + ".nds";
}

std::unique_ptr<RomSource> open_zip(const std::string& path, ZipOpen& how, std::string& err) {
  err.clear();
  how.chosen.clear(); how.cache_path.clear(); how.extracted = false;

  std::unique_ptr<RomSource> archive = RomSource::map_file(path, err);
  if (!archive) return nullptr;
  // RomSource pads to a page; the parser only reads inside `size`.
  const u8* zip = archive->page(0);
  const size_t size = archive->size();
  ZipEntry e;
  if (!find_rom(zip, size, e, err)) return nullptr;
  // A CIA must be unwrapped to its SRL first (io/dsi_title_install.h).
  if (e.cia) { err = cia_refusal(e.name); return nullptr; }
  how.chosen = e.name;

  if (e.stored()) {
    return RomSource::map_file(path, e.data_off, e.usize, err);
  }

  struct stat zst{};
  if (stat(path.c_str(), &zst) != 0) { err = std::strerror(errno); return nullptr; }
  const std::string tag = make_tag(canonical(path), zst, e);

  std::string image;
  if (!choose_cache(path, how.fallback_dir, image, err)) return nullptr;
  const std::string tag_path = image + ".tag", part = image + ".part";
  how.cache_path = image;
  sweep_cache(dir_of(image));

  std::string have; u64 have_size = 0;
  const bool fresh = read_file(tag_path, have) && have == tag && file_size(image, have_size) && have_size == e.usize;
  if (!fresh) {
    std::remove(tag_path.c_str());
    std::remove(image.c_str());
    enforce_cap(dir_of(image), how.max_bytes, e.usize, image);
    FILE* f = std::fopen(part.c_str(), "wb");
    if (!f) { err = "cannot write " + part + ": " + std::strerror(errno); return nullptr; }
    FileSink sink{f, how.cancel};
    const bool ok = inflate_entry(zip, size, e, file_sink, &sink, how.progress, how.progress_user, err);
    bool flushed = ok;
    if (ok) { flushed = std::fflush(f) == 0 && fsync(fileno(f)) == 0; if (!flushed) err = "cannot write " + part + ": " + std::strerror(errno); }
    std::fclose(f);
    if (!ok || !flushed) {
      std::remove(part.c_str());
      if (how.cancel && how.cancel->load()) err = "cancelled";
      return nullptr;
    }
    if (std::rename(part.c_str(), image.c_str()) != 0) { err = "cannot replace " + image + ": " + std::strerror(errno); std::remove(part.c_str()); return nullptr; }
    if (!write_file(tag_path, tag)) { err = "cannot write " + tag_path; return nullptr; }
    how.extracted = true;
  }
  utime(tag_path.c_str(), nullptr);   // launch stamp for LRU eviction
  archive.reset();
  return RomSource::map_file(image, err);
}

} // namespace ds::cart
