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

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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

// The checkpoint file: the services count at the save point, then three
// length-prefixed sections (context+memory, kernel, banks). Each section
// keeps its own magic, so a section decodes standalone; the file only
// frames them.
struct CheckpointFile {
    std::uint64_t services_handled = 0;
    std::vector<std::uint8_t> context_memory;
    std::vector<std::uint8_t> kernel;
    std::vector<std::uint8_t> banks;
};

[[nodiscard]] std::vector<std::uint8_t> save_checkpoint_file(
    const CheckpointFile& file);
[[nodiscard]] CheckpointFile load_checkpoint_file(
    std::span<const std::uint8_t> bytes);

// Autosave photo names (decision 0027): every photo is `ckpt-<S>.bin`
// where S is the cumulative services handled since boot. The three
// helpers below are pure filename arithmetic with no filesystem or
// model dependency, so the rotation policy is unit-testable.
struct AutosavePhoto {
    std::uint64_t services = 0;
    std::uint64_t bytes = 0;
};

enum class AutosaveEvictReason {
    BeyondKeep,
    OverByteCap,
};

struct AutosaveEviction {
    std::uint64_t services = 0;
    AutosaveEvictReason reason = AutosaveEvictReason::BeyondKeep;
};

// Formats the photo name for a service count.
[[nodiscard]] inline std::string format_autosave_name(
    std::uint64_t services) {
    return "ckpt-" + std::to_string(services) + ".bin";
}

// Parses a photo name back into its service count. Anything that is
// not exactly `ckpt-<digits>.bin` (foreign files, manual checkpoints
// under other names) is refused so rotation never touches it.
[[nodiscard]] inline bool parse_autosave_name(
    const std::string& filename, std::uint64_t& services) {
    constexpr std::string_view prefix = "ckpt-";
    constexpr std::string_view suffix = ".bin";
    if (filename.size() <= prefix.size() + suffix.size()
        || filename.compare(0, prefix.size(), prefix) != 0
        || filename.compare(filename.size() - suffix.size(), suffix.size(),
                            suffix)
            != 0) {
        return false;
    }
    const std::string digits = filename.substr(
        prefix.size(), filename.size() - prefix.size() - suffix.size());
    if (digits.empty()) {
        return false;
    }
    std::uint64_t value = 0;
    for (const char digit : digits) {
        if (digit < '0' || digit > '9') {
            return false;
        }
        const std::uint64_t next =
            value * 10 + static_cast<std::uint64_t>(digit - '0');
        if (next < value) {
            return false;
        }
        value = next;
    }
    services = value;
    return true;
}

// Selects which managed photos to delete, oldest first: everything
// beyond the newest `keep` by service number, then the oldest while
// the retained bytes exceed the cap. The just-written photo is never
// selected by the cap step, so a run never deletes the photo it just
// took (a lone over-cap photo stays and reports). Rotation needs no
// such guard: it deletes strictly by service number, even the fresh
// photo when higher-numbered files already fill the keep window.
[[nodiscard]] inline std::vector<AutosaveEviction> select_autosave_evictions(
    const std::vector<AutosavePhoto>& photos, std::uint64_t keep,
    bool has_byte_cap, std::uint64_t max_bytes,
    std::uint64_t just_written) {
    std::vector<AutosavePhoto> ordered = photos;
    std::sort(ordered.begin(), ordered.end(),
              [](const AutosavePhoto& left, const AutosavePhoto& right) {
                  return left.services < right.services;
              });
    std::vector<AutosaveEviction> doomed;
    std::vector<bool> evict(ordered.size(), false);
    if (ordered.size() > keep) {
        for (std::size_t index = 0; index < ordered.size() - keep; ++index) {
            evict[index] = true;
            doomed.push_back({ordered[index].services,
                              AutosaveEvictReason::BeyondKeep});
        }
    }
    if (has_byte_cap) {
        std::uint64_t retained = 0;
        for (std::size_t index = 0; index < ordered.size(); ++index) {
            if (!evict[index]) {
                retained += ordered[index].bytes;
            }
        }
        for (std::size_t index = 0;
             index < ordered.size() && retained > max_bytes; ++index) {
            if (!evict[index]
                && ordered[index].services != just_written) {
                evict[index] = true;
                retained -= ordered[index].bytes;
                doomed.push_back({ordered[index].services,
                                  AutosaveEvictReason::OverByteCap});
            }
        }
    }
    return doomed;
}

} // namespace gt4recomp::ee
