// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/spu/spu.h"

#include <SDL2/SDL.h>
#include <vector>

namespace ds { struct NDS; }
namespace ds::sdl {

// Audio output. Device opened at its own rate; the SPU stream (32728.5 Hz,
// or 47605.1 on a DSi title) is linearly resampled and queued, not pulled
// from a callback. The frame limiter is the clock; the queue drifts against
// the sound card's own crystal, so a dynamic rate controller nudges the
// resample step each frame to hold queue depth at target. Whole frames are
// dropped instead for what the controller can't answer: fast forward, and a
// device that stops consuming (queue past MAX_FRAMES).
class Audio {
public:
  bool open(bool native_rate = true);
  u32  rate() const { return rate_; }
  void close();
  bool active() const { return dev_ != 0; }

  // Drains the SPU ring into the queue. With `drop`, whole frames are
  // discarded once the queue is at target depth (fast forward), or once it
  // has drifted past MAX_FRAMES regardless.
  void push(NDS& nds, bool drop = false);
  // Wall-clock rate against the console's own (frame limiter's rate / 59.8261
  // * emu.speed): stretches the resample step so slowed playback drops in
  // pitch like a slowed console. Fast forward instead drops whole frames.
  void set_speed(double factor);
  // A DS frame of audio, ms (console's 59.8261 Hz), the unit queue depth is measured in.
  static constexpr double FRAME_MS = 1e3 * CYCLES_PER_FRAME / ARM9_CLOCK_HZ;
  static constexpr double DEFAULT_MS = 3 * FRAME_MS;   // [audio] latency_frames = 3, the old default
  // Wider than the menu's range. Floor isn't arbitrary: the device drains in
  // whole periods of its own, so a target under one is unreachable.
  static constexpr double MIN_MS = 10.0, MAX_MS = 400.0;

  // Target queue depth in ms: output latency and slack before it runs dry.
  // [audio] buffer_size. Milliseconds, not frames, since the device's own
  // period (a sample count) only compares to it in time.
  void set_buffer_ms(double ms);
  double buffer_ms() const { return target_frames_ * FRAME_MS; }
  double target_frames() const { return target_frames_; }

  // [audio] buffer_size = auto: grows the target only while the machine keeps
  // up and still runs dry (an isolated hitch, which depth can fix). Behind
  // real time it leaves the target alone -- that's a frameskip
  // problem, and a deeper queue would just detune the output.
  void set_buffer_auto();
  bool buffer_auto() const { return auto_; }
  void auto_tick(bool on_time);   // once a frame, with whether it met budget
  // Controller's current correction, ppm (positive: playing faster to shed a deep queue).
  double rate_trim_ppm() const { return trim_ * 1e6; }
  // The device's actual period (a daemon-backed device may not honor the ask);
  // the queue target must clear it or the controller can never hold it.
  u32    device_samples() const { return dev_samples_; }
  double device_buffer_frames() const;   // that chunk, in frames of audio
  double input_rate() const { return in_rate_; }   // the SPU's, live (DSi: 47605.1)

  // Output statistics since the last reset, gathered where already measured.
  // `dry`: the queue was empty when measured -- the closest thing to an
  // underrun signal available (a queued device has no callback to catch one).
  struct Stats {
    u64    frames = 0;        // frames the queue was measured on
    u64    dry = 0;           // ... and found empty
    u64    under_half = 0;    // ... and found under half a frame
    u64    under_one = 0;
    u64    dropped = 0;       // frames sent to drain() instead of the device
    double min_depth = 1e9;   // shallowest measurement, in frames
    double max_depth = 0.0;
  };
  const Stats& stats() const { return stats_; }
  void reset_stats() { stats_ = Stats{}; }

  void set_volume(int percent);   // 0..100
  int  volume() const { return volume_; }
  void set_muted(bool m) { muted_ = m; }
  bool muted() const { return muted_; }
  void pause(bool p);       // stop the device and drop what is queued
  void clear() { if (dev_) SDL_ClearQueuedAudio(dev_); depth_ = -1.0; trim_ = 0.0; }

  // Microphone: default capture device at the SPU rate, mono. Opened
  // separately so --no-audio still records. capture() returns everything
  // since the last call; a backlog is dropped rather than queued.
  bool open_capture();
  const std::vector<s16>& capture();
  bool capturing() const { return cap_ != 0; }
  double queued_frames() const;   // how much audio is buffered, in frames

private:
  // Past this the queue isn't drifting, it's not being consumed (dead daemon,
  // lost device): the controller can't fix that, so whole frames go instead.
  static constexpr int MAX_FRAMES = 8;
  // Proportional controller on depth error: 1 frame of error asks for 0.2%,
  // closing in ~8s; clamp is what a DS mix can take unheard (0.5% ~8 cents).
  static constexpr double DRC_GAIN = 0.002;      // per frame of depth error
  static constexpr double DRC_CLAMP = 0.005;     // +/- 0.5 %
  static constexpr double DRC_SMOOTH = 1.0 / 32.0;   // depth EMA, ~0.5 s
  SDL_AudioDeviceID dev_ = 0, cap_ = 0;
  std::vector<s16> mic_;
  u32 frame_bytes_ = 0;
  u32 dev_samples_ = 0;     // got.samples: the device's own period
  Stats stats_;
  u32 rate_ = spu::Spu::SAMPLE_RATE;     // the device's rate
  s16 prev_l_ = 0, prev_r_ = 0;   // resampler state: previous input frame
  u32 phase_ = 0;                // output phase within the input step, 16.16
  std::vector<s16> out_;
  bool over_ = false;    // queue not being consumed; dropping until back at target
  void apply_target_ms(double ms);
  // Raises the queue by inserting silence at a moment it's already dry, since
  // the limiter leaves no headroom for the trim alone to fill a raised target.
  void queue_silence(double ms);

  static constexpr double AUTO_STEP_MS = 8.0;
  static constexpr double AUTO_MIN_MS = 30.0, AUTO_MAX_MS = 150.0;
  static constexpr int AUTO_WINDOW = 300;       // frames per decision, ~5 s
  static constexpr int AUTO_CALM_WINDOWS = 6;   // ~30 s clean before giving latency back
  static constexpr int AUTO_SETTLE = 60;        // frames ignored after a step, and at startup
  // 90%, not 100%: adaptive frameskip/vsync put the odd frame over budget on
  // an otherwise-fine machine, and those are exactly the hitches depth is for.
  static constexpr int AUTO_ON_TIME_PCT = 90;

  double target_frames_ = DEFAULT_MS / FRAME_MS;   // fractional: ms target doesn't land on whole frames
  bool auto_ = false;
  bool dbg_auto_ = false;   // DS_AUDIO_AUTO: log every decision
  int auto_frames_ = 0, auto_on_time_ = 0, auto_calm_ = 0, auto_settle_ = AUTO_SETTLE;
  u64 auto_dry_mark_ = 0;   // stats_.dry when this window opened
  double speed_ = 1.0;   // wall-clock rate against the console's own
  double depth_ = -1.0;  // smoothed queue depth in frames; < 0 until the first measurement
  double trim_ = 0.0;    // controller's correction, as a fraction of the nominal rate
  double in_rate_ = 0.0;   // SPU's real rate (32728.5 on DS; not integral), 0 until first push
  int  volume_ = 100;
  bool muted_ = false;
};

} // namespace ds::sdl
