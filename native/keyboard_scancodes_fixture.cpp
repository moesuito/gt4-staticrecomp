// CTest native_keyboard_scancodes (TASK-074 L2): every key the controls table can name has a HID scancode, checked
// against SDL3's own SDL_Scancode values, and the built-in keyboard map resolves completely.

#include "controls.hpp"
#include "keyboard_scancodes.hpp"

#include <SDL3/SDL_scancode.h>

#include <cstdio>
#include <string>

namespace {

int failures = 0;

void expect(bool condition, const std::string &what) {
  if (!condition) {
    std::fprintf(stderr, "[keyboard-scancodes] FAIL: %s\n", what.c_str());
    ++failures;
  }
}

void expect_key(std::uint8_t virtual_key, SDL_Scancode first, SDL_Scancode second, const std::string &name) {
  const auto codes = gt4::keyboard::scancodes_for_virtual_key(virtual_key);
  expect(codes[0] == first && codes[1] == second, name);
}

} // namespace

int main() {
  for (char letter = 'A'; letter <= 'Z'; ++letter)
    expect_key(static_cast<std::uint8_t>(letter), static_cast<SDL_Scancode>(SDL_SCANCODE_A + (letter - 'A')),
               SDL_SCANCODE_UNKNOWN, std::string("letter ") + letter);
  expect_key('0', SDL_SCANCODE_0, SDL_SCANCODE_UNKNOWN, "digit 0");
  expect_key('1', SDL_SCANCODE_1, SDL_SCANCODE_UNKNOWN, "digit 1");
  expect_key('9', SDL_SCANCODE_9, SDL_SCANCODE_UNKNOWN, "digit 9");
  expect_key(0x70, SDL_SCANCODE_F1, SDL_SCANCODE_UNKNOWN, "F1");
  expect_key(0x7B, SDL_SCANCODE_F12, SDL_SCANCODE_UNKNOWN, "F12");
  expect_key(0x60, SDL_SCANCODE_KP_0, SDL_SCANCODE_UNKNOWN, "num0");
  expect_key(0x61, SDL_SCANCODE_KP_1, SDL_SCANCODE_UNKNOWN, "num1");
  expect_key(0x69, SDL_SCANCODE_KP_9, SDL_SCANCODE_UNKNOWN, "num9");
  expect_key(0x0D, SDL_SCANCODE_RETURN, SDL_SCANCODE_UNKNOWN, "enter");
  expect_key(0x08, SDL_SCANCODE_BACKSPACE, SDL_SCANCODE_UNKNOWN, "backspace");
  expect_key(0x09, SDL_SCANCODE_TAB, SDL_SCANCODE_UNKNOWN, "tab");
  expect_key(0x20, SDL_SCANCODE_SPACE, SDL_SCANCODE_UNKNOWN, "space");
  expect_key(0x1B, SDL_SCANCODE_ESCAPE, SDL_SCANCODE_UNKNOWN, "esc");
  expect_key(0x10, SDL_SCANCODE_LSHIFT, SDL_SCANCODE_RSHIFT, "shift: both sides");
  expect_key(0x11, SDL_SCANCODE_LCTRL, SDL_SCANCODE_RCTRL, "ctrl: both sides");
  expect_key(0x12, SDL_SCANCODE_LALT, SDL_SCANCODE_RALT, "alt: both sides");
  expect_key(0x26, SDL_SCANCODE_UP, SDL_SCANCODE_UNKNOWN, "up");
  expect_key(0x28, SDL_SCANCODE_DOWN, SDL_SCANCODE_UNKNOWN, "down");
  expect_key(0x25, SDL_SCANCODE_LEFT, SDL_SCANCODE_UNKNOWN, "left");
  expect_key(0x27, SDL_SCANCODE_RIGHT, SDL_SCANCODE_UNKNOWN, "right");
  expect_key(0x2D, SDL_SCANCODE_INSERT, SDL_SCANCODE_UNKNOWN, "insert");
  expect_key(0x2E, SDL_SCANCODE_DELETE, SDL_SCANCODE_UNKNOWN, "delete");
  expect_key(0x24, SDL_SCANCODE_HOME, SDL_SCANCODE_UNKNOWN, "home");
  expect_key(0x23, SDL_SCANCODE_END, SDL_SCANCODE_UNKNOWN, "end");
  expect_key(0x21, SDL_SCANCODE_PAGEUP, SDL_SCANCODE_UNKNOWN, "pageup");
  expect_key(0x22, SDL_SCANCODE_PAGEDOWN, SDL_SCANCODE_UNKNOWN, "pagedown");
  expect_key(0x00, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, "no key");
  expect_key(0x91, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, "a key the table cannot name");

  // Every key the parser accepts, through the real parser.
  const char *names[] = {"a", "z", "0", "9", "f1", "f12", "num0", "num9", "enter", "backspace", "tab", "space", "shift",
                         "ctrl", "control", "alt", "esc", "up", "down", "left", "right", "insert", "delete", "home",
                         "end", "pageup", "pagedown"};
  for (const char *name : names) {
    const auto map =
        gt4::controls::parse_controls(std::string("schema = 1\n\n[bindings]\ncross = [\"key:") + name + "\"]\n", "fixture");
    const auto bindings = gt4::controls::keyboard_bindings(map);
    bool resolved = !bindings.empty();
    for (const auto &binding : bindings)
      resolved = resolved && gt4::keyboard::scancodes_for_virtual_key(binding.virtual_key)[0] != 0;
    expect(resolved, std::string("parsed key '") + name + "' has a scancode");
  }
  // The built-in keyboard layout (README controls table) resolves completely.
  for (const auto &binding : gt4::controls::keyboard_bindings(gt4::controls::default_controls()))
    expect(gt4::keyboard::scancodes_for_virtual_key(binding.virtual_key)[0] != 0,
           "default binding vk " + std::to_string(binding.virtual_key));

  if (failures) {
    std::fprintf(stderr, "[keyboard-scancodes] %d failure(s)\n", failures);
    return 1;
  }
  std::fprintf(stderr, "[keyboard-scancodes] PASS\n");
  return 0;
}
