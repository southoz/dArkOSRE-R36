// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/cpu/cpu.h"

namespace ds {

// ARM946E-S system control coprocessor: ID, control, PU regions (stored, not
// enforced), cache ops (no-ops), TCM region registers (forwarded to Bus).
u32  cp15_read (CpuContext& cpu, u32 opc1, u32 crn, u32 crm, u32 opc2);
void cp15_write(CpuContext& cpu, u32 opc1, u32 crn, u32 crm, u32 opc2, u32 value);
// Re-derive the PU/cacheability map after a save state restores registers.
void cp15_update_pu_map(CpuContext& cpu);

} // namespace ds
