// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include <cstddef>
#include <cstdint>

namespace ds {

using u8  = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s8  = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;

enum class Cpu : u8 { ARM9 = 0, ARM7 = 1 };

// Bus clock; both CPUs are expressed in ARM9 cycles (the ARM7 runs at half).
constexpr u32 ARM9_CLOCK_HZ = 67'027'964;
constexpr u32 ARM7_CLOCK_HZ = ARM9_CLOCK_HZ / 2;

// Scheduler time is counted in ARM9 cycles (2 per system/ARM7 cycle).
// One scanline is 355 dots x 6 system cycles = 2130 system cycles; a frame is
// 263 scanlines (560,190 system cycles, ~59.83 Hz).
constexpr u32 SYSTEM_CYCLES_PER_SCANLINE = 355 * 6;
constexpr u32 CYCLES_PER_SCANLINE = SYSTEM_CYCLES_PER_SCANLINE * 2;
constexpr u32 SCANLINES_PER_FRAME = 263;
constexpr u32 CYCLES_PER_FRAME    = CYCLES_PER_SCANLINE * SCANLINES_PER_FRAME;

constexpr u32 SCREEN_W = 256;
constexpr u32 SCREEN_H = 192;

} // namespace ds
