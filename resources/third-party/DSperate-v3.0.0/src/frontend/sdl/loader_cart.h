// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"

#include <string>
#include <vector>

namespace ds::sdl {

// Builds the loader cart in memory: the card firmware shows with no game
// loaded, launching it raises the game list. A hand-made BootMenu.nds
// overrides this one. `title`/`subtitle` are UTF-8 banner lines (128 KB rom).
std::vector<u8> build_loader_cart(const std::string& title, const std::string& subtitle);

} // namespace ds::sdl
