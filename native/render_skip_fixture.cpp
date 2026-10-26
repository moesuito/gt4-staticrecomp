#include "render_skip.hpp"
#include "game_target.hpp"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <stdexcept>

namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::abort();
  }
}

struct FakeCpu {
  struct Reg {
    std::uint32_t w[4] = {};
  };
  Reg r[32] = {};
  std::uint32_t pc = 0;
};

void setup_fake_race_object(std::vector<std::uint8_t> &ram, std::uint32_t race_obj, std::uint32_t dyn) {
  // RacePS2Base vtable at race_obj + 0x64
  const std::uint32_t vptr = 0x00679378u;
  std::memcpy(ram.data() + race_obj + 0x64u, &vptr, sizeof vptr);

  // DynamicsConductor pointer at race_obj + 0x70
  std::memcpy(ram.data() + race_obj + 0x70u, &dyn, sizeof dyn);

  // CameraManager at race_obj + 0x80 (viewport count = 1 at cam_mgr + 4)
  const std::uint32_t viewports = 1u;
  std::memcpy(ram.data() + race_obj + 0x80u + 4u, &viewports, sizeof viewports);
}

void test_env_parsing() {
  auto &skip = gt4::render_skip::SkipManager::shared();

  skip.init(nullptr, nullptr);
  require(!skip.enabled(), "default unset must be disabled");
  require(skip.force_pattern() == 0, "default force pattern is 0");

  skip.init("", "");
  require(!skip.enabled(), "empty string must be disabled");

  skip.init("0", "0");
  require(!skip.enabled(), "'0' must be disabled");

  skip.init("false", "2");
  require(!skip.enabled(), "'false' must be disabled");

  skip.init("skip", nullptr);
  require(skip.enabled(), "'skip' must be enabled");
  require(skip.force_pattern() == 0, "unset force pattern is 0");

  skip.init("skip", "2");
  require(skip.enabled(), "'skip' enabled with force=2");
  require(skip.force_pattern() == 2, "force pattern 2 parsed");

  skip.init("skip", "3");
  require(skip.enabled(), "'skip' enabled with force=3");
  require(skip.force_pattern() == 3, "force pattern 3 parsed");

  skip.init("skip", "4");
  require(skip.enabled(), "'skip' enabled with force=4 fallback to 0");
  require(skip.force_pattern() == 0, "force pattern 4 falls back to 0");

  skip.init("skip", "invalid");
  require(skip.force_pattern() == 0, "invalid force pattern falls back to 0");

  skip.reset();
}

void test_target_gate() {
  auto &skip = gt4::render_skip::SkipManager::shared();
  skip.init("skip", "2");

  std::vector<std::uint8_t> ram(0x200000, 0);
  const std::uint32_t race_obj = 0x50000u;
  const std::uint32_t dyn = 0x80000u;
  setup_fake_race_object(ram, race_obj, dyn);

  skip.set_active_race_object_for_test(race_obj);

  // Active target is v2.00: evaluate_gate should succeed
  require(gt4::target::active().id == gt4::target::Id::v200, "active target is v2.00");
  skip.evaluate_gate(ram.data(), ram.size());
  // Pre-clock good_frames is 0
  require(!skip.gate_open(), "gate not open before clock advances");

  skip.reset();
}

void test_gate_engagement() {
  auto &skip = gt4::render_skip::SkipManager::shared();
  skip.init("skip", "2");

  std::vector<std::uint8_t> ram(0x200000, 0);
  const std::uint32_t race_obj = 0x50000u;
  const std::uint32_t dyn = 0x80000u;
  setup_fake_race_object(ram, race_obj, dyn);
  skip.set_active_race_object_for_test(race_obj);

  // 1. Failure case: null race object
  skip.set_active_race_object_for_test(0);
  skip.evaluate_gate(ram.data(), ram.size());
  require(!skip.gate_open(), "gate must stay closed for null race object");

  // 2. Failure case: non-race object (invalid vtable)
  skip.set_active_race_object_for_test(race_obj);
  const std::uint32_t bad_vptr = 0x12345678u;
  std::memcpy(ram.data() + race_obj + 0x64u, &bad_vptr, sizeof bad_vptr);
  skip.evaluate_gate(ram.data(), ram.size());
  require(!skip.gate_open(), "gate must stay closed for non-race object");

  // Restore vtable
  const std::uint32_t vptr = 0x00679378u;
  std::memcpy(ram.data() + race_obj + 0x64u, &vptr, sizeof vptr);

  // 3. Failure case: clock does not advance (delta == 0)
  std::uint32_t clock = 0;
  std::memcpy(ram.data() + dyn + 0xF898u, &clock, sizeof clock);
  skip.evaluate_gate(ram.data(), ram.size());
  require(!skip.gate_open(), "gate must stay closed when clock is 0");
  require(skip.good_frames() == 0, "good frames is 0 when clock is 0");

  // 4. Consecutive good frames: 1 to 59
  for (std::uint32_t i = 1; i < 60; ++i) {
    clock += 50u;
    std::memcpy(ram.data() + dyn + 0xF898u, &clock, sizeof clock);
    skip.evaluate_gate(ram.data(), ram.size());
    require(skip.good_frames() == i, "good frames incrementing");
    require(!skip.gate_open(), "gate must not open before frame 60");
  }

  // Frame 60: gate opens!
  clock += 50u;
  std::memcpy(ram.data() + dyn + 0xF898u, &clock, sizeof clock);
  skip.evaluate_gate(ram.data(), ram.size());
  require(skip.good_frames() == 60, "good frames reached 60");
  require(skip.gate_open(), "gate MUST open at 60 consecutive good frames");

  // Frame 61: pause / stall (clock does not advance) -> gate immediately closes
  std::memcpy(ram.data() + dyn + 0xF898u, &clock, sizeof clock);
  skip.evaluate_gate(ram.data(), ram.size());
  require(!skip.gate_open(), "gate MUST close immediately when clock stops");
  require(skip.good_frames() == 0, "good frames reset to 0 on gate closure");
  require(skip.stats().gate_closures == 1, "gate closure counted");

  skip.reset();
}

void test_force_pattern_2() {
  auto &skip = gt4::render_skip::SkipManager::shared();
  skip.init("skip", "2");

  std::vector<std::uint8_t> ram(0x200000, 0);
  const std::uint32_t race_obj = 0x50000u;
  const std::uint32_t dyn = 0x80000u;
  setup_fake_race_object(ram, race_obj, dyn);
  skip.set_active_race_object_for_test(race_obj);

  FakeCpu cpu;
  cpu.r[4].w[0] = race_obj;
  const auto &hooks = gt4::target::kV200Hooks;

  // Open gate by feeding 60 good frames
  std::uint32_t clock = 0;
  for (std::uint32_t i = 0; i <= 60; ++i) {
    clock += 50u;
    std::memcpy(ram.data() + dyn + 0xF898u, &clock, sizeof clock);
    skip.evaluate_gate(ram.data(), ram.size());
  }
  require(skip.gate_open(), "gate is open for force pattern test");

  // Pattern 2: alternating Render (false), Skip (true), Render (false), Skip (true)...
  clock += 50u;
  std::memcpy(ram.data() + dyn + 0xF898u, &clock, sizeof clock);
  bool b0 = skip.handle_dispatch(cpu, hooks.main_render_functor, ram.data(), ram.size());
  require(!b0, "frame 0 must render");

  clock += 50u;
  std::memcpy(ram.data() + dyn + 0xF898u, &clock, sizeof clock);
  bool b1 = skip.handle_dispatch(cpu, hooks.main_render_functor, ram.data(), ram.size());
  require(b1, "frame 1 must skip");

  clock += 50u;
  std::memcpy(ram.data() + dyn + 0xF898u, &clock, sizeof clock);
  bool b2 = skip.handle_dispatch(cpu, hooks.main_render_functor, ram.data(), ram.size());
  require(!b2, "frame 2 must render");

  clock += 50u;
  std::memcpy(ram.data() + dyn + 0xF898u, &clock, sizeof clock);
  bool b3 = skip.handle_dispatch(cpu, hooks.main_render_functor, ram.data(), ram.size());
  require(b3, "frame 3 must skip");

  require(skip.stats().frames_rendered == 2, "2 frames rendered");
  require(skip.stats().frames_skipped == 2, "2 frames skipped");
  require(skip.stats().longest_skip_run == 1, "longest skip run is 1 for force=2");

  skip.reset();
}

void test_force_pattern_3() {
  auto &skip = gt4::render_skip::SkipManager::shared();
  skip.init("skip", "3");

  std::vector<std::uint8_t> ram(0x200000, 0);
  const std::uint32_t race_obj = 0x50000u;
  const std::uint32_t dyn = 0x80000u;
  setup_fake_race_object(ram, race_obj, dyn);
  skip.set_active_race_object_for_test(race_obj);

  FakeCpu cpu;
  cpu.r[4].w[0] = race_obj;
  const auto &hooks = gt4::target::kV200Hooks;

  // Open gate
  std::uint32_t clock = 0;
  for (std::uint32_t i = 0; i <= 60; ++i) {
    clock += 50u;
    std::memcpy(ram.data() + dyn + 0xF898u, &clock, sizeof clock);
    skip.evaluate_gate(ram.data(), ram.size());
  }
  require(skip.gate_open(), "gate is open for force pattern 3 test");

  // Pattern 3: Render (false), Skip (true), Skip (true), Render (false), Skip (true), Skip (true)...
  clock += 50u;
  std::memcpy(ram.data() + dyn + 0xF898u, &clock, sizeof clock);
  bool b0 = skip.handle_dispatch(cpu, hooks.main_render_functor, ram.data(), ram.size());
  require(!b0, "frame 0 must render");

  clock += 50u;
  std::memcpy(ram.data() + dyn + 0xF898u, &clock, sizeof clock);
  bool b1 = skip.handle_dispatch(cpu, hooks.main_render_functor, ram.data(), ram.size());
  require(b1, "frame 1 must skip");

  clock += 50u;
  std::memcpy(ram.data() + dyn + 0xF898u, &clock, sizeof clock);
  bool b2 = skip.handle_dispatch(cpu, hooks.main_render_functor, ram.data(), ram.size());
  require(b2, "frame 2 must skip");

  clock += 50u;
  std::memcpy(ram.data() + dyn + 0xF898u, &clock, sizeof clock);
  bool b3 = skip.handle_dispatch(cpu, hooks.main_render_functor, ram.data(), ram.size());
  require(!b3, "frame 3 must render");

  clock += 50u;
  std::memcpy(ram.data() + dyn + 0xF898u, &clock, sizeof clock);
  bool b4 = skip.handle_dispatch(cpu, hooks.main_render_functor, ram.data(), ram.size());
  require(b4, "frame 4 must skip");

  clock += 50u;
  std::memcpy(ram.data() + dyn + 0xF898u, &clock, sizeof clock);
  bool b5 = skip.handle_dispatch(cpu, hooks.main_render_functor, ram.data(), ram.size());
  require(b5, "frame 5 must skip");

  require(skip.stats().frames_rendered == 2, "2 frames rendered");
  require(skip.stats().frames_skipped == 4, "4 frames skipped");
  require(skip.stats().longest_skip_run == 2, "longest skip run is 2 for force=3");

  skip.reset();
}

void test_flare_queue_clearing() {
  auto &skip = gt4::render_skip::SkipManager::shared();
  skip.init("skip", "2");

  std::vector<std::uint8_t> ram(0x200000, 0);
  const std::uint32_t race_obj = 0x50000u;
  const std::uint32_t dyn = 0x80000u;
  setup_fake_race_object(ram, race_obj, dyn);
  skip.set_active_race_object_for_test(race_obj);

  FakeCpu cpu;
  cpu.r[4].w[0] = race_obj;
  const auto &hooks = gt4::target::kV200Hooks;

  // Open gate by feeding 60 good frames
  std::uint32_t clock = 0;
  for (std::uint32_t i = 0; i <= 60; ++i) {
    clock += 50u;
    std::memcpy(ram.data() + dyn + 0xF898u, &clock, sizeof clock);
    skip.evaluate_gate(ram.data(), ram.size());
  }
  require(skip.gate_open(), "gate is open for flare queue test");

  // Set initial flare queue counts
  const std::uint32_t flare_base = race_obj + 0x0DA0u;
  const std::uint32_t track_count_addr = flare_base + 0x1848u;
  const std::uint32_t car_count_addr = flare_base + 0x0A34u;

  const std::uint32_t initial_track = 45u;
  const std::uint32_t initial_car = 38u;
  const std::uint32_t canary = 0xDEADBEEFu;

  std::memcpy(ram.data() + track_count_addr, &initial_track, sizeof initial_track);
  std::memcpy(ram.data() + car_count_addr, &initial_car, sizeof initial_car);
  std::memcpy(ram.data() + track_count_addr + 4u, &canary, sizeof canary);

  // Frame 0: render (not skipped)
  clock += 50u;
  std::memcpy(ram.data() + dyn + 0xF898u, &clock, sizeof clock);
  skip.handle_dispatch(cpu, hooks.main_render_functor, ram.data(), ram.size());
  require(!skip.skipped_this_frame(), "frame 0 not skipped");

  // Frame 1: skip render
  clock += 50u;
  std::memcpy(ram.data() + dyn + 0xF898u, &clock, sizeof clock);
  bool bypassed = skip.handle_dispatch(cpu, hooks.main_render_functor, ram.data(), ram.size());
  require(bypassed, "frame 1 render skipped");
  require(skip.skipped_this_frame(), "skipped_this_frame set");
  require(skip.stats().max_flare_queue_count == 45u, "max flare count recorded before clearing");

  // Flare counts still non-zero before update runs
  std::uint32_t val = 0;
  std::memcpy(&val, ram.data() + track_count_addr, sizeof val);
  require(val == 45u, "flare count not cleared before update");

  // Idle functor runs and is bypassed
  bool idle_bypassed = skip.handle_dispatch(cpu, hooks.main_idle_functor, ram.data(), ram.size());
  require(idle_bypassed, "idle functor bypassed on skipped frame");

  // Frame 2: update functor enters!
  skip.handle_dispatch(cpu, hooks.main_update_functor, ram.data(), ram.size());

  // Flare counts MUST be 0 now!
  std::memcpy(&val, ram.data() + track_count_addr, sizeof val);
  require(val == 0u, "track flare count cleared to 0 before update");
  std::memcpy(&val, ram.data() + car_count_addr, sizeof val);
  require(val == 0u, "car flare count cleared to 0 before update");

  // Adjacent memory must not be corrupted
  std::memcpy(&val, ram.data() + track_count_addr + 4u, sizeof val);
  require(val == canary, "adjacent canary word unmodified");

  skip.reset();
}

void test_timing_policy() {
  auto &skip = gt4::render_skip::SkipManager::shared();
  skip.init("skip", nullptr); // No force pattern -> timing policy active

  std::vector<std::uint8_t> ram(0x200000, 0);
  const std::uint32_t race_obj = 0x50000u;
  const std::uint32_t dyn = 0x80000u;
  setup_fake_race_object(ram, race_obj, dyn);
  skip.set_active_race_object_for_test(race_obj);

  FakeCpu cpu;
  cpu.r[4].w[0] = race_obj;
  const auto &hooks = gt4::target::kV200Hooks;

  // Open gate by feeding 60 good frames
  std::uint32_t clock = 0;
  for (std::uint32_t i = 0; i <= 60; ++i) {
    clock += 50u;
    std::memcpy(ram.data() + dyn + 0xF898u, &clock, sizeof clock);
    skip.evaluate_gate(ram.data(), ram.size());
  }
  require(skip.gate_open(), "gate is open for timing policy test");

  const std::uint64_t freq = 1000000; // 1 tick = 1 us
  std::int64_t ticks = 0;

  // 1. Initial frame release: anchors clock
  skip.record_frame_wall(ticks, freq);
  require(skip.debt_us() == 0, "initial debt is 0");

  // 2. Fast frame: 12 ms (< 16.68 ms) -> debt stays 0, render runs
  ticks += 12000;
  skip.record_frame_wall(ticks, freq);
  require(skip.debt_us() == 0, "debt remains 0 on fast frame");
  clock += 50u;
  std::memcpy(ram.data() + dyn + 0xF898u, &clock, sizeof clock);
  bool bypassed = skip.handle_dispatch(cpu, hooks.main_render_functor, ram.data(), ram.size());
  require(!bypassed, "frame rendered when debt is 0");

  // 3. Heavy frame: 22 ms (> 16.683 ms) -> debt increases by 22000 - 16683 = 5317 us
  ticks += 22000;
  skip.record_frame_wall(ticks, freq);
  require(skip.debt_us() == 5317, "debt accumulated on heavy frame");

  // Next frame: render MUST be skipped!
  clock += 50u;
  std::memcpy(ram.data() + dyn + 0xF898u, &clock, sizeof clock);
  bypassed = skip.handle_dispatch(cpu, hooks.main_render_functor, ram.data(), ram.size());
  require(bypassed, "frame skipped when debt > 0");

  // 4. Floor 30 FPS rule: never skip two renders in a row
  // Even if debt is still > 0, the next frame MUST be rendered
  clock += 50u;
  std::memcpy(ram.data() + dyn + 0xF898u, &clock, sizeof clock);
  bypassed = skip.handle_dispatch(cpu, hooks.main_render_functor, ram.data(), ram.size());
  require(!bypassed, "never skip two renders in a row: subsequent frame must render");

  // 5. Hard cap: massive hitch of 100 ms
  ticks += 100000;
  skip.record_frame_wall(ticks, freq);
  require(skip.debt_us() == 33366, "debt capped at 33,366 us (hard cap)");

  // 6. Gate closure resets debt to 0
  skip.close_gate();
  require(skip.debt_us() == 0, "gate closure resets debt to 0");

  skip.reset();
}

void test_10000_steps_mixed() {
  auto &skip = gt4::render_skip::SkipManager::shared();
  skip.init("skip", nullptr);

  std::vector<std::uint8_t> ram(0x200000, 0);
  const std::uint32_t race_obj = 0x50000u;
  const std::uint32_t dyn = 0x80000u;
  setup_fake_race_object(ram, race_obj, dyn);
  skip.set_active_race_object_for_test(race_obj);

  FakeCpu cpu;
  cpu.r[4].w[0] = race_obj;
  const auto &hooks = gt4::target::kV200Hooks;

  const std::uint64_t freq = 1000000;
  std::int64_t ticks = 0;
  std::uint32_t clock = 0;

  skip.record_frame_wall(ticks, freq);

  for (int step = 0; step < 10000; ++step) {
    // Simulate varying wall times: 10ms, 14ms, 18ms, 22ms
    const int mod = step % 4;
    const std::int64_t wall_us = (mod == 0) ? 10000 : (mod == 1) ? 14000 : (mod == 2) ? 18000 : 22000;
    ticks += wall_us;
    skip.record_frame_wall(ticks, freq);

    // Periodic pause / pit stop every 2000 steps for 100 steps
    const bool paused = (step % 2000 >= 1900);
    if (!paused) {
      clock += 50u;
    }
    std::memcpy(ram.data() + dyn + 0xF898u, &clock, sizeof clock);

    // Update functor
    skip.handle_dispatch(cpu, hooks.main_update_functor, ram.data(), ram.size());

    // Render functor
    bool bypassed = skip.handle_dispatch(cpu, hooks.main_render_functor, ram.data(), ram.size());
    if (bypassed) {
      require(skip.stats().longest_skip_run <= 1, "timing policy never skips 2 in a row");
    }

    // Idle functor
    skip.handle_dispatch(cpu, hooks.main_idle_functor, ram.data(), ram.size());

    require(skip.debt_us() >= 0 && skip.debt_us() <= 33366, "debt stays within [0, kMaxDebtUs]");
  }

  const auto &st = skip.stats();
  require(st.frames_rendered > 0, "frames rendered > 0");
  require(st.frames_skipped > 0, "frames skipped > 0");
  require(st.gate_closures >= 4, "gate closures tracked across periodic pauses");
  require(st.longest_skip_run == 1, "longest skip run strictly 1 under timing policy");

  skip.reset();
}

} // namespace

int main() {
  test_env_parsing();
  test_target_gate();
  test_gate_engagement();
  test_force_pattern_2();
  test_force_pattern_3();
  test_flare_queue_clearing();
  test_timing_policy();
  test_10000_steps_mixed();

  std::printf("render_skip_fixture: ok\n");
  return 0;
}
