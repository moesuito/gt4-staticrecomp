#pragma once
// One standardized SDL3 gamepad reading in the XInput convention of pad_mapping.hpp. Shared by the Windows host
// (host_io.cpp) and the Linux host (native/linux/sdl_window.cpp); the SDL header stays out of this file.

#include "pad_mapping.hpp"

struct SDL_Gamepad;

gt4::pad::GamepadReading read_sdl_gamepad(SDL_Gamepad *gamepad);
