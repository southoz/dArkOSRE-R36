// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Sampling profiler for platforms with no perf. Uses ITIMER_PROF/SIGPROF to
// record the interrupted PC and thread id on every tick, across all threads.
//
// Output file: base/pid/obj/window/thread header lines followed by one
// "<pc> <tid>" line per sample. PCs outside any obj range are JIT code.
#pragma once
#include <cstdint>

namespace ds::pcsample {

// Installs the handler and starts the timer; starts inactive.
bool start();
void set_active(bool on);
// Stops the timer and writes the profile; false on failure.
bool write(const char* path);

} // namespace ds::pcsample
