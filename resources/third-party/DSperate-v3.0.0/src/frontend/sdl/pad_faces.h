// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include <SDL2/SDL.h>
#include <vector>

namespace ds::sdl {

// Checks a controller's SDL mapping against the kernel's positional face
// keys (BTN_SOUTH/EAST/WEST/NORTH). phys[f] is the SDL face button (0 a/south,
// 1 b/east, 2 x/west, 3 y/north) that SDL's button f really is. True only for
// a clean pair swap (a<->b and/or x<->y) the kernel can vouch for; false (phys
// left alone) when it can't tell or the mapping already agrees.
bool face_positions(SDL_GameController* pad, int phys[4]);

// The pure halves, for tests. SDL's evdev backend numbers buttons from
// BTN_JOYSTICK up to KEY_MAX, then from 0 up to BTN_JOYSTICK.
std::vector<int> sdl_button_codes(const std::vector<int>& key_codes);
bool face_positions_from(const std::vector<int>& codes, const int bind[4], int phys[4]);

} // namespace ds::sdl
