//@section includes
#include "fps_probe.hpp"
#include "render_skip.hpp"
//@section helpers
// TASK-082: frame loop observation and controlled render skip.
// Behind GT4_FPS_PROBE=1 and GT4_NATIVE_FPS=skip (default off).
// Disabled fast path: single branch predicts false, zero overhead.
namespace {

bool fps_probe_dispatch_hook(Cpu &c, std::uint32_t pc) {
  bool bypass = false;
  if (gt4::render_skip::SkipManager::shared().enabled()) {
    bypass = gt4::render_skip::on_dispatch(c, pc, g_ram, kRamSize);
  }
  if (gt4::fps_probe::Probe::shared().enabled()) {
    const bool probe_bypass = gt4::fps_probe::on_dispatch(c, pc, g_ram, kRamSize);
    if (!bypass) bypass = probe_bypass;
  }
  return bypass;
}

}  // namespace
//@section hook
    if (gt4::render_skip::SkipManager::shared().enabled() || gt4::fps_probe::Probe::shared().enabled()) [[unlikely]] {
      if (fps_probe_dispatch_hook(c, pc)) {
        c.pc = c.r[31].w[0];
        return;
      }
    }
