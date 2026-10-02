#pragma once

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace gt4recomp::tools {

// Parse a decimal or 0x-prefixed hexadecimal 32-bit guest value. Rejects
// signs, whitespace, trailing characters and values above 32 bits.
inline std::uint32_t parse_number(std::wstring_view text) {
    std::uint32_t base = 10;
    if (text.starts_with(L"0x") || text.starts_with(L"0X")) {
        base = 16;
        text.remove_prefix(2);
    }
    if (text.empty()) {
        throw std::runtime_error("Expected decimal or 0x-prefixed hexadecimal number");
    }
    std::uint32_t result = 0;
    for (const wchar_t character : text) {
        std::uint32_t digit = 0;
        if (character >= L'0' && character <= L'9') {
            digit = character - L'0';
        } else if (character >= L'a' && character <= L'f') {
            digit = character - L'a' + 10;
        } else if (character >= L'A' && character <= L'F') {
            digit = character - L'A' + 10;
        } else {
            throw std::runtime_error("Invalid character in numeric argument");
        }
        if (digit >= base || result > (std::numeric_limits<std::uint32_t>::max() - digit) / base) {
            throw std::runtime_error("Numeric argument is invalid or exceeds 32 bits");
        }
        result = result * base + digit;
    }
    return result;
}

} // namespace gt4recomp::tools
