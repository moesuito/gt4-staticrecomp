// TASK-074 (Linux): the game's main. The same steps as the non-standalone path of native/game_main.cpp, which stays the
// Windows main: the patches that read their switches once (TASK-082 frame skip, TASK-159 360-degree camera, TASK-160
// field of view, TASK-161 full trigger range) are configured before the game starts, so an invalid value stops the start
// as it does on Windows, then the donor's main (the port's main.cpp, compiled as gt4_donor_main) runs the game.
// Not here yet: game_main.cpp's standalone start (game.ini beside the executable, the launcher's folders and card lock),
// which belongs with the Linux launcher and packaging (TASK-074 step L4).

#include "camera_360.hpp"
#include "controls.hpp"
#include "fov_adjust.hpp"
#include "frame_skip.hpp"
#include "trigger_range.hpp"
#include "window.hpp"

#ifdef GT4_GS_STREAM_CLIENT
#include "gs_stream/gs_stream_client.hpp"
#include "ps2/memory.hpp"
#endif

#include <cstring>

int gt4_donor_main(int argc, char **argv);

namespace {

// TASK-161: one switch drives both sides of the full trigger range (as game_main.cpp configure_trigger_range).
void configure_trigger_range() {
  gt4::trigger_range::configure_from_environment();
  gt4::controls::set_pressure_scale(gt4::trigger_range::enabled() ? gt4::controls::PressureScale::full
                                                                  : gt4::controls::PressureScale::stock);
}

} // namespace

int main(int argc, char **argv) {
#ifdef GT4_GS_STREAM_CLIENT
  // TASK-116 step 0: the guest RAM window of the GT4_GS_READBACK_LOG lines (nothing happens when the switch is unset).
  gt4::gs_stream::set_guest_ram(ps2::g_ram, ps2::kRamSize);
#endif
  bool window = false;
  for (int i = 1; i < argc; ++i)
    window = window || std::strcmp(argv[i], "--window") == 0 || std::strcmp(argv[i], "--fullscreen") == 0;
  gt4::frame_skip::start_from_environment();
  gt4::camera360::configure_from_environment();  // an invalid GT4_CAMERA_360 stops the start here
  gt4::fov_adjust::configure_from_environment();  // an invalid GT4_FOV_ADJUST stops the start here
  configure_trigger_range();                       // an invalid GT4_TRIGGER_FULL_RANGE stops the start here
  if (!window && !install_input_replay_without_window()) return 1;
  return gt4_donor_main(argc, argv);
}
