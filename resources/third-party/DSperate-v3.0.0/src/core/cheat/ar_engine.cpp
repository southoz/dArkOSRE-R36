// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/cheat/ar_engine.h"
#include "core/nds.h"

#include <cstdio>

namespace ds::cheat {
namespace {

constexpr Cpu kCpu = Cpu::ARM7;   // the real cartridge runs off the ARM7's VBlank handler

u32 ror32(u32 v, u32 n) { n &= 31; return n ? ((v >> n) | (v << (32 - n))) : v; }

} // namespace

const char* stop_name(Stop s) {
  switch (s) {
  case Stop::Ok:          return "ok";
  case Stop::BadOpcode:   return "bad opcode";
  case Stop::Truncated:   return "code ends mid-instruction";
  case Stop::Unsupported: return "unsupported opcode C4";
  case Stop::RunawayLoop: return "runaway loop";
  }
  return "?";
}

// `words` read two at a time: `a` carries the opcode in its top byte, `b` the
// operand. Skipped while the condition flag is clear, except opcodes that
// manipulate the flag itself.
Stop Engine::run_code(NDS& nds, const Code& code) {
  mem::Bus& bus = nds.bus;
  const std::vector<u32>& w = code.words;
  const size_t n = w.size() & ~size_t{1};   // an odd tail cannot form a pair

  size_t pc = 0;
  u32 offset = 0, datareg = 0;
  u32 cond = 1, condstack = 0;
  size_t loopstart = 0;
  u32 loopcount = 0, loopcond = 1, loopcondstack = 0;
  u32 c5count = 0;
  u64 steps = 0;

  const auto read32 = [&](u32 addr) { return bus.dma_read32(kCpu, addr); };
  const auto read16 = [&](u32 addr) { return bus.dma_read16(kCpu, addr); };
  const auto read8  = [&](u32 addr) { return bus.dma_read8(kCpu, addr); };

  while (pc + 1 < n) {
    if (++steps > MAX_STEPS) return Stop::RunawayLoop;
    const u32 a = w[pc], b = w[pc + 1];
    pc += 2;
    const u8 op = static_cast<u8>(a >> 24);
    const u8 hi = static_cast<u8>(op >> 4);

    // D0/D1/D2 and C5 run regardless (they restore the flag, end a loop, or
    // count); a skipped block-write must still step over its data.
    if ((op < 0xD0 && op != 0xC5) || op > 0xD2) {
      if (!cond) {
        if (hi == 0xE) {
          const u64 pairs = (static_cast<u64>(b) + 7) / 8;   // the data words that follow
          if (pc + pairs * 2 > n) return Stop::Truncated;
          pc += static_cast<size_t>(pairs * 2);
        }
        continue;
      }
    }

    // Address from `a`, or the offset register when `a`'s address field is zero.
    const auto cond_addr = [&] { const u32 addr = a & 0x0FFFFFFF; return addr ? addr : offset; };
    const auto push_cond = [&](bool taken) { condstack = (condstack << 1) | cond; cond = taken ? 1u : 0u; };

    switch (hi) {
    case 0x0: bus.dma_write32(kCpu, (a & 0x0FFFFFFF) + offset, b); break;
    case 0x1: bus.dma_write16(kCpu, (a & 0x0FFFFFFF) + offset, static_cast<u16>(b)); break;
    case 0x2: bus.dma_write8 (kCpu, (a & 0x0FFFFFFF) + offset, static_cast<u8>(b)); break;

    case 0x3: push_cond(b >  read32(cond_addr())); break;
    case 0x4: push_cond(b <  read32(cond_addr())); break;
    case 0x5: push_cond(b == read32(cond_addr())); break;
    case 0x6: push_cond(b != read32(cond_addr())); break;

    // Masks the loaded halfword with ~b.h first, so a code can test a few bits.
    case 0x7: case 0x8: case 0x9: case 0xA: {
      const u16 val = static_cast<u16>(read16(cond_addr()) & ~static_cast<u16>(b >> 16));
      const u16 chk = static_cast<u16>(b);
      push_cond(hi == 0x7 ? chk >  val
              : hi == 0x8 ? chk <  val
              : hi == 0x9 ? chk == val
                          : chk != val);
      break;
    }

    case 0xB: offset = read32((a & 0x0FFFFFFF) + offset); break;

    case 0xC:
      switch (op) {
      case 0xC0:   // FOR 0..b, with the flag state to restore when it ends
        loopstart = pc;
        loopcount = b;
        loopcond = cond;
        loopcondstack = condstack;
        break;
      case 0xC2: return Stop::Unsupported;   // flashcard native-code extension
      case 0xC4: return Stop::Unsupported;   // self-modifying code
      case 0xC5:   // count++, then test it -- counted even while skipping
        ++c5count;
        if (!cond) break;
        push_cond((c5count & (b & 0xFFFF)) == (b >> 16));
        break;
      case 0xC6: bus.dma_write32(kCpu, b, offset); break;
      default: return Stop::BadOpcode;
      }
      break;

    case 0xD:
      switch (op) {
      case 0xD0: cond = condstack & 1; condstack >>= 1; break;   // ENDIF
      case 0xD1:                                                 // NEXT
        if (loopcount > 0) { --loopcount; pc = loopstart; }
        else { cond = loopcond; condstack = loopcondstack; }
        break;
      case 0xD2:                                                 // NEXT, then reset everything
        if (loopcount > 0) { --loopcount; pc = loopstart; }
        else { offset = 0; datareg = 0; condstack = 0; cond = 1; }
        break;
      case 0xD3: offset = b; break;
      case 0xD4:                                                 // datareg <op>= b
        switch (a & 0xFF) {
        case 0x00: datareg += b; break;
        case 0x01: datareg |= b; break;
        case 0x02: datareg &= b; break;
        case 0x03: datareg ^= b; break;
        case 0x04: datareg = (b & 0xFF) > 31 ? 0 : datareg << (b & 0xFF); break;
        case 0x05: datareg = (b & 0xFF) > 31 ? 0 : datareg >> (b & 0xFF); break;
        case 0x06: datareg = ror32(datareg, b); break;
        case 0x07: datareg = (b & 0xFF) > 31 ? static_cast<u32>(static_cast<s32>(datareg) >> 31)
                                             : static_cast<u32>(static_cast<s32>(datareg) >> (b & 0xFF));
                   break;
        case 0x08: datareg *= b; break;
        default: return Stop::BadOpcode;
        }
        break;
      case 0xD5: datareg = b; break;
      case 0xD6: bus.dma_write32(kCpu, b + offset, datareg); offset += 4; break;
      case 0xD7: bus.dma_write16(kCpu, b + offset, static_cast<u16>(datareg)); offset += 2; break;
      case 0xD8: bus.dma_write8 (kCpu, b + offset, static_cast<u8>(datareg)); offset += 1; break;
      case 0xD9: datareg = read32(b + offset); break;
      case 0xDA: datareg = read16(b + offset); break;
      case 0xDB: datareg = read8(b + offset); break;
      case 0xDC: offset += b; break;
      default: return Stop::BadOpcode;
      }
      break;

    // Copy b bytes to a+offset. Packed two words per eight bytes; a trailing
    // 1-7 byte run still costs a whole pair.
    case 0xE: {
      const u64 pairs = (static_cast<u64>(b) + 7) / 8;
      if (pc + pairs * 2 > n) return Stop::Truncated;
      u32 dst = (a & 0x0FFFFFFF) + offset;
      u32 left = b;
      while (left >= 8) {
        bus.dma_write32(kCpu, dst, w[pc]);     dst += 4;
        bus.dma_write32(kCpu, dst, w[pc + 1]); dst += 4;
        pc += 2; left -= 8;
      }
      if (left > 0) {
        // Unpacked by hand, little-endian: the words are u32, host byte order
        // is not the code's business.
        const u8 bytes[8] = {
          static_cast<u8>(w[pc]), static_cast<u8>(w[pc] >> 8), static_cast<u8>(w[pc] >> 16), static_cast<u8>(w[pc] >> 24),
          static_cast<u8>(w[pc + 1]), static_cast<u8>(w[pc + 1] >> 8), static_cast<u8>(w[pc + 1] >> 16), static_cast<u8>(w[pc + 1] >> 24)};
        pc += 2;
        u32 at = 0;
        if (left >= 4) {
          bus.dma_write32(kCpu, dst, static_cast<u32>(bytes[0]) | (static_cast<u32>(bytes[1]) << 8) |
                                     (static_cast<u32>(bytes[2]) << 16) | (static_cast<u32>(bytes[3]) << 24));
          dst += 4; at = 4; left -= 4;
        }
        while (left > 0) { bus.dma_write8(kCpu, dst++, bytes[at++]); --left; }
      }
      break;
    }

    // Copy b bytes from the offset register to a (source is guest memory).
    case 0xF: {
      u32 src = offset, dst = a & 0x0FFFFFFF, left = b;
      for (; left >= 4; left -= 4, src += 4, dst += 4) bus.dma_write32(kCpu, dst, read32(src));
      for (; left > 0; --left, ++src, ++dst) bus.dma_write8(kCpu, dst, read8(src));
      break;
    }

    default: return Stop::BadOpcode;
    }
  }
  return pc == n ? Stop::Ok : Stop::Truncated;
}

void Engine::run(NDS& nds) {
  if (codes.empty()) return;
  if (complained_.size() != codes.size()) complained_.assign(codes.size(), 0);
  for (size_t i = 0; i < codes.size(); ++i) {
    const Code& c = codes[i];
    if (!c.enabled) continue;
    const Stop why = run_code(nds, c);
    // Say so once; it stops at the same place every frame after.
    if (why != Stop::Ok && !complained_[i]) {
      complained_[i] = 1;
      std::fprintf(stderr, "cheat: \"%s\" stopped: %s\n", c.name.c_str(), stop_name(why));
    }
  }
}

} // namespace ds::cheat
