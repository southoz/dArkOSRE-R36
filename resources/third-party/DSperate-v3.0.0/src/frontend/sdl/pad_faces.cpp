// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
// Own translation unit: linux/input.h's BTN_A etc. collide with io::Io::Button.
#include "pad_faces.h"

#include <algorithm>
#include <cstring>
#ifdef __linux__
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace ds::sdl {

namespace {
// Kernel codes (Documentation/input/gamepad.rst), positional.
constexpr int kSouth = 0x130, kEast = 0x131, kNorth = 0x133, kWest = 0x134;
constexpr int kBtnJoystick = 0x120, kKeyMax = 0x2ff;
} // namespace

std::vector<int> sdl_button_codes(const std::vector<int>& key_codes) {
  std::vector<int> sorted = key_codes, out;
  std::sort(sorted.begin(), sorted.end());
  for (const int k : sorted) if (k >= kBtnJoystick && k < kKeyMax) out.push_back(k);
  for (const int k : sorted) if (k >= 0 && k < kBtnJoystick) out.push_back(k);
  return out;
}

bool face_positions_from(const std::vector<int>& codes, const int bind[4], int phys[4]) {
  static constexpr int kWant[4] = {kSouth, kEast, kWest, kNorth};   // SDL a, b, x, y
  int p[4];
  for (int f = 0; f < 4; ++f) {
    if (bind[f] < 0 || bind[f] >= static_cast<int>(codes.size())) return false;
    const int* at = std::find(kWant, kWant + 4, codes[static_cast<size_t>(bind[f])]);
    if (at == kWant + 4) return false;
    p[f] = static_cast<int>(at - kWant);
  }
  // Only pair swaps within a/b and x/y: anything else is a mapping we don't understand.
  if (!((p[0] == 0 && p[1] == 1) || (p[0] == 1 && p[1] == 0))) return false;
  if (!((p[2] == 2 && p[3] == 3) || (p[2] == 3 && p[3] == 2))) return false;
  if (p[0] == 0 && p[2] == 2) return false;   // already positional
  std::memcpy(phys, p, sizeof p);
  return true;
}

bool face_positions(SDL_GameController* pad, int phys[4]) {
#if defined(__linux__) && SDL_VERSION_ATLEAST(2, 24, 0)
  if (!pad) return false;
  SDL_Joystick* joy = SDL_GameControllerGetJoystick(pad);
  const char* path = joy ? SDL_JoystickPath(joy) : nullptr;
  if (!path || std::strncmp(path, "/dev/input/event", 16) != 0) return false;   // not SDL's evdev backend
  const int fd = ::open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) return false;
  constexpr int kBits = 8 * sizeof(unsigned long);
  unsigned long bits[(KEY_MAX + kBits) / kBits] = {};
  const bool ok = ::ioctl(fd, EVIOCGBIT(EV_KEY, sizeof bits), bits) >= 0;
  ::close(fd);
  if (!ok) return false;
  std::vector<int> keys;
  for (int k = 0; k < KEY_MAX; ++k) if ((bits[k / kBits] >> (k % kBits)) & 1) keys.push_back(k);
  const std::vector<int> codes = sdl_button_codes(keys);
  if (static_cast<int>(codes.size()) != SDL_JoystickNumButtons(joy)) return false;   // not the numbering we assume
  int bind[4];
  for (int f = 0; f < 4; ++f) {
    const SDL_GameControllerButtonBind b = SDL_GameControllerGetBindForButton(pad, static_cast<SDL_GameControllerButton>(f));
    bind[f] = b.bindType == SDL_CONTROLLER_BINDTYPE_BUTTON ? b.value.button : -1;
  }
  return face_positions_from(codes, bind, phys);
#else
  (void)pad; (void)phys;
  return false;
#endif
}

} // namespace ds::sdl
