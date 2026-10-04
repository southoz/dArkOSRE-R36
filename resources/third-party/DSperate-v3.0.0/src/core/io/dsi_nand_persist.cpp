// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/io/dsi_nand_persist.h"
#include "core/io/dsi_nand_fs.h"
#include "core/io/dsi_sd.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>

namespace fs = std::filesystem;

namespace ds::io {
namespace {

constexpr char kSidecarMagic[8] = {'D', 'S', 'P', 'N', 'S', 'Y', 'S', '1'};

bool slurp(const std::string& path, std::vector<u8>& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  out.assign(std::istreambuf_iterator<char>(f), {});
  return true;
}

// Written beside itself and renamed over, so a crash mid-write never leaves a half file.
bool spill(const std::string& path, const u8* data, size_t len) {
  std::error_code ec;
  const fs::path p(path);
  if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
  const std::string tmp = path + ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(len));
    if (!f) return false;
  }
  fs::rename(tmp, path, ec);
  return !ec;
}

bool same_as_file(const std::string& path, const std::vector<u8>& data) {
  std::vector<u8> have;
  return slurp(path, have) && have == data;
}

std::string upper(std::string s) {
  for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

bool starts_with(const std::string& s, const char* prefix) { return s.compare(0, std::strlen(prefix), prefix) == 0; }

// "4B533345" -> "KS3E"; a title ID whose low half is not printable keeps its hex.
std::string game_code(const std::string& hex8) {
  std::string out;
  for (size_t i = 0; i + 1 < hex8.size() && out.size() < 4; i += 2) {
    const int v = std::stoi(hex8.substr(i, 2), nullptr, 16);
    if (v < 0x20 || v > 0x7E) return hex8;
    out += static_cast<char>(v);
  }
  return out.size() == 4 ? out : hex8;
}

// A title save's host extension, or null for any other file (/TITLE/00030004/<ID>/DATA/<NAME>).
const char* save_ext(const std::string& path) {
  if (!starts_with(path, "/TITLE/00030004/") || path.size() < 30 || path.compare(24, 6, "/DATA/") != 0) return nullptr;
  const std::string name = path.substr(30);
  if (name == "PUBLIC.SAV") return ".pub";
  if (name == "PRIVATE.SAV") return ".prv";
  if (name == "BANNER.SAV") return ".bnr";
  return nullptr;
}

// Excludes scratch space, title saves (own files), and virtual-install writes.
bool system_file(const std::string& path) {
  if (starts_with(path, "/TMP/") || starts_with(path, "/IMPORT/")) return false;
  if (starts_with(path, "/TICKET/00030004/")) return false;
  if (starts_with(path, "/TITLE/00030004/")) return false;
  return true;
}

// Whether the session wrote any of a file's data. Not its directory entry, since a size update
// there would mark every neighbour in the same sector; a new file is caught by its clusters.
bool touched(const NandImage& nand, u64 base, const FatVolume& vol, const FatVolume::Entry& e) {
  if (!nand.any_changed()) return false;
  for (u64 off : vol.extents(e))
    for (u64 s = (base + off) / 512, end = (base + off + vol.cluster_bytes()) / 512; s < end; ++s)
      if (nand.changed(s)) return true;
  return false;
}

bool mkdirs(FatVolume& vol, const std::string& file_path, std::string* err) {
  std::string at;
  size_t i = 1;
  while (true) {
    const size_t j = file_path.find('/', i);
    if (j == std::string::npos) return true;
    at = file_path.substr(0, j);
    if (!vol.mkdir(at, err)) return false;
    i = j + 1;
  }
}

std::map<std::string, std::vector<u8>> read_sidecar(const std::string& path) {
  std::map<std::string, std::vector<u8>> out;
  std::vector<u8> b;
  if (!slurp(path, b) || b.size() < 12 || std::memcmp(b.data(), kSidecarMagic, 8) != 0) return out;
  size_t p = 8;
  auto u32at = [&](size_t o) { return static_cast<u32>(b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (static_cast<u32>(b[o + 3]) << 24)); };
  const u32 n = u32at(p); p += 4;
  for (u32 k = 0; k < n; ++k) {
    if (p + 2 > b.size()) break;
    const size_t plen = b[p] | (b[p + 1] << 8); p += 2;
    if (p + plen + 4 > b.size()) break;
    std::string name(reinterpret_cast<const char*>(&b[p]), plen); p += plen;
    const u32 len = u32at(p); p += 4;
    if (p + len > b.size()) break;
    out[name].assign(b.begin() + static_cast<long>(p), b.begin() + static_cast<long>(p + len));
    p += len;
  }
  return out;
}

std::vector<u8> encode_sidecar(const std::map<std::string, std::vector<u8>>& files) {
  std::vector<u8> b(kSidecarMagic, kSidecarMagic + 8);
  auto put32 = [&](u32 v) { for (int i = 0; i < 4; ++i) b.push_back(static_cast<u8>(v >> (8 * i))); };
  put32(static_cast<u32>(files.size()));
  for (const auto& [name, data] : files) {
    b.push_back(static_cast<u8>(name.size())); b.push_back(static_cast<u8>(name.size() >> 8));
    b.insert(b.end(), name.begin(), name.end());
    put32(static_cast<u32>(data.size()));
    b.insert(b.end(), data.begin(), data.end());
  }
  return b;
}

}  // namespace

NandPersistPaths NandPersistPaths::beside(const std::string& nand_path, const std::string& saves_dir) {
  NandPersistPaths p;
  const fs::path n(nand_path);
  p.saves_dir = !saves_dir.empty() ? saves_dir : (n.has_parent_path() ? n.parent_path().string() : std::string("."));
  p.sidecar = nand_path + ".ovr";
  p.photos_dir = nand_path + ".photos";
  return p;
}

NandPersistReport nand_import(NandImage& nand, const u8* bios7i, const NandPersistPaths& paths) {
  NandPersistReport r;
  NandFs nfs;
  std::string err;
  if (!nfs.mount(nand, bios7i, &err)) { r.notes.push_back("nand: " + err); return r; }
  FatVolume& vol = nfs.main();

  // Replace with the host's copy only when it's the same size (else a different/broken save).
  std::vector<std::pair<std::string, FatVolume::Entry>> saves;
  vol.walk([&](const std::string& path, const FatVolume::Entry& e) { if (!e.dir() && save_ext(path)) saves.emplace_back(path, e); });
  for (const auto& [path, e] : saves) {
    const std::string host = paths.saves_dir + "/" + game_code(path.substr(16, 8)) + save_ext(path);
    std::vector<u8> data, cur;
    if (!slurp(host, data)) continue;
    if (data.size() != e.size) { r.notes.push_back(host + ": " + std::to_string(data.size()) + " bytes, the title's save is " + std::to_string(e.size) + "; left alone"); continue; }
    if (vol.read(e, cur) && cur == data) continue;
    if (vol.write(path, data.data(), static_cast<u32>(data.size()), &err)) r.saves++;
    else r.notes.push_back(err);
  }

  // The system sidecar.
  for (const auto& [path, data] : read_sidecar(paths.sidecar)) {
    FatVolume::Entry e;
    std::vector<u8> cur;
    if (vol.lookup(path, e) && !e.dir() && vol.read(e, cur) && cur == data) continue;
    if (mkdirs(vol, path, &err) && vol.write(path, data.data(), static_cast<u32>(data.size()), &err)) r.system_files++;
    else r.notes.push_back(err);
  }

  // Photos: every file under the folder, at the same path on the partition.
  std::error_code ec;
  if (nfs.photo().valid() && fs::is_directory(paths.photos_dir, ec)) {
    for (auto it = fs::recursive_directory_iterator(paths.photos_dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
      if (!it->is_regular_file(ec)) continue;
      const std::string rel = "/" + upper(fs::relative(it->path(), paths.photos_dir, ec).generic_string());
      std::vector<u8> data, cur;
      if (!slurp(it->path().string(), data)) continue;
      FatVolume::Entry e;
      if (nfs.photo().lookup(rel, e) && !e.dir() && nfs.photo().read(e, cur) && cur == data) continue;
      if (mkdirs(nfs.photo(), rel, &err) && nfs.photo().write(rel, data.data(), static_cast<u32>(data.size()), &err)) r.photos++;
      else r.notes.push_back("photo " + err);
    }
  }
  return r;
}

NandPersistReport nand_export(NandImage& nand, const u8* bios7i, const NandPersistPaths& paths) {
  NandPersistReport r;
  if (!nand.any_changed()) return r;
  NandFs nfs;
  std::string err;
  if (!nfs.mount(nand, bios7i, &err)) { r.notes.push_back("nand: " + err); return r; }
  FatVolume& vol = nfs.main();

  std::map<std::string, std::vector<u8>> sidecar = read_sidecar(paths.sidecar);
  bool sidecar_changed = false;
  vol.walk([&](const std::string& path, const FatVolume::Entry& e) {
    if (e.dir()) return;
    const char* ext = save_ext(path);
    const bool in_sidecar = !ext && system_file(path) && sidecar.count(path);
    if (!touched(nand, nfs.main_base(), vol, e) && !in_sidecar) return;
    std::vector<u8> data;
    if (!vol.read(e, data)) { r.notes.push_back(path + ": unreadable"); return; }
    if (ext) {
      const std::string host = paths.saves_dir + "/" + game_code(path.substr(16, 8)) + ext;
      if (same_as_file(host, data)) return;
      if (spill(host, data.data(), data.size())) r.saves++;
      else r.notes.push_back(host + ": cannot write");
    } else if (system_file(path) && !paths.sidecar.empty()) {
      auto it = sidecar.find(path);
      if (it != sidecar.end() && it->second == data) return;
      sidecar[path] = std::move(data);
      sidecar_changed = true;
      r.system_files++;
    }
  });
  if (sidecar_changed) {
    const std::vector<u8> b = encode_sidecar(sidecar);
    if (!spill(paths.sidecar, b.data(), b.size())) r.notes.push_back(paths.sidecar + ": cannot write");
  }

  if (nfs.photo().valid()) {
    nfs.photo().walk([&](const std::string& path, const FatVolume::Entry& e) {
      if (e.dir() || !touched(nand, nfs.photo_base(), nfs.photo(), e)) return;
      std::vector<u8> data;
      if (!nfs.photo().read(e, data)) return;
      const std::string host = paths.photos_dir + path;
      if (same_as_file(host, data)) return;
      if (spill(host, data.data(), data.size())) r.photos++;
      else r.notes.push_back(host + ": cannot write");
    });
  }
  return r;
}

}  // namespace ds::io
