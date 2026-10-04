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

std::vector<std::pair<std::uint32_t, std::uint32_t>>
RegisterBank::registers_snapshot() const {
    return {registers_.begin(), registers_.end()};
}

void RegisterBank::restore_registers(
    std::span<const std::pair<std::uint32_t, std::uint32_t>> entries) {
    // Straight into storage, never through write_register: a restore is a
    // verbatim photo, so future guest-write effects (flag acknowledge,
    // masks, completions) must not run here. The device contract keeps its
    // write path; only this restore path bypasses it (decision 0028).
    registers_.clear();
    for (const auto& [address, value] : entries) {
        registers_[address] = value;
    }
}

DmaChannel::DmaChannel(std::uint32_t base, std::uint32_t size,
                       std::uint32_t cause,
                       std::function<void(std::uint32_t)> raise)
    : bank_(base, size), base_(base), cause_(cause), raise_(std::move(raise)) {
    if (!raise_) {
        throw std::runtime_error("A DMA channel needs a completion callback");
    }
}

void DmaChannel::map_into(GuestMemory& memory) {
    memory.map_mmio(
        base_, bank_.size(),
        [this](std::uint32_t address, std::size_t width) {
            return read_register(address, width);
        },
        [this](std::uint32_t address, std::size_t width, std::uint32_t value) {
            write_register(address, width, value);
        });
}

std::uint32_t DmaChannel::register_value(std::uint32_t address) const {
    return bank_.register_value(address);
}

std::uint32_t DmaChannel::base() const noexcept {
    return base_;
}

std::uint32_t DmaChannel::size() const noexcept {
    return bank_.size();
}

std::vector<std::pair<std::uint32_t, std::uint32_t>>
DmaChannel::registers_snapshot() const {
    return bank_.registers_snapshot();
}

void DmaChannel::restore_registers(
    std::span<const std::pair<std::uint32_t, std::uint32_t>> entries) {
    // Straight into the bank's storage: a live write would complete a
    // transfer whose start bit is set, which a restore must never fire.
    bank_.restore_registers(entries);
}

std::uint32_t DmaChannel::read_register(std::uint32_t address,
                                        std::size_t width) const {
    return bank_.read_register(address, width);
}

void DmaChannel::write_register(std::uint32_t address, std::size_t width,
                                std::uint32_t value) {
    if (address == base_ + chcr_offset && width == 4
        && (value & start_bit) != 0) {
        // The model has no transfer engine: a started transfer completes at
        // once. The start bit clears so polling code sees it finish; the
        // transfer interrupt, when enabled, reports the channel's cause.
        bank_.write_register(address, width, value & ~start_bit);
        if ((value & interrupt_enable) != 0) {
            raise_(cause_);
        }
        return;
    }
    bank_.write_register(address, width, value);
}

} // namespace gt4recomp::ee
