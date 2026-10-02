#pragma once

#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>

namespace gt4recomp::tools {

// Fixed-width lowercase hexadecimal for guest addresses and words.
inline std::string hex_value(std::uint32_t value, int width) {
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(width) << value;
    return output.str();
}

} // namespace gt4recomp::tools
