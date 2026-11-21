#include "keyboard_scancodes.hpp"

namespace gt4::keyboard {
namespace {

// HID keyboard usage IDs (USB HID Usage Tables 1.5, section 10, page 0x07).
constexpr std::uint16_t hid_a = 0x04, hid_1 = 0x1E, hid_0 = 0x27, hid_enter = 0x28, hid_escape = 0x29,
                        hid_backspace = 0x2A, hid_tab = 0x2B, hid_space = 0x2C, hid_f1 = 0x3A, hid_insert = 0x49,
                        hid_home = 0x4A, hid_page_up = 0x4B, hid_delete = 0x4C, hid_end = 0x4D, hid_page_down = 0x4E,
                        hid_right = 0x4F, hid_left = 0x50, hid_down = 0x51, hid_up = 0x52, hid_keypad_1 = 0x59,
                        hid_keypad_0 = 0x62, hid_left_control = 0xE0, hid_left_shift = 0xE1, hid_left_alt = 0xE2,
                        hid_right_control = 0xE4, hid_right_shift = 0xE5, hid_right_alt = 0xE6;

// Win32 virtual-key codes, the same values as controls.cpp.
constexpr std::uint8_t vk_back = 0x08, vk_tab = 0x09, vk_return = 0x0D, vk_shift = 0x10, vk_control = 0x11,
                       vk_menu = 0x12, vk_escape = 0x1B, vk_space = 0x20, vk_prior = 0x21, vk_next = 0x22,
                       vk_end = 0x23, vk_home = 0x24, vk_left = 0x25, vk_up = 0x26, vk_right = 0x27, vk_down = 0x28,
                       vk_insert = 0x2D, vk_delete = 0x2E, vk_numpad0 = 0x60, vk_f1 = 0x70;

} // namespace

std::array<std::uint16_t, 2> scancodes_for_virtual_key(std::uint8_t virtual_key) {
  if (virtual_key >= 'A' && virtual_key <= 'Z')
    return {static_cast<std::uint16_t>(hid_a + (virtual_key - 'A')), 0};
  // HID orders the digit row 1..9 then 0; Windows orders 0..9.
  if (virtual_key == '0')
    return {hid_0, 0};
  if (virtual_key >= '1' && virtual_key <= '9')
    return {static_cast<std::uint16_t>(hid_1 + (virtual_key - '1')), 0};
  if (virtual_key >= vk_f1 && virtual_key < vk_f1 + 12)
    return {static_cast<std::uint16_t>(hid_f1 + (virtual_key - vk_f1)), 0};
  // The keypad has the same 1..9 then 0 order as the digit row.
  if (virtual_key == vk_numpad0)
    return {hid_keypad_0, 0};
  if (virtual_key > vk_numpad0 && virtual_key <= vk_numpad0 + 9)
    return {static_cast<std::uint16_t>(hid_keypad_1 + (virtual_key - vk_numpad0 - 1)), 0};
  switch (virtual_key) {
  case vk_return: return {hid_enter, 0};
  case vk_back: return {hid_backspace, 0};
  case vk_tab: return {hid_tab, 0};
  case vk_space: return {hid_space, 0};
  case vk_escape: return {hid_escape, 0};
  case vk_shift: return {hid_left_shift, hid_right_shift};
  case vk_control: return {hid_left_control, hid_right_control};
  case vk_menu: return {hid_left_alt, hid_right_alt};
  case vk_up: return {hid_up, 0};
  case vk_down: return {hid_down, 0};
  case vk_left: return {hid_left, 0};
  case vk_right: return {hid_right, 0};
  case vk_insert: return {hid_insert, 0};
  case vk_delete: return {hid_delete, 0};
  case vk_home: return {hid_home, 0};
  case vk_end: return {hid_end, 0};
  case vk_prior: return {hid_page_up, 0};
  case vk_next: return {hid_page_down, 0};
  default: return {0, 0};
  }
}

} // namespace gt4::keyboard
