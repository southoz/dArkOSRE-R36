// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"
#include "core/gpu/vram_map.h"

#include <unordered_map>
#include <vector>

namespace ds::gpu {

// Decoded-texture cache: each (format, address, size, palette) decodes once
// into one 32-bit word per texel (colour16 | alpha5<<16), so span kernels do
// a single load per texel.
//
// Validity is by content, not write tracking, but gated by VramMap::generation
// (texture VRAM isn't CPU-mapped, so no VRAMCNT remap of its banks means the
// bytes can't have moved) to skip the compare on a hit.
// DS_TEXCACHE_VERIFY=1 compares anyway and aborts if the gate was wrong.
class TextureCache {
public:
  static constexpr size_t BUDGET_BYTES = 24u << 20;
  TextureCache();
  ~TextureCache();

  void begin_frame(u64 frame);
  // Decoded texels (width*height words); nullptr for fmt 0.
  const u32* lookup(const VramMap& vm, u32 fmt, u32 base, u32 width, u32 height, u32 texpal, u32 alpha0);
  // Same, plus identity: `id` is stable for the entry's life, `version` counts
  // decodes, so a caller keeping its own copy can tell if it's stale.
  struct Ref { const u32* texels = nullptr; u32 words = 0; u32 id = 0; u32 version = 0; bool transparent = false; };   // transparent: any texel has alpha 0
  Ref lookup_ref(const VramMap& vm, u32 fmt, u32 base, u32 width, u32 height, u32 texpal, u32 alpha0);
  void clear();
  u32  decodes_this_frame() const { return decodes_; }

private:
  struct Source { u32 addr = 0, len = 0; bool palette = false; };   // a validated range
  struct Entry {
    u32 fmt, base, width, height, texpal, alpha0;
    Source src[3]; u32 nsrc = 0;
    std::vector<u8> copy;        // the source ranges, concatenated
    std::vector<u32> texels;
    u64 validated = 0, used = 0; // frames
    u32 id = 0, version = 0;     // identity and decode count, for Ref
    bool transparent = false;
    u32 gen = 0, banks = 0;      // generation and banks last validated at
    u64 sig = 0;                 // block_signature of the ranges then
  };
  static u64 key(u32 fmt, u32 base, u32 width, u32 height, u32 texpal, u32 alpha0);
  void decode(const VramMap& vm, Entry& e);
  void snapshot(const VramMap& vm, Entry& e);
  bool unchanged(const VramMap& vm, Entry& e);
  void stamp(const VramMap& vm, Entry& e) const;
  bool verify_ = false;   // DS_TEXCACHE_VERIFY

  Entry& find_or_decode(const VramMap& vm, u32 fmt, u32 base, u32 width, u32 height, u32 texpal, u32 alpha0);
  std::unordered_map<u64, Entry> entries_;
  u32 next_id_ = 1;
  size_t bytes_ = 0;
  u64 frame_ = 0;
  u32 gate_hits_ = 0;   // DS_TEXCACHE_VERIFY report: gate-only hits
  u32 decodes_ = 0;
};

} // namespace ds::gpu
