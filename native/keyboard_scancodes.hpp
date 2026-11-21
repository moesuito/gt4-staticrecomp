#pragma once
// TASK-074 L2: the keyboard keys of the controls table (Win32 virtual-key codes, controls.cpp) as USB HID keyboard
// usage IDs (HID Usage Tables, page 0x07), which are the values of SDL_Scancode. The Linux host reads SDL's keyboard
// state with them. Pure: no SDL or Windows header, so keyboard_scancodes_fixture runs on any host.

#include <array>
#include <cstdint>

namespace gt4::keyboard {

// Up to two scancodes per key: Shift, Ctrl and Alt are one virtual key on Windows and a left and a right key in HID.
// Unused slots are 0. A virtual key the controls table cannot name gives {0, 0}.
std::array<std::uint16_t, 2> scancodes_for_virtual_key(std::uint8_t virtual_key);

} // namespace gt4::keyboard
