// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/bios/freebios.h"

#include <cstring>

namespace ds::bios {

u16 crc16(const u8* data, u32 len, u16 start) {
  static const u16 poly[8] = {0xC0C1, 0xC181, 0xC301, 0xC601, 0xCC01, 0xD801, 0xF001, 0xA001};
  u32 crc = start;
  for (u32 i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int j = 0; j < 8; ++j) {
      const bool carry = crc & 1;
      crc >>= 1;
      if (carry) crc ^= static_cast<u32>(poly[j]) << (7 - j);
    }
  }
  return static_cast<u16>(crc);
}

namespace {
void w16(u8* p, u16 v) { p[0] = static_cast<u8>(v); p[1] = static_cast<u8>(v >> 8); }

// Field offsets follow GBATEK "DS Firmware Header" / "User Settings".
void fill_header(u8* h, u32 size, u8 console_type) {
  std::memset(h, 0, 0x200);
  std::memcpy(h + 0x08, "DSPR", 4);            // not "MACP", so it's not mistaken for a dump
  h[0x1D] = console_type;                      // 0x20 DS Lite, 0x57 DSi
  w16(h + 0x20, static_cast<u16>((size - 0x200) >> 3));   // user settings offset (/8)
  w16(h + 0x2C, 0x138);                        // wifi config length
  h[0x2F] = 6;                                 // wifi version W006
  static const u8 unused3[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
  std::memcpy(h + 0x30, unused3, 6);
  static const u8 mac[6] = {0x00, 0x09, 0xBF, 0x11, 0x22, 0x33};
  std::memcpy(h + 0x36, mac, 6);
  w16(h + 0x3C, 0x3FFE);                       // enabled channels
  h[0x3E] = 0xFF; h[0x3F] = 0xFF;
  h[0x40] = 3;                                 // RF chip type 3
  h[0x41] = 0x94;                              // RF bits per entry
  h[0x42] = 0x29;                              // RF entries
  h[0x43] = 0x02;
  static const u16 init[9] = {0x0002, 0x0017, 0x0026, 0x1818, 0x0048, 0x4840, 0x0058, 0x0042, 0x0146};
  for (u32 i = 0; i < 9; ++i) w16(h + 0x44 + 2 * i, init[i]);
  w16(h + 0x2A, crc16(h + 0x2C, 0x138, 0x0000));   // wifi config checksum
}

void put_utf16(u8* dst, const std::string& s, u32 max_chars, u16* len_out) {
  const u32 n = static_cast<u32>(s.size() < max_chars ? s.size() : max_chars);
  for (u32 i = 0; i < n; ++i) w16(dst + 2 * i, static_cast<u8>(s[i]));
  w16(reinterpret_cast<u8*>(len_out), static_cast<u16>(n));
}

void fill_user(u8* u, const UserSettings& s) {
  std::memset(u, 0, 0x100);
  w16(u + 0x00, 5);                            // version
  u[0x02] = static_cast<u8>(s.favourite_colour & 15);
  u[0x03] = (s.birthday_month >= 1 && s.birthday_month <= 12) ? s.birthday_month : 1;
  u[0x04] = (s.birthday_day >= 1 && s.birthday_day <= 31) ? s.birthday_day : 1;
  u16 len;
  put_utf16(u + 0x06, s.nickname, 10, &len); std::memcpy(u + 0x1A, &len, 2);
  put_utf16(u + 0x1C, s.message, 26, &len);  std::memcpy(u + 0x50, &len, 2);
  // Identity touch calibration, ADC<<4 form.
  w16(u + 0x58, 0); w16(u + 0x5A, 0); u[0x5C] = 0; u[0x5D] = 0;
  w16(u + 0x5E, 255 << 4); w16(u + 0x60, 191 << 4); u[0x62] = 255; u[0x63] = 191;
  w16(u + 0x64, static_cast<u16>((s.language & 7) | (7 << 3)));   // language, backlight max
  w16(u + 0x70, 0);                            // update counter
  w16(u + 0x72, crc16(u, 0x70, 0xFFFF));
}

std::vector<u8> build(const UserSettings& user, u32 size, u8 console_type) {
  std::vector<u8> fw(size, 0xFF);
  fill_header(fw.data(), size, console_type);
  fw[0x2FF] = 0x80;                            // boot0: NAND as stage-2 medium
  fill_access_point(fw.data() + size - 0x600, true, console_type == 0x57);
  fill_access_point(fw.data() + size - 0x500, false, false);
  fill_access_point(fw.data() + size - 0x400, false, false);
  for (u32 blk = 0; blk < 2; ++blk) fill_user(fw.data() + size - 0x200 + blk * 0x100, user);
  return fw;
}
} // namespace

void fill_access_point(u8* ap, bool configured, bool dsi) {
  std::memset(ap, 0, 0x100);
  if (configured) {
    std::strncpy(reinterpret_cast<char*>(ap + 0x40), kAccessPointSsid, 32);   // SSID
    ap[0xE7] = 0x00;                           // status: normal
    if (dsi) w16(ap + 0xEA, 1400);             // MTU
  } else {
    ap[0xE7] = 0xFF;                           // status: not configured
  }
  ap[0xEF] = 0x01;                             // connection configured (melonDS sets it on both)
  w16(ap + 0xFE, crc16(ap, 0xFE, 0x0000));
}

int stamp_access_point(std::vector<u8>& fw) {
  if (fw.size() < 0x20000) return -1;
  const u32 user = static_cast<u32>(fw[0x20] | (fw[0x21] << 8)) << 3;
  if (user < 0x400 || user + 0x100 > fw.size()) return -1;
  const u32 base = user - 0x400;
  const bool dsi = fw[0x1D] == 0x57;
  int free_slot = -1;
  for (int i = 0; i < 3; ++i) {
    const u8* ap = fw.data() + base + i * 0x100;
    const bool valid = crc16(ap, 0xFE, 0x0000) == static_cast<u16>(ap[0xFE] | (ap[0xFF] << 8));
    if (valid && ap[0xE7] != 0xFF) {
      if (std::strncmp(reinterpret_cast<const char*>(ap + 0x40), kAccessPointSsid, 32) == 0) return i;
    } else if (free_slot < 0) {
      free_slot = i;
    }
  }
  if (free_slot >= 0) fill_access_point(fw.data() + base + free_slot * 0x100, true, dsi);
  return free_slot;
}

std::vector<u8> generate_firmware(const UserSettings& user) { return build(user, 0x40000, 0x20); }

std::vector<u8> generate_firmware_dsi(const UserSettings& user, u8 language, u16 language_mask) {
  std::vector<u8> fw = build(user, 0x20000, 0x57);
  // Wi-Fi board/flash type of a retail DSi (board 2).
  fw[0x1FD] = 0x02;
  fw[0x1FE] = 0x20;
  // Extended user-settings block at 0x74: version, language, region mask, own CRC.
  for (u32 blk = 0; blk < 2; ++blk) {
    u8* u = fw.data() + 0x20000 - 0x200 + blk * 0x100;
    w16(u + 0x64, static_cast<u16>((language & 7) | 0xFC00));
    w16(u + 0x72, crc16(u, 0x70, 0xFFFF));
    u[0x74] = 0x01;
    u[0x75] = language;
    w16(u + 0x76, language_mask);
    w16(u + 0xFE, crc16(u + 0x74, 0x8A, 0xFFFF));
  }
  return fw;
}

} // namespace ds::bios
