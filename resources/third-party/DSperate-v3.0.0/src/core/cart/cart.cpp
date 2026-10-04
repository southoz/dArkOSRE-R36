// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Slot-1 retail cartridge. Protocol per GBATEK ("DS Cartridge Protocol",
// "DS Cartridge Secure Area", "DS Cartridge Backup").
#include "core/cart/cart.h"
#include "core/state/state.h"
#include "core/nds.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ds::cart {

namespace {
inline u32 bswap(u32 v) { return __builtin_bswap32(v); }
}

namespace {
// save_list.inc, sorted by game code. Returns the listed type, or -1 if the
// code isn't in the table (distinct from a listed 64 KB EEPROM default).
int lookup_save_type(u32 code) {
  struct E { u32 code; u32 type; };
  static const E list[] = {
#include "core/cart/save_list.inc"
  };
  size_t lo = 0, hi = sizeof list / sizeof list[0];
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    if (list[mid].code < code) lo = mid + 1;
    else if (list[mid].code > code) hi = mid;
    else return static_cast<int>(list[mid].type);
  }
  return -1;
}
} // namespace

bool known_game_code(u32 code) { return lookup_save_type(code) >= 0; }

SaveType save_type_for(u32 code, u32& size) {
  // Numbering: 1 = 512 B EEPROM, 2 = 8 KB, 3 = 64 KB, 4 = 128 KB EEPROM,
  // 5 = 256 KB, 6 = 512 KB, 7 = 1 MB, 8..10 = 8/16/64 MB FLASH.
  const int found = lookup_save_type(code);
  u32 t = 3;   // unknown title: 64 KB EEPROM, the most common chip
  if (found >= 1 && found <= 10) t = static_cast<u32>(found);
  static const u32 sizes[] = {0, 512, 8192, 65536, 131072, 262144, 524288, 1048576,
                              8388608, 16777216, 67108864};
  size = sizes[t];
  if (t == 1) return SaveType::EepromTiny;
  if (t <= 4) return SaveType::Eeprom;
  return SaveType::Flash;
}

SaveType save_type_for_size(u32 bytes) {
  if (bytes == 512) return SaveType::EepromTiny;
  if (bytes == 8192 || bytes == 65536 || bytes == 131072) return SaveType::Eeprom;
  if (bytes >= 262144 && bytes <= 67108864 && (bytes & (bytes - 1)) == 0) return SaveType::Flash;
  return SaveType::None;
}

Cart::Cart(NDS& nds, std::unique_ptr<RomSource> rom) : nds_(nds), rom_(std::move(rom)) {
  const u32 size = rom_->mask() + 1;
  rom_mask_ = rom_->mask();
  rom_->read(0, reinterpret_cast<u8*>(&header_), sizeof header_);
  rom_->read(TwlHeader::ROM_OFFSET, reinterpret_cast<u8*>(&twl_), sizeof twl_);
  chip_id_ = 0x000000C2;
  if (size >= 1024 * 1024 && size <= 128 * 1024 * 1024) chip_id_ |= ((size >> 20) - 1) << 8;
  else chip_id_ |= (0x100 - (size >> 28)) << 8;
  // DSi-capable (unit code bit 1) with a nonzero DSi region: bit 30.
  if ((header_.unit_code & 2) && twl_.region_flags != 0) chip_id_ |= 0x40000000;
  u32 sram_size = 0;
  save_type_ = save_type_for(header_.game_code_u32(), sram_size);
  ir_cart_ = (header_.game_code_u32() & 0xFF) == 'I';
  listed_ = known_game_code(header_.game_code_u32());
  if (listed_) sram_.assign(sram_size, 0xFF);
  else save_type_ = SaveType::Detect;   // load_save or the first save access names the chip

  // Dumps often carry a decrypted secure area; re-encrypt if the "decrypted"
  // marker is present, rewriting the source's overlay rather than the file.
  const u32 a9 = header_.arm9_rom_offset;
  if (a9 >= 0x4000 && a9 < 0x8000) {
    const u32 w0 = rom_->read32(a9), w4 = rom_->read32(a9 + 0x10);
    if (w0 == 0xE7FFDEFF && w4 != 0xE7FFDEFF) {
      u8* sec = rom_->patch(a9) + (a9 & (RomSource::PAGE - 1));
      std::memcpy(sec, "encryObj", 8);
      key1_init(header_.game_code_u32(), 3, 2);
      for (u32 i = 0; i < 0x800; i += 8) key1_encrypt(reinterpret_cast<u32*>(sec + i));
      key1_init(header_.game_code_u32(), 2, 2);
      key1_encrypt(reinterpret_cast<u32*>(sec));
    }
  }
  reset();
}

void Cart::reset() {
  in_reset_ = true; cmd_mode_ = data_mode_ = 0; rom_addr_ = 0; std::memset(rom_cmd_, 0, 8);
  launch_read_ = false;
  spi_pos_ = 0; spi_cmd_ = 0; spi_addr_ = 0; spi_status_ = 0;
}

void Cart::set_reset(bool reset) {
  if (reset == in_reset_) return;
  in_reset_ = reset;
  cmd_mode_ = data_mode_ = 0; rom_addr_ = 0; std::memset(rom_cmd_, 0, 8);
}

void Cart::setup_direct_boot() { cmd_mode_ = 2; data_mode_ = 2; in_reset_ = false; }

// ---- KEY1 -----------------------------------------------------------------
void Cart::key1_encrypt(u32* data) const {
  u32 y = data[0], x = data[1], z;
  for (u32 i = 0; i <= 0xF; ++i) {
    z = key1_[i] ^ x;
    x  = key1_[0x012 + (z >> 24)];
    x += key1_[0x112 + ((z >> 16) & 0xFF)];
    x ^= key1_[0x212 + ((z >> 8) & 0xFF)];
    x += key1_[0x312 + (z & 0xFF)];
    x ^= y; y = z;
  }
  data[0] = x ^ key1_[0x10];
  data[1] = y ^ key1_[0x11];
}
void Cart::key1_decrypt(u32* data) const {
  u32 y = data[0], x = data[1], z;
  for (u32 i = 0x11; i >= 0x2; --i) {
    z = key1_[i] ^ x;
    x  = key1_[0x012 + (z >> 24)];
    x += key1_[0x112 + ((z >> 16) & 0xFF)];
    x ^= key1_[0x212 + ((z >> 8) & 0xFF)];
    x += key1_[0x312 + (z & 0xFF)];
    x ^= y; y = z;
  }
  data[0] = x ^ key1_[0x1];
  data[1] = y ^ key1_[0x0];
}
void Cart::key1_apply_keycode(u32* keycode, u32 mod) {
  key1_encrypt(&keycode[1]);
  key1_encrypt(&keycode[0]);
  u32 temp[2] = {0, 0};
  for (u32 i = 0; i <= 0x11; ++i) key1_[i] ^= bswap(keycode[i % mod]);
  for (u32 i = 0; i <= 0x410; i += 2) {
    key1_encrypt(temp);
    key1_[i] = temp[1]; key1_[i + 1] = temp[0];
  }
}
void Cart::key1_init(u32 idcode, u32 level, u32 mod) {
  // Key table lives at ARM7 BIOS 0x30 (0x1048 bytes).
  std::memcpy(key1_.data(), nds_.bus.bios7.get() + 0x30, key1_.size() * 4);
  u32 keycode[3] = {idcode, idcode >> 1, idcode << 1};
  if (level >= 1) key1_apply_keycode(keycode, mod);
  if (level >= 2) key1_apply_keycode(keycode, mod);
  keycode[1] <<= 1; keycode[2] >>= 1;
  if (level >= 3) key1_apply_keycode(keycode, mod);
}

void Cart::decrypt_secure_area(u8 out[0x800]) {
  const u32 a9 = header_.arm9_rom_offset;
  rom_->read(a9, out, 0x800);
  key1_init(header_.game_code_u32(), 2, 2);
  key1_decrypt(reinterpret_cast<u32*>(&out[0]));
  key1_init(header_.game_code_u32(), 3, 2);
  for (u32 i = 0; i < 0x800; i += 8) key1_decrypt(reinterpret_cast<u32*>(&out[i]));
  if (!std::strncmp(reinterpret_cast<const char*>(out), "encryObj", 8)) {
    u32 marker = 0xE7FFDEFF; std::memcpy(&out[0], &marker, 4); std::memcpy(&out[4], &marker, 4);
  } else {
    std::fprintf(stderr, "[cart] secure area decryption failed\n");
    for (u32 i = 0; i < 0x800; i += 4) { u32 marker = 0xE7FFDEFF; std::memcpy(&out[i], &marker, 4); }
  }
}

// ---- ROM commands -----------------------------------------------------------
u32 Cart::rom_read32() {   // reads wrap within a 4 KB page
  const u32 hi = rom_addr_ & rom_mask_ & ~0xFFFu;
  u32 lo = rom_addr_ & 0xFFF;
  if (hi != page_base_) { page_base_ = hi; page_ = rom_->page(hi); }
  u32 v = 0;
  for (int i = 0; i < 4; ++i) { v |= static_cast<u32>(page_[lo]) << (8 * i); lo = (lo + 1) & 0xFFF; }
  rom_addr_ = hi | lo;
  return v;
}

// DS_CART_LOG=1: one line per Slot-1 command, with the mode it was issued in.
static const bool g_cart_log = std::getenv("DS_CART_LOG") != nullptr;

void Cart::command_start(const u8 cmd[8]) {
  if (g_cart_log)
    std::fprintf(stderr, "[cart] mode %u reset %u cmd %02x %02x %02x %02x %02x %02x %02x %02x\n",
                 cmd_mode_, in_reset_ ? 1u : 0u,
                 cmd[0], cmd[1], cmd[2], cmd[3], cmd[4], cmd[5], cmd[6], cmd[7]);
  if (in_reset_) return;
  if (cmd_mode_ == 0) {
    std::memcpy(rom_cmd_, cmd, 8);
    switch (rom_cmd_[0]) {
    case 0x00: rom_addr_ = ((rom_cmd_[1] << 24) | (rom_cmd_[2] << 16) | (rom_cmd_[3] << 8) | rom_cmd_[4]) & 0xFFF; return;
    case 0x3C: cmd_mode_ = 1; key1_init(header_.game_code_u32(), 2, 2); return;
    default: return;
    }
  }
  if (cmd_mode_ == 1) {
    // KEY1 commands arrive encrypted; decrypt to dispatch.
    u8 dec[8]; u32 w;
    std::memcpy(&w, &cmd[4], 4); w = bswap(w); std::memcpy(&dec[0], &w, 4);
    std::memcpy(&w, &cmd[0], 4); w = bswap(w); std::memcpy(&dec[4], &w, 4);
    key1_decrypt(reinterpret_cast<u32*>(dec));
    u32 t0, t1; std::memcpy(&t0, &dec[0], 4); std::memcpy(&t1, &dec[4], 4);
    t0 = bswap(t0); t1 = bswap(t1);
    std::memcpy(&dec[0], &t1, 4); std::memcpy(&dec[4], &t0, 4);
    std::memcpy(rom_cmd_, dec, 8);
    if (g_cart_log)
      std::fprintf(stderr, "[cart]   key1 decrypted %02x %02x %02x %02x %02x %02x %02x %02x\n",
                   dec[0], dec[1], dec[2], dec[3], dec[4], dec[5], dec[6], dec[7]);
    switch (rom_cmd_[0] & 0xF0) {
    case 0x40: data_mode_ = 2; return;                          // enable KEY2 data
    case 0x20: rom_addr_ = (rom_cmd_[2] & 0xF0) << 8; return;   // secure area block
    case 0xA0: cmd_mode_ = 2; return;                           // leave KEY1 mode
    default: return;
    }
  }
  std::memcpy(rom_cmd_, cmd, 8);
  if (rom_cmd_[0] == 0xB7) {
    rom_addr_ = ((rom_cmd_[1] << 24) | (rom_cmd_[2] << 16) | (rom_cmd_[3] << 8) | rom_cmd_[4]) & rom_mask_;
    // Must read before the clamp below folds sub-0x8000 addresses onto 0x8000,
    // which would otherwise forge the launch signal from an unrelated read.
    if (rom_addr_ == header_.arm9_rom_offset) launch_read_ = true;
    if (rom_addr_ < 0x8000) rom_addr_ = 0x8000 + (rom_addr_ & 0x1FF);   // secure area is not readable here
  }
}

u32 Cart::command_receive() {
  if (in_reset_) return 0;
  if (cmd_mode_ == 0) {
    switch (rom_cmd_[0]) { case 0x9F: return 0xFFFFFFFF; case 0x00: return rom_read32(); case 0x90: return chip_id_; }
  } else if (cmd_mode_ == 1) {
    switch (rom_cmd_[0] & 0xF0) { case 0x10: return chip_id_; case 0x20: return rom_read32(); }
  } else {
    switch (rom_cmd_[0]) { case 0xB7: return rom_read32(); case 0xB8: return chip_id_; }
  }
  return 0;
}

// ---- save chip --------------------------------------------------------------
// DS_AUXSPI_LOG=1: one line per save-chip transaction (command, address, bytes).
static const bool g_auxspi_log = std::getenv("DS_AUXSPI_LOG") != nullptr;
void Cart::spi_release() {
  if (g_auxspi_log && spi_pos_) std::fprintf(stderr, "[auxspi] cmd %02x addr %06x len %u status %02x%s\n", spi_cmd_, spi_addr_, spi_pos_, spi_status_, ir_cart_ ? " (ir)" : "");
  if (save_type_ == SaveType::Detect && spi_pos_ > 0) detect_release();
  spi_pos_ = 0; ir_pos_ = 0;
}

void Cart::set_chip(SaveType type, u32 bytes) {
  save_type_ = type;
  sram_.assign(bytes, 0xFF);
}

Cart::SaveLoad Cart::load_save(const u8* data, size_t n) {
  // .dsv is the raw image with a 122-byte footer ending in this cookie.
  static constexpr char DSV_COOKIE[] = "|-DESMUME SAVE-|";
  if (n >= 122 && std::memcmp(data + n - 16, DSV_COOKIE, 16) == 0) n -= 122;
  if (!listed_) {
    const SaveType t = n <= 67108864 ? save_type_for_size(static_cast<u32>(n)) : SaveType::None;
    if (t == SaveType::None) return {false, 0};   // no chip is that size: stay detecting
    set_chip(t, static_cast<u32>(n));
  }
  if (save_type_ == SaveType::None) return {true, 0};   // no save chip: nothing will overwrite the file
  std::memcpy(sram_.data(), data, n < sram_.size() ? n : sram_.size());
  return {n == sram_.size(), static_cast<u32>(sram_.size())};
}

// SaveType::Detect: no save file, chip is blank. The SDK's first access moves
// a single byte, so transaction length (command + address + 1) plus which
// commands only one chip class has determines the type. Nothing is decided
// until the transaction ends; a deciding write is then replayed into the chip.
u8 Cart::spi_detect(u8 v) {
  if (detect_buf_.size() < 0x1000) detect_buf_.push_back(v);
  return spi_cmd_ == 0x05 ? spi_status_ : 0xFF;
}

void Cart::detect_release() {
  if (detect_buf_.empty()) return;
  const u8 cmd = detect_buf_[0];
  const u32 len = spi_pos_;    // bytes clocked, the command included
  SaveType t = SaveType::Detect;
  u32 bytes = 0;
  switch (cmd) {
  case 0x02: case 0x03:
    if (len == 3) { t = SaveType::EepromTiny; bytes = 512; }
    else if (len == 4) { t = SaveType::Eeprom; bytes = 65536; }
    else if (len == 5) { t = SaveType::Flash; bytes = 524288; }
    else if (cmd == 0x02 && len > 5) { t = SaveType::Eeprom; bytes = 65536; }   // commonest chip
    break;
  case 0x0A: case 0x0B:
    // 512 B EEPROM's upper-half commands, or FLASH page write/fast read.
    if (len == 5 || (cmd == 0x0B && len == 6)) { t = SaveType::Flash; bytes = 524288; }
    else if (len == 3 || cmd == 0x0A) { t = SaveType::EepromTiny; bytes = 512; }
    break;
  case 0xD8: case 0xDB:   // erases: FLASH only
    t = SaveType::Flash; bytes = 524288;
    break;
  default: break;         // status, write enable, ID: say nothing about the chip
  }
  if (t == SaveType::Detect) { detect_buf_.clear(); return; }   // write enable/disable never refill it
  std::fprintf(stderr, "save: game code not in the save list; detected a %u %s %s from its first access (cmd %02x, %u bytes)\n",
               bytes >= 1024 ? bytes >> 10 : bytes, bytes >= 1024 ? "KB" : "B", t == SaveType::Flash ? "FLASH" : "EEPROM", cmd, len);
  const std::vector<u8> tx = std::move(detect_buf_);
  detect_buf_.clear();
  set_chip(t, bytes);
  if (cmd == 0x02 || cmd == 0x0A || cmd == 0xD8 || cmd == 0xDB) {
    spi_cmd_ = cmd; spi_addr_ = 0;
    for (u32 i = 1; i < tx.size(); ++i) { spi_pos_ = i; spi_chip(tx[i]); }
  }
}

u8 Cart::spi_chip(u8 v) {
  switch (save_type_) {
  case SaveType::EepromTiny: return spi_eeprom_tiny(v);
  case SaveType::Eeprom: return spi_eeprom(v);
  case SaveType::Flash: return spi_flash(v);
  case SaveType::Detect: return spi_detect(v);
  default: return 0xFF;
  }
}

u8 Cart::spi_transfer(u8 v) {
  if (ir_cart_) {
    if (ir_pos_++ == 0) { ir_cmd_ = v; if (g_auxspi_log && v != 0) std::fprintf(stderr, "[auxspi] ir cmd %02x\n", v); return 0; }
    if (ir_cmd_ == 0x08) return 0xAA;
    if (ir_cmd_ != 0x00) return 0;
  }
  if (save_type_ == SaveType::None) return 0;
  u8 ret = 0xFF;
  if (spi_pos_ == 0) {
    switch (v) {
    case 0x04: spi_status_ &= ~2; spi_pos_++; return 0;   // write disable
    case 0x06: spi_status_ |= 2;  spi_pos_++; return 0;   // write enable
    default: spi_cmd_ = v; spi_addr_ = 0; break;
    }
    if (save_type_ == SaveType::Detect) detect_buf_.assign(1, v);
  } else {
    ret = spi_chip(v);
  }
  spi_pos_++;
  return ret;
}

u8 Cart::spi_eeprom_tiny(u8 v) {
  switch (spi_cmd_) {
  case 0x01: if (spi_pos_ == 1) spi_status_ = (spi_status_ & 1) | (v & 0x0C); return 0;
  case 0x05: return spi_status_ | 0xF0;
  case 0x02: case 0x0A:
    if (spi_pos_ < 2) spi_addr_ = v;
    else { if (spi_status_ & 2) { sram_[(spi_addr_ + (spi_cmd_ == 0x0A ? 0x100 : 0)) & 0x1FF] = v; mark_dirty(); } spi_addr_++; }
    return 0;
  case 0x03: case 0x0B:
    if (spi_pos_ < 2) { spi_addr_ = v; return 0; }
    return sram_[(spi_addr_++ + (spi_cmd_ == 0x0B ? 0x100 : 0)) & 0x1FF];
  case 0x9F: return 0xFF;
  default: return 0xFF;
  }
}

u8 Cart::spi_eeprom(u8 v) {
  const u32 addrsize = sram_.size() > 65536 ? 3 : 2;
  const u32 mask = static_cast<u32>(sram_.size() - 1);
  switch (spi_cmd_) {
  case 0x01: if (spi_pos_ == 1) spi_status_ = (spi_status_ & 1) | (v & 0x0C); return 0;
  case 0x05: return spi_status_;
  case 0x02:
    if (spi_pos_ <= addrsize) spi_addr_ = (spi_addr_ << 8) | v;
    else { if (spi_status_ & 2) { sram_[spi_addr_ & mask] = v; mark_dirty(); } spi_addr_++; }
    return 0;
  case 0x03:
    if (spi_pos_ <= addrsize) { spi_addr_ = (spi_addr_ << 8) | v; return 0; }
    return sram_[(spi_addr_++) & mask];
  case 0x9F: return 0xFF;
  default: return 0xFF;
  }
}

// Unlisted chip: an address past the end grows it to the next power of two
// (up to 8 MB) rather than wrapping onto data at the bottom.
void Cart::fit_flash(u32 addr) {
  if (addr < sram_.size() || addr >= 0x800000) return;
  u32 bytes = static_cast<u32>(sram_.size());
  while (bytes <= addr) bytes <<= 1;
  sram_.resize(bytes, 0xFF);
}

u8 Cart::spi_flash(u8 v) {
  const bool addressing = spi_pos_ <= 3 && (spi_cmd_ == 0x02 || spi_cmd_ == 0x03 || spi_cmd_ == 0x0A ||
                                            spi_cmd_ == 0x0B || spi_cmd_ == 0xD8 || spi_cmd_ == 0xDB);
  if (addressing) {
    spi_addr_ = (spi_addr_ << 8) | v;
    if (spi_pos_ == 3 && !listed_) fit_flash(spi_addr_);
  }
  const u32 mask = static_cast<u32>(sram_.size() - 1);
  switch (spi_cmd_) {
  case 0x05: return spi_status_;
  case 0x02:   // page program: can only clear bits (an erased page reads 0xFF) -- unless unlisted, see save_type_listed
    if (!addressing) { if (spi_status_ & 2) { u8& b = sram_[spi_addr_ & mask]; b = listed_ ? (b & v) : v; mark_dirty(); } spi_addr_++; }
    return 0;
  case 0x0A:   // page write
    if (!addressing) { if (spi_status_ & 2) { sram_[spi_addr_ & mask] = v; mark_dirty(); } spi_addr_++; }
    return 0;
  case 0x03:
    if (addressing) return 0;
    return sram_[(spi_addr_++) & mask];
  case 0x0B:   // fast read (dummy byte)
    if (addressing || spi_pos_ == 4) return 0;
    return sram_[(spi_addr_++) & mask];
  case 0x9F: return 0xFF;
  case 0xD8:   // sector erase
    if (spi_pos_ == 3 && (spi_status_ & 2)) { for (u32 i = 0; i < 0x10000; ++i) sram_[(spi_addr_++) & mask] = 0xFF; mark_dirty(); }
    return 0;
  case 0xDB:   // page erase
    if (spi_pos_ == 3 && (spi_status_ & 2)) { for (u32 i = 0; i < 0x100; ++i) sram_[(spi_addr_++) & mask] = 0xFF; mark_dirty(); }
    return 0;
  default: return 0xFF;
  }
}


template <class S> void Cart::sync_state(S& s) {
  s.begin("CART");
  s.fields(in_reset_, cmd_mode_, data_mode_, rom_cmd_, rom_addr_, spi_pos_, spi_cmd_, spi_addr_, spi_status_, ir_cmd_, ir_pos_);
  u32 n = static_cast<u32>(sram_.size());
  s.put(n);
  if constexpr (S::reading) {
    if (n != sram_.size()) {
      const SaveType t = n ? save_type_for_size(n) : SaveType::Detect;
      if (listed_ || t == SaveType::None) { s.fail("save chip size differs"); return; }
      set_chip(t, n);
      detect_buf_.clear();
    }
  }
  s.blob(sram_.data(), sram_.size());
  s.end();
  if constexpr (S::reading) {
    mark_dirty();
    if (cmd_mode_ == 1) key1_init(header_.game_code_u32(), 2, 2);
  }
}
template void Cart::sync_state<state::Writer>(state::Writer&);
template void Cart::sync_state<state::Reader>(state::Reader&);

} // namespace ds::cart
