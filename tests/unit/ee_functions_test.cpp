#include "gt4recomp/ee_functions.hpp"

#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace gt4recomp;
using namespace gt4recomp::ee;

namespace {

ImageRecord make_text(std::uint32_t address, std::initializer_list<std::uint32_t> words) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(words.size() * 4);
    for (const auto word : words) {
        bytes.push_back(static_cast<std::uint8_t>(word & 0xff));
        bytes.push_back(static_cast<std::uint8_t>((word >> 8) & 0xff));
        bytes.push_back(static_cast<std::uint8_t>((word >> 16) & 0xff));
        bytes.push_back(static_cast<std::uint8_t>((word >> 24) & 0xff));
    }
    return ImageRecord{address, std::move(bytes)};
}

} // namespace

int main() {
    int failures = 0;
    const auto check = [&](bool passed, const char* label) {
        if (!passed) { std::cerr << label << '\n'; ++failures; }
    };

    // Two functions: the first returns and calls the second directly.
    //   0x10000 addiu / 0x10004 jal 0x10014 / 0x10008 delay
    //   0x1000c jr ra / 0x10010 delay / 0x10014 addiu / 0x10018 jr ra / 0x1001c delay
    const auto base = 0x00010000u;
    const auto text = make_text(base, {0x27bdfff0, 0x0c004005, 0x0080982d, 0x03e00008,
                                       0x0080982d, 0x27bdfff0, 0x03e00008, 0x0080982d});
    const FunctionSeed entry_seed[] = {{base, FunctionEvidence::ElfEntry}};
    const auto map = build_function_map(text, entry_seed, 10, 100);
    check(map.functions.size() == 2, "function count");
    check(!map.limited && map.pending.empty(), "map bounds");
    check(map.functions[0].entry == base
          && map.functions[0].evidence == FunctionEvidence::ElfEntry, "entry record");
    check(map.functions[0].block_count == 2 && map.functions[0].instruction_count == 5
          && map.functions[0].open_ends == 1 && !map.functions[0].limited,
          "entry traversal counts");
    check(map.functions[0].call_targets.size() == 1
          && map.functions[0].call_targets[0] == base + 0x14, "entry call target");
    check(map.functions[1].entry == base + 0x14
          && map.functions[1].evidence == FunctionEvidence::DirectCall,
          "discovered function evidence");
    check(map.functions[1].block_count == 1 && map.functions[1].instruction_count == 3,
          "discovered traversal counts");

    // The function cap stops discovery and records the queued candidates.
    const auto capped = build_function_map(text, entry_seed, 1, 100);
    check(capped.functions.size() == 1 && capped.limited && capped.pending.size() == 1
          && capped.pending[0] == base + 0x14, "function cap records pending work");

    // The per-function cap trims the reachable set and flags the record.
    const auto shallow = build_function_map(text, entry_seed, 10, 1);
    check(shallow.functions.size() == 2 && shallow.functions[0].limited
          && shallow.functions[0].block_count == 1, "per-function cap");

    // Seeds keep their own evidence; duplicates collapse.
    const FunctionSeed seed_only[] = {{base, FunctionEvidence::Seed}};
    const auto seeded = build_function_map(text, seed_only, 10, 100);
    check(seeded.functions[0].evidence == FunctionEvidence::Seed, "seed evidence");
    const FunctionSeed duplicates[] = {{base, FunctionEvidence::Seed},
                                       {base, FunctionEvidence::Seed}};
    const auto deduped = build_function_map(text, duplicates, 10, 100);
    check(deduped.functions.size() == 2, "duplicate seeds collapse");

    // Zero caps are rejected before any traversal.
    bool zero_functions_rejected = false;
    bool zero_blocks_rejected = false;
    try {
        (void)build_function_map(text, entry_seed, 0, 10);
    } catch (const std::runtime_error&) {
        zero_functions_rejected = true;
    }
    try {
        (void)build_function_map(text, entry_seed, 10, 0);
    } catch (const std::runtime_error&) {
        zero_blocks_rejected = true;
    }
    check(zero_functions_rejected, "zero function cap rejected");
    check(zero_blocks_rejected, "zero block cap rejected");

    // An invalid seed is rejected before any traversal, even when a cap would
    // otherwise stop before reaching it.
    const FunctionSeed mixed_seeds[] = {{base, FunctionEvidence::Seed},
                                        {base + 0x30, FunctionEvidence::Seed}};
    bool late_invalid_seed_rejected = false;
    try {
        (void)build_function_map(text, mixed_seeds, 1, 100);
    } catch (const std::runtime_error&) {
        late_invalid_seed_rejected = true;
    }
    check(late_invalid_seed_rejected, "invalid seed rejected despite the cap");

    // Stable evidence names for reports and tests.
    check(std::string(function_evidence_name(FunctionEvidence::ElfEntry)) == "elf-entry"
          && std::string(function_evidence_name(FunctionEvidence::Seed)) == "seed"
          && std::string(function_evidence_name(FunctionEvidence::DirectCall)) == "direct-call",
          "evidence names");

    if (failures != 0) {
        return 1;
    }
    std::cout << "function map fixtures passed\n";
    return 0;
}
