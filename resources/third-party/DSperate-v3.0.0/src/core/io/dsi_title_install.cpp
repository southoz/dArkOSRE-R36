// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/io/dsi_title_install.h"
#include "core/io/dsi_nand_fs.h"
#include "core/io/dsi_sd.h"
#include "core/cart/rom_source.h"
#include "core/cart/zip.h"
#include "core/crypto/sha1.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>

namespace ds::io {
namespace {

u32 rd32le(const u8* p) { return static_cast<u32>(p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<u32>(p[3]) << 24)); }
u64 rd64le(const u8* p) { return rd32le(p) | (static_cast<u64>(rd32le(p + 4)) << 32); }
u32 rd32be(const u8* p) { return (static_cast<u32>(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
u16 rd16be(const u8* p) { return static_cast<u16>((p[0] << 8) | p[1]); }
u64 rd64be(const u8* p) { return (static_cast<u64>(rd32be(p)) << 32) | rd32be(p + 4); }
void wr16le(u8* p, u16 v) { p[0] = static_cast<u8>(v); p[1] = static_cast<u8>(v >> 8); }
void wr32le(u8* p, u32 v) { wr16le(p, static_cast<u16>(v)); wr16le(p + 2, static_cast<u16>(v >> 16)); }
void wr16be(u8* p, u16 v) { p[0] = static_cast<u8>(v >> 8); p[1] = static_cast<u8>(v); }
void wr32be(u8* p, u32 v) { wr16be(p, static_cast<u16>(v >> 16)); wr16be(p + 2, static_cast<u16>(v)); }

constexpr u32 kDsiWareHigh = 0x00030004;
constexpr u64 kDsiWareQuota = 1024ull * 128 * 1024;   // 1024 blocks of 128 KB
constexpr u32 kTmdSize = 520, kTicketSize = 0x2C4, kTicketBody = 0x2A4;

std::string hex8(u32 v) { char b[9]; std::snprintf(b, sizeof b, "%08x", v); return b; }

bool is_dsiware(const std::vector<u8>& s, std::string* err) {
  if (s.size() < 0x1000) { if (err) *err = "too small for a DSi header"; return false; }
  if (!(s[0x12] & 2)) { if (err) *err = "not a DSi title (unit code " + std::to_string(s[0x12]) + ")"; return false; }
  if (rd32le(&s[0x234]) != kDsiWareHigh) {
    if (err) *err = "a DSi-enhanced cartridge, not DSiWare (title ID high " + hex8(rd32le(&s[0x234])) + ")";
    return false;
  }
  return true;
}

}  // namespace

bool read_dsiware(const std::string& path, std::vector<u8>& srl, std::string* err, std::vector<u8>* embedded_tmd) {
  if (embedded_tmd) embedded_tmd->clear();
  std::ifstream f(path, std::ios::binary);
  if (!f) { if (err) *err = path + ": cannot open"; return false; }
  std::vector<u8> b(std::istreambuf_iterator<char>(f), {});
  std::string why;

  if (cart::is_zip(b.data(), b.size())) {
    std::vector<u8> inner;
    std::string zerr, chosen;
    if (!cart::extract_rom(b.data(), b.size(), inner, zerr, &chosen, /*allow_cia=*/true)) {
      if (err) *err = path + ": " + zerr;
      return false;
    }
    b = std::move(inner);
  }

  // CIA layout: header, certs, ticket, TMD, then content, each padded to 64
  // bytes. A DSiWare CIA's only content is the SRL itself.
  const bool cia = b.size() >= 0x20 && rd32le(&b[0]) == 0x2020;
  if (cia) {
    auto al = [](u64 x) { return (x + 0x3F) & ~u64{0x3F}; };
    const u64 tmd = al(al(al(rd32le(&b[0])) + rd32le(&b[8])) + rd32le(&b[12]));
    if (tmd + 4 > b.size()) { if (err) *err = path + ": truncated CIA"; return false; }
    const u64 content = al(tmd + rd32le(&b[16]));
    const u64 size = rd64le(&b[24]);
    // Content chunk record: in a 3DS TMD, after signature block (0x140),
    // header (0xC4) and 64 content info records (0x900); in a DSi TMD (the
    // one the launcher accepts), straight after the header at 0x1E4.
    const u32 sig = rd32be(&b[tmd]);
    const u64 rec = sig == 0x00010001 ? tmd + 0x1E4 : tmd + 0x204 + 0x900;
    if ((sig != 0x00010004 && sig != 0x00010001) || rec + 0x10 > b.size() || content + size > b.size()) {
      if (err) *err = path + ": not a CIA this reads (unexpected TMD signature type or truncated)";
      return false;
    }
    if (rd16be(&b[rec + 6]) & 1) {
      if (err) *err = path + ": its content is encrypted with a title key; extract the plain SRL first";
      return false;
    }
    srl.assign(b.begin() + static_cast<long>(content), b.begin() + static_cast<long>(content + std::min<u64>(size, rd64be(&b[rec + 8]))));
    if (embedded_tmd && sig == 0x00010001 && tmd + kTmdSize <= b.size())
      embedded_tmd->assign(b.begin() + static_cast<long>(tmd), b.begin() + static_cast<long>(tmd + kTmdSize));
  } else {
    srl = std::move(b);
  }
  if (!is_dsiware(srl, &why)) { if (err) *err = path + ": " + why; return false; }
  return true;
}

std::vector<u8> make_dsi_save(u32 len) {
  if (len == 0 || len < 0x200 || len > 0x8000000) return {};
  const u32 sectorsize = 0x200;
  const u32 maxsectors = len / sectorsize;
  u32 tracksize = 1, headcount = 1, totsec16 = 0, next = 0;
  while (next <= maxsectors) {
    next = tracksize * (headcount + 1) * (headcount + 1);
    if (next <= maxsectors) {
      headcount++;
      totsec16 = next;
      tracksize++;
      next = tracksize * headcount * headcount;
      if (next <= maxsectors) totsec16 = next;
    }
  }
  next = (tracksize + 1) * headcount * headcount;
  if (next <= maxsectors) { tracksize++; totsec16 = next; }

  const u32 maxfiles = len < 0x8C000 ? 0x20 : 0x200;
  const u32 clustersize = totsec16 > (8u << 10) ? 8 : (totsec16 > (1u << 10) ? 4 : 1);
  auto align = [](u32 v, u32 a) { return v % a ? v + a - v % a : v; };
  const u32 totalclusters = align(totsec16, clustersize) / clustersize;
  const u32 fatbytes = align(totalclusters, 2) / 2 * 3;
  const u32 fatsz16 = align(fatbytes, sectorsize) / sectorsize;

  std::vector<u8> d(len, 0);
  d[0x000] = 0xE9;
  std::memcpy(&d[0x003], "MSWIN4.1", 8);
  wr16le(&d[0x00B], static_cast<u16>(sectorsize));
  d[0x00D] = static_cast<u8>(clustersize);
  wr16le(&d[0x00E], 1);
  d[0x010] = 2;
  wr16le(&d[0x011], static_cast<u16>(maxfiles));
  wr16le(&d[0x013], static_cast<u16>(totsec16));
  d[0x015] = 0xF8;
  wr16le(&d[0x016], static_cast<u16>(fatsz16));
  wr16le(&d[0x018], static_cast<u16>(tracksize));
  wr16le(&d[0x01A], static_cast<u16>(headcount));
  d[0x024] = 0x05;
  d[0x026] = 0x29;
  wr32le(&d[0x027], 0x12345678);
  std::memcpy(&d[0x02B], "VOLUMELABEL", 11);
  std::memcpy(&d[0x036], "FAT12   ", 8);
  wr16le(&d[0x1FE], 0xAA55);
  return d;
}

namespace {

// "4b533345" -> "KS3E" (hex kept when not printable).
std::string code_of(const std::string& id) {
  std::string c;
  for (size_t i = 0; i + 1 < id.size(); i += 2) {
    const int v = std::stoi(id.substr(i, 2), nullptr, 16);
    if (v < 0x20 || v > 0x7E) return id;
    c += static_cast<char>(v);
  }
  return c;
}

}  // namespace

bool check_signed_tmd(const std::vector<u8>& tmd, const std::vector<u8>& srl, std::string* err) {
  auto fail = [&](const std::string& m) { if (err) *err = m; return false; };
  if (tmd.size() < kTmdSize) return fail("shorter than a DSi TMD");
  if (rd32be(&tmd[0]) != 0x00010001 || std::memcmp(&tmd[0x140], "Root-CA00000001", 15) != 0) return fail("not signed for the DSi (a 3DS or unsigned TMD)");
  if (rd32be(&tmd[0x18C]) != kDsiWareHigh || rd32be(&tmd[0x190]) != rd32le(&srl[0x230])) return fail("for another title (" + hex8(rd32be(&tmd[0x18C])) + "/" + hex8(rd32be(&tmd[0x190])) + ")");
  if (rd16be(&tmd[0x1DE]) != 1) return fail("does not describe exactly one content");
  if (rd64be(&tmd[0x1EC]) != srl.size()) return fail("for a different version of the title (content is " + std::to_string(rd64be(&tmd[0x1EC])) + " bytes, this one " + std::to_string(srl.size()) + ")");
  u8 digest[20];
  crypto::sha1(srl.data(), srl.size(), digest);
  if (std::memcmp(digest, &tmd[0x1F4], 20) != 0) return fail("its content hash does not match this dump (another version, or a modified SRL)");
  return true;
}

std::string nus_tmd_url(u32 title_lo) {
  char b[96];
  std::snprintf(b, sizeof b, "http://nus.cdn.t.shop.nintendowifi.net/ccs/download/00030004%08X/tmd", title_lo);
  return b;
}

std::vector<u8> find_signed_tmd(const std::vector<u8>& srl, const std::vector<u8>& embedded, const std::string& cache_path,
                                const std::string& beside_rom, const TmdFetch& fetch, std::vector<std::string>& log) {
  std::string why;
  auto usable = [&](const std::vector<u8>& t, const std::string& from) {
    if (t.empty()) return false;
    if (check_signed_tmd(t, srl, &why)) { log.push_back("TMD from " + from); return true; }
    log.push_back("TMD from " + from + " not usable: " + why);
    return false;
  };
  auto file = [](const std::string& p) {
    std::vector<u8> v;
    if (p.empty()) return v;
    std::ifstream f(p, std::ios::binary);
    if (f) v.assign(std::istreambuf_iterator<char>(f), {});
    return v;
  };
  if (usable(embedded, "the CIA")) return {embedded.begin(), embedded.begin() + kTmdSize};
  std::vector<u8> t = file(cache_path);
  if (usable(t, cache_path)) return {t.begin(), t.begin() + kTmdSize};
  if (fetch) {
    const std::string url = nus_tmd_url(rd32le(&srl[0x230]));
    t.clear();
    if (fetch(url, t) && usable(t, url)) {
      t.resize(kTmdSize);
      if (!cache_path.empty()) {
        std::ofstream o(cache_path, std::ios::binary | std::ios::trunc);
        if (o) o.write(reinterpret_cast<const char*>(t.data()), kTmdSize);
      }
      return t;
    }
    if (t.empty()) log.push_back("TMD download failed (" + url + ")");
  }
  t = file(beside_rom);
  if (usable(t, beside_rom)) return {t.begin(), t.begin() + kTmdSize};
  return {};
}

std::vector<DsiWareUsage> dsiware_usage(const FatVolume& vol) {
  std::vector<DsiWareUsage> out;
  FatVolume::Entry dir;
  if (!vol.lookup("/title/00030004", dir)) return out;
  const u32 cs = vol.cluster_bytes();
  for (const FatVolume::Entry& t : vol.list(dir)) {
    if (!t.dir() || t.name.size() != 8) continue;
    DsiWareUsage u;
    for (char ch : t.name) u.id += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    u.code = code_of(u.id);
    std::vector<FatVolume::Entry> stack{t};
    while (!stack.empty()) {
      const FatVolume::Entry d = stack.back();
      stack.pop_back();
      for (const FatVolume::Entry& x : vol.list(d)) {
        if (x.name == "." || x.name == "..") continue;
        if (x.dir()) { if (x.cluster) stack.push_back(x); }
        else u.bytes += (static_cast<u64>(x.size) + cs - 1) / cs * cs;
      }
    }
    out.push_back(std::move(u));
  }
  return out;
}

bool hide_dsiware_title(FatVolume& vol, const std::string& id, std::string* err) {
  return vol.remove("/title/00030004/" + id, err) && vol.remove("/ticket/00030004/" + id + ".tik", err);
}

TitleInstall nand_install_title(NandImage& nand, const u8* bios7i, const std::vector<u8>& srl, const std::vector<u8>& signed_tmd) {
  TitleInstall r;
  std::string err;
  if (!is_dsiware(srl, &err)) { r.message = err; return r; }
  const u32 lo = rd32le(&srl[0x230]);
  r.title_lo = lo;
  const u8 version = srl[0x1E];
  if (!check_signed_tmd(signed_tmd, srl, &err)) { r.message = "TMD: " + err; return r; }
  const std::vector<u8> tmd(signed_tmd.begin(), signed_tmd.begin() + kTmdSize);
  const u32 content_id = rd32be(&tmd[0x1E4]);
  const u32 pub = rd32le(&srl[0x238]), prv = rd32le(&srl[0x23C]);
  const bool banner = srl[0x1BF] & 0x04;

  NandFs fs;
  if (!fs.mount(nand, bios7i, &err)) { r.message = "nand: " + err; return r; }
  if (!fs.has_es_key()) { r.message = "the DSi ARM7 BIOS is needed to make the title's ticket"; return r; }
  FatVolume& vol = fs.main();
  const std::string id = hex8(lo);
  const std::string title = "/title/00030004/" + id;
  FatVolume::Entry e;
  if (vol.lookup(title + "/content/title.tmd", e)) {
    r.result = TitleInstall::Result::AlreadyInstalled;
    r.message = "already installed on the NAND";
    return r;
  }

  const u32 cs = vol.cluster_bytes();
  auto clusters = [cs](u64 n) { return static_cast<u32>((n + cs - 1) / cs); };
  const u32 need = clusters(srl.size()) + clusters(kTmdSize) + clusters(kTicketSize) + clusters(pub) + clusters(prv) +
                   (banner ? clusters(0x4000) : 0) + 5;   // five directories at most

  // If adding it would exceed kDsiWareQuota, hide the dump's own titles, largest first, to fit.
  const std::vector<DsiWareUsage> installed = dsiware_usage(vol);
  u64 used = 0;
  for (const DsiWareUsage& t : installed) used += t.bytes;
  const u64 adding = static_cast<u64>(need - 5) * cs;
  if (used + adding > kDsiWareQuota) {
    std::vector<DsiWareUsage> order = installed;
    std::sort(order.begin(), order.end(), [](const DsiWareUsage& a, const DsiWareUsage& b) { return a.bytes > b.bytes; });
    for (const DsiWareUsage& t : order) {
      if (used + adding <= kDsiWareQuota) break;
      if (!hide_dsiware_title(vol, t.id, &err)) { r.message = err; return r; }
      used -= t.bytes;
      r.hidden.push_back(t.code);
    }
    if (used + adding > kDsiWareQuota) { r.message = "the title alone is larger than the DSi's DSiWare quota"; return r; }
  }

  // Check space for all of it up front: no half-installed title.
  if (vol.free_clusters() < need) {
    r.message = "not enough free space on the NAND (" + std::to_string(need * (cs >> 10)) + " KB needed, " +
                std::to_string(vol.free_clusters() * (cs >> 10)) + " KB free)";
    return r;
  }

  u8 tik[kTicketSize] = {};
  wr32be(&tik[0x000], 0x00010001);
  std::memcpy(&tik[0x140], "Root-CA00000001-XS00000006", 26);
  wr32be(&tik[0x1DC], kDsiWareHigh);
  wr32be(&tik[0x1E0], lo);
  tik[0x1E6] = version;
  std::memset(&tik[0x222], 0xFF, 0x20);
  const u8 nonce[12] = {};
  fs.es_encrypt(tik, kTicketBody, nonce);

  auto fail = [&](const std::string& m) { r.message = m; return r; };
  for (const char* d : {"/ticket", "/ticket/00030004", "/title", "/title/00030004"})
    if (!vol.mkdir(d, &err)) return fail(err);
  if (!vol.write("/ticket/00030004/" + id + ".tik", tik, kTicketSize, &err)) return fail(err);
  for (const std::string& d : {title, title + "/content", title + "/data"})
    if (!vol.mkdir(d, &err)) return fail(err);
  if (pub) { const std::vector<u8> s = make_dsi_save(pub); if (!vol.write(title + "/data/public.sav", s.data(), static_cast<u32>(s.size()), &err)) return fail(err); }
  if (prv) { const std::vector<u8> s = make_dsi_save(prv); if (!vol.write(title + "/data/private.sav", s.data(), static_cast<u32>(s.size()), &err)) return fail(err); }
  if (banner) { const std::vector<u8> s(0x4000, 0); if (!vol.write(title + "/data/banner.sav", s.data(), 0x4000, &err)) return fail(err); }
  if (!vol.write(title + "/content/title.tmd", tmd.data(), kTmdSize, &err)) return fail(err);
  if (!vol.write(title + "/content/" + hex8(content_id) + ".app", srl.data(), static_cast<u32>(srl.size()), &err)) return fail(err);

  r.result = TitleInstall::Result::Installed;
  r.message = "installed into the session (" + std::to_string(srl.size() >> 10) + " KB)";
  if (!r.hidden.empty()) {
    r.message += "; hidden this session to fit the DSiWare quota:";
    for (const std::string& c : r.hidden) r.message += " " + c;
  }
  return r;
}

int nand_hide_installed_dsiware(NandImage& nand, const u8* bios7i, std::string* err) {
  NandFs fs;
  if (!fs.mount(nand, bios7i, err)) return -1;
  int n = 0;
  for (const DsiWareUsage& t : dsiware_usage(fs.main())) {
    if (!hide_dsiware_title(fs.main(), t.id, err)) return -1;
    ++n;
  }
  return n;
}

bool nand_has_title(NandImage& nand, const u8* bios7i, u32 title_lo) {
  NandFs fs;
  FatVolume::Entry e;
  return fs.mount(nand, bios7i) && fs.main().lookup("/title/00030004/" + hex8(title_lo) + "/content/title.tmd", e);
}

bool file_is_dsiware(const std::string& path) {
  const size_t dot = path.find_last_of('.');
  if (dot == std::string::npos) return false;
  std::string ext = path.substr(dot + 1);
  for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (ext == "cia") return true;
  if (ext == "zip") return zip_is_dsiware(path);
  if (ext != "nds" && ext != "dsi" && ext != "srl") return false;
  std::ifstream f(path, std::ios::binary);
  std::vector<u8> head(0x1000);
  if (!f.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()))) return false;
  return is_dsiware(head, nullptr);
}

namespace {
std::unique_ptr<cart::RomSource> zip_entry_of(const std::string& path, cart::ZipEntry& e) {
  std::string err;
  std::unique_ptr<cart::RomSource> src = cart::RomSource::map_file(path, err);
  if (!src || !cart::find_rom(src->page(0), src->size(), e, err)) return nullptr;
  return src;
}
}  // namespace

bool zip_is_dsiware(const std::string& path) {
  cart::ZipEntry e;
  std::unique_ptr<cart::RomSource> src = zip_entry_of(path, e);
  if (!src) return false;
  if (e.cia) return true;
  std::vector<u8> head(0x1000);
  if (cart::peek_entry(src->page(0), src->size(), e, head.data(), head.size()) != head.size()) return false;
  return is_dsiware(head, nullptr);
}

bool zip_holds_cia(const std::string& path) {
  cart::ZipEntry e;
  return zip_entry_of(path, e) != nullptr && e.cia;
}

bool nand_title_content_id(NandImage& nand, const u8* bios7i, u32 title_lo, u32& content_id) {
  NandFs fs;
  FatVolume::Entry dir;
  if (!fs.mount(nand, bios7i) || !fs.main().lookup("/title/00030004/" + hex8(title_lo) + "/content", dir) || !dir.dir()) return false;
  for (const FatVolume::Entry& e : fs.main().list(dir)) {
    if (e.dir() || e.name.size() != 12 || e.name.compare(8, 4, ".APP") != 0) continue;
    char* end = nullptr;
    const unsigned long v = std::strtoul(e.name.substr(0, 8).c_str(), &end, 16);
    if (end && *end == 0) { content_id = static_cast<u32>(v); return true; }
  }
  return false;
}

}  // namespace ds::io
