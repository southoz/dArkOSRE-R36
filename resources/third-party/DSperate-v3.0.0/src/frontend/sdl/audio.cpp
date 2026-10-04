// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio.h"
#include "core/nds.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ds::sdl {

bool Audio::open(bool native_rate) {
  dbg_auto_ = std::getenv("DS_AUDIO_AUTO") != nullptr;   // log every buffer-size decision
  SDL_AudioSpec want{}, got{};
  want.freq = static_cast<int>(spu::Spu::SAMPLE_RATE);
  if (native_rate) {
    // Ask for the device's own rate explicitly: a daemon-backed device
    // accepts and converts any rate, so "allow a change" alone changes
    // nothing. 48 kHz is the daemon default before SDL 2.24's query exists.
    int freq = 48000;
#if SDL_VERSION_ATLEAST(2, 24, 0)
    // Only daemon backends implement the query; some ALSA backends crash in it.
    const char* drv = SDL_GetCurrentAudioDriver();
    if (drv && (std::strcmp(drv, "pipewire") == 0 || std::strcmp(drv, "pulseaudio") == 0)) {
      SDL_AudioSpec def{};
      if (SDL_GetDefaultAudioInfo(nullptr, &def, 0) == 0 && def.freq > 0) freq = def.freq;
    }
#endif
    want.freq = freq;
  }
  want.format = AUDIO_S16SYS;
  want.channels = 2;
  want.samples = 2048;      // queue granularity only (no callback); fewer, larger device writes
  want.callback = nullptr;
  dev_ = SDL_OpenAudioDevice(nullptr, 0, &want, &got, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
  if (!dev_) { std::fprintf(stderr, "audio: %s (continuing without sound)\n", SDL_GetError()); return false; }
  rate_ = got.freq > 0 ? static_cast<u32>(got.freq) : spu::Spu::SAMPLE_RATE;
  dev_samples_ = got.samples;
  frame_bytes_ = static_cast<u32>(static_cast<u64>(rate_) * 4 * CYCLES_PER_FRAME / ARM9_CLOCK_HZ);
  prev_l_ = prev_r_ = 0; phase_ = 0;
  stats_ = Stats{};
  SDL_PauseAudioDevice(dev_, 0);
  std::fprintf(stderr, "audio: %s driver, %d Hz, %d channels, %u-sample buffer (%.2f frames)\n",
               SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?",
               got.freq, got.channels, got.samples, device_buffer_frames());
  return true;
}

void Audio::close() {
  if (dev_) { SDL_CloseAudioDevice(dev_); dev_ = 0; }
  if (cap_) { SDL_CloseAudioDevice(cap_); cap_ = 0; }
}

bool Audio::open_capture() {
  if (SDL_GetNumAudioDevices(1) <= 0) { std::fprintf(stderr, "mic: no capture device\n"); return false; }
  SDL_AudioSpec want{}, got{};
  want.freq = static_cast<int>(spu::Spu::SAMPLE_RATE);
  want.format = AUDIO_S16SYS;
  want.channels = 1;
  want.samples = 512;
  cap_ = SDL_OpenAudioDevice(nullptr, 1, &want, &got, 0);
  if (!cap_) { std::fprintf(stderr, "mic: %s (no microphone)\n", SDL_GetError()); return false; }
  SDL_PauseAudioDevice(cap_, 0);
  std::fprintf(stderr, "mic: %s\n", SDL_GetAudioDeviceName(0, 1) ? SDL_GetAudioDeviceName(0, 1) : "default");
  return true;
}

const std::vector<s16>& Audio::capture() {
  mic_.clear();
  if (!cap_) return mic_;
  const u32 per_frame = spu::Spu::SAMPLE_RATE / 60 + 1;
  u32 avail = SDL_GetQueuedAudioSize(cap_) / 2;
  if (avail > per_frame * 4) {                   // stale backlog: keep the newest
    std::vector<s16> junk(avail - per_frame * 2);
    SDL_DequeueAudio(cap_, junk.data(), static_cast<u32>(junk.size() * 2));
    avail = per_frame * 2;
  }
  mic_.resize(avail);
  const u32 got = SDL_DequeueAudio(cap_, mic_.data(), avail * 2) / 2;
  mic_.resize(got);
  return mic_;
}

void Audio::push(NDS& nds, bool drop) {
  in_rate_ = nds.spu.output_rate_hz();   // live: a DSi title can move it to 47.6 kHz via SNDEXCNT

  // Measure before queueing: depth is what's left to play, not plus this frame.
  if (dev_) {
    const u32 queued = SDL_GetQueuedAudioSize(dev_);
    const double now = static_cast<double>(queued) / frame_bytes_;
    depth_ = depth_ < 0 ? now : depth_ + (now - depth_) * DRC_SMOOTH;
    if (now > target_frames_ + MAX_FRAMES) over_ = true;
    else if (now <= target_frames_) over_ = false;
    ++stats_.frames;
    if (queued == 0) ++stats_.dry;
    if (now < 0.5) ++stats_.under_half;
    if (now < 1.0) ++stats_.under_one;
    if (now < stats_.min_depth) stats_.min_depth = now;
    if (now > stats_.max_depth) stats_.max_depth = now;
  }

  if (over_ || (drop && (!dev_ || SDL_GetQueuedAudioSize(dev_) > frame_bytes_ * target_frames_))) {
    ++stats_.dropped;
    nds.spu.drain();
    return;
  }

  // Steer the queue back to the target. Positive trim plays out faster than
  // nominal, which drains a deep queue; negative fills a shallow one.
  if (dev_ && depth_ >= 0) {
    const double err = depth_ - target_frames_;
    trim_ = err * DRC_GAIN;
    if (trim_ > DRC_CLAMP) trim_ = DRC_CLAMP;
    else if (trim_ < -DRC_CLAMP) trim_ = -DRC_CLAMP;
  }

  s16 buf[2048 * 2];
  size_t n;
  while ((n = nds.spu.take(buf, 2048)) != 0) {
    if (!dev_) continue;
    // Always resampled, even at matching rates, since the trim is applied
    // here. Step is input frames per output frame, 16.16.
    const double ratio = in_rate_ / rate_ * speed_ * (1.0 + trim_);
    const u32 step = static_cast<u32>(ratio * 65536.0 + 0.5);
    out_.resize((static_cast<size_t>(n / ratio) + 2) * 2);
    size_t m = 0;
    for (size_t i = 0; i < n; ++i) {
      const s16 cl = buf[i * 2], cr = buf[i * 2 + 1];
      while (phase_ < 0x10000) {
        const u32 f = phase_;
        if ((m + 1) * 2 > out_.size()) out_.resize(out_.size() * 2);
        out_[m * 2]     = static_cast<s16>(prev_l_ + (((cl - prev_l_) * static_cast<s32>(f)) >> 16));
        out_[m * 2 + 1] = static_cast<s16>(prev_r_ + (((cr - prev_r_) * static_cast<s32>(f)) >> 16));
        ++m;
        phase_ += step;
      }
      phase_ -= 0x10000;
      prev_l_ = cl; prev_r_ = cr;
    }
    s16* out = out_.data();
    if (muted_) std::memset(out, 0, m * 4);
    else if (volume_ != 100) {
      const int g = volume_ * 256 / 100;
      for (size_t i = 0; i < m * 2; ++i) out[i] = static_cast<s16>((out[i] * g) >> 8);
    }
    SDL_QueueAudio(dev_, out, static_cast<u32>(m * 4));
  }
}

void Audio::set_speed(double factor) {
  speed_ = factor > 0.05 ? (factor < 20.0 ? factor : 20.0) : 0.05;
}

void Audio::apply_target_ms(double ms) {
  target_frames_ = std::clamp(ms, MIN_MS, MAX_MS) / FRAME_MS;
  depth_ = -1.0;      // target moved: measure again rather than chase the old error
  trim_ = 0.0;
}

void Audio::set_buffer_ms(double ms) {
  if (!(ms > 0.0)) ms = DEFAULT_MS;      // unset, unparseable, or a NaN out of atof
  auto_ = false;
  apply_target_ms(ms);
}

void Audio::set_buffer_auto() {
  auto_ = true;
  auto_frames_ = auto_on_time_ = auto_calm_ = 0;
  auto_settle_ = AUTO_SETTLE;
  auto_dry_mark_ = stats_.dry;
  apply_target_ms(DEFAULT_MS);
}

void Audio::queue_silence(double ms) {
  if (!dev_ || ms <= 0.0) return;
  const size_t frames = static_cast<size_t>(ms / 1000.0 * rate_);
  if (!frames) return;
  const std::vector<s16> zero(frames * 2, 0);
  SDL_QueueAudio(dev_, zero.data(), static_cast<u32>(frames * 4));
}

void Audio::auto_tick(bool on_time) {
  if (!auto_ || !dev_) return;
  // Ignore right after a step, and at startup (queue filling from empty
  // would otherwise read as underruns and grow the target needlessly).
  if (auto_settle_ > 0) { --auto_settle_; auto_dry_mark_ = stats_.dry; return; }

  ++auto_frames_;
  if (on_time) ++auto_on_time_;
  if (auto_frames_ < AUTO_WINDOW) return;

  const u64 dry = stats_.dry - auto_dry_mark_;
  const bool keeping_up = auto_on_time_ * 100 >= auto_frames_ * AUTO_ON_TIME_PCT;
  const double now = buffer_ms();

  if (dbg_auto_)
    std::fprintf(stderr, "[audio] auto: %.0f ms, %llu dry, %d/%d frames on time -> %s\n", now,
                 static_cast<unsigned long long>(dry), auto_on_time_, auto_frames_,
                 !keeping_up ? "behind, leaving it" : dry ? "grow" : "clean");
  if (!keeping_up) {
    auto_calm_ = 0;
  } else if (dry) {
    if (now < AUTO_MAX_MS) {
      const double want = std::min(now + AUTO_STEP_MS, AUTO_MAX_MS);
      queue_silence(want - now);
      apply_target_ms(want);
      auto_settle_ = AUTO_SETTLE;
    }
    auto_calm_ = 0;
  } else if (++auto_calm_ >= AUTO_CALM_WINDOWS) {
    if (now > AUTO_MIN_MS) { apply_target_ms(now - AUTO_STEP_MS); auto_settle_ = AUTO_SETTLE; }
    auto_calm_ = 0;
  }
  auto_frames_ = auto_on_time_ = 0;
  auto_dry_mark_ = stats_.dry;
}

void Audio::set_volume(int percent) {
  volume_ = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
}

void Audio::pause(bool p) {
  if (!dev_) return;
  SDL_PauseAudioDevice(dev_, p ? 1 : 0);
  if (p) { SDL_ClearQueuedAudio(dev_); over_ = false; depth_ = -1.0; trim_ = 0.0; }
}

double Audio::queued_frames() const {
  return dev_ ? static_cast<double>(SDL_GetQueuedAudioSize(dev_)) / frame_bytes_ : 0.0;
}

double Audio::device_buffer_frames() const {
  return frame_bytes_ ? static_cast<double>(dev_samples_) * 4 / frame_bytes_ : 0.0;
}

} // namespace ds::sdl
