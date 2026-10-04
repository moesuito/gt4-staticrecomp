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
        // once. The start bit clears so polling code sees it finish, and the
        // completion is always reported as the channel's DMAC cause. TIE is
        // stored but never gates the report (see the header): it shapes tag
        // termination, which the P06 chain engine will own.
        bank_.write_register(address, width, value & ~start_bit);
        raise_(cause_);
        return;
    }
    bank_.write_register(address, width, value);
}

void IntcUnit::map_into(GuestMemory& memory) {
    memory.map_mmio(
        window_base, window_size,
        [this](std::uint32_t address, std::size_t width) {
            return read_register(address, width);
        },
        [this](std::uint32_t address, std::size_t width, std::uint32_t value) {
            write_register(address, width, value);
        });
}

std::uint32_t IntcUnit::register_value(std::uint32_t address) const {
    if (address == window_base + stat_offset) {
        return stat_;
    }
    if (address == window_base + mask_offset) {
        return mask_;
    }
    const auto found = rest_.find(address);
    return found == rest_.end() ? 0 : found->second;
}

std::uint32_t IntcUnit::read_register(std::uint32_t address,
                                      std::size_t width) const {
    if (width != 4) {
        throw std::runtime_error(access_text("read", address, width));
    }
    return register_value(address);
}

void IntcUnit::write_register(std::uint32_t address, std::size_t width,
                              std::uint32_t value) {
    if (width != 4) {
        throw std::runtime_error(access_text("write", address, width));
    }
    if (address == window_base + stat_offset) {
        // Write-1-to-clear: acknowledging one cause leaves the others.
        stat_ &= ~value;
        return;
    }
    if (address == window_base + mask_offset) {
        // The mask register toggles the named low 16 bits.
        mask_ ^= (value & mask_write_bits);
        return;
    }
    rest_[address] = value;
}

std::vector<std::pair<std::uint32_t, std::uint32_t>>
IntcUnit::registers_snapshot() const {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> out;
    out.emplace_back(window_base + stat_offset, stat_);
    out.emplace_back(window_base + mask_offset, mask_);
    for (const auto& entry : rest_) {
        out.emplace_back(entry);
    }
    return out;
}

void IntcUnit::restore_registers(
    std::span<const std::pair<std::uint32_t, std::uint32_t>> entries) {
    // Straight into storage, never through write_register: a restore must
    // not acknowledge flags nor toggle masks.
    stat_ = 0;
    mask_ = 0;
    rest_.clear();
    for (const auto& [address, value] : entries) {
        if (address == window_base + stat_offset) {
            stat_ = value;
        } else if (address == window_base + mask_offset) {
            mask_ = value;
        } else {
            rest_[address] = value;
        }
    }
}

void IntcUnit::set_pending_internal(std::uint32_t cause) {
    if (cause < 32) {
        stat_ |= (1u << cause);
    }
}

void IntcUnit::enable_internal(std::uint32_t cause) {
    if (cause < 32) {
        mask_ |= (1u << cause);
    }
}

void IntcUnit::disable_internal(std::uint32_t cause) {
    if (cause < 32) {
        mask_ &= ~(1u << cause);
    }
}

bool IntcUnit::is_pending(std::uint32_t cause) const noexcept {
    return cause < 32 && (stat_ & (1u << cause)) != 0;
}

bool IntcUnit::mask_allows(std::uint32_t cause) const noexcept {
    return cause < 32 && (mask_ & (1u << cause)) != 0;
}

std::uint32_t IntcUnit::base() const noexcept {
    return window_base;
}

std::uint32_t IntcUnit::size() const noexcept {
    return window_size;
}

void DmacStatusUnit::map_into(GuestMemory& memory) {
    memory.map_mmio(
        window_base, window_size,
        [this](std::uint32_t address, std::size_t width) {
            return read_register(address, width);
        },
        [this](std::uint32_t address, std::size_t width, std::uint32_t value) {
            write_register(address, width, value);
        });
}

std::uint32_t DmacStatusUnit::register_value(std::uint32_t address) const {
    if (address == window_base + stat_offset) {
        return (completion_mask_ << 16) | completion_status_;
    }
    const auto found = rest_.find(address);
    return found == rest_.end() ? 0 : found->second;
}

std::uint32_t DmacStatusUnit::read_register(std::uint32_t address,
                                            std::size_t width) const {
    if (width != 4) {
        throw std::runtime_error(access_text("read", address, width));
    }
    return register_value(address);
}

void DmacStatusUnit::write_register(std::uint32_t address, std::size_t width,
                                    std::uint32_t value) {
    if (width != 4) {
        throw std::runtime_error(access_text("write", address, width));
    }
    if (address == window_base + stat_offset) {
        // Low half W1C (acknowledge completions one by one), high half
        // toggle (mask bits flip where the value names 1).
        completion_status_ &= ~(value & status_bits);
        completion_mask_ ^= (value >> 16) & status_bits;
        return;
    }
    rest_[address] = value;
}

std::vector<std::pair<std::uint32_t, std::uint32_t>>
DmacStatusUnit::registers_snapshot() const {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> out;
    out.emplace_back(window_base + stat_offset,
                     (completion_mask_ << 16) | completion_status_);
    for (const auto& entry : rest_) {
        out.emplace_back(entry);
    }
    return out;
}

void DmacStatusUnit::restore_registers(
    std::span<const std::pair<std::uint32_t, std::uint32_t>> entries) {
    // Straight into storage, never through write_register: a restore must
    // not acknowledge completions nor toggle masks.
    completion_status_ = 0;
    completion_mask_ = 0;
    rest_.clear();
    for (const auto& [address, value] : entries) {
        if (address == window_base + stat_offset) {
            completion_status_ = value & status_bits;
            completion_mask_ = (value >> 16) & status_bits;
        } else {
            rest_[address] = value;
        }
    }
}

void DmacStatusUnit::set_completion_internal(std::uint32_t channel) {
    if (channel < 16) {
        completion_status_ |= (1u << channel);
    }
}

void DmacStatusUnit::enable_internal(std::uint32_t channel) {
    if (channel < 16) {
        completion_mask_ |= (1u << channel);
    }
}

void DmacStatusUnit::disable_internal(std::uint32_t channel) {
    if (channel < 16) {
        completion_mask_ &= ~(1u << channel);
    }
}

bool DmacStatusUnit::completion_pending(std::uint32_t channel) const noexcept {
    return channel < 16 && (completion_status_ & (1u << channel)) != 0;
}

bool DmacStatusUnit::mask_allows(std::uint32_t channel) const noexcept {
    return channel < 16 && (completion_mask_ & (1u << channel)) != 0;
}

std::uint32_t DmacStatusUnit::base() const noexcept {
    return window_base;
}

std::uint32_t DmacStatusUnit::size() const noexcept {
    return window_size;
}

} // namespace gt4recomp::ee
