// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/io/dsi_nand_fs.h"
#include "core/io/dsi_aes.h"
#include "core/io/dsi_sd.h"
#include "core/crypto/sha1.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <unordered_set>

namespace ds::io {
namespace {

u16 rd16(const u8* p) { return static_cast<u16>(p[0] | (p[1] << 8)); }
u32 rd32(const u8* p) { return static_cast<u32>(p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<u32>(p[3]) << 24)); }
void wr16(u8* p, u16 v) { p[0] = static_cast<u8>(v); p[1] = static_cast<u8>(v >> 8); }
void wr32(u8* p, u32 v) { wr16(p, static_cast<u16>(v)); wr16(p + 2, static_cast<u16>(v >> 16)); }

// FAT date for 2000-01-01, the date the DSi's own files carry; no time.
constexpr u16 kFatDate = ((2000 - 1980) << 9) | (1 << 5) | 1;

std::string upper(std::string s) {
  for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

std::vector<std::string> parts_of(const std::string& path) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i < path.size()) {
    size_t j = path.find('/', i);
    if (j == std::string::npos) j = path.size();
    if (j > i) out.push_back(path.substr(i, j - i));
    i = j + 1;
  }
  return out;
}

// Long names are UTF-16 on disk and UTF-8 everywhere else.
bool utf8_to_16(const std::string& in, std::u16string& out) {
  out.clear();
  for (size_t i = 0; i < in.size();) {
    const u8 c = static_cast<u8>(in[i]);
    u32 cp;
    int n;
    if (c < 0x80)                { cp = c;        n = 0; }
    else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; n = 1; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; n = 2; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; n = 3; }
    else return false;
    if (n && i + n >= in.size()) return false;
    for (int k = 1; k <= n; ++k) {
      const u8 d = static_cast<u8>(in[i + k]);
      if ((d & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (d & 0x3F);
    }
    i += n + 1;
    if (cp >= 0x10000) {
      if (cp > 0x10FFFF) return false;
      cp -= 0x10000;
      out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
      out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
    } else {
      if (cp >= 0xD800 && cp < 0xE000) return false;
      out.push_back(static_cast<char16_t>(cp));
    }
  }
  return true;
}

std::string utf16_to_8(const std::u16string& in) {
  std::string out;
  for (size_t i = 0; i < in.size(); ++i) {
    u32 cp = in[i];
    if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < in.size() && in[i + 1] >= 0xDC00 && in[i + 1] < 0xE000) {
      cp = 0x10000 + ((cp - 0xD800) << 10) + (in[i + 1] - 0xDC00);
      ++i;
    } else if (cp >= 0xD800 && cp < 0xE000) {
      cp = '?';
    }
    if (cp < 0x80) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }
  return out;
}

}  // namespace

// ---- FatVolume ---------------------------------------------------------------

bool FatVolume::open(ReadFn read, WriteFn write, std::string* err) {
  auto fail = [&](const char* m) { if (err) *err = m; bps_ = 0; return false; };
  read_ = std::move(read);
  write_ = std::move(write);
  u8 b[512];
  read_(0, 512, b);
  if (b[0x1FE] != 0x55 || b[0x1FF] != 0xAA) return fail("no boot sector signature");
  const u32 bps = rd16(b + 0x0B), spc = b[0x0D], rsv = rd16(b + 0x0E);
  const u32 nfats = b[0x10], root = rd16(b + 0x11);
  const bool fat32 = rd16(b + 0x16) == 0;   // no 16-bit FAT size, no fixed root directory
  const u32 spf = fat32 ? rd32(b + 0x24) : rd16(b + 0x16);
  const u64 total = rd16(b + 0x13) ? rd16(b + 0x13) : rd32(b + 0x20);
  if (bps != 512 || spc == 0 || (spc & (spc - 1)) || nfats == 0 || spf == 0 || rsv == 0 || (fat32 ? root != 0 : root == 0))
    return fail("not a FAT volume");
  const u32 root_sectors = (root * 32 + bps - 1) / bps;
  const u64 data_sector = rsv + static_cast<u64>(nfats) * spf + root_sectors;
  if (total <= data_sector) return fail("volume smaller than its own metadata");
  const u64 clusters = (total - data_sector) / spc;
  if (!fat32 && clusters >= 65525) return fail("too many clusters for FAT16");
  if (fat32 && clusters > 0x0FFFFFF5) return fail("too many clusters for FAT32");
  bps_ = bps; spc_ = spc; nfats_ = nfats; fat_sectors_ = spf; root_entries_ = root;
  clusters_ = static_cast<u32>(clusters);
  fat_bits_ = fat32 ? 32 : clusters < 4085 ? 12 : 16;
  root_cluster_ = fat32 ? rd32(b + 0x2C) : 0;
  fat_off_ = static_cast<u64>(rsv) * bps;
  root_off_ = fat_off_ + static_cast<u64>(nfats) * spf * bps;
  data_off_ = data_sector * bps;
  fat_.assign(static_cast<size_t>(spf) * bps, 0);
  for (u32 s = 0; s < spf; ++s) read_(fat_off_ + static_cast<u64>(s) * bps, bps, fat_.data() + s * bps);
  fat_dirty_.assign(spf, false);
  free_count_ = 0;
  for (u32 c = 2; c < clusters_ + 2; ++c) free_count_ += fat_get(c) == 0;
  next_free_ = 2;
  return true;
}

bool FatVolume::format(const WriteFn& write, const FormatSpec& s, std::string* err) {
  auto fail = [&](const char* m) { if (err) *err = m; return false; };
  const bool fat32 = s.fat_bits == 32;
  if (s.fat_bits != 16 && !fat32) return fail("format: FAT16 or FAT32 only");
  const u32 spc = s.sectors_per_cluster;
  if (spc == 0 || spc > 128 || (spc & (spc - 1))) return fail("format: bad cluster size");
  if (s.sectors > 0xFFFFFFFFull || s.sectors < 64) return fail("format: bad volume size");
  const u32 total = static_cast<u32>(s.sectors);
  const u32 rsv = fat32 ? 32 : 1, nfats = 2, root_entries = fat32 ? 0 : 512;
  const u32 root_sectors = root_entries * 32 / 512;
  const u64 clusters_max = (total - rsv - root_sectors) / spc;   // slight overestimate, as formatters make
  const u32 spf = static_cast<u32>(((clusters_max + 2) * (fat32 ? 4 : 2) + 511) / 512);
  const u64 data = rsv + static_cast<u64>(nfats) * spf + root_sectors;
  if (total <= data + spc) return fail("format: volume smaller than its own metadata");
  const u64 clusters = (total - data) / spc;
  if (!fat32 && (clusters < 4085 || clusters >= 65525)) return fail("format: the cluster count does not suit FAT16");
  if (fat32 && clusters < 65525) return fail("format: the cluster count does not suit FAT32");

  u8 b[512] = {};
  b[0] = 0xEB; b[1] = fat32 ? 0x58 : 0x3C; b[2] = 0x90;
  std::memset(b + 3, ' ', 8);
  std::memcpy(b + 3, s.oem, std::min<size_t>(8, std::strlen(s.oem)));
  wr16(b + 0x0B, 512); b[0x0D] = static_cast<u8>(spc); wr16(b + 0x0E, static_cast<u16>(rsv)); b[0x10] = static_cast<u8>(nfats);
  wr16(b + 0x11, static_cast<u16>(root_entries));
  if (!fat32 && total < 0x10000) wr16(b + 0x13, static_cast<u16>(total)); else wr32(b + 0x20, total);
  b[0x15] = 0xF8;
  if (!fat32) wr16(b + 0x16, static_cast<u16>(spf));
  wr16(b + 0x18, 63); wr16(b + 0x1A, 255); wr32(b + 0x1C, s.hidden);
  if (fat32) { wr32(b + 0x24, spf); wr32(b + 0x2C, 2); wr16(b + 0x30, 1); wr16(b + 0x32, 6); }
  u8* ext = b + (fat32 ? 0x40 : 0x24);
  ext[0] = 0x80; ext[2] = 0x29; wr32(ext + 3, s.serial);
  std::memset(ext + 7, ' ', 11);
  std::memcpy(ext + 7, s.label, std::min<size_t>(11, std::strlen(s.label)));
  std::memcpy(ext + 18, fat32 ? "FAT32   " : "FAT16   ", 8);
  b[0x1FE] = 0x55; b[0x1FF] = 0xAA;

  const std::vector<u8> zero(512, 0);
  auto zero_range = [&](u64 first, u64 count) {
    if (s.zeroed) return;
    for (u64 k = 0; k < count; ++k) write((first + k) * 512, 512, zero.data());
  };
  zero_range(1, rsv - 1);
  write(0, 512, b);
  if (fat32) {
    u8 fsi[512] = {};
    wr32(fsi, 0x41615252); wr32(fsi + 0x1E4, 0x61417272);
    wr32(fsi + 0x1E8, 0xFFFFFFFF); wr32(fsi + 0x1EC, 0xFFFFFFFF);   // free count and next free: unknown
    fsi[0x1FE] = 0x55; fsi[0x1FF] = 0xAA;
    write(1 * 512, 512, fsi);
    write(6 * 512, 512, b);
    write(7 * 512, 512, fsi);
  }
  for (u32 f = 0; f < nfats; ++f) {
    const u64 fat = rsv + static_cast<u64>(f) * spf;
    zero_range(fat + 1, spf - 1);
    u8 first[512] = {};
    if (fat32) { wr32(first, 0x0FFFFFF8); wr32(first + 4, 0x0FFFFFFF); wr32(first + 8, 0x0FFFFFFF); }   // cluster 2: the root
    else       { wr16(first, 0xFFF8); wr16(first + 2, 0xFFFF); }
    write(fat * 512, 512, first);
  }
  if (fat32) zero_range(data, spc);
  else       zero_range(rsv + static_cast<u64>(nfats) * spf, root_sectors);
  return true;
}

FatVolume::Entry FatVolume::root() const {
  Entry r;
  r.attr = 0x10;
  r.cluster = root_cluster_;
  return r;
}

u32 FatVolume::fat_get(u32 c) const {
  if (fat_bits_ == 12) {
    const size_t o = c + c / 2;
    if (o + 1 >= fat_.size()) return 0xFFF;
    const u32 v = fat_[o] | (fat_[o + 1] << 8);
    return (c & 1) ? v >> 4 : v & 0xFFF;
  }
  if (fat_bits_ == 16) {
    const size_t o = static_cast<size_t>(c) * 2;
    return o + 1 < fat_.size() ? rd16(&fat_[o]) : 0xFFFF;
  }
  const size_t o = static_cast<size_t>(c) * 4;
  return o + 3 < fat_.size() ? rd32(&fat_[o]) & 0x0FFFFFFF : 0x0FFFFFFF;
}

void FatVolume::fat_set(u32 c, u32 v) {
  const size_t width = fat_bits_ == 12 ? 2 : fat_bits_ == 16 ? 2 : 4;
  const size_t o = fat_bits_ == 12 ? c + c / 2 : static_cast<size_t>(c) * (fat_bits_ / 8);
  if (o + width > fat_.size()) return;
  if (c >= 2 && c < clusters_ + 2) {
    const bool was_free = fat_get(c) == 0;
    if (was_free && v != 0) --free_count_;
    if (!was_free && v == 0) ++free_count_;
    if (v == 0 && c < next_free_) next_free_ = c;   // stays at or below the lowest free cluster
  }
  if (fat_bits_ == 12) {
    if (c & 1) { fat_[o] = static_cast<u8>((fat_[o] & 0x0F) | ((v << 4) & 0xF0)); fat_[o + 1] = static_cast<u8>(v >> 4); }
    else       { fat_[o] = static_cast<u8>(v); fat_[o + 1] = static_cast<u8>((fat_[o + 1] & 0xF0) | ((v >> 8) & 0x0F)); }
  } else if (fat_bits_ == 16) {
    wr16(&fat_[o], static_cast<u16>(v));
  } else {
    wr32(&fat_[o], (rd32(&fat_[o]) & 0xF0000000) | (v & 0x0FFFFFFF));
  }
  fat_dirty_[o / bps_] = true;
  fat_dirty_[(o + width - 1) / bps_] = true;
}

void FatVolume::fat_flush() {
  for (u32 s = 0; s < fat_sectors_; ++s) {
    if (!fat_dirty_[s]) continue;
    for (u32 k = 0; k < nfats_; ++k)
      write_(fat_off_ + (static_cast<u64>(k) * fat_sectors_ + s) * bps_, bps_, fat_.data() + s * bps_);
    fat_dirty_[s] = false;
  }
}

std::vector<u32> FatVolume::chain(u32 first) const {
  std::vector<u32> out;
  const u32 eoc = eoc_min();
  for (u32 c = first; c >= 2 && c < clusters_ + 2 && out.size() <= clusters_; c = fat_get(c)) {
    out.push_back(c);
    if (fat_get(c) >= eoc) break;
  }
  return out;
}

bool FatVolume::alloc_chain(u32 count, u32& first) {
  first = 0;
  if (count == 0) return true;
  if (free_count_ < count) return false;
  std::vector<u32> got;
  got.reserve(count);
  u32 c = std::max<u32>(next_free_, 2);
  for (; c < clusters_ + 2 && got.size() < count; ++c)
    if (fat_get(c) == 0) got.push_back(c);
  if (got.size() < count) return false;
  for (size_t i = 0; i < got.size(); ++i) fat_set(got[i], i + 1 < got.size() ? got[i + 1] : eoc_mark());
  next_free_ = std::max(next_free_, c);   // every cluster below c is now in use
  first = got[0];
  return true;
}

void FatVolume::free_chain(u32 first) {
  for (u32 c : chain(first)) fat_set(c, 0);
}

u32 FatVolume::entry_cluster(const u8* e) const {
  return rd16(e + 26) | (fat_bits_ == 32 ? static_cast<u32>(rd16(e + 20)) << 16 : 0);
}

void FatVolume::set_entry_cluster(u8* e, u32 c) const {
  wr16(e + 26, static_cast<u16>(c));
  if (fat_bits_ == 32) wr16(e + 20, static_cast<u16>(c >> 16));
}

std::vector<u8> FatVolume::dir_bytes(const Entry& dir) const {
  std::vector<u8> out;
  const u32 first = dir.cluster ? dir.cluster : root_cluster_;
  if (first == 0) {
    out.resize((root_entries_ * 32 + bps_ - 1) / bps_ * bps_);
    for (size_t s = 0; s < out.size(); s += bps_) read_(root_off_ + s, bps_, out.data() + s);
    return out;
  }
  const u32 cs = cluster_bytes();
  for (u32 c : chain(first)) {
    const size_t at = out.size();
    out.resize(at + cs);
    for (u32 s = 0; s < cs; s += bps_) read_(cluster_offset(c) + s, bps_, out.data() + at + s);
  }
  return out;
}

std::vector<FatVolume::Entry> FatVolume::list(const Entry& dir) const {
  std::vector<Entry> out;
  const u32 first = dir.cluster ? dir.cluster : root_cluster_;
  const std::vector<u8> d = dir_bytes(dir);
  const std::vector<u32> cl = first ? chain(first) : std::vector<u32>{};
  const u32 cs = cluster_bytes();
  std::u16string lfn;
  u8 lfn_sum = 0;
  for (size_t i = 0; i + 32 <= d.size(); i += 32) {
    const u8* e = &d[i];
    if (e[0] == 0x00) break;
    if (e[0] == 0xE5) { lfn.clear(); continue; }
    if (e[11] == 0x0F) {
      // Long-name pieces come last-first, 13 UTF-16 units each.
      std::u16string piece;
      for (int k : {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30}) {
        const char16_t ch = static_cast<char16_t>(e[k] | (e[k + 1] << 8));
        if (ch == 0x0000 || ch == 0xFFFF) break;
        piece.push_back(ch);
      }
      if (e[0] & 0x40) lfn.clear();
      lfn = piece + lfn;
      lfn_sum = e[13];
      continue;
    }
    if (e[11] & 0x08) { lfn.clear(); continue; }
    Entry en;
    if (!lfn.empty()) {
      u8 sum = 0;
      for (int k = 0; k < 11; ++k) sum = static_cast<u8>(((sum & 1) << 7) + (sum >> 1) + e[k]);
      if (sum == lfn_sum) en.long_name = utf16_to_8(lfn);
      lfn.clear();
    }
    std::string base(reinterpret_cast<const char*>(e), 8), ext(reinterpret_cast<const char*>(e + 8), 3);
    if (static_cast<u8>(base[0]) == 0x05) base[0] = static_cast<char>(0xE5);
    base.erase(base.find_last_not_of(' ') + 1);
    ext.erase(ext.find_last_not_of(' ') + 1);
    en.name = ext.empty() ? base : base + "." + ext;
    // Windows and Linux store a lower-case 8.3 name as upper case plus two
    // flags in the reserved byte, not as a long name.
    if (en.long_name.empty() && (e[12] & 0x18)) {
      std::string b2 = base, e2 = ext;
      if (e[12] & 0x08) for (char& ch : b2) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
      if (e[12] & 0x10) for (char& ch : e2) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
      en.long_name = e2.empty() ? b2 : b2 + "." + e2;
    }
    en.attr = e[11];
    en.cluster = entry_cluster(e);
    en.size = rd32(e + 28);
    en.mtime = rd16(e + 22);
    en.mdate = rd16(e + 24);
    en.dirent = first == 0 ? root_off_ + i : cluster_offset(cl[i / cs]) + i % cs;
    out.push_back(std::move(en));
  }
  return out;
}

bool FatVolume::lookup(const std::string& path, Entry& out) const {
  Entry cur = root();
  for (const std::string& p : parts_of(path)) {
    if (!cur.dir()) return false;
    const std::string want = upper(p);
    bool found = false;
    for (const Entry& e : list(cur)) {
      if (e.name != want && (e.long_name.empty() || upper(e.long_name) != want)) continue;
      cur = e;
      found = true;
      break;
    }
    if (!found) return false;
  }
  out = cur;
  return true;
}

bool FatVolume::read(const Entry& file, std::vector<u8>& out) const {
  out.clear();
  if (file.dir()) return false;
  const u32 cs = cluster_bytes();
  const std::vector<u32> cl = chain(file.cluster);
  if (static_cast<u64>(cl.size()) * cs < file.size) return false;   // a chain shorter than the size: a broken file
  out.resize(static_cast<size_t>(cl.size()) * cs);
  for (size_t k = 0; k < cl.size(); ++k)
    for (u32 s = 0; s < cs; s += bps_) read_(cluster_offset(cl[k]) + s, bps_, out.data() + k * cs + s);
  out.resize(file.size);
  return true;
}

bool FatVolume::read_part(const Entry& file, u64 offset, u32 len, u8* out) const {
  if (file.dir() || offset + len > file.size) return false;
  if (!len) return true;
  const u32 cs = cluster_bytes();
  const std::vector<u32> cl = chain(file.cluster);
  if (static_cast<u64>(cl.size()) * cs < file.size) return false;
  std::vector<u8> sec(bps_);
  u64 done = 0;
  while (done < len) {
    const u64 at = offset + done;
    const u64 k = at / cs;
    const u32 in_cluster = static_cast<u32>(at % cs);
    const u32 s = in_cluster / bps_ * bps_;
    read_(cluster_offset(cl[static_cast<size_t>(k)]) + s, bps_, sec.data());
    const u32 from = in_cluster - s;
    const u32 take = static_cast<u32>(std::min<u64>(bps_ - from, len - done));
    std::memcpy(out + done, sec.data() + from, take);
    done += take;
  }
  return true;
}

void FatVolume::walk(const std::function<void(const std::string&, const Entry&)>& fn) const {
  std::vector<std::pair<std::string, Entry>> stack;
  stack.emplace_back("", root());
  while (!stack.empty()) {
    auto [base, dir] = stack.back();
    stack.pop_back();
    for (const Entry& e : list(dir)) {
      if (e.name == "." || e.name == "..") continue;
      const std::string path = base + "/" + e.name;
      fn(path, e);
      if (e.dir() && e.cluster) stack.emplace_back(path, e);
    }
  }
}

std::vector<u64> FatVolume::extents(const Entry& file) const {
  std::vector<u64> out;
  for (u32 c : chain(file.cluster)) out.push_back(cluster_offset(c));
  return out;
}

bool FatVolume::to_83(const std::string& name, u8 out[11]) {
  std::memset(out, ' ', 11);
  const size_t dot = name.find_last_of('.');
  const std::string base = upper(dot == std::string::npos ? name : name.substr(0, dot));
  const std::string ext = upper(dot == std::string::npos ? "" : name.substr(dot + 1));
  if (base.empty() || base.size() > 8 || ext.size() > 3) return false;
  for (char c : base + ext)
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '~')) return false;
  std::memcpy(out, base.data(), base.size());
  std::memcpy(out + 8, ext.data(), ext.size());
  return true;
}

bool FatVolume::split(const std::string& path, Entry& parent, std::string& name, std::string* err) const {
  std::vector<std::string> p = parts_of(path);
  if (p.empty()) { if (err) *err = "empty path"; return false; }
  name = p.back();
  p.pop_back();
  std::string dir;
  for (const std::string& s : p) dir += "/" + s;
  if (!lookup(dir, parent) || !parent.dir()) { if (err) *err = "no directory " + (dir.empty() ? "/" : dir); return false; }
  return true;
}

void FatVolume::write_entry(u64 dirent, const u8 raw[32]) {
  const u64 sec = dirent / bps_ * bps_;
  u8 b[512];
  read_(sec, bps_, b);
  std::memcpy(b + (dirent - sec), raw, 32);
  write_(sec, bps_, b);
}

bool FatVolume::add_entry(const Entry& parent, const u8* raw, u32 count, u64& dirent_out) {
  const u32 first = parent.cluster ? parent.cluster : root_cluster_;
  const std::vector<u8> d = dir_bytes(parent);
  const std::vector<u32> cl = first ? chain(first) : std::vector<u32>{};
  const u32 cs = cluster_bytes();
  const size_t limit = first ? d.size() : static_cast<size_t>(root_entries_) * 32;
  auto at = [&](size_t i) { return first ? cluster_offset(cl[i / cs]) + i % cs : root_off_ + i; };
  // First run of `count` free slots; everything from the first 0x00 entry on is free.
  size_t run = 0;
  for (size_t i = 0; i + 32 <= limit; i += 32) {
    if (d[i] != 0x00 && d[i] != 0xE5) { run = 0; continue; }
    if (++run < count) continue;
    const size_t start = i + 32 - count * 32;
    for (u32 k = 0; k < count; ++k) write_entry(at(start + k * 32), raw + k * 32);
    dirent_out = at(i);
    return true;
  }
  if (!first || cl.empty()) return false;   // the fixed root directory is full
  // Grow by a zeroed cluster, entries at its start (a run cut by the cluster end is abandoned).
  if (count * 32 > cs) return false;
  u32 c;
  if (!alloc_chain(1, c)) return false;
  fat_set(cl.back(), c);
  std::vector<u8> zero(bps_, 0);
  for (u32 s = 0; s < cs; s += bps_) write_(cluster_offset(c) + s, bps_, zero.data());
  for (u32 k = 0; k < count; ++k) write_entry(cluster_offset(c) + k * 32, raw + k * 32);
  dirent_out = cluster_offset(c) + (count - 1) * 32;
  return true;
}

bool FatVolume::make_entries(const Entry& parent, const std::string& name, std::vector<u8>& raws, std::string* why) const {
  std::unordered_set<std::string> taken;
  for (const Entry& e : list(parent)) taken.insert(e.name);
  return make_entries_in(taken, name, raws, why);
}

bool FatVolume::make_entries_in(const std::unordered_set<std::string>& taken, const std::string& name, std::vector<u8>& raws, std::string* why) const {
  auto fail = [&](const char* m) { if (why) *why = m; return false; };
  if (name == "." || name == "..") return fail("a reserved name");
  u8 short_name[11];
  const bool is_83 = to_83(name, short_name);
  const bool has_lower = std::any_of(name.begin(), name.end(), [](char c) { return c >= 'a' && c <= 'z'; });
  if (is_83 && !(preserve_case_ && has_lower)) { raws.assign(short_name, short_name + 11); raws.resize(32, 0); return true; }

  std::u16string wide;
  if (!utf8_to_16(name, wide)) return fail("the name is not UTF-8");
  if (wide.size() > 255) return fail("the name is longer than 255 characters");
  for (char16_t ch : wide)
    if (ch < 0x20 || (ch < 0x80 && std::strchr("\"*/:<>?\\|", static_cast<char>(ch)))) return fail("the name has a character FAT does not allow");
  if (name.back() == '.' || name.back() == ' ') return fail("the name ends in a dot or a space");

  if (!is_83) {
    // NAME~N.EXT from the long name's letters, N the first not in use.
    std::string base, ext;
    const size_t dot = name.find_last_of('.');
    for (char c : name.substr(0, dot)) if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-') base.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    if (dot != std::string::npos) for (char c : name.substr(dot + 1)) if (std::isalnum(static_cast<unsigned char>(c)) && ext.size() < 3) ext.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    if (base.empty()) base = "FILE";
    bool found = false;
    for (u32 n = 1; n <= 999999 && !found; ++n) {
      const std::string tail = "~" + std::to_string(n);
      const std::string sn = base.substr(0, 8 - tail.size()) + tail;
      if (taken.count(ext.empty() ? sn : sn + "." + ext)) continue;
      std::memset(short_name, ' ', 11);
      std::memcpy(short_name, sn.data(), sn.size());
      std::memcpy(short_name + 8, ext.data(), ext.size());
      found = true;
    }
    if (!found) return fail("no free short name");
  }
  u8 sum = 0;
  for (int k = 0; k < 11; ++k) sum = static_cast<u8>(((sum & 1) << 7) + (sum >> 1) + short_name[k]);
  const u32 pieces = static_cast<u32>((wide.size() + 12) / 13);
  raws.assign((pieces + 1) * 32, 0);
  for (u32 p = 0; p < pieces; ++p) {
    u8* e = &raws[(pieces - 1 - p) * 32];            // stored last piece first
    e[0] = static_cast<u8>((p + 1) | (p + 1 == pieces ? 0x40 : 0));
    e[11] = 0x0F;
    e[13] = sum;
    int slot = 0;
    for (int k : {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30}) {
      const size_t ci = p * 13 + slot++;
      const u16 ch = ci < wide.size() ? static_cast<u16>(wide[ci]) : ci == wide.size() ? 0x0000 : 0xFFFF;
      wr16(e + k, ch);
    }
  }
  std::memcpy(&raws[pieces * 32], short_name, 11);
  return true;
}

bool FatVolume::put(const std::string& path, const u8* data, u32 len, bool with_data, Entry* out, std::string* err, const Stamp* stamp) {
  auto fail = [&](const std::string& m) { if (err) *err = path + ": " + m; return false; };
  Entry parent;
  std::string name;
  if (!split(path, parent, name, err)) return false;
  Entry existing;
  const bool exists = lookup(path, existing);
  if (exists && existing.dir()) return fail("is a directory");
  std::vector<u8> raws;
  std::string why;
  if (!exists && !make_entries(parent, name, raws, &why)) return fail(why);

  const u32 cs = cluster_bytes();
  const u32 need = static_cast<u32>((static_cast<u64>(len) + cs - 1) / cs);
  const u32 have = exists ? static_cast<u32>(chain(existing.cluster).size()) : 0;
  if (need > have && free_count_ < need - have) return fail("not enough free space");

  u32 first = exists ? existing.cluster : 0;
  if (need != have) {
    if (exists && existing.cluster) free_chain(existing.cluster);
    if (!alloc_chain(need, first)) return fail("allocation failed");
  }
  if (with_data) {
    std::vector<u8> buf(bps_);
    const std::vector<u32> cl = chain(first);
    for (u32 k = 0; k < need; ++k)
      for (u32 s = 0; s < cs; s += bps_) {
        const u64 at = static_cast<u64>(k) * cs + s;
        std::memset(buf.data(), 0, bps_);
        if (at < len) std::memcpy(buf.data(), data + at, static_cast<size_t>(std::min<u64>(bps_, len - at)));
        write_(cluster_offset(cl[k]) + s, bps_, buf.data());
      }
  }
  fat_flush();

  const u16 date = stamp ? stamp->date : kFatDate, time = stamp ? stamp->time : 0;
  if (exists) {
    u8 b[512];
    const u64 sec = existing.dirent / bps_ * bps_;
    read_(sec, bps_, b);
    u8* e = b + (existing.dirent - sec);
    set_entry_cluster(e, first);
    wr32(e + 28, len);
    if (stamp) { wr16(e + 22, time); wr16(e + 24, date); }
    write_(sec, bps_, b);
  } else {
    u8* raw = &raws[raws.size() - 32];
    raw[11] = 0x20;   // archive
    wr16(raw + 14, time); wr16(raw + 16, date);   // created
    wr16(raw + 18, date);                         // accessed
    wr16(raw + 22, time); wr16(raw + 24, date);   // modified
    set_entry_cluster(raw, first);
    wr32(raw + 28, len);
    u64 dirent;
    if (!add_entry(parent, raws.data(), static_cast<u32>(raws.size() / 32), dirent)) { free_chain(first); fat_flush(); return fail("directory is full"); }
    fat_flush();
  }
  if (out && !lookup(path, *out)) return fail("the new entry cannot be found");
  return true;
}

bool FatVolume::write(const std::string& path, const u8* data, u32 len, std::string* err, const Stamp* stamp) {
  return put(path, data, len, true, nullptr, err, stamp);
}

bool FatVolume::create(const std::string& path, u32 len, Entry* out, std::string* err, const Stamp* stamp) {
  return put(path, nullptr, len, false, out, err, stamp);
}

bool FatVolume::mkdir(const std::string& path, std::string* err, const Stamp* stamp) {
  auto fail = [&](const std::string& m) { if (err) *err = path + ": " + m; return false; };
  Entry e;
  if (lookup(path, e)) return e.dir() ? true : fail("exists as a file");
  Entry parent;
  std::string name;
  if (!split(path, parent, name, err)) return false;
  std::vector<u8> raws;
  std::string why;
  if (!make_entries(parent, name, raws, &why)) return fail(why);
  u32 c;
  if (!alloc_chain(1, c)) return fail("not enough free space");

  write_dir_cluster(c, parent.dirent == 0 ? 0 : parent.cluster, stamp);
  u8* raw = &raws[raws.size() - 32];
  const u16 date = stamp ? stamp->date : kFatDate, time = stamp ? stamp->time : 0;
  raw[11] = 0x10;
  wr16(raw + 14, time); wr16(raw + 16, date); wr16(raw + 18, date); wr16(raw + 22, time); wr16(raw + 24, date);
  set_entry_cluster(raw, c);
  u64 dirent;
  if (!add_entry(parent, raws.data(), static_cast<u32>(raws.size() / 32), dirent)) { free_chain(c); fat_flush(); return fail("directory is full"); }
  fat_flush();
  return true;
}

// A new directory's cluster: "." and "..", rest zero. ".." names the root as cluster 0.
void FatVolume::write_dir_cluster(u32 cluster, u32 parent_cluster, const Stamp* stamp) {
  const u16 date = stamp ? stamp->date : kFatDate, time = stamp ? stamp->time : 0;
  auto dot = [&](u8* p, const char* n, u32 c) {
    std::memset(p, ' ', 11);
    std::memcpy(p, n, std::strlen(n));
    p[11] = 0x10;
    wr16(p + 14, time); wr16(p + 16, date); wr16(p + 18, date); wr16(p + 22, time); wr16(p + 24, date);
    set_entry_cluster(p, c);
  };
  u8 sec[512] = {};   // bps_ is always 512 (open() refuses anything else)
  dot(sec, ".", cluster);
  dot(sec + 32, "..", parent_cluster);
  write_(cluster_offset(cluster), bps_, sec);
  if (fresh_) return;
  std::memset(sec, 0, sizeof sec);
  for (u32 s = bps_; s < cluster_bytes(); s += bps_) write_(cluster_offset(cluster) + s, bps_, sec);
}

bool FatVolume::populate(const Entry& dir, std::vector<NewEntry>& items, std::string* err) {
  const u32 first = dir.cluster ? dir.cluster : root_cluster_;
  const u32 cs = cluster_bytes();
  std::vector<u8> d = dir_bytes(dir);
  std::unordered_set<std::string> taken;
  for (const Entry& e : list(dir)) taken.insert(e.name);
  size_t used = 0;
  while (used + 32 <= d.size() && d[used] != 0x00) used += 32;

  std::vector<u8> buf;
  std::vector<std::pair<size_t, size_t>> placed;   // item, offset of its short entry in buf
  for (size_t k = 0; k < items.size(); ++k) {
    NewEntry& it = items[k];
    it.ok = false;
    std::vector<u8> raws;
    if (!make_entries_in(taken, it.name, raws, &it.why)) continue;
    const u32 need = it.dir ? 1 : static_cast<u32>((static_cast<u64>(it.size) + cs - 1) / cs);
    u32 c = 0;
    if (need && !alloc_chain(need, c)) { it.why = "not enough free space"; continue; }
    if (it.dir) write_dir_cluster(c, dir.dirent == 0 ? 0 : dir.cluster, &it.stamp);
    u8* raw = &raws[raws.size() - 32];
    raw[11] = it.dir ? 0x10 : 0x20;
    wr16(raw + 14, it.stamp.time); wr16(raw + 16, it.stamp.date); wr16(raw + 18, it.stamp.date);
    wr16(raw + 22, it.stamp.time); wr16(raw + 24, it.stamp.date);
    set_entry_cluster(raw, c);
    wr32(raw + 28, it.dir ? 0 : it.size);
    std::string sn(reinterpret_cast<const char*>(raw), 8), ext(reinterpret_cast<const char*>(raw + 8), 3);
    sn.erase(sn.find_last_not_of(' ') + 1);
    ext.erase(ext.find_last_not_of(' ') + 1);
    it.out = Entry{};
    it.out.name = ext.empty() ? sn : sn + "." + ext;
    if (raws.size() > 32) it.out.long_name = it.name;
    it.out.attr = raw[11];
    it.out.cluster = c;
    it.out.size = it.dir ? 0 : it.size;
    it.out.mdate = it.stamp.date;
    it.out.mtime = it.stamp.time;
    taken.insert(it.out.name);
    placed.emplace_back(k, buf.size() + raws.size() - 32);
    buf.insert(buf.end(), raws.begin(), raws.end());
  }

  auto undo = [&](const char* m) {
    for (auto [k, at] : placed) if (items[k].out.cluster) free_chain(items[k].out.cluster);
    fat_flush();
    if (err) *err = m;
    return false;
  };
  const size_t total = used + buf.size();
  std::vector<u32> cl;
  if (first == 0) {
    if (total > static_cast<size_t>(root_entries_) * 32) return undo("too many entries for the FAT16 root directory");
  } else {
    cl = chain(first);
    const size_t need = (total + cs - 1) / cs;
    if (need > cl.size()) {
      u32 more;
      if (!alloc_chain(static_cast<u32>(need - cl.size()), more)) return undo("not enough free space for the directory");
      fat_set(cl.back(), more);
      const std::vector<u32> tail = chain(more);
      cl.insert(cl.end(), tail.begin(), tail.end());
    }
    d.resize(cl.size() * cs, 0);
  }
  if (d.size() < total) d.resize(total, 0);
  std::memcpy(d.data() + used, buf.data(), buf.size());
  auto at = [&](size_t i) { return first ? cluster_offset(cl[i / cs]) + i % cs : root_off_ + i; };
  for (size_t sec = used / bps_ * bps_; sec < total; sec += bps_) write_(at(sec), bps_, d.data() + sec);
  fat_flush();
  for (auto [k, off] : placed) {
    items[k].out.dirent = at(used + off);
    items[k].ok = true;
  }
  return true;
}

bool FatVolume::remove(const std::string& path, std::string* err) {
  Entry e;
  if (!lookup(path, e)) return true;
  if (e.dirent == 0) { if (err) *err = "cannot remove the root"; return false; }
  if (e.dir() && e.cluster) {
    for (const Entry& c : list(e)) {
      if (c.name == "." || c.name == "..") continue;
      if (!remove(path + "/" + c.name, err)) return false;
    }
  }
  if (e.cluster) free_chain(e.cluster);
  fat_flush();
  u8 raw[32];
  const u64 sec = e.dirent / bps_ * bps_;
  u8 b[512];
  read_(sec, bps_, b);
  std::memcpy(raw, b + (e.dirent - sec), 32);
  raw[0] = 0xE5;
  write_entry(e.dirent, raw);
  return true;
}

// ---- NandFs ------------------------------------------------------------------

namespace {
void bswap16(u8* dst, const u8* src) { for (int i = 0; i < 16; ++i) dst[i] = src[15 - i]; }
}  // namespace

void NandFs::setup_crypto(NandImage& nand, const u8* bios7i) {
  nand_ = &nand;
  const u64 id = nand.console_id();
  const u32 lo = static_cast<u32>(id), hi = static_cast<u32>(id >> 32);
  u8 kx[16], ky[16], tmp[16];

  wr32(kx, lo); wr32(kx + 4, lo ^ 0x24EE6906); wr32(kx + 8, hi ^ 0xE65B601D); wr32(kx + 12, hi);
  wr32(ky, 0x0AB9DC76); wr32(ky + 4, 0xBD4DC4D3); wr32(ky + 8, 0x202DDD1D); wr32(ky + 12, 0xE1A00005);
  DsiAes::derive_normal_key(kx, ky, tmp);
  u8 key[16];
  bswap16(key, tmp);
  AES_init_ctx(&fat_ctx_, key);
  u8 digest[20];
  crypto::sha1(nand.emmc_cid(), 16, digest);
  bswap16(fat_iv_, digest);

  es_key_ok_ = bios7i != nullptr;   // ES key, from the DSi ARM7 BIOS
  if (es_key_ok_) {
    wr32(kx, 0x4E00004A); wr32(kx + 4, 0x4A00004E); wr32(kx + 8, hi ^ 0xC80C4B72); wr32(kx + 12, lo);
    DsiAes::derive_normal_key(kx, bios7i + 0x8308, tmp);
    bswap16(es_key_, tmp);
  }
}

bool NandFs::mount(NandImage& nand, const u8* bios7i, std::string* err) {
  setup_crypto(nand, bios7i);
  u8 mbr[512];
  crypt_read(0, 512, mbr);
  if (mbr[0x1FE] != 0x55 || mbr[0x1FF] != 0xAA) { if (err) *err = "the MBR did not decrypt (wrong console ID or CID in the footer)"; return false; }
  auto part = [&](int i, FatVolume& vol, bool required) {
    const u8* p = mbr + 0x1BE + i * 16;
    const u64 base = static_cast<u64>(rd32(p + 8)) * 512;
    (i == 0 ? main_base_ : photo_base_) = base;
    if (!p[4] || !rd32(p + 12)) return !required;
    std::string why;
    const bool ok = vol.open([this, base](u64 o, u32 n, u8* out) { crypt_read(base + o, n, out); },
                             [this, base](u64 o, u32 n, const u8* in) { crypt_write(base + o, n, in); }, &why);
    if (!ok && required && err) *err = "partition " + std::to_string(i) + ": " + why;
    return ok || !required;
  };
  if (!part(0, main_, true)) return false;
  part(1, photo_, false);
  return true;
}

bool NandFs::format(NandImage& nand, const u8* bios7i, std::string* err) {
  if (nand.length() < kImageBytes) { if (err) *err = "the image is smaller than a DSi NAND"; return false; }
  setup_crypto(nand, bios7i);
  const std::vector<u8> zero(512, 0);

  // A retail DSi's partition table: main FAT16, photo, and a small FAT12 nothing mounts.
  u8 mbr[512] = {};
  static const u8 kParts[3][16] = {
    {0x00, 0x03, 0x18, 0x04, 0x06, 0x0F, 0xE0, 0x3B, 0x77, 0x08, 0x00, 0x00, 0x89, 0x6F, 0x06, 0x00},
    {0x00, 0x02, 0xCE, 0x3C, 0x06, 0x0F, 0xE0, 0xBE, 0x4D, 0x78, 0x06, 0x00, 0xB3, 0x05, 0x01, 0x00},
    {0x00, 0x02, 0xDE, 0xBF, 0x01, 0x0F, 0xE0, 0xBF, 0x5D, 0x7E, 0x07, 0x00, 0xA3, 0x01, 0x00, 0x00},
  };
  for (int i = 0; i < 3; ++i) std::memcpy(mbr + 0x1BE + i * 16, kParts[i], 16);
  mbr[0x1FE] = 0x55; mbr[0x1FF] = 0xAA;
  crypt_write(0, 512, mbr);

  // Each FAT16 partition's boot sector (OEM "TWL", 32 sectors/cluster, 512 root entries) plus
  // empty FATs and root directory.
  auto fat16 = [&](int index, u16 fat_sectors) {
    const u8* p = kParts[index];
    const u32 start = rd32(p + 8), sectors = rd32(p + 12);
    const u64 base = static_cast<u64>(start) * 512;
    u8 b[512] = {};
    b[0] = 0xE9;
    std::memcpy(b + 3, "TWL     ", 8);
    wr16(b + 0x0B, 512); b[0x0D] = 32; wr16(b + 0x0E, 1); b[0x10] = 2; wr16(b + 0x11, 512);
    b[0x15] = 0xF8; wr16(b + 0x16, fat_sectors); wr16(b + 0x18, 32); wr16(b + 0x1A, 16);
    wr32(b + 0x1C, start); wr32(b + 0x20, sectors);
    b[0x24] = static_cast<u8>(index); b[0x26] = 0x29; wr32(b + 0x27, 0x12345678);
    std::memset(b + 0x2B, ' ', 11);
    b[0x1FE] = 0x55; b[0x1FF] = 0xAA;
    crypt_write(base, 512, b);
    for (u32 f = 0; f < 2; ++f) {
      const u64 fat = base + 512 + static_cast<u64>(f) * fat_sectors * 512;
      for (u32 s = 0; s < fat_sectors; ++s) crypt_write(fat + static_cast<u64>(s) * 512, 512, zero.data());
      const u8 head[4] = {0xF8, 0xFF, 0xFF, 0xFF};
      u8 first[512] = {};
      std::memcpy(first, head, 4);
      crypt_write(fat, 512, first);
    }
    const u64 root = base + 512 + 2ull * fat_sectors * 512;
    for (u32 s = 0; s < 32; ++s) crypt_write(root + static_cast<u64>(s) * 512, 512, zero.data());
  };
  fat16(0, 52);
  fat16(1, 9);
  return mount(nand, bios7i, err);
}

// AES-CTR over byte-reversed 16-byte blocks; counter is the base IV plus block position, so
// any sector decrypts on its own.
void NandFs::xcrypt(u64 offset, u8* buf, u32 len) const {
  for (u32 i = 0; i < len; i += 16) {
    u8 ctr[16];
    std::memcpy(ctr, fat_iv_, 16);
    u64 add = (offset + i) >> 4;
    unsigned carry = 0;
    for (int k = 15; k >= 0; --k) {
      const unsigned v = ctr[k] + static_cast<unsigned>(add & 0xFF) + carry;
      ctr[k] = static_cast<u8>(v);
      carry = v >> 8;
      add >>= 8;
    }
    AES_ECB_encrypt(&fat_ctx_, ctr);
    for (int k = 0; k < 16; ++k) buf[i + k] ^= ctr[15 - k];
  }
}

void NandFs::crypt_read(u64 offset, u32 len, u8* out) {
  nand_->peek(offset, len, out);
  xcrypt(offset, out, len);
}

void NandFs::crypt_write(u64 offset, u32 len, const u8* in) {
  std::vector<u8> buf(in, in + len);
  xcrypt(offset, buf.data(), len);
  nand_->poke(offset, len, buf.data());
}

void NandFs::es_encrypt(u8* data, u32 len, const u8 nonce[12]) const {
  for (int i = 0; i < 12; ++i) data[len + 0x1C - i] = nonce[i];
  AES_ctx ctx;
  u8 iv[16], mac[16];
  iv[0] = 0x02;
  for (int i = 0; i < 12; ++i) iv[1 + i] = data[len + 0x1C - i];
  iv[13] = 0x00; iv[14] = 0x00; iv[15] = 0x01;
  AES_init_ctx_iv(&ctx, es_key_, iv);

  const u32 blklen = (len + 0xF) & ~0xFu;
  mac[0] = 0x3A;
  for (int i = 1; i < 13; ++i) mac[i] = iv[i];
  mac[13] = static_cast<u8>(blklen >> 16); mac[14] = static_cast<u8>(blklen >> 8); mac[15] = static_cast<u8>(blklen);
  AES_ECB_encrypt(&ctx, mac);

  const u32 coarse = len & ~0xFu;
  for (u32 i = 0; i < coarse; i += 16) {
    u8 tmp[16];
    bswap16(tmp, &data[i]);
    for (int k = 0; k < 16; ++k) mac[k] ^= tmp[k];
    AES_CTR_xcrypt_buffer(&ctx, tmp, 16);
    AES_ECB_encrypt(&ctx, mac);
    bswap16(&data[i], tmp);
  }
  if (const u32 rem = len - coarse) {
    u8 r[16] = {};
    for (u32 i = 0; i < rem; ++i) r[15 - i] = data[coarse + i];
    for (int k = 0; k < 16; ++k) mac[k] ^= r[k];
    AES_CTR_xcrypt_buffer(&ctx, r, 16);
    AES_ECB_encrypt(&ctx, mac);
    for (u32 i = 0; i < rem; ++i) data[coarse + i] = r[15 - i];
  }
  ctx.Iv[13] = 0x00; ctx.Iv[14] = 0x00; ctx.Iv[15] = 0x00;
  AES_CTR_xcrypt_buffer(&ctx, mac, 16);
  bswap16(&data[len], mac);

  u8 footer[16] = {};
  iv[0] = 0x00; iv[1] = 0x00; iv[2] = 0x00;
  for (int i = 0; i < 12; ++i) iv[3 + i] = data[len + 0x1C - i];
  iv[15] = 0x00;
  footer[15] = 0x3A;
  footer[2] = static_cast<u8>(len >> 16); footer[1] = static_cast<u8>(len >> 8); footer[0] = static_cast<u8>(len);
  AES_ctx_set_iv(&ctx, iv);
  AES_CTR_xcrypt_buffer(&ctx, footer, 16);
  data[len + 0x10] = footer[15];
  data[len + 0x1D] = footer[2];
  data[len + 0x1E] = footer[1];
  data[len + 0x1F] = footer[0];
}

bool NandFs::es_decrypt(u8* data, u32 len) const {
  AES_ctx ctx;
  u8 iv[16], mac[16];
  iv[0] = 0x02;
  for (int i = 0; i < 12; ++i) iv[1 + i] = data[len + 0x1C - i];
  iv[13] = 0x00; iv[14] = 0x00; iv[15] = 0x01;
  AES_init_ctx_iv(&ctx, es_key_, iv);

  const u32 blklen = (len + 0xF) & ~0xFu;
  mac[0] = 0x3A;
  for (int i = 1; i < 13; ++i) mac[i] = iv[i];
  mac[13] = static_cast<u8>(blklen >> 16); mac[14] = static_cast<u8>(blklen >> 8); mac[15] = static_cast<u8>(blklen);
  AES_ECB_encrypt(&ctx, mac);

  const u32 coarse = len & ~0xFu;
  for (u32 i = 0; i < coarse; i += 16) {
    u8 tmp[16];
    bswap16(tmp, &data[i]);
    AES_CTR_xcrypt_buffer(&ctx, tmp, 16);
    for (int k = 0; k < 16; ++k) mac[k] ^= tmp[k];
    AES_ECB_encrypt(&ctx, mac);
    bswap16(&data[i], tmp);
  }
  if (const u32 rem = len - coarse) {
    const u32 ivnum = (coarse >> 4) + 1;
    iv[13] = static_cast<u8>(ivnum >> 16); iv[14] = static_cast<u8>(ivnum >> 8); iv[15] = static_cast<u8>(ivnum);
    u8 r[16] = {};
    AES_ctx_set_iv(&ctx, iv);
    AES_CTR_xcrypt_buffer(&ctx, r, 16);
    for (u32 i = 0; i < rem; ++i) r[15 - i] = data[coarse + i];
    AES_ctx_set_iv(&ctx, iv);
    AES_CTR_xcrypt_buffer(&ctx, r, 16);
    for (int k = 0; k < 16; ++k) mac[k] ^= r[k];
    AES_ECB_encrypt(&ctx, mac);
    for (u32 i = 0; i < rem; ++i) data[coarse + i] = r[15 - i];
  }
  ctx.Iv[13] = 0x00; ctx.Iv[14] = 0x00; ctx.Iv[15] = 0x00;
  AES_CTR_xcrypt_buffer(&ctx, mac, 16);

  u8 footer[16];
  iv[0] = 0x00; iv[1] = 0x00; iv[2] = 0x00;
  for (int i = 0; i < 12; ++i) iv[3 + i] = data[len + 0x1C - i];
  iv[15] = 0x00;
  bswap16(footer, &data[len + 0x10]);
  AES_ctx_set_iv(&ctx, iv);
  AES_CTR_xcrypt_buffer(&ctx, footer, 16);
  data[len + 0x10] = footer[15];
  data[len + 0x1D] = footer[2];
  data[len + 0x1E] = footer[1];
  data[len + 0x1F] = footer[0];
  if ((footer[0] | (footer[1] << 8) | (footer[2] << 16)) != static_cast<int>(len)) return false;
  for (int i = 0; i < 16; ++i)
    if (data[len + i] != mac[15 - i]) return false;
  return true;
}

}  // namespace ds::io
