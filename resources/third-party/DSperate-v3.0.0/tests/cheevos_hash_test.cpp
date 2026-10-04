// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// The RetroAchievements hash. The golden values below were computed
// independently of this code (a few lines of Python over the same synthetic
// ROM, following the rule in rcheevos' hash_rom.c: MD5 of the 0x160-byte
// header, then the ARM9 binary, then the ARM7 binary, then 0xA00 bytes of
// icon block, zero-padded if the file is short). So they check our wiring
// against the specification rather than against itself.
//
// The test that earns its keep is hash_ignores_the_secure_area_rewrite. Cart
// re-encrypts the secure area in place through RomSource::patch(), and the
// secure area is the first 0x800 bytes of the ARM9 binary -- exactly what the
// hash covers. Get that wrong and nothing fails: the hash is simply one
// RetroAchievements has never seen, and the game quietly has no achievements.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>

#include "check.h"
#include "cheevos/cheevos_hash.h"
#include "core/cart/rom_source.h"

using ds::u8;
using ds::u32;

namespace {

constexpr u32 ARM9_OFF = 0x4000, ARM9_LEN = 0x1000;
constexpr u32 ARM7_OFF = 0x8000, ARM7_LEN = 0x0800;
constexpr u32 ICON_OFF = 0xA000, ICON_LEN = 0x0A00;

// A ROM whose every byte is determined by its offset, so the golden hashes are
// reproducible by anything that can count. `total` sets where the file ends,
// which is how the short-icon case is built.
std::vector<u8> synthetic_rom(u32 total) {
  std::vector<u8> b(total);
  for (u32 i = 0; i < total; ++i) b[i] = static_cast<u8>((i * 31 + 7) & 0xFF);
  auto w32 = [&b](u32 off, u32 v) {
    b[off + 0] = static_cast<u8>(v);
    b[off + 1] = static_cast<u8>(v >> 8);
    b[off + 2] = static_cast<u8>(v >> 16);
    b[off + 3] = static_cast<u8>(v >> 24);
  };
  w32(0x20, ARM9_OFF);
  w32(0x2C, ARM9_LEN);
  w32(0x30, ARM7_OFF);
  w32(0x3C, ARM7_LEN);
  w32(0x68, ICON_OFF);
  return b;
}

std::string hash_of(const ds::cart::RomSource& src) {
  std::string out, err;
  CHECK(ds::cheevos::rom_hash(src, "synthetic", out, err));
  CHECK(err.empty());
  CHECK(out.size() == 32);
  return out;
}

const char* FULL_HASH  = "9d072ade9c1badd3a4325488f687f253";
const char* SHORT_HASH = "5f1e526181ad5db9f5d20f625f5f9622";

void hash_matches_the_specification() {
  const auto src = ds::cart::RomSource::from_memory(synthetic_rom(ICON_OFF + ICON_LEN));
  CHECK(src);
  CHECK(hash_of(*src) == FULL_HASH);
}

// A homebrew ROM that stops inside its icon block. rcheevos zero-pads the
// remainder, which it can only do if our reader reports the short count --
// RomSource would otherwise happily serve 0xFF past the end of the image, and
// 0xFF padding hashes differently from 0x00 padding.
void short_icon_block_is_zero_padded() {
  const auto src = ds::cart::RomSource::from_memory(synthetic_rom(ICON_OFF + 0x400));
  CHECK(src);
  CHECK(hash_of(*src) == SHORT_HASH);
  CHECK(std::strcmp(SHORT_HASH, FULL_HASH) != 0);
}

// The reason read_unpatched exists.
void hash_ignores_the_secure_area_rewrite() {
  auto src = ds::cart::RomSource::from_memory(synthetic_rom(ICON_OFF + ICON_LEN));
  CHECK(src);
  CHECK(hash_of(*src) == FULL_HASH);

  // What Cart does at construction: take a writable copy of the secure-area
  // page and rewrite it. 0xAA stands in for the re-encryption.
  u8* page = src->patch(ARM9_OFF);
  CHECK(page != nullptr);
  std::memset(page, 0xAA, 0x800);

  // The rewrite is visible through the cart's own read path...
  u8 probe[8] = {};
  src->read(ARM9_OFF, probe, sizeof probe);
  CHECK(probe[0] == 0xAA);
  // ...and invisible to the hash.
  src->read_unpatched(ARM9_OFF, probe, sizeof probe);
  CHECK(probe[0] != 0xAA);
  CHECK(hash_of(*src) == FULL_HASH);
}

// Mapped and owned sources are supposed to be indistinguishable; the hash is
// one more place that has to hold, and it is the path a real ROM file takes.
void a_mapped_file_hashes_the_same() {
  const auto bytes = synthetic_rom(ICON_OFF + ICON_LEN);
  char path[] = "/tmp/dsperate_cheevos_hash_testXXXXXX";
  const int fd = mkstemp(path);
  CHECK(fd >= 0);
  CHECK(write(fd, bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size()));
  close(fd);

  std::string err;
  const auto src = ds::cart::RomSource::map_file(path, err);
  unlink(path);
  CHECK(src);
  CHECK(src->mapped());
  CHECK(hash_of(*src) == FULL_HASH);
}

// Not a DS ROM at all: a failure has to come back as one, with a reason, and
// without a hash anyone could mistake for real.
void a_file_too_small_is_refused() {
  const auto src = ds::cart::RomSource::from_memory(std::vector<u8>(0x40, 0u));
  CHECK(src);
  std::string out = "untouched", err;
  CHECK(!ds::cheevos::rom_hash(*src, "tiny", out, err));
  CHECK(out.empty());
  CHECK(!err.empty());
}

} // namespace

int main() {
  hash_matches_the_specification();
  short_icon_block_is_zero_padded();
  hash_ignores_the_secure_area_rewrite();
  a_mapped_file_hashes_the_same();
  a_file_too_small_is_refused();
  std::printf("cheevos_hash: ok\n");
  return 0;
}
