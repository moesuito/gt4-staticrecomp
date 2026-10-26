#define _CRT_SECURE_NO_WARNINGS
#include "render_skip.hpp"
#include "fps_probe.hpp"
#ifdef GT4_PERF_PROBE
#include "perf_probe.hpp"
#endif

#include <cstdlib>
#include <algorithm>

namespace gt4::render_skip {

SkipManager &SkipManager::shared() noexcept {
  static SkipManager instance;
  return instance;
}

SkipManager::SkipManager() noexcept {
  init();
}

void SkipManager::init(const char *env_fps, const char *env_force) noexcept {
  reset();

  const char *fps_val = env_fps ? env_fps : std::getenv("GT4_NATIVE_FPS");
  enabled_ = (fps_val != nullptr && std::strcmp(fps_val, "skip") == 0);

  const char *force_val = env_force ? env_force : std::getenv("GT4_NATIVE_FPS_FORCE");
  if (force_val != nullptr) {
    if (std::strcmp(force_val, "2") == 0) {
      force_pattern_ = 2;
    } else if (std::strcmp(force_val, "3") == 0) {
      force_pattern_ = 3;
    } else {
      force_pattern_ = 0;
    }
  } else {
    force_pattern_ = 0;
  }

#ifdef GT4_PERF_PROBE
  if (enabled_) {
    gt4::perf::Probe::shared().set_report_section([](std::FILE *output) {
      if (gt4::fps_probe::Probe::shared().enabled()) {
        gt4::fps_probe::Probe::shared().format_report(output);
      }
      if (gt4::render_skip::SkipManager::shared().enabled()) {
        gt4::render_skip::SkipManager::shared().format_report(output);
      }
    });
  }
#endif
}

void SkipManager::reset() noexcept {
  enabled_ = false;
  force_pattern_ = 0;
  force_counter_ = 0;

  gate_open_ = false;
  good_frames_ = 0;
  prev_clock_f898_ = 0;
  has_prev_clock_ = false;
  spec2_logged_ = false;

  active_race_object_ = 0;
  seen_render_ = false;
  render_evaluated_this_frame_ = false;
  skipped_this_frame_ = false;
  need_clear_flares_ = false;
  prev_frame_skipped_ = false;
  current_skip_run_ = 0;

  debt_us_ = 0;
  last_wall_ticks_ = 0;
  has_last_wall_ = false;

  stats_ = SkipStats{};
}

void SkipManager::evaluate_gate(const std::uint8_t *ram, std::size_t ram_size) noexcept {
  // 1. Target check: v2.00 only; Spec II is unsupported/disabled
  if (gt4::target::active().id != gt4::target::Id::v200) {
    if (!spec2_logged_) {
      std::fprintf(stderr, "[render-skip] Spec II target is unsupported/disabled; render skipping inactive\n");
      spec2_logged_ = true;
    }
    close_gate();
    return;
  }

  // 2. Race object check
  if (!is_race_object(active_race_object_, ram, ram_size)) {
    close_gate();
    return;
  }

  // 3. Clock check: DynamicsConductor at race_obj + 0x70, clock_f898 must rise by exactly 50
  const std::uint32_t dyn_ptr = read_u32(ram, ram_size, active_race_object_ + 0x70u);
  if (dyn_ptr == 0 || dyn_ptr + 0x10000u > ram_size) {
    close_gate();
    return;
  }

  const std::uint32_t curr_clock = read_u32(ram, ram_size, dyn_ptr + 0xF898u);
  const bool clock_rose_50 = (curr_clock > 0 && has_prev_clock_ && (curr_clock == prev_clock_f898_ + 50u));
  prev_clock_f898_ = curr_clock;
  has_prev_clock_ = true;

  if (clock_rose_50) {
    ++good_frames_;
    if (good_frames_ >= 60u) {
      gate_open_ = true;
    }
  } else {
    close_gate();
  }
}

void SkipManager::close_gate() noexcept {
  if (gate_open_) {
    ++stats_.gate_closures;
    debt_us_ = 0;
    prev_frame_skipped_ = false;
  }
  gate_open_ = false;
  good_frames_ = 0;
}

bool SkipManager::should_skip_render() noexcept {
  if (force_pattern_ == 2) {
    // 1 render, 1 skip (30 FPS presented)
    const bool skip = (force_counter_ % 2 == 1);
    ++force_counter_;
    return skip;
  }
  if (force_pattern_ == 3) {
    // 1 render, 2 skips (20 FPS presented)
    const bool skip = (force_counter_ % 3 != 0);
    ++force_counter_;
    return skip;
  }

  // Timing policy: skip if debt accumulated and previous frame was not skipped
  // Floor is 30 rendered fps (never skip two renders in a row)
  const bool skip = (!prev_frame_skipped_ && debt_us_ > 0);
  prev_frame_skipped_ = skip;
  return skip;
}

void SkipManager::clear_flare_queues(std::uint8_t *ram, std::size_t ram_size) noexcept {
  if (ram == nullptr || active_race_object_ == 0) return;
  const std::uint32_t flare_base = active_race_object_ + 0x0DA0u;
  const std::uint32_t track_count_addr = flare_base + 0x1848u;
  const std::uint32_t car_count_addr = flare_base + 0x0A34u;

  if (track_count_addr + 4u <= ram_size && car_count_addr + 4u <= ram_size) {
    const std::uint32_t zero = 0;
    std::memcpy(ram + track_count_addr, &zero, sizeof zero);
    std::memcpy(ram + car_count_addr, &zero, sizeof zero);
  }
}

void SkipManager::sample_flare_queue_counts(const std::uint8_t *ram, std::size_t ram_size) noexcept {
  if (ram == nullptr || active_race_object_ == 0) return;
  const std::uint32_t flare_base = active_race_object_ + 0x0DA0u;
  const std::uint32_t track_count_addr = flare_base + 0x1848u;
  const std::uint32_t car_count_addr = flare_base + 0x0A34u;

  std::uint32_t track_count = 0;
  std::uint32_t car_count = 0;
  if (track_count_addr + 4u <= ram_size) {
    std::memcpy(&track_count, ram + track_count_addr, sizeof track_count);
  }
  if (car_count_addr + 4u <= ram_size) {
    std::memcpy(&car_count, ram + car_count_addr, sizeof car_count);
  }

  if (track_count > stats_.max_flare_queue_count) stats_.max_flare_queue_count = track_count;
  if (car_count > stats_.max_flare_queue_count) stats_.max_flare_queue_count = car_count;
}

void SkipManager::on_frame_boundary() noexcept {
  // Reset per-frame skip latch at boundary
}

void SkipManager::record_frame_wall(std::int64_t ticks, std::uint64_t frequency) noexcept {
  if (!enabled_ || frequency == 0) return;

  if (has_last_wall_) {
    const std::int64_t delta_ticks = ticks - last_wall_ticks_;
    const std::int64_t frame_wall_us = (delta_ticks * 1000000) / static_cast<std::int64_t>(frequency);

    constexpr std::int64_t kBudgetUs = 16683; // 16.68 ms budget
    constexpr std::int64_t kMaxDebtUs = 33366; // 33.37 ms hard cap (2 frames)

    if (frame_wall_us > kBudgetUs) {
      debt_us_ += (frame_wall_us - kBudgetUs);
      if (debt_us_ > kMaxDebtUs) {
        debt_us_ = kMaxDebtUs;
      }
    } else {
      const std::int64_t saved = kBudgetUs - frame_wall_us;
      debt_us_ = (debt_us_ > saved) ? (debt_us_ - saved) : 0;
    }
  }

  last_wall_ticks_ = ticks;
  has_last_wall_ = true;
}

void SkipManager::format_report(std::FILE *stream) const noexcept {
  if (!stream) return;
  std::fprintf(stream,
               "[render-skip] counters: rendered=%llu skipped=%llu gate_closures=%llu longest_skip_run=%llu\n",
               static_cast<unsigned long long>(stats_.frames_rendered),
               static_cast<unsigned long long>(stats_.frames_skipped),
               static_cast<unsigned long long>(stats_.gate_closures),
               static_cast<unsigned long long>(stats_.longest_skip_run));
  std::fprintf(stream,
               "[render-skip] max flare count seen at a skip: %llu\n",
               static_cast<unsigned long long>(stats_.max_flare_queue_count));
}

void SkipManager::finish(std::FILE *stream) const noexcept {
  if (enabled_) {
    format_report(stream);
  }
}

} // namespace gt4::render_skip
