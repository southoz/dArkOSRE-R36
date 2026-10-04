// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/spu/spu.h"
#include "core/state/state.h"
#include "core/nds.h"
#include "core/profile.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ds::spu {

// IMA-ADPCM step table and index adjustments (GBATEK "Sound ADPCM").
const u16 Spu::ADPCM_TABLE[89] = {
  0x0007, 0x0008, 0x0009, 0x000A, 0x000B, 0x000C, 0x000D, 0x000E, 0x0010, 0x0011, 0x0013, 0x0015,
  0x0017, 0x0019, 0x001C, 0x001F, 0x0022, 0x0025, 0x0029, 0x002D, 0x0032, 0x0037, 0x003C, 0x0042,
  0x0049, 0x0050, 0x0058, 0x0061, 0x006B, 0x0076, 0x0082, 0x008F, 0x009D, 0x00AD, 0x00BE, 0x00D1,
  0x00E6, 0x00FD, 0x0117, 0x0133, 0x0151, 0x0173, 0x0198, 0x01C1, 0x01EE, 0x0220, 0x0256, 0x0292,
  0x02D4, 0x031C, 0x036C, 0x03C3, 0x0424, 0x048E, 0x0502, 0x0583, 0x0610, 0x06AB, 0x0756, 0x0812,
  0x08E0, 0x09C3, 0x0ABD, 0x0BD0, 0x0CFF, 0x0E4C, 0x0FBA, 0x114C, 0x1307, 0x14EE, 0x1706, 0x1954,
  0x1BDC, 0x1EA5, 0x21B6, 0x2515, 0x28CA, 0x2CDF, 0x315B, 0x364B, 0x3BB9, 0x41B2, 0x4844, 0x4F7E,
  0x5771, 0x602F, 0x69CE, 0x7462, 0x7FFF,
};
const s8 Spu::ADPCM_INDEX[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

// PSG square: duty d = (d+1)/8 of the period low.
static s16 psg_sample(u32 duty, u32 phase) { return phase < 7 - duty ? -0x7FFF : 0x7FFF; }

void Spu::reset() {
  dbg_ = std::getenv("DS_DEBUG_SPU") != nullptr;
  for (auto& c : ch_) c = Channel{};
  for (auto& cp : cap_) cp = Capture{};
  cnt_ = 0; bias_ = 0; master_ = 0; muted_ = true;
  rd_ = wr_ = 0;
  mix_period_ = MIX_PERIOD; timer_step_ = TIMER_STEP;
  batch_ = nds_.dsi ? 1 : 16;   // DSi: slice-aligned mix events
  mix_at_ = nds_.sched.now() + mix_period_;
  nds_.sched.schedule(EventId::Spu, mix_at_, ev_mix);
}

// ---- registers --------------------------------------------------------------

void Spu::set_cnt(Channel& c, u32 v) {
  const u32 old = c.cnt;
  c.cnt = v & 0xFF7F837F;
  c.volume = c.cnt & 0x7F; if (c.volume == 127) c.volume = 128;
  static const u8 shifts[4] = {4, 3, 2, 0};
  c.vol_shift = shifts[(c.cnt >> 8) & 3];
  c.pan = (c.cnt >> 16) & 0x7F; if (c.pan == 127) c.pan = 128;
  if ((v & 0x80000000u) && !(old & 0x80000000u)) {
    c.key_on = true;
    if (dbg_) std::fprintf(stderr, "[spu] mix %llu f%llu line %u keyon ch%d cnt %08x src %08x loop %u len %u timer %04x\n", (unsigned long long)(mix_at_ / mix_period_), (unsigned long long)nds_.frame_count, nds_.gpu.line(), int(&c - ch_.data()), c.cnt, c.src, c.loop, c.len, c.timer_reload);
  }
}

void Spu::cap_set_cnt(Capture& cp, u8 v) {
  if ((v & 0x80) && !(cp.cnt & 0x80)) {
    cp.timer = cp.timer_reload; cp.pos = 0;
    cp.fifo_rd = cp.fifo_wr = cp.fifo_off = cp.fifo_level = 0;
  }
  v &= 0x8F;
  if (!(v & 0x80)) v &= ~0x01;
  cp.cnt = v;
  if ((v & 0x03) && !cap_warned_) { cap_warned_ = true; std::fprintf(stderr, "[spu] capture add/channel source modes not implemented (cnt %02x)\n", v); }
}

u32 Spu::read(u32 addr, u32 width) {
  catch_up();                             // SOUNDxCNT.31 / capture busy clear as samples end
  u32 v = 0;
  if (addr < 0x04000500) {
    const Channel& c = ch_[(addr >> 4) & 0xF];
    if ((addr & 0xC) == 0) v = c.cnt;                 // everything else is write-only
  } else {
    switch (addr & ~3u) {
    case 0x04000500: v = cnt_; break;
    case 0x04000504: v = bias_; break;
    case 0x04000508: v = cap_[0].cnt | (cap_[1].cnt << 8); break;
    case 0x04000510: v = cap_[0].dst; break;
    case 0x04000518: v = cap_[1].dst; break;
    default: break;
    }
  }
  const u32 shift = (addr & 3) * 8;
  if (width == 32) return v;
  if (width == 16) return (v >> (shift & 16)) & 0xFFFF;
  return (v >> shift) & 0xFF;
}

void Spu::write(u32 addr, u32 width, u32 value) {
  catch_up();
  if (dbg_ && (addr >= 0x04000500 || (width == 32 && (addr & 0xC) == 0 && (value & 0x80000000u)))) std::fprintf(stderr, "[spu] mix %llu f%llu line %u w%u %08x = %08x\n", (unsigned long long)(mix_at_ / mix_period_), (unsigned long long)nds_.frame_count, nds_.gpu.line(), width, addr, value);
  // Merge narrower writes into the 32-bit register image first; the side
  // effects below see the whole word.
  const u32 base = addr & ~3u;
  if (width != 32) {
    const u32 shift = (addr & 3) * 8;
    const u32 mask = (width == 16 ? 0xFFFFu : 0xFFu) << shift;
    u32 cur;
    if (base < 0x04000500) {
      const Channel& c = ch_[(base >> 4) & 0xF];
      switch (base & 0xC) {
      case 0x0: cur = c.cnt; break;
      case 0x4: cur = c.src; break;
      case 0x8: cur = c.timer_reload | (c.loop >> 2 << 16); break;
      default:  cur = c.len >> 2; break;
      }
    } else {
      switch (base) {
      case 0x04000500: cur = cnt_; break;
      case 0x04000504: cur = bias_; break;
      case 0x04000508: cur = cap_[0].cnt | (cap_[1].cnt << 8); break;
      case 0x04000510: cur = cap_[0].dst; break;
      case 0x04000514: cur = cap_[0].len >> 2; break;
      case 0x04000518: cur = cap_[1].dst; break;
      case 0x0400051C: cur = cap_[1].len >> 2; break;
      default: cur = 0; break;
      }
    }
    value = (cur & ~mask) | ((value << shift) & mask);
  }
  if (base < 0x04000500) {
    const u32 n = (base >> 4) & 0xF;
    Channel& c = ch_[n];
    switch (base & 0xC) {
    case 0x0: set_cnt(c, value); break;
    case 0x4: c.src = value & 0x07FFFFFC; break;
    case 0x8:
      c.timer_reload = static_cast<u16>(value);
      c.loop = (value >> 16) << 2;
      if (n == 1) cap_[0].timer_reload = c.timer_reload;   // captures share the channel 1/3 timers
      if (n == 3) cap_[1].timer_reload = c.timer_reload;
      break;
    default:  c.len = (value & 0x001FFFFF) << 2; break;
    }
    return;
  }
  switch (base) {
  case 0x04000500:
    cnt_ = value & 0xBF7F;
    master_ = cnt_ & 0x7F; if (master_ == 127) master_ = 128;
    break;
  case 0x04000504: bias_ = value & 0x3FF; break;
  case 0x04000508: cap_set_cnt(cap_[0], value & 0xFF); cap_set_cnt(cap_[1], (value >> 8) & 0xFF); break;
  case 0x04000510: cap_[0].dst = value & 0x07FFFFFC; break;
  case 0x04000514: cap_[0].len = (value & 0xFFFF) << 2; if (!cap_[0].len) cap_[0].len = 4; break;
  case 0x04000518: cap_[1].dst = value & 0x07FFFFFC; break;
  case 0x0400051C: cap_[1].len = (value & 0xFFFF) << 2; if (!cap_[1].len) cap_[1].len = 4; break;
  default: break;
  }
}

// ---- channel sample fetch ---------------------------------------------------

void Spu::fifo_fill(Channel& c) {
  const u32 total = c.loop + c.len;
  if (c.fifo_off >= total) {
    const u32 rep = c.repeat();
    if (rep & 1) c.fifo_off = c.loop;
    else if (rep & 2) return;                 // one-shot: nothing more to fetch
  }
  u32 burst = 16;
  if (c.fifo_off + 16 > total) burst = total - c.fifo_off;
  const bool readable = (c.src + c.fifo_off) >= 0x00004000;   // the SPU cannot read the ARM7 BIOS
  for (u32 i = 0; i < burst; i += 4) {
    c.fifo[c.fifo_wr] = readable ? nds_.bus.dma_read32(Cpu::ARM7, c.src + c.fifo_off) : 0;
    c.fifo_off += 4;
    c.fifo_wr = (c.fifo_wr + 1) & 7;
  }
  c.fifo_level += burst;
}

template <typename T> T Spu::fifo_read(Channel& c) {
  T v; std::memcpy(&v, reinterpret_cast<const u8*>(c.fifo) + c.fifo_rd, sizeof(T));
  c.fifo_rd = (c.fifo_rd + sizeof(T)) & 0x1F;
  c.fifo_level -= sizeof(T);
  if (c.fifo_level <= 16) fifo_fill(c);
  return v;
}

void Spu::start(Channel& c) {
  c.timer = c.timer_reload;
  c.pos = c.format() == 3 ? -1 : -3;
  c.noise = 0x7FFF;
  c.cur = 0;
  c.fifo_rd = c.fifo_wr = c.fifo_off = c.fifo_level = 0;
  if (c.format() != 3) { fifo_fill(c); fifo_fill(c); }
}

void Spu::next_pcm8(Channel& c) {
  if (++c.pos < 0) return;
  if (static_cast<u32>(c.pos) >= c.loop + c.len) {
    const u32 rep = c.repeat();
    if (rep & 1) c.pos = static_cast<s32>(c.loop);
    else if (rep & 2) { c.cur = 0; c.cnt &= ~0x80000000u; return; }
  }
  c.cur = static_cast<s16>(fifo_read<s8>(c) << 8);
}

void Spu::next_pcm16(Channel& c) {
  if (++c.pos < 0) return;
  if (static_cast<u32>(c.pos << 1) >= c.loop + c.len) {
    const u32 rep = c.repeat();
    if (rep & 1) c.pos = static_cast<s32>(c.loop >> 1);
    else if (rep & 2) { c.cur = 0; c.cnt &= ~0x80000000u; return; }
  }
  c.cur = fifo_read<s16>(c);
}

void Spu::next_adpcm(Channel& c) {
  ++c.pos;
  if (c.pos < 8) {
    if (c.pos == 0) {                          // header: initial predictor + step index
      const u32 h = fifo_read<u32>(c);
      c.adpcm_val = static_cast<s16>(h & 0xFFFF);
      c.adpcm_idx = std::min<s32>((h >> 16) & 0x7F, 88);
      c.adpcm_val_loop = c.adpcm_val; c.adpcm_idx_loop = c.adpcm_idx;
    }
    return;
  }
  if (static_cast<u32>(c.pos >> 1) >= c.loop + c.len) {
    const u32 rep = c.repeat();
    if (rep & 1) {
      c.pos = static_cast<s32>(c.loop << 1);
      c.adpcm_val = c.adpcm_val_loop; c.adpcm_idx = c.adpcm_idx_loop;
      c.adpcm_byte = fifo_read<u8>(c);
    } else if (rep & 2) { c.cur = 0; c.cnt &= ~0x80000000u; return; }
  } else {
    if (!(c.pos & 1)) c.adpcm_byte = fifo_read<u8>(c); else c.adpcm_byte >>= 4;
    const u32 step = ADPCM_TABLE[c.adpcm_idx];
    u32 diff = step >> 3;
    if (c.adpcm_byte & 1) diff += step >> 2;
    if (c.adpcm_byte & 2) diff += step >> 1;
    if (c.adpcm_byte & 4) diff += step;
    if (c.adpcm_byte & 8) c.adpcm_val = std::max(c.adpcm_val - static_cast<s32>(diff), -0x7FFF);
    else                  c.adpcm_val = std::min(c.adpcm_val + static_cast<s32>(diff),  0x7FFF);
    c.adpcm_idx = std::clamp(c.adpcm_idx + ADPCM_INDEX[c.adpcm_byte & 7], 0, 88);
    if (c.pos == static_cast<s32>(c.loop << 1)) { c.adpcm_val_loop = c.adpcm_val; c.adpcm_idx_loop = c.adpcm_idx; }
  }
  c.cur = static_cast<s16>(c.adpcm_val);
}

// Advances one channel by `n` timer ticks and returns its volume-scaled
// sample (16-bit sample << vol_shift * volume, i.e. up to 23 bits).
s32 Spu::run_channel(Channel& c, u32 n) {
  if (!(c.cnt & 0x80000000u)) return 0;
  const u32 fmt = c.format();
  const bool psg = fmt == 3 && (&c - ch_.data()) < 14;
  const bool noise = fmt == 3 && !psg;
  if (fmt < 3 && c.loop + c.len < 16) return 0;
  if (c.key_on) { start(c); c.key_on = false; }
  c.timer += n;
  while (c.timer >> 16) {
    c.timer = c.timer_reload + (c.timer - 0x10000);
    switch (fmt) {
    case 0: next_pcm8(c); break;
    case 1: next_pcm16(c); break;
    case 2: next_adpcm(c); break;
    default:
      if (noise) {
        if (c.noise & 1) { c.noise = (c.noise >> 1) ^ 0x6000; c.cur = -0x7FFF; }
        else             { c.noise >>= 1;                      c.cur =  0x7FFF; }
      } else {
        ++c.pos; c.cur = psg_sample((c.cnt >> 24) & 7, c.pos & 7);
      }
      break;
    }
    if (!(c.cnt & 0x80000000u)) break;
  }
  return (static_cast<s32>(c.cur) << c.vol_shift) * c.volume;
}

// ---- capture ----------------------------------------------------------------

void Spu::cap_flush(Capture& cp) {
  for (u32 i = 0; i < 4; ++i) {
    nds_.bus.dma_write32(Cpu::ARM7, cp.dst + cp.fifo_off, cp.fifo[cp.fifo_rd]);
    cp.fifo_rd = (cp.fifo_rd + 1) & 3;
    cp.fifo_level -= 4;
    cp.fifo_off += 4;
    if (cp.fifo_off >= cp.len) { cp.fifo_off = 0; break; }
  }
}

template <typename T> void Spu::cap_write(Capture& cp, T v) {
  std::memcpy(reinterpret_cast<u8*>(cp.fifo) + cp.fifo_wr, &v, sizeof(T));
  cp.fifo_wr = (cp.fifo_wr + sizeof(T)) & 0xF;
  cp.fifo_level += sizeof(T);
  if (cp.fifo_level >= 16) cap_flush(cp);
}

void Spu::cap_run(Capture& cp, s32 sample) {
  cp.timer += timer_step_;
  const bool eight = cp.cnt & 0x08;
  while (cp.timer >> 16) {
    cp.timer = cp.timer_reload + (cp.timer - 0x10000);
    if (eight) { cap_write<s8>(cp, static_cast<s8>(sample >> 8)); cp.pos += 1; }
    else       { cap_write<s16>(cp, static_cast<s16>(sample));    cp.pos += 2; }
    if (cp.pos >= cp.len) {
      if (cp.fifo_level >= 4) cap_flush(cp);
      if (cp.cnt & 0x04) { cp.cnt &= 0x7F; return; }   // one-shot
      cp.pos = 0;
    }
  }
}

// ---- mixer ------------------------------------------------------------------

// Rescheduled from the nominal time, not now(), so CPU overshoot doesn't
// accumulate into a slow sample clock.
void Spu::ev_mix(NDS& nds, u32) {
  Spu& s = nds.spu;
  s.run_to(nds.sched.event_time());
  // Capture writes ARM7 RAM, so the batch drops to one sample while one runs.
  const u32 n = ((s.cap_[0].cnt | s.cap_[1].cnt) & 0x80) ? 1 : s.batch_;
  nds.sched.schedule(EventId::Spu, s.mix_at_ + (n - 1) * s.mix_period_, ev_mix);
}

void Spu::catch_up() { run_to(nds_.sched.now()); }

void Spu::write_sndexcnt(u16 value, u16 mask) {
  u16& cur = nds_.io.dsi.sndexcnt;
  value = static_cast<u16>((value & mask) | (cur & ~mask));
  if (cur & 0x8000) value = static_cast<u16>((value & ~0x2000) | (cur & 0x2000));   // I2S freq locked while enabled
  if ((cur ^ value) & 0x2000) {
    catch_up();
    mix_period_ = (value & 0x2000) ? MIX_PERIOD_47K : MIX_PERIOD;
    timer_step_ = mix_period_ / 4;
    if (dbg_) std::fprintf(stderr, "[spu] SNDEXCNT: output %.1f Hz\n", output_rate_hz());
  }
  catch_up();
  cur = value & 0xE00F;   // bits 15, 14 and 0-3 are applied per sample in mix()
}

void Spu::push(s16 l, s16 r) {
  ring_[wr_ * 2] = l; ring_[wr_ * 2 + 1] = r;
  wr_ = (wr_ + 1) & (RING_FRAMES - 1);
  if (wr_ == rd_) { rd_ = (rd_ + 1) & (RING_FRAMES - 1); ++overruns_; }   // overwrite the oldest
}

size_t Spu::take(s16* dst, size_t max_frames) {
  size_t n = 0;
  while (n < max_frames && rd_ != wr_) {
    dst[n * 2] = ring_[rd_ * 2]; dst[n * 2 + 1] = ring_[rd_ * 2 + 1];
    rd_ = (rd_ + 1) & (RING_FRAMES - 1); ++n;
  }
  return n;
}

void Spu::mix() {
  DS_PROF(SPU);
  s32 left = 0, right = 0, out_l = 0, out_r = 0;
  if (cnt_ & 0x8000) {
    auto pan_out = [&](const Channel& c, s32 v) {
      left  += static_cast<s32>((static_cast<s64>(v) * (128 - c.pan)) >> 10);
      right += static_cast<s32>((static_cast<s64>(v) * c.pan) >> 10);
    };
    const s32 ch0 = run_channel(ch_[0], timer_step_), ch1 = run_channel(ch_[1], timer_step_);
    const s32 ch2 = run_channel(ch_[2], timer_step_), ch3 = run_channel(ch_[3], timer_step_);
    pan_out(ch_[0], ch0); pan_out(ch_[2], ch2);
    if (!(cnt_ & 0x1000)) pan_out(ch_[1], ch1);     // bit 12/13: channel 1/3 bypass the mixer
    if (!(cnt_ & 0x2000)) pan_out(ch_[3], ch3);
    // Skip disabled channels before the call: run_channel(off) is exactly 0 anyway.
    for (int i = 4; i < 16; ++i)
      if (ch_[i].cnt & 0x80000000u) pan_out(ch_[i], run_channel(ch_[i], timer_step_));

    for (int k = 0; k < 2; ++k) {
      if (!(cap_[k].cnt & 0x80)) continue;
      const s32 v = std::clamp((k ? right : left) >> 8, -0x8000, 0x7FFF);
      cap_run(cap_[k], v);
    }

    auto side = [&](u32 sel, s32 mixer, s32 p1, s32 p3) -> s32 {
      switch (sel) {
      case 0:  return mixer;
      case 1:  return static_cast<s32>((static_cast<s64>(ch1) * p1) >> 10);
      case 2:  return static_cast<s32>((static_cast<s64>(ch3) * p3) >> 10);
      default: return static_cast<s32>((static_cast<s64>(ch1) * p1) >> 10) + static_cast<s32>((static_cast<s64>(ch3) * p3) >> 10);
      }
    };
    out_l = side((cnt_ >> 8) & 3, left, 128 - ch_[1].pan, 128 - ch_[3].pan);
    out_r = side((cnt_ >> 10) & 3, right, ch_[1].pan, ch_[3].pan);
  }
  out_l = static_cast<s32>((static_cast<s64>(out_l) * master_) >> 7) >> 8;
  out_r = static_cast<s32>((static_cast<s64>(out_r) * master_) >> 7) >> 8;
  // SOUNDBIAS centres the 10-bit DAC; commercial games use 0x200, so the bias
  // is applied relative to it and the output stays centred on zero.
  if (apply_bias_) { out_l += (bias_ << 6) - 0x8000; out_r += (bias_ << 6) - 0x8000; }
  // On the DSi, POWCNT2 bit 0 only silences the NITRO mixer.
  s16 l = muted_ ? 0 : static_cast<s16>(std::clamp(out_l, -0x8000, 0x7FFF));
  s16 r = muted_ ? 0 : static_cast<s16>(std::clamp(out_r, -0x8000, 0x7FFF));
  if (nds_.dsi) {
    // Nothing reaches output/mic while SNDEXCNT disables I2S; otherwise each
    // output sample is a mic sample clock, mixing NITRO with (silent) DSP.
    const u16 sx = nds_.io.dsi.sndexcnt;
    if (!(sx & 0x8000)) {
      l = r = 0;
    } else {
      nds_.io.dsi_mic_clock(nds_.io.mic_at(mix_at_));
      if (sx & 0x4000) {
        l = r = 0;
      } else {
        const int fn = std::min(sx & 0xF, 8);
        l = static_cast<s16>((l * fn) >> 3);
        r = static_cast<s16>((r * fn) >> 3);
      }
    }
  }
  push(l, r);
}


template <class S> void Spu::sync_state(S& s) {
  s.begin("SPU ");
  for (Channel& c : ch_)
    s.fields(c.cnt, c.src, c.loop, c.len, c.timer_reload, c.timer, c.pos, c.cur, c.noise, c.volume, c.vol_shift, c.pan, c.key_on,
             c.adpcm_val, c.adpcm_idx, c.adpcm_val_loop, c.adpcm_idx_loop, c.adpcm_byte, c.fifo, c.fifo_rd, c.fifo_wr, c.fifo_off, c.fifo_level);
  for (Capture& cp : cap_)
    s.fields(cp.cnt, cp.dst, cp.len, cp.timer_reload, cp.timer, cp.pos, cp.fifo, cp.fifo_rd, cp.fifo_wr, cp.fifo_off, cp.fifo_level);
  s.fields(cnt_, bias_, master_, muted_, mix_at_, batch_);
  s.fields(mix_period_, apply_bias_);   // appended: DS states leave the defaults
  s.end();
  if constexpr (S::reading) timer_step_ = mix_period_ / 4;
  if constexpr (S::reading) { rd_ = wr_ = 0; nds_.sched.rebind(EventId::Spu, ev_mix); }
}
template void Spu::sync_state<state::Writer>(state::Writer&);
template void Spu::sync_state<state::Reader>(state::Reader&);

} // namespace ds::spu
