// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"

#include <vector>

namespace ds::sdl {

// Microphone via direct ALSA, bypassing SDL/PipeWire: on the handhelds
// PipeWire's only capture "source" is a monitor of the speaker. libasound is
// dlopen'd; on failure open() returns false and the SDL path is used.
class MicAlsa {
public:
  bool open(u32 rate, const char* device = nullptr);   // null/empty device = plughw:0,0
  void close();
  bool active() const { return pcm_ != nullptr; }
  // True if the PCM opened but rejected our params: device is busy/restricted,
  // so the SDL fallback (which would hit the same codec) is skipped too.
  bool rejected() const { return rejected_; }
  void capture(std::vector<s16>& out);   // everything available, non-blocking

private:
  bool rejected_ = false;
  void* lib_ = nullptr;
  void* pcm_ = nullptr;
  long (*readi_)(void*, void*, unsigned long) = nullptr;
  int  (*recover_)(void*, int, int) = nullptr;
  int  (*close_)(void*) = nullptr;
};

} // namespace ds::sdl
