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
// a write to CHCR that sets the start bit (STR, 0x100) completes the
// transfer at once, because the model has no transfer engine. The start bit
// clears, so polling code sees the transfer finish, and the completion is
// ALWAYS reported through the raise callback as the channel's DMAC cause
// (VIF0 = channel 0, VIF1 = channel 1, GIF = channel 2). The transfer
// interrupt enable (TIE, 0x80) is stored like any other CHCR bit but never
// gates the completion: per ps2autotests dmac/tagintr @97469ff (CIS status
// after termination in every TIE x tag-IRQ combination) and PCSX2's
// hwDmacIrq callers, the completion exists with TIE = 0; TIE only shapes
// tag-interrupt termination, which the chain engine of P06 will own.
// Everything else is storage.
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
    // The channel's DMAC cause (VIF0 = 0, VIF1 = 1, GIF = 2, ...), reported
    // through raise_ on every started transfer.
    std::uint32_t cause_ = 0;
    std::function<void(std::uint32_t)> raise_;
};

// The EE interrupt controller window (INTC_STAT at +0x00, INTC_MASK at
// +0x10, inside the 0x1000F000 block). Guest and internal operations stay
// separate:
//   guest write to STAT clears the named bits (W1C, PCSX2 HwWrite.cpp
//     @81526d4: psHu32(INTC_STAT) &= ~value);
//   guest write to MASK toggles the named low 16 bits (PCSX2 HwWrite.cpp
//     @81526d4: psHu32(INTC_MASK) ^= (u16)value);
//   internal set/enable OR bits; internal disable clears a mask bit.
// A restore writes storage directly and never toggles or acknowledges.
// Pending (STAT) exists whether or not a handler is registered or the mask
// allows delivery; choosing whether to call a handler is the dispatch step.
class IntcUnit {
public:
    static constexpr std::uint32_t window_base = 0x1000F000;
    static constexpr std::uint32_t window_size = 0x100;
    static constexpr std::uint32_t stat_offset = 0x00;
    static constexpr std::uint32_t mask_offset = 0x10;
    static constexpr std::uint32_t mask_write_bits = 0x0000FFFF;

    IntcUnit() = default;

    // Routes the window of the memory to this unit. The unit must outlive
    // the memory (the callbacks capture it).
    void map_into(GuestMemory& memory);

    [[nodiscard]] std::uint32_t read_register(std::uint32_t address,
                                              std::size_t width) const;
    void write_register(std::uint32_t address, std::size_t width,
                        std::uint32_t value);

    // The stored value of one register; zero when never written.
    [[nodiscard]] std::uint32_t register_value(std::uint32_t address) const;

    // Snapshot/restore pair. Restoring replaces the state wholesale,
    // bypassing the guest W1C/toggle path (decision 0028).
    [[nodiscard]] std::vector<std::pair<std::uint32_t, std::uint32_t>>
    registers_snapshot() const;
    void restore_registers(
        std::span<const std::pair<std::uint32_t, std::uint32_t>> entries);

    // Device-internal operations (occurrence, privileged enable/disable).
    void set_pending_internal(std::uint32_t cause);
    void enable_internal(std::uint32_t cause);
    void disable_internal(std::uint32_t cause);
    [[nodiscard]] bool is_pending(std::uint32_t cause) const noexcept;
    [[nodiscard]] bool mask_allows(std::uint32_t cause) const noexcept;

    [[nodiscard]] std::uint32_t base() const noexcept;
    [[nodiscard]] std::uint32_t size() const noexcept;

private:
    std::uint32_t stat_ = 0;
    std::uint32_t mask_ = 0;
    std::map<std::uint32_t, std::uint32_t> rest_;
};

// The DMAC status window (D_STAT at +0x10 inside the 0x1000E000 block).
// The low 16 bits are the per-channel completion status (CIS), the high 16
// are the per-channel mask (CIM):
//   guest write clears CIS bits named with 1 (W1C) and toggles the named
//     CIM bits (PCSX2 Dmac.cpp @81526d4: cis &= ~(value & 0xffff),
//     cim ^= (value >> 16));
//   internal completion ORs a CIS bit; internal enable/disable set/clear a
//     CIM bit.
// A restore writes storage directly. A completion stays pending whether or
// not a handler is registered; the mask decides delivery, never existence.
// The global DMAE/D_ENABLER gates that PCSX2 checks in dmacInterrupt are
// NOT modeled: nothing in the verified boot path programs them yet, so
// requiring them would refuse completions the game demonstrably consumes.
// That gate is P06 territory and stays an explicit limit here.
class DmacStatusUnit {
public:
    static constexpr std::uint32_t window_base = 0x1000E000;
    static constexpr std::uint32_t window_size = 0x100;
    static constexpr std::uint32_t stat_offset = 0x10;
    static constexpr std::uint32_t status_bits = 0x0000FFFF;

    DmacStatusUnit() = default;

    // Routes the window of the memory to this unit. The unit must outlive
    // the memory (the callbacks capture it).
    void map_into(GuestMemory& memory);

    [[nodiscard]] std::uint32_t read_register(std::uint32_t address,
                                              std::size_t width) const;
    void write_register(std::uint32_t address, std::size_t width,
                        std::uint32_t value);

    // The stored value of one register; zero when never written.
    [[nodiscard]] std::uint32_t register_value(std::uint32_t address) const;

    // Snapshot/restore pair. Restoring replaces the state wholesale,
    // bypassing the guest W1C/toggle path (decision 0028).
    [[nodiscard]] std::vector<std::pair<std::uint32_t, std::uint32_t>>
    registers_snapshot() const;
    void restore_registers(
        std::span<const std::pair<std::uint32_t, std::uint32_t>> entries);

    // Device-internal operations (completion, privileged enable/disable).
    void set_completion_internal(std::uint32_t channel);
    void enable_internal(std::uint32_t channel);
    void disable_internal(std::uint32_t channel);
    [[nodiscard]] bool completion_pending(std::uint32_t channel) const noexcept;
    [[nodiscard]] bool mask_allows(std::uint32_t channel) const noexcept;

    [[nodiscard]] std::uint32_t base() const noexcept;
    [[nodiscard]] std::uint32_t size() const noexcept;

private:
    std::uint32_t completion_status_ = 0;  // CIS, low 16 bits
    std::uint32_t completion_mask_ = 0;    // CIM, low 16 bits
    std::map<std::uint32_t, std::uint32_t> rest_;
};

} // namespace gt4recomp::ee
