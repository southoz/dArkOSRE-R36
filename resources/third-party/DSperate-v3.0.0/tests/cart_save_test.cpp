// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// The save chip of a game the save list does not know: a ROM hack that changed
// its game code, or homebrew. The chip follows an existing save file's size,
// or is detected from the game's first access; a guess must never destroy what
// the game stores. A listed game keeps its fixed chip and exact FLASH rules.
#include "core/nds.h"
#include "core/cart/cart.h"
#include "core/state/state.h"
#include "check.h"

#include <cstring>
#include <initializer_list>
#include <vector>

using namespace ds;
using cart::Cart;
using cart::SaveType;

namespace {

Cart make_cart(NDS& nds, const char code[4]) {
  std::vector<u8> rom(1 << 20, 0);
  std::memcpy(rom.data() + 0x0C, code, 4);    // arm9_rom_offset stays 0: no secure area to re-encrypt
  return Cart(nds, std::move(rom));
}

// One chip-select period: every byte clocked, then the release.
std::vector<u8> tx(Cart& c, std::initializer_list<u8> bytes) {
  std::vector<u8> out;
  for (u8 b : bytes) out.push_back(c.spi_transfer(b));
  c.spi_release();
  return out;
}

void test_size_classes() {
  CHECK(cart::save_type_for_size(512) == SaveType::EepromTiny);
  CHECK(cart::save_type_for_size(8192) == SaveType::Eeprom);
  CHECK(cart::save_type_for_size(65536) == SaveType::Eeprom);
  CHECK(cart::save_type_for_size(131072) == SaveType::Eeprom);
  CHECK(cart::save_type_for_size(262144) == SaveType::Flash);
  CHECK(cart::save_type_for_size(524288) == SaveType::Flash);
  CHECK(cart::save_type_for_size(8388608) == SaveType::Flash);
  CHECK(cart::save_type_for_size(0) == SaveType::None);
  CHECK(cart::save_type_for_size(1000) == SaveType::None);
  CHECK(cart::save_type_for_size(300000) == SaveType::None);
}

// A one-byte first read is command + address + 1 (DeSmuME's rule).
void test_detect_from_first_read() {
  NDS nds;
  CHECK(!cart::known_game_code(0x5A5A5A5A));
  {
    Cart c = make_cart(nds, "ZZZZ");
    CHECK(!c.save_type_listed() && c.save_type() == SaveType::Detect && c.sram().empty());
    tx(c, {0x05, 0x00});                            // a status poll says nothing
    CHECK(c.save_type() == SaveType::Detect);
    CHECK(tx(c, {0x03, 0x00, 0x00, 0x00})[3] == 0xFF);   // blank chip
    CHECK(c.save_type() == SaveType::Eeprom && c.sram().size() == 65536);
    tx(c, {0x06});
    tx(c, {0x02, 0x01, 0x23, 0xAB});
    CHECK(c.sram()[0x123] == 0xAB);
  }
  {
    Cart c = make_cart(nds, "ZZZZ");
    tx(c, {0x03, 0x00, 0x00});
    CHECK(c.save_type() == SaveType::EepromTiny && c.sram().size() == 512);
  }
  {
    Cart c = make_cart(nds, "ZZZZ");
    tx(c, {0x03, 0x00, 0x00, 0x00, 0x00});
    CHECK(c.save_type() == SaveType::Flash && c.sram().size() == 524288);
  }
  {
    // A long first read cannot be split, and the chip is blank anyway: wait.
    Cart c = make_cart(nds, "ZZZZ");
    CHECK(tx(c, {0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00})[7] == 0xFF);
    CHECK(c.save_type() == SaveType::Detect);
    tx(c, {0x0B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});   // undecided either
    tx(c, {0x06});                                  // must not re-read the transaction before it
    CHECK(c.save_type() == SaveType::Detect);
    tx(c, {0xD8, 0x00, 0x00, 0x00});                // an erase: FLASH
    CHECK(c.save_type() == SaveType::Flash);
  }
}

// A write that decides the chip is replayed into it.
void test_detecting_write_is_kept() {
  NDS nds;
  {
    Cart c = make_cart(nds, "ZZZZ");
    tx(c, {0x06});
    tx(c, {0x02, 0x40, 0x5A});
    CHECK(c.save_type() == SaveType::EepromTiny && c.sram()[0x40] == 0x5A && c.sram_dirty());
  }
  {
    Cart c = make_cart(nds, "ZZZZ");
    tx(c, {0x06});
    tx(c, {0x02, 0x01, 0x02, 0x03, 0x77});
    CHECK(c.save_type() == SaveType::Flash && c.sram()[0x10203] == 0x77);
  }
  {
    // Too long to split: the commonest chip, and the bytes land under it.
    Cart c = make_cart(nds, "ZZZZ");
    tx(c, {0x06});
    tx(c, {0x02, 0x00, 0x10, 1, 2, 3, 4});
    CHECK(c.save_type() == SaveType::Eeprom);
    CHECK(c.sram()[0x10] == 1 && c.sram()[0x13] == 4);
  }
}

// An unlisted FLASH overwrites (an EEPROM game on it never erases) and grows
// to fit; a listed one ANDs as the chip does.
void test_lenient_flash() {
  NDS nds;
  Cart c = make_cart(nds, "ZZZZ");
  tx(c, {0x03, 0x00, 0x00, 0x00, 0x00});
  tx(c, {0x06});
  tx(c, {0x02, 0x00, 0x00, 0x10, 0x0F});
  tx(c, {0x02, 0x00, 0x00, 0x10, 0xF0});
  CHECK(c.sram()[0x10] == 0xF0);
  tx(c, {0x02, 0x0F, 0x00, 0x00, 0x42});
  CHECK(c.sram().size() == 0x100000 && c.sram()[0x0F0000] == 0x42 && c.sram()[0x10] == 0xF0);

  const char listed[4] = {'A', 'D', 'A', 'D'};
  u32 code; std::memcpy(&code, listed, 4);
  u32 size = 0;
  CHECK(cart::known_game_code(code) && cart::save_type_for(code, size) == SaveType::Flash);
  Cart l = make_cart(nds, listed);
  CHECK(l.save_type_listed());
  tx(l, {0x06});
  tx(l, {0x02, 0x00, 0x00, 0x10, 0x0F});
  tx(l, {0x02, 0x00, 0x00, 0x10, 0xF0});
  CHECK(l.sram()[0x10] == 0x00);
}

void test_load_save() {
  NDS nds;
  {
    Cart c = make_cart(nds, "ZZZZ");
    std::vector<u8> file(524288, 0x11);
    const Cart::SaveLoad r = c.load_save(file.data(), file.size());
    CHECK(r.fitted && r.chip_bytes == 524288 && c.save_type() == SaveType::Flash && c.sram()[5] == 0x11);
  }
  {
    // DeSmuME .dsv: the image plus a 122-byte footer ending in the cookie.
    Cart c = make_cart(nds, "ZZZZ");
    std::vector<u8> file(65536 + 122, 0x22);
    std::memcpy(file.data() + file.size() - 16, "|-DESMUME SAVE-|", 16);
    const Cart::SaveLoad r = c.load_save(file.data(), file.size());
    CHECK(r.fitted && c.save_type() == SaveType::Eeprom && c.sram().size() == 65536);
  }
  {
    Cart c = make_cart(nds, "ZZZZ");
    std::vector<u8> file(1000, 0x33);
    const Cart::SaveLoad r = c.load_save(file.data(), file.size());
    CHECK(!r.fitted && r.chip_bytes == 0 && c.save_type() == SaveType::Detect);
  }
  {
    // A listed chip does not move: the part that fits, and the mismatch said.
    const char listed[4] = {'A', 'D', 'A', 'D'};
    Cart c = make_cart(nds, listed);
    const u32 chip = static_cast<u32>(c.sram().size());
    std::vector<u8> file(65536, 0x44);
    const Cart::SaveLoad r = c.load_save(file.data(), file.size());
    CHECK(!r.fitted && r.chip_bytes == chip && c.sram()[0] == 0x44 && c.sram()[65536] == 0xFF);
  }
}

// A state names an unlisted chip by its size, either way round.
void test_state_carries_the_chip() {
  NDS nds;
  Cart a = make_cart(nds, "ZZZZ");
  tx(a, {0x03, 0x00, 0x00, 0x00, 0x00});
  tx(a, {0x06});
  tx(a, {0x02, 0x00, 0x00, 0x08, 0x99});
  state::Writer w;
  a.sync_state(w);

  Cart b = make_cart(nds, "ZZZZ");
  state::Reader r(w.data().data(), w.data().size());
  b.sync_state(r);
  CHECK(r.ok() && b.save_type() == SaveType::Flash && b.sram().size() == 524288 && b.sram()[8] == 0x99);

  Cart fresh = make_cart(nds, "ZZZZ");
  state::Writer w0;
  fresh.sync_state(w0);
  state::Reader r0(w0.data().data(), w0.data().size());
  a.sync_state(r0);
  CHECK(r0.ok() && a.save_type() == SaveType::Detect && a.sram().empty());
}

} // namespace

int main() {
  test_size_classes();
  test_detect_from_first_read();
  test_detecting_write_is_kept();
  test_lenient_flash();
  test_load_save();
  test_state_carries_the_chip();
  std::puts("cart_save: ok");
  return 0;
}
