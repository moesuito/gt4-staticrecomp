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

    if (failures != 0) {
        return 1;
    }
    std::cout << "guest state and memory fixtures passed\n";
    return 0;
}
