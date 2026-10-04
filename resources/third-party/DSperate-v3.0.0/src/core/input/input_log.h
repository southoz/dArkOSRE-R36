// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"

#include <cstdio>
#include <string>

namespace ds { struct NDS; }
namespace ds::input {

// One frame of player input, handed to the core before `run_frame`. Buttons are Io::Button
// bits; the pen is in screen pixels.
struct Frame {
  static constexpr int MIC_SAMPLES = 8;
  u16  buttons = 0;
  u8   x = 0, y = 0;
  bool down = false;
  bool lid = false;                 // hinge closed
  // MIC_SAMPLES evenly spaced samples over the frame (~480 Hz); enough for loudness detection.
  s8   mic[MIC_SAMPLES] = {};
  bool silent() const { for (s8 m : mic) if (m) return false; return true; }
  bool operator==(const Frame& o) const {
    if (buttons != o.buttons || x != o.x || y != o.y || down != o.down || lid != o.lid) return false;
    for (int i = 0; i < MIC_SAMPLES; ++i) if (mic[i] != o.mic[i]) return false;
    return true;
  }
};

// Drives the registers from a frame. With `mic`, the core samples that buffer (any rate, s16
// mono) instead of the frame's own eight; the frame's eight are logged either way.
void apply(NDS& nds, const Frame& f, const s16* mic = nullptr, size_t mic_count = 0);
// Decimates a capture buffer into a frame's mic samples (peak-preserving per slot).
void decimate_mic(Frame& f, const s16* mic, size_t mic_count);

// Input log file: one record per frame, replayable headlessly (deterministic given ROM, BIOS,
// battery save).
//
// Layout: 16-byte header ("DSIN", version u32, frame count u32 written on close, 4 reserved),
// then one record per frame. Version 2 records: buttons u16, x u8, y u8, flags u8 (bit 0 pen
// down, bit 1 lid closed), 3 reserved, eight s8 mic samples. Version 1 records are the first
// 8 bytes only (silent mic, lid open).
class Log {
public:
  ~Log() { close(); }
  bool open_write(const std::string& path);
  bool open_read(const std::string& path);
  void close();

  void write(const Frame& f);
  bool read(Frame& f);            // false at the end
  u32  frames() const { return count_; }
  bool writing() const { return f_ && write_; }
  bool reading() const { return f_ && !write_; }

private:
  FILE* f_ = nullptr;
  bool  write_ = false;
  u32   count_ = 0;
  size_t record_ = 16;
};

} // namespace ds::input
