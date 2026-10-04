#include "gt4recomp/ee_compare.hpp"

#include <cstdio>
#include <map>

namespace gt4recomp::ee {
namespace {

void append_value(std::string& text, const char* owner, const char* field,
                  const std::string& left, const std::string& right) {
    text += owner;
    text += ' ';
    text += field;
    text += ": left ";
    text += left;
    text += ", right ";
    text += right;
}

} // namespace

std::string hex_text(std::uint32_t value) {
    char buffer[11] = {};
    std::snprintf(buffer, sizeof buffer, "0x%08x", value);
    return buffer;
}

std::string hex_text64(std::uint64_t value) {
    char buffer[19] = {};
    std::snprintf(buffer, sizeof buffer, "0x%016llx",
                  static_cast<unsigned long long>(value));
    return buffer;
}

std::string hex_byte(std::uint8_t value) {
    char buffer[5] = {};
    std::snprintf(buffer, sizeof buffer, "0x%02x", value);
    return buffer;
}

std::optional<std::string> compare_contexts(const RegisterContext& left,
                                            const RegisterContext& right,
                                            const char* owner) {
    std::string difference;
    const auto fail32 = [&](const char* field, std::uint32_t left_value,
                            std::uint32_t right_value) {
        if (left_value != right_value && difference.empty()) {
            append_value(difference, owner, field, hex_text(left_value),
                         hex_text(right_value));
        }
    };
    const auto fail64 = [&](const char* field, std::uint64_t left_value,
                            std::uint64_t right_value) {
        if (left_value != right_value && difference.empty()) {
            append_value(difference, owner, field, hex_text64(left_value),
                         hex_text64(right_value));
        }
    };
    for (std::uint32_t index = 0; index < 32; ++index) {
        char field[16] = {};
        std::snprintf(field, sizeof field, "gpr[%u]", index);
        fail64(field, left.gpr[index], right.gpr[index]);
        std::snprintf(field, sizeof field, "gpr_high[%u]", index);
        fail64(field, left.gpr_high[index], right.gpr_high[index]);
        std::snprintf(field, sizeof field, "fpr[%u]", index);
        fail32(field, left.fpr[index], right.fpr[index]);
    }
    fail64("hi", left.hi, right.hi);
    fail64("lo", left.lo, right.lo);
    fail64("hi1", left.hi1, right.hi1);
    fail64("lo1", left.lo1, right.lo1);
    fail32("fpu_accumulator", left.fpu_accumulator, right.fpu_accumulator);
    fail32("fpu_control", left.fpu_control, right.fpu_control);
    fail32("shift_amount_cache", left.shift_amount_cache,
           right.shift_amount_cache);
    for (std::uint32_t index = 0; index < 32; ++index) {
        char field[16] = {};
        std::snprintf(field, sizeof field, "cp0[%u]", index);
        fail32(field, left.cp0[index], right.cp0[index]);
    }
    for (std::uint32_t index = 0; index < 32; ++index) {
        for (std::uint32_t lane = 0; lane < 4; ++lane) {
            char field[20] = {};
            std::snprintf(field, sizeof field, "vu0_vf[%u][%u]", index,
                          lane);
            fail32(field, left.vu0_vf[index][lane],
                   right.vu0_vf[index][lane]);
        }
    }
    for (std::uint32_t index = 0; index < 32; ++index) {
        char field[16] = {};
        std::snprintf(field, sizeof field, "vu0_vi[%u]", index);
        fail32(field, left.vu0_vi[index], right.vu0_vi[index]);
    }
    fail32("vu0_clip_flag", left.vu0_clip_flag, right.vu0_clip_flag);
    for (std::uint32_t lane = 0; lane < 4; ++lane) {
        char field[16] = {};
        std::snprintf(field, sizeof field, "vu0_acc[%u]", lane);
        fail32(field, left.vu0_acc[lane], right.vu0_acc[lane]);
    }
    fail32("vu0_mac_flag", left.vu0_mac_flag, right.vu0_mac_flag);
    fail32("vu0_status_flag", left.vu0_status_flag, right.vu0_status_flag);
    fail32("pc", left.pc, right.pc);
    if (difference.empty()) {
        return std::nullopt;
    }
    return difference;
}

std::optional<std::string> compare_memory_regions(
    const std::vector<MemoryRegion>& left,
    const std::vector<MemoryRegion>& right) {
    if (left.size() != right.size()) {
        return "memory region count: left " + std::to_string(left.size())
            + ", right " + std::to_string(right.size());
    }
    std::map<std::uint32_t, std::size_t> right_by_base;
    for (std::size_t index = 0; index < right.size(); ++index) {
        right_by_base[right[index].base] = index;
    }
    for (const MemoryRegion& left_region : left) {
        const auto found = right_by_base.find(left_region.base);
        if (found == right_by_base.end()) {
            return "memory region base " + hex_text(left_region.base)
                + " missing on the right";
        }
        const MemoryRegion& right_region = right[found->second];
        if (left_region.bytes.size() != right_region.bytes.size()) {
            return "memory region base " + hex_text(left_region.base)
                + " size: left "
                + std::to_string(left_region.bytes.size()) + ", right "
                + std::to_string(right_region.bytes.size());
        }
        for (std::size_t offset = 0; offset < left_region.bytes.size();
             ++offset) {
            if (left_region.bytes[offset] != right_region.bytes[offset]) {
                const std::uint32_t address =
                    left_region.base + static_cast<std::uint32_t>(offset);
                return "memory region base " + hex_text(left_region.base)
                    + " byte at " + hex_text(address) + ": left "
                    + hex_byte(left_region.bytes[offset]) + ", right "
                    + hex_byte(right_region.bytes[offset]);
            }
        }
    }
    for (const MemoryRegion& right_region : right) {
        bool known = false;
        for (const MemoryRegion& left_region : left) {
            if (left_region.base == right_region.base) {
                known = true;
                break;
            }
        }
        if (!known) {
            return "memory region base " + hex_text(right_region.base)
                + " missing on the left";
        }
    }
    return std::nullopt;
}

std::optional<std::string> compare_bank_sections(
    const std::vector<NamedBank>& left, const std::vector<NamedBank>& right) {
    std::map<std::string, std::size_t> right_by_name;
    for (std::size_t index = 0; index < right.size(); ++index) {
        right_by_name[right[index].name] = index;
    }
    if (left.size() != right.size()) {
        return "device bank count: left " + std::to_string(left.size())
            + ", right " + std::to_string(right.size());
    }
    for (const NamedBank& left_bank : left) {
        const auto found = right_by_name.find(left_bank.name);
        if (found == right_by_name.end()) {
            return "device bank \"" + left_bank.name + "\" missing on the right";
        }
        const BankRegisters& right_entries = right[found->second].entries;
        std::map<std::uint32_t, std::uint32_t> right_by_address;
        for (const auto& [address, value] : right_entries) {
            right_by_address[address] = value;
        }
        for (const auto& [address, value] : left_bank.entries) {
            const auto entry = right_by_address.find(address);
            if (entry == right_by_address.end()) {
                return "device bank \"" + left_bank.name + "\" register "
                    + hex_text(address) + " missing on the right";
            }
            if (entry->second != value) {
                return "device bank \"" + left_bank.name + "\" register "
                    + hex_text(address) + ": left " + hex_text(value)
                    + ", right " + hex_text(entry->second);
            }
        }
        for (const auto& [address, value] : right_entries) {
            bool known = false;
            for (const auto& [left_address, left_value] :
                 left_bank.entries) {
                if (left_address == address) {
                    known = true;
                    break;
                }
            }
            if (!known) {
                return "device bank \"" + left_bank.name + "\" register "
                    + hex_text(address) + " missing on the left";
            }
        }
    }
    for (const NamedBank& right_bank : right) {
        bool known = false;
        for (const NamedBank& left_bank : left) {
            if (left_bank.name == right_bank.name) {
                known = true;
                break;
            }
        }
        if (!known) {
            return "device bank \"" + right_bank.name
                + "\" missing on the left";
        }
    }
    return std::nullopt;
}

std::optional<std::string> compare_guest_states(const GuestState& left,
                                                const GuestState& right) {
    if (left.memory().segment_alias_enabled()
        != right.memory().segment_alias_enabled()) {
        return std::string("segment alias flag differs");
    }
    if (const std::optional<std::string> registers =
            compare_contexts(left.save_registers(), right.save_registers(),
                             "registers")) {
        return registers;
    }
    if (const std::optional<std::string> memory = compare_memory_regions(
            left.memory().regions_snapshot(),
            right.memory().regions_snapshot())) {
        return memory;
    }
    return std::nullopt;
}

std::optional<std::string> compare_full_states(
    const GuestState& left_state, const Kernel& left_kernel,
    const std::vector<NamedBank>& left_banks, const GuestState& right_state,
    const Kernel& right_kernel, const std::vector<NamedBank>& right_banks) {
    if (const std::optional<std::string> guest =
            compare_guest_states(left_state, right_state)) {
        return guest;
    }
    if (const std::optional<std::string> kernel =
            left_kernel.describe_kernel_difference(right_kernel)) {
        return kernel;
    }
    if (const std::optional<std::string> banks =
            compare_bank_sections(left_banks, right_banks)) {
        return banks;
    }
    return std::nullopt;
}

} // namespace gt4recomp::ee
