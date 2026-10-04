// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Hashes a ROM for RetroAchievements. Feeds bytes via RomSource rather than
// letting rcheevos open the file, so a zipped ROM hashes without unpacking
// and the secure-area rewrite never reaches the hash (read_unpatched).
#pragma once

#include <string>

namespace ds::cart { class RomSource; }

namespace ds::cheevos {

// 32 lowercase hex chars in `out`, or false with a reason in `err`. `name` is
// for messages only. Safe to call on any thread.
bool rom_hash(const cart::RomSource& src, const std::string& name,
              std::string& out, std::string& err, bool dsi = false);

} // namespace ds::cheevos
