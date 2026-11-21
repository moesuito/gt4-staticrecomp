#include "sdl_audio.hpp"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>

namespace gt4::linux_host {
namespace {

constexpr int kChannels = 2;
constexpr int kBytesPerFrame = kChannels * 2;
// Blocks moved to SDL in paced mode, as on Windows (host_io.cpp paced_frames_per_buffer).
constexpr std::size_t kBlockFrames = 256;
// Audio mode: the VBlank waits past this much queued sound (the donor's rule); sound beyond the drop limit is not queued.
constexpr std::uint64_t kAudioModeAheadFrames = 80 * 48;
constexpr std::uint64_t kAudioModeDropFrames = 500 * 48;

} // namespace

std::size_t paced_target_frames(const char *latency_ms) {
  if (latency_ms == nullptr || *latency_ms == '\0')
    return gt4::pacing::target_latency_frames;
  char *end = nullptr;
  const long milliseconds = std::strtol(latency_ms, &end, 10);
  if (end != latency_ms && *end == '\0' && milliseconds >= 20 && milliseconds <= 500)
    return static_cast<std::size_t>(milliseconds) * 48u;  // 48 frames per millisecond at 48 kHz
  std::fprintf(stderr, "[audio] GT4_AUDIO_LATENCY_MS=%s is not 20..500: the default target is kept\n", latency_ms);
  return gt4::pacing::target_latency_frames;
}

SdlAudioOutput::SdlAudioOutput(gt4::pacing::Mode mode) : mode_(mode) {
  target_frames_ = paced_target_frames(std::getenv("GT4_AUDIO_LATENCY_MS"));
  controller_ = gt4::pacing::RateController{static_cast<double>(target_frames_),
                                            1.0 / static_cast<double>(gt4::pacing::guest_vblank_rate)};
  block_.resize(kBlockFrames * kChannels);
  const SDL_AudioSpec spec{SDL_AUDIO_S16, kChannels, static_cast<int>(gt4::pacing::sample_rate_hz)};
  stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
  if (!stream_) {
    std::fprintf(stderr, "[audio] no audio device (%s)\n", SDL_GetError());
    return;
  }
  SDL_ResumeAudioStreamDevice(stream_);
  std::fprintf(stderr, "[audio] SDL stream 48 kHz stereo, %s mode, target %zu ms\n", gt4::pacing::mode_name(mode_),
               target_frames_ / 48);
}

SdlAudioOutput::~SdlAudioOutput() {
  if (stream_)
    SDL_DestroyAudioStream(stream_);
}

std::uint64_t SdlAudioOutput::stream_frames() const {
  const int bytes = SDL_GetAudioStreamQueued(stream_);
  return bytes > 0 ? static_cast<std::uint64_t>(bytes) / kBytesPerFrame : 0;
}

std::uint64_t SdlAudioOutput::queued_frames() const {
  if (!stream_)
    return 0;
  return stream_frames() + (mode_ == gt4::pacing::Mode::paced ? ring_.size() : 0);
}

void SdlAudioOutput::push(const std::int16_t *samples, std::size_t frames) {
  if (!stream_ || frames == 0)
    return;
  if (mode_ == gt4::pacing::Mode::audio) {
    if (stream_frames() > kAudioModeDropFrames)
      return;
    SDL_PutAudioStreamData(stream_, samples, static_cast<int>(frames * kBytesPerFrame));
    return;
  }
  converted_.clear();
  const std::size_t produced = resampler_.process(samples, frames, converted_);
  const std::size_t accepted = ring_.write(converted_.data(), produced);
  overflow_frames_ += produced - accepted;
}

void SdlAudioOutput::pump() {
  if (!stream_ || mode_ != gt4::pacing::Mode::paced)
    return;
  if (!started_ && ring_.size() >= target_frames_)
    started_ = true;
  if (!started_)
    return;
  // Nothing left in the stream after audio was sent: the device starved.
  if (submitted_ && stream_frames() == 0)
    ++underruns_;
  // The controller sees the whole queue: SDL stream plus ring.
  resampler_.set_ratio(controller_.update(static_cast<double>(queued_frames())));
  // Send only while the stream stays within the target: the excess waits in the ring, where the controller sees it.
  while (ring_.size() >= kBlockFrames && stream_frames() + kBlockFrames <= target_frames_) {
    ring_.read(block_.data(), kBlockFrames);
    SDL_PutAudioStreamData(stream_, block_.data(), static_cast<int>(kBlockFrames * kBytesPerFrame));
    submitted_ = true;
  }
}

void SdlAudioOutput::wait_for_room() {
  if (!stream_ || mode_ != gt4::pacing::Mode::audio)
    return;
  while (stream_frames() > kAudioModeAheadFrames)
    SDL_Delay(1);
}

} // namespace gt4::linux_host
