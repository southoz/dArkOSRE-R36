// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"

namespace ds::sdl {

// Host lid switch (evdev SW_LID; SDL has no switch events). Hosts without a
// switch get a synthetic pulse: a BOOTTIME/MONOTONIC clock jump after resume
// closes the lid briefly, mimicking the real hinge sequence.
class Lid {
public:
  void open();
  void close();
  // Call once a frame. Returns true if state changed, stores it in `closed`.
  bool poll(bool& closed);
  bool has_switch() const { return fd_ >= 0; }

private:
  int  fd_ = -1;
  bool closed_ = false;
  int  pulse_ = 0;             // frames left of a suspend-detected close
  s64  skew_ns_ = 0;           // last boottime - monotonic
};

} // namespace ds::sdl
