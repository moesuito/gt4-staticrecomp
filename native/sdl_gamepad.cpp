#include "sdl_gamepad.hpp"

#include <SDL3/SDL.h>

// TASK-074 L2: moved unchanged from host_io.cpp so the Windows host and the Linux host (native/linux/sdl_window.cpp)
// read SDL3 gamepads with one table.
// SDL_GetGamepadAxis has already normalized every mapped gamepad trigger to 0..32767,
// including DualSense. Device names and raw joystick/HID ranges must not rescale it again.
gt4::pad::GamepadReading read_sdl_gamepad(SDL_Gamepad *gamepad) {
  gt4::pad::GamepadReading reading;
  // SDL face buttons are positions: SOUTH is Cross on a PlayStation pad and A on an Xbox pad.
  const struct {
    SDL_GamepadButton button;
    std::uint16_t mask;
  } buttons[] = {
      {SDL_GAMEPAD_BUTTON_SOUTH, gt4::controls::xinput::a},
      {SDL_GAMEPAD_BUTTON_EAST, gt4::controls::xinput::b},
      {SDL_GAMEPAD_BUTTON_WEST, gt4::controls::xinput::x},
      {SDL_GAMEPAD_BUTTON_NORTH, gt4::controls::xinput::y},
      {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, gt4::controls::xinput::left_shoulder},
      {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, gt4::controls::xinput::right_shoulder},
      {SDL_GAMEPAD_BUTTON_BACK, gt4::controls::xinput::back},
      {SDL_GAMEPAD_BUTTON_START, gt4::controls::xinput::start},
      {SDL_GAMEPAD_BUTTON_LEFT_STICK, gt4::controls::xinput::left_thumb},
      {SDL_GAMEPAD_BUTTON_RIGHT_STICK, gt4::controls::xinput::right_thumb},
      {SDL_GAMEPAD_BUTTON_DPAD_UP, gt4::controls::xinput::dpad_up},
      {SDL_GAMEPAD_BUTTON_DPAD_DOWN, gt4::controls::xinput::dpad_down},
      {SDL_GAMEPAD_BUTTON_DPAD_LEFT, gt4::controls::xinput::dpad_left},
      {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, gt4::controls::xinput::dpad_right},
  };
  for (const auto &entry : buttons)
    if (SDL_GetGamepadButton(gamepad, entry.button))
      reading.buttons |= entry.mask;
  reading.left_trigger = gt4::pad::trigger_byte(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
  reading.right_trigger = gt4::pad::trigger_byte(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
  reading.thumb_lx = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX);
  reading.thumb_ly = gt4::pad::negate_axis(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY));
  reading.thumb_rx = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTX);
  reading.thumb_ry = gt4::pad::negate_axis(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTY));
  return reading;
}
