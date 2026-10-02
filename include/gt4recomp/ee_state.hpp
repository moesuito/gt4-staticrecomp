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

    // LWR with a non-aligned address replaces only the low 32 bits and keeps
    // the upper half, which neither 32-bit nor 64-bit writes express; that
    // rule is why this accessor exists.
    void write_gpr_low32(std::uint8_t index, std::uint32_t value);

    // The R5900 register file is 128 bits wide; the accesses above view the
    // low half. MMI instructions read and write all four 32-bit lanes, so the
    // upper half is addressable separately. The alias register r0 is zero for
    // both halves and ignores writes.
    [[nodiscard]] std::uint64_t read_gpr_high64(std::uint8_t index) const;
    void write_gpr_high64(std::uint8_t index, std::uint64_t value);

    [[nodiscard]] std::uint32_t pc() const noexcept;
    void set_pc(std::uint32_t value) noexcept;

    [[nodiscard]] GuestMemory& memory() noexcept;
    [[nodiscard]] const GuestMemory& memory() const noexcept;

    // FPU registers keep single-precision values as their 32-bit bit patterns;
    // interpreting those patterns as floats happens in the interpreter.
    [[nodiscard]] std::uint32_t read_fpr(std::uint8_t index) const;
    void write_fpr(std::uint8_t index, std::uint32_t value);

    // The FPU accumulator is written by the ADDA/SUBA/MULA forms and read by
    // MADD/MSUB. FCR31 holds the compare condition (bit 23) plus cause bits.
    [[nodiscard]] std::uint32_t fpu_accumulator() const noexcept;
    void set_fpu_accumulator(std::uint32_t value) noexcept;
    [[nodiscard]] std::uint32_t fpu_control() const noexcept;
    void set_fpu_control(std::uint32_t value) noexcept;

    // HI/LO come in two 64-bit halves each: the "1" halves serve the MMI
    // variants (MFHI1/MTHI1/...) and the 128-bit shift staging.
    [[nodiscard]] std::uint64_t hi() const noexcept;
    void set_hi(std::uint64_t value) noexcept;
    [[nodiscard]] std::uint64_t lo() const noexcept;
    void set_lo(std::uint64_t value) noexcept;
    [[nodiscard]] std::uint64_t hi1() const noexcept;
    void set_hi1(std::uint64_t value) noexcept;
    [[nodiscard]] std::uint64_t lo1() const noexcept;
    void set_lo1(std::uint64_t value) noexcept;

    // Shift-amount cache written by MTSA/MTSAB/MTSAH and read by the shift
    // instructions that take their amount from state instead of the encoding.
    [[nodiscard]] std::uint32_t shift_amount_cache() const noexcept;
    void set_shift_amount_cache(std::uint32_t value) noexcept;

private:
    static void require_gpr_index(std::uint8_t index);
    static void require_fpr_index(std::uint8_t index);

    std::array<std::uint64_t, 32> gpr_{};
    std::array<std::uint64_t, 32> gpr_high_{};
    std::array<std::uint32_t, 32> fpr_{};
    std::uint64_t hi_ = 0;
    std::uint64_t lo_ = 0;
    std::uint64_t hi1_ = 0;
    std::uint64_t lo1_ = 0;
    std::uint32_t fpu_accumulator_ = 0;
    std::uint32_t fpu_control_ = 0;
    std::uint32_t shift_amount_cache_ = 0;
    std::uint32_t pc_ = 0;
    GuestMemory memory_;
};

} // namespace gt4recomp::ee
