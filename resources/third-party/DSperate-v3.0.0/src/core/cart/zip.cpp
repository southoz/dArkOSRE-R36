// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.

#include "core/cart/zip.h"

#include <cstring>

#include "core/cart/cart.h"
#include "core/cart/miniz/miniz_tinfl.h"

namespace ds::cart {
namespace {

constexpr u32 SIG_LOCAL   = 0x04034B50;   // "PK\3\4"
constexpr u32 SIG_CENTRAL = 0x02014B50;   // "PK\1\2"
constexpr u32 SIG_EOCD    = 0x06054B50;   // "PK\5\6"

constexpr u16 METHOD_STORE   = 0;
constexpr u16 METHOD_DEFLATE = 8;

// Largest image unpacked; caps a hostile archive's declared uncompressed size.
constexpr u64 MAX_ROM = 512ull << 20;

// Zip multi-byte fields are little-endian and unaligned; bounds-checked by the caller.
u16 rd16(const u8* p) { return static_cast<u16>(p[0] | (p[1] << 8)); }
u32 rd32(const u8* p) { return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) |
                               (static_cast<u32>(p[2]) << 16) | (static_cast<u32>(p[3]) << 24); }

enum class Kind { Other, Rom, Cia };   // case-insensitive extension match

Kind entry_kind(const char* name, size_t len) {
  if (len < 4) return Kind::Other;
  const char* e = name + len - 4;
  if (e[0] != '.') return Kind::Other;
  auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; };
  const char a = lower(e[1]), b = lower(e[2]), c = lower(e[3]);
  if (a == 'n' && b == 'd' && c == 's') return Kind::Rom;
  if (a == 'd' && b == 's' && c == 'i') return Kind::Rom;
  if (a == 's' && b == 'r' && c == 'l') return Kind::Rom;
  if (a == 'c' && b == 'i' && c == 'a') return Kind::Cia;   // SRL in a CIA container
  return Kind::Other;
}

struct Entry {
  size_t   name_off = 0, name_len = 0;
  u16      method = 0;
  u32      crc = 0;
  u64      csize = 0, usize = 0;
  size_t   local_off = 0;
  size_t   order = 0;         // position in the central directory, the tie-break
  bool     cia = false;
};

// Inflates raw DEFLATE from `src` into `sink`, stopping once `want` bytes are
// produced. Returns the count produced; a short return means a corrupt
// stream, not a short ROM. Dictionary must be the full 32 KB window even for
// a small `want`, since a back-reference may reach that far.
template <class Sink>
size_t inflate_raw(const u8* src, size_t csize, size_t want, Sink&& sink) {
  tinfl_decompressor d;
  tinfl_init(&d);
  std::vector<u8> dict(TINFL_LZ_DICT_SIZE);
  size_t in_ofs = 0, dict_ofs = 0, out_total = 0;
  for (;;) {
    size_t in_bytes = csize - in_ofs;
    size_t out_bytes = TINFL_LZ_DICT_SIZE - dict_ofs;
    const tinfl_status st = tinfl_decompress(&d, src + in_ofs, &in_bytes,
                                             dict.data(), dict.data() + dict_ofs, &out_bytes,
                                             TINFL_FLAG_HAS_MORE_INPUT);
    in_ofs += in_bytes;
    if (out_bytes) {
      const size_t take = out_total + out_bytes > want ? want - out_total : out_bytes;
      if (!sink(dict.data() + dict_ofs, take)) return out_total;
      out_total += take;
      if (out_total >= want) return out_total;   // got what we came for
    }
    dict_ofs = (dict_ofs + out_bytes) & (TINFL_LZ_DICT_SIZE - 1);
    if (st == TINFL_STATUS_DONE) return out_total;
    // NEEDS_MORE_INPUT with nothing left, or any negative status: malformed stream.
    if (st != TINFL_STATUS_HAS_MORE_OUTPUT && st != TINFL_STATUS_NEEDS_MORE_INPUT) return out_total;
    if (st == TINFL_STATUS_NEEDS_MORE_INPUT && in_ofs >= csize) return out_total;
  }
}

bool read_entry(const u8* zip, const Entry& e, size_t data_off, u8* dst, size_t want) {
  if (want > e.usize) return false;
  if (e.method == METHOD_STORE) {
    if (e.csize != e.usize) return false;
    std::memcpy(dst, zip + data_off, want);
    return true;
  }
  size_t got = 0;
  return inflate_raw(zip + data_off, static_cast<size_t>(e.csize), want,
                     [&](const u8* p, size_t n) { std::memcpy(dst + got, p, n); got += n; return true; }) == want;
}

// Central directory's name/extra-field lengths may differ from the local
// header's own, so the local header must be read to find the payload.
bool data_offset(const u8* zip, size_t size, const Entry& e, size_t& out) {
  if (e.local_off + 30 > size) return false;
  const u8* lh = zip + e.local_off;
  if (rd32(lh) != SIG_LOCAL) return false;
  const size_t off = e.local_off + 30 + rd16(lh + 26) + rd16(lh + 28);
  if (off > size || e.csize > size - off) return false;
  out = off;
  return true;
}

} // namespace

bool is_zip(const u8* data, size_t size) {
  return size >= 4 && rd32(data) == SIG_LOCAL;
}

bool find_rom(const u8* zip, size_t size, ZipEntry& entry, std::string& err) {
  err.clear();
  // EOCD is last, but a trailing comment of up to 64 KB may follow it.
  if (size < 22) { err = "not a zip archive (too short)"; return false; }
  size_t eocd = 0;
  bool found = false;
  const size_t limit = size < 22 + 0xFFFF ? size : 22 + 0xFFFF;
  for (size_t back = 22; back <= limit; ++back) {
    if (rd32(zip + size - back) == SIG_EOCD) { eocd = size - back; found = true; break; }
  }
  if (!found) { err = "not a zip archive (no end-of-central-directory record)"; return false; }

  const u32 count = rd16(zip + eocd + 10);
  const u32 cd_size = rd32(zip + eocd + 12);
  const u32 cd_off = rd32(zip + eocd + 16);
  if (cd_off > size || cd_size > size - cd_off) { err = "corrupt zip (central directory out of range)"; return false; }

  std::vector<Entry> cands;
  size_t p = cd_off;
  const size_t cd_end = cd_off + cd_size;
  size_t rom_seen = 0, skipped_zip64 = 0, skipped_crypt = 0, skipped_method = 0, skipped_huge = 0;
  for (u32 i = 0; i < count && p + 46 <= cd_end; ++i) {
    const u8* h = zip + p;
    if (rd32(h) != SIG_CENTRAL) { err = "corrupt zip (bad central directory entry)"; return false; }
    const u16 flags = rd16(h + 8);
    const u16 method = rd16(h + 10);
    const u32 crc = rd32(h + 16), csize = rd32(h + 20), usize = rd32(h + 24);
    const u16 name_len = rd16(h + 28), extra_len = rd16(h + 30), cmt_len = rd16(h + 32);
    const u32 local_off = rd32(h + 42);
    const size_t name_off = p + 46;
    const size_t next = name_off + name_len + extra_len + cmt_len;
    if (next > cd_end) { err = "corrupt zip (central directory entry overruns)"; return false; }
    p = next;

    const Kind kind = entry_kind(reinterpret_cast<const char*>(zip + name_off), name_len);
    if (kind == Kind::Other) continue;
    ++rom_seen;
    // Counted rather than fatal: a zip may hold one usable ROM beside another we can't read.
    if (flags & 1) { ++skipped_crypt; continue; }
    if (csize == 0xFFFFFFFFu || usize == 0xFFFFFFFFu || local_off == 0xFFFFFFFFu) { ++skipped_zip64; continue; }
    if (method != METHOD_STORE && method != METHOD_DEFLATE) { ++skipped_method; continue; }
    if (usize < 0x160) continue;      // smaller than a DS header: not a ROM
    if (usize > MAX_ROM) { ++skipped_huge; continue; }

    Entry e;
    e.name_off = name_off; e.name_len = name_len;
    e.method = method; e.crc = crc; e.csize = csize; e.usize = usize;
    e.local_off = local_off; e.order = cands.size();
    e.cia = kind == Kind::Cia;
    cands.push_back(e);
  }

  if (cands.empty()) {
    if (!rom_seen) err = "no .nds, .dsi, .srl or .cia file in the archive";
    else if (skipped_crypt) err = "the ROM in the archive is encrypted";
    else if (skipped_zip64) err = "the archive is zip64, which is not supported";
    else if (skipped_method) err = "the ROM uses an unsupported compression method";
    else if (skipped_huge) err = "the ROM in the archive is larger than any DS card";
    else err = "the file in the archive is too small to be a ROM";
    return false;
  }

  // A bare image is always preferred over a CIA, which must be unwrapped
  // before its header is readable. Only with no bare entry does a CIA win.
  std::vector<size_t> bare;
  for (size_t i = 0; i < cands.size(); ++i) if (!cands[i].cia) bare.push_back(i);

  size_t best = bare.empty() ? 0 : bare[0];
  if (bare.size() > 1) {
    int best_known = -1, best_rev = -1;
    for (size_t i : bare) {
      size_t off = 0;
      if (!data_offset(zip, size, cands[i], off)) continue;
      u8 head[sizeof(Header)];
      if (!read_entry(zip, cands[i], off, head, sizeof head)) continue;
      Header hdr;
      std::memcpy(&hdr, head, sizeof hdr);
      const int known = known_game_code(hdr.game_code_u32()) ? 1 : 0;
      const int rev = hdr.rom_version;
      // Database membership first, then revision, then archive order (strict >, never ties).
      if (known > best_known || (known == best_known && rev > best_rev)) {
        best_known = known; best_rev = rev; best = i;
      }
    }
    if (best_known < 0) { err = "no readable ROM in the archive"; return false; }
  }

  const Entry& e = cands[best];
  size_t off = 0;
  if (!data_offset(zip, size, e, off)) { err = "corrupt zip (entry data out of range)"; return false; }
  if (e.method == METHOD_STORE && e.csize != e.usize) { err = "corrupt zip (stored entry sizes disagree)"; return false; }
  entry.name.assign(reinterpret_cast<const char*>(zip + e.name_off), e.name_len);
  entry.method = e.method; entry.data_off = off; entry.cia = e.cia;
  entry.csize = e.csize; entry.usize = e.usize; entry.crc32 = e.crc;
  return true;
}

u32 crc32_update(u32 crc, const u8* data, size_t n) {
  static const u32* table = [] {
    static u32 t[256];
    for (u32 i = 0; i < 256; ++i) {
      u32 c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      t[i] = c;
    }
    return t;
  }();
  crc = ~crc;
  for (size_t i = 0; i < n; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

bool inflate_entry(const u8* zip, size_t size, const ZipEntry& entry, ZipSink sink, void* sink_user,
                   ZipProgress progress, void* progress_user, std::string& err) {
  err.clear();
  if (entry.data_off > size || entry.csize > size - entry.data_off) { err = "corrupt zip (entry data out of range)"; return false; }
  const u8* src = zip + entry.data_off;
  const u64 total = entry.usize;
  u64 done = 0, next_report = 0;
  u32 crc = 0;
  bool sink_ok = true;
  auto feed = [&](const u8* p, size_t n) {
    crc = crc32_update(crc, p, n);
    if (!sink(sink_user, p, n)) { sink_ok = false; return false; }
    done += n;
    if (progress && done >= next_report) { progress(progress_user, done, total); next_report = done + (1u << 20); }
    return true;
  };
  if (entry.stored()) {
    // 1 MB pieces, matching the deflate path's progress rhythm.
    while (done < total) {
      const size_t n = static_cast<size_t>(total - done < (1u << 20) ? total - done : (1u << 20));
      if (!feed(src + done, n)) break;
    }
  } else {
    inflate_raw(src, static_cast<size_t>(entry.csize), static_cast<size_t>(total), feed);
  }
  if (!sink_ok) { err = "could not write the extracted ROM"; return false; }
  if (done != total) { err = "corrupt zip (the compressed stream ended early)"; return false; }
  if (crc != entry.crc32) { err = "corrupt zip (the extracted file's CRC does not match)"; return false; }
  if (progress) progress(progress_user, done, total);
  return true;
}

size_t peek_entry(const u8* zip, size_t size, const ZipEntry& entry, u8* out, size_t n) {
  if (entry.data_off > size || entry.csize > size - entry.data_off) return 0;
  const size_t want = n < entry.usize ? n : static_cast<size_t>(entry.usize);
  if (!want) return 0;
  const u8* src = zip + entry.data_off;
  if (entry.stored()) { std::memcpy(out, src, want); return want; }
  size_t got = 0;
  return inflate_raw(src, static_cast<size_t>(entry.csize), want,
                     [&](const u8* p, size_t m) { std::memcpy(out + got, p, m); got += m; return true; });
}

std::string cia_refusal(const std::string& name) {
  return "the archive holds " + name + ", a DSiWare CIA; it runs in DSi mode, which needs the DSi BIOS dumps";
}

bool extract_rom(const u8* zip, size_t size, std::vector<u8>& out, std::string& err,
                 std::string* chosen, bool allow_cia, bool* was_cia) {
  ZipEntry e;
  if (was_cia) *was_cia = false;
  if (!find_rom(zip, size, e, err)) return false;
  if (e.cia && !allow_cia) { err = cia_refusal(e.name); return false; }
  if (was_cia) *was_cia = e.cia;
  out.clear();
  out.reserve(static_cast<size_t>(e.usize));
  auto sink = [](void* user, const u8* p, size_t n) {
    auto& v = *static_cast<std::vector<u8>*>(user);
    v.insert(v.end(), p, p + n);
    return true;
  };
  if (!inflate_entry(zip, size, e, sink, &out, nullptr, nullptr, err)) { out.clear(); return false; }
  if (chosen) *chosen = e.name;
  return true;
}

} // namespace ds::cart
