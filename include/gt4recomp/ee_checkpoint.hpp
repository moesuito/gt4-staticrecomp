#pragma once

// Snapshots of guest CPU and RAM state ("save and resume from here").
//
// A snapshot captures a RegisterContext plus every RAM region's bytes so a
// run can later resume bit-identically instead of replaying from the
// entry. Device registers are NOT included: MMIO windows route to live
// device objects, whose contents are snapshotted with the devices (a
// later slice), never here. Kernel state (threads, semaphores, clocks)
// joins in a later slice the same way.
//
// File layout, all integers little endian:
//   8 bytes magic "GT4CKPT1" (the last byte is the version)
//   RegisterContext as fixed fields in declaration order:
//     gpr[32] u64, gpr_high[32] u64, fpr[32] u32,
//     hi/lo/hi1/lo1 u64, fpu_accumulator/control/shift u32,
//     cp0[32] u32, vu0_vf[32][4] u32, vu0_vi[32] u32,
//     clip/mac/status u32, acc[4] u32, pc u32
//   u32 segment_alias (0 or 1)
//   u32 region count, then per region {u32 base, u32 size, size bytes}
//
// Anything malformed (magic, version, truncation, a size that overruns
// the buffer) throws std::runtime_error instead of reading past the end.

#include "gt4recomp/ee_state.hpp"

#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace gt4recomp::ee {

// A parsed snapshot: the CPU context, the segment-alias flag and the RAM
// regions with their bases.
struct Snapshot {
    RegisterContext context{};
    bool segment_alias = false;
    std::vector<MemoryRegion> regions;
};

// Serializes one context plus the given RAM regions.
[[nodiscard]] std::vector<std::uint8_t> save_snapshot(
    const RegisterContext& context, bool segment_alias,
    const std::vector<MemoryRegion>& regions);

// Parses and validates a snapshot blob; anything malformed throws.
[[nodiscard]] Snapshot load_snapshot(std::span<const std::uint8_t> bytes);

// Writes a snapshot's regions into a memory built with the same geometry
// (same bases and sizes). Bulk writes refuse MMIO windows, so a memory
// with mapped devices rejects its overlapping spans loudly instead of
// shadowing them; a geometry or alias mismatch throws as well. The caller
// rebuilds the geometry identically and restores device registers
// separately.
void restore_memory(GuestMemory& memory, const Snapshot& snapshot);

// Device banks in the caller's fixed order (the gt4boot wiring uses
// map_into order): each bank contributes its stored registers.
using BankRegisters = std::vector<std::pair<std::uint32_t, std::uint32_t>>;

// One section holding every bank: magic "GT4BANK1", u32 bank count, then
// per bank {u32 entry count, per entry {u32 address, u32 value}}. Anything
// malformed (magic, truncation, trailing bytes) throws std::runtime_error.
[[nodiscard]] std::vector<std::uint8_t> save_bank_section(
    const std::vector<BankRegisters>& banks);
[[nodiscard]] std::vector<BankRegisters> load_bank_section(
    std::span<const std::uint8_t> bytes);

} // namespace gt4recomp::ee
