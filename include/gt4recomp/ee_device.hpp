#pragma once

// A plain device register bank: 32-bit storage for one hardware register
// window. Reads return the last written value and untouched registers read
// as zero. No side effects, no ticking, no interrupts — the honest default
// for device blocks the verified paths have not needed to drive yet.
//
// The timer unit keeps its own typed class on top of the same storage; DMAC
// and SIF use a bank directly. Only 32-bit accesses are modeled; any other
// width stops with the address instead of guessing a byte order.

#include "gt4recomp/ee_state.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <utility>
#include <vector>

namespace gt4recomp::ee {

class RegisterBank {
public:
    RegisterBank(std::uint32_t base, std::uint32_t size);

    // Routes the bank's window of the memory to this bank. The bank must
    // outlive the memory (the callbacks capture it).
    void map_into(GuestMemory& memory);

    [[nodiscard]] std::uint32_t read_register(std::uint32_t address,
                                              std::size_t width) const;
    void write_register(std::uint32_t address, std::size_t width,
                        std::uint32_t value);

    // The stored value of one register; zero when never written.
    [[nodiscard]] std::uint32_t register_value(std::uint32_t address) const;

    // A copy of every stored register (address/value, ordered) for
    // snapshots. Restoring replaces the stored registers wholesale,
    // writing storage directly and bypassing the guest write path: no
    // flag acknowledge, mask or completion effect ever runs on restore.
    // MMIO routing is untouched.
    [[nodiscard]] std::vector<std::pair<std::uint32_t, std::uint32_t>>
    registers_snapshot() const;
    void restore_registers(
        std::span<const std::pair<std::uint32_t, std::uint32_t>> entries);

    [[nodiscard]] std::uint32_t base() const noexcept;
    [[nodiscard]] std::uint32_t size() const noexcept;

private:
    std::uint32_t base_ = 0;
    std::uint32_t size_ = 0;
    std::map<std::uint32_t, std::uint32_t> registers_;
};

// A DMA channel control window (VIF0, VIF1, GIF, ...). Like RegisterBank it
// stores 32-bit writes and returns them on reads, with one modeled behavior:
// a write to CHCR that sets the start bit (STR, 0x100) "completes" the
// transfer at once, because the model has no transfer engine. The start bit
// clears, so polling code sees the transfer finish, and when the transfer
// interrupt is enabled (TIE, 0x80) the channel's interrupt cause is reported
// through the raise callback. Everything else is storage.
class DmaChannel {
public:
    static constexpr std::uint32_t chcr_offset = 0x00;
    static constexpr std::uint32_t interrupt_enable = 0x00000080;  // TIE
    static constexpr std::uint32_t start_bit = 0x00000100;         // STR

    DmaChannel(std::uint32_t base, std::uint32_t size, std::uint32_t cause,
               std::function<void(std::uint32_t)> raise);

    // Routes the channel's window of the memory to this channel. The
    // channel must outlive the memory (the callbacks capture it).
    void map_into(GuestMemory& memory);

    [[nodiscard]] std::uint32_t register_value(std::uint32_t address) const;
    [[nodiscard]] std::uint32_t base() const noexcept;
    [[nodiscard]] std::uint32_t size() const noexcept;

    // Snapshot passthrough to the channel's bank. Restoring never fires a
    // completion: it writes the bank's storage directly, bypassing the
    // start-bit behavior of a live write.
    [[nodiscard]] std::vector<std::pair<std::uint32_t, std::uint32_t>>
    registers_snapshot() const;
    void restore_registers(
        std::span<const std::pair<std::uint32_t, std::uint32_t>> entries);

private:
    [[nodiscard]] std::uint32_t read_register(std::uint32_t address,
                                              std::size_t width) const;
    void write_register(std::uint32_t address, std::size_t width,
                        std::uint32_t value);

    RegisterBank bank_;
    std::uint32_t base_ = 0;
    std::uint32_t cause_ = 0;
    std::function<void(std::uint32_t)> raise_;
};

} // namespace gt4recomp::ee
