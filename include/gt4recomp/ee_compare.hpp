#pragma once

// Canonical state comparison for the driver/interpreter differential
// (decision 0036, PLAN.md P08): snapshots of the live registers, every
// mapped RAM region (main memory, the scratchpad and any other mapped
// region), the kernel tables and the device banks, compared field by
// field with the first divergent component named.
//
// The comparator only observes, never changes behavior:
// - RAM comes from GuestMemory::regions_snapshot (MMIO windows are
//   skipped there; their contents are compared as device banks).
// - Device state comes from registers_snapshot/register_value storage
//   reads, never from a guest MMIO read through memory (a guest read
//   could carry side effects on other hardware; the snapshot path has
//   none by construction).
// - Kernel state is compared member by member (the same members the
//   checkpoint blob serializes). Host pointers the boot relinks
//   (disc sources, device units, the service table), the diagnostic
//   RPC telemetry (pair/bind stats, never serialized) and the
//   strict-mode config flag are not guest state and never compared.
// - Containers compare by identity where order is incidental (memory
//   regions by base, banks by name, bank entries by address, threads
//   and semaphores by id, maps by key). Handler chains, the pending
//   queue and the deferred-call stack keep their order: dispatch and
//   chaining read them in order, so there order is semantic.

#include "gt4recomp/ee_checkpoint.hpp"
#include "gt4recomp/ee_kernel.hpp"
#include "gt4recomp/ee_state.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace gt4recomp::ee {

// One device bank with its diagnostic label. The name only labels the
// diagnosis; the address/value entries carry the semantics.
struct NamedBank {
    std::string name;
    BankRegisters entries;
};

// Lowercase hex with a 0x prefix: 8 digits for 32-bit values, 16 for
// 64-bit ones, 2 for single bytes.
[[nodiscard]] std::string hex_text(std::uint32_t value);
[[nodiscard]] std::string hex_text64(std::uint64_t value);
[[nodiscard]] std::string hex_byte(std::uint8_t value);

// Field-by-field RegisterContext comparison. The owner names the holder
// ("registers", "thread 2 saved context", ...). Empty when identical,
// otherwise the first divergent field with both values.
[[nodiscard]] std::optional<std::string> compare_contexts(
    const RegisterContext& left, const RegisterContext& right,
    const char* owner);

// RAM regions matched by base address, then size, then bytes. Names the
// first divergent region and, for content, the first divergent address.
[[nodiscard]] std::optional<std::string> compare_memory_regions(
    const std::vector<MemoryRegion>& left,
    const std::vector<MemoryRegion>& right);

// Device banks matched by name, entries matched by address, so an
// incidental snapshot order can never report a divergence. Names the
// first divergent bank and register with both values.
[[nodiscard]] std::optional<std::string> compare_bank_sections(
    const std::vector<NamedBank>& left, const std::vector<NamedBank>& right);

// The live machine: the segment-alias flag, the live register context
// and every mapped RAM region (main memory, scratchpad, the GS block,
// any other mapped region). The running thread's live registers are
// compared here; every other thread's saved context lives in the kernel
// and is compared with it.
[[nodiscard]] std::optional<std::string> compare_guest_states(
    const GuestState& left, const GuestState& right);

// The whole semantic machine in one fixed order: live registers, RAM
// regions, kernel tables, device banks. Empty when identical, otherwise
// the first divergent component.
[[nodiscard]] std::optional<std::string> compare_full_states(
    const GuestState& left_state, const Kernel& left_kernel,
    const std::vector<NamedBank>& left_banks, const GuestState& right_state,
    const Kernel& right_kernel, const std::vector<NamedBank>& right_banks);

} // namespace gt4recomp::ee
