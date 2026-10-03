#include "gt4recomp/ee_checkpoint.hpp"

#include <stdexcept>

namespace gt4recomp::ee {
namespace {

constexpr char snapshot_magic[8] = {'G', 'T', '4', 'C',
                                    'K', 'P', 'T', '1'};

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFFu));
}

void put_u64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    put_u32(out, static_cast<std::uint32_t>(value & 0xFFFFFFFFull));
    put_u32(out, static_cast<std::uint32_t>((value >> 32) & 0xFFFFFFFFull));
}

struct Reader {
    std::span<const std::uint8_t> bytes;
    std::size_t offset = 0;

    std::uint8_t take_byte() {
        if (offset >= bytes.size()) {
            throw std::runtime_error("The snapshot ends mid-value");
        }
        return bytes[offset++];
    }

    std::uint32_t take_u32() {
        const std::uint32_t low = take_byte();
        const std::uint32_t mid_low = take_byte();
        const std::uint32_t mid_high = take_byte();
        const std::uint32_t high = take_byte();
        return low | (mid_low << 8) | (mid_high << 16) | (high << 24);
    }

    std::uint64_t take_u64() {
        const std::uint64_t low = take_u32();
        const std::uint64_t high = take_u32();
        return low | (high << 32);
    }
};

} // namespace

std::vector<std::uint8_t> save_snapshot(
    const RegisterContext& context, bool segment_alias,
    const std::vector<MemoryRegion>& regions) {
    std::vector<std::uint8_t> out;
    for (const char letter : snapshot_magic) {
        out.push_back(static_cast<std::uint8_t>(letter));
    }
    for (const std::uint64_t value : context.gpr) {
        put_u64(out, value);
    }
    for (const std::uint64_t value : context.gpr_high) {
        put_u64(out, value);
    }
    for (const std::uint32_t value : context.fpr) {
        put_u32(out, value);
    }
    put_u64(out, context.hi);
    put_u64(out, context.lo);
    put_u64(out, context.hi1);
    put_u64(out, context.lo1);
    put_u32(out, context.fpu_accumulator);
    put_u32(out, context.fpu_control);
    put_u32(out, context.shift_amount_cache);
    for (const std::uint32_t value : context.cp0) {
        put_u32(out, value);
    }
    for (const auto& lanes : context.vu0_vf) {
        for (const std::uint32_t lane : lanes) {
            put_u32(out, lane);
        }
    }
    for (const std::uint32_t value : context.vu0_vi) {
        put_u32(out, value);
    }
    put_u32(out, context.vu0_clip_flag);
    for (const std::uint32_t lane : context.vu0_acc) {
        put_u32(out, lane);
    }
    put_u32(out, context.vu0_mac_flag);
    put_u32(out, context.vu0_status_flag);
    put_u32(out, context.pc);
    put_u32(out, segment_alias ? 1u : 0u);
    if (regions.size() > 0xFFFFFFFFu) {
        throw std::runtime_error("The snapshot holds too many regions");
    }
    put_u32(out, static_cast<std::uint32_t>(regions.size()));
    for (const MemoryRegion& region : regions) {
        if (region.bytes.size() > 0xFFFFFFFFu) {
            throw std::runtime_error("The snapshot holds too large a region");
        }
        put_u32(out, region.base);
        put_u32(out, static_cast<std::uint32_t>(region.bytes.size()));
        out.insert(out.end(), region.bytes.begin(), region.bytes.end());
    }
    return out;
}

Snapshot load_snapshot(std::span<const std::uint8_t> bytes) {
    Reader reader{bytes, 0};
    for (const char letter : snapshot_magic) {
        if (reader.take_byte() != static_cast<std::uint8_t>(letter)) {
            throw std::runtime_error("The snapshot has a bad magic or version");
        }
    }
    Snapshot snapshot;
    for (std::uint64_t& value : snapshot.context.gpr) {
        value = reader.take_u64();
    }
    for (std::uint64_t& value : snapshot.context.gpr_high) {
        value = reader.take_u64();
    }
    for (std::uint32_t& value : snapshot.context.fpr) {
        value = reader.take_u32();
    }
    snapshot.context.hi = reader.take_u64();
    snapshot.context.lo = reader.take_u64();
    snapshot.context.hi1 = reader.take_u64();
    snapshot.context.lo1 = reader.take_u64();
    snapshot.context.fpu_accumulator = reader.take_u32();
    snapshot.context.fpu_control = reader.take_u32();
    snapshot.context.shift_amount_cache = reader.take_u32();
    for (std::uint32_t& value : snapshot.context.cp0) {
        value = reader.take_u32();
    }
    for (auto& lanes : snapshot.context.vu0_vf) {
        for (std::uint32_t& lane : lanes) {
            lane = reader.take_u32();
        }
    }
    for (std::uint32_t& value : snapshot.context.vu0_vi) {
        value = reader.take_u32();
    }
    snapshot.context.vu0_clip_flag = reader.take_u32();
    for (std::uint32_t& lane : snapshot.context.vu0_acc) {
        lane = reader.take_u32();
    }
    snapshot.context.vu0_mac_flag = reader.take_u32();
    snapshot.context.vu0_status_flag = reader.take_u32();
    snapshot.context.pc = reader.take_u32();
    const std::uint32_t alias = reader.take_u32();
    if (alias > 1u) {
        throw std::runtime_error("The snapshot has a bad alias flag");
    }
    snapshot.segment_alias = alias == 1u;
    const std::uint32_t region_count = reader.take_u32();
    for (std::uint32_t index = 0; index < region_count; ++index) {
        MemoryRegion region;
        region.base = reader.take_u32();
        const std::uint32_t size = reader.take_u32();
        if (size > reader.bytes.size() - reader.offset) {
            throw std::runtime_error("The snapshot region overruns the blob");
        }
        region.bytes.assign(reader.bytes.begin() + reader.offset,
                            reader.bytes.begin() + reader.offset + size);
        reader.offset += size;
        snapshot.regions.push_back(std::move(region));
    }
    return snapshot;
}

void restore_memory(GuestMemory& memory, const Snapshot& snapshot) {
    if (snapshot.segment_alias != memory.segment_alias_enabled()) {
        if (snapshot.segment_alias) {
            memory.enable_segment_alias();
        } else {
            throw std::runtime_error(
                "The snapshot alias flag disagrees with the memory");
        }
    }
    for (const MemoryRegion& region : snapshot.regions) {
        memory.write_bytes(region.base, region.bytes);
    }
}

std::vector<std::uint8_t> save_bank_section(
    const std::vector<BankRegisters>& banks) {
    std::vector<std::uint8_t> out;
    for (const char letter : {'G', 'T', '4', 'B', 'A', 'N', 'K', '1'}) {
        out.push_back(static_cast<std::uint8_t>(letter));
    }
    if (banks.size() > 0xFFFFFFFFu) {
        throw std::runtime_error("The bank section holds too many banks");
    }
    put_u32(out, static_cast<std::uint32_t>(banks.size()));
    for (const BankRegisters& bank : banks) {
        if (bank.size() > 0xFFFFFFFFu) {
            throw std::runtime_error("The bank section holds too many entries");
        }
        put_u32(out, static_cast<std::uint32_t>(bank.size()));
        for (const auto& [address, value] : bank) {
            put_u32(out, address);
            put_u32(out, value);
        }
    }
    return out;
}

std::vector<BankRegisters> load_bank_section(
    std::span<const std::uint8_t> bytes) {
    Reader reader{bytes, 0};
    constexpr char magic[8] = {'G', 'T', '4', 'B', 'A', 'N', 'K', '1'};
    for (const char letter : magic) {
        if (reader.take_byte() != static_cast<std::uint8_t>(letter)) {
            throw std::runtime_error("The bank section has a bad magic");
        }
    }
    std::vector<BankRegisters> banks;
    const std::uint32_t bank_count = reader.take_u32();
    for (std::uint32_t bank = 0; bank < bank_count; ++bank) {
        BankRegisters entries;
        const std::uint32_t entry_count = reader.take_u32();
        for (std::uint32_t entry = 0; entry < entry_count; ++entry) {
            const std::uint32_t address = reader.take_u32();
            entries.emplace_back(address, reader.take_u32());
        }
        banks.push_back(std::move(entries));
    }
    if (reader.offset != reader.bytes.size()) {
        throw std::runtime_error("The bank section has trailing bytes");
    }
    return banks;
}

} // namespace gt4recomp::ee
