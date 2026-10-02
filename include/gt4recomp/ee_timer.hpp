#pragma once

// The EE's four timers, as the SDK's timer and alarm initialization sees
// them. Each timer owns four 32-bit registers at a fixed base:
// COUNT at +0x00, MODE at +0x10, COMP at +0x20, HOLD at +0x30; the bases are
// 0x10000000, 0x10000800, 0x10001000 and 0x10001800. The model stores every
// 32-bit write and returns it on read, so the initialization code sees its
// own configuration and T3_MODE reads as "not started", exactly the value
// the SDK's InitAlarm checks before patching.
//
// Model limits, recorded in docs/decisions/0007-timer-registers.md: the
// counters do not tick, the MODE overflow/compare bits are plain storage and
// no timer interrupt is ever raised. Code that needs a firing compare or
// overflow stops at that boundary instead of observing a guessed time.

#include "gt4recomp/ee_device.hpp"
#include "gt4recomp/ee_state.hpp"

#include <cstdint>

namespace gt4recomp::ee {

class TimerUnit {
public:
    static constexpr std::uint32_t window_base = 0x10000000;
    static constexpr std::uint32_t window_size = 0x2000;
    static constexpr std::uint32_t timer_count = 4;
    static constexpr std::uint32_t timer_stride = 0x800;
    static constexpr std::uint32_t count_offset = 0x00;
    static constexpr std::uint32_t mode_offset = 0x10;
    static constexpr std::uint32_t compare_offset = 0x20;
    static constexpr std::uint32_t hold_offset = 0x30;

    // Routes the whole timer window of the memory into this unit. The unit
    // must outlive the memory (the callbacks capture it).
    void map_into(GuestMemory& memory);

    // One register access. Only 32-bit accesses are modeled; anything else
    // throws with the address, because the hardware definition of a byte
    // access to these registers is not part of the evidence.
    [[nodiscard]] std::uint32_t read_register(std::uint32_t address,
                                              std::size_t width) const;
    void write_register(std::uint32_t address, std::size_t width,
                        std::uint32_t value);

    // The stored value of one register; zero when never written.
    [[nodiscard]] std::uint32_t register_value(std::uint32_t address) const;

private:
    RegisterBank registers_{window_base, window_size};
};

} // namespace gt4recomp::ee
