//@section includes
#include "fps_probe.hpp"
//@section helpers
// TASK-082 P0: frame loop observation and dependency proof (native/fps_probe.hpp).
// Behind GT4_FPS_PROBE=1 (default off). Disabled fast path: Probe::shared().enabled() is false,
// a single branch predicts false, zero overhead.
namespace {

bool fps_probe_dispatch_hook(Cpu &c, std::uint32_t pc) {
  return gt4::fps_probe::on_dispatch(c, pc, g_ram, kRamSize);
}

}  // namespace
//@section hook
    if (gt4::fps_probe::Probe::shared().enabled()) [[unlikely]] {
      if (fps_probe_dispatch_hook(c, pc)) {
        c.pc = c.r[31].w[0];
        return;
      }
    }
