// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/io/dsi_nand_synth.h"
#include "core/io/dsi_nand_fs.h"
#include "core/io/dsi_sd.h"
#include "core/io/dsi_title_install.h"
#include "core/crypto/sha1.h"

#include <cstdio>
#include <cstring>

namespace ds::io {

const u64 kSynthConsoleId = 0x08A1202600000001ull;
// eMMC CID in DSi layout (serial first, manufacturer last), serial 1.
const u8 kSynthEmmcCid[16] = {0x01, 0x00, 0x00, 0x00, 0x2D, 0x03, 0x4D, 0x30, 0x30, 0x46, 0x50, 0x41, 0x00, 0x00, 0x15, 0x00};

namespace {

void wr16(u8* p, u16 v) { p[0] = static_cast<u8>(v); p[1] = static_cast<u8>(v >> 8); }
void wr32(u8* p, u32 v) { wr16(p, static_cast<u16>(v)); wr16(p + 2, static_cast<u16>(v >> 16)); }
u32 rd32(const u8* p) { return static_cast<u32>(p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<u32>(p[3]) << 24)); }

struct RegionInfo { u8 region, country, mask, language; char letter; const char* serial; };
// Country codes and language sets per region (USA from a dump, rest from GBATEK).
const RegionInfo kRegions[6] = {
  {0, 0x01, 0x01, 0, 'J', "TJF"},   // Japan
  {1, 0x31, 0x26, 1, 'E', "TW"},    // USA: en fr es
  {2, 0x6E, 0x3E, 1, 'P', "TEF"},   // Europe (United Kingdom): en fr de it es
  {3, 0x41, 0x02, 1, 'U', "TAF"},   // Australia
  {4, 0xA0, 0x40, 6, 'C', "TCF"},   // China
  {5, 0x88, 0x80, 7, 'K', "TKF"},   // Korea
};

// DSi settings file: SHA-1/signature, version 1, counter, length, data at 0x88, 0xFF-padded to 16 KB.
std::vector<u8> settings_file(const u8* data, u32 len, bool hashed, u8 counter) {
  std::vector<u8> f(0x4000, 0xFF);
  std::memset(f.data(), 0, 0x88);
  f[0x80] = 1;
  f[0x81] = counter;
  wr32(&f[0x84], len);
  std::memcpy(&f[0x88], data, len);
  if (hashed) crypto::sha1(data, len, f.data());
  return f;
}

void put_utf16(u8* dst, const std::string& s, u32 max_chars) {
  for (u32 i = 0; i < max_chars && i < s.size(); ++i) wr16(dst + 2 * i, static_cast<u8>(s[i]));
}

}  // namespace

DsiRegion dsi_region_for(u32 flags, u8 user_language) {
  if ((flags & 0x3F) == 0) flags = 0x3F;           // no region bits: region-free
  int pick = -1;
  // User's language picks among allowed regions; ties favor USA.
  static const int kOrder[6] = {1, 2, 0, 3, 4, 5};
  for (int k : kOrder)
    if ((flags >> k & 1) && (kRegions[k].mask >> user_language & 1)) { pick = k; break; }
  if (pick < 0) for (int k : kOrder) if (flags >> k & 1) { pick = k; break; }
  const RegionInfo& r = kRegions[pick];
  DsiRegion out;
  out.region = r.region;
  out.country = r.country;
  out.language_mask = r.mask;
  out.language = (r.mask >> user_language & 1) ? user_language : r.language;
  out.letter = r.letter;
  return out;
}

DsiConsoleFiles make_dsi_console_files(const bios::UserSettings& user, const DsiRegion& region, u64 console_id) {
  DsiConsoleFiles out;

  // TWLCFG, laid out as a retail dump's. RTC offset/alarm/last-launched title stay zero.
  u8 cfg[0x128] = {};
  wr32(cfg + 0x00, 0x0100000B);                    // flags of a set-up console
  cfg[0x05] = region.country;
  cfg[0x06] = region.language;
  wr32(cfg + 0x10, 1);
  // Touch calibration in CODEC ADC units: identity at pixel << 4.
  wr16(cfg + 0x30, 0x20 << 4); wr16(cfg + 0x32, 0x20 << 4); cfg[0x34] = 0x20; cfg[0x35] = 0x20;
  wr16(cfg + 0x36, 0xE0 << 4); wr16(cfg + 0x38, 0xA0 << 4); cfg[0x3A] = 0xE0; cfg[0x3B] = 0xA0;
  cfg[0x3C] = 0x9C; cfg[0x3D] = 0x20; cfg[0x3E] = 0x01; cfg[0x3F] = 0x02;   // unknown, as dumped
  cfg[0x44] = static_cast<u8>(user.favourite_colour & 15);
  cfg[0x46] = (user.birthday_month >= 1 && user.birthday_month <= 12) ? user.birthday_month : 1;
  cfg[0x47] = (user.birthday_day >= 1 && user.birthday_day <= 31) ? user.birthday_day : 1;
  put_utf16(cfg + 0x48, user.nickname, 10);
  put_utf16(cfg + 0x5E, user.message, 26);
  out.twlcfg = settings_file(cfg, sizeof cfg, true, 1);

  // 0x14 console-unique bytes of unknown meaning, derived from the console ID.
  u8 n[0x14] = {};
  u8 id[8], digest[20];
  for (int i = 0; i < 8; ++i) id[i] = static_cast<u8>(console_id >> (8 * i));
  crypto::sha1(id, sizeof id, digest);
  std::memcpy(n + 1, digest, 0x13);
  out.hwinfo_n = settings_file(n, sizeof n, true, 0);

  // HWINFO_S: languages, region, serial number, launcher's title ID.
  u8 s[0x1C] = {};
  wr32(s + 0x00, region.language_mask);
  s[0x08] = region.region;
  const RegionInfo& r = kRegions[region.region < 6 ? region.region : 1];
  char serial[13] = {};
  std::snprintf(serial, sizeof serial, "%s%0*u", r.serial, static_cast<int>(11 - std::strlen(r.serial)), 1u);
  std::memcpy(s + 0x09, serial, 12);
  s[0x17] = 0x3C;                                  // unknown, as dumped
  const u8 launcher[4] = {static_cast<u8>(region.letter), 'A', 'N', 'H'};
  std::memcpy(s + 0x18, launcher, 4);
  out.hwinfo_s = settings_file(s, sizeof s, false, 0);
  return out;
}

std::vector<u8> DsiConsoleFiles::boot_blobs() const {
  std::vector<u8> b;
  b.insert(b.end(), twlcfg.begin() + 0x88, twlcfg.begin() + 0x88 + 0x128);
  b.insert(b.end(), hwinfo_n.begin() + 0x88, hwinfo_n.begin() + 0x88 + 0x14);
  b.insert(b.end(), hwinfo_s.begin() + 0x88, hwinfo_s.begin() + 0x88 + 0x18);
  return b;
}

bool build_synthetic_nand(NandImage& nand, const u8* bios7i, const std::vector<u8>& srl,
                          const DsiConsoleFiles& files, std::string* err) {
  auto fail = [&](const std::string& m) { if (err) *err = m; return false; };
  if (srl.size() < 0x1000) return fail("the title is not a DSi SRL");
  nand.create_in_memory(NandFs::kImageBytes, kSynthEmmcCid, kSynthConsoleId);
  NandFs fs;
  std::string why;
  if (!fs.format(nand, bios7i, &why)) return fail("formatting the NAND: " + why);
  FatVolume& vol = fs.main();

  // Retail top-level directories.
  for (const char* d : {"/sys", "/sys/log", "/title", "/ticket", "/shared1", "/shared2", "/import", "/tmp", "/progress"})
    if (!vol.mkdir(d, &why)) return fail(why);
  auto put = [&](const std::string& path, const std::vector<u8>& data) {
    return vol.write(path, data.data(), static_cast<u32>(data.size()), &why);
  };
  if (!put("/shared1/TWLCFG0.dat", files.twlcfg) || !put("/shared1/TWLCFG1.dat", files.twlcfg) ||
      !put("/sys/HWINFO_N.dat", files.hwinfo_n) || !put("/sys/HWINFO_S.dat", files.hwinfo_s))
    return fail(why);
  if (!files.font.empty() && !put("/sys/TWLFontTable.dat", files.font)) return fail(why);
  if (fs.photo().valid()) fs.photo().mkdir("/photo", nullptr);

  // Title layout the launcher's mount table expects: content 00000000, and
  // empty saves sized from the header.
  char id[9];
  std::snprintf(id, sizeof id, "%08x", rd32(&srl[0x230]));
  char hi[9];
  std::snprintf(hi, sizeof hi, "%08x", rd32(&srl[0x234]));
  const std::string cat = std::string("/title/") + hi, title = cat + "/" + id;
  for (const std::string& d : {cat, title, title + "/content", title + "/data"})
    if (!vol.mkdir(d, &why)) return fail(why);
  const u32 pub = rd32(&srl[0x238]), prv = rd32(&srl[0x23C]);
  if (pub && !put(title + "/data/public.sav", make_dsi_save(pub))) return fail(why);
  if (prv && !put(title + "/data/private.sav", make_dsi_save(prv))) return fail(why);
  if ((srl[0x1BF] & 0x04) && !put(title + "/data/banner.sav", std::vector<u8>(0x4000, 0))) return fail(why);
  if (!put(title + "/content/00000000.app", srl)) return fail(why);
  return true;
}

}  // namespace ds::io
