#include "gt4recomp/ee_interpreter.hpp"
#include "gt4recomp/ee_flow.hpp"

#include "vu_macro.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace gt4recomp::ee {
namespace {

constexpr std::uint8_t link_register = 31;

// FCR31 carries the compare condition in bit 23 plus cause and sticky bits;
// the FPU arithmetic in this interpreter maintains exactly the bits the
// reference implementation maintains.
constexpr std::uint32_t fpu_flag_c = 0x00800000u;
constexpr std::uint32_t fpu_flag_i = 0x00020000u;
constexpr std::uint32_t fpu_flag_d = 0x00010000u;
constexpr std::uint32_t fpu_flag_o = 0x00008000u;
constexpr std::uint32_t fpu_flag_u = 0x00004000u;
constexpr std::uint32_t fpu_flag_si = 0x00000040u;
constexpr std::uint32_t fpu_flag_sd = 0x00000020u;
constexpr std::uint32_t fpu_flag_so = 0x00000010u;
constexpr std::uint32_t fpu_flag_su = 0x00000008u;

// Raised by the trapping arithmetic when its signed overflow check fires; the
// step loop turns this into the stable Exception outcome at the offending
// word. The reference sets Cause and EPC before entering its handler; this
// model stops at the boundary instead, like it does for SYSCALL and BREAK.
struct IntegerOverflow {};

// The trapping arithmetic raises on signed overflow. The checks mirror the
// reference exactly: the 32-bit method compares bit 31 against bit 32 of the
// 64-bit sum, the 64-bit method tests the sign-bit identity, and the subtract
// forms negate the right operand before the check (which reproduces the
// reference's edge-case behavior for the most negative operand). The check
// runs before any register write, even when the destination is the zero
// register.
std::uint32_t add_checked_32(std::uint32_t left, std::uint32_t right) {
    const std::int64_t sum = static_cast<std::int64_t>(static_cast<std::int32_t>(left))
        + static_cast<std::int64_t>(static_cast<std::int32_t>(right));
    const auto bits = static_cast<std::uint64_t>(sum);
    if (((bits >> 31) & 1u) != ((bits >> 32) & 1u)) {
        throw IntegerOverflow{};
    }
    return static_cast<std::uint32_t>(bits);
}

std::uint64_t add_checked_64(std::uint64_t left, std::uint64_t right) {
    const std::uint64_t sum = left + right;
    if (((~(left ^ right) & (left ^ sum)) & 0x8000000000000000ull) != 0) {
        throw IntegerOverflow{};
    }
    return sum;
}

bool is_negative_64(std::uint64_t value) {
    return (value & 0x8000000000000000ull) != 0;
}

bool branch_taken(const DecodedInstruction& instruction, const GuestState& state) {
    const auto left = state.read_gpr64(instruction.rs);
    switch (instruction.operation) {
    case Operation::Beq:
    case Operation::Beql:
        return left == state.read_gpr64(instruction.rt);
    case Operation::Bne:
    case Operation::Bnel:
        return left != state.read_gpr64(instruction.rt);
    case Operation::Blez:
    case Operation::Blezl:
        return is_negative_64(left) || left == 0;
    case Operation::Bgtz:
    case Operation::Bgtzl:
        return !is_negative_64(left) && left != 0;
    case Operation::Bltz:
    case Operation::Bltzl:
    case Operation::Bltzal:
    case Operation::Bltzall:
        return is_negative_64(left);
    case Operation::Bgez:
    case Operation::Bgezl:
    case Operation::Bgezal:
    case Operation::Bgezall:
        return !is_negative_64(left);
    case Operation::Bc1f:
    case Operation::Bc1fl:
        return (state.fpu_control() & fpu_flag_c) == 0;
    case Operation::Bc1t:
    case Operation::Bc1tl:
        return (state.fpu_control() & fpu_flag_c) != 0;
    default:
        throw std::logic_error("branch condition requested for a non-branch operation");
    }
}

std::uint32_t sign_extended_16(std::uint16_t value) {
    return (value & 0x8000u) != 0 ? (0xffff0000u | value) : value;
}

std::uint32_t sign_extended_8(std::uint8_t value) {
    return (value & 0x80u) != 0 ? (0xffffff00u | value) : value;
}

bool less_than_signed_64(std::uint64_t left, std::uint64_t right) {
    // Flipping the sign bit turns the unsigned comparison into a signed one
    // without relying on conversion semantics.
    return (left ^ 0x8000000000000000ull) < (right ^ 0x8000000000000000ull);
}

std::uint32_t arithmetic_shift_right_32(std::uint32_t value, std::uint8_t shift) {
    // Explicit sign fill instead of a host-defined arithmetic shift.
    if (shift == 0) {
        return value;
    }
    const std::uint32_t shifted = value >> shift;
    return (value & 0x80000000u) != 0 ? (shifted | (0xffffffffu << (32 - shift))) : shifted;
}

std::uint64_t arithmetic_shift_right_64(std::uint64_t value, std::uint8_t shift) {
    // Same explicit sign fill for the 64-bit shifts.
    if (shift == 0) {
        return value;
    }
    const std::uint64_t shifted = value >> shift;
    return (value & 0x8000000000000000ull) != 0
        ? (shifted | (0xffffffffffffffffull << (64 - shift)))
        : shifted;
}

// GPR[rs] + sign-extended immediate, truncated to the 32-bit address model.
std::uint32_t effective_address(const GuestState& state, const DecodedInstruction& instruction) {
    const std::uint64_t base = state.read_gpr64(instruction.rs);
    const auto displacement = static_cast<std::uint64_t>(
        static_cast<std::int64_t>(instruction.signed_immediate()));
    return static_cast<std::uint32_t>(base + displacement);
}

constexpr std::uint32_t largest_finite_bits = 0x7f7fffffu;

// The PS2 FPU has no denormals and saturates at the largest finite value:
// denormal inputs read as signed zero and infinite (or NaN) inputs as
// +/-FLT_MAX. The reference implementation applies the same rules before
// every operation, so results agree bit for bit.
float hardware_float(std::uint32_t bits) {
    const std::uint32_t exponent = bits & 0x7f800000u;
    if (exponent == 0) {
        return std::bit_cast<float>(bits & 0x80000000u);
    }
    if (exponent == 0x7f800000u) {
        return std::bit_cast<float>((bits & 0x80000000u) | largest_finite_bits);
    }
    return std::bit_cast<float>(bits);
}

// Applies the reference overflow/underflow rules to a computed result. An
// infinite result becomes the largest finite value of the same sign and sets
// the overflow flags (stopping before the underflow check, like the early
// return in the reference); a denormal result flushes to signed zero and sets
// the underflow flags. Otherwise the corresponding cause flags are cleared.
std::uint32_t normalize_fpu_result(float value, std::uint32_t overflow_flags,
                                   std::uint32_t underflow_flags, GuestState& state) {
    std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
    if ((bits & 0x7fffffffu) == 0x7f800000u) {
        bits = (bits & 0x80000000u) | largest_finite_bits;
        state.set_fpu_control(state.fpu_control() | overflow_flags);
        return bits;
    }
    if ((overflow_flags & fpu_flag_o) != 0) {
        state.set_fpu_control(state.fpu_control() & ~fpu_flag_o);
    }
    if ((bits & 0x7f800000u) == 0 && (bits & 0x007fffffu) != 0) {
        bits &= 0x80000000u;
        state.set_fpu_control(state.fpu_control() | underflow_flags);
        return bits;
    }
    if ((underflow_flags & fpu_flag_u) != 0) {
        state.set_fpu_control(state.fpu_control() & ~fpu_flag_u);
    }
    return bits;
}

// Division by a zero or denormal divisor produces a saturated quotient whose
// sign is the exclusive-or of the operand signs; the flags distinguish a zero
// dividend (invalid) from a nonzero one (divide by zero).
bool divide_by_zero(std::uint32_t& bits, std::uint32_t divisor, std::uint32_t dividend,
                    std::uint32_t nonzero_dividend_flags, std::uint32_t zero_dividend_flags,
                    GuestState& state) {
    if ((divisor & 0x7f800000u) != 0) {
        return false;
    }
    state.set_fpu_control(state.fpu_control()
        | ((dividend & 0x7f800000u) == 0 ? zero_dividend_flags : nonzero_dividend_flags));
    bits = ((divisor ^ dividend) & 0x80000000u) | largest_finite_bits;
    return true;
}

void set_fpu_condition(GuestState& state, bool condition) {
    const auto control = state.fpu_control();
    state.set_fpu_control(condition ? (control | fpu_flag_c) : (control & ~fpu_flag_c));
}

// max.s and min.s order the raw bit patterns as signed integers, which is the
// hardware behaviour; NaN and -0.0 therefore compare by their bit patterns.
std::uint32_t fpu_max(std::uint32_t left, std::uint32_t right) {
    const auto a = static_cast<std::int32_t>(left);
    const auto b = static_cast<std::int32_t>(right);
    if (a < 0 && b < 0) {
        return static_cast<std::uint32_t>(std::min(a, b));
    }
    return static_cast<std::uint32_t>(std::max(a, b));
}

std::uint32_t fpu_min(std::uint32_t left, std::uint32_t right) {
    const auto a = static_cast<std::int32_t>(left);
    const auto b = static_cast<std::int32_t>(right);
    if (a < 0 && b < 0) {
        return static_cast<std::uint32_t>(std::max(a, b));
    }
    return static_cast<std::uint32_t>(std::min(a, b));
}

std::uint64_t sign_extend_32_to_64(std::uint32_t value) {
    return (value & 0x80000000u) != 0 ? (0xffffffff00000000ull | value) : value;
}

// Unaligned load/store merge tables from the reference implementation: LWL
// and SWL shift the incoming word left through the low bytes (24..0), LWR and
// SWR shift it right into the low bytes (0..24), and the masks preserve the
// register or memory bytes that are not replaced.
constexpr std::array<std::uint32_t, 4> lwl_mask = {
    0x00ffffffu, 0x0000ffffu, 0x000000ffu, 0x00000000u
};
constexpr std::array<std::uint32_t, 4> lwr_mask = {
    0x00000000u, 0xff000000u, 0xffff0000u, 0xffffff00u
};
constexpr std::array<std::uint32_t, 4> swl_mask = {
    0xffffff00u, 0xffff0000u, 0xff000000u, 0x00000000u
};
constexpr std::array<std::uint32_t, 4> swr_mask = {
    0x00000000u, 0x000000ffu, 0x0000ffffu, 0x00ffffffu
};
constexpr std::array<std::uint8_t, 4> merge_shift = { 24, 16, 8, 0 };
constexpr std::array<std::uint8_t, 4> place_shift = { 0, 8, 16, 24 };

// The same tables for the unaligned doubleword forms: LDL/SDL shift the
// incoming value left through the low bytes (56..0), LDR/SDR shift it right
// into the low bytes (0..56).
constexpr std::array<std::uint64_t, 8> ldl_mask = {
    0x00ffffffffffffffull, 0x0000ffffffffffffull, 0x000000ffffffffffull,
    0x00000000ffffffffull, 0x0000000000ffffffull, 0x000000000000ffffull,
    0x00000000000000ffull, 0x0000000000000000ull
};
constexpr std::array<std::uint64_t, 8> ldr_mask = {
    0x0000000000000000ull, 0xff00000000000000ull, 0xffff000000000000ull,
    0xffffff0000000000ull, 0xffffffff00000000ull, 0xffffffffff000000ull,
    0xffffffffffff0000ull, 0xffffffffffffff00ull
};
constexpr std::array<std::uint64_t, 8> sdl_mask = {
    0xffffffffffffff00ull, 0xffffffffffff0000ull, 0xffffffffff000000ull,
    0xffffffff00000000ull, 0xffffff0000000000ull, 0xffff000000000000ull,
    0xff00000000000000ull, 0x0000000000000000ull
};
constexpr std::array<std::uint64_t, 8> sdr_mask = {
    0x0000000000000000ull, 0x00000000000000ffull, 0x000000000000ffffull,
    0x0000000000ffffffull, 0x00000000ffffffffull, 0x000000ffffffffffull,
    0x0000ffffffffffffull, 0x00ffffffffffffffull
};
constexpr std::array<std::uint8_t, 8> doubleword_merge_shift = {
    56, 48, 40, 32, 24, 16, 8, 0
};
constexpr std::array<std::uint8_t, 8> doubleword_place_shift = {
    0, 8, 16, 24, 32, 40, 48, 56
};

// PLZCW counts the leading bits equal to the sign (excluding the sign bit
// itself): the reference inverts negative values, counts 32 for zero, and the
// instruction stores one less than the count of leading equal bits.
std::uint32_t count_leading_sign_bits(std::uint32_t value) {
    if ((value & 0x80000000u) != 0) {
        value = ~value;
    }
    return value == 0 ? 32u : static_cast<std::uint32_t>(std::countl_zero(value));
}

// The multiply and divide results store each 32-bit half sign-extended into
// its 64-bit HI/LO register, in both banks.
void write_hilo_low(GuestState& state, std::uint64_t bits) {
    state.set_lo(sign_extend_32_to_64(static_cast<std::uint32_t>(bits)));
    state.set_hi(sign_extend_32_to_64(static_cast<std::uint32_t>(bits >> 32)));
}

void write_hilo_high(GuestState& state, std::uint64_t bits) {
    state.set_lo1(sign_extend_32_to_64(static_cast<std::uint32_t>(bits)));
    state.set_hi1(sign_extend_32_to_64(static_cast<std::uint32_t>(bits >> 32)));
}

// The parallel multiply/divide family reaches the four 32-bit lanes of each
// 128-bit accumulator: lanes 0-1 live in the low bank, lanes 2-3 in the "1"
// bank the MMI variants use.
std::uint32_t lo_lane(const GuestState& state, int lane) {
    const std::uint64_t bank = lane < 2 ? state.lo() : state.lo1();
    return static_cast<std::uint32_t>(bank >> (32 * (lane & 1)));
}

void write_lo_lane(GuestState& state, int lane, std::uint32_t value) {
    const std::uint64_t bank = lane < 2 ? state.lo() : state.lo1();
    const int shift = 32 * (lane & 1);
    const std::uint64_t updated =
        (bank & ~(0xffffffffull << shift)) | (static_cast<std::uint64_t>(value) << shift);
    if (lane < 2) {
        state.set_lo(updated);
    } else {
        state.set_lo1(updated);
    }
}

std::uint32_t hi_lane(const GuestState& state, int lane) {
    const std::uint64_t bank = lane < 2 ? state.hi() : state.hi1();
    return static_cast<std::uint32_t>(bank >> (32 * (lane & 1)));
}

void write_hi_lane(GuestState& state, int lane, std::uint32_t value) {
    const std::uint64_t bank = lane < 2 ? state.hi() : state.hi1();
    const int shift = 32 * (lane & 1);
    const std::uint64_t updated =
        (bank & ~(0xffffffffull << shift)) | (static_cast<std::uint64_t>(value) << shift);
    if (lane < 2) {
        state.set_hi(updated);
    } else {
        state.set_hi1(updated);
    }
}

// MMI results fill all four 32-bit lanes of the 128-bit register; the low 64
// bits hold lanes 0..1 (words), 0..3 (halfwords) and 0..7 (bytes), the stored
// upper half holds the remainder.
struct WideRegister {
    std::uint64_t low = 0;
    std::uint64_t high = 0;

    [[nodiscard]] std::uint32_t word(int lane) const {
        return static_cast<std::uint32_t>((lane < 2 ? low : high) >> (32 * (lane & 1)));
    }
    void set_word(int lane, std::uint32_t value) {
        const int shift = 32 * (lane & 1);
        std::uint64_t& half = lane < 2 ? low : high;
        const std::uint64_t mask = 0xffffffffull << shift;
        half = (half & ~mask) | (static_cast<std::uint64_t>(value) << shift);
    }
    [[nodiscard]] std::uint16_t halfword(int lane) const {
        return static_cast<std::uint16_t>((lane < 4 ? low : high) >> (16 * (lane & 3)));
    }
    void set_halfword(int lane, std::uint16_t value) {
        const int shift = 16 * (lane & 3);
        std::uint64_t& half = lane < 4 ? low : high;
        const std::uint64_t mask = 0xffffull << shift;
        half = (half & ~mask) | (static_cast<std::uint64_t>(value) << shift);
    }
    [[nodiscard]] std::uint8_t byte(int lane) const {
        return static_cast<std::uint8_t>((lane < 8 ? low : high) >> (8 * (lane & 7)));
    }
    void set_byte(int lane, std::uint8_t value) {
        const int shift = 8 * (lane & 7);
        std::uint64_t& half = lane < 8 ? low : high;
        const std::uint64_t mask = 0xffull << shift;
        half = (half & ~mask) | (static_cast<std::uint64_t>(value) << shift);
    }
};

WideRegister read_wide(const GuestState& state, std::uint8_t index) {
    return WideRegister{state.read_gpr64(index), state.read_gpr_high64(index)};
}

void write_wide(GuestState& state, std::uint8_t index, const WideRegister& value) {
    state.write_gpr64(index, value.low);
    state.write_gpr_high64(index, value.high);
}

// One signed halfword product of the parallel halfword forms (PMADDH, PMULTH,
// PMSUBH): the eight products fill the eight accumulator lanes.
std::uint32_t parallel_halfword_product(
    const WideRegister& left, const WideRegister& right, int lane) {
    const auto left_half =
        static_cast<std::int32_t>(static_cast<std::int16_t>(left.halfword(lane)));
    const auto right_half =
        static_cast<std::int32_t>(static_cast<std::int16_t>(right.halfword(lane)));
    return static_cast<std::uint32_t>(left_half * right_half);
}

// The halfword forms pack the low accumulator words of each pair into the
// destination register.
void write_parallel_packing(GuestState& state, std::uint8_t rd) {
    if (rd == 0) {
        return;
    }
    state.write_gpr64(rd, (static_cast<std::uint64_t>(hi_lane(state, 0)) << 32) | lo_lane(state, 0));
    state.write_gpr_high64(
        rd, (static_cast<std::uint64_t>(hi_lane(state, 2)) << 32) | lo_lane(state, 2));
}

std::uint16_t arithmetic_shift_right_16(std::uint16_t value, std::uint8_t shift) {
    const std::uint32_t widened = sign_extended_16(value);
    return static_cast<std::uint16_t>(arithmetic_shift_right_32(widened, shift) & 0xffffu);
}

// PMFHL's halfword packing saturates each word to the signed 16-bit range.
std::uint16_t pmfhl_clamp(std::uint32_t value) {
    const auto signed_value = static_cast<std::int32_t>(value);
    if (signed_value > 0x7fff) {
        return 0x7fff;
    }
    if (signed_value < -0x8000) {
        return 0x8000;
    }
    return static_cast<std::uint16_t>(value);
}

// Returns true when the operation belongs to the special-register group:
// HI/LO moves (both halves), synchronization and the shift-amount cache.
bool execute_special_register(const DecodedInstruction& instruction, GuestState& state) {
    switch (instruction.operation) {
    case Operation::Mfhi:
        state.write_gpr64(instruction.rd, state.hi());
        break;
    case Operation::Mflo:
        state.write_gpr64(instruction.rd, state.lo());
        break;
    case Operation::Mthi:
        state.set_hi(state.read_gpr64(instruction.rs));
        break;
    case Operation::Mtlo:
        state.set_lo(state.read_gpr64(instruction.rs));
        break;
    case Operation::Mfhi1:
        state.write_gpr64(instruction.rd, state.hi1());
        break;
    case Operation::Mflo1:
        state.write_gpr64(instruction.rd, state.lo1());
        break;
    case Operation::Mthi1:
        state.set_hi1(state.read_gpr64(instruction.rs));
        break;
    case Operation::Mtlo1:
        state.set_lo1(state.read_gpr64(instruction.rs));
        break;
    case Operation::Mult: {
        const std::int64_t product = static_cast<std::int64_t>(
            static_cast<std::int32_t>(state.read_gpr32(instruction.rs)))
            * static_cast<std::int64_t>(static_cast<std::int32_t>(state.read_gpr32(instruction.rt)));
        write_hilo_low(state, static_cast<std::uint64_t>(product));
        if (instruction.rd != 0) {
            state.write_gpr64(instruction.rd, state.lo());
        }
        break;
    }
    case Operation::Multu: {
        const std::uint64_t product = static_cast<std::uint64_t>(state.read_gpr32(instruction.rs))
            * static_cast<std::uint64_t>(state.read_gpr32(instruction.rt));
        write_hilo_low(state, product);
        if (instruction.rd != 0) {
            state.write_gpr64(instruction.rd, state.lo());
        }
        break;
    }
    case Operation::Div: {
        const std::uint32_t dividend_bits = state.read_gpr32(instruction.rs);
        const std::uint32_t divisor_bits = state.read_gpr32(instruction.rt);
        const auto dividend = static_cast<std::int32_t>(dividend_bits);
        const auto divisor = static_cast<std::int32_t>(divisor_bits);
        if (dividend_bits == 0x80000000u && divisor_bits == 0xffffffffu) {
            // The one overflowing quotient has the documented saturated result.
            state.set_lo(sign_extend_32_to_64(0x80000000u));
            state.set_hi(0);
        } else if (divisor != 0) {
            state.set_lo(sign_extend_32_to_64(static_cast<std::uint32_t>(dividend / divisor)));
            state.set_hi(sign_extend_32_to_64(static_cast<std::uint32_t>(dividend % divisor)));
        } else {
            // Division by zero: LO signals the dividend's sign, HI the dividend.
            state.set_lo(sign_extend_32_to_64(dividend < 0 ? 1u : 0xffffffffu));
            state.set_hi(sign_extend_32_to_64(dividend_bits));
        }
        break;
    }
    case Operation::Divu: {
        const std::uint32_t dividend_bits = state.read_gpr32(instruction.rs);
        const std::uint32_t divisor_bits = state.read_gpr32(instruction.rt);
        if (divisor_bits != 0) {
            state.set_lo(sign_extend_32_to_64(dividend_bits / divisor_bits));
            state.set_hi(sign_extend_32_to_64(dividend_bits % divisor_bits));
        } else {
            state.set_lo(sign_extend_32_to_64(0xffffffffu));
            state.set_hi(sign_extend_32_to_64(dividend_bits));
        }
        break;
    }
    case Operation::Madd: {
        const std::uint64_t accumulated = (state.lo() & 0xffffffffull)
            | ((state.hi() & 0xffffffffull) << 32);
        const std::int64_t product = static_cast<std::int64_t>(
            static_cast<std::int32_t>(state.read_gpr32(instruction.rs)))
            * static_cast<std::int64_t>(static_cast<std::int32_t>(state.read_gpr32(instruction.rt)));
        write_hilo_low(state, accumulated + static_cast<std::uint64_t>(product));
        if (instruction.rd != 0) {
            state.write_gpr64(instruction.rd, state.lo());
        }
        break;
    }
    case Operation::Maddu: {
        const std::uint64_t accumulated = (state.lo() & 0xffffffffull)
            | ((state.hi() & 0xffffffffull) << 32);
        const std::uint64_t product = static_cast<std::uint64_t>(state.read_gpr32(instruction.rs))
            * static_cast<std::uint64_t>(state.read_gpr32(instruction.rt));
        write_hilo_low(state, accumulated + product);
        if (instruction.rd != 0) {
            state.write_gpr64(instruction.rd, state.lo());
        }
        break;
    }
    case Operation::Mult1: {
        const std::int64_t product = static_cast<std::int64_t>(
            static_cast<std::int32_t>(state.read_gpr32(instruction.rs)))
            * static_cast<std::int64_t>(static_cast<std::int32_t>(state.read_gpr32(instruction.rt)));
        write_hilo_high(state, static_cast<std::uint64_t>(product));
        if (instruction.rd != 0) {
            state.write_gpr64(instruction.rd, state.lo1());
        }
        break;
    }
    case Operation::Multu1: {
        const std::uint64_t product = static_cast<std::uint64_t>(state.read_gpr32(instruction.rs))
            * static_cast<std::uint64_t>(state.read_gpr32(instruction.rt));
        write_hilo_high(state, product);
        if (instruction.rd != 0) {
            state.write_gpr64(instruction.rd, state.lo1());
        }
        break;
    }
    case Operation::Div1: {
        const std::uint32_t dividend_bits = state.read_gpr32(instruction.rs);
        const std::uint32_t divisor_bits = state.read_gpr32(instruction.rt);
        const auto dividend = static_cast<std::int32_t>(dividend_bits);
        const auto divisor = static_cast<std::int32_t>(divisor_bits);
        if (dividend_bits == 0x80000000u && divisor_bits == 0xffffffffu) {
            state.set_lo1(sign_extend_32_to_64(0x80000000u));
            state.set_hi1(0);
        } else if (divisor != 0) {
            state.set_lo1(sign_extend_32_to_64(static_cast<std::uint32_t>(dividend / divisor)));
            state.set_hi1(sign_extend_32_to_64(static_cast<std::uint32_t>(dividend % divisor)));
        } else {
            state.set_lo1(sign_extend_32_to_64(dividend < 0 ? 1u : 0xffffffffu));
            state.set_hi1(sign_extend_32_to_64(dividend_bits));
        }
        break;
    }
    case Operation::Divu1: {
        const std::uint32_t dividend_bits = state.read_gpr32(instruction.rs);
        const std::uint32_t divisor_bits = state.read_gpr32(instruction.rt);
        if (divisor_bits != 0) {
            state.set_lo1(sign_extend_32_to_64(dividend_bits / divisor_bits));
            state.set_hi1(sign_extend_32_to_64(dividend_bits % divisor_bits));
        } else {
            state.set_lo1(sign_extend_32_to_64(0xffffffffu));
            state.set_hi1(sign_extend_32_to_64(dividend_bits));
        }
        break;
    }
    case Operation::Madd1: {
        const std::uint64_t accumulated = (state.lo1() & 0xffffffffull)
            | ((state.hi1() & 0xffffffffull) << 32);
        const std::int64_t product = static_cast<std::int64_t>(
            static_cast<std::int32_t>(state.read_gpr32(instruction.rs)))
            * static_cast<std::int64_t>(static_cast<std::int32_t>(state.read_gpr32(instruction.rt)));
        write_hilo_high(state, accumulated + static_cast<std::uint64_t>(product));
        if (instruction.rd != 0) {
            state.write_gpr64(instruction.rd, state.lo1());
        }
        break;
    }
    case Operation::Maddu1: {
        const std::uint64_t accumulated = (state.lo1() & 0xffffffffull)
            | ((state.hi1() & 0xffffffffull) << 32);
        const std::uint64_t product = static_cast<std::uint64_t>(state.read_gpr32(instruction.rs))
            * static_cast<std::uint64_t>(state.read_gpr32(instruction.rt));
        write_hilo_high(state, accumulated + product);
        if (instruction.rd != 0) {
            state.write_gpr64(instruction.rd, state.lo1());
        }
        break;
    }
    case Operation::Mtsa:
        state.set_shift_amount_cache(static_cast<std::uint32_t>(state.read_gpr64(instruction.rs)));
        break;
    case Operation::Mtsab:
        state.set_shift_amount_cache(
            (state.read_gpr32(instruction.rs) & 0xfu) ^ (instruction.immediate & 0xfu));
        break;
    case Operation::Mtsah:
        state.set_shift_amount_cache(
            ((state.read_gpr32(instruction.rs) & 0x7u) ^ (instruction.immediate & 0x7u)) << 1);
        break;
    case Operation::Sync:
        // The pipeline barrier is a no-op in this model; the reference
        // implementation treats it the same way outside of pipeline stalls.
        break;
    case Operation::Mfc0: {
        const std::uint8_t cp0_register = instruction.rd;
        if (instruction.rt == 0 && cp0_register != 9) {
            break;  // the reference skips the read entirely here
        }
        std::uint32_t value = state.read_cp0(cp0_register);
        if (cp0_register == 12) {
            value &= 0xf0c79c1fu;  // only the readable Status bits appear
        }
        // PCCR (25), the performance counter control register, is storage:
        // the model does not count cycles, like the timer policy of
        // decision 0007, so the game's frame-time sums stay zero.
        state.write_gpr32(instruction.rt, value);
        break;
    }
    case Operation::Mtc0: {
        const std::uint8_t cp0_register = instruction.rd;
        const std::uint32_t value = state.read_gpr32(instruction.rt);
        switch (cp0_register) {
        case 16:
            // Config protects the cache-size bits and reports the fixed ones.
            state.write_cp0(16, (value & ~0xfc0u) | 0x440u);
            break;
        case 24:
            // The debug register accepts the write as feedback only; the
            // reference does not store it either.
            break;
        case 25:
            // PCCR stores like any other register; nothing counts cycles.
            state.write_cp0(cp0_register, value);
            break;
        default:
            state.write_cp0(cp0_register, value);
            break;
        }
        break;
    }
    case Operation::Ei:
    case Operation::Di: {
        // Both take effect only in kernel mode (KSU == 0) or when already in
        // an exception level, exactly like the reference implementation.
        const std::uint32_t status = state.read_cp0(12);
        const bool takes_effect = (status & 0x00020000u) != 0  // _EDI
            || (status & 0x00000002u) != 0                     // EXL
            || (status & 0x00000004u) != 0                     // ERL
            || (status & 0x00000018u) == 0;                    // KSU == kernel
        if (takes_effect) {
            state.write_cp0(12, instruction.operation == Operation::Ei
                ? (status | 0x00010000u)
                : (status & ~0x00010000u));
        }
        break;
    }
    default:
        return false;
    }
    return true;
}

// Returns true when the operation belongs to COP1, the scalar FPU.
bool execute_cop1(const DecodedInstruction& instruction, GuestState& state) {
    switch (instruction.operation) {
    case Operation::Mfc1:
        // The raw 32-bit pattern moves sign-extended into the GPR.
        state.write_gpr32(instruction.rt, state.read_fpr(instruction.cop1_fs()));
        break;
    case Operation::Mtc1:
        state.write_fpr(instruction.cop1_fs(), state.read_gpr32(instruction.rt));
        break;
    case Operation::Cfc1:
        // FCR0 reads back the reference model's revision word; control
        // registers other than FCR0 and FCR31 read as zero.
        if (instruction.cop1_fs() == 31) {
            state.write_gpr32(instruction.rt, state.fpu_control());
        } else if (instruction.cop1_fs() == 0) {
            state.write_gpr32(instruction.rt, 0x2e00);
        } else {
            state.write_gpr32(instruction.rt, 0);
        }
        break;
    case Operation::Ctc1:
        // Only FCR31 is writable; writes to other control registers are
        // ignored, like the reference implementation.
        if (instruction.cop1_fs() == 31) {
            state.set_fpu_control(state.read_gpr32(instruction.rt));
        }
        break;
    case Operation::Lwc1: {
        const auto address = effective_address(state, instruction);
        state.write_fpr(instruction.rt, state.memory().read_word(address));
        break;
    }
    case Operation::Swc1: {
        const auto address = effective_address(state, instruction);
        state.memory().write_word(address, state.read_fpr(instruction.rt));
        break;
    }
    case Operation::AddS: {
        const float result = hardware_float(state.read_fpr(instruction.cop1_fs()))
            + hardware_float(state.read_fpr(instruction.cop1_ft()));
        state.write_fpr(instruction.cop1_fd(),
                        normalize_fpu_result(result, fpu_flag_o | fpu_flag_so,
                                             fpu_flag_u | fpu_flag_su, state));
        break;
    }
    case Operation::SubS: {
        const float result = hardware_float(state.read_fpr(instruction.cop1_fs()))
            - hardware_float(state.read_fpr(instruction.cop1_ft()));
        state.write_fpr(instruction.cop1_fd(),
                        normalize_fpu_result(result, fpu_flag_o | fpu_flag_so,
                                             fpu_flag_u | fpu_flag_su, state));
        break;
    }
    case Operation::MulS: {
        const float result = hardware_float(state.read_fpr(instruction.cop1_fs()))
            * hardware_float(state.read_fpr(instruction.cop1_ft()));
        state.write_fpr(instruction.cop1_fd(),
                        normalize_fpu_result(result, fpu_flag_o | fpu_flag_so,
                                             fpu_flag_u | fpu_flag_su, state));
        break;
    }
    case Operation::DivS: {
        const std::uint32_t divisor = state.read_fpr(instruction.cop1_ft());
        const std::uint32_t dividend = state.read_fpr(instruction.cop1_fs());
        std::uint32_t bits = 0;
        if (divide_by_zero(bits, divisor, dividend, fpu_flag_d | fpu_flag_sd,
                           fpu_flag_i | fpu_flag_si, state)) {
            state.write_fpr(instruction.cop1_fd(), bits);
            break;
        }
        const float result = hardware_float(dividend) / hardware_float(divisor);
        state.write_fpr(instruction.cop1_fd(), normalize_fpu_result(result, 0, 0, state));
        break;
    }
    case Operation::SqrtS: {
        // The source of sqrt.s is ft; the fs field is not an operand.
        const std::uint32_t source = state.read_fpr(instruction.cop1_ft());
        state.set_fpu_control(state.fpu_control() & ~(fpu_flag_i | fpu_flag_d));
        std::uint32_t result = 0;
        if ((source & 0x7f800000u) == 0) {
            result = source & 0x80000000u;
        } else if ((source & 0x80000000u) != 0) {
            state.set_fpu_control(state.fpu_control() | (fpu_flag_i | fpu_flag_si));
            result = std::bit_cast<std::uint32_t>(std::sqrt(std::fabs(hardware_float(source))));
        } else {
            result = std::bit_cast<std::uint32_t>(std::sqrt(hardware_float(source)));
        }
        state.write_fpr(instruction.cop1_fd(), result);
        break;
    }
    case Operation::RsqrtS: {
        // rsqrt.s computes fs / sqrt(ft) with the reference special cases.
        const std::uint32_t source = state.read_fpr(instruction.cop1_ft());
        state.set_fpu_control(state.fpu_control() & ~(fpu_flag_d | fpu_flag_i));
        if ((source & 0x7f800000u) == 0) {
            state.set_fpu_control(state.fpu_control() | (fpu_flag_d | fpu_flag_sd));
            state.write_fpr(instruction.cop1_fd(),
                            (source & 0x80000000u) | largest_finite_bits);
            break;
        }
        float root = 0.0f;
        if ((source & 0x80000000u) != 0) {
            state.set_fpu_control(state.fpu_control() | (fpu_flag_i | fpu_flag_si));
            root = std::sqrt(std::fabs(hardware_float(source)));
        } else {
            root = std::sqrt(hardware_float(source));
        }
        const float result = hardware_float(state.read_fpr(instruction.cop1_fs())) / root;
        state.write_fpr(instruction.cop1_fd(), normalize_fpu_result(result, 0, 0, state));
        break;
    }
    case Operation::AbsS:
        state.write_fpr(instruction.cop1_fd(),
                        state.read_fpr(instruction.cop1_fs()) & 0x7fffffffu);
        state.set_fpu_control(state.fpu_control() & ~(fpu_flag_o | fpu_flag_u));
        break;
    case Operation::NegS:
        state.write_fpr(instruction.cop1_fd(),
                        state.read_fpr(instruction.cop1_fs()) ^ 0x80000000u);
        state.set_fpu_control(state.fpu_control() & ~(fpu_flag_o | fpu_flag_u));
        break;
    case Operation::MovS:
        state.write_fpr(instruction.cop1_fd(), state.read_fpr(instruction.cop1_fs()));
        break;
    case Operation::MaxS:
        state.write_fpr(instruction.cop1_fd(),
                        fpu_max(state.read_fpr(instruction.cop1_fs()),
                                state.read_fpr(instruction.cop1_ft())));
        state.set_fpu_control(state.fpu_control() & ~(fpu_flag_o | fpu_flag_u));
        break;
    case Operation::MinS:
        state.write_fpr(instruction.cop1_fd(),
                        fpu_min(state.read_fpr(instruction.cop1_fs()),
                                state.read_fpr(instruction.cop1_ft())));
        state.set_fpu_control(state.fpu_control() & ~(fpu_flag_o | fpu_flag_u));
        break;
    case Operation::AddaS: {
        const float result = hardware_float(state.read_fpr(instruction.cop1_fs()))
            + hardware_float(state.read_fpr(instruction.cop1_ft()));
        state.set_fpu_accumulator(normalize_fpu_result(
            result, fpu_flag_o | fpu_flag_so, fpu_flag_u | fpu_flag_su, state));
        break;
    }
    case Operation::SubaS: {
        const float result = hardware_float(state.read_fpr(instruction.cop1_fs()))
            - hardware_float(state.read_fpr(instruction.cop1_ft()));
        state.set_fpu_accumulator(normalize_fpu_result(
            result, fpu_flag_o | fpu_flag_so, fpu_flag_u | fpu_flag_su, state));
        break;
    }
    case Operation::MulaS: {
        const float result = hardware_float(state.read_fpr(instruction.cop1_fs()))
            * hardware_float(state.read_fpr(instruction.cop1_ft()));
        state.set_fpu_accumulator(normalize_fpu_result(
            result, fpu_flag_o | fpu_flag_so, fpu_flag_u | fpu_flag_su, state));
        break;
    }
    case Operation::MaddaS: {
        // The product is not flushed before the add, matching the reference.
        const float result = hardware_float(state.fpu_accumulator())
            + hardware_float(state.read_fpr(instruction.cop1_fs()))
                * hardware_float(state.read_fpr(instruction.cop1_ft()));
        state.set_fpu_accumulator(normalize_fpu_result(
            result, fpu_flag_o | fpu_flag_so, fpu_flag_u | fpu_flag_su, state));
        break;
    }
    case Operation::MsubaS: {
        const float result = hardware_float(state.fpu_accumulator())
            - hardware_float(state.read_fpr(instruction.cop1_fs()))
                * hardware_float(state.read_fpr(instruction.cop1_ft()));
        state.set_fpu_accumulator(normalize_fpu_result(
            result, fpu_flag_o | fpu_flag_so, fpu_flag_u | fpu_flag_su, state));
        break;
    }
    case Operation::MaddS: {
        // The product passes through the flush/saturate rules before the add.
        const std::uint32_t product = std::bit_cast<std::uint32_t>(
            hardware_float(state.read_fpr(instruction.cop1_fs()))
            * hardware_float(state.read_fpr(instruction.cop1_ft())));
        const float result = hardware_float(state.fpu_accumulator()) + hardware_float(product);
        state.write_fpr(instruction.cop1_fd(),
                        normalize_fpu_result(result, fpu_flag_o | fpu_flag_so,
                                             fpu_flag_u | fpu_flag_su, state));
        break;
    }
    case Operation::MsubS: {
        const std::uint32_t product = std::bit_cast<std::uint32_t>(
            hardware_float(state.read_fpr(instruction.cop1_fs()))
            * hardware_float(state.read_fpr(instruction.cop1_ft())));
        const float result = hardware_float(state.fpu_accumulator()) - hardware_float(product);
        state.write_fpr(instruction.cop1_fd(),
                        normalize_fpu_result(result, fpu_flag_o | fpu_flag_so,
                                             fpu_flag_u | fpu_flag_su, state));
        break;
    }
    case Operation::CF:
        state.set_fpu_control(state.fpu_control() & ~fpu_flag_c);
        break;
    case Operation::CEq:
        set_fpu_condition(state, hardware_float(state.read_fpr(instruction.cop1_fs()))
            == hardware_float(state.read_fpr(instruction.cop1_ft())));
        break;
    case Operation::CLt:
        set_fpu_condition(state, hardware_float(state.read_fpr(instruction.cop1_fs()))
            < hardware_float(state.read_fpr(instruction.cop1_ft())));
        break;
    case Operation::CLe:
        set_fpu_condition(state, hardware_float(state.read_fpr(instruction.cop1_fs()))
            <= hardware_float(state.read_fpr(instruction.cop1_ft())));
        break;
    case Operation::CvtS:
        // cvt.s.w converts the raw word as a signed integer; the FPU flush
        // rules do not apply to this conversion.
        state.write_fpr(instruction.cop1_fd(),
                        std::bit_cast<std::uint32_t>(static_cast<float>(
                            static_cast<std::int32_t>(state.read_fpr(instruction.cop1_fs())))));
        break;
    case Operation::CvtW: {
        const std::uint32_t source = state.read_fpr(instruction.cop1_fs());
        if ((source & 0x7f800000u) <= 0x4e800000u) {
            // The exponent check guarantees a representable int32 before the
            // conversion, so no out-of-range cast happens.
            state.write_fpr(instruction.cop1_fd(),
                            static_cast<std::uint32_t>(static_cast<std::int32_t>(
                                std::bit_cast<float>(source))));
        } else {
            state.write_fpr(instruction.cop1_fd(),
                            (source & 0x80000000u) != 0 ? 0x80000000u : 0x7fffffffu);
        }
        break;
    }
    default:
        return false;
    }
    return true;
}

// Returns true when the operation belongs to COP2, the VU0 macro interface
// used from the EE side: 128-bit moves, control-register moves, the quad
// memory accesses and the reference no-operation.
bool execute_cop2(const DecodedInstruction& instruction, GuestState& state) {
    switch (instruction.operation) {
    case Operation::Qmfc2: {
        // The full 128 bits move into both GPR halves.
        const std::uint8_t vector = instruction.rd;
        const std::uint64_t low =
            static_cast<std::uint64_t>(state.read_vf_lane(vector, 0))
            | (static_cast<std::uint64_t>(state.read_vf_lane(vector, 1)) << 32);
        const std::uint64_t high =
            static_cast<std::uint64_t>(state.read_vf_lane(vector, 2))
            | (static_cast<std::uint64_t>(state.read_vf_lane(vector, 3)) << 32);
        state.write_gpr64(instruction.rt, low);
        state.write_gpr_high64(instruction.rt, high);
        break;
    }
    case Operation::Qmtc2: {
        const std::uint8_t vector = instruction.rd;
        // The constant register ignores writes (the model's accessor also
        // protects it).
        const std::uint64_t low = state.read_gpr64(instruction.rt);
        const std::uint64_t high = state.read_gpr_high64(instruction.rt);
        state.write_vf_lane(vector, 0, static_cast<std::uint32_t>(low));
        state.write_vf_lane(vector, 1, static_cast<std::uint32_t>(low >> 32));
        state.write_vf_lane(vector, 2, static_cast<std::uint32_t>(high));
        state.write_vf_lane(vector, 3, static_cast<std::uint32_t>(high >> 32));
        break;
    }
    case Operation::Cfc2: {
        if (instruction.rt == 0) {
            break;
        }
        const std::uint8_t control = instruction.rd;
        if (control == 20) {
            // The reciprocal register reads through its mantissa mask and
            // leaves the upper word untouched, like the reference.
            state.write_gpr_low32(instruction.rt, state.read_vi(20) & 0x7fffffu);
        } else {
            state.write_gpr32(instruction.rt, state.read_vi(control));
        }
        break;
    }
    case Operation::Ctc2: {
        const std::uint8_t control = instruction.rd;
        if (control == 0) {
            break;
        }
        const std::uint32_t value = state.read_gpr32(instruction.rt);
        switch (control) {
        case 17:  // MAC_FLAG is read-only
        case 26:  // TPC is read-only
        case 29:  // VPU_STAT is read-only
            break;
        case 20:  // the reciprocal register keeps its exponent constant
            state.write_vi(20, (value & 0x7fffffu) | 0x3f800000u);
            break;
        case 28: {
            // FBRST (VI28): the model stores the writable bits. The VU0
            // reset bit clears the whole VU0 register file. The VU1 control
            // bits (0x100/0x200) are recorded but have no target state: the
            // model does not execute VU1 microcode, so "resetting VU1" is a
            // no-op by construction (a documented model choice, recorded in
            // the eighth slice's evidence; it replaces the earlier stop).
            state.write_vi(28, value & 0x0c0cu);
            if ((value & 0x00000002u) != 0) {
                state.reset_vu0_registers();
            }
            // The force-break request only matters with running VU micro
            // code, which this model does not execute.
            break;
        }
        case 31:
            throw std::runtime_error("ctc2 CMSAR1: VU1 execution is not modeled");
        case 18:
            // CLIP_FLAG reaches the shadow register and the VI entry.
            state.set_vu0_clip_flag(value);
            state.write_vi(18, value);
            break;
        default:
            state.write_vi(control, value);
            break;
        }
        break;
    }
    case Operation::Lqc2: {
        const std::uint32_t address = effective_address(state, instruction);
        if ((address & 0xfu) != 0) {
            throw std::runtime_error("lqc2 requires a 16-byte aligned address");
        }
        // The access happens even when the destination is the constant
        // register, which discards the value.
        const std::uint64_t low = state.memory().read_doubleword(address);
        const std::uint64_t high = state.memory().read_doubleword(address + 8);
        state.write_vf_lane(instruction.rt, 0, static_cast<std::uint32_t>(low));
        state.write_vf_lane(instruction.rt, 1, static_cast<std::uint32_t>(low >> 32));
        state.write_vf_lane(instruction.rt, 2, static_cast<std::uint32_t>(high));
        state.write_vf_lane(instruction.rt, 3, static_cast<std::uint32_t>(high >> 32));
        break;
    }
    case Operation::Sqc2: {
        const std::uint32_t address = effective_address(state, instruction);
        if ((address & 0xfu) != 0) {
            throw std::runtime_error("sqc2 requires a 16-byte aligned address");
        }
        const std::uint64_t low =
            static_cast<std::uint64_t>(state.read_vf_lane(instruction.rt, 0))
            | (static_cast<std::uint64_t>(state.read_vf_lane(instruction.rt, 1)) << 32);
        const std::uint64_t high =
            static_cast<std::uint64_t>(state.read_vf_lane(instruction.rt, 2))
            | (static_cast<std::uint64_t>(state.read_vf_lane(instruction.rt, 3)) << 32);
        state.memory().write_doubleword(address, low);
        state.memory().write_doubleword(address + 8, high);
        break;
    }
    case Operation::Vnop:
    default:
        // The VU macro arithmetic and the no-operation live in their own
        // module next to the flag model they share.
        return execute_vu_macro(instruction, state);
    }
    return true;
}

// Returns true when the operation belongs to the MMI extension. Lane results
// fill all four 32-bit lanes of the 128-bit register; saturated forms clamp to
// the range limits exactly like the reference implementation.
bool execute_mmi(const DecodedInstruction& instruction, GuestState& state) {
    switch (instruction.operation) {
    case Operation::Paddw: {
        const auto left = read_wide(state, instruction.rs);
        const auto right = read_wide(state, instruction.rt);
        WideRegister result;
        for (int lane = 0; lane < 4; ++lane) {
            result.set_word(lane, left.word(lane) + right.word(lane));
        }
        write_wide(state, instruction.rd, result);
        break;
    }
    case Operation::Psubw: {
        const auto left = read_wide(state, instruction.rs);
        const auto right = read_wide(state, instruction.rt);
        WideRegister result;
        for (int lane = 0; lane < 4; ++lane) {
            result.set_word(lane, left.word(lane) - right.word(lane));
        }
        write_wide(state, instruction.rd, result);
        break;
    }
    case Operation::Paddh: {
        const auto left = read_wide(state, instruction.rs);
        const auto right = read_wide(state, instruction.rt);
        WideRegister result;
        for (int lane = 0; lane < 8; ++lane) {
            result.set_halfword(lane, static_cast<std::uint16_t>(
                left.halfword(lane) + right.halfword(lane)));
        }
        write_wide(state, instruction.rd, result);
        break;
    }
    case Operation::Psubh: {
        const auto left = read_wide(state, instruction.rs);
        const auto right = read_wide(state, instruction.rt);
        WideRegister result;
        for (int lane = 0; lane < 8; ++lane) {
            result.set_halfword(lane, static_cast<std::uint16_t>(
                left.halfword(lane) - right.halfword(lane)));
        }
        write_wide(state, instruction.rd, result);
        break;
    }
    case Operation::Paddb: {
        const auto left = read_wide(state, instruction.rs);
        const auto right = read_wide(state, instruction.rt);
        WideRegister result;
        for (int lane = 0; lane < 16; ++lane) {
            result.set_byte(lane, static_cast<std::uint8_t>(left.byte(lane) + right.byte(lane)));
        }
        write_wide(state, instruction.rd, result);
        break;
    }
    case Operation::Psubb: {
        const auto left = read_wide(state, instruction.rs);
        const auto right = read_wide(state, instruction.rt);
        WideRegister result;
        for (int lane = 0; lane < 16; ++lane) {
            result.set_byte(lane, static_cast<std::uint8_t>(left.byte(lane) - right.byte(lane)));
        }
        write_wide(state, instruction.rd, result);
        break;
    }
    case Operation::Paddsw: {
        const auto left = read_wide(state, instruction.rs);
        const auto right = read_wide(state, instruction.rt);
        WideRegister result;
        for (int lane = 0; lane < 4; ++lane) {
            const std::int64_t sum =
                static_cast<std::int64_t>(static_cast<std::int32_t>(left.word(lane)))
                + static_cast<std::int64_t>(static_cast<std::int32_t>(right.word(lane)));
            if (sum > 0x7fffffffll) {
                result.set_word(lane, 0x7fffffffu);
            } else if (sum < -0x80000000ll) {
                result.set_word(lane, 0x80000000u);
            } else {
                result.set_word(lane, static_cast<std::uint32_t>(sum));
            }
        }
        write_wide(state, instruction.rd, result);
        break;
    }
    case Operation::Psubsw: {
        const auto left = read_wide(state, instruction.rs);
        const auto right = read_wide(state, instruction.rt);
        WideRegister result;
        for (int lane = 0; lane < 4; ++lane) {
            const std::int64_t difference =
                static_cast<std::int64_t>(static_cast<std::int32_t>(left.word(lane)))
                - static_cast<std::int64_t>(static_cast<std::int32_t>(right.word(lane)));
            if (difference >= 0x7fffffffll) {
                result.set_word(lane, 0x7fffffffu);
            } else if (difference < -0x80000000ll) {
                result.set_word(lane, 0x80000000u);
            } else {
                result.set_word(lane, static_cast<std::uint32_t>(difference));
            }
        }
        write_wide(state, instruction.rd, result);
        break;
    }
    case Operation::Paddsh: {
        const auto left = read_wide(state, instruction.rs);
        const auto right = read_wide(state, instruction.rt);
        WideRegister result;
        for (int lane = 0; lane < 8; ++lane) {
            const std::int32_t sum =
                static_cast<std::int32_t>(static_cast<std::int16_t>(left.halfword(lane)))
                + static_cast<std::int32_t>(static_cast<std::int16_t>(right.halfword(lane)));
            if (sum > 0x7fff) {
                result.set_halfword(lane, 0x7fff);
            } else if (sum < -0x8000) {
                result.set_halfword(lane, 0x8000);
            } else {
                result.set_halfword(lane, static_cast<std::uint16_t>(sum));
            }
        }
        write_wide(state, instruction.rd, result);
        break;
    }
    case Operation::Psubsh: {
        const auto left = read_wide(state, instruction.rs);
        const auto right = read_wide(state, instruction.rt);
        WideRegister result;
        for (int lane = 0; lane < 8; ++lane) {
            const std::int32_t difference =
                static_cast<std::int32_t>(static_cast<std::int16_t>(left.halfword(lane)))
                - static_cast<std::int32_t>(static_cast<std::int16_t>(right.halfword(lane)));
            if (difference >= 0x7fff) {
                result.set_halfword(lane, 0x7fff);
            } else if (difference < -0x8000) {
                result.set_halfword(lane, 0x8000);
            } else {
                result.set_halfword(lane, static_cast<std::uint16_t>(difference));
            }
        }
        write_wide(state, instruction.rd, result);
        break;
    }
    case Operation::Paddsb: {
        const auto left = read_wide(state, instruction.rs);
        const auto right = read_wide(state, instruction.rt);
        WideRegister result;
        for (int lane = 0; lane < 16; ++lane) {
            const std::int32_t sum =
                static_cast<std::int32_t>(static_cast<std::int8_t>(left.byte(lane)))
                + static_cast<std::int32_t>(static_cast<std::int8_t>(right.byte(lane)));
            if (sum > 0x7f) {
                result.set_byte(lane, 0x7f);
            } else if (sum < -0x80) {
                result.set_byte(lane, 0x80);
            } else {
                result.set_byte(lane, static_cast<std::uint8_t>(sum));
            }
        }
        write_wide(state, instruction.rd, result);
        break;
    }
    case Operation::Psubsb: {
        const auto left = read_wide(state, instruction.rs);
        const auto right = read_wide(state, instruction.rt);
        WideRegister result;
        for (int lane = 0; lane < 16; ++lane) {
            const std::int32_t difference =
                static_cast<std::int32_t>(static_cast<std::int8_t>(left.byte(lane)))
                - static_cast<std::int32_t>(static_cast<std::int8_t>(right.byte(lane)));
            if (difference >= 0x7f) {
                result.set_byte(lane, 0x7f);
            } else if (difference < -0x80) {
                result.set_byte(lane, 0x80);
            } else {
                result.set_byte(lane, static_cast<std::uint8_t>(difference));
            }
        }
        write_wide(state, instruction.rd, result);
        break;
    }
    case Operation::Padduw: {
        const auto left = read_wide(state, instruction.rs);
        const auto right = read_wide(state, instruction.rt);
        WideRegister result;
        for (int lane = 0; lane < 4; ++lane) {
            const std::int64_t sum = static_cast<std::int64_t>(left.word(lane))
                + static_cast<std::int64_t>(right.word(lane));
            result.set_word(lane, sum > 0xffffffffll
                ? 0xffffffffu
                : static_cast<std::uint32_t>(sum));
        }
        write_wide(state, instruction.rd, result);
        break;
    }
    case Operation::Psubuw: {
        const auto left = read_wide(state, instruction.rs);
        const auto right = read_wide(state, instruction.rt);
        WideRegister result;
        for (int lane = 0; lane < 4; ++lane) {
            const std::int64_t difference = static_cast<std::int64_t>(left.word(lane))
                - static_cast<std::int64_t>(right.word(lane));
            result.set_word(lane, difference <= 0
                ? 0u
                : static_cast<std::uint32_t>(difference));
        }
        write_wide(state, instruction.rd, result);
        break;
    }
    case Operation::Padduh: {
        const auto left = read_wide(state, instruction.rs);
        const auto right = read_wide(state, instruction.rt);
        WideRegister result;
        for (int lane = 0; lane < 8; ++lane) {
            const std::int32_t sum = static_cast<std::int32_t>(left.halfword(lane))
                + static_cast<std::int32_t>(right.halfword(lane));
            result.set_halfword(lane, sum > 0xffff
                ? 0xffff
                : static_cast<std::uint16_t>(sum));
        }
        write_wide(state, instruction.rd, result);
        break;
    }
    case Operation::Psubuh: {
        const auto left = read_wide(state, instruction.rs);
        const auto right = read_wide(state, instruction.rt);
        WideRegister result;
        for (int lane = 0; lane < 8; ++lane) {
            const std::int32_t difference = static_cast<std::int32_t>(left.halfword(lane))
                - static_cast<std::int32_t>(right.halfword(lane));
            result.set_halfword(lane, difference <= 0
                ? 0
                : static_cast<std::uint16_t>(difference));
        }
        write_wide(state, instruction.rd, result);
        break;
    }
    case Operation::Paddub: {
        const auto left = read_wide(state, instruction.rs);
        const auto right = read_wide(state, instruction.rt);
        WideRegister result;
        for (int lane = 0; lane < 16; ++lane) {
            const int sum = left.byte(lane) + right.byte(lane);
            result.set_byte(lane, sum > 0xff ? 0xff : static_cast<std::uint8_t>(sum));
        }
        write_wide(state, instruction.rd, result);
        break;
    }
    case Operation::Psubub: {
        const auto left = read_wide(state, instruction.rs);
        const auto right = read_wide(state, instruction.rt);
        WideRegister result;
        for (int lane = 0; lane < 16; ++lane) {
            const int difference = left.byte(lane) - right.byte(lane);
            result.set_byte(lane, difference <= 0 ? 0 : static_cast<std::uint8_t>(difference));
        }
        write_wide(state, instruction.rd, result);
        break;
    }
    case Operation::Pcgtw: {
        const auto left = read_wide(state, instruction.rs);
        const auto right = read_wide(state, instruction.rt);
        WideRegister result;
        for (int lane = 0; lane < 4; ++lane) {
            result.set_word(lane,
                            static_cast<std::int32_t>(left.word(lane))
                                    > static_cast<std::int32_t>(right.word(lane))
                                ? 0xffffffffu
                                : 0u);
        }
        write_wide(state, instruction.rd, result);
        break;
    }
    case Operation::Pcgth: {
        const auto left = read_wide(state, instruction.rs);
        const auto right = read_wide(state, instruction.rt);
        WideRegister result;
        for (int lane = 0; lane < 8; ++lane) {
            result.set_halfword(lane,
                                static_cast<std::int16_t>(left.halfword(lane))
                                        > static_cast<std::int16_t>(right.halfword(lane))
                                    ? 0xffff
                                    : 0);
        }
        write_wide(state, instruction.rd, result);
        break;
    }
    case Operation::Pcgtb: {
        const auto left = read_wide(state, instruction.rs);
        const auto right = read_wide(state, instruction.rt);
        WideRegister result;
        for (int lane = 0; lane < 16; ++lane) {
            result.set_byte(lane,
                            static_cast<std::int8_t>(left.byte(lane))
                                    > static_cast<std::int8_t>(right.byte(lane))
                                ? 0xff
                                : 0);
        }
        write_wide(state, instruction.rd, result);
        break;
    }
    case Operation::Pceqw: {
        const auto left = read_wide(state, instruction.rs);
        const auto right = read_wide(state, instruction.rt);
        WideRegister result;
        for (int lane = 0; lane < 4; ++lane) {
            result.set_word(lane, left.word(lane) == right.word(lane) ? 0xffffffffu : 0u);
        }
        write_wide(state, instruction.rd, result);
        break;
    }
    case Operation::Pceqh: {
        const auto left = read_wide(state, instruction.rs);
        const auto right = read_wide(state, instruction.rt);
        WideRegister result;
        for (int lane = 0; lane < 8; ++lane) {
            result.set_halfword(lane, left.halfword(lane) == right.halfword(lane) ? 0xffff : 0);
        }
        write_wide(state, instruction.rd, result);
        break;
    }
    case Operation::Pceqb: {
        const auto left = read_wide(state, instruction.rs);
        const auto right = read_wide(state, instruction.rt);
        WideRegister result;
        for (int lane = 0; lane < 16; ++lane) {
            result.set_byte(lane, left.byte(lane) == right.byte(lane) ? 0xff : 0);
        }
        write_wide(state, instruction.rd, result);
        break;
    }
    case Operation::Pmaxw: {
        const auto left = read_wide(state, instruction.rs);
        const auto right = read_wide(state, instruction.rt);
        WideRegister result;
        for (int lane = 0; lane < 4; ++lane) {
            result.set_word(lane,
                            static_cast<std::int32_t>(left.word(lane))
                                    > static_cast<std::int32_t>(right.word(lane))
                                ? left.word(lane)
                                : right.word(lane));
        }
        write_wide(state, instruction.rd, result);
        break;
    }
    case Operation::Pmaxh: {
        const auto left = read_wide(state, instruction.rs);
        const auto right = read_wide(state, instruction.rt);
        WideRegister result;
        for (int lane = 0; lane < 8; ++lane) {
            result.set_halfword(lane,
                                static_cast<std::int16_t>(left.halfword(lane))
                                        > static_cast<std::int16_t>(right.halfword(lane))
                                    ? left.halfword(lane)
                                    : right.halfword(lane));
        }
        write_wide(state, instruction.rd, result);
        break;
    }
    case Operation::Pminw: {
        const auto left = read_wide(state, instruction.rs);
        const auto right = read_wide(state, instruction.rt);
        WideRegister result;
        for (int lane = 0; lane < 4; ++lane) {
            result.set_word(lane,
                            static_cast<std::int32_t>(left.word(lane))
                                    < static_cast<std::int32_t>(right.word(lane))
                                ? left.word(lane)
                                : right.word(lane));
        }
        write_wide(state, instruction.rd, result);
        break;
    }
    case Operation::Pminh: {
        const auto left = read_wide(state, instruction.rs);
        const auto right = read_wide(state, instruction.rt);
        WideRegister result;
        for (int lane = 0; lane < 8; ++lane) {
            result.set_halfword(lane,
                                static_cast<std::int16_t>(left.halfword(lane))
                                        < static_cast<std::int16_t>(right.halfword(lane))
                                    ? left.halfword(lane)
                                    : right.halfword(lane));
        }
        write_wide(state, instruction.rd, result);
        break;
    }
    case Operation::Pabsw: {
        const auto source = read_wide(state, instruction.rt);
        WideRegister result;
        for (int lane = 0; lane < 4; ++lane) {
            const std::uint32_t value = source.word(lane);
            if (value == 0x80000000u) {
                result.set_word(lane, 0x7fffffffu);  // clamp, like the reference
            } else if ((value & 0x80000000u) != 0) {
                result.set_word(lane, 0u - value);
            } else {
                result.set_word(lane, value);
            }
        }
        write_wide(state, instruction.rd, result);
        break;
    }
    case Operation::Pabsh: {
        const auto source = read_wide(state, instruction.rt);
        WideRegister result;
        for (int lane = 0; lane < 8; ++lane) {
            const std::uint16_t value = source.halfword(lane);
            if (value == 0x8000) {
                result.set_halfword(lane, 0x7fff);
            } else if ((value & 0x8000) != 0) {
                result.set_halfword(lane, static_cast<std::uint16_t>(0u - value));
            } else {
                result.set_halfword(lane, value);
            }
        }
        write_wide(state, instruction.rd, result);
        break;
    }
    case Operation::Pand: {
        const auto left = read_wide(state, instruction.rs);
        const auto right = read_wide(state, instruction.rt);
        WideRegister result;
        result.low = left.low & right.low;
        result.high = left.high & right.high;
        write_wide(state, instruction.rd, result);
        break;
    }
    case Operation::Por: {
        const auto left = read_wide(state, instruction.rs);
        const auto right = read_wide(state, instruction.rt);
        WideRegister result;
        result.low = left.low | right.low;
        result.high = left.high | right.high;
        write_wide(state, instruction.rd, result);
        break;
    }
    case Operation::Pxor: {
        const auto left = read_wide(state, instruction.rs);
        const auto right = read_wide(state, instruction.rt);
        WideRegister result;
        result.low = left.low ^ right.low;
        result.high = left.high ^ right.high;
        write_wide(state, instruction.rd, result);
        break;
    }
    case Operation::Pnor: {
        const auto left = read_wide(state, instruction.rs);
        const auto right = read_wide(state, instruction.rt);
        WideRegister result;
        result.low = ~(left.low | right.low);
        result.high = ~(left.high | right.high);
        write_wide(state, instruction.rd, result);
        break;
    }
    case Operation::Psllh: {
        const auto source = read_wide(state, instruction.rt);
        const unsigned amount = instruction.shift_amount & 0x0fu;
        WideRegister result;
        for (int lane = 0; lane < 8; ++lane) {
            result.set_halfword(lane, static_cast<std::uint16_t>(
                source.halfword(lane) << amount));
        }
        write_wide(state, instruction.rd, result);
        break;
    }
    case Operation::Psrlh: {
        const auto source = read_wide(state, instruction.rt);
        const unsigned amount = instruction.shift_amount & 0x0fu;
        WideRegister result;
        for (int lane = 0; lane < 8; ++lane) {
            result.set_halfword(lane, static_cast<std::uint16_t>(
                source.halfword(lane) >> amount));
        }
        write_wide(state, instruction.rd, result);
        break;
    }
    case Operation::Psrah: {
        const auto source = read_wide(state, instruction.rt);
        const unsigned amount = instruction.shift_amount & 0x0fu;
        WideRegister result;
        for (int lane = 0; lane < 8; ++lane) {
            result.set_halfword(lane, arithmetic_shift_right_16(
                source.halfword(lane), static_cast<std::uint8_t>(amount)));
        }
        write_wide(state, instruction.rd, result);
        break;
    }
    case Operation::Psllw: {
        const auto source = read_wide(state, instruction.rt);
        WideRegister result;
        for (int lane = 0; lane < 4; ++lane) {
            result.set_word(lane, source.word(lane) << instruction.shift_amount);
        }
        write_wide(state, instruction.rd, result);
        break;
    }
    case Operation::Psrlw: {
        const auto source = read_wide(state, instruction.rt);
        WideRegister result;
        for (int lane = 0; lane < 4; ++lane) {
            result.set_word(lane, source.word(lane) >> instruction.shift_amount);
        }
        write_wide(state, instruction.rd, result);
        break;
    }
    case Operation::Psraw: {
        const auto source = read_wide(state, instruction.rt);
        WideRegister result;
        for (int lane = 0; lane < 4; ++lane) {
            result.set_word(lane, arithmetic_shift_right_32(
                source.word(lane), instruction.shift_amount));
        }
        write_wide(state, instruction.rd, result);
        break;
    }
    case Operation::Psllvw: {
        const auto amounts = read_wide(state, instruction.rs);
        const auto source = read_wide(state, instruction.rt);
        WideRegister result;
        result.low = sign_extend_32_to_64(
            source.word(0) << (amounts.word(0) & 0x1fu));
        result.high = sign_extend_32_to_64(
            source.word(2) << (amounts.word(2) & 0x1fu));
        write_wide(state, instruction.rd, result);
        break;
    }
    case Operation::Psrlvw: {
        const auto amounts = read_wide(state, instruction.rs);
        const auto source = read_wide(state, instruction.rt);
        WideRegister result;
        result.low = sign_extend_32_to_64(
            source.word(0) >> (amounts.word(0) & 0x1fu));
        result.high = sign_extend_32_to_64(
            source.word(2) >> (amounts.word(2) & 0x1fu));
        write_wide(state, instruction.rd, result);
        break;
    }
    case Operation::Psravw: {
        const auto amounts = read_wide(state, instruction.rs);
        const auto source = read_wide(state, instruction.rt);
        WideRegister result;
        result.low = sign_extend_32_to_64(arithmetic_shift_right_32(
            source.word(0), static_cast<std::uint8_t>(amounts.word(0) & 0x1fu)));
        result.high = sign_extend_32_to_64(arithmetic_shift_right_32(
            source.word(2), static_cast<std::uint8_t>(amounts.word(2) & 0x1fu)));
        write_wide(state, instruction.rd, result);
        break;
    }
    case Operation::Pextlw: {
        const auto left = read_wide(state, instruction.rs);
        const auto right = read_wide(state, instruction.rt);
        WideRegister result;
        result.set_word(0, right.word(0));
        result.set_word(1, left.word(0));
        result.set_word(2, right.word(1));
        result.set_word(3, left.word(1));
        write_wide(state, instruction.rd, result);
        break;
    }
    case Operation::Pextuw: {
        const auto left = read_wide(state, instruction.rs);
        const auto right = read_wide(state, instruction.rt);
        WideRegister result;
        result.set_word(0, right.word(2));
        result.set_word(1, left.word(2));
        result.set_word(2, right.word(3));
        result.set_word(3, left.word(3));
        write_wide(state, instruction.rd, result);
        break;
    }
    case Operation::Pextlh: {
        const auto left = read_wide(state, instruction.rs);
        const auto right = read_wide(state, instruction.rt);
        WideRegister result;
        for (int lane = 0; lane < 4; ++lane) {
            result.set_halfword(2 * lane, right.halfword(lane));
            result.set_halfword(2 * lane + 1, left.halfword(lane));
        }
        write_wide(state, instruction.rd, result);
        break;
    }
    case Operation::Pextuh: {
        const auto left = read_wide(state, instruction.rs);
        const auto right = read_wide(state, instruction.rt);
        WideRegister result;
        for (int lane = 0; lane < 4; ++lane) {
            result.set_halfword(2 * lane, right.halfword(4 + lane));
            result.set_halfword(2 * lane + 1, left.halfword(4 + lane));
        }
        write_wide(state, instruction.rd, result);
        break;
    }
    case Operation::Pextlb: {
        const auto left = read_wide(state, instruction.rs);
        const auto right = read_wide(state, instruction.rt);
        WideRegister result;
        for (int lane = 0; lane < 8; ++lane) {
            result.set_byte(2 * lane, right.byte(lane));
            result.set_byte(2 * lane + 1, left.byte(lane));
        }
        write_wide(state, instruction.rd, result);
        break;
    }
    case Operation::Pextub: {
        const auto left = read_wide(state, instruction.rs);
        const auto right = read_wide(state, instruction.rt);
        WideRegister result;
        for (int lane = 0; lane < 8; ++lane) {
            result.set_byte(2 * lane, right.byte(8 + lane));
            result.set_byte(2 * lane + 1, left.byte(8 + lane));
        }
        write_wide(state, instruction.rd, result);
        break;
    }
    case Operation::Ppacw: {
        const auto left = read_wide(state, instruction.rs);
        const auto right = read_wide(state, instruction.rt);
        WideRegister result;
        result.set_word(0, right.word(0));
        result.set_word(1, right.word(2));
        result.set_word(2, left.word(0));
        result.set_word(3, left.word(2));
        write_wide(state, instruction.rd, result);
        break;
    }
    case Operation::Ppach: {
        const auto left = read_wide(state, instruction.rs);
        const auto right = read_wide(state, instruction.rt);
        WideRegister result;
        for (int lane = 0; lane < 4; ++lane) {
            result.set_halfword(lane, right.halfword(2 * lane));
            result.set_halfword(4 + lane, left.halfword(2 * lane));
        }
        write_wide(state, instruction.rd, result);
        break;
    }
    case Operation::Ppacb: {
        const auto left = read_wide(state, instruction.rs);
        const auto right = read_wide(state, instruction.rt);
        WideRegister result;
        for (int lane = 0; lane < 8; ++lane) {
            result.set_byte(lane, right.byte(2 * lane));
            result.set_byte(8 + lane, left.byte(2 * lane));
        }
        write_wide(state, instruction.rd, result);
        break;
    }
    case Operation::Pext5: {
        const auto source = read_wide(state, instruction.rt);
        WideRegister result;
        for (int lane = 0; lane < 4; ++lane) {
            const std::uint32_t value = source.word(lane);
            result.set_word(lane,
                            ((value & 0x0000001fu) << 3) | ((value & 0x000003e0u) << 6)
                                | ((value & 0x00007c00u) << 9) | ((value & 0x00008000u) << 16));
        }
        write_wide(state, instruction.rd, result);
        break;
    }
    case Operation::Ppac5: {
        const auto source = read_wide(state, instruction.rt);
        WideRegister result;
        for (int lane = 0; lane < 4; ++lane) {
            const std::uint32_t value = source.word(lane);
            result.set_word(lane,
                            ((value >> 3) & 0x0000001fu) | ((value >> 6) & 0x000003e0u)
                                | ((value >> 9) & 0x00007c00u) | ((value >> 16) & 0x00008000u));
        }
        write_wide(state, instruction.rd, result);
        break;
    }
    case Operation::Padsbh: {
        const auto left = read_wide(state, instruction.rs);
        const auto right = read_wide(state, instruction.rt);
        WideRegister result;
        for (int lane = 0; lane < 4; ++lane) {
            result.set_halfword(lane, static_cast<std::uint16_t>(
                left.halfword(lane) - right.halfword(lane)));
        }
        for (int lane = 4; lane < 8; ++lane) {
            result.set_halfword(lane, static_cast<std::uint16_t>(
                left.halfword(lane) + right.halfword(lane)));
        }
        write_wide(state, instruction.rd, result);
        break;
    }
    case Operation::Pinth: {
        const auto left = read_wide(state, instruction.rs);
        const auto right = read_wide(state, instruction.rt);
        WideRegister result;
        result.set_halfword(0, right.halfword(0));
        result.set_halfword(1, left.halfword(4));
        result.set_halfword(2, right.halfword(1));
        result.set_halfword(3, left.halfword(5));
        result.set_halfword(4, right.halfword(2));
        result.set_halfword(5, left.halfword(6));
        result.set_halfword(6, right.halfword(3));
        result.set_halfword(7, left.halfword(7));
        write_wide(state, instruction.rd, result);
        break;
    }
    case Operation::Pinteh: {
        const auto left = read_wide(state, instruction.rs);
        const auto right = read_wide(state, instruction.rt);
        WideRegister result;
        result.set_halfword(0, right.halfword(0));
        result.set_halfword(1, left.halfword(0));
        result.set_halfword(2, right.halfword(2));
        result.set_halfword(3, left.halfword(2));
        result.set_halfword(4, right.halfword(4));
        result.set_halfword(5, left.halfword(4));
        result.set_halfword(6, right.halfword(6));
        result.set_halfword(7, left.halfword(6));
        write_wide(state, instruction.rd, result);
        break;
    }
    case Operation::Pcpyld: {
        const auto left = read_wide(state, instruction.rs);
        const auto right = read_wide(state, instruction.rt);
        WideRegister result;
        result.high = left.low;
        result.low = right.low;
        write_wide(state, instruction.rd, result);
        break;
    }
    case Operation::Pcpyud: {
        const auto left = read_wide(state, instruction.rs);
        const auto right = read_wide(state, instruction.rt);
        WideRegister result;
        result.low = left.high;
        result.high = right.high;
        write_wide(state, instruction.rd, result);
        break;
    }
    case Operation::Pcpyh: {
        const auto source = read_wide(state, instruction.rt);
        WideRegister result;
        for (int lane = 0; lane < 4; ++lane) {
            result.set_halfword(lane, source.halfword(0));
            result.set_halfword(4 + lane, source.halfword(4));
        }
        write_wide(state, instruction.rd, result);
        break;
    }
    case Operation::Pexeh: {
        const auto source = read_wide(state, instruction.rt);
        WideRegister result;
        result.set_halfword(0, source.halfword(2));
        result.set_halfword(1, source.halfword(1));
        result.set_halfword(2, source.halfword(0));
        result.set_halfword(3, source.halfword(3));
        result.set_halfword(4, source.halfword(6));
        result.set_halfword(5, source.halfword(5));
        result.set_halfword(6, source.halfword(4));
        result.set_halfword(7, source.halfword(7));
        write_wide(state, instruction.rd, result);
        break;
    }
    case Operation::Prevh: {
        const auto source = read_wide(state, instruction.rt);
        WideRegister result;
        result.set_halfword(0, source.halfword(3));
        result.set_halfword(1, source.halfword(2));
        result.set_halfword(2, source.halfword(1));
        result.set_halfword(3, source.halfword(0));
        result.set_halfword(4, source.halfword(7));
        result.set_halfword(5, source.halfword(6));
        result.set_halfword(6, source.halfword(5));
        result.set_halfword(7, source.halfword(4));
        write_wide(state, instruction.rd, result);
        break;
    }
    case Operation::Pexew: {
        const auto source = read_wide(state, instruction.rt);
        WideRegister result;
        result.set_word(0, source.word(2));
        result.set_word(1, source.word(1));
        result.set_word(2, source.word(0));
        result.set_word(3, source.word(3));
        write_wide(state, instruction.rd, result);
        break;
    }
    case Operation::Prot3w: {
        const auto source = read_wide(state, instruction.rt);
        WideRegister result;
        result.set_word(0, source.word(1));
        result.set_word(1, source.word(2));
        result.set_word(2, source.word(0));
        result.set_word(3, source.word(3));
        write_wide(state, instruction.rd, result);
        break;
    }
    case Operation::Pexch: {
        const auto source = read_wide(state, instruction.rt);
        WideRegister result;
        result.set_halfword(0, source.halfword(0));
        result.set_halfword(1, source.halfword(2));
        result.set_halfword(2, source.halfword(1));
        result.set_halfword(3, source.halfword(3));
        result.set_halfword(4, source.halfword(4));
        result.set_halfword(5, source.halfword(6));
        result.set_halfword(6, source.halfword(5));
        result.set_halfword(7, source.halfword(7));
        write_wide(state, instruction.rd, result);
        break;
    }
    case Operation::Pexcw: {
        const auto source = read_wide(state, instruction.rt);
        WideRegister result;
        result.set_word(0, source.word(0));
        result.set_word(1, source.word(2));
        result.set_word(2, source.word(1));
        result.set_word(3, source.word(3));
        write_wide(state, instruction.rd, result);
        break;
    }
    case Operation::Pmfhi: {
        WideRegister result;
        result.low = state.hi();
        result.high = state.hi1();
        write_wide(state, instruction.rd, result);
        break;
    }
    case Operation::Pmflo: {
        WideRegister result;
        result.low = state.lo();
        result.high = state.lo1();
        write_wide(state, instruction.rd, result);
        break;
    }
    case Operation::Pmthi: {
        const auto source = read_wide(state, instruction.rs);
        state.set_hi(source.low);
        state.set_hi1(source.high);
        break;
    }
    case Operation::Pmtlo: {
        const auto source = read_wide(state, instruction.rs);
        state.set_lo(source.low);
        state.set_lo1(source.high);
        break;
    }
    case Operation::Pmfhl: {
        // The five formats pack the two 128-bit HI/LO registers differently.
        WideRegister result;
        switch (instruction.shift_amount) {
        case 0x00:  // .lw
            result.set_word(0, static_cast<std::uint32_t>(state.lo()));
            result.set_word(1, static_cast<std::uint32_t>(state.hi()));
            result.set_word(2, static_cast<std::uint32_t>(state.lo1()));
            result.set_word(3, static_cast<std::uint32_t>(state.hi1()));
            break;
        case 0x01:  // .uw
            result.set_word(0, static_cast<std::uint32_t>(state.lo() >> 32));
            result.set_word(1, static_cast<std::uint32_t>(state.hi() >> 32));
            result.set_word(2, static_cast<std::uint32_t>(state.lo1() >> 32));
            result.set_word(3, static_cast<std::uint32_t>(state.hi1() >> 32));
            break;
        case 0x02: {  // .slw
            const auto pack_signed_word = [](std::uint64_t high_register,
                                             std::uint64_t low_register) {
                const std::uint64_t high_word = static_cast<std::uint32_t>(high_register);
                const std::uint64_t low_word = static_cast<std::uint32_t>(low_register);
                const auto combined = static_cast<std::int64_t>((high_word << 32) | low_word);
                if (combined >= 0x7fffffffll) {
                    return static_cast<std::uint64_t>(0x000000007fffffffll);
                }
                if (combined <= -0x80000000ll) {
                    return static_cast<std::uint64_t>(0xffffffff80000000ull);
                }
                return sign_extend_32_to_64(static_cast<std::uint32_t>(low_word));
            };
            result.low = pack_signed_word(state.hi(), state.lo());
            result.high = pack_signed_word(state.hi1(), state.lo1());
            break;
        }
        case 0x03:  // .lh
            result.set_halfword(0, static_cast<std::uint16_t>(state.lo()));
            result.set_halfword(1, static_cast<std::uint16_t>(state.lo() >> 32));
            result.set_halfword(2, static_cast<std::uint16_t>(state.hi()));
            result.set_halfword(3, static_cast<std::uint16_t>(state.hi() >> 32));
            result.set_halfword(4, static_cast<std::uint16_t>(state.lo1()));
            result.set_halfword(5, static_cast<std::uint16_t>(state.lo1() >> 32));
            result.set_halfword(6, static_cast<std::uint16_t>(state.hi1()));
            result.set_halfword(7, static_cast<std::uint16_t>(state.hi1() >> 32));
            break;
        case 0x04:  // .sh
            result.set_halfword(0, pmfhl_clamp(static_cast<std::uint32_t>(state.lo())));
            result.set_halfword(1, pmfhl_clamp(static_cast<std::uint32_t>(state.lo() >> 32)));
            result.set_halfword(2, pmfhl_clamp(static_cast<std::uint32_t>(state.hi())));
            result.set_halfword(3, pmfhl_clamp(static_cast<std::uint32_t>(state.hi() >> 32)));
            result.set_halfword(4, pmfhl_clamp(static_cast<std::uint32_t>(state.lo1())));
            result.set_halfword(5, pmfhl_clamp(static_cast<std::uint32_t>(state.lo1() >> 32)));
            result.set_halfword(6, pmfhl_clamp(static_cast<std::uint32_t>(state.hi1())));
            result.set_halfword(7, pmfhl_clamp(static_cast<std::uint32_t>(state.hi1() >> 32)));
            break;
        default:
            throw std::runtime_error("pmfhl with an unmodeled layout selector");
        }
        write_wide(state, instruction.rd, result);
        break;
    }
    case Operation::Pmthl: {
        // Only the .lw layout exists; other selectors are undefined.
        if (instruction.shift_amount != 0) {
            throw std::runtime_error("pmthl with a nonzero layout selector is not modeled");
        }
        state.set_lo(static_cast<std::uint32_t>(state.read_gpr64(instruction.rs)));
        state.set_hi(static_cast<std::uint32_t>(state.read_gpr64(instruction.rs) >> 32));
        state.set_lo1(static_cast<std::uint32_t>(state.read_gpr_high64(instruction.rs)));
        state.set_hi1(static_cast<std::uint32_t>(state.read_gpr_high64(instruction.rs) >> 32));
        break;
    }
    case Operation::Qfsrv: {
        // The funnel shift takes its amount from the MTSA/MTSAB/MTSAH cache
        // and concatenates rs (upper) with rt (lower), eight bits per step.
        const auto source = read_wide(state, instruction.rs);
        const auto data = read_wide(state, instruction.rt);
        const std::uint32_t shift = state.shift_amount_cache() << 3;
        WideRegister result;
        if (shift == 0) {
            result = data;
        } else if (shift < 64) {
            result.low = (data.low >> shift) | (data.high << (64 - shift));
            result.high = (data.high >> shift) | (source.low << (64 - shift));
        } else if (shift < 128) {
            result.low = data.high >> (shift - 64);
            result.high = source.low >> (shift - 64);
            if (shift != 64) {
                result.low |= source.low << (128 - shift);
                result.high |= source.high << (128 - shift);
            }
        } else {
            throw std::runtime_error("qfsrv with a shift amount beyond 127 is not modeled");
        }
        write_wide(state, instruction.rd, result);
        break;
    }
    case Operation::Plzcw: {
        // The two output words are the leading-sign counts of the source's
        // low 64 bits minus one; the register's upper half is untouched.
        const std::uint32_t low_word = state.read_gpr32(instruction.rs);
        const std::uint32_t high_word =
            static_cast<std::uint32_t>(state.read_gpr64(instruction.rs) >> 32);
        const std::uint32_t low_count = count_leading_sign_bits(low_word) - 1;
        const std::uint32_t high_count = count_leading_sign_bits(high_word) - 1;
        state.write_gpr64(instruction.rd,
                          (static_cast<std::uint64_t>(high_count) << 32) | low_count);
        break;
    }
    // The parallel multiply and divide family: the low pass works on the low
    // words of the operands and the low accumulator pair, the second pass on
    // their high halves and the "1" pair.
    case Operation::Pmaddh:
    case Operation::Pmsubh: {
        const auto left = read_wide(state, instruction.rs);
        const auto right = read_wide(state, instruction.rt);
        const bool subtract = instruction.operation == Operation::Pmsubh;
        const auto combine = [&](std::uint32_t accumulated, int lane) {
            const std::uint32_t product =
                parallel_halfword_product(left, right, lane);
            return subtract ? accumulated - product : accumulated + product;
        };
        write_lo_lane(state, 0, combine(lo_lane(state, 0), 0));
        write_lo_lane(state, 1, combine(lo_lane(state, 1), 1));
        write_hi_lane(state, 0, combine(hi_lane(state, 0), 2));
        write_hi_lane(state, 1, combine(hi_lane(state, 1), 3));
        write_lo_lane(state, 2, combine(lo_lane(state, 2), 4));
        write_lo_lane(state, 3, combine(lo_lane(state, 3), 5));
        write_hi_lane(state, 2, combine(hi_lane(state, 2), 6));
        write_hi_lane(state, 3, combine(hi_lane(state, 3), 7));
        write_parallel_packing(state, instruction.rd);
        break;
    }
    case Operation::Pmulth: {
        const auto left = read_wide(state, instruction.rs);
        const auto right = read_wide(state, instruction.rt);
        write_lo_lane(state, 0, parallel_halfword_product(left, right, 0));
        write_lo_lane(state, 1, parallel_halfword_product(left, right, 1));
        write_hi_lane(state, 0, parallel_halfword_product(left, right, 2));
        write_hi_lane(state, 1, parallel_halfword_product(left, right, 3));
        write_lo_lane(state, 2, parallel_halfword_product(left, right, 4));
        write_lo_lane(state, 3, parallel_halfword_product(left, right, 5));
        write_hi_lane(state, 2, parallel_halfword_product(left, right, 6));
        write_hi_lane(state, 3, parallel_halfword_product(left, right, 7));
        write_parallel_packing(state, instruction.rd);
        break;
    }
    case Operation::Pmultw: {
        const auto multiply = [](std::uint32_t left, std::uint32_t right) {
            return static_cast<std::uint64_t>(
                static_cast<std::int64_t>(static_cast<std::int32_t>(left))
                * static_cast<std::int64_t>(static_cast<std::int32_t>(right)));
        };
        const std::uint64_t low_product =
            multiply(state.read_gpr32(instruction.rs), state.read_gpr32(instruction.rt));
        write_hilo_low(state, low_product);
        if (instruction.rd != 0) {
            state.write_gpr64(instruction.rd, low_product);
        }
        const std::uint64_t high_product =
            multiply(static_cast<std::uint32_t>(state.read_gpr_high64(instruction.rs)),
                     static_cast<std::uint32_t>(state.read_gpr_high64(instruction.rt)));
        write_hilo_high(state, high_product);
        if (instruction.rd != 0) {
            state.write_gpr_high64(instruction.rd, high_product);
        }
        break;
    }
    case Operation::Pmultuw: {
        const auto multiply = [](std::uint32_t left, std::uint32_t right) {
            return static_cast<std::uint64_t>(left) * static_cast<std::uint64_t>(right);
        };
        const std::uint64_t low_product =
            multiply(state.read_gpr32(instruction.rs), state.read_gpr32(instruction.rt));
        write_hilo_low(state, low_product);
        if (instruction.rd != 0) {
            state.write_gpr64(instruction.rd, low_product);
        }
        const std::uint64_t high_product =
            multiply(static_cast<std::uint32_t>(state.read_gpr_high64(instruction.rs)),
                     static_cast<std::uint32_t>(state.read_gpr_high64(instruction.rt)));
        write_hilo_high(state, high_product);
        if (instruction.rd != 0) {
            state.write_gpr_high64(instruction.rd, high_product);
        }
        break;
    }
    case Operation::Pmadduw: {
        const auto accumulate = [](std::uint32_t left, std::uint32_t right,
                                   std::uint32_t accumulator_low,
                                   std::uint32_t accumulator_high) {
            const std::uint64_t accumulator = static_cast<std::uint64_t>(accumulator_low)
                | (static_cast<std::uint64_t>(accumulator_high) << 32);
            return accumulator + static_cast<std::uint64_t>(left) * static_cast<std::uint64_t>(right);
        };
        const std::uint64_t low_sum = accumulate(state.read_gpr32(instruction.rs),
                                                 state.read_gpr32(instruction.rt),
                                                 lo_lane(state, 0), hi_lane(state, 0));
        write_hilo_low(state, low_sum);
        if (instruction.rd != 0) {
            state.write_gpr64(instruction.rd, low_sum);
        }
        const std::uint64_t high_sum = accumulate(
            static_cast<std::uint32_t>(state.read_gpr_high64(instruction.rs)),
            static_cast<std::uint32_t>(state.read_gpr_high64(instruction.rt)),
            lo_lane(state, 2), hi_lane(state, 2));
        write_hilo_high(state, high_sum);
        if (instruction.rd != 0) {
            state.write_gpr_high64(instruction.rd, high_sum);
        }
        break;
    }
    case Operation::Pdivw: {
        const auto divide = [](std::uint32_t left_bits, std::uint32_t right_bits) {
            const auto left = static_cast<std::int32_t>(left_bits);
            const auto right = static_cast<std::int32_t>(right_bits);
            std::int32_t quotient = 0;
            std::int32_t remainder = 0;
            if (left_bits == 0x80000000u && right_bits == 0xffffffffu) {
                quotient = static_cast<std::int32_t>(0x80000000u);
            } else if (right != 0) {
                quotient = left / right;
                remainder = left % right;
            } else {
                quotient = left < 0 ? 1 : -1;
                remainder = left;
            }
            return static_cast<std::uint64_t>(static_cast<std::uint32_t>(quotient))
                | (static_cast<std::uint64_t>(static_cast<std::uint32_t>(remainder)) << 32);
        };
        write_hilo_low(state, divide(state.read_gpr32(instruction.rs),
                                     state.read_gpr32(instruction.rt)));
        write_hilo_high(state, divide(
            static_cast<std::uint32_t>(state.read_gpr_high64(instruction.rs)),
            static_cast<std::uint32_t>(state.read_gpr_high64(instruction.rt))));
        break;
    }
    case Operation::Pdivuw: {
        const auto divide = [](std::uint32_t left, std::uint32_t right) {
            std::uint32_t quotient = 0;
            std::uint32_t remainder = 0;
            if (right != 0) {
                quotient = left / right;
                remainder = left % right;
            } else {
                quotient = 0xffffffffu;  // the reference's -1
                remainder = left;
            }
            return static_cast<std::uint64_t>(quotient)
                | (static_cast<std::uint64_t>(remainder) << 32);
        };
        write_hilo_low(state, divide(state.read_gpr32(instruction.rs),
                                     state.read_gpr32(instruction.rt)));
        write_hilo_high(state, divide(
            static_cast<std::uint32_t>(state.read_gpr_high64(instruction.rs)),
            static_cast<std::uint32_t>(state.read_gpr_high64(instruction.rt))));
        break;
    }
    default:
        return false;
    }
    return true;
}

void execute_plain(const DecodedInstruction& instruction, GuestState& state) {
    switch (instruction.operation) {
    case Operation::Addu:
        state.write_gpr32(instruction.rd,
                          state.read_gpr32(instruction.rs) + state.read_gpr32(instruction.rt));
        break;
    case Operation::Subu:
        state.write_gpr32(instruction.rd,
                          state.read_gpr32(instruction.rs) - state.read_gpr32(instruction.rt));
        break;
    case Operation::Add:
        state.write_gpr32(instruction.rd, add_checked_32(state.read_gpr32(instruction.rs),
                                                         state.read_gpr32(instruction.rt)));
        break;
    case Operation::Sub:
        // The reference negates the right operand before the overflow check.
        state.write_gpr32(instruction.rd,
                          add_checked_32(state.read_gpr32(instruction.rs),
                                         0u - state.read_gpr32(instruction.rt)));
        break;
    case Operation::Dadd:
        state.write_gpr64(instruction.rd, add_checked_64(state.read_gpr64(instruction.rs),
                                                         state.read_gpr64(instruction.rt)));
        break;
    case Operation::Dsub:
        state.write_gpr64(instruction.rd,
                          add_checked_64(state.read_gpr64(instruction.rs),
                                         0ull - state.read_gpr64(instruction.rt)));
        break;
    case Operation::And:
        state.write_gpr64(instruction.rd,
                          state.read_gpr64(instruction.rs) & state.read_gpr64(instruction.rt));
        break;
    case Operation::Or:
        state.write_gpr64(instruction.rd,
                          state.read_gpr64(instruction.rs) | state.read_gpr64(instruction.rt));
        break;
    case Operation::Xor:
        state.write_gpr64(instruction.rd,
                          state.read_gpr64(instruction.rs) ^ state.read_gpr64(instruction.rt));
        break;
    case Operation::Slt:
        // SLT compares the full 64-bit registers (MIPS64).
        state.write_gpr64(instruction.rd,
                          less_than_signed_64(state.read_gpr64(instruction.rs),
                                              state.read_gpr64(instruction.rt)) ? 1 : 0);
        break;
    case Operation::Sltu:
        state.write_gpr64(instruction.rd,
                          state.read_gpr64(instruction.rs) < state.read_gpr64(instruction.rt) ? 1 : 0);
        break;
    case Operation::Slti:
        // SLTI compares 64-bit values against the sign-extended immediate.
        state.write_gpr64(instruction.rt,
                          less_than_signed_64(
                              state.read_gpr64(instruction.rs),
                              static_cast<std::uint64_t>(
                                  static_cast<std::int64_t>(instruction.signed_immediate()))) ? 1 : 0);
        break;
    case Operation::Sltiu:
        state.write_gpr64(instruction.rt,
                          state.read_gpr64(instruction.rs)
                              < static_cast<std::uint64_t>(
                                  static_cast<std::int64_t>(instruction.signed_immediate())) ? 1 : 0);
        break;
    case Operation::Xori:
        state.write_gpr64(instruction.rt, state.read_gpr64(instruction.rs) ^ instruction.immediate);
        break;
    case Operation::Daddiu:
        // The 64-bit immediate add: the sign-extended immediate joins the
        // full 64-bit register without truncation.
        state.write_gpr64(instruction.rt,
                          state.read_gpr64(instruction.rs)
                              + static_cast<std::uint64_t>(
                                  static_cast<std::int64_t>(instruction.signed_immediate())));
        break;
    case Operation::Nor:
        state.write_gpr64(instruction.rd,
                          ~(state.read_gpr64(instruction.rs) | state.read_gpr64(instruction.rt)));
        break;
    case Operation::Daddu:
        state.write_gpr64(instruction.rd,
                          state.read_gpr64(instruction.rs) + state.read_gpr64(instruction.rt));
        break;
    case Operation::Dsubu:
        // The 64-bit subtract; the trapping DSUB form stays unsupported until
        // the exception path exists.
        state.write_gpr64(instruction.rd,
                          state.read_gpr64(instruction.rs) - state.read_gpr64(instruction.rt));
        break;
    case Operation::Movz:
        // Conditional move: the destination changes only when rt is zero
        // (movz) or nonzero (movn); r0 ignores writes either way.
        if (state.read_gpr64(instruction.rt) == 0) {
            state.write_gpr64(instruction.rd, state.read_gpr64(instruction.rs));
        }
        break;
    case Operation::Movn:
        if (state.read_gpr64(instruction.rt) != 0) {
            state.write_gpr64(instruction.rd, state.read_gpr64(instruction.rs));
        }
        break;
    case Operation::Addiu:
        state.write_gpr32(instruction.rt,
                          state.read_gpr32(instruction.rs)
                              + static_cast<std::uint32_t>(instruction.signed_immediate()));
        break;
    case Operation::Addi:
        state.write_gpr32(instruction.rt,
                          add_checked_32(state.read_gpr32(instruction.rs),
                                         static_cast<std::uint32_t>(instruction.signed_immediate())));
        break;
    case Operation::Daddi:
        state.write_gpr64(
            instruction.rt,
            add_checked_64(state.read_gpr64(instruction.rs),
                           static_cast<std::uint64_t>(instruction.signed_immediate())));
        break;
    case Operation::Andi:
        state.write_gpr64(instruction.rt, state.read_gpr64(instruction.rs) & instruction.immediate);
        break;
    case Operation::Ori:
        state.write_gpr64(instruction.rt, state.read_gpr64(instruction.rs) | instruction.immediate);
        break;
    case Operation::Lui:
        state.write_gpr32(instruction.rt, static_cast<std::uint32_t>(instruction.immediate) << 16);
        break;
    case Operation::Sll:
        state.write_gpr32(instruction.rd,
                          state.read_gpr32(instruction.rt) << instruction.shift_amount);
        break;
    case Operation::Srl:
        state.write_gpr32(instruction.rd,
                          state.read_gpr32(instruction.rt) >> instruction.shift_amount);
        break;
    case Operation::Sra:
        state.write_gpr32(instruction.rd,
                          arithmetic_shift_right_32(state.read_gpr32(instruction.rt),
                                                    instruction.shift_amount));
        break;
    case Operation::Sllv:
        state.write_gpr32(instruction.rd, state.read_gpr32(instruction.rt)
            << (state.read_gpr32(instruction.rs) & 0x1fu));
        break;
    case Operation::Srlv:
        state.write_gpr32(instruction.rd, state.read_gpr32(instruction.rt)
            >> (state.read_gpr32(instruction.rs) & 0x1fu));
        break;
    case Operation::Srav:
        state.write_gpr32(instruction.rd, arithmetic_shift_right_32(
            state.read_gpr32(instruction.rt),
            static_cast<std::uint8_t>(state.read_gpr32(instruction.rs) & 0x1fu)));
        break;
    case Operation::Dsll:
        state.write_gpr64(instruction.rd,
            state.read_gpr64(instruction.rt) << instruction.shift_amount);
        break;
    case Operation::Dsrl:
        state.write_gpr64(instruction.rd,
            state.read_gpr64(instruction.rt) >> instruction.shift_amount);
        break;
    case Operation::Dsra:
        state.write_gpr64(instruction.rd, arithmetic_shift_right_64(
            state.read_gpr64(instruction.rt), instruction.shift_amount));
        break;
    case Operation::Dsll32:
        // The "32" forms add 32 to the five-bit shift amount.
        state.write_gpr64(instruction.rd,
            state.read_gpr64(instruction.rt) << (instruction.shift_amount + 32));
        break;
    case Operation::Dsrl32:
        state.write_gpr64(instruction.rd,
            state.read_gpr64(instruction.rt) >> (instruction.shift_amount + 32));
        break;
    case Operation::Dsra32:
        state.write_gpr64(instruction.rd, arithmetic_shift_right_64(
            state.read_gpr64(instruction.rt),
            static_cast<std::uint8_t>(instruction.shift_amount + 32)));
        break;
    case Operation::Dsllv:
        state.write_gpr64(instruction.rd, state.read_gpr64(instruction.rt)
            << (state.read_gpr32(instruction.rs) & 0x3fu));
        break;
    case Operation::Dsrlv:
        state.write_gpr64(instruction.rd, state.read_gpr64(instruction.rt)
            >> (state.read_gpr32(instruction.rs) & 0x3fu));
        break;
    case Operation::Dsrav:
        state.write_gpr64(instruction.rd, arithmetic_shift_right_64(
            state.read_gpr64(instruction.rt),
            static_cast<std::uint8_t>(state.read_gpr32(instruction.rs) & 0x3fu)));
        break;
    case Operation::Lw: {
        const auto address = effective_address(state, instruction);
        state.write_gpr32(instruction.rt, state.memory().read_word(address));
        break;
    }
    case Operation::Lh: {
        const auto address = effective_address(state, instruction);
        state.write_gpr32(instruction.rt, sign_extended_16(state.memory().read_halfword(address)));
        break;
    }
    case Operation::Lb: {
        const auto address = effective_address(state, instruction);
        state.write_gpr32(instruction.rt, sign_extended_8(state.memory().read_byte(address)));
        break;
    }
    case Operation::Lbu: {
        const auto address = effective_address(state, instruction);
        state.write_gpr32(instruction.rt, state.memory().read_byte(address));
        break;
    }
    case Operation::Ld: {
        const auto address = effective_address(state, instruction);
        state.write_gpr64(instruction.rt, state.memory().read_doubleword(address));
        break;
    }
    case Operation::Lq: {
        // LQ silently aligns to 16 bytes and moves the full 128-bit register.
        const std::uint32_t address = effective_address(state, instruction) & ~0xfu;
        state.write_gpr64(instruction.rt, state.memory().read_doubleword(address));
        state.write_gpr_high64(instruction.rt, state.memory().read_doubleword(address + 8));
        break;
    }
    case Operation::Sw: {
        const auto address = effective_address(state, instruction);
        state.memory().write_word(address, state.read_gpr32(instruction.rt));
        break;
    }
    case Operation::Sb: {
        const auto address = effective_address(state, instruction);
        state.memory().write_byte(address,
                                  static_cast<std::uint8_t>(state.read_gpr64(instruction.rt) & 0xff));
        break;
    }
    case Operation::Sd: {
        const auto address = effective_address(state, instruction);
        state.memory().write_doubleword(address, state.read_gpr64(instruction.rt));
        break;
    }
    case Operation::Sq: {
        // SQ also aligns to 16 bytes and stores both halves of the register.
        const std::uint32_t address = effective_address(state, instruction) & ~0xfu;
        state.memory().write_doubleword(address, state.read_gpr64(instruction.rt));
        state.memory().write_doubleword(address + 8, state.read_gpr_high64(instruction.rt));
        break;
    }
    case Operation::Lhu: {
        const auto address = effective_address(state, instruction);
        state.write_gpr64(instruction.rt, state.memory().read_halfword(address));
        break;
    }
    case Operation::Lwu: {
        const auto address = effective_address(state, instruction);
        state.write_gpr64(instruction.rt, state.memory().read_word(address));
        break;
    }
    case Operation::Sh: {
        const auto address = effective_address(state, instruction);
        state.memory().write_halfword(address, static_cast<std::uint16_t>(
            state.read_gpr64(instruction.rt) & 0xffffu));
        break;
    }
    case Operation::Lwl: {
        // Unaligned word assembly: the addressed bytes merge into the low
        // word and the result sign-extends (the reference's mask/shift tables).
        const std::uint32_t address = effective_address(state, instruction);
        const std::uint32_t shift = address & 3u;
        const std::uint32_t word = state.memory().read_word(address & ~3u);
        state.write_gpr32(instruction.rt,
                          (state.read_gpr32(instruction.rt) & lwl_mask[shift])
                              | (word << merge_shift[shift]));
        break;
    }
    case Operation::Lwr: {
        const std::uint32_t address = effective_address(state, instruction);
        const std::uint32_t shift = address & 3u;
        const std::uint32_t word = state.memory().read_word(address & ~3u);
        const std::uint32_t merged = (state.read_gpr32(instruction.rt) & lwr_mask[shift])
            | (word >> place_shift[shift]);
        if (shift == 0) {
            // The aligned case sign-extends the whole register; the others
            // replace only the low 32-bit word and keep the upper half.
            state.write_gpr32(instruction.rt, merged);
        } else {
            state.write_gpr_low32(instruction.rt, merged);
        }
        break;
    }
    case Operation::Swl: {
        const std::uint32_t address = effective_address(state, instruction);
        const std::uint32_t shift = address & 3u;
        const std::uint32_t aligned = address & ~3u;
        const std::uint32_t word = state.memory().read_word(aligned);
        state.memory().write_word(aligned,
            (state.read_gpr32(instruction.rt) >> merge_shift[shift]) | (word & swl_mask[shift]));
        break;
    }
    case Operation::Swr: {
        const std::uint32_t address = effective_address(state, instruction);
        const std::uint32_t shift = address & 3u;
        const std::uint32_t aligned = address & ~3u;
        const std::uint32_t word = state.memory().read_word(aligned);
        state.memory().write_word(aligned,
            (state.read_gpr32(instruction.rt) << place_shift[shift]) | (word & swr_mask[shift]));
        break;
    }
    case Operation::Cache:
        // The hint has no effect in this model, like the reference.
        break;
    case Operation::Pref:
        // The prefetch hint has no effect either.
        break;
    case Operation::Ldl: {
        // The 64-bit sibling of LWL: the bytes merge into the full register.
        const std::uint32_t address = effective_address(state, instruction);
        const std::uint32_t shift = address & 7u;
        const std::uint64_t word = state.memory().read_doubleword(address & ~7u);
        state.write_gpr64(instruction.rt,
                          (state.read_gpr64(instruction.rt) & ldl_mask[shift])
                              | (word << doubleword_merge_shift[shift]));
        break;
    }
    case Operation::Ldr: {
        const std::uint32_t address = effective_address(state, instruction);
        const std::uint32_t shift = address & 7u;
        const std::uint64_t word = state.memory().read_doubleword(address & ~7u);
        state.write_gpr64(instruction.rt,
                          (state.read_gpr64(instruction.rt) & ldr_mask[shift])
                              | (word >> doubleword_place_shift[shift]));
        break;
    }
    case Operation::Sdl: {
        const std::uint32_t address = effective_address(state, instruction);
        const std::uint32_t shift = address & 7u;
        const std::uint32_t aligned = address & ~7u;
        const std::uint64_t word = state.memory().read_doubleword(aligned);
        state.memory().write_doubleword(aligned,
            (state.read_gpr64(instruction.rt) >> doubleword_merge_shift[shift])
                | (word & sdl_mask[shift]));
        break;
    }
    case Operation::Sdr: {
        const std::uint32_t address = effective_address(state, instruction);
        const std::uint32_t shift = address & 7u;
        const std::uint32_t aligned = address & ~7u;
        const std::uint64_t word = state.memory().read_doubleword(aligned);
        state.memory().write_doubleword(aligned,
            (state.read_gpr64(instruction.rt) << doubleword_place_shift[shift])
                | (word & sdr_mask[shift]));
        break;
    }
    default:
        if (execute_special_register(instruction, state)
            || execute_cop1(instruction, state) || execute_cop2(instruction, state)
            || execute_mmi(instruction, state)) {
            return;
        }
        throw std::logic_error("non-plain instruction reached the plain executor");
    }
}

// Executes one plain instruction, turning the trapping arithmetic's overflow
// into the step's stable Exception outcome; returns true when it completed.
bool execute_plain_allowing_trap(const DecodedInstruction& instruction, GuestState& state) {
    try {
        execute_plain(instruction, state);
        return true;
    } catch (const IntegerOverflow&) {
        return false;
    }
}

} // namespace

bool execute_plain_effect(GuestState& state, const DecodedInstruction& instruction) {
    return execute_plain_allowing_trap(instruction, state);
}

Interpreter::Interpreter(GuestState& state) : state_(state) {}

bool Interpreter::pending_transfer() const noexcept {
    return transfer_pending_;
}

StepResult Interpreter::step() {
    const auto pc = state_.pc();
    const auto instruction = decode(state_.memory().read_word(pc));
    const auto flow = classify(instruction, pc);

    if (transfer_pending_) {
        // We are executing a delay slot. A transfer here is architecturally
        // undefined; stop before executing it. An undecodable word stops as
        // unsupported. Everything else executes before the pending transfer.
        if (flow.kind == FlowKind::Unsupported) {
            return StepResult{StepOutcome::Unsupported, pc, instruction.operation};
        }
        if (flow.kind == FlowKind::Exception) {
            // A trap in the delay slot fires before the pending transfer; the
            // handler is not modeled, so stop at the trapping word.
            return StepResult{StepOutcome::Exception, pc, instruction.operation};
        }
        if (flow.kind != FlowKind::FallThrough) {
            return StepResult{StepOutcome::IllegalDelaySlot, pc, instruction.operation};
        }
        if (!execute_plain_allowing_trap(instruction, state_)) {
            // The overflow fires before the pending transfer; stopping here
            // leaves the same word, so the outcome is stable.
            return StepResult{StepOutcome::Exception, pc, instruction.operation};
        }
        transfer_pending_ = false;
        state_.set_pc(transfer_target_);
        return StepResult{StepOutcome::Executed, pc, instruction.operation};
    }

    switch (flow.kind) {
    case FlowKind::Unsupported:
        return StepResult{StepOutcome::Unsupported, pc, instruction.operation};
    case FlowKind::Exception:
        // The exception handler is not modeled; stop at the boundary.
        return StepResult{StepOutcome::Exception, pc, instruction.operation};
    case FlowKind::FallThrough:
        if (!execute_plain_allowing_trap(instruction, state_)) {
            return StepResult{StepOutcome::Exception, pc, instruction.operation};
        }
        state_.set_pc(pc + 4);
        return StepResult{StepOutcome::Executed, pc, instruction.operation};
    case FlowKind::Branch: {
        if (branch_taken(instruction, state_)) {
            if (writes_link_register(instruction.operation)) {
                state_.write_gpr64(link_register, pc + 8);
            }
            transfer_target_ = flow.target;
            transfer_pending_ = true;
            state_.set_pc(pc + 4);
        } else if (is_likely_branch(instruction.operation)) {
            state_.set_pc(pc + 8);  // the delay slot is nullified
        } else {
            state_.set_pc(pc + 4);  // the delay slot still runs
        }
        return StepResult{StepOutcome::Executed, pc, instruction.operation};
    }
    case FlowKind::Jump:
        transfer_target_ = flow.target;
        transfer_pending_ = true;
        state_.set_pc(pc + 4);
        return StepResult{StepOutcome::Executed, pc, instruction.operation};
    case FlowKind::Call:
        if (instruction.operation == Operation::Jal) {
            state_.write_gpr64(link_register, pc + 8);
            transfer_target_ = flow.target;
        } else {
            // JALR: read the target before writing the link register, so a
            // shared rd == rs encoding still jumps to the old value.
            const auto target = static_cast<std::uint32_t>(state_.read_gpr64(instruction.rs));
            state_.write_gpr64(instruction.rd, pc + 8);
            transfer_target_ = target;
        }
        transfer_pending_ = true;
        state_.set_pc(pc + 4);
        return StepResult{StepOutcome::Executed, pc, instruction.operation};
    case FlowKind::Return:
    case FlowKind::IndirectJump:
        if (instruction.operation == Operation::Eret) {
            // The exception return applies immediately (no delay slot): the
            // target comes from EPC or ErrorEPC by the error level, which the
            // return clears, exactly like the reference.
            const std::uint32_t status = state_.read_cp0(12);
            if ((status & 0x00000004u) != 0) {
                state_.set_pc(state_.read_cp0(30));
                state_.write_cp0(12, status & ~0x00000004u);
            } else {
                state_.set_pc(state_.read_cp0(14));
                state_.write_cp0(12, status & ~0x00000002u);
            }
            return StepResult{StepOutcome::Executed, pc, instruction.operation};
        }
        // JR targets the low 32 bits of the register in the 32-bit model.
        transfer_target_ = static_cast<std::uint32_t>(state_.read_gpr64(instruction.rs));
        transfer_pending_ = true;
        state_.set_pc(pc + 4);
        return StepResult{StepOutcome::Executed, pc, instruction.operation};
    }
    throw std::logic_error("unhandled flow kind");
}

} // namespace gt4recomp::ee
