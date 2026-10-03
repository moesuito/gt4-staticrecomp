#include "gt4recomp/ee_checkpoint.hpp"
#include "gt4recomp/ee_state.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace gt4recomp::ee;

namespace {

bool contexts_equal(const RegisterContext& left, const RegisterContext& right) {
    return left.gpr == right.gpr && left.gpr_high == right.gpr_high
        && left.fpr == right.fpr && left.hi == right.hi && left.lo == right.lo
        && left.hi1 == right.hi1 && left.lo1 == right.lo1
        && left.fpu_accumulator == right.fpu_accumulator
        && left.fpu_control == right.fpu_control
        && left.shift_amount_cache == right.shift_amount_cache
        && left.cp0 == right.cp0 && left.vu0_vf == right.vu0_vf
        && left.vu0_vi == right.vu0_vi
        && left.vu0_clip_flag == right.vu0_clip_flag
        && left.vu0_acc == right.vu0_acc
        && left.vu0_mac_flag == right.vu0_mac_flag
        && left.vu0_status_flag == right.vu0_status_flag && left.pc == right.pc;
}

RegisterContext make_context() {
    RegisterContext context;
    for (std::size_t index = 0; index < 32; ++index) {
        context.gpr[index] = 0x1000 + index;
        context.gpr_high[index] = 0x2000 + index;
        context.fpr[index] = 0x3000 + static_cast<std::uint32_t>(index);
        context.cp0[index] = 0x4000 + static_cast<std::uint32_t>(index);
        context.vu0_vi[index] = 0x5000 + static_cast<std::uint32_t>(index);
        for (std::size_t lane = 0; lane < 4; ++lane) {
            context.vu0_vf[index][lane] =
                0x6000 + static_cast<std::uint32_t>(index * 4 + lane);
        }
    }
    context.hi = 0x7001;
    context.lo = 0x7002;
    context.hi1 = 0x7003;
    context.lo1 = 0x7004;
    context.fpu_accumulator = 0x7005;
    context.fpu_control = 0x7006;
    context.shift_amount_cache = 0x7007;
    context.vu0_clip_flag = 0x7008;
    for (std::size_t lane = 0; lane < 4; ++lane) {
        context.vu0_acc[lane] = 0x7010 + static_cast<std::uint32_t>(lane);
    }
    context.vu0_mac_flag = 0x7020;
    context.vu0_status_flag = 0x7030;
    context.pc = 0x00100008;
    return context;
}

} // namespace

int main() {
    int failures = 0;
    const auto check = [&](bool passed, const char* label) {
        if (!passed) { std::cerr << label << '\n'; ++failures; }
    };
    const auto throws = []<typename Action>(Action&& action) {
        try {
            action();
        } catch (const std::runtime_error&) {
            return true;
        }
        return false;
    };

    // A two-region memory with a pattern in each region.
    GuestMemory memory(0x00100000, 0x100);
    memory.map_region(0x70000000, 0x40);
    memory.enable_segment_alias();
    for (std::uint32_t offset = 0; offset < 0x100; ++offset) {
        memory.write_byte(0x00100000 + offset,
                           static_cast<std::uint8_t>(offset & 0xFFu));
    }
    for (std::uint32_t offset = 0; offset < 0x40; ++offset) {
        memory.write_byte(0x70000000 + offset,
                           static_cast<std::uint8_t>(0xA0 + offset));
    }

    const RegisterContext context = make_context();
    const std::vector<std::uint8_t> blob =
        save_snapshot(context, memory.segment_alias_enabled(),
                      memory.regions_snapshot());
    // 8 magic + 1484 context + 4 alias + 4 count + (4 + 4 + 0x100)
    // + (4 + 4 + 0x40) bytes.
    check(blob.size() == 8 + 1484 + 4 + 4 + (8 + 0x100) + (8 + 0x40),
          "the snapshot size is exact");

    // Mutate everything, then restore and compare.
    GuestMemory restored(0x00100000, 0x100);
    restored.map_region(0x70000000, 0x40);
    const Snapshot snapshot = load_snapshot(blob);
    check(contexts_equal(snapshot.context, context), "the context parses back");
    check(snapshot.segment_alias, "the alias flag parses back");
    check(snapshot.regions.size() == 2 && snapshot.regions[0].base == 0x00100000
              && snapshot.regions[1].bytes.size() == 0x40,
          "the regions parse back");
    restore_memory(restored, snapshot);
    check(restored.segment_alias_enabled(), "the alias restores");
    bool bytes_match = true;
    for (std::uint32_t offset = 0; offset < 0x100; ++offset) {
        bytes_match = bytes_match
            && restored.read_byte(0x00100000 + offset)
                == static_cast<std::uint8_t>(offset & 0xFFu);
    }
    for (std::uint32_t offset = 0; offset < 0x40; ++offset) {
        bytes_match = bytes_match
            && restored.read_byte(0x70000000 + offset)
                == static_cast<std::uint8_t>(0xA0 + offset);
    }
    check(bytes_match, "the region bytes restore");

    // GuestState round-trips through its own accessors too.
    GuestState state(std::move(restored));
    state.restore_registers(snapshot.context);
    check(contexts_equal(state.save_registers(), context),
          "the state round-trips registers");

    // Malformed blobs stop loudly.
    std::vector<std::uint8_t> bad_magic = blob;
    bad_magic[0] = 'X';
    check(throws([&] { (void)load_snapshot(bad_magic); }),
          "a bad magic throws");
    check(throws([&] { (void)load_snapshot({blob.data(), 10}); }),
          "a truncation throws");
    std::vector<std::uint8_t> bad_alias = blob;
    bad_alias[8 + 1484] = 2;
    check(throws([&] { (void)load_snapshot(bad_alias); }),
          "a bad alias flag throws");

    // A different geometry refuses the restore.
    GuestMemory other_geometry(0x00200000, 0x100);
    check(throws([&] { restore_memory(other_geometry, snapshot); }),
          "a geometry mismatch throws");

    return failures == 0 ? 0 : 1;
}
