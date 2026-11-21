#pragma once
// TASK-074 (Linux): the refresh rate of the display that shows the game window, in Hz (0 = not known). The Linux window host
// (native/linux/sdl_window.cpp) writes it from SDL3 at start and when the window moves to another display; the CoreGS
// performance HUD reads it (native/hw_gs/hud/hw_hud_system_linux.cpp). On Windows the HUD asks the window's monitor itself.

#include <atomic>
#include <cstdint>

namespace gt4::display {

inline std::atomic<std::uint32_t> g_refresh_hz{0};

} // namespace gt4::display
