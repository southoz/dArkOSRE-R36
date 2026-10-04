// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/gpu/texcache.h"
#include "core/profile.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ds::gpu {

namespace {

// Chunked over unique-bank blocks; overlapping-bank bytes go through the OR read.
void view_copy(const VramMap& vm, const VramView& v, u32 addr, u32 len, u8* dst) {
  while (len) {
    const u32 a = addr & v.addr_mask();
    const u32 in_block = VramView::BLOCK - (a & (VramView::BLOCK - 1));
    const u32 n = std::min(len, in_block);
    if (const u8* p = v.ptr[a / VramView::BLOCK]) std::memcpy(dst, p + (a & (VramView::BLOCK - 1)), n);
    else for (u32 i = 0; i < n; ++i) dst[i] = vm.read8(v, a + i);
    addr += n; dst += n; len -= n;
  }
}
bool view_equal(const VramMap& vm, const VramView& v, u32 addr, u32 len, const u8* src) {
  while (len) {
    const u32 a = addr & v.addr_mask();
    const u32 in_block = VramView::BLOCK - (a & (VramView::BLOCK - 1));
    const u32 n = std::min(len, in_block);
    if (const u8* p = v.ptr[a / VramView::BLOCK]) { if (std::memcmp(src, p + (a & (VramView::BLOCK - 1)), n) != 0) return false; }
    else for (u32 i = 0; i < n; ++i) if (src[i] != vm.read8(v, a + i)) return false;
    addr += n; src += n; len -= n;
  }
  return true;
}

inline u32 pack(u32 colour16, u32 alpha) { return colour16 | (alpha << 16); }

} // namespace

TextureCache::TextureCache() { verify_ = std::getenv("DS_TEXCACHE_VERIFY") != nullptr; }
TextureCache::~TextureCache() { if (verify_) std::fprintf(stderr, "[texcache] verify: %u gated validations, all agreed with the compare\n", gate_hits_); }

u64 TextureCache::key(u32 fmt, u32 base, u32 width, u32 height, u32 texpal, u32 alpha0) {
  // bits: fmt:3 base>>3:16 lw:3 lh:3 texpal:13 alpha0:1
  u32 lw = 0, lh = 0;
  while ((8u << lw) < width) ++lw;
  while ((8u << lh) < height) ++lh;
  return static_cast<u64>(fmt) | (static_cast<u64>(base >> 3) << 3) | (static_cast<u64>(lw) << 19) | (static_cast<u64>(lh) << 22)
       | (static_cast<u64>(texpal & 0x1FFF) << 25) | (static_cast<u64>(alpha0 ? 1 : 0) << 38);
}

void TextureCache::begin_frame(u64 frame) {
  frame_ = frame;
  decodes_ = 0;
  if (bytes_ <= BUDGET_BYTES) return;
  // Over budget: drop unused-last-frame entries first, then anything stale.
  for (int pass = 0; pass < 2 && bytes_ > BUDGET_BYTES; ++pass) {
    for (auto it = entries_.begin(); it != entries_.end();) {
      const bool stale = pass == 0 ? (it->second.used + 1 < frame) : (it->second.used < frame);
      if (stale) { bytes_ -= it->second.texels.size() * 4 + it->second.copy.size(); it = entries_.erase(it); }
      else ++it;
    }
  }
}

void TextureCache::clear() { entries_.clear(); bytes_ = 0; }

const u32* TextureCache::lookup(const VramMap& vm, u32 fmt, u32 base, u32 width, u32 height, u32 texpal, u32 alpha0) {
  if (fmt == 0) return nullptr;
  return find_or_decode(vm, fmt, base, width, height, texpal, alpha0).texels.data();
}

TextureCache::Ref TextureCache::lookup_ref(const VramMap& vm, u32 fmt, u32 base, u32 width, u32 height, u32 texpal, u32 alpha0) {
  if (fmt == 0) return {};
  const Entry& e = find_or_decode(vm, fmt, base, width, height, texpal, alpha0);
  return { e.texels.data(), static_cast<u32>(e.texels.size()), e.id, e.version, e.transparent };
}

TextureCache::Entry& TextureCache::find_or_decode(const VramMap& vm, u32 fmt, u32 base, u32 width, u32 height, u32 texpal, u32 alpha0) {
  const u64 k = key(fmt, base, width, height, texpal, alpha0);
  auto it = entries_.find(k);
  if (it == entries_.end()) {
    Entry e; e.fmt = fmt; e.base = base; e.width = width; e.height = height; e.texpal = texpal; e.alpha0 = alpha0;
    e.id = next_id_++;
    it = entries_.emplace(k, std::move(e)).first;
    decode(vm, it->second);
    snapshot(vm, it->second);
    bytes_ += it->second.texels.size() * 4 + it->second.copy.size();
    it->second.validated = frame_;
    ++decodes_; prof::add(prof::C_TEXCACHE_DECODE, 1);
  } else if (it->second.validated != frame_) {
    if (unchanged(vm, it->second)) prof::add(prof::C_TEXCACHE_HIT, 1);
    else {
      bytes_ -= it->second.texels.size() * 4 + it->second.copy.size();
      decode(vm, it->second);
      snapshot(vm, it->second);
      bytes_ += it->second.texels.size() * 4 + it->second.copy.size();
      ++decodes_; prof::add(prof::C_TEXCACHE_DECODE, 1);
    }
    it->second.validated = frame_;
  }
  it->second.used = frame_;
  return it->second;
}

void TextureCache::snapshot(const VramMap& vm, Entry& e) {
  size_t total = 0;
  for (u32 i = 0; i < e.nsrc; ++i) total += e.src[i].len;
  e.copy.resize(total);
  u8* dst = e.copy.data();
  for (u32 i = 0; i < e.nsrc; ++i) { view_copy(vm, e.src[i].palette ? vm.texpal : vm.texture, e.src[i].addr, e.src[i].len, dst); dst += e.src[i].len; }
  stamp(vm, e);
}

void TextureCache::stamp(const VramMap& vm, Entry& e) const {
  e.gen = vm.generation(); e.banks = 0; e.sig = 0;
  for (u32 i = 0; i < e.nsrc; ++i) e.sig = e.sig * 31 + vm.block_signature(e.src[i].palette ? vm.texpal : vm.texture, e.src[i].addr, e.src[i].len, e.banks);
}

bool TextureCache::unchanged(const VramMap& vm, Entry& e) {
  bool compare = vm.generation() != e.gen;
  if (compare) {
    u32 banks = 0; u64 sig = 0;
    for (u32 i = 0; i < e.nsrc; ++i) sig = sig * 31 + vm.block_signature(e.src[i].palette ? vm.texpal : vm.texture, e.src[i].addr, e.src[i].len, banks);
    compare = sig != e.sig;
    for (u32 b = 0; b < 9 && !compare; ++b) if ((banks & (1u << b)) && vm.bank_writable_gen(static_cast<int>(b)) > e.gen) compare = true;
  }
  if (!compare) { ++gate_hits_; if (!verify_) { e.gen = vm.generation(); return true; } }
  const u8* src = e.copy.data();
  for (u32 i = 0; i < e.nsrc; ++i) {
    prof::add(prof::C_TEXCACHE_BYTES, e.src[i].len);
    if (!view_equal(vm, e.src[i].palette ? vm.texpal : vm.texture, e.src[i].addr, e.src[i].len, src)) {
      if (!compare) { std::fprintf(stderr, "[texcache] VERIFY FAILED: gate said unchanged, bytes differ (fmt %u base %x)\n", e.fmt, e.base); std::abort(); }
      return false;
    }
    src += e.src[i].len;
  }
  stamp(vm, e);
  return true;
}

// Mirrors Renderer3D::texture_sample.
void TextureCache::decode(const VramMap& vm, Entry& e) {
  ++e.version;
  const VramView& tv = vm.texture; const VramView& pv = vm.texpal;
  const u32 w = e.width, h = e.height, n = w * h;
  e.texels.resize(n);
  u32* out = e.texels.data();
  auto tex8 = [&](u32 a) { return vram_fetch8(vm, tv, a); };
  auto tex16 = [&](u32 a) { return vram_fetch16(vm, tv, a); };
  auto pal16 = [&](u32 a) { return vram_fetch16(vm, pv, a); };
  const u32 base = e.base, texpal = e.texpal, alpha0 = e.alpha0;
  e.nsrc = 0;
  auto src = [&](u32 addr, u32 len, bool palette) { e.src[e.nsrc++] = Source{addr, len, palette}; };

  switch (e.fmt) {
  case 1:   // A3I5
    for (u32 i = 0; i < n; ++i) { const u8 px = tex8(base + i); out[i] = pack(pal16((texpal << 4) + ((px & 0x1F) << 1)), ((px >> 3) & 0x1C) + (px >> 6)); }
    src(base, n, false); src(texpal << 4, 64, true);
    break;
  case 2:   // 4 colours
    for (u32 t = 0, i = 0; t < h; ++t) for (u32 s = 0; s < w; ++s, ++i) {
      const u8 px = (tex8(base + (i >> 2)) >> ((s & 3) << 1)) & 3;
      out[i] = pack(pal16((texpal << 3) + (px << 1)), px ? 31 : alpha0);
    }
    src(base, n / 4, false); src(texpal << 3, 8, true);
    break;
  case 3:   // 16 colours
    for (u32 t = 0, i = 0; t < h; ++t) for (u32 s = 0; s < w; ++s, ++i) {
      u8 px = tex8(base + (i >> 1)); px = (s & 1) ? (px >> 4) : (px & 0xF);
      out[i] = pack(pal16((texpal << 4) + (px << 1)), px ? 31 : alpha0);
    }
    src(base, n / 2, false); src(texpal << 4, 32, true);
    break;
  case 4:   // 256 colours
    for (u32 i = 0; i < n; ++i) { const u8 px = tex8(base + i); out[i] = pack(pal16((texpal << 4) + (px << 1)), px ? 31 : alpha0); }
    src(base, n, false); src(texpal << 4, 512, true);
    break;
  case 5: { // 4x4 compressed: blocks of 4 bytes, palette info in slot 1 at half the offset
    u32 pal_lo = 0xFFFFFFFFu, pal_hi = 0;
    for (u32 t = 0, i = 0; t < h; ++t) for (u32 s = 0; s < w; ++s, ++i) {
      u32 addr = base + ((t & 0x3FC) * (w >> 2)) + (s & 0x3FC) + (t & 3);
      addr &= 0x7FFFF;
      u32 slot1 = 0x20000 + ((addr & 0x1FFFC) >> 1);
      if (addr >= 0x40000) slot1 += 0x10000;
      const u32 val = (addr >= 0x20000 && addr < 0x40000) ? 0 : ((tex8(addr) >> (2 * (s & 3))) & 3);
      const u16 palinfo = tex16(slot1);
      const u32 paloff = (palinfo & 0x3FFF) << 2, pbase = (texpal << 4) + paloff;
      pal_lo = std::min(pal_lo, paloff); pal_hi = std::max(pal_hi, paloff + 8);
      const u16 c0 = pal16(pbase), c1 = pal16(pbase + 2);
      auto mix = [&](u32 ma, u32 mb, u32 sh) -> u16 {
        const u32 r = ((c0 & 0x1F) * ma + (c1 & 0x1F) * mb) >> sh;
        const u32 g = (((c0 & 0x3E0) * ma + (c1 & 0x3E0) * mb) >> sh) & 0x3E0;
        const u32 b = (((c0 & 0x7C00) * ma + (c1 & 0x7C00) * mb) >> sh) & 0x7C00;
        return static_cast<u16>(r | g | b);
      };
      const u32 mode = palinfo >> 14;
      u32 colour, alpha = 31;
      switch (val) {
      case 0: colour = c0; break;
      case 1: colour = c1; break;
      case 2: colour = mode == 1 ? mix(1, 1, 1) : (mode == 3 ? mix(5, 3, 3) : pal16(pbase + 4)); break;
      default:
        if (mode == 2) colour = pal16(pbase + 6);
        else if (mode == 3) colour = mix(3, 5, 3);
        else { colour = 0; alpha = 0; }
        break;
      }
      out[i] = pack(colour, alpha);
    }
    const u32 tex_bytes = n / 4;
    src(base & 0x7FFFF, tex_bytes, false);
    const u32 s1 = 0x20000 + (((base & 0x7FFFF) & 0x1FFFC) >> 1) + ((base & 0x7FFFF) >= 0x40000 ? 0x10000 : 0);
    src(s1, tex_bytes / 2, false);
    if (pal_hi > pal_lo) src((texpal << 4) + pal_lo, pal_hi - pal_lo, true);
    break;
  }
  case 6:   // A5I3
    for (u32 i = 0; i < n; ++i) { const u8 px = tex8(base + i); out[i] = pack(pal16((texpal << 4) + ((px & 7) << 1)), px >> 3); }
    src(base, n, false); src(texpal << 4, 16, true);
    break;
  default:  // direct colour
    for (u32 i = 0; i < n; ++i) { const u16 c = tex16(base + i * 2); out[i] = pack(c, (c & 0x8000) ? 31 : 0); }
    src(base, n * 2, false);
    break;
  }
  bool transparent = false;
  for (u32 i = 0; i < n && !transparent; ++i) transparent = ((out[i] >> 16) & 0x1Fu) == 0u;
  e.transparent = transparent;
}

} // namespace ds::gpu
