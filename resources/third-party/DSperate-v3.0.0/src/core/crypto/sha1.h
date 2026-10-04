// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// SHA-1 (FIPS 180-4), one-shot. The DSi keys its NAND filesystem counter from
// the SHA-1 of the eMMC CID, and signs TWLCFG and HWINFO with it.
#pragma once
#include "core/types.h"

#include <cstddef>

namespace ds::crypto {

void sha1(const u8* data, size_t len, u8 out[20]);

}  // namespace ds::crypto
