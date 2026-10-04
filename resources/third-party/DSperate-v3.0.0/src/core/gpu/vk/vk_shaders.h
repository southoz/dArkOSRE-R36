// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#ifndef DS_VK_SHADERS_H
#define DS_VK_SHADERS_H
#include "core/types.h"
#include <cstddef>

// GPU raster's SPIR-V, linked in from the tracked blobs beside the .comp sources.

namespace ds::gpu::vk {

struct Spirv {
  const u32* code = nullptr;
  size_t     bytes = 0;      // vkCreateShaderModule wants a byte count
};

Spirv shader_tri_fs();       // a fullscreen triangle (the resolve subpass)
Spirv shader_lean_vert();    // lean path (vk_lean.cpp): fans at 1x, MSAA vertex placement
Spirv shader_lean_frag();    // ... its one fragment stage
Spirv shader_lean_frag_early();   // ... without the alpha test (early depth test) for polygons that cannot fail it
Spirv shader_lean_frag_opq();     // ... a translucent polygon's alpha-31 pixels only (drawn as opaque)
Spirv shader_lean_frag_trans();   // ... its other pixels only (the translucent passes)
Spirv shader_lean_resolve(); // ... and the resolve into the 3D layer record
Spirv shader_lean_resolve_1x();   // ... the same over single-sample attachments (aa off)

} // namespace ds::gpu::vk

#endif // DS_VK_SHADERS_H
