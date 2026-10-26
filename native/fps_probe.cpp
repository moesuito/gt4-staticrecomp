#define _CRT_SECURE_NO_WARNINGS 1
#include "fps_probe.hpp"
#include "render_skip.hpp"
#ifdef GT4_PERF_PROBE
#include "perf_probe.hpp"
#endif

#include <chrono>
#include <cstdlib>
#include <cstring>

namespace gt4::fps_probe {

namespace {

inline std::uint32_t read_u32(const std::uint8_t *ram, std::size_t ram_size, std::uint32_t addr) noexcept {
  if (addr + 4u > ram_size) return 0;
  std::uint32_t v = 0;
  std::memcpy(&v, ram + addr, sizeof v);
  return v;
}

inline float read_f32(const std::uint8_t *ram, std::size_t ram_size, std::uint32_t addr) noexcept {
  if (addr + 4u > ram_size) return 0.0f;
  float v = 0.0f;
  std::memcpy(&v, ram + addr, sizeof v);
  return v;
}

inline std::uint8_t read_u8(const std::uint8_t *ram, std::size_t ram_size, std::uint32_t addr) noexcept {
  if (addr >= ram_size) return 0;
  return ram[addr];
}

std::uint64_t default_steady_clock_us() noexcept {
  using namespace std::chrono;
  return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

bool is_race_object(std::uint32_t obj_ptr, const std::uint8_t *ram, std::size_t ram_size) noexcept {
  if (obj_ptr == 0 || obj_ptr >= ram_size || ram == nullptr) return false;
  // In game: reject static ELF data (0x00600000 .. 0x00780000: static variables, managers, course tables)
  if (obj_ptr >= 0x00600000u && obj_ptr <= 0x00780000u) return false;

  const std::uint32_t vptr = read_u32(ram, ram_size, obj_ptr + 0x64u);
  // In GT4, all Race* vtables are located between 0x00676000 and 0x00686000
  // (RacePS2Base: 0x00679378, RaceArcadeSingle: 0x0067BAD8, RaceFreeRun: 0x00680140, etc.)
  if (vptr >= 0x00676000u && vptr <= 0x00686000u) {
    const std::uint32_t dyn = read_u32(ram, ram_size, obj_ptr + 0x70u);
    if (dyn != 0 && dyn < ram_size && dyn != obj_ptr) {
      return true;
    }
  }
  return false;
}

inline bool is_valid_guest_span(std::uint32_t addr, std::uint32_t size, std::size_t ram_size) noexcept {
  return addr >= 0x00010000u && addr < ram_size && size <= (ram_size - addr) && (addr % 4u == 0);
}

inline std::uint64_t fnv1a_64(const void *data, std::size_t size, std::uint64_t hash = 14695981039346656037ULL) noexcept {
  const auto *bytes = static_cast<const std::uint8_t *>(data);
  for (std::size_t i = 0; i < size; ++i) {
    hash ^= bytes[i];
    hash *= 1099511628211ULL;
  }
  return hash;
}

}  // namespace

MutationBucket classify_mutation(std::uint32_t addr, std::uint32_t race_obj,
                                 std::uint32_t camera_addr, std::uint32_t coursedata_addr) noexcept {
  (void)coursedata_addr;
  if (race_obj != 0) {
    if (addr >= race_obj + 0xE200u && addr < race_obj + 0xE500u) return MutationBucket::render_frame_counter;
    if (addr >= race_obj + 0xCF38u && addr <= race_obj + 0xCF70u) return MutationBucket::hud_history_byte;
    if (addr >= race_obj + 0x80u && addr < race_obj + 0x0D80u) return MutationBucket::camera_cached_matrix;

    // Flare object at race_obj + 0xDA0 (TASK-117 / TASK-082 study):
    // flare+0x00..+0x78: buffer pointer, sample counter (+0x08), buffer offset (+0x20), target coords
    if (addr >= race_obj + 0x0DA0u && addr < race_obj + 0x0EA4u) return MutationBucket::flare_state;
    // flare+0x104 (race_obj + 0xEA4, 2048 bytes): GS-to-EE readback result table
    if (addr >= race_obj + 0x0EA4u && addr < race_obj + 0x0EA4u + 2048u) return MutationBucket::flare_state;
    // Flare queues at flare+0xA30 (0x17D0) and flare+0x1840 (0x25E0)
    if (addr >= race_obj + 0x17D0u && addr < race_obj + 0x17F0u) return MutationBucket::flare_state;
    if (addr >= race_obj + 0x25E0u && addr < race_obj + 0x2600u) return MutationBucket::flare_state;

    // Render DMA packet heads (excluding flare object)
    if (addr >= race_obj + 0x0D80u && addr < race_obj + 0x0DA0u) return MutationBucket::render_packet_ptr;
  }
  if (camera_addr != 0) {
    if (addr >= camera_addr && addr < camera_addr + 0x200u) return MutationBucket::camera_cached_matrix;
  }
  if (addr >= 0x0062020Cu && addr <= 0x00620214u) return MutationBucket::double_buffer_toggle;
  if (addr >= 0x006214D0u && addr <= 0x00621520u) return MutationBucket::coursedata_ms;

  return MutationBucket::unattributed;
}

Probe::Probe() noexcept {
  init();
}

Probe::~Probe() noexcept {
  if (enabled_) {
    finish(stderr);
  }
}

Probe &Probe::shared() noexcept {
  static Probe instance;
  return instance;
}

void Probe::init(const char *env_fps_probe, const char *env_diag_update_only) noexcept {
  const char *probe_val = env_fps_probe ? env_fps_probe : std::getenv("GT4_FPS_PROBE");
  enabled_ = (probe_val != nullptr && std::strcmp(probe_val, "1") == 0);

  const char *diag_val = env_diag_update_only ? env_diag_update_only : std::getenv("GT4_FPS_PROBE_DIAG_UPDATE_ONLY");
  diag_update_only_ = enabled_ && (diag_val != nullptr && std::strcmp(diag_val, "1") == 0);

  clock_us_ = &default_steady_clock_us;
  reset();

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

void Probe::reset() noexcept {
  phase_ = MainLoopPhase::outside;
  phase_start_us_ = 0;
  frame_start_us_ = 0;
  active_race_obj_ = 0;
  active_camera_view_ = 0;
  active_dynamics_ = 0;
  seen_render_ = true;
  diag_skip_render_ = false;

  stats_ = FrameStats{};
  watched_spans_.clear();
  snapshot_buffer_.clear();
  recent_mutations_.clear();
}

void Probe::set_clock_adapter(ClockFn clock_us) noexcept {
  clock_us_ = clock_us ? clock_us : &default_steady_clock_us;
}

std::uint64_t Probe::now_us() const noexcept {
  return clock_us_ ? clock_us_() : 0;
}

void Probe::clear_watched_spans() noexcept {
  watched_spans_.clear();
  snapshot_buffer_.clear();
}

void Probe::add_watched_span(std::uint32_t guest_addr, std::uint32_t size_bytes, const char *name) noexcept {
  watched_spans_.push_back({guest_addr, size_bytes, name});
}

void Probe::setup_default_spans(std::uint32_t race_obj, const std::uint8_t *ram, std::size_t ram_size) noexcept {
  if (!watched_spans_.empty() || race_obj == 0 || ram == nullptr) return;

  // 1. RacePS2Base root, input, replay, HUD history, render frame counter
  add_watched_span(race_obj + 0x0000u, 0x100u, "RacePS2Base_root_pointers");
  add_watched_span(race_obj + 0x0020u, 0x50u,  "RacePS2Base_input_control_fetch");
  add_watched_span(race_obj + 0x0D00u, 0x100u, "RacePS2Base_replay_timers");
  add_watched_span(race_obj + 0xCF00u, 0x100u, "RacePS2Base_hud_history_smoothing");
  add_watched_span(race_obj + 0xE200u, 0x200u, "RacePS2Base_render_frame_counter_tail");

  // 2. CameraManager (race_obj + 0x80) and CameraView 0
  const std::uint32_t cam_mgr = race_obj + 0x80u;
  active_camera_view_ = cam_mgr + 0x158u;
  add_watched_span(cam_mgr, 0x1000u, "CameraManager_and_views");

  // 3. DynamicsConductor simulation state (dereferenced from pointer at race_obj + 0x70)
  const std::uint32_t dynamics_ptr = read_u32(ram, ram_size, race_obj + 0x70u);
  if (is_valid_guest_span(dynamics_ptr, 0x10000u, ram_size)) {
    active_dynamics_ = dynamics_ptr;
    // Full 64 KB DynamicsConductor: physics, clocks, lap times, collision, AI
    add_watched_span(active_dynamics_, 0x10000u, "DynamicsConductor_physics_clocks_ai");
  }

  // 4. Cars (Automobile entries from race_obj + 0x60)
  const std::uint32_t cars_ptr = read_u32(ram, ram_size, race_obj + 0x60u);
  if (is_valid_guest_span(cars_ptr, 24u, ram_size)) {
    for (std::uint32_t i = 0; i < 6u; ++i) {
      const std::uint32_t car_entry = read_u32(ram, ram_size, cars_ptr + i * 4u);
      if (is_valid_guest_span(car_entry, 0x3000u, ram_size)) {
        add_watched_span(car_entry, 0x3000u, "Automobile_car_entry");
      }
    }
  }

  // 5. Global clocks, course data ms, double buffer toggle
  add_watched_span(0x00620000u, 0x400u, "DoubleBuffer_and_DMA_state");
  add_watched_span(0x00621400u, 0x200u, "CourseData_clock_ms");

  // 6. Flare object state (TASK-117 / TASK-082)
  add_watched_span(race_obj + 0x0DA0u, 0x104u, "Flare_header_and_sample_table");
  add_watched_span(race_obj + 0x0EA4u, 2048u,  "Flare_readback_result_table");
  add_watched_span(race_obj + 0x17D0u, 0x20u,  "Flare_queue_primary");
  add_watched_span(race_obj + 0x25E0u, 0x20u,  "Flare_queue_secondary");
}

void Probe::observe_candidate_object(std::uint32_t obj_ptr, const std::uint8_t *ram, std::size_t ram_size) noexcept {
  if (!enabled_ || obj_ptr == 0 || ram == nullptr) return;
  if (is_race_object(obj_ptr, ram, ram_size)) {
    if (active_race_obj_ != obj_ptr) {
      const bool had_previous = (active_race_obj_ != 0);
      active_race_obj_ = obj_ptr;
      if (had_previous) {
        clear_watched_spans();
        stats_.purity = PurityReport{};
        recent_mutations_.clear();
      }
      setup_default_spans(active_race_obj_, ram, ram_size);
    }
  }
}

void Probe::on_update_enter(std::uint32_t obj_ptr, const std::uint8_t *ram, std::size_t ram_size) noexcept {
  if (!enabled_) return;

  if (obj_ptr != 0) {
    observe_candidate_object(obj_ptr, ram, ram_size);
  }

  phase_ = MainLoopPhase::update;
  phase_start_us_ = now_us();
  if (frame_start_us_ == 0) frame_start_us_ = phase_start_us_;

  ++stats_.update_count;

  // Read guest race clock if accessible
  if (ram != nullptr && active_race_obj_ != 0) {
    const std::uint32_t dynamics_ptr = active_dynamics_ ? active_dynamics_ : read_u32(ram, ram_size, active_race_obj_ + 0x70u);
    if (dynamics_ptr != 0) {
      stats_.race_clock_f898 = read_u32(ram, ram_size, dynamics_ptr + 0xF898u);
    }
    stats_.render_frame_counter = read_u32(ram, ram_size, active_race_obj_ + 0xE32Cu);
  }
}

std::uint64_t Probe::compute_sim_digest(const std::uint8_t *ram, std::size_t ram_size) const noexcept {
  if (ram == nullptr || active_race_obj_ == 0) return 0;
  std::uint64_t h = 14695981039346656037ULL;
  bool found_any = false;
  for (const auto &span : watched_spans_) {
    if (std::strcmp(span.name, "DynamicsConductor_physics_clocks_ai") == 0 ||
        std::strcmp(span.name, "Automobile_car_entry") == 0) {
      if (is_valid_guest_span(span.guest_addr, span.size_bytes, ram_size)) {
        h = fnv1a_64(ram + span.guest_addr, span.size_bytes, h);
        found_any = true;
      }
    }
  }
  return found_any ? h : 0;
}

void Probe::on_update_leave(const std::uint8_t *ram, std::size_t ram_size) noexcept {
  if (!enabled_ || phase_ != MainLoopPhase::update) return;

  const std::uint64_t now = now_us();
  stats_.update_time_us += (now - phase_start_us_);
  phase_ = MainLoopPhase::outside;

  if (ram != nullptr && active_race_obj_ != 0) {
    const std::uint32_t dynamics_ptr = active_dynamics_ ? active_dynamics_ : read_u32(ram, ram_size, active_race_obj_ + 0x70u);
    if (dynamics_ptr != 0 && is_valid_guest_span(dynamics_ptr, 0x10000u, ram_size)) {
      stats_.race_clock_f898 = read_u32(ram, ram_size, dynamics_ptr + 0xF898u);
      stats_.sim_digest = compute_sim_digest(ram, ram_size);
    }
  }
}

void Probe::on_render_enter(std::uint32_t obj_ptr, const std::uint8_t *ram, std::size_t ram_size) noexcept {
  if (!enabled_) return;

  if (obj_ptr != 0) {
    observe_candidate_object(obj_ptr, ram, ram_size);
  }

  phase_ = MainLoopPhase::render;
  phase_start_us_ = now_us();
  if (frame_start_us_ == 0) frame_start_us_ = phase_start_us_;

  ++stats_.render_count;

  // Snapshot memory before render pass
  take_render_snapshot(ram, ram_size);
}

void Probe::on_render_leave(const std::uint8_t *ram, std::size_t ram_size) noexcept {
  if (!enabled_ || phase_ != MainLoopPhase::render) return;

  const std::uint64_t now = now_us();
  stats_.render_time_us += (now - phase_start_us_);
  phase_ = MainLoopPhase::outside;

  // Diff memory altered during render
  diff_render_purity(ram, ram_size, /*is_idle_pass=*/false);

  // Read guest state post-render
  if (ram != nullptr && active_race_obj_ != 0) {
    stats_.render_frame_counter = read_u32(ram, ram_size, active_race_obj_ + 0xE32Cu);
    stats_.hud_history_byte     = read_u8(ram, ram_size, active_race_obj_ + 0xCF5Au);
    stats_.double_buffer_toggle = read_u32(ram, ram_size, 0x00620210u);
    stats_.coursedata_ms        = read_u32(ram, ram_size, 0x006214D8u);

    // Read CameraView poses and matrix
    if (active_camera_view_ != 0) {
      stats_.camera_fov = read_f32(ram, ram_size, active_camera_view_ + 0x14u);
      for (int i = 0; i < 3; ++i) {
        stats_.camera_pose0.angles[i].x = read_f32(ram, ram_size, active_camera_view_ + 0x28u + i * 12u);
        stats_.camera_pose0.angles[i].y = read_f32(ram, ram_size, active_camera_view_ + 0x2Cu + i * 12u);
        stats_.camera_pose0.angles[i].z = read_f32(ram, ram_size, active_camera_view_ + 0x30u + i * 12u);

        stats_.camera_pose0.translations[i].x = read_f32(ram, ram_size, active_camera_view_ + 0x4Cu + i * 12u);
        stats_.camera_pose0.translations[i].y = read_f32(ram, ram_size, active_camera_view_ + 0x50u + i * 12u);
        stats_.camera_pose0.translations[i].z = read_f32(ram, ram_size, active_camera_view_ + 0x54u + i * 12u);

        stats_.camera_pose1.angles[i].x = read_f32(ram, ram_size, active_camera_view_ + 0x94u + i * 12u);
        stats_.camera_pose1.angles[i].y = read_f32(ram, ram_size, active_camera_view_ + 0x98u + i * 12u);
        stats_.camera_pose1.angles[i].z = read_f32(ram, ram_size, active_camera_view_ + 0x9Cu + i * 12u);

        stats_.camera_pose1.translations[i].x = read_f32(ram, ram_size, active_camera_view_ + 0xB8u + i * 12u);
        stats_.camera_pose1.translations[i].y = read_f32(ram, ram_size, active_camera_view_ + 0xBCu + i * 12u);
        stats_.camera_pose1.translations[i].z = read_f32(ram, ram_size, active_camera_view_ + 0xC0u + i * 12u);
      }
      for (int m = 0; m < 16; ++m) {
        stats_.camera_cached_matrix[m] = read_f32(ram, ram_size, active_camera_view_ + 0x100u + m * 4u);
      }
    }
  }
}

void Probe::on_idle_enter(std::uint32_t obj_ptr, const std::uint8_t *ram, std::size_t ram_size) noexcept {
  (void)obj_ptr; (void)ram; (void)ram_size;
  if (!enabled_) return;

  phase_ = MainLoopPhase::idle;
  phase_start_us_ = now_us();
  ++stats_.idle_count;
}

void Probe::on_idle_leave(const std::uint8_t *ram, std::size_t ram_size) noexcept {
  if (!enabled_ || phase_ != MainLoopPhase::idle) return;

  const std::uint64_t now = now_us();
  stats_.idle_time_us += (now - phase_start_us_);
  phase_ = MainLoopPhase::outside;

  // Diff memory altered during idle (DMAsafe swaps)
  diff_render_purity(ram, ram_size, /*is_idle_pass=*/true);
}

void Probe::on_flip_wait_enter() noexcept {
  if (!enabled_) return;

  phase_ = MainLoopPhase::flip_wait;
  phase_start_us_ = now_us();
  ++stats_.flip_wait_count;
}

void Probe::on_flip_wait_leave() noexcept {
  if (!enabled_ || phase_ != MainLoopPhase::flip_wait) return;

  const std::uint64_t now = now_us();
  stats_.flip_wait_time_us += (now - phase_start_us_);
  phase_ = MainLoopPhase::outside;
}

void Probe::on_frame_boundary() noexcept {
  if (!enabled_) return;

  if (phase_ == MainLoopPhase::flip_wait) {
    on_flip_wait_leave();
  }

  const std::uint64_t now = now_us();
  if (frame_start_us_ != 0) {
    stats_.total_frame_time_us += (now - frame_start_us_);
  }
  frame_start_us_ = now;
  ++stats_.frame_count;

  if (diag_update_only_ && stats_.race_clock_f898 > 0) {
    diag_skip_render_ = !diag_skip_render_;
  } else {
    diag_skip_render_ = false;
  }

  if (stats_.frame_count > 0 && stats_.frame_count % 600 == 0) {
    format_report(stderr);
  }
}

void Probe::take_render_snapshot(const std::uint8_t *ram, std::size_t ram_size) noexcept {
  if (ram == nullptr || watched_spans_.empty()) return;

  std::size_t total_bytes = 0;
  for (const auto &span : watched_spans_) {
    total_bytes += span.size_bytes;
  }
  snapshot_buffer_.resize(total_bytes);

  std::size_t offset = 0;
  for (const auto &span : watched_spans_) {
    if (span.guest_addr + span.size_bytes <= ram_size) {
      std::memcpy(snapshot_buffer_.data() + offset, ram + span.guest_addr, span.size_bytes);
    } else {
      std::memset(snapshot_buffer_.data() + offset, 0, span.size_bytes);
    }
    offset += span.size_bytes;
  }
}

void Probe::diff_render_purity(const std::uint8_t *ram, std::size_t ram_size, bool is_idle_pass) noexcept {
  if (ram == nullptr || snapshot_buffer_.empty()) return;

  std::size_t offset = 0;
  for (const auto &span : watched_spans_) {
    if (span.guest_addr + span.size_bytes > ram_size) {
      offset += span.size_bytes;
      continue;
    }

    const std::size_t words = span.size_bytes / 4u;
    stats_.purity.total_inspected_words += words;

    for (std::size_t w = 0; w < words; ++w) {
      const std::uint32_t word_addr = span.guest_addr + static_cast<std::uint32_t>(w * 4u);
      std::uint32_t old_val = 0;
      std::uint32_t new_val = 0;

      std::memcpy(&old_val, snapshot_buffer_.data() + offset + w * 4u, sizeof old_val);
      std::memcpy(&new_val, ram + word_addr, sizeof new_val);

      if (old_val != new_val) {
        ++stats_.purity.mutated_words;
        // Update snapshot so subsequent checks on this pass don't re-trigger
        std::memcpy(snapshot_buffer_.data() + offset + w * 4u, &new_val, sizeof new_val);

        MutationBucket bucket = classify_mutation(word_addr, active_race_obj_, active_camera_view_, 0x006214D8u);
        if (is_idle_pass && bucket == MutationBucket::unattributed) {
          // Attribute only known DMA buffer chain pointers in CourseData or double buffer swaps during idle
          if ((word_addr >= 0x00620000u && word_addr < 0x00620100u) ||
              (word_addr >= 0x00621400u && word_addr < 0x00621450u)) {
            bucket = MutationBucket::dma_safe_idle_swap;
          }
        }

        switch (bucket) {
          case MutationBucket::render_frame_counter:
            ++stats_.purity.render_counter_mutations;
            break;
          case MutationBucket::hud_history_byte:
            ++stats_.purity.hud_history_mutations;
            break;
          case MutationBucket::double_buffer_toggle:
            ++stats_.purity.double_buffer_mutations;
            break;
          case MutationBucket::coursedata_ms:
            ++stats_.purity.coursedata_mutations;
            break;
          case MutationBucket::camera_cached_matrix:
            ++stats_.purity.camera_matrix_mutations;
            break;
          case MutationBucket::dma_safe_idle_swap:
            ++stats_.purity.dma_swap_mutations;
            break;
          case MutationBucket::flare_state:
            ++stats_.purity.flare_mutations;
            break;
          case MutationBucket::render_packet_ptr:
            ++stats_.purity.render_packet_ptr_mutations;
            break;
          case MutationBucket::unattributed:
          default:
            ++stats_.purity.unattributed_mutations;
            if (recent_mutations_.size() < 16) {
              recent_mutations_.push_back({word_addr, old_val, new_val, MutationBucket::unattributed});
            }
            break;
        }
      }
    }
    offset += span.size_bytes;
  }
}

void Probe::format_report(std::FILE *stream) const noexcept {
  if (!enabled_ || stream == nullptr) return;

  const double f = (stats_.frame_count > 0) ? static_cast<double>(stats_.frame_count) : 1.0;
  const double update_ms = (static_cast<double>(stats_.update_time_us) / 1000.0) / f;
  const double render_ms = (static_cast<double>(stats_.render_time_us) / 1000.0) / f;
  const double idle_ms   = (static_cast<double>(stats_.idle_time_us) / 1000.0) / f;
  const double flip_ms   = (static_cast<double>(stats_.flip_wait_time_us) / 1000.0) / f;
  const double total_ms  = (static_cast<double>(stats_.total_frame_time_us) / 1000.0) / f;

  std::fprintf(stream,
               "[fps-probe] interval: frames=%llu update_ms=%.3f render_ms=%.3f idle_ms=%.3f flip_ms=%.3f total_ms=%.3f\n",
               static_cast<unsigned long long>(stats_.frame_count),
               update_ms, render_ms, idle_ms, flip_ms, total_ms);

  std::fprintf(stream,
               "[fps-probe] guest: clock_f898=%u render_counter=%u hud_hist=0x%02X buf_toggle=%u course_ms=%u\n",
               stats_.race_clock_f898,
               stats_.render_frame_counter,
               static_cast<unsigned>(stats_.hud_history_byte),
               stats_.double_buffer_toggle,
               stats_.coursedata_ms);

  std::fprintf(stream,
               "[fps-probe] camera: fov=%.1f cached_m00=%.4f\n",
               stats_.camera_fov,
               stats_.camera_cached_matrix[0]);

  std::fprintf(stream,
               "[fps-probe] digest: clock_f898=%u hash=0x%016llx\n",
               stats_.race_clock_f898,
               static_cast<unsigned long long>(stats_.sim_digest));

  std::fprintf(stream,
               "[fps-probe] purity: mutations=%llu (render_counter:%llu hud_hist:%llu buf_toggle:%llu cam_matrix:%llu dma_swap:%llu flare:%llu pkt_ptr:%llu unattributed:%llu)\n",
               static_cast<unsigned long long>(stats_.purity.mutated_words),
               static_cast<unsigned long long>(stats_.purity.render_counter_mutations),
               static_cast<unsigned long long>(stats_.purity.hud_history_mutations),
               static_cast<unsigned long long>(stats_.purity.double_buffer_mutations),
               static_cast<unsigned long long>(stats_.purity.camera_matrix_mutations),
               static_cast<unsigned long long>(stats_.purity.dma_swap_mutations),
               static_cast<unsigned long long>(stats_.purity.flare_mutations),
               static_cast<unsigned long long>(stats_.purity.render_packet_ptr_mutations),
               static_cast<unsigned long long>(stats_.purity.unattributed_mutations));

  if (stats_.purity.simulation_clean()) {
    std::fprintf(stream, "[fps-probe] verdict: clean (unattributed_words=0)\n");
  } else {
    std::fprintf(stream, "[fps-probe] verdict: CONTAMINATED (unattributed_words=%llu)\n",
                 static_cast<unsigned long long>(stats_.purity.unattributed_mutations));
    for (std::size_t i = 0; i < recent_mutations_.size(); ++i) {
      const auto &m = recent_mutations_[i];
      std::fprintf(stream, "[fps-probe] mutation[%zu]: addr=0x%08X old=0x%08X new=0x%08X (unattributed)\n",
                   i, m.address, m.old_value, m.new_value);
    }
  }

  if (diag_update_only_) {
    std::fprintf(stream, "[fps-probe] diagnostic: UPDATE_ONLY_ACTIVE (render and idle bypassed for EE budgeting)\n");
  }

  if (gt4::render_skip::SkipManager::shared().enabled()) {
    gt4::render_skip::SkipManager::shared().format_report(stream);
  }
}

void Probe::finish(std::FILE *stream) noexcept {
  if (!enabled_ || stream == nullptr) return;
  format_report(stream);
}

}  // namespace gt4::fps_probe
