#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "game_target.hpp"

namespace gt4::render_skip {

inline std::uint32_t read_u32(const std::uint8_t *ram, std::size_t ram_size, std::uint32_t addr) noexcept {
  if (addr + 4u > ram_size || ram == nullptr) return 0;
  std::uint32_t v = 0;
  std::memcpy(&v, ram + addr, sizeof v);
  return v;
}

inline bool is_race_object(std::uint32_t obj_ptr, const std::uint8_t *ram, std::size_t ram_size) noexcept {
  if (obj_ptr == 0 || obj_ptr >= ram_size || ram == nullptr) return false;
  if (obj_ptr >= 0x00600000u && obj_ptr <= 0x00780000u) return false;

  const std::uint32_t vptr = read_u32(ram, ram_size, obj_ptr + 0x64u);
  if (vptr >= 0x00676000u && vptr <= 0x00686000u) {
    const std::uint32_t dyn = read_u32(ram, ram_size, obj_ptr + 0x70u);
    if (dyn != 0 && dyn < ram_size && dyn != obj_ptr) {
      return true;
    }
  }
  return false;
}

struct SkipStats {
  std::uint64_t frames_rendered = 0;
  std::uint64_t frames_skipped = 0;
  std::uint64_t gate_closures = 0;
  std::uint64_t longest_skip_run = 0;
  std::uint32_t max_flare_queue_count = 0;
};

class SkipManager {
public:
  static SkipManager &shared() noexcept;

  SkipManager() noexcept;

  void init(const char *env_fps = nullptr, const char *env_force = nullptr) noexcept;
  void reset() noexcept;

  bool enabled() const noexcept { return enabled_; }
  std::uint32_t force_pattern() const noexcept { return force_pattern_; }
  bool gate_open() const noexcept { return gate_open_; }
  bool skipped_this_frame() const noexcept { return skipped_this_frame_; }
  std::uint32_t active_race_object() const noexcept { return active_race_object_; }
  const SkipStats &stats() const noexcept { return stats_; }

  template <typename CpuType>
  bool handle_dispatch(CpuType &c, std::uint32_t pc, std::uint8_t *ram, std::size_t ram_size) noexcept;

  void on_frame_boundary() noexcept;
  void record_frame_wall(std::int64_t ticks, std::uint64_t frequency) noexcept;
  void format_report(std::FILE *stream) const noexcept;
  void finish(std::FILE *stream = stderr) const noexcept;

  // Test accessors and overrides
  void set_debt_us(std::int64_t debt) noexcept { debt_us_ = debt; }
  std::int64_t debt_us() const noexcept { return debt_us_; }
  std::uint32_t good_frames() const noexcept { return good_frames_; }
  void set_gate_open_for_test(bool open) noexcept { gate_open_ = open; }
  void set_active_race_object_for_test(std::uint32_t obj) noexcept { active_race_object_ = obj; }

  void evaluate_gate(const std::uint8_t *ram, std::size_t ram_size) noexcept;
  void close_gate() noexcept;
  bool should_skip_render() noexcept;
  void clear_flare_queues(std::uint8_t *ram, std::size_t ram_size) noexcept;
  void sample_flare_queue_counts(const std::uint8_t *ram, std::size_t ram_size) noexcept;

private:
  bool enabled_ = false;
  std::uint32_t force_pattern_ = 0; // 0 = debt policy, 2 = 1 render / 1 skip, 3 = 1 render / 2 skip
  std::uint32_t force_counter_ = 0;

  bool gate_open_ = false;
  std::uint32_t good_frames_ = 0;
  std::uint32_t prev_clock_f898_ = 0;
  bool has_prev_clock_ = false;
  bool spec2_logged_ = false;

  std::uint32_t active_race_object_ = 0;
  bool seen_render_ = false;
  bool render_evaluated_this_frame_ = false;
  bool skipped_this_frame_ = false;
  bool need_clear_flares_ = false;
  bool prev_frame_skipped_ = false;
  std::uint64_t current_skip_run_ = 0;

  // Timing policy
  std::int64_t debt_us_ = 0;
  std::int64_t last_wall_ticks_ = 0;
  bool has_last_wall_ = false;

  SkipStats stats_{};
};

template <typename CpuType>
inline bool SkipManager::handle_dispatch(CpuType &c, std::uint32_t pc, std::uint8_t *ram, std::size_t ram_size) noexcept {
  const auto &hooks = ::gt4::target::active().hooks;

  // 1. Update functor: write 0 to flare counts before update runs if previous frame skipped
  if (hooks.main_update_functor && pc == hooks.main_update_functor) {
    if (seen_render_ || need_clear_flares_) {
      // Transition from render/idle phase to next frame's update phase
      seen_render_ = false;
      render_evaluated_this_frame_ = false;
      if (need_clear_flares_) {
        clear_flare_queues(ram, ram_size);
        need_clear_flares_ = false;
      }
    }
    const std::uint32_t obj = c.r[4].w[0];
    if (obj != 0 && is_race_object(obj, ram, ram_size)) {
      active_race_object_ = obj;
    }
    return false; // Update functor always runs
  }

  // 2. Render functor: gate check and skip decision
  if (hooks.main_render_functor && pc == hooks.main_render_functor) {
    const std::uint32_t obj = c.r[4].w[0];
    if (obj != 0 && is_race_object(obj, ram, ram_size)) {
      active_race_object_ = obj;
    }

    std::uint32_t curr_clock = 0;
    if (active_race_object_ != 0) {
      const std::uint32_t dyn_ptr = read_u32(ram, ram_size, active_race_object_ + 0x70u);
      if (dyn_ptr != 0 && dyn_ptr + 0x10000u <= ram_size) {
        curr_clock = read_u32(ram, ram_size, dyn_ptr + 0xF898u);
      }
    }

    // Intra-frame repeated render call check:
    // If we already evaluated this frame and guest clock has not advanced (or in menus),
    // reuse the skip decision for this frame without re-evaluating or re-counting.
    if (render_evaluated_this_frame_ && (curr_clock == prev_clock_f898_ || active_race_object_ == 0)) {
      return skipped_this_frame_;
    }

    // First render call in this frame:
    render_evaluated_this_frame_ = true;
    seen_render_ = true;
    evaluate_gate(ram, ram_size);

    if (gate_open_ && should_skip_render()) {
      sample_flare_queue_counts(ram, ram_size);
      skipped_this_frame_ = true;
      need_clear_flares_ = true;
      prev_frame_skipped_ = true;
      ++stats_.frames_skipped;
      ++current_skip_run_;
      if (current_skip_run_ > stats_.longest_skip_run) {
        stats_.longest_skip_run = current_skip_run_;
      }
      return true; // Bypass render functor execution on skipped frame
    } else {
      skipped_this_frame_ = false;
      prev_frame_skipped_ = false;
      current_skip_run_ = 0;
      ++stats_.frames_rendered;
      return false; // Render functor executes normally
    }
  }

  // 3. Idle functor: bypass on skipped frames
  if (hooks.main_idle_functor && pc == hooks.main_idle_functor) {
    if (skipped_this_frame_) {
      return true; // Bypass idle functor execution on skipped frame
    }
    return false;
  }

  return false;
}

template <typename CpuType>
inline bool on_dispatch(CpuType &c, std::uint32_t pc, std::uint8_t *ram, std::size_t ram_size) noexcept {
  auto &skip = SkipManager::shared();
  if (!skip.enabled()) return false;
  return skip.handle_dispatch(c, pc, ram, ram_size);
}

} // namespace gt4::render_skip
