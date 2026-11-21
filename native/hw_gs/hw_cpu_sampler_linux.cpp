// TASK-074 (Linux): the hw_cpu_sampler.hpp interface without the Windows profiler of hw_cpu_sampler.cpp (it suspends
// the game's threads and reads their instruction pointers with Win32 thread APIs). The TASK-027 CPU survey is a
// diagnostic, so on Linux the sampler stays off and says so when GT4_CPU_SAMPLER asks for it.
#include "hw_cpu_sampler.hpp"

#include <cstdlib>

namespace gt4::hw::sampler {

void name_current_thread(const char *) {}

bool start_from_env() {
  if (std::getenv("GT4_CPU_SAMPLER") != nullptr)
    std::fprintf(stderr, "[sampler] GT4_CPU_SAMPLER: the CPU sampler is Windows-only; it stays off on Linux\n");
  return false;
}

void note_vblank(std::uint64_t) {}

void finish(std::FILE *) {}

}  // namespace gt4::hw::sampler
