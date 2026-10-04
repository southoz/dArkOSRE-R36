// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Built-in replacements for the images a DS boots from when the user has no
// dumps. BIOS is FreeBIOS (BSD-3-Clause): SWI table + IRQ dispatcher, no boot
// code. Firmware is data-only. Both work only with direct boot; SWI routines
// are not cycle-matched to Nintendo's.
#pragma once
#include "core/types.h"

#include <string>
#include <vector>

namespace ds::bios {

// Smaller than the BIOS regions they fill (rest is zero). ARM9 image leaves
// the logo area at 0x20 blank; ARM7 image has no KEY1 table at 0x30.
extern const u8 kFreeBios9[];
extern const u32 kFreeBios9_len;
extern const u8 kFreeBios7[];
extern const u32 kFreeBios7_len;

// Fields the DS menu's settings pages would have written. Nickname/message
// are plain strings here; only the first 10 / 26 chars are used.
struct UserSettings {
  std::string nickname = "DSperate";
  std::string message;
  u8 birthday_month = 1;   // 1..12
  u8 birthday_day = 1;     // 1..31
  u8 favourite_colour = 0; // 0..15 (GBATEK order)
  u8 language = 1;         // 0 ja, 1 en, 2 fr, 3 de, 4 it, 5 es
};

// 256 KB firmware image: header, two user-settings copies, three wifi
// access-point blocks. No code.
std::vector<u8> generate_firmware(const UserSettings& user);
// The DSi's: 128 KB, plus extended user settings (`language`, `language_mask`).
std::vector<u8> generate_firmware_dsi(const UserSettings& user, u8 language, u16 language_mask);

// SSID of DSperate's emulated access point (io::Wifi and io::NWifi both answer under it).
inline constexpr const char* kAccessPointSsid = "DSperate-AP";
// One 0x100-byte Wi-Fi access point slot: open network under kAccessPointSsid
// with DHCP, or unconfigured.
void fill_access_point(u8* ap, bool configured, bool dsi);
// Places the emulated AP in the first unconfigured slot of three below the
// user settings, unless already present. Returns the slot (0-2), or -1.
int stamp_access_point(std::vector<u8>& firmware);

u16 crc16(const u8* data, u32 len, u16 start);

} // namespace ds::bios
