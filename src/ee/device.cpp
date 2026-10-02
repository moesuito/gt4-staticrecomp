#include "gt4recomp/ee_device.hpp"

#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace gt4recomp::ee {
namespace {

std::string access_text(const char* name, std::uint32_t address, std::size_t width) {
    std::ostringstream message;
    message << "Device register " << name << " access at 0x" << std::hex
            << std::setfill('0') << std::setw(8) << address << " (width "
            << std::dec << width << ") is not modeled";
    return message.str();
}

} // namespace

RegisterBank::RegisterBank(std::uint32_t base, std::uint32_t size)
    : base_(base), size_(size) {
    if (size == 0) {
        throw std::runtime_error("A device register bank needs a nonzero size");
    }
}

void RegisterBank::map_into(GuestMemory& memory) {
    memory.map_mmio(
        base_, size_,
        [this](std::uint32_t address, std::size_t width) {
            return read_register(address, width);
        },
        [this](std::uint32_t address, std::size_t width, std::uint32_t value) {
            write_register(address, width, value);
        });
}

std::uint32_t RegisterBank::register_value(std::uint32_t address) const {
    const auto found = registers_.find(address);
    return found == registers_.end() ? 0 : found->second;
}

std::uint32_t RegisterBank::read_register(std::uint32_t address,
                                          std::size_t width) const {
    if (width != 4) {
        throw std::runtime_error(access_text("read", address, width));
    }
    return register_value(address);
}

void RegisterBank::write_register(std::uint32_t address, std::size_t width,
                                  std::uint32_t value) {
    if (width != 4) {
        throw std::runtime_error(access_text("write", address, width));
    }
    registers_[address] = value;
}

std::uint32_t RegisterBank::base() const noexcept {
    return base_;
}

std::uint32_t RegisterBank::size() const noexcept {
    return size_;
}

} // namespace gt4recomp::ee
