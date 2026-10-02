#include "gt4recomp/ee_flow.hpp"

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

    // Classification of single instructions against their guest PC. Words are
    // the same literal encodings used by the decode and disassemble fixtures.
    struct ClassifyCase {
        std::uint32_t word, pc;
        FlowKind kind;
        bool delay, known;
        std::uint32_t target;
    };
    const ClassifyCase classify_cases[] = {
        {0x11090003, 0x00001000, FlowKind::Branch, true, true, 0x00001010},
        {0x50400004, 0x00001000, FlowKind::Branch, true, true, 0x00001014},
        {0x0603fffc, 0x00001000, FlowKind::Branch, true, true, 0x00000ff4},
        {0x08040000, 0x0ffffffcu, FlowKind::Jump, true, true, 0x10100000},
        {0x0c040000, 0x00001000, FlowKind::Call, true, true, 0x00100000},
        {0x0040f809, 0x00001000, FlowKind::Call, true, false, 0},
        {0x03e00008, 0x00001000, FlowKind::Return, true, false, 0},
        {0x01000008, 0x00001000, FlowKind::IndirectJump, true, false, 0},
        {0x0000000c, 0x00001000, FlowKind::Exception, false, false, 0},
        {0x27bdfff0, 0x00001000, FlowKind::FallThrough, false, false, 0},
        {0x70000002, 0x00001000, FlowKind::Unsupported, false, false, 0},
    };
    for (const auto& expected : classify_cases) {
        const auto actual = classify(decode(expected.word), expected.pc);
        check(actual.kind == expected.kind, "classify kind");
        check(actual.has_delay_slot == expected.delay, "classify delay slot");
        check(actual.target_known == expected.known, "classify target flag");
        check(!expected.known || actual.target == expected.target, "classify target");
    }

    const auto base = 0x00010000u;

    // A limit stop leaves the block falling through to the resume address.
    const auto limit_text = make_text(base, {0x27bdfff0, 0x27bdfff0, 0x27bdfff0, 0x27bdfff0});
    const auto limit_block = build_basic_block(limit_text, base, 2);
    check(limit_block.instruction_count == 2 && limit_block.end_exclusive == base + 8,
          "limit block extent");
    check(limit_block.ending == FlowKind::FallThrough
          && limit_block.stop_reason == "instruction-limit"
          && limit_block.continuation == base + 8, "limit block stop");

    // Reaching the end of file-backed text also stops as a fall-through.
    const auto range_text = make_text(base, {0x27bdfff0, 0x27bdfff0});
    const auto range_block = build_basic_block(range_text, base, 10);
    check(range_block.instruction_count == 2 && range_block.stop_reason == "range"
          && range_block.continuation == base + 8, "range block stop");

    // A branch ends the block; its delay slot belongs to the block.
    const auto branch_text = make_text(base, {0x27bdfff0, 0x11090003, 0x0080982d, 0x8fa80010});
    const auto branch_block = build_basic_block(branch_text, base, 40);
    check(branch_block.instruction_count == 3 && branch_block.end_exclusive == base + 12,
          "branch block extent");
    check(branch_block.ending == FlowKind::Branch && branch_block.stop_reason == "branch",
          "branch block ending");
    check(branch_block.target_known && branch_block.target == base + 0x14
          && branch_block.continuation == base + 12, "branch block successors");

    // A call records the direct target and the pc+8 return point.
    const auto call_text = make_text(base, {0x0c040000, 0x0080982d});
    const auto call_block = build_basic_block(call_text, base, 40);
    check(call_block.instruction_count == 2 && call_block.ending == FlowKind::Call
          && call_block.target_known && call_block.target == 0x00100000
          && call_block.continuation == base + 8 && call_block.stop_reason == "call",
          "call block");

    // jr ra is a return: no static target and no static continuation.
    const auto return_text = make_text(base, {0x03e00008, 0x0080982d});
    const auto return_block = build_basic_block(return_text, base, 40);
    check(return_block.instruction_count == 2 && return_block.ending == FlowKind::Return
          && !return_block.target_known && return_block.continuation == 0
          && return_block.stop_reason == "return", "return block");

    const auto indirect_text = make_text(base, {0x01000008, 0x0080982d});
    const auto indirect_block = build_basic_block(indirect_text, base, 40);
    check(indirect_block.ending == FlowKind::IndirectJump
          && indirect_block.stop_reason == "indirect-jump", "indirect jump block");

    // SYSCALL has no delay slot and ends the block immediately.
    const auto syscall_text = make_text(base, {0x0000000c, 0x27bdfff0});
    const auto syscall_block = build_basic_block(syscall_text, base, 40);
    check(syscall_block.instruction_count == 1 && syscall_block.end_exclusive == base + 4
          && syscall_block.ending == FlowKind::Exception
          && syscall_block.stop_reason == "exception", "syscall block");

    // An unsupported word is included and ends the walk with context.
    const auto unsupported_text = make_text(base, {0x27bdfff0, 0x70000002, 0x27bdfff0});
    const auto unsupported_block = build_basic_block(unsupported_text, base, 40);
    check(unsupported_block.instruction_count == 2
          && unsupported_block.ending == FlowKind::Unsupported
          && unsupported_block.stop_reason == "unsupported", "unsupported block");

    // An unsupported delay slot keeps the transfer's facts and is flagged.
    const auto bad_slot_text = make_text(base, {0x11090003, 0x70000002});
    const auto bad_slot_block = build_basic_block(bad_slot_text, base, 40);
    check(bad_slot_block.instruction_count == 2 && bad_slot_block.ending == FlowKind::Branch
          && bad_slot_block.target_known && bad_slot_block.target == base + 0x10
          && bad_slot_block.continuation == base + 8 && bad_slot_block.delay_slot_unsupported
          && bad_slot_block.stop_reason == "branch", "unsupported delay slot");

    // A branch inside a delay slot is undefined behavior; claim nothing.
    const auto illegal_text = make_text(base, {0x11090003, 0x1509fffe});
    const auto illegal_block = build_basic_block(illegal_text, base, 40);
    check(illegal_block.instruction_count == 2
          && illegal_block.ending == FlowKind::Unsupported
          && illegal_block.stop_reason == "branch-in-delay-slot"
          && !illegal_block.target_known && illegal_block.continuation == 0,
          "branch in delay slot");

    // A transfer as the first word still consumes its delay slot.
    const auto first_text = make_text(base, {0x03e00008, 0x0080982d});
    const auto first_block = build_basic_block(first_text, base, 40);
    check(first_block.instruction_count == 2 && first_block.ending == FlowKind::Return,
          "first-word transfer");

    // A transfer at the very end of text keeps its facts and reports the range.
    const auto edge_text = make_text(base, {0x11090003});
    const auto edge_block = build_basic_block(edge_text, base, 40);
    check(edge_block.instruction_count == 1 && edge_block.ending == FlowKind::Branch
          && edge_block.target_known && edge_block.target == base + 0x10
          && edge_block.continuation == base + 8 && edge_block.stop_reason == "range",
          "delay slot out of range");

    // Malformed requests are rejected before any output is produced.
    const auto rejects = [](const ImageRecord& record, std::uint32_t start, std::uint32_t limit) {
        try {
            (void)build_basic_block(record, start, limit);
        } catch (const std::runtime_error&) {
            return true;
        }
        return false;
    };
    const auto two_words = make_text(base, {0x27bdfff0, 0x27bdfff0});
    check(rejects(two_words, base, 0), "zero limit rejected");
    check(rejects(two_words, base + 1, 10), "unaligned start rejected");
    check(rejects(two_words, base - 4, 10), "start before text rejected");
    check(rejects(two_words, base + 8, 10), "start at text end rejected");

    // Stable names for reports and tests.
    check(std::string(flow_name(FlowKind::FallThrough)) == "fall-through"
          && std::string(flow_name(FlowKind::IndirectJump)) == "indirect-jump"
          && std::string(flow_name(FlowKind::Unsupported)) == "unsupported",
          "flow names");

    if (failures != 0) {
        return 1;
    }
    std::cout << "flow classification and basic block fixtures passed\n";
    return 0;
}
