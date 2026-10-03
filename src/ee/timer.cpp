#include "gt4recomp/ee_timer.hpp"

namespace gt4recomp::ee {

void TimerUnit::map_into(GuestMemory& memory) {
    registers_.map_into(memory);
}

std::uint32_t TimerUnit::read_register(std::uint32_t address,
                                       std::size_t width) const {
    return registers_.read_register(address, width);
}

void TimerUnit::write_register(std::uint32_t address, std::size_t width,
                               std::uint32_t value) {
    registers_.write_register(address, width, value);
}

std::uint32_t TimerUnit::register_value(std::uint32_t address) const {
    return registers_.register_value(address);
}

std::vector<std::pair<std::uint32_t, std::uint32_t>>
TimerUnit::registers_snapshot() const {
    return registers_.registers_snapshot();
}

void TimerUnit::restore_registers(
    std::span<const std::pair<std::uint32_t, std::uint32_t>> entries) {
    registers_.restore_registers(entries);
}

} // namespace gt4recomp::ee
