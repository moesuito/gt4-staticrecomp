#pragma once
// TASK-074 L2: the Linux sound output, an SDL3 audio stream at 48 kHz stereo, with the two pacing modes of the Windows
// host (host_io.cpp AudioOutput, GT4_PACING):
//   paced (default): audio never blocks the game. The guest's sound goes through gt4::pacing::LinearResampler into a
//     FrameRing; once per VBlank pump() lets the RateController set the ratio (within +-0.5 %) from the whole queue
//     (SDL stream plus ring) and moves 256-frame blocks to SDL while the stream stays within the latency target. The
//     frame pacer (sdl_window.cpp) is the clock.
//   audio: the earlier rule: the guest's sound goes straight to SDL and the VBlank waits while more than 80 ms is
//     queued, so the sound device is the clock.
// GT4_AUDIO_LATENCY_MS (20..500, default 60) sets the paced target, as on Windows.

#include "pacing.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

struct SDL_AudioStream;

namespace gt4::linux_host {

class SdlAudioOutput {
public:
  // Opens the default playback device; silent() is true when none could be opened.
  explicit SdlAudioOutput(gt4::pacing::Mode mode);
  ~SdlAudioOutput();
  SdlAudioOutput(const SdlAudioOutput &) = delete;
  SdlAudioOutput &operator=(const SdlAudioOutput &) = delete;

  bool silent() const { return stream_ == nullptr; }
  gt4::pacing::Mode mode() const { return mode_; }
  // The audio sink (ps2::set_audio_sink): interleaved stereo 16-bit frames.
  void push(const std::int16_t *samples, std::size_t frames);
  // Paced mode, once per VBlank: rate control and moving ring data to SDL. Never waits.
  void pump();
  // Audio mode: waits while more than 80 ms is queued (the device paces the game).
  void wait_for_room();

  std::uint64_t queued_frames() const;
  std::uint64_t underruns() const { return underruns_; }
  std::uint64_t overflow_frames() const { return overflow_frames_; }
  double ratio() const { return resampler_.ratio(); }
  std::size_t target_frames() const { return target_frames_; }

private:
  std::uint64_t stream_frames() const;
  gt4::pacing::Mode mode_;
  SDL_AudioStream *stream_ = nullptr;
  std::size_t target_frames_ = gt4::pacing::target_latency_frames;
  bool started_ = false;
  bool submitted_ = false;
  std::uint64_t underruns_ = 0;
  std::uint64_t overflow_frames_ = 0;
  gt4::pacing::FrameRing ring_{gt4::pacing::paced_ring_frames};
  gt4::pacing::LinearResampler resampler_;
  gt4::pacing::RateController controller_{static_cast<double>(gt4::pacing::target_latency_frames),
                                          1.0 / static_cast<double>(gt4::pacing::guest_vblank_rate)};
  std::vector<std::int16_t> converted_;
  std::vector<std::int16_t> block_;
};

// GT4_AUDIO_LATENCY_MS: 20..500 ms as 48 kHz frames; anything else keeps the 60 ms default (with a message).
std::size_t paced_target_frames(const char *latency_ms);

} // namespace gt4::linux_host
