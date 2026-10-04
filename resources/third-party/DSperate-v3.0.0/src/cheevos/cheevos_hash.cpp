// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "cheevos/cheevos_hash.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "core/cart/rom_source.h"
#include "rc_hash.h"

namespace ds::cheevos {
namespace {

using ds::u8;
using ds::u32;
using ds::u64;
using ds::s64;

// rcheevos' file-open callback takes no userdata, so the source is reached via
// a static; thread_local so hashing works from more than one thread.
thread_local const cart::RomSource* t_src = nullptr;

struct Cursor { u64 pos = 0; };

void* rs_open(const char*) {
  if (!t_src) return nullptr;
  return new Cursor{};
}

void rs_close(void* h) { delete static_cast<Cursor*>(h); }

void rs_seek(void* h, int64_t offset, int origin) {
  Cursor* c = static_cast<Cursor*>(h);
  const s64 size = t_src ? static_cast<s64>(t_src->size()) : 0;
  s64 to = offset;
  if (origin == SEEK_CUR) to = static_cast<s64>(c->pos) + offset;
  else if (origin == SEEK_END) to = size + offset;
  c->pos = to < 0 ? 0 : static_cast<u64>(to);
}

int64_t rs_tell(void* h) { return static_cast<int64_t>(static_cast<Cursor*>(h)->pos); }

size_t rs_read(void* h, void* buffer, size_t requested) {
  Cursor* c = static_cast<Cursor*>(h);
  if (!t_src || requested == 0) return 0;
  if (c->pos > 0xFFFFFFFFull) return 0;
  const u32 n = static_cast<u32>(std::min<u64>(requested, 0xFFFFFFFFu));
  // Return the short count, not `n`: lets rcheevos 0-pad a truncated read itself.
  const u32 have = t_src->read_unpatched(static_cast<u32>(c->pos), static_cast<u8*>(buffer), n);
  c->pos += have;
  return have;
}

void rs_error(const char* message, const rc_hash_iterator_t*) { std::fprintf(stderr, "cheevos: hash: %s\n", message); }

} // namespace

bool rom_hash(const cart::RomSource& src, const std::string& name,
              std::string& out, std::string& err, bool dsi) {
  out.clear();
  err.clear();
  if (src.size() < 0x200) { err = "too small to be a DS ROM"; return false; }

  rc_hash_iterator_t it;
  // Fixed synthetic name: a ".nds" extension picks the single-console path
  // without probing through the not-yet-replaced default filereader.
  rc_hash_initialize_iterator(&it, "dsperate.nds", nullptr, 0);
  it.callbacks.filereader.open = rs_open;
  it.callbacks.filereader.close = rs_close;
  it.callbacks.filereader.seek = rs_seek;
  it.callbacks.filereader.tell = rs_tell;
  it.callbacks.filereader.read = rs_read;
  it.callbacks.error_message = rs_error;

  char hash[33] = {};
  t_src = &src;
  const int ok = rc_hash_generate(hash, dsi ? RC_CONSOLE_NINTENDO_DSI : RC_CONSOLE_NINTENDO_DS, &it);
  t_src = nullptr;
  rc_hash_destroy_iterator(&it);

  if (!ok || hash[0] == '\0') {
    err = "rcheevos could not hash " + (name.empty() ? std::string("the ROM") : name);
    return false;
  }
  out.assign(hash);
  return true;
}

} // namespace ds::cheevos
