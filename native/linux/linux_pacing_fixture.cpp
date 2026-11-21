// CTest native_linux_pacing (TASK-074 L2): the Linux frame clock and the paced audio target setting.
//   - FrameClock: nanosecond monotonic ticks; wait_until never returns before its target, and over a 60-frame grid of
//     waits the median lateness is under 1 ms (the final stretch is spun). Run serially; a loaded machine can raise
//     the maximum, so only the median is a pass condition.
//   - paced_target_frames: GT4_AUDIO_LATENCY_MS accepts 20..500 ms, as on Windows; anything else keeps 60 ms.

#include "frame_clock.hpp"
#include "linux/sdl_audio.hpp"
#include "pacing.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const std::string &what) {
  if (!condition) {
    std::fprintf(stderr, "[linux-pacing] FAIL: %s\n", what.c_str());
    ++failures;
  }
}

} // namespace

int main() {
  FrameClock clock;
  expect(clock.frequency() == 1000000000u, "the Linux clock counts nanoseconds");
  expect(clock.high_resolution_timer(), "the Linux clock reports its high-resolution timer");
  const std::int64_t first = clock.now();
  const std::int64_t second = clock.now();
  expect(second >= first, "the clock is monotonic");

  // 60 releases on the real grid, as the host does.
  gt4::pacing::FramePacer pacer(clock.frequency(), gt4::pacing::guest_vblank_rate);
  pacer.anchor(clock.now());
  std::vector<std::int64_t> lateness;
  bool early = false;
  for (int frame = 0; frame < 60; ++frame) {
    const std::int64_t deadline = pacer.deadline();
    clock.wait_until(deadline);
    const std::int64_t released = clock.now();
    early = early || released < deadline;
    lateness.push_back(released - deadline);
    pacer.release(released);
  }
  std::sort(lateness.begin(), lateness.end());
  const double median_us = static_cast<double>(lateness[lateness.size() / 2]) / 1000.0;
  const double maximum_us = static_cast<double>(lateness.back()) / 1000.0;
  std::fprintf(stderr, "[linux-pacing] 60 grid waits: lateness median %.1f us, maximum %.1f us, late frames %llu\n",
               median_us, maximum_us, static_cast<unsigned long long>(pacer.late_frames()));
  expect(!early, "wait_until never returns before its target");
  expect(median_us < 1000.0, "median lateness under 1 ms");

  using gt4::linux_host::paced_target_frames;
  expect(paced_target_frames(nullptr) == gt4::pacing::target_latency_frames, "unset: 60 ms");
  expect(paced_target_frames("") == gt4::pacing::target_latency_frames, "empty: 60 ms");
  expect(paced_target_frames("20") == 960, "20 ms");
  expect(paced_target_frames("500") == 24000, "500 ms");
  expect(paced_target_frames("19") == gt4::pacing::target_latency_frames, "below 20 ms is refused");
  expect(paced_target_frames("501") == gt4::pacing::target_latency_frames, "above 500 ms is refused");
  expect(paced_target_frames("80ms") == gt4::pacing::target_latency_frames, "trailing text is refused");

  if (failures) {
    std::fprintf(stderr, "[linux-pacing] %d failure(s)\n", failures);
    return 1;
  }
  std::fprintf(stderr, "[linux-pacing] PASS\n");
  return 0;
}
