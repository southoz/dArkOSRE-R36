// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "frontend/sdl/loader_cart.h"

#include "frontend/sdl/loader_icon.h"

#include <cstring>

namespace ds::sdl {
namespace {

constexpr u32 kBannerOff = 0x9000, kArm9Off = 0x8000, kArm7Off = 0x8200;
constexpr u32 kRomSize = 0x20000;

// CRC-16/ARC, as the firmware checks header/banner with.
u16 crc16(const u8* p, size_t n) {
  u16 v = 0xFFFF;
  for (size_t i = 0; i < n; ++i) {
    v ^= p[i];
    for (int b = 0; b < 8; ++b) v = (v & 1) ? (v >> 1) ^ 0xA001 : v >> 1;
  }
  return v;
}

void w16(u8* p, u32 off, u16 v) { p[off] = u8(v); p[off + 1] = u8(v >> 8); }
void w32(u8* p, u32 off, u32 v) { for (int i = 0; i < 4; ++i) p[off + i] = u8(v >> (i * 8)); }

// UTF-8 to UTF-16LE (banner slot encoding). Malformed input becomes U+FFFD.
std::vector<u16> utf16(const std::string& s) {
  std::vector<u16> out;
  for (size_t i = 0; i < s.size();) {
    const u8 c = u8(s[i]);
    u32 cp; int extra;
    if (c < 0x80) { cp = c; extra = 0; }
    else if ((c & 0xE0) == 0xC0) { cp = c & 0x1Fu; extra = 1; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0Fu; extra = 2; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07u; extra = 3; }
    else { out.push_back(0xFFFD); ++i; continue; }
    if (i + size_t(extra) >= s.size()) { out.push_back(0xFFFD); break; }
    bool ok = true;
    for (int k = 1; k <= extra; ++k) {
      const u8 t = u8(s[i + size_t(k)]);
      if ((t & 0xC0) != 0x80) { ok = false; break; }
      cp = (cp << 6) | (t & 0x3Fu);
    }
    if (!ok) { out.push_back(0xFFFD); ++i; continue; }
    i += size_t(extra) + 1;
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp < 0xE000)) cp = 0xFFFD;
    if (cp >= 0x10000) {
      cp -= 0x10000;
      out.push_back(u16(0xD800 + (cp >> 10)));
      out.push_back(u16(0xDC00 + (cp & 0x3FF)));
    } else {
      out.push_back(u16(cp));
    }
  }
  return out;
}

// ARM9/ARM7 stub: str r1,[r0] ; b . -- never actually loaded by firmware,
// just gives the header somewhere to point; `magic` distinguishes the two.
void write_stub(u8* p, u32 magic) {
  const u32 words[] = {0xE59F0008, 0xE59F1008, 0xE5801000, 0xEAFFFFFE, 0x04FFFFF0, magic};
  for (int i = 0; i < 6; ++i) w32(p, u32(i) * 4, words[i]);
}

} // namespace

std::vector<u8> build_loader_cart(const std::string& title, const std::string& subtitle) {
  std::vector<u8> rom(kRomSize, 0);

  // ---- banner ----
  u8* ban = rom.data() + kBannerOff;
  w16(ban, 0x00, 1);                                        // version 1
  std::memcpy(ban + 0x20, kLoaderIconTiles, sizeof kLoaderIconTiles);
  for (int i = 0; i < 16; ++i) w16(ban, u32(0x220 + i * 2), kLoaderIconPalette[i]);
  // Six language slots of 0x100 bytes; firmware clips beyond three lines.
  std::vector<u16> text = utf16(subtitle.empty() ? title : title + "\n" + subtitle);
  if (text.size() > 0x100 / 2 - 1) text.resize(0x100 / 2 - 1);
  for (u32 lang = 0; lang < 6; ++lang)
    for (size_t k = 0; k < text.size(); ++k) w16(ban, 0x240 + lang * 0x100 + u32(k) * 2, text[k]);
  w16(ban, 0x02, crc16(ban + 0x20, 0x840 - 0x20));

  // ---- header ----
  u8* h = rom.data();
  // 12-byte game title (not the menu display, which is the banner above).
  for (u32 i = 0; i < 12 && i < title.size(); ++i) {
    const u8 c = u8(title[i]);
    if (c == '\n') break;
    h[i] = (c >= 'a' && c <= 'z') ? u8(c - 32) : (c < 0x80 ? c : u8('?'));
  }
  std::memcpy(h + 0x0C, "#DSP", 4);                         // game code
  std::memcpy(h + 0x10, "01", 2);                           // maker code
  h[0x12] = 0x00;                                           // unit code: NDS
  h[0x14] = 0x09;                                           // device capacity
  w32(h, 0x20, kArm9Off); w32(h, 0x24, 0x02000000); w32(h, 0x28, 0x02000000); w32(h, 0x2C, 0x40);
  w32(h, 0x30, kArm7Off); w32(h, 0x34, 0x02380000); w32(h, 0x38, 0x02380000); w32(h, 0x3C, 0x40);
  w32(h, 0x68, kBannerOff);
  w16(h, 0x6E, 0x0D7E);                                     // secure area delay
  w32(h, 0x80, kRomSize);
  w32(h, 0x84, 0x4000);
  // logo block stays zeroed; firmware only checks this CRC against the constant
  w16(h, 0x15C, 0xCF56);
  w16(h, 0x15E, crc16(h, 0x15E));

  write_stub(rom.data() + kArm9Off, 0x44535039);            // "DSP9"
  write_stub(rom.data() + kArm7Off, 0x44535037);            // "DSP7"
  return rom;
}

} // namespace ds::sdl
