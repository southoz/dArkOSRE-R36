// SPDX-License-Identifier: GPL-3.0-or-later
// The console's own settings inside a firmware image: reading them back, and
// editing one field without disturbing the others or the checksums.
#include "core/nds.h"
#include "core/bios/freebios.h"
#include "check.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

using ds::u8;
using ds::u16;
using ds::u32;

const char* kFw = "firmware_test.bin";
const char* kOvr = "firmware_test.bin.ovr";

u16 rd16(const u8* p) { return static_cast<u16>(p[0] | (p[1] << 8)); }

// A generated image written to disk, so it loads as a dump rather than as the
// synthetic firmware: the dirty-page tracking the sidecar needs is only kept
// for a real one.
void write_dump(const ds::bios::UserSettings& u) {
  const std::vector<u8> fw = ds::bios::generate_firmware(u);
  std::ofstream f(kFw, std::ios::binary);
  f.write(reinterpret_cast<const char*>(fw.data()), static_cast<std::streamsize>(fw.size()));
}

void load(ds::NDS& nds) {
  std::string err;
  CHECK(nds.load_bios("", "", kFw, {}, &err));
  CHECK(!nds.firmware_synthetic);
}

// Both copies of the settings, checked the way the console does: the CRC16
// over the first 0x70 bytes has to match the halfword at 0x72, or the block
// is rejected and the settings silently revert.
void check_crcs(const ds::NDS& nds) {
  const u32 base = nds.user_settings_offset();
  CHECK(base != 0);
  for (u32 blk = 0; blk < 2; ++blk) {
    const u8* u = nds.firmware.data() + base + blk * 0x100;
    CHECK(rd16(u + 0x72) == ds::bios::crc16(u, 0x70, 0xFFFF));
  }
}

void test_round_trip() {
  ds::bios::UserSettings in;
  in.nickname = "Ada";
  in.message = "Hello there";
  in.favourite_colour = 11;
  in.birthday_month = 7;
  in.birthday_day = 5;
  in.language = 3;
  write_dump(in);
  ds::NDS nds;
  load(nds);
  ds::bios::UserSettings out;
  CHECK(nds.read_user_settings(out));
  CHECK(out.nickname == "Ada");
  CHECK(out.message == "Hello there");
  CHECK(out.favourite_colour == 11);
  CHECK(out.birthday_month == 7);
  CHECK(out.birthday_day == 5);
  CHECK(out.language == 3);
  check_crcs(nds);
}

// Changing one field must leave the rest exactly as they were: the menu edits
// one at a time, and a dump's nickname can hold characters these strings
// cannot carry, so rewriting all of them would corrupt one the player never
// touched.
void test_one_field_at_a_time() {
  ds::bios::UserSettings in;
  in.nickname = "Setya";
  in.message = "Keep me";
  in.favourite_colour = 0;
  in.birthday_month = 7;
  in.birthday_day = 5;
  in.language = 1;
  write_dump(in);
  ds::NDS nds;
  load(nds);

  ds::bios::UserSettings u;
  CHECK(nds.read_user_settings(u));
  u.favourite_colour = 2;
  CHECK(nds.write_user_settings(ds::NDS::UserField::Colour, u));

  ds::bios::UserSettings after;
  CHECK(nds.read_user_settings(after));
  CHECK(after.favourite_colour == 2);
  CHECK(after.nickname == "Setya");      // untouched, and still its own case
  CHECK(after.message == "Keep me");
  CHECK(after.birthday_month == 7);
  CHECK(after.birthday_day == 5);
  CHECK(after.language == 1);
  check_crcs(nds);

  // A string field, including one that gets shorter: the length halfword has
  // to follow it, or the console reads the tail of the old name after the new.
  u = after;
  u.nickname = "Bo";
  CHECK(nds.write_user_settings(ds::NDS::UserField::Nickname, u));
  CHECK(nds.read_user_settings(after));
  CHECK(after.nickname == "Bo");
  CHECK(after.message == "Keep me");
  CHECK(after.favourite_colour == 2);
  check_crcs(nds);

  // Longer than the firmware keeps: cut, not overflowed into the next field.
  u = after;
  u.nickname = "ABCDEFGHIJKLMNOP";
  CHECK(nds.write_user_settings(ds::NDS::UserField::Nickname, u));
  CHECK(nds.read_user_settings(after));
  CHECK(after.nickname == "ABCDEFGHIJ");   // ten characters
  CHECK(after.message == "Keep me");
  check_crcs(nds);

  // The language shares its halfword with the backlight bits, which are the
  // console's and must survive.
  const u32 base = nds.user_settings_offset();
  const u16 before_bits = static_cast<u16>(rd16(nds.firmware.data() + base + 0x64) & ~7);
  u = after;
  u.language = 5;
  CHECK(nds.write_user_settings(ds::NDS::UserField::Language, u));
  CHECK(nds.read_user_settings(after));
  CHECK(after.language == 5);
  CHECK((rd16(nds.firmware.data() + base + 0x64) & ~7) == before_bits);
  check_crcs(nds);
}

// An edit has to reach the sidecar and come back, because that is the only
// place it is kept: the dump itself is never written.
void test_sidecar_round_trip() {
  ds::bios::UserSettings in;
  in.nickname = "Setya";
  in.favourite_colour = 0;
  write_dump(in);
  std::remove(kOvr);

  std::vector<u8> dump_before;
  {
    std::ifstream f(kFw, std::ios::binary);
    dump_before.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  }

  {
    ds::NDS nds;
    load(nds);
    ds::bios::UserSettings u;
    CHECK(nds.read_user_settings(u));
    u.favourite_colour = 9;
    CHECK(nds.write_user_settings(ds::NDS::UserField::Colour, u));
    CHECK(nds.firmware_override_dirty());
    std::string err;
    CHECK(nds.save_firmware_override(kOvr, err));
  }

  // The dump is untouched: that is the whole point of the sidecar.
  std::vector<u8> dump_after;
  {
    std::ifstream f(kFw, std::ios::binary);
    dump_after.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  }
  CHECK(dump_before == dump_after);

  // A fresh console with the sidecar sees the change.
  {
    ds::NDS nds;
    load(nds);
    ds::bios::UserSettings fresh;
    CHECK(nds.read_user_settings(fresh));
    CHECK(fresh.favourite_colour == 0);      // not until the sidecar is applied
    std::string err;
    CHECK(nds.load_firmware_override(kOvr, err));
    CHECK(nds.read_user_settings(fresh));
    CHECK(fresh.favourite_colour == 9);
    CHECK(fresh.nickname == "Setya");
    check_crcs(nds);
  }
}

} // namespace

// The emulated access point goes into the first free slot of a firmware and
// nowhere else: not over a player's network, not twice.
static void test_access_point_stamp() {
  using namespace ds;
  bios::UserSettings user;
  std::vector<u8> fw = bios::generate_firmware_dsi(user, 1, 0x3E);
  const u32 base = (static_cast<u32>(fw[0x20] | (fw[0x21] << 8)) << 3) - 0x400;
  auto slot = [&](int i) { return fw.data() + base + i * 0x100; };
  auto crc_ok = [&](int i) { const u8* a = slot(i); return bios::crc16(a, 0xFE, 0) == static_cast<u16>(a[0xFE] | (a[0xFF] << 8)); };
  // A generated image already has it in slot 0, with the DSi's MTU.
  CHECK(bios::stamp_access_point(fw) == 0);
  CHECK(std::string(reinterpret_cast<const char*>(slot(0) + 0x40)) == bios::kAccessPointSsid);
  CHECK((slot(0)[0xEA] | (slot(0)[0xEB] << 8)) == 1400);
  // A player's network in slot 0 and an erased slot 1: slot 1 gets it.
  std::memset(slot(0) + 0x40, 0, 32);
  std::strcpy(reinterpret_cast<char*>(slot(0) + 0x40), "HomeNet");
  const u16 c0 = bios::crc16(slot(0), 0xFE, 0);
  slot(0)[0xFE] = static_cast<u8>(c0); slot(0)[0xFF] = static_cast<u8>(c0 >> 8);
  std::memset(slot(1), 0xFF, 0x100);
  CHECK(bios::stamp_access_point(fw) == 1);
  CHECK(std::string(reinterpret_cast<const char*>(slot(0) + 0x40)) == "HomeNet");
  CHECK(std::string(reinterpret_cast<const char*>(slot(1) + 0x40)) == bios::kAccessPointSsid);
  CHECK(slot(1)[0xE7] == 0x00 && slot(1)[0xEF] == 0x01 && crc_ok(1));
  // Found again, not stamped a second time.
  const std::vector<u8> before = fw;
  CHECK(bios::stamp_access_point(fw) == 1);
  CHECK(fw == before);
  // Every slot holding another network: left alone.
  for (int i = 0; i < 3; ++i) std::memcpy(slot(i), slot(0), 0x100);
  const std::vector<u8> full = fw;
  CHECK(bios::stamp_access_point(fw) == -1);
  CHECK(fw == full);
}

int main() {
  test_access_point_stamp();
  test_round_trip();
  test_one_field_at_a_time();
  test_sidecar_round_trip();
  std::remove(kFw);
  std::remove(kOvr);
  std::printf("firmware: ok\n");
  return 0;
}
