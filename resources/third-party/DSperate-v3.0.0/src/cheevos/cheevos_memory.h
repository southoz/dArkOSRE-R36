// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Translates RetroAchievements' flat per-console address space to ours: bounds
// check + memcpy from a host pointer, no page tables/traps/timing. DS: 4 MB
// main RAM, a 12 MB hole, then data TCM; DSi has the hole filled with RAM.
// An unbacked address reads as zero and reports itself unbacked, so rcheevos
// marks the achievement unsupported. The region table is checked against
// rcheevos at run time so an upstream map change fails loudly.
#pragma once

#include <cstdint>
#include <string>

#include "core/types.h"

namespace ds { class NDS; }

namespace ds::cheevos {

class Memory {
public:
  bool attach(NDS& nds, std::string& err);
  bool attached() const { return regions_[0].host != nullptr; }

  // Returns how many of `n` bytes were actually backed; unbacked bytes read
  // as zero. rcheevos treats a short count as "address not supported".
  u32 read(u32 address, u8* dst, u32 n) const;
  bool supported(u32 address, u32 n = 1) const;

  u32 max_address() const { return max_address_; }

  // `ud` is a Memory*. Safe to call unattached (reads zero).
  static u32 peek(u32 address, u32 num_bytes, void* ud);          // rc_runtime_peek_t
  static u32 read_memory(u32 address, u8* buffer, u32 num_bytes, void* ud);

private:
  struct Region {
    u32 start = 0, end = 0;   // inclusive RetroAchievements addresses
    u8* host = nullptr;       // null = exists in map but unbacked (the DSi hole)
    u32 size = 0;
  };
  Region regions_[3];
  u32 max_address_ = 0;
};

// Product token sent to the RetroAchievements server. A malformed value gets
// every endpoint refused with 403 unsupported_client.
const char* user_agent();

} // namespace ds::cheevos
