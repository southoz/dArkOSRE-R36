// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/io/dsi_sd_card.h"
#include "core/io/dsi_nand_fs.h"

#include <sys/stat.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>

namespace ds::io {
namespace fs = std::filesystem;
namespace {

void wr32(u8* p, u32 v) { p[0] = static_cast<u8>(v); p[1] = static_cast<u8>(v >> 8); p[2] = static_cast<u8>(v >> 16); p[3] = static_cast<u8>(v >> 24); }
u32 rd32(const u8* p) { return static_cast<u32>(p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<u32>(p[3]) << 24)); }

std::string upper(std::string s) {
  for (char& c : s) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
  return s;
}

struct HostStat {
  bool ok = false, dir = false, regular = false;
  u64 size = 0;
  s64 mtime = 0, mtime_ns = 0;
};

HostStat host_stat(const std::string& path) {
  struct stat st;
  HostStat h;
  if (::stat(path.c_str(), &st) != 0) return h;
  h.ok = true;
  h.dir = S_ISDIR(st.st_mode);
  h.regular = S_ISREG(st.st_mode);
  h.size = static_cast<u64>(st.st_size);
  h.mtime = static_cast<s64>(st.st_mtime);
#if defined(__linux__)
  h.mtime_ns = static_cast<s64>(st.st_mtim.tv_nsec);
#endif
  return h;
}

// A host modification time as a FAT timestamp, local time as a PC stores it.
FatVolume::Stamp fat_stamp(s64 t) {
  const std::time_t tt = static_cast<std::time_t>(t);
  std::tm lt{};
#ifdef _WIN32
  localtime_s(&lt, &tt);
#else
  localtime_r(&tt, &lt);
#endif
  FatVolume::Stamp s;
  int year = lt.tm_year + 1900;
  if (year < 1980) return {static_cast<u16>((1 << 5) | 1), 0};
  if (year > 2107) year = 2107;
  s.date = static_cast<u16>(((year - 1980) << 9) | ((lt.tm_mon + 1) << 5) | lt.tm_mday);
  s.time = static_cast<u16>((lt.tm_hour << 11) | (lt.tm_min << 5) | (lt.tm_sec / 2));
  return s;
}

// Content plus 128 MB, rounded up to a power of two (a size that already is one doubles).
u64 card_size_for(u64 content) {
  u64 size = content + 0x8000000ull;
  size |= size >> 1; size |= size >> 2; size |= size >> 4; size |= size >> 8; size |= size >> 16; size |= size >> 32;
  return size + 1;
}

void log_access(bool write, u64 addr, u32 len) {
  static std::FILE* log = [] {
    const char* path = std::getenv("DS_SD_LOG");
    return path ? std::fopen(path, "w") : nullptr;
  }();
  if (log) { std::fprintf(log, "%c %llx %u\n", write ? 'w' : 'r', static_cast<unsigned long long>(addr), len); std::fflush(log); }
}

}  // namespace

SdCard::~SdCard() { close(); }

void SdCard::close() {
  host_in_.reset();
  open_index_ = ~0u;
  dir_.clear();
  length_ = 0;
  fat_bits_ = 0;
  part_base_ = 0;
  sectors_.clear();
  changed_.clear();
  extents_.clear();
  files_.clear();
  known_.clear();
  reads = writes = 0;
}

bool SdCard::open(const std::string& dir, Report* report, std::string* err) {
  close();
  Report local;
  Report& r = report ? *report : local;
  r = Report{};
  auto fail = [&](const std::string& m) { if (err) *err = m; close(); return false; };
  std::error_code ec;
  if (!fs::exists(dir, ec)) {
    fs::create_directories(dir, ec);
    if (ec) return fail("cannot create " + dir + ": " + ec.message());
  }
  if (!fs::is_directory(dir, ec)) return fail(dir + " is not a directory");
  dir_ = fs::path(dir).lexically_normal().string();
  while (dir_.size() > 1 && (dir_.back() == '/' || dir_.back() == '\\')) dir_.pop_back();

  // Directory links are not followed (they can loop); file links are.
  struct Item { std::string rel; bool dir; u64 size; s64 mtime, mtime_ns; };
  std::vector<Item> items;
  u64 content = 0;
  const fs::path root(dir_);
  fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
  if (ec) return fail("cannot read " + dir_ + ": " + ec.message());
  for (; it != end; it.increment(ec)) {
    if (ec) return fail("reading " + dir_ + ": " + ec.message());
    const fs::path& p = it->path();
    const std::string rel = p.lexically_relative(root).generic_string();
    const HostStat h = host_stat(p.string());
    if (!h.ok) { r.notes.push_back(rel + ": cannot be read; skipped"); continue; }
    if (h.dir && it->is_symlink(ec)) { it.disable_recursion_pending(); r.notes.push_back(rel + ": a link to a directory; skipped"); continue; }
    if (!h.dir && !h.regular) { r.notes.push_back(rel + ": not a regular file; skipped"); continue; }
    items.push_back({rel, h.dir, h.dir ? 0 : h.size, h.mtime, h.mtime_ns});
    content += h.dir ? 0x8000 : (h.size + 0x7FFF) / 0x8000 * 0x8000;
  }
  std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.rel < b.rel; });

  length_ = card_size_for(content);
  if (length_ > kMaxBytes)
    return fail(dir_ + " holds " + std::to_string(content >> 20) + " MB; a DSi SD card holds at most 32 GB");
  fat_bits_ = length_ >= (1ull << 30) ? 32 : 16;
  const u64 sectors = length_ / 512;
  const u64 part_sectors = sectors - kPartitionStart;
  // FAT16: ~32000 clusters. FAT32: 4 KB clusters up to 2 GB, 32 KB above (SDHC formatter shape).
  const u32 spc = fat_bits_ == 16 ? static_cast<u32>(sectors / 32768) : length_ <= (2ull << 30) ? 8 : 64;

  u8 mbr[512] = {};
  u8* pe = mbr + 0x1BE;
  pe[1] = 0xFE; pe[2] = 0xFF; pe[3] = 0xFF;    // CHS: use the LBA fields
  pe[4] = fat_bits_ == 32 ? 0x0C : 0x06;
  pe[5] = 0xFE; pe[6] = 0xFF; pe[7] = 0xFF;
  wr32(pe + 8, static_cast<u32>(kPartitionStart));
  wr32(pe + 12, static_cast<u32>(part_sectors));
  mbr[0x1FE] = 0x55; mbr[0x1FF] = 0xAA;
  poke(0, 512, mbr);

  FatVolume::FormatSpec spec;
  spec.sectors = part_sectors;
  spec.hidden = static_cast<u32>(kPartitionStart);
  spec.fat_bits = fat_bits_;
  spec.sectors_per_cluster = spc;
  spec.zeroed = true;
  const u64 base = kPartitionStart * 512;
  std::string why;
  if (!FatVolume::format([this, base](u64 o, u32 n, const u8* in) { poke(base + o, n, in); }, spec, &why))
    return fail("formatting the card: " + why);
  FatVolume v;
  if (!mount(v, &why)) return fail("mounting the new card: " + why);
  v.set_preserve_case(true);
  v.set_fresh(true);

  std::map<std::string, std::vector<size_t>> by_parent;   // directory -> its entries, one pass each
  for (size_t i = 0; i < items.size(); ++i) {
    const size_t slash = items[i].rel.rfind('/');
    by_parent[slash == std::string::npos ? std::string() : items[i].rel.substr(0, slash)].push_back(i);
  }
  struct Pending { std::string rel; FatVolume::Entry entry; };
  std::vector<Pending> queue{{std::string(), v.root()}};
  for (size_t q = 0; q < queue.size(); ++q) {
    const Pending cur = queue[q];
    const auto children = by_parent.find(cur.rel);
    if (children == by_parent.end()) continue;
    std::vector<FatVolume::NewEntry> news;
    std::vector<size_t> src;
    std::unordered_set<std::string> names;
    for (size_t i : children->second) {
      const Item& h = items[i];
      const size_t slash = h.rel.rfind('/');
      const std::string name = slash == std::string::npos ? h.rel : h.rel.substr(slash + 1);
      if (h.size > 0xFFFFFFFFull) { r.notes.push_back(h.rel + ": 4 GB or larger, which FAT cannot hold; skipped"); continue; }
      if (!names.insert(upper(name)).second) { r.notes.push_back(h.rel + ": differs from another name only in case; skipped"); continue; }
      FatVolume::NewEntry n;
      n.name = name;
      n.dir = h.dir;
      n.size = static_cast<u32>(h.size);
      n.stamp = fat_stamp(h.mtime);
      news.push_back(std::move(n));
      src.push_back(i);
    }
    if (!v.populate(cur.entry, news, &why)) return fail((cur.rel.empty() ? std::string("the folder's top level") : cur.rel) + ": " + why);
    for (size_t k = 0; k < news.size(); ++k) {
      const Item& h = items[src[k]];
      if (!news[k].ok) { r.notes.push_back(h.rel + ": " + news[k].why + "; skipped"); continue; }
      known_[upper(h.rel)] = Known{h.rel, h.dir, h.size, h.mtime, h.mtime_ns};
      if (h.dir) {
        r.dirs++;
        queue.push_back({h.rel, news[k].out});
      } else {
        r.files++;
        map_file(v, h.rel, news[k].out);
      }
    }
  }
  std::sort(extents_.begin(), extents_.end(), [](const Extent& a, const Extent& b) { return a.start < b.start; });
  return true;
}

bool SdCard::mount(FatVolume& v, std::string* why) {
  // The partition the MBR names (guest may have repartitioned), else a volume from sector 0.
  u8 mbr[512];
  peek(0, 512, mbr);
  part_base_ = 0;
  if (mbr[0x1FE] == 0x55 && mbr[0x1FF] == 0xAA && mbr[0x1BE + 4] != 0 && rd32(mbr + 0x1BE + 8) != 0)
    part_base_ = static_cast<u64>(rd32(mbr + 0x1BE + 8)) * 512;
  const u64 base = part_base_;
  return v.open([this, base](u64 o, u32 n, u8* out) { peek(base + o, n, out); },
                [this, base](u64 o, u32 n, const u8* in) { poke(base + o, n, in); }, why);
}

void SdCard::map_file(const FatVolume& v, const std::string& host_rel, const FatVolume::Entry& e) {
  if (!e.size || !e.cluster) return;
  const u32 index = static_cast<u32>(files_.size());
  files_.push_back(dir_ + "/" + host_rel);
  const u32 cs = v.cluster_bytes();
  const size_t first = extents_.size();
  u64 file_off = 0;
  for (u64 o : v.extents(e)) {
    const u64 at = part_base_ + o;
    if (extents_.size() > first && extents_.back().start + extents_.back().len == at) extents_.back().len += cs;
    else extents_.push_back({at, cs, index, file_off});
    file_off += cs;
  }
}

std::istream* SdCard::host_file(u32 index) {
  if (host_in_ && open_index_ == index) return host_in_.get();
  host_in_ = std::make_unique<std::ifstream>(files_[index], std::ios::binary);
  open_index_ = index;
  if (!*host_in_) { host_in_.reset(); open_index_ = ~0u; return nullptr; }
  return host_in_.get();
}

const SdCard::Extent* SdCard::extent_at(u64 addr) const {
  auto it = std::upper_bound(extents_.begin(), extents_.end(), addr, [](u64 a, const Extent& e) { return a < e.start; });
  if (it == extents_.begin()) return nullptr;
  --it;
  return addr < it->start + it->len ? &*it : nullptr;
}

void SdCard::read_backing(u64 addr, u8* out) {
  std::memset(out, 0, 512);
  const Extent* it = extent_at(addr);
  if (!it) return;
  std::istream* f = host_file(it->file);
  if (!f) return;
  f->clear();
  f->seekg(static_cast<std::streamoff>(it->file_off + (addr - it->start)));
  f->read(reinterpret_cast<char*>(out), 512);   // short past the end of the file: the rest stays zero
}

void SdCard::read_sector(u64 sector, u8* out) {
  const auto it = sectors_.find(sector);
  if (it != sectors_.end()) std::memcpy(out, it->second.data(), 512);
  else read_backing(sector * 512, out);
}

void SdCard::peek(u64 addr, u32 len, u8* out) {
  for (u64 s = addr / 512, end = (addr + len + 511) / 512; s < end; ++s) {
    u8 b[512];
    read_sector(s, b);
    const u64 sec = s * 512;
    const u64 from = std::max(sec, addr), to = std::min(sec + 512, addr + len);
    std::memcpy(out + (from - addr), b + (from - sec), static_cast<size_t>(to - from));
  }
}

void SdCard::poke(u64 addr, u32 len, const u8* in) {
  for (u64 s = addr / 512, end = (addr + len + 511) / 512; s < end; ++s) {
    const u64 sec = s * 512;
    auto [it, fresh] = sectors_.try_emplace(s);
    // A write that covers only part of a sector keeps the rest of it.
    if (fresh && (addr > sec || addr + len < sec + 512)) read_backing(sec, it->second.data());
    const u64 from = std::max(sec, addr), to = std::min(sec + 512, addr + len);
    std::memcpy(it->second.data() + (from - sec), in + (from - addr), static_cast<size_t>(to - from));
  }
}

void SdCard::read(u64 addr, u32 len, u8* out) {
  reads++;
  log_access(false, addr, len);
  if (addr + len > length_) { std::memset(out, 0, len); return; }
  peek(addr, len, out);
}

void SdCard::write(u64 addr, u32 len, const u8* in) {
  writes++;
  log_access(true, addr, len);
  if (addr + len > length_) return;
  poke(addr, len, in);
  for (u64 s = addr / 512, end = (addr + len + 511) / 512; s < end; ++s) changed_.insert(s);
}

bool SdCard::host_unchanged(const Known& k) const {
  const HostStat h = host_stat(dir_ + "/" + k.host);
  return h.ok && !h.dir && h.size == k.size && h.mtime == k.mtime && h.mtime_ns == k.mtime_ns;
}

SdCard::Report SdCard::sync() {
  Report r;
  if (!valid() || changed_.empty()) return r;
  FatVolume v;
  std::string why;
  if (!mount(v, &why)) {
    r.notes.push_back("the card's filesystem cannot be read (" + why + "); nothing was carried out");
    changed_.clear();
    return r;
  }
  const u32 cs = v.cluster_bytes();
  const u64 data = part_base_ + v.data_offset();
  std::unordered_set<u32> dirty;   // clusters the guest wrote
  for (u64 s : changed_) {
    const u64 a = s * 512;
    if (a >= data) dirty.insert(static_cast<u32>((a - data) / cs) + 2);
  }
  auto cluster_of = [&](u64 volume_off) { return static_cast<u32>((part_base_ + volume_off - data) / cs) + 2; };

  struct Node { std::string path, key; FatVolume::Entry e; };   // card's tree, parents before children
  std::vector<Node> nodes;
  {
    std::vector<Node> stack{{std::string(), std::string(), v.root()}};
    std::unordered_set<u32> seen_dirs;
    while (!stack.empty()) {
      const Node cur = stack.back();
      stack.pop_back();
      for (const FatVolume::Entry& e : v.list(cur.e)) {
        if (e.name == "." || e.name == "..") continue;
        Node n;
        n.path = cur.path.empty() ? e.display_name() : cur.path + "/" + e.display_name();
        n.key = upper(n.path);
        n.e = e;
        nodes.push_back(n);
        if (e.dir() && e.cluster && seen_dirs.insert(e.cluster).second) stack.push_back(n);
      }
    }
    std::sort(nodes.begin(), nodes.end(), [](const Node& a, const Node& b) { return a.path < b.path; });
  }

  // Copies a file's sectors out of whatever backs them; the card then keeps it in memory.
  auto keep_in_memory = [&](const FatVolume::Entry& e) {
    for (u64 o : v.extents(e))
      for (u32 s = 0; s < cs; s += 512) {
        const u64 sec = (part_base_ + o + s) / 512;
        if (sectors_.count(sec)) continue;
        u8 b[512];
        read_backing(sec * 512, b);
        std::memcpy(sectors_[sec].data(), b, 512);
      }
  };

  std::unordered_map<std::string, std::string> host_of{{std::string(), std::string()}};
  std::unordered_set<std::string> present;
  std::vector<std::pair<std::string, FatVolume::Entry>> backed;   // host file -> the card file it holds
  std::error_code ec;
  for (const Node& n : nodes) {
    present.insert(n.key);
    const size_t slash = n.key.rfind('/');
    const std::string parent_key = slash == std::string::npos ? std::string() : n.key.substr(0, slash);
    const auto kit = known_.find(n.key);
    const bool known = kit != known_.end() && kit->second.dir == n.e.dir();
    std::string host_rel;
    if (known) {
      host_rel = kit->second.host;
    } else {
      const auto p = host_of.find(parent_key);
      const std::string& ph = p != host_of.end() ? p->second : std::string();
      host_rel = ph.empty() ? n.e.display_name() : ph + "/" + n.e.display_name();
    }
    host_of[n.key] = host_rel;
    const std::string host = dir_ + "/" + host_rel;

    if (n.e.dir()) {
      if (!known) {
        fs::create_directories(host, ec);
        if (ec) { r.notes.push_back(host_rel + "/: cannot be created (" + ec.message() + ")"); ec.clear(); continue; }
        known_[n.key] = Known{host_rel, true, 0, 0, 0};
        r.dirs++;
      }
      continue;
    }

    bool changed = !known || kit->second.size != n.e.size;
    if (!changed)
      for (u64 o : v.extents(n.e))
        if (dirty.count(cluster_of(o))) { changed = true; break; }
    if (!changed) {
      // Stays where it is read from now: the host, or the card's memory after an earlier conflict.
      const std::vector<u64> ex = v.extents(n.e);
      if (ex.empty() || is_backed(part_base_ + ex.front())) backed.emplace_back(host_rel, n.e);
      continue;
    }

    if (known ? !host_unchanged(kit->second) : fs::exists(host, ec)) {
      r.notes.push_back(host_rel + (known ? ": changed on the host since the card was built; the card's copy was not written"
                                          : ": already exists on the host; the card's copy was not written"));
      keep_in_memory(n.e);
      continue;
    }
    // Written beside the file and renamed over it, so a failed write leaves the host's copy whole.
    const std::string tmp = host + ".dsperate-sync";
    bool ok = false;
    {
      std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
      if (out) {
        u64 left = n.e.size;
        u8 b[512];
        for (u64 o : v.extents(n.e)) {
          for (u32 s = 0; s < cs && left; s += 512) {
            peek(part_base_ + o + s, 512, b);
            const u32 w = static_cast<u32>(std::min<u64>(512, left));
            out.write(reinterpret_cast<const char*>(b), w);
            left -= w;
          }
          if (!left) break;
        }
        out.flush();
        ok = left == 0 && static_cast<bool>(out);
      }
    }
    if (ok) {
      fs::rename(tmp, host, ec);
      ok = !ec;
      ec.clear();
    }
    if (!ok) {
      fs::remove(tmp, ec);
      ec.clear();
      r.notes.push_back(host_rel + ": cannot be written; the card keeps its copy");
      keep_in_memory(n.e);
      continue;
    }
    const HostStat h = host_stat(host);
    known_[n.key] = Known{host_rel, false, n.e.size, h.mtime, h.mtime_ns};
    backed.emplace_back(host_rel, n.e);
    r.files++;
  }

  std::vector<std::string> gone;   // guest-removed: files then directories, children before parents
  for (const auto& [key, k] : known_) if (!present.count(key)) gone.push_back(key);
  std::sort(gone.rbegin(), gone.rend());
  for (const std::string& key : gone) {
    const Known k = known_[key];
    known_.erase(key);
    const std::string host = dir_ + "/" + k.host;
    if (k.dir) {
      if (fs::is_directory(host, ec) && fs::is_empty(host, ec) && fs::remove(host, ec)) r.removed++;
      ec.clear();
      continue;
    }
    if (!fs::exists(host, ec)) { ec.clear(); continue; }
    if (!host_unchanged(k)) { r.notes.push_back(k.host + ": removed on the card but changed on the host; kept"); continue; }
    if (fs::remove(host, ec)) r.removed++;
    else r.notes.push_back(k.host + ": cannot be removed (" + ec.message() + ")");
    ec.clear();
  }

  // Rebuild the backing from the tree; the now-written sectors are let go.
  host_in_.reset();
  open_index_ = ~0u;
  extents_.clear();
  files_.clear();
  for (const auto& [host_rel, e] : backed) {
    map_file(v, host_rel, e);
    for (u64 o : v.extents(e))
      for (u32 s = 0; s < cs; s += 512) sectors_.erase((part_base_ + o + s) / 512);
  }
  std::sort(extents_.begin(), extents_.end(), [](const Extent& a, const Extent& b) { return a.start < b.start; });
  changed_.clear();
  return r;
}

SdCard::StateSnapshot SdCard::state_snapshot() const {
  StateSnapshot st;
  if (!valid()) return st;
  st.present = true;
  st.length = length_;
  st.part_base = part_base_;
  st.fat_bits = fat_bits_;
  for (const auto& [sec, data] : sectors_) st.sectors.push_back(sec);
  std::sort(st.sectors.begin(), st.sectors.end());
  st.data.resize(st.sectors.size() * 512);
  for (size_t i = 0; i < st.sectors.size(); ++i) std::memcpy(&st.data[i * 512], sectors_.at(st.sectors[i]).data(), 512);
  st.changed.assign(changed_.begin(), changed_.end());
  std::sort(st.changed.begin(), st.changed.end());
  for (const Extent& e : extents_) {
    st.ext_start.push_back(e.start); st.ext_len.push_back(e.len);
    st.ext_file.push_back(e.file); st.ext_file_off.push_back(e.file_off);
  }
  for (const std::string& f : files_) st.files.push_back(f.substr(dir_.size() + 1));
  for (const auto& [key, k] : known_) {   // std::map: in key order
    st.known_key.push_back(key); st.known_host.push_back(k.host);
    st.known_dir.push_back(k.dir ? 1 : 0); st.known_size.push_back(k.size);
    st.known_mtime.push_back(k.mtime); st.known_mtime_ns.push_back(k.mtime_ns);
  }
  return st;
}

bool SdCard::state_matches(const StateSnapshot& snap, std::string* why) const {
  auto no = [why](const std::string& m) { if (why) *why = m; return false; };
  if (!snap.present) return no("the state has no SD card");
  if (!valid()) return no("this session has no SD card");
  const size_t n = snap.known_key.size();
  if (snap.known_host.size() != n || snap.known_dir.size() != n || snap.known_size.size() != n ||
      snap.known_mtime.size() != n || snap.known_mtime_ns.size() != n || snap.data.size() != snap.sectors.size() * 512)
    return no("the state's card record is damaged");
  std::unordered_map<std::string, size_t> by_host;
  for (size_t i = 0; i < n; ++i) if (!snap.known_dir[i]) by_host[snap.known_host[i]] = i;
  for (u32 f : snap.ext_file) if (f >= snap.files.size()) return no("the state's card record is damaged");
  for (const std::string& rel : snap.files) {
    const auto it = by_host.find(rel);
    if (it == by_host.end()) return no(rel + " is not in the state's card record");
    const Known k{rel, false, snap.known_size[it->second], snap.known_mtime[it->second], snap.known_mtime_ns[it->second]};
    if (!host_unchanged(k)) return no(dir_ + "/" + rel + " has changed since the state was made");
  }
  return true;
}

void SdCard::apply_state_snapshot(const StateSnapshot& snap) {
  host_in_.reset();
  open_index_ = ~0u;
  length_ = snap.length;
  part_base_ = snap.part_base;
  fat_bits_ = snap.fat_bits;
  sectors_.clear();
  for (size_t i = 0; i < snap.sectors.size(); ++i) std::memcpy(sectors_[snap.sectors[i]].data(), &snap.data[i * 512], 512);
  changed_.clear();
  changed_.insert(snap.changed.begin(), snap.changed.end());
  extents_.clear();
  for (size_t i = 0; i < snap.ext_start.size(); ++i) extents_.push_back({snap.ext_start[i], snap.ext_len[i], snap.ext_file[i], snap.ext_file_off[i]});
  files_.clear();
  for (const std::string& rel : snap.files) files_.push_back(dir_ + "/" + rel);
  known_.clear();
  for (size_t i = 0; i < snap.known_key.size(); ++i)
    known_[snap.known_key[i]] = Known{snap.known_host[i], snap.known_dir[i] != 0, snap.known_size[i], snap.known_mtime[i], snap.known_mtime_ns[i]};
  writes++;   // the frontend's cue to sync what the state's card had not yet carried out
}

bool SdCard::dump(const std::string& path) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  u8 b[512];
  for (u64 s = 0; s < length_ / 512; ++s) {
    read_sector(s, b);
    out.write(reinterpret_cast<const char*>(b), 512);
  }
  return static_cast<bool>(out);
}

}  // namespace ds::io
