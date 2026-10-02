#include "gt4recomp/ee_state.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace gt4recomp::ee {
namespace {

std::string access_text(const char* problem, std::uint32_t address, std::size_t width) {
    std::ostringstream message;
    message << "Guest access at 0x" << std::hex << std::setfill('0') << std::setw(8) << address
            << std::dec << " (width " << width << ") " << problem;
    return message.str();
}

} // namespace

GuestMemory::GuestMemory(std::uint32_t base, std::size_t size_bytes)
    : base_(base), bytes_(size_bytes) {
    if (size_bytes == 0) {
        throw std::runtime_error("Guest memory region must have a nonzero size");
    }
    if (static_cast<std::uint64_t>(base) + size_bytes > 0x100000000ull) {
        throw std::runtime_error("Guest memory region must fit the 32-bit address space");
    }
}

std::uint32_t GuestMemory::base() const noexcept {
    return base_;
}

std::size_t GuestMemory::size() const noexcept {
    return bytes_.size();
}

bool GuestMemory::contains(std::uint32_t address, std::size_t width) const noexcept {
    const std::uint32_t physical = physical_address(address);
    const std::uint64_t end = static_cast<std::uint64_t>(physical) + width;
    return width != 0 && physical >= base_
        && end <= static_cast<std::uint64_t>(base_) + bytes_.size();
}

void GuestMemory::enable_kseg0_alias() noexcept {
    kseg0_alias_ = true;
}

bool GuestMemory::kseg0_alias_enabled() const noexcept {
    return kseg0_alias_;
}

std::uint32_t GuestMemory::physical_address(std::uint32_t address) const noexcept {
    if (kseg0_alias_ && address >= 0x80000000u) {
        return address - 0x80000000u;
    }
    return address;
}

void GuestMemory::require_alignment(std::uint32_t address, std::size_t width) const {
    if (width > 1 && address % width != 0) {
        throw std::runtime_error(access_text("is not naturally aligned", address, width));
    }
}

std::size_t GuestMemory::range_offset(std::uint32_t address, std::size_t width) const {
    if (!contains(address, width)) {
        throw std::runtime_error(access_text("is outside the mapped region", address, width));
    }
    return static_cast<std::size_t>(physical_address(address) - base_);
}

// All assembly uses explicit unsigned shifts: no host signed overflow and no
// reinterpretation of the byte buffer as a wider type.

std::uint8_t GuestMemory::read_byte(std::uint32_t address) const {
    return bytes_[range_offset(address, 1)];
}

std::uint16_t GuestMemory::read_halfword(std::uint32_t address) const {
    require_alignment(address, 2);
    const auto offset = range_offset(address, 2);
    return static_cast<std::uint16_t>(static_cast<std::uint32_t>(bytes_[offset])
        | (static_cast<std::uint32_t>(bytes_[offset + 1]) << 8));
}

std::uint32_t GuestMemory::read_word(std::uint32_t address) const {
    require_alignment(address, 4);
    const auto offset = range_offset(address, 4);
    return static_cast<std::uint32_t>(bytes_[offset])
        | (static_cast<std::uint32_t>(bytes_[offset + 1]) << 8)
        | (static_cast<std::uint32_t>(bytes_[offset + 2]) << 16)
        | (static_cast<std::uint32_t>(bytes_[offset + 3]) << 24);
}

std::uint64_t GuestMemory::read_doubleword(std::uint32_t address) const {
    require_alignment(address, 8);
    const auto offset = range_offset(address, 8);
    return static_cast<std::uint64_t>(bytes_[offset])
        | (static_cast<std::uint64_t>(bytes_[offset + 1]) << 8)
        | (static_cast<std::uint64_t>(bytes_[offset + 2]) << 16)
        | (static_cast<std::uint64_t>(bytes_[offset + 3]) << 24)
        | (static_cast<std::uint64_t>(bytes_[offset + 4]) << 32)
        | (static_cast<std::uint64_t>(bytes_[offset + 5]) << 40)
        | (static_cast<std::uint64_t>(bytes_[offset + 6]) << 48)
        | (static_cast<std::uint64_t>(bytes_[offset + 7]) << 56);
}

void GuestMemory::write_byte(std::uint32_t address, std::uint8_t value) {
    bytes_[range_offset(address, 1)] = value;
}

void GuestMemory::write_halfword(std::uint32_t address, std::uint16_t value) {
    require_alignment(address, 2);
    const auto offset = range_offset(address, 2);
    bytes_[offset] = static_cast<std::uint8_t>(value & 0xff);
    bytes_[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xff);
}

void GuestMemory::write_word(std::uint32_t address, std::uint32_t value) {
    require_alignment(address, 4);
    const auto offset = range_offset(address, 4);
    bytes_[offset] = static_cast<std::uint8_t>(value & 0xff);
    bytes_[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xff);
    bytes_[offset + 2] = static_cast<std::uint8_t>((value >> 16) & 0xff);
    bytes_[offset + 3] = static_cast<std::uint8_t>((value >> 24) & 0xff);
}

void GuestMemory::write_doubleword(std::uint32_t address, std::uint64_t value) {
    require_alignment(address, 8);
    const auto offset = range_offset(address, 8);
    for (std::size_t index = 0; index < 8; ++index) {
        bytes_[offset + index] = static_cast<std::uint8_t>((value >> (8 * index)) & 0xff);
    }
}

void GuestMemory::write_bytes(std::uint32_t address, std::span<const std::uint8_t> source) {
    if (source.empty()) {
        return;
    }
    const auto offset = range_offset(address, source.size());
    std::copy(source.begin(), source.end(),
              bytes_.begin() + static_cast<std::ptrdiff_t>(offset));
}

GuestState::GuestState(GuestMemory memory) : memory_(std::move(memory)) {
    // The live observation (M14) recorded the running game's Status as
    // 0x40000000 (CU2 usable); the model starts there rather than at a cold
    // reset value, because the code under test comes from a running game.
    cp0_[12] = 0x40000000u;
}

RegisterContext GuestState::save_registers() const noexcept {
    RegisterContext context;
    context.gpr = gpr_;
    context.gpr_high = gpr_high_;
    context.fpr = fpr_;
    context.hi = hi_;
    context.lo = lo_;
    context.hi1 = hi1_;
    context.lo1 = lo1_;
    context.fpu_accumulator = fpu_accumulator_;
    context.fpu_control = fpu_control_;
    context.shift_amount_cache = shift_amount_cache_;
    context.cp0 = cp0_;
    context.vu0_vf = vu0_vf_;
    context.vu0_vi = vu0_vi_;
    context.vu0_clip_flag = vu0_clip_flag_;
    context.vu0_acc = vu0_acc_;
    context.vu0_mac_flag = vu0_mac_flag_;
    context.vu0_status_flag = vu0_status_flag_;
    context.pc = pc_;
    return context;
}

void GuestState::restore_registers(const RegisterContext& context) noexcept {
    gpr_ = context.gpr;
    gpr_high_ = context.gpr_high;
    fpr_ = context.fpr;
    hi_ = context.hi;
    lo_ = context.lo;
    hi1_ = context.hi1;
    lo1_ = context.lo1;
    fpu_accumulator_ = context.fpu_accumulator;
    fpu_control_ = context.fpu_control;
    shift_amount_cache_ = context.shift_amount_cache;
    cp0_ = context.cp0;
    vu0_vf_ = context.vu0_vf;
    vu0_vi_ = context.vu0_vi;
    vu0_clip_flag_ = context.vu0_clip_flag;
    vu0_acc_ = context.vu0_acc;
    vu0_mac_flag_ = context.vu0_mac_flag;
    vu0_status_flag_ = context.vu0_status_flag;
    pc_ = context.pc;
}

void GuestState::require_gpr_index(std::uint8_t index) {
    if (index >= 32) {
        throw std::runtime_error("Guest register index must be below 32");
    }
}

std::uint64_t GuestState::read_gpr64(std::uint8_t index) const {
    require_gpr_index(index);
    return index == 0 ? 0 : gpr_[index];
}

void GuestState::write_gpr64(std::uint8_t index, std::uint64_t value) {
    require_gpr_index(index);
    if (index == 0) {
        return;  // R0 is constant zero; every write is ignored.
    }
    gpr_[index] = value;
}

std::uint32_t GuestState::read_gpr32(std::uint8_t index) const {
    return static_cast<std::uint32_t>(read_gpr64(index));
}

void GuestState::write_gpr32(std::uint8_t index, std::uint32_t value) {
    // 32-bit results sign-extend into the 64-bit register; the mask avoids
    // relying on implementation-defined signed conversion.
    const std::uint64_t extended = (value & 0x80000000u) != 0
        ? (0xffffffff00000000ull | value)
        : value;
    write_gpr64(index, extended);
}

void GuestState::write_gpr_low32(std::uint8_t index, std::uint32_t value) {
    // Only bits 0-31 change; the rest of the register (including the upper
    // half) is preserved, matching the unaligned-load rule this serves.
    require_gpr_index(index);
    if (index == 0) {
        return;
    }
    gpr_[index] = (gpr_[index] & 0xffffffff00000000ull) | value;
}

std::uint64_t GuestState::read_gpr_high64(std::uint8_t index) const {
    require_gpr_index(index);
    return index == 0 ? 0 : gpr_high_[index];
}

void GuestState::write_gpr_high64(std::uint8_t index, std::uint64_t value) {
    require_gpr_index(index);
    if (index == 0) {
        return;  // The upper half of r0 is constant zero as well.
    }
    gpr_high_[index] = value;
}

void GuestState::require_fpr_index(std::uint8_t index) {
    if (index >= 32) {
        throw std::runtime_error("Guest FPU register index must be below 32");
    }
}

std::uint32_t GuestState::read_fpr(std::uint8_t index) const {
    require_fpr_index(index);
    return fpr_[index];
}

void GuestState::write_fpr(std::uint8_t index, std::uint32_t value) {
    require_fpr_index(index);
    fpr_[index] = value;
}

std::uint32_t GuestState::fpu_accumulator() const noexcept {
    return fpu_accumulator_;
}

void GuestState::set_fpu_accumulator(std::uint32_t value) noexcept {
    fpu_accumulator_ = value;
}

std::uint32_t GuestState::fpu_control() const noexcept {
    return fpu_control_;
}

void GuestState::set_fpu_control(std::uint32_t value) noexcept {
    fpu_control_ = value;
}

std::uint64_t GuestState::hi() const noexcept {
    return hi_;
}

void GuestState::set_hi(std::uint64_t value) noexcept {
    hi_ = value;
}

std::uint64_t GuestState::lo() const noexcept {
    return lo_;
}

void GuestState::set_lo(std::uint64_t value) noexcept {
    lo_ = value;
}

std::uint64_t GuestState::hi1() const noexcept {
    return hi1_;
}

void GuestState::set_hi1(std::uint64_t value) noexcept {
    hi1_ = value;
}

std::uint64_t GuestState::lo1() const noexcept {
    return lo1_;
}

void GuestState::set_lo1(std::uint64_t value) noexcept {
    lo1_ = value;
}

std::uint32_t GuestState::shift_amount_cache() const noexcept {
    return shift_amount_cache_;
}

void GuestState::set_shift_amount_cache(std::uint32_t value) noexcept {
    shift_amount_cache_ = value;
}

std::uint32_t GuestState::read_cp0(std::uint8_t index) const {
    if (index >= 32) {
        throw std::runtime_error("CP0 register index must be below 32");
    }
    return cp0_[index];
}

void GuestState::write_cp0(std::uint8_t index, std::uint32_t value) {
    if (index >= 32) {
        throw std::runtime_error("CP0 register index must be below 32");
    }
    cp0_[index] = value;
}

std::uint32_t GuestState::read_vf_lane(std::uint8_t index, std::uint8_t lane) const {
    if (index >= 32 || lane >= 4) {
        throw std::runtime_error("VU0 register or lane index out of range");
    }
    if (index == 0) {
        // The constant register reads as (0, 0, 0, 1.0).
        return lane == 3 ? 0x3f800000u : 0u;
    }
    return vu0_vf_[index][lane];
}

void GuestState::write_vf_lane(std::uint8_t index, std::uint8_t lane, std::uint32_t value) {
    if (index >= 32 || lane >= 4) {
        throw std::runtime_error("VU0 register or lane index out of range");
    }
    if (index == 0) {
        return;  // the constant register ignores writes
    }
    vu0_vf_[index][lane] = value;
}

std::uint32_t GuestState::read_vi(std::uint8_t index) const {
    if (index >= 32) {
        throw std::runtime_error("VU0 integer register index out of range");
    }
    return index == 0 ? 0 : vu0_vi_[index];
}

void GuestState::write_vi(std::uint8_t index, std::uint32_t value) {
    if (index >= 32) {
        throw std::runtime_error("VU0 integer register index out of range");
    }
    if (index == 0) {
        return;  // VI0 is hardwired zero
    }
    vu0_vi_[index] = value;
}

std::uint32_t GuestState::vu0_clip_flag() const noexcept {
    return vu0_clip_flag_;
}

void GuestState::set_vu0_clip_flag(std::uint32_t value) noexcept {
    vu0_clip_flag_ = value;
}

std::uint32_t GuestState::read_acc_lane(std::uint8_t lane) const {
    if (lane >= 4) {
        throw std::runtime_error("VU0 accumulator lane index out of range");
    }
    return vu0_acc_[lane];
}

void GuestState::write_acc_lane(std::uint8_t lane, std::uint32_t value) {
    if (lane >= 4) {
        throw std::runtime_error("VU0 accumulator lane index out of range");
    }
    vu0_acc_[lane] = value;
}

std::uint32_t GuestState::vu0_mac_flag() const noexcept {
    return vu0_mac_flag_;
}

void GuestState::set_vu0_mac_flag(std::uint32_t value) noexcept {
    vu0_mac_flag_ = value;
}

std::uint32_t GuestState::vu0_status_flag() const noexcept {
    return vu0_status_flag_;
}

void GuestState::set_vu0_status_flag(std::uint32_t value) noexcept {
    vu0_status_flag_ = value;
}

void GuestState::reset_vu0_registers() noexcept {
    // VF0 stays the constant through the accessors; the storage clears.
    for (auto& vector : vu0_vf_) {
        vector.fill(0);
    }
    vu0_vi_.fill(0);
    vu0_clip_flag_ = 0;
    vu0_acc_.fill(0);
    vu0_mac_flag_ = 0;
    vu0_status_flag_ = 0;
}

std::uint32_t GuestState::pc() const noexcept {
    return pc_;
}

void GuestState::set_pc(std::uint32_t value) noexcept {
    pc_ = value;
}

GuestMemory& GuestState::memory() noexcept {
    return memory_;
}

const GuestMemory& GuestState::memory() const noexcept {
    return memory_;
}

} // namespace gt4recomp::ee
