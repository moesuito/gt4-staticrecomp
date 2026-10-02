// Unit tests for the EE timer register window, the generic device register
// bank and the explicit MMIO routing that carries them, with no game data.
#include "gt4recomp/ee_device.hpp"
#include "gt4recomp/ee_timer.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>

using namespace gt4recomp::ee;

namespace {

constexpr std::uint32_t ram_base = 0x00000000;
constexpr std::uint32_t ram_size = 0x1000;

bool throws_runtime(const auto& action) {
    try {
        action();
    } catch (const std::runtime_error&) {
        return true;
    }
    return false;
}

} // namespace

int main() {
    int failures = 0;
    const auto check = [&](bool passed, const char* label) {
        if (!passed) { std::cerr << label << '\n'; ++failures; }
    };

    // The register window routes reads and writes to the unit.
    {
        GuestMemory memory(ram_base, ram_size);
        TimerUnit timer;
        check(!memory.contains(TimerUnit::window_base, 4),
              "the timer window is unmapped before map_into");
        timer.map_into(memory);
        check(memory.contains(TimerUnit::window_base, 4)
                  && memory.contains(TimerUnit::window_base + TimerUnit::window_size - 4, 4)
                  && !memory.contains(TimerUnit::window_base + TimerUnit::window_size, 4),
              "the mapped window is bounded");
        constexpr std::uint32_t t3_mode = TimerUnit::window_base
            + 3 * TimerUnit::timer_stride + TimerUnit::mode_offset;
        check(memory.read_word(t3_mode) == 0,
              "an untouched register reads as not started");
        memory.write_word(t3_mode, 0x00000100u);
        check(memory.read_word(t3_mode) == 0x00000100u
                  && timer.register_value(t3_mode) == 0x00000100u,
              "a written register reads back and is visible to the unit");
        memory.enable_segment_alias();
        check(memory.read_word(0x90001810u) == 0x00000100u
                  && memory.read_word(0xB0001810u) == 0x00000100u,
              "the KSEG0 and KSEG1 aliases reach the device window");
        memory.write_word(0x00000100u, 0x11223344u);
        check(memory.read_word(0x00000100u) == 0x11223344u
                  && timer.register_value(0x00000100u) == 0,
              "RAM and the device window stay separate");
        check(throws_runtime([&] { (void)memory.read_byte(t3_mode); })
                  && throws_runtime([&] { memory.write_halfword(t3_mode, 0x1234u); }),
              "unsupported register widths are rejected");
        check(throws_runtime([&] { (void)memory.read_doubleword(t3_mode); })
                  && throws_runtime([&] { memory.write_doubleword(t3_mode, 0); }),
              "wide register accesses are rejected");
        check(throws_runtime([&] {
                  const std::uint8_t payload[4] = {1, 2, 3, 4};
                  memory.write_bytes(t3_mode, payload);
              }),
              "bulk writes into the device window are rejected");
    }

    // Two device windows coexist: the timer unit and a plain register bank
    // (the DMAC block the SIF initialization reads).
    {
        GuestMemory memory(ram_base, ram_size);
        TimerUnit timer;
        RegisterBank dmac(0x1000E000u, 0x100u);
        timer.map_into(memory);
        dmac.map_into(memory);
        check(memory.is_mmio(0x1000E010u, 4) && memory.is_mmio(TimerUnit::window_base, 4)
                  && !memory.is_mmio(0x1000D000u, 4),
              "each device window is bounded and separate");
        check(memory.read_word(0x1000E010u) == 0,
              "an untouched device register reads as zero");
        memory.write_word(0x1000E010u, 0x00000020u);
        check(memory.read_word(0x1000E010u) == 0x00000020u
                  && dmac.register_value(0x1000E010u) == 0x00000020u,
              "the register bank stores and returns the value");
        memory.write_word(TimerUnit::window_base + 0x10, 0x1234u);
        check(memory.read_word(0x1000E010u) == 0x00000020u
                  && timer.register_value(TimerUnit::window_base + 0x10) == 0x1234u,
              "the two windows stay independent");
        check(throws_runtime([&] { (void)memory.read_byte(0x1000E010u); }),
              "the register bank rejects non-32-bit widths");
    }

    if (failures != 0) {
        return 1;
    }
    std::cout << "timer registers and MMIO routing behave as specified\n";
    return 0;
}
