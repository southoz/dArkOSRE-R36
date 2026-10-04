// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// How many cores the emulator's threads can share (tunes the 3D band pool).
#pragma once
#include "core/types.h"
#include <string>

namespace ds {

// DS_HOST_CORES=N overrides, then set_host_cores(). Else the CPUs online now, re-read at
// most once a second (the online set can move under a running game).
u32 host_cores();
// Frontend setting (emu.host_cores); 0 = detect.
void set_host_cores(u32 n);

// Calling thread's name (15 chars max on Linux), for profilers/debuggers.
void name_current_thread(const char* name);

// Thread layout (emu.thread_layout): with four or more usable cores, each
// thread role gets a core of its own or a shared pair, the DraStic shape:
//   core A (the one taking the device interrupts, CPU0 on the RK3566): aux --
//          audio, SDL, input, driver threads, anything we did not start
//   core B: 2D engine A + 3D band 0      core C: 2D engine B + 3D band 1
//   core D: emulation (CPU cores, JIT, scheduler; steals raster bins)
// Bands past 1 share the aux core. Off, or fewer cores: threads migrate
// freely. DS_PIN="emu=3,band0=1,line=2,aux=0,..." overrides per role.
enum class ThreadRole { Emu, Band, Line, EngineA, Aux };
void set_thread_layout(bool on);
bool thread_layout_on();
// Latches the calling thread's scheduling policy as the one the video
// workers run at (call once the frontend has set it, before any worker
// starts). place_current_thread then applies it per role, so a worker's
// policy does not depend on which thread happened to create it.
// DS_EMU_OTHER=1 makes the emulation thread a normal task and keeps the
// workers real-time. On ROCKNIX that costs ~1.3 ms a frame (ST): RR is what
// keeps PipeWire's SCHED_FIFO data loops and the input-poll kworker (1 kHz)
// off the emulation core.
void latch_worker_sched();
// Places the calling thread (`index`: the band number).
void place_current_thread(ThreadRole role, u32 index = 0);
// Moves every thread of the process not placed through place_current_thread
// (audio, driver pools, ...) to the aux core. Call after the libraries that
// start threads are up; again later catches lazily started ones.
void place_foreign_threads();

} // namespace ds
