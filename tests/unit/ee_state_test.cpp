#include "gt4recomp/ee_state.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace gt4recomp::ee;

namespace {

constexpr std::uint32_t base = 0x00100000;
constexpr std::size_t region_size = 0x100;

GuestMemory make_memory() {
    return GuestMemory(base, region_size);
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

    // Construction limits.
    check(throws([] { GuestMemory memory(0x00100000, 0); }), "zero size rejected");
    check(throws([] { GuestMemory memory(0xfffffff0u, 0x20); }),
          "region beyond the address space rejected");
    const GuestMemory top(0xfffffff0u, 0x10);
    check(top.size() == 0x10, "topmost region accepted");

    auto memory = make_memory();

    // Little-endian byte order for every width.
    memory.write_halfword(base, 0x1234);
    check(memory.read_byte(base) == 0x34 && memory.read_byte(base + 1) == 0x12,
          "halfword is little endian");
    memory.write_word(base + 4, 0x11223344);
    check(memory.read_byte(base + 4) == 0x44 && memory.read_byte(base + 5) == 0x33
          && memory.read_byte(base + 6) == 0x22 && memory.read_byte(base + 7) == 0x11,
          "word is little endian");
    memory.write_doubleword(base + 8, 0x1122334455667788ull);
    check(memory.read_byte(base + 8) == 0x88 && memory.read_byte(base + 15) == 0x11,
          "doubleword is little endian");
    check(memory.read_halfword(base) == 0x1234
          && memory.read_word(base + 4) == 0x11223344
          && memory.read_doubleword(base + 8) == 0x1122334455667788ull,
          "roundtrips");

    // Alignment rules.
    check(throws([&] { (void)memory.read_halfword(base + 1); }),
          "misaligned halfword read rejected");
    check(throws([&] { (void)memory.read_word(base + 2); }),
          "misaligned word read rejected");
    check(throws([&] { memory.write_doubleword(base + 4, 0); }),
          "misaligned doubleword write rejected");

    // Bounds rules, including the topmost word of the address space.
    check(throws([&] { (void)memory.read_word(base + region_size - 2); }),
          "word crossing the region end rejected");
    check(throws([&] { (void)memory.read_byte(base + region_size); }),
          "byte past the region end rejected");
    check(throws([&] { (void)memory.read_byte(base - 1); }),
          "byte before the region rejected");
    check(memory.read_byte(base + region_size - 1) == 0, "last byte reachable and zeroed");
    GuestMemory top_memory(0xfffffff0u, 0x10);
    top_memory.write_word(0xfffffffc, 0xaabbccddu);
    check(top_memory.read_word(0xfffffffc) == 0xaabbccddu, "last word reachable");
    const std::vector<std::uint8_t> four{1, 2, 3, 4};
    check(throws([&] { top_memory.write_bytes(0xfffffffd, four); }),
          "bulk write crossing the top rejected");

    // Bulk copies and contains().
    const std::vector<std::uint8_t> payload{0xde, 0xad, 0xbe, 0xef, 0x11};
    memory.write_bytes(base + 0x20, payload);
    check(memory.read_byte(base + 0x20) == 0xde && memory.read_byte(base + 0x24) == 0x11,
          "bulk write readable");
    memory.write_bytes(base + 0x20, {});
    check(memory.read_byte(base + 0x20) == 0xde, "empty bulk write is a no-op");
    check(memory.contains(base, 1) && memory.contains(base + region_size - 4, 4)
          && !memory.contains(base + region_size - 3, 4) && !memory.contains(base - 1, 1)
          && !memory.contains(base, 0), "contains bounds");

    // Segment aliasing is opt-in and translates KSEG0/KSEG1 to physical.
    {
        GuestMemory aliased(0, 0x1000);
        check(!aliased.segment_alias_enabled() && !aliased.contains(0x80000000u, 4),
              "KSEG0 is unmapped by default");
        aliased.enable_segment_alias();
        check(aliased.segment_alias_enabled(), "segment aliasing can be enabled");
        aliased.write_word(0x80000010u, 0x12345678u);
        check(aliased.read_word(0x10) == 0x12345678u
                  && aliased.read_word(0x80000010u) == 0x12345678u
                  && aliased.read_word(0xA0000010u) == 0x12345678u,
              "KSEG0, KSEG1 and physical access the same bytes");
        check(aliased.contains(0x80000ffcu, 4) && !aliased.contains(0x80001000u, 4),
              "the alias is bounded by the region");
    }

    // A second RAM region (the EE scratchpad) is independent of the first.
    {
        GuestMemory with_scratchpad = make_memory();
        with_scratchpad.map_region(0x70000000u, 0x4000u);
        with_scratchpad.write_doubleword(0x70002000u, 0x1122334455667788ull);
        check(with_scratchpad.read_doubleword(0x70002000u) == 0x1122334455667788ull
                  && with_scratchpad.contains(0x70002000u, 8),
              "a second RAM region is addressable");
        check(!with_scratchpad.contains(0x70004000u, 1)
                  && throws([&] { (void)with_scratchpad.read_byte(0x70004000u); }),
              "the second region is bounded");
        check(with_scratchpad.contains(base, 4)
                  && with_scratchpad.read_word(base) == 0,
              "the main region still works beside the second");
        check(throws([&] { with_scratchpad.map_region(0x70000000u, 0x10u); }),
              "overlapping regions rejected");
        check(throws([&] { with_scratchpad.map_region(0x00100080u, 0x10u); }),
              "a region overlapping the main one is rejected");
    }

    // Register file semantics.
    GuestState state(make_memory());
    check(state.read_gpr64(0) == 0 && state.read_gpr64(31) == 0 && state.pc() == 0,
          "initial state zeroed");
    state.write_gpr64(5, 0x1122334455667788ull);
    check(state.read_gpr64(5) == 0x1122334455667788ull
          && state.read_gpr32(5) == 0x55667788u, "64-bit register roundtrip");
    state.write_gpr32(5, 0x80000000u);
    check(state.read_gpr64(5) == 0xffffffff80000000ull, "32-bit write sign-extends");
    state.write_gpr32(6, 0x00000123u);
    check(state.read_gpr64(6) == 0x123ull, "positive 32-bit write stays positive");
    state.write_gpr64(0, 0xffffffffffffffffull);
    check(state.read_gpr64(0) == 0 && state.read_gpr32(0) == 0,
          "zero register ignores writes");
    check(throws([&] { state.write_gpr64(32, 0); })
          && throws([&] { (void)state.read_gpr32(32); }), "register index 32 rejected");
    state.set_pc(0x0010011cu);
    check(state.pc() == 0x0010011cu, "program counter roundtrip");
    state.memory().write_word(base + 0x30, 0xcafebabeu);
    check(state.memory().read_word(base + 0x30) == 0xcafebabeu, "state memory access");

    // DMA start polls (decision 0034): only a completing channel CHCR with
    // STR set fires, and only when a delivery path is wired.
    {
        GuestState polled(make_memory());
        polled.set_pc(0x00100000u);
        check(!polled.poll_dma_start(0x10009000u, 0x100001c5u, 0x00100004u)
                  && polled.pc() == 0x00100000u,
              "no hook means no poll and no pc change");
        std::uint32_t delivered_pc = 0;
        int deliveries = 0;
        polled.set_dma_start_poll(
            [&](GuestState& running) {
                delivered_pc = running.pc();
                ++deliveries;
                return true;
            });
        check(polled.poll_dma_start(0x10009000u, 0x100001c5u, 0x00100004u)
                  && deliveries == 1 && delivered_pc == 0x00100004u
                  && polled.pc() == 0x00100004u,
              "VIF1 CHCR with STR sets the next pc and polls");
        check(!polled.poll_dma_start(0x10009000u, 0x000000c5u, 0x00100008u)
                  && deliveries == 1 && polled.pc() == 0x00100004u,
              "CHCR without STR never polls");
        check(!polled.poll_dma_start(0x10009020u, 0x100001c5u, 0x00100008u)
                  && deliveries == 1 && polled.pc() == 0x00100004u,
              "QWC offset never polls");
        check(!polled.poll_dma_start(0x00100000u, 0x100001c5u, 0x00100008u)
                  && deliveries == 1 && polled.pc() == 0x00100004u,
              "plain RAM never polls");
        check(polled.poll_dma_start(0x10008000u, 0x00000100u, 0x0010000cu)
                  && deliveries == 2 && delivered_pc == 0x0010000cu
                  && polled.pc() == 0x0010000cu,
              "VIF0 CHCR with STR polls");
        check(polled.poll_dma_start(0x1000a000u, 0x00000100u, 0x00100010u)
                  && deliveries == 3 && delivered_pc == 0x00100010u
                  && polled.pc() == 0x00100010u,
              "GIF CHCR with STR polls");
        polled.set_dma_start_poll(
            [&](GuestState&) { return false; });
        check(!polled.poll_dma_start(0x10009000u, 0x100001c5u, 0x00100014u)
                  && polled.pc() == 0x00100014u,
              "a refusing hook still reports false");
    }
    // The KSEG0 mirror of a channel CHCR polls once the alias is on.
    {
        GuestMemory aliased_ram(0, 0x2000000);
        GuestState aliased(std::move(aliased_ram));
        aliased.memory().enable_segment_alias();
        bool delivered = false;
        aliased.set_dma_start_poll(
            [&](GuestState&) {
                delivered = true;
                return true;
            });
        check(aliased.poll_dma_start(0x90009000u, 0x100001c5u, 0x00100004u)
                  && delivered,
              "KSEG0 CHCR mirror polls with the alias on");
    }

    if (failures != 0) {
        return 1;
    }
    std::cout << "guest state and memory fixtures passed\n";
    return 0;
}
