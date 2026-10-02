#pragma once

// Explicit guest state for the R5900: a 32-entry 64-bit register file with the
// CPU's 32-bit sign-extension rule, and a byte-addressable little-endian
// memory region. Nothing here executes guest code; the state model defines
// what a value means and what counts as an invalid access.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace gt4recomp::ee {

// One contiguous region of the guest address space. Loads and stores require
// natural alignment for their width and must lie entirely inside the region;
// anything else throws std::runtime_error naming the address instead of
// silently wrapping or touching host memory.
class GuestMemory {
public:
    // size_bytes must be nonzero, and base + size_bytes must fit the 32-bit
    // guest address space.
    GuestMemory(std::uint32_t base, std::size_t size_bytes);

    [[nodiscard]] std::uint32_t base() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool contains(std::uint32_t address, std::size_t width) const noexcept;

    [[nodiscard]] std::uint8_t read_byte(std::uint32_t address) const;
    [[nodiscard]] std::uint16_t read_halfword(std::uint32_t address) const;
    [[nodiscard]] std::uint32_t read_word(std::uint32_t address) const;
    [[nodiscard]] std::uint64_t read_doubleword(std::uint32_t address) const;
    void write_byte(std::uint32_t address, std::uint8_t value);
    void write_halfword(std::uint32_t address, std::uint16_t value);
    void write_word(std::uint32_t address, std::uint32_t value);
    void write_doubleword(std::uint32_t address, std::uint64_t value);

    // Bulk copy for loading images and test fixtures. Byte granularity: no
    // alignment requirement beyond the region bounds. An empty source is a no-op.
    void write_bytes(std::uint32_t address, std::span<const std::uint8_t> source);

private:
    void require_alignment(std::uint32_t address, std::size_t width) const;
    [[nodiscard]] std::size_t range_offset(std::uint32_t address, std::size_t width) const;

    std::uint32_t base_ = 0;
    std::vector<std::uint8_t> bytes_;
};

// The register file and program counter. R0 reads as zero and ignores writes;
// 32-bit writes sign-extend into the 64-bit register, matching the CPU's rule
// for all 32-bit results.
class GuestState {
public:
    explicit GuestState(GuestMemory memory);

    [[nodiscard]] std::uint64_t read_gpr64(std::uint8_t index) const;
    void write_gpr64(std::uint8_t index, std::uint64_t value);
    [[nodiscard]] std::uint32_t read_gpr32(std::uint8_t index) const;
    void write_gpr32(std::uint8_t index, std::uint32_t value);

    [[nodiscard]] std::uint32_t pc() const noexcept;
    void set_pc(std::uint32_t value) noexcept;

    [[nodiscard]] GuestMemory& memory() noexcept;
    [[nodiscard]] const GuestMemory& memory() const noexcept;

private:
    static void require_gpr_index(std::uint8_t index);

    std::array<std::uint64_t, 32> gpr_{};
    std::uint32_t pc_ = 0;
    GuestMemory memory_;
};

} // namespace gt4recomp::ee
