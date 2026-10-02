#include "gt4recomp/ee_timer.hpp"

#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace gt4recomp::ee {
namespace {

std::string access_text(std::uint32_t address, std::size_t width) {
    std::ostringstream message;
    message << "Timer register access at 0x" << std::hex << std::setfill('0')
            << std::setw(8) << address << " (width " << std::dec << width
            << ") is not modeled";
    return message.str();
}

} // namespace

void TimerUnit::map_into(GuestMemory& memory) {
    memory.map_mmio(
        window_base, window_size,
        [this](std::uint32_t address, std::size_t width) {
            return read_register(address, width);
        },
        [this](std::uint32_t address, std::size_t width, std::uint32_t value) {
            write_register(address, width, value);
        });
}

std::uint32_t TimerUnit::register_value(std::uint32_t address) const {
    const auto found = registers_.find(address);
    return found == registers_.end() ? 0 : found->second;
}

std::uint32_t TimerUnit::read_register(std::uint32_t address,
                                       std::size_t width) const {
    if (width != 4) {
        throw std::runtime_error(access_text(address, width));
    }
    return register_value(address);
}

void TimerUnit::write_register(std::uint32_t address, std::size_t width,
                               std::uint32_t value) {
    if (width != 4) {
        throw std::runtime_error(access_text(address, width));
    }
    registers_[address] = value;
}

} // namespace gt4recomp::ee
