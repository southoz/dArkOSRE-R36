// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/gpu/vk/vk_shaders.h"

// Compute shaders linked in as SPIR-V via .incbin; no shader compiler needed to build.

#define DS_INCBIN(sym, file)                                     \
  __asm__(".section .rodata\n"                                   \
          ".balign 4\n"                                          \
          ".globl " #sym "_data\n"                               \
          #sym "_data:\n"                                        \
          ".incbin \"" DSPERATE_VK_SHADER_DIR "/" file "\"\n"    \
          ".globl " #sym "_end\n"                                \
          #sym "_end:\n"                                         \
          ".previous\n");                                        \
  extern "C" const unsigned char sym##_data[], sym##_end[]

DS_INCBIN(ds_vk_tri_fs, "tri_fs_vert.spv");
DS_INCBIN(ds_vk_lean_vert, "lean_vert.spv");
DS_INCBIN(ds_vk_lean_frag, "lean_frag.spv");
DS_INCBIN(ds_vk_lean_frag_early, "lean_frag_early.spv");
DS_INCBIN(ds_vk_lean_frag_opq, "lean_frag_opq.spv");
DS_INCBIN(ds_vk_lean_frag_trans, "lean_frag_trans.spv");
DS_INCBIN(ds_vk_lean_resolve, "lean_resolve_frag.spv");
DS_INCBIN(ds_vk_lean_resolve_1x, "lean_resolve_1x_frag.spv");

namespace ds::gpu::vk {

Spirv shader_tri_fs() { return { reinterpret_cast<const u32*>(ds_vk_tri_fs_data), static_cast<size_t>(ds_vk_tri_fs_end - ds_vk_tri_fs_data) }; }

Spirv shader_lean_vert() { return { reinterpret_cast<const u32*>(ds_vk_lean_vert_data), static_cast<size_t>(ds_vk_lean_vert_end - ds_vk_lean_vert_data) }; }
Spirv shader_lean_frag() { return { reinterpret_cast<const u32*>(ds_vk_lean_frag_data), static_cast<size_t>(ds_vk_lean_frag_end - ds_vk_lean_frag_data) }; }
Spirv shader_lean_frag_opq() { return { reinterpret_cast<const u32*>(ds_vk_lean_frag_opq_data), static_cast<size_t>(ds_vk_lean_frag_opq_end - ds_vk_lean_frag_opq_data) }; }
Spirv shader_lean_frag_trans() { return { reinterpret_cast<const u32*>(ds_vk_lean_frag_trans_data), static_cast<size_t>(ds_vk_lean_frag_trans_end - ds_vk_lean_frag_trans_data) }; }
Spirv shader_lean_frag_early() { return { reinterpret_cast<const u32*>(ds_vk_lean_frag_early_data), static_cast<size_t>(ds_vk_lean_frag_early_end - ds_vk_lean_frag_early_data) }; }
Spirv shader_lean_resolve_1x() { return { reinterpret_cast<const u32*>(ds_vk_lean_resolve_1x_data), static_cast<size_t>(ds_vk_lean_resolve_1x_end - ds_vk_lean_resolve_1x_data) }; }
Spirv shader_lean_resolve() { return { reinterpret_cast<const u32*>(ds_vk_lean_resolve_data), static_cast<size_t>(ds_vk_lean_resolve_end - ds_vk_lean_resolve_data) }; }
} // namespace ds::gpu::vk
