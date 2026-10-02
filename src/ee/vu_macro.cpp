#include "vu_macro.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>

namespace gt4recomp::ee {
namespace {

// ---------------------------------------------------------------------------
// The reference's floating-point and flag model.
//
// PCSX2's vuDouble flushes denormal inputs to a signed zero and clamps
// infinities to the largest magnitude; its default configuration enables the
// VU0 overflow fix, which this model mirrors. VU_MAC_UPDATE classifies each
// result lane the same way while maintaining the MAC flag bits, and
// VU_STAT_UPDATE aggregates the MAC flags into the status flag. Both land in
// the integer file through the sync the arithmetic wrappers perform.
// ---------------------------------------------------------------------------

std::uint32_t lane_bit(int lane) {
    return 0x8u >> lane;  // lane 0 is x and holds mask bit 3
}

float macro_float(std::uint32_t bits) {
    const std::uint32_t exponent = bits & 0x7f800000u;
    if (exponent == 0u) {
        return std::bit_cast<float>(bits & 0x80000000u);
    }
    if (exponent == 0x7f800000u) {
        return std::bit_cast<float>((bits & 0x80000000u) | 0x7f7fffffu);
    }
    return std::bit_cast<float>(bits);
}

// VU_MAC_UPDATE: sign, zero, denormal and overflow classification for one
// lane, with the denormal flush and the overflow clamp; returns the stored
// result bits.
std::uint32_t mac_update(GuestState& state, int lane_shift, float value) {
    const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
    const std::uint32_t exponent = (bits >> 23) & 0xffu;
    const std::uint32_t sign = bits & 0x80000000u;
    std::uint32_t flags = state.vu0_mac_flag();

    if (sign != 0) {
        flags |= 0x0010u << lane_shift;
    } else {
        flags &= ~(0x0010u << lane_shift);
    }

    if (value == 0.0f) {
        flags = (flags & ~(0x1100u << lane_shift)) | (0x0001u << lane_shift);
        state.set_vu0_mac_flag(flags);
        return bits;
    }

    switch (exponent) {
    case 0:
        flags = (flags & ~(0x1000u << lane_shift)) | (0x0101u << lane_shift);
        state.set_vu0_mac_flag(flags);
        return sign;
    case 255:
        flags = (flags & ~(0x0101u << lane_shift)) | (0x1000u << lane_shift);
        state.set_vu0_mac_flag(flags);
        return sign | 0x7f7fffffu;
    default:
        flags &= ~(0x1101u << lane_shift);
        state.set_vu0_mac_flag(flags);
        return bits;
    }
}

void mac_clear(GuestState& state, int lane_shift) {
    state.set_vu0_mac_flag(state.vu0_mac_flag() & ~(0x1111u << lane_shift));
}

// VU_STAT_UPDATE: one bit per lane group that has any MAC flag set.
void update_status_flag(GuestState& state) {
    const std::uint32_t mac = state.vu0_mac_flag();
    std::uint32_t status = 0;
    if ((mac & 0x000fu) != 0) {
        status = 0x1;
    }
    if ((mac & 0x00f0u) != 0) {
        status |= 0x2;
    }
    if ((mac & 0x0f00u) != 0) {
        status |= 0x4;
    }
    if ((mac & 0xf000u) != 0) {
        status |= 0x8;
    }
    state.set_vu0_status_flag(status);
}

// SYNCMSFLAGS: the status register keeps its high bits and takes the low four
// both directly and six places higher; the MAC register takes the raw value.
void sync_mac_status(GuestState& state) {
    update_status_flag(state);
    const std::uint32_t status = state.vu0_status_flag();
    const std::uint32_t current = state.read_vi(16);
    state.write_vi(16, (current & 0xfc0u) | (status & 0xfu) | ((status & 0xfu) << 6));
    state.write_vi(17, state.vu0_mac_flag());
}

// SYNCFDIV: the division unit publishes its result through the Q register and
// only its own two status bits, leaving the rest of the register alone.
void sync_divide_status(GuestState& state, std::uint32_t q_bits) {
    const std::uint32_t status = state.vu0_status_flag();
    state.write_vi(22, q_bits);
    const std::uint32_t current = state.read_vi(16);
    state.write_vi(16, (current & 0x3cfu) | (status & 0x30u) | ((status & 0x30u) << 6));
}

// ---------------------------------------------------------------------------
// Operand classes shared by the arithmetic families.
// ---------------------------------------------------------------------------

enum class Broadcast { Elementwise, X, Y, Z, W, Q, I };

std::uint32_t broadcast_bits(
    const GuestState& state, Broadcast broadcast, std::uint8_t ft, int lane) {
    switch (broadcast) {
    case Broadcast::Elementwise:
        return state.read_vf_lane(ft, static_cast<std::uint8_t>(lane));
    case Broadcast::X: return state.read_vf_lane(ft, 0);
    case Broadcast::Y: return state.read_vf_lane(ft, 1);
    case Broadcast::Z: return state.read_vf_lane(ft, 2);
    case Broadcast::W: return state.read_vf_lane(ft, 3);
    case Broadcast::Q: return state.read_vi(22);
    case Broadcast::I: return state.read_vi(21);
    }
    return 0;
}

enum class BinaryOp { Add, Sub, Mul };

float apply_binary(BinaryOp op, float left, float right) {
    if (op == BinaryOp::Add) {
        return left + right;
    }
    if (op == BinaryOp::Sub) {
        return left - right;
    }
    return left * right;
}

// The binary accumulators (VADD/VSUB/VMUL and their element, broadcast and
// accumulator forms): fd = fs op ft, or acc = fs op ft for the A-variants.
void apply_binary_mac(GuestState& state, const DecodedInstruction& instruction,
                      BinaryOp op, Broadcast broadcast, bool to_accumulator) {
    const std::uint8_t fs = instruction.rd;
    const std::uint8_t ft = instruction.rt;
    const std::uint8_t fd = instruction.shift_amount;
    for (int lane = 0; lane < 4; ++lane) {
        if ((instruction.rs & lane_bit(lane)) != 0) {
            const float left = macro_float(state.read_vf_lane(fs, static_cast<std::uint8_t>(lane)));
            const float right =
                macro_float(broadcast_bits(state, broadcast, ft, lane));
            const std::uint32_t result =
                mac_update(state, 3 - lane, apply_binary(op, left, right));
            if (to_accumulator) {
                state.write_acc_lane(static_cast<std::uint8_t>(lane), result);
            } else {
                state.write_vf_lane(fd, static_cast<std::uint8_t>(lane), result);
            }
        } else {
            mac_clear(state, 3 - lane);
        }
    }
    sync_mac_status(state);
}

enum class TernaryOp { Madd, Msub };

// The multiply-accumulates: fd = acc op fs * ft, or acc = acc op fs * ft.
void apply_ternary_mac(GuestState& state, const DecodedInstruction& instruction,
                       TernaryOp op, Broadcast broadcast, bool to_accumulator) {
    const std::uint8_t fs = instruction.rd;
    const std::uint8_t ft = instruction.rt;
    const std::uint8_t fd = instruction.shift_amount;
    for (int lane = 0; lane < 4; ++lane) {
        if ((instruction.rs & lane_bit(lane)) != 0) {
            const float accumulator =
                macro_float(state.read_acc_lane(static_cast<std::uint8_t>(lane)));
            const float left = macro_float(state.read_vf_lane(fs, static_cast<std::uint8_t>(lane)));
            const float right =
                macro_float(broadcast_bits(state, broadcast, ft, lane));
            const float product = left * right;
            const float result = op == TernaryOp::Madd ? accumulator + product
                                                       : accumulator - product;
            const std::uint32_t stored = mac_update(state, 3 - lane, result);
            if (to_accumulator) {
                state.write_acc_lane(static_cast<std::uint8_t>(lane), stored);
            } else {
                state.write_vf_lane(fd, static_cast<std::uint8_t>(lane), stored);
            }
        } else {
            mac_clear(state, 3 - lane);
        }
    }
    sync_mac_status(state);
}

// The min/max forms compare integer representations so denormals and special
// numbers order like floats; they maintain no flags.
std::uint32_t fp_max(std::uint32_t left, std::uint32_t right) {
    const auto signed_left = static_cast<std::int32_t>(left);
    const auto signed_right = static_cast<std::int32_t>(right);
    return (signed_left < 0 && signed_right < 0)
               ? static_cast<std::uint32_t>(std::min(signed_left, signed_right))
               : static_cast<std::uint32_t>(std::max(signed_left, signed_right));
}

std::uint32_t fp_min(std::uint32_t left, std::uint32_t right) {
    const auto signed_left = static_cast<std::int32_t>(left);
    const auto signed_right = static_cast<std::int32_t>(right);
    return (signed_left < 0 && signed_right < 0)
               ? static_cast<std::uint32_t>(std::max(signed_left, signed_right))
               : static_cast<std::uint32_t>(std::min(signed_left, signed_right));
}

void apply_minmax(GuestState& state, const DecodedInstruction& instruction,
                  bool maximum, Broadcast broadcast) {
    const std::uint8_t fd = instruction.shift_amount;
    if (fd == 0) {
        return;  // the constant register ignores writes
    }
    const std::uint8_t fs = instruction.rd;
    const std::uint8_t ft = instruction.rt;
    for (int lane = 0; lane < 4; ++lane) {
        if ((instruction.rs & lane_bit(lane)) == 0) {
            continue;
        }
        const std::uint32_t left = state.read_vf_lane(fs, static_cast<std::uint8_t>(lane));
        const std::uint32_t right = broadcast_bits(state, broadcast, ft, lane);
        state.write_vf_lane(fd, static_cast<std::uint8_t>(lane),
                            maximum ? fp_max(left, right) : fp_min(left, right));
    }
}

// The outer product: acc = fs.y * ft.z, fs.z * ft.x, fs.x * ft.y (VOPMULA) and
// fd = acc - the same products (VOPMSUB).
void apply_outer_product_add(GuestState& state, const DecodedInstruction& instruction) {
    const std::uint8_t fs = instruction.rd;
    const std::uint8_t ft = instruction.rt;
    const float fs_x = macro_float(state.read_vf_lane(fs, 0));
    const float fs_y = macro_float(state.read_vf_lane(fs, 1));
    const float fs_z = macro_float(state.read_vf_lane(fs, 2));
    const float ft_x = macro_float(state.read_vf_lane(ft, 0));
    const float ft_y = macro_float(state.read_vf_lane(ft, 1));
    const float ft_z = macro_float(state.read_vf_lane(ft, 2));
    state.write_acc_lane(0, mac_update(state, 3, fs_y * ft_z));
    state.write_acc_lane(1, mac_update(state, 2, fs_z * ft_x));
    state.write_acc_lane(2, mac_update(state, 1, fs_x * ft_y));
    sync_mac_status(state);
}

void apply_outer_product_subtract(
    GuestState& state, const DecodedInstruction& instruction) {
    const std::uint8_t fs = instruction.rd;
    const std::uint8_t ft = instruction.rt;
    const std::uint8_t fd = instruction.shift_amount;
    const float fs_x = macro_float(state.read_vf_lane(fs, 0));
    const float fs_y = macro_float(state.read_vf_lane(fs, 1));
    const float fs_z = macro_float(state.read_vf_lane(fs, 2));
    const float ft_x = macro_float(state.read_vf_lane(ft, 0));
    const float ft_y = macro_float(state.read_vf_lane(ft, 1));
    const float ft_z = macro_float(state.read_vf_lane(ft, 2));
    state.write_vf_lane(fd, 0,
        mac_update(state, 3, macro_float(state.read_acc_lane(0)) - fs_y * ft_z));
    state.write_vf_lane(fd, 1,
        mac_update(state, 2, macro_float(state.read_acc_lane(1)) - fs_z * ft_x));
    state.write_vf_lane(fd, 2,
        mac_update(state, 1, macro_float(state.read_acc_lane(2)) - fs_x * ft_y));
    sync_mac_status(state);
}

// The conversions and the absolute value: vf[ft] = f(vf[fs]) per selected
// lane, with no flag effects.
using UnaryFunction = std::uint32_t (*)(std::uint32_t, unsigned offset);

std::uint32_t absolute_bits(std::uint32_t bits, unsigned) {
    return bits & 0x7fffffffu;
}

std::uint32_t convert_float_to_int(std::uint32_t bits, unsigned offset) {
    float value = std::bit_cast<float>(bits);
    if (offset != 0) {
        value *= std::bit_cast<float>(0x3f800000u + (offset << 23));
    }
    const std::uint32_t scaled = std::bit_cast<std::uint32_t>(value);
    if ((scaled & 0x7f800000u) >= 0x4f000000u) {
        return (scaled & 0x80000000u) != 0 ? 0x80000000u : 0x7fffffffu;
    }
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(value));
}

std::uint32_t convert_int_to_float(std::uint32_t bits, unsigned offset) {
    float value = static_cast<float>(static_cast<std::int32_t>(bits));
    if (offset != 0) {
        value *= std::bit_cast<float>(0x3f800000u - (offset << 23));
    }
    return std::bit_cast<std::uint32_t>(value);
}

void apply_unary(GuestState& state, const DecodedInstruction& instruction,
                 UnaryFunction function, unsigned offset) {
    const std::uint8_t ft = instruction.rt;
    if (ft == 0) {
        return;
    }
    const std::uint8_t fs = instruction.rd;
    for (int lane = 0; lane < 4; ++lane) {
        if ((instruction.rs & lane_bit(lane)) != 0) {
            const std::uint32_t source =
                state.read_vf_lane(fs, static_cast<std::uint8_t>(lane));
            state.write_vf_lane(ft, static_cast<std::uint8_t>(lane),
                                function(source, offset));
        }
    }
}

// VCLIPw: compares three source lanes against the w lane's magnitude and
// shifts the clip flags six places per update.
void apply_clip(GuestState& state, const DecodedInstruction& instruction) {
    std::uint32_t magnitude = state.read_vf_lane(instruction.rt, 3);
    magnitude = (magnitude & 0x7f800000u) != 0 ? magnitude & 0x7fffffffu
                                               : 0x007fffffu;
    const auto limit = static_cast<std::int32_t>(magnitude);
    const std::uint8_t fs = instruction.rd;
    std::uint32_t flags = state.vu0_clip_flag() << 6;
    if (static_cast<std::int32_t>(state.read_vf_lane(fs, 0)) > limit) {
        flags |= 0x01;
    }
    if (static_cast<std::int32_t>(state.read_vf_lane(fs, 0) ^ 0x80000000u) > limit) {
        flags |= 0x02;
    }
    if (static_cast<std::int32_t>(state.read_vf_lane(fs, 1)) > limit) {
        flags |= 0x04;
    }
    if (static_cast<std::int32_t>(state.read_vf_lane(fs, 1) ^ 0x80000000u) > limit) {
        flags |= 0x08;
    }
    if (static_cast<std::int32_t>(state.read_vf_lane(fs, 2)) > limit) {
        flags |= 0x10;
    }
    if (static_cast<std::int32_t>(state.read_vf_lane(fs, 2) ^ 0x80000000u) > limit) {
        flags |= 0x20;
    }
    state.set_vu0_clip_flag(flags & 0xffffffu);
    state.write_vi(18, state.vu0_clip_flag());
}

// VMOVE and VMR32 write vf[ft] from vf[fs], one rotation for the latter.
void apply_move(GuestState& state, const DecodedInstruction& instruction) {
    const std::uint8_t ft = instruction.rt;
    if (ft == 0) {
        return;
    }
    const std::uint8_t fs = instruction.rd;
    for (int lane = 0; lane < 4; ++lane) {
        if ((instruction.rs & lane_bit(lane)) != 0) {
            state.write_vf_lane(ft, static_cast<std::uint8_t>(lane),
                                state.read_vf_lane(fs, static_cast<std::uint8_t>(lane)));
        }
    }
}

void apply_rotate(GuestState& state, const DecodedInstruction& instruction) {
    const std::uint8_t ft = instruction.rt;
    if (ft == 0) {
        return;
    }
    const std::uint8_t fs = instruction.rd;
    const std::uint32_t former_x = state.read_vf_lane(fs, 0);
    if ((instruction.rs & lane_bit(0)) != 0) {
        state.write_vf_lane(ft, 0, state.read_vf_lane(fs, 1));
    }
    if ((instruction.rs & lane_bit(1)) != 0) {
        state.write_vf_lane(ft, 1, state.read_vf_lane(fs, 2));
    }
    if ((instruction.rs & lane_bit(2)) != 0) {
        state.write_vf_lane(ft, 2, state.read_vf_lane(fs, 3));
    }
    if ((instruction.rs & lane_bit(3)) != 0) {
        state.write_vf_lane(ft, 3, former_x);
    }
}

// The division unit. The operand lanes come from bits 24-23 and 22-21 of the
// word, exactly like the reference reads them.
std::uint32_t ft_element(const DecodedInstruction& instruction) {
    return (instruction.word >> 23) & 3u;
}

std::uint32_t fs_element(const DecodedInstruction& instruction) {
    return (instruction.word >> 21) & 3u;
}

void apply_divide(GuestState& state, const DecodedInstruction& instruction) {
    const std::uint32_t divisor = state.read_vf_lane(instruction.rt, ft_element(instruction));
    const std::uint32_t dividend =
        state.read_vf_lane(instruction.rd, fs_element(instruction));
    const float divisor_value = macro_float(divisor);
    const float dividend_value = macro_float(dividend);

    std::uint32_t status = state.vu0_status_flag() & ~0x30u;
    std::uint32_t q_bits = 0;
    if (divisor_value == 0.0f) {
        if (dividend_value == 0.0f) {
            status |= 0x10;
        } else {
            status |= 0x20;
        }
        q_bits = ((divisor ^ dividend) & 0x80000000u) != 0 ? 0xff7fffffu : 0x7f7fffffu;
    } else {
        q_bits = std::bit_cast<std::uint32_t>(
            macro_float(std::bit_cast<std::uint32_t>(dividend_value / divisor_value)));
    }
    state.set_vu0_status_flag(status);
    sync_divide_status(state, q_bits);
}

void apply_sqrt(GuestState& state, const DecodedInstruction& instruction) {
    const float divisor_value =
        macro_float(state.read_vf_lane(instruction.rt, ft_element(instruction)));

    std::uint32_t status = state.vu0_status_flag() & ~0x30u;
    if (divisor_value < 0.0f) {
        status |= 0x10;
    }
    const std::uint32_t q_bits = std::bit_cast<std::uint32_t>(
        macro_float(std::bit_cast<std::uint32_t>(std::sqrt(std::fabs(divisor_value)))));
    state.set_vu0_status_flag(status);
    sync_divide_status(state, q_bits);
}

void apply_reciprocal_sqrt(GuestState& state, const DecodedInstruction& instruction) {
    const std::uint32_t divisor = state.read_vf_lane(instruction.rt, ft_element(instruction));
    const std::uint32_t dividend =
        state.read_vf_lane(instruction.rd, fs_element(instruction));
    const float divisor_value = macro_float(divisor);
    const float dividend_value = macro_float(dividend);

    std::uint32_t status = state.vu0_status_flag() & ~0x30u;
    std::uint32_t q_bits = 0;
    if (divisor_value == 0.0f) {
        status |= 0x20;
        if (dividend_value != 0.0f) {
            q_bits = ((divisor ^ dividend) & 0x80000000u) != 0 ? 0xff7fffffu : 0x7f7fffffu;
        } else {
            q_bits = ((divisor ^ dividend) & 0x80000000u) != 0 ? 0x80000000u : 0u;
            status |= 0x10;
        }
    } else {
        if (divisor_value < 0.0f) {
            status |= 0x10;
        }
        const float root = std::sqrt(std::fabs(divisor_value));
        q_bits = std::bit_cast<std::uint32_t>(
            macro_float(std::bit_cast<std::uint32_t>(dividend_value / root)));
    }
    state.set_vu0_status_flag(status);
    sync_divide_status(state, q_bits);
}

// VMTIR/VMFIR move between the vector file and the integer file; VMTIR writes
// only the low half of the integer entry, like the reference.
void apply_move_to_integer(GuestState& state, const DecodedInstruction& instruction) {
    const std::uint8_t target = instruction.rt & 0xfu;
    if (target == 0) {
        return;
    }
    const std::uint32_t half =
        state.read_vf_lane(instruction.rd, fs_element(instruction)) & 0xffffu;
    state.write_vi(target, (state.read_vi(target) & 0xffff0000u) | half);
}

void apply_move_from_integer(GuestState& state, const DecodedInstruction& instruction) {
    const std::uint8_t ft = instruction.rt;
    if (ft == 0) {
        return;
    }
    const auto value = static_cast<std::int32_t>(
        static_cast<std::int16_t>(state.read_vi(instruction.rd & 0xfu) & 0xffffu));
    for (int lane = 0; lane < 4; ++lane) {
        if ((instruction.rs & lane_bit(lane)) != 0) {
            state.write_vf_lane(ft, static_cast<std::uint8_t>(lane),
                                static_cast<std::uint32_t>(value));
        }
    }
}

// The random generator state lives in the reciprocal register (20).
void advance_lfsr(GuestState& state) {
    std::uint32_t value = state.read_vi(20);
    const std::uint32_t tap_x = (value >> 4) & 1u;
    const std::uint32_t tap_y = (value >> 22) & 1u;
    value <<= 1;
    value ^= tap_x ^ tap_y;
    state.write_vi(20, (value & 0x7fffffu) | 0x3f800000u);
}

void apply_random_init(GuestState& state, const DecodedInstruction& instruction) {
    const std::uint32_t half =
        state.read_vf_lane(instruction.rd, fs_element(instruction)) & 0x007fffffu;
    state.write_vi(20, 0x3f800000u | half);
}

void apply_random_get(GuestState& state, const DecodedInstruction& instruction) {
    const std::uint8_t ft = instruction.rt;
    if (ft == 0) {
        return;
    }
    const std::uint32_t value = state.read_vi(20);
    for (int lane = 0; lane < 4; ++lane) {
        if ((instruction.rs & lane_bit(lane)) != 0) {
            state.write_vf_lane(ft, static_cast<std::uint8_t>(lane), value);
        }
    }
}

void apply_random_next(GuestState& state, const DecodedInstruction& instruction) {
    const std::uint8_t ft = instruction.rt;
    if (ft == 0) {
        return;
    }
    advance_lfsr(state);
    const std::uint32_t value = state.read_vi(20);
    for (int lane = 0; lane < 4; ++lane) {
        if ((instruction.rs & lane_bit(lane)) != 0) {
            state.write_vf_lane(ft, static_cast<std::uint8_t>(lane), value);
        }
    }
}

void apply_random_xor(GuestState& state, const DecodedInstruction& instruction) {
    const std::uint32_t operand =
        state.read_vf_lane(instruction.rd, fs_element(instruction));
    state.write_vi(20, 0x3f800000u | ((state.read_vi(20) ^ operand) & 0x007fffffu));
}

// The integer forms operate on the low 16 bits of the integer file. The
// destination and sources select the low four bits of the fd/ft/fs fields.
void write_vi_low16(GuestState& state, std::uint8_t index, std::uint32_t value) {
    state.write_vi(index, (state.read_vi(index) & 0xffff0000u) | (value & 0xffffu));
}

void apply_integer_add(GuestState& state, const DecodedInstruction& instruction) {
    const std::uint8_t destination = instruction.shift_amount & 0xfu;
    if (destination == 0) {
        return;
    }
    const std::uint32_t left = state.read_vi(instruction.rd & 0xfu);
    const std::uint32_t right = state.read_vi(instruction.rt & 0xfu);
    write_vi_low16(state, destination, left + right);
}

void apply_integer_subtract(GuestState& state, const DecodedInstruction& instruction) {
    const std::uint8_t destination = instruction.shift_amount & 0xfu;
    if (destination == 0) {
        return;
    }
    const std::uint32_t left = state.read_vi(instruction.rd & 0xfu);
    const std::uint32_t right = state.read_vi(instruction.rt & 0xfu);
    write_vi_low16(state, destination, left - right);
}

void apply_integer_add_immediate(
    GuestState& state, const DecodedInstruction& instruction) {
    const std::uint8_t destination = instruction.rt & 0xfu;
    if (destination == 0) {
        return;
    }
    // The five-bit immediate sits in bits 10-6 and sign-extends like the
    // reference's mask.
    const std::uint32_t raw = (instruction.word >> 6) & 0x1fu;
    const std::int32_t immediate = static_cast<std::int32_t>(
        (raw & 0x10u) != 0 ? (raw | 0xfffffff0u) : raw);
    const std::uint32_t left = state.read_vi(instruction.rd & 0xfu);
    write_vi_low16(state, destination,
                   static_cast<std::uint32_t>(left + static_cast<std::uint32_t>(immediate)));
}

void apply_integer_and(GuestState& state, const DecodedInstruction& instruction) {
    const std::uint8_t destination = instruction.shift_amount & 0xfu;
    if (destination == 0) {
        return;
    }
    const std::uint32_t left = state.read_vi(instruction.rd & 0xfu);
    const std::uint32_t right = state.read_vi(instruction.rt & 0xfu);
    write_vi_low16(state, destination, left & right);
}

void apply_integer_or(GuestState& state, const DecodedInstruction& instruction) {
    const std::uint8_t destination = instruction.shift_amount & 0xfu;
    if (destination == 0) {
        return;
    }
    const std::uint32_t left = state.read_vi(instruction.rd & 0xfu);
    const std::uint32_t right = state.read_vi(instruction.rt & 0xfu);
    write_vi_low16(state, destination, left | right);
}

} // namespace

bool execute_vu_macro(const DecodedInstruction& instruction, GuestState& state) {
    switch (instruction.operation) {
    // VADD/VSUB/VMUL and the multiply-accumulates, element and broadcast
    // forms writing fd.
    case Operation::Vaddx: apply_binary_mac(state, instruction, BinaryOp::Add, Broadcast::X, false); break;
    case Operation::Vaddy: apply_binary_mac(state, instruction, BinaryOp::Add, Broadcast::Y, false); break;
    case Operation::Vaddz: apply_binary_mac(state, instruction, BinaryOp::Add, Broadcast::Z, false); break;
    case Operation::Vaddw: apply_binary_mac(state, instruction, BinaryOp::Add, Broadcast::W, false); break;
    case Operation::Vsubx: apply_binary_mac(state, instruction, BinaryOp::Sub, Broadcast::X, false); break;
    case Operation::Vsuby: apply_binary_mac(state, instruction, BinaryOp::Sub, Broadcast::Y, false); break;
    case Operation::Vsubz: apply_binary_mac(state, instruction, BinaryOp::Sub, Broadcast::Z, false); break;
    case Operation::Vsubw: apply_binary_mac(state, instruction, BinaryOp::Sub, Broadcast::W, false); break;
    case Operation::Vmaddx: apply_ternary_mac(state, instruction, TernaryOp::Madd, Broadcast::X, false); break;
    case Operation::Vmaddy: apply_ternary_mac(state, instruction, TernaryOp::Madd, Broadcast::Y, false); break;
    case Operation::Vmaddz: apply_ternary_mac(state, instruction, TernaryOp::Madd, Broadcast::Z, false); break;
    case Operation::Vmaddw: apply_ternary_mac(state, instruction, TernaryOp::Madd, Broadcast::W, false); break;
    case Operation::Vmsubx: apply_ternary_mac(state, instruction, TernaryOp::Msub, Broadcast::X, false); break;
    case Operation::Vmsuby: apply_ternary_mac(state, instruction, TernaryOp::Msub, Broadcast::Y, false); break;
    case Operation::Vmsubz: apply_ternary_mac(state, instruction, TernaryOp::Msub, Broadcast::Z, false); break;
    case Operation::Vmsubw: apply_ternary_mac(state, instruction, TernaryOp::Msub, Broadcast::W, false); break;
    case Operation::Vmulx: apply_binary_mac(state, instruction, BinaryOp::Mul, Broadcast::X, false); break;
    case Operation::Vmuly: apply_binary_mac(state, instruction, BinaryOp::Mul, Broadcast::Y, false); break;
    case Operation::Vmulz: apply_binary_mac(state, instruction, BinaryOp::Mul, Broadcast::Z, false); break;
    case Operation::Vmulw: apply_binary_mac(state, instruction, BinaryOp::Mul, Broadcast::W, false); break;
    case Operation::Vmulq: apply_binary_mac(state, instruction, BinaryOp::Mul, Broadcast::Q, false); break;
    case Operation::Vmuli: apply_binary_mac(state, instruction, BinaryOp::Mul, Broadcast::I, false); break;
    case Operation::Vaddq: apply_binary_mac(state, instruction, BinaryOp::Add, Broadcast::Q, false); break;
    case Operation::Vmaddq: apply_ternary_mac(state, instruction, TernaryOp::Madd, Broadcast::Q, false); break;
    case Operation::Vaddi: apply_binary_mac(state, instruction, BinaryOp::Add, Broadcast::I, false); break;
    case Operation::Vmaddi: apply_ternary_mac(state, instruction, TernaryOp::Madd, Broadcast::I, false); break;
    case Operation::Vsubq: apply_binary_mac(state, instruction, BinaryOp::Sub, Broadcast::Q, false); break;
    case Operation::Vmsubq: apply_ternary_mac(state, instruction, TernaryOp::Msub, Broadcast::Q, false); break;
    case Operation::Vsubi: apply_binary_mac(state, instruction, BinaryOp::Sub, Broadcast::I, false); break;
    case Operation::Vmsubi: apply_ternary_mac(state, instruction, TernaryOp::Msub, Broadcast::I, false); break;
    case Operation::Vadd: apply_binary_mac(state, instruction, BinaryOp::Add, Broadcast::Elementwise, false); break;
    case Operation::Vmadd: apply_ternary_mac(state, instruction, TernaryOp::Madd, Broadcast::Elementwise, false); break;
    case Operation::Vmul: apply_binary_mac(state, instruction, BinaryOp::Mul, Broadcast::Elementwise, false); break;
    case Operation::Vsub: apply_binary_mac(state, instruction, BinaryOp::Sub, Broadcast::Elementwise, false); break;
    case Operation::Vmsub: apply_ternary_mac(state, instruction, TernaryOp::Msub, Broadcast::Elementwise, false); break;
    case Operation::Vopmsub: apply_outer_product_subtract(state, instruction); break;
    // The same families with the accumulator as destination.
    case Operation::Vaddax: apply_binary_mac(state, instruction, BinaryOp::Add, Broadcast::X, true); break;
    case Operation::Vadday: apply_binary_mac(state, instruction, BinaryOp::Add, Broadcast::Y, true); break;
    case Operation::Vaddaz: apply_binary_mac(state, instruction, BinaryOp::Add, Broadcast::Z, true); break;
    case Operation::Vaddaw: apply_binary_mac(state, instruction, BinaryOp::Add, Broadcast::W, true); break;
    case Operation::Vsubax: apply_binary_mac(state, instruction, BinaryOp::Sub, Broadcast::X, true); break;
    case Operation::Vsubay: apply_binary_mac(state, instruction, BinaryOp::Sub, Broadcast::Y, true); break;
    case Operation::Vsubaz: apply_binary_mac(state, instruction, BinaryOp::Sub, Broadcast::Z, true); break;
    case Operation::Vsubaw: apply_binary_mac(state, instruction, BinaryOp::Sub, Broadcast::W, true); break;
    case Operation::Vmaddax: apply_ternary_mac(state, instruction, TernaryOp::Madd, Broadcast::X, true); break;
    case Operation::Vmadday: apply_ternary_mac(state, instruction, TernaryOp::Madd, Broadcast::Y, true); break;
    case Operation::Vmaddaz: apply_ternary_mac(state, instruction, TernaryOp::Madd, Broadcast::Z, true); break;
    case Operation::Vmaddaw: apply_ternary_mac(state, instruction, TernaryOp::Madd, Broadcast::W, true); break;
    case Operation::Vmsubax: apply_ternary_mac(state, instruction, TernaryOp::Msub, Broadcast::X, true); break;
    case Operation::Vmsubay: apply_ternary_mac(state, instruction, TernaryOp::Msub, Broadcast::Y, true); break;
    case Operation::Vmsubaz: apply_ternary_mac(state, instruction, TernaryOp::Msub, Broadcast::Z, true); break;
    case Operation::Vmsubaw: apply_ternary_mac(state, instruction, TernaryOp::Msub, Broadcast::W, true); break;
    case Operation::Vmulax: apply_binary_mac(state, instruction, BinaryOp::Mul, Broadcast::X, true); break;
    case Operation::Vmulay: apply_binary_mac(state, instruction, BinaryOp::Mul, Broadcast::Y, true); break;
    case Operation::Vmulaz: apply_binary_mac(state, instruction, BinaryOp::Mul, Broadcast::Z, true); break;
    case Operation::Vmulaw: apply_binary_mac(state, instruction, BinaryOp::Mul, Broadcast::W, true); break;
    case Operation::Vmulaq: apply_binary_mac(state, instruction, BinaryOp::Mul, Broadcast::Q, true); break;
    case Operation::Vmulai: apply_binary_mac(state, instruction, BinaryOp::Mul, Broadcast::I, true); break;
    case Operation::Vaddaq: apply_binary_mac(state, instruction, BinaryOp::Add, Broadcast::Q, true); break;
    case Operation::Vmaddaq: apply_ternary_mac(state, instruction, TernaryOp::Madd, Broadcast::Q, true); break;
    case Operation::Vaddai: apply_binary_mac(state, instruction, BinaryOp::Add, Broadcast::I, true); break;
    case Operation::Vmaddai: apply_ternary_mac(state, instruction, TernaryOp::Madd, Broadcast::I, true); break;
    case Operation::Vsubaq: apply_binary_mac(state, instruction, BinaryOp::Sub, Broadcast::Q, true); break;
    case Operation::Vmsubaq: apply_ternary_mac(state, instruction, TernaryOp::Msub, Broadcast::Q, true); break;
    case Operation::Vsubai: apply_binary_mac(state, instruction, BinaryOp::Sub, Broadcast::I, true); break;
    case Operation::Vmsubai: apply_ternary_mac(state, instruction, TernaryOp::Msub, Broadcast::I, true); break;
    case Operation::Vadda: apply_binary_mac(state, instruction, BinaryOp::Add, Broadcast::Elementwise, true); break;
    case Operation::Vmadda: apply_ternary_mac(state, instruction, TernaryOp::Madd, Broadcast::Elementwise, true); break;
    case Operation::Vmula: apply_binary_mac(state, instruction, BinaryOp::Mul, Broadcast::Elementwise, true); break;
    case Operation::Vsuba: apply_binary_mac(state, instruction, BinaryOp::Sub, Broadcast::Elementwise, true); break;
    case Operation::Vmsuba: apply_ternary_mac(state, instruction, TernaryOp::Msub, Broadcast::Elementwise, true); break;
    case Operation::Vopmula: apply_outer_product_add(state, instruction); break;
    // Min/max, with no flag effects.
    case Operation::Vmaxx: apply_minmax(state, instruction, true, Broadcast::X); break;
    case Operation::Vmaxy: apply_minmax(state, instruction, true, Broadcast::Y); break;
    case Operation::Vmaxz: apply_minmax(state, instruction, true, Broadcast::Z); break;
    case Operation::Vmaxw: apply_minmax(state, instruction, true, Broadcast::W); break;
    case Operation::Vmaxi: apply_minmax(state, instruction, true, Broadcast::I); break;
    case Operation::Vmax: apply_minmax(state, instruction, true, Broadcast::Elementwise); break;
    case Operation::Vminix: apply_minmax(state, instruction, false, Broadcast::X); break;
    case Operation::Vminiy: apply_minmax(state, instruction, false, Broadcast::Y); break;
    case Operation::Vminiz: apply_minmax(state, instruction, false, Broadcast::Z); break;
    case Operation::Vminiw: apply_minmax(state, instruction, false, Broadcast::W); break;
    case Operation::Vminii: apply_minmax(state, instruction, false, Broadcast::I); break;
    case Operation::Vmini: apply_minmax(state, instruction, false, Broadcast::Elementwise); break;
    // Conversions and the absolute value.
    case Operation::Vitof0: apply_unary(state, instruction, convert_int_to_float, 0); break;
    case Operation::Vitof4: apply_unary(state, instruction, convert_int_to_float, 4); break;
    case Operation::Vitof12: apply_unary(state, instruction, convert_int_to_float, 12); break;
    case Operation::Vitof15: apply_unary(state, instruction, convert_int_to_float, 15); break;
    case Operation::Vftoi0: apply_unary(state, instruction, convert_float_to_int, 0); break;
    case Operation::Vftoi4: apply_unary(state, instruction, convert_float_to_int, 4); break;
    case Operation::Vftoi12: apply_unary(state, instruction, convert_float_to_int, 12); break;
    case Operation::Vftoi15: apply_unary(state, instruction, convert_float_to_int, 15); break;
    case Operation::Vabs: apply_unary(state, instruction, absolute_bits, 0); break;
    // The clip flags, the moves and the division unit.
    case Operation::Vclipw: apply_clip(state, instruction); break;
    case Operation::Vmove: apply_move(state, instruction); break;
    case Operation::Vmr32: apply_rotate(state, instruction); break;
    case Operation::Vdiv: apply_divide(state, instruction); break;
    case Operation::Vsqrt: apply_sqrt(state, instruction); break;
    case Operation::Vrsqrt: apply_reciprocal_sqrt(state, instruction); break;
    case Operation::Vwaitq: break;  // the wait has no effect without a pipeline
    case Operation::Vmtir: apply_move_to_integer(state, instruction); break;
    case Operation::Vmfir: apply_move_from_integer(state, instruction); break;
    // The random generator.
    case Operation::Vrinit: apply_random_init(state, instruction); break;
    case Operation::Vrget: apply_random_get(state, instruction); break;
    case Operation::Vrnext: apply_random_next(state, instruction); break;
    case Operation::Vrxor: apply_random_xor(state, instruction); break;
    // The integer forms.
    case Operation::Viadd: apply_integer_add(state, instruction); break;
    case Operation::Visub: apply_integer_subtract(state, instruction); break;
    case Operation::Viaddi: apply_integer_add_immediate(state, instruction); break;
    case Operation::Viand: apply_integer_and(state, instruction); break;
    case Operation::Vior: apply_integer_or(state, instruction); break;
    case Operation::Vnop:
        // The reference treats it as a full no-operation.
        break;
    default:
        return false;
    }
    return true;
}

} // namespace gt4recomp::ee
