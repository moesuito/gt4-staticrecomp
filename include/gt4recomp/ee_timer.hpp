#pragma once

// The EE's four timers, as the SDK's timer and alarm initialization sees
// them. Each timer owns four 16-bit logical registers at a fixed base:
// COUNT at +0x00, MODE at +0x10, COMP at +0x20, HOLD at +0x30; the bases are
// 0x10000000, 0x10000800, 0x10001000 and 0x10001800. The 32-bit MMIO access
// width and the 16-bit logical width are different things: reads return the
// low 16 bits with the upper half zero, writes keep the low 16 bits.
//
// References (pinned by PLAN.md and GPT_FEEDBACK.md):
//   PCSX2 Counters.cpp/H at 81526d4dc7cc70e4ae75abb35a789417456c6d43
//   (count/target masked to 0xFFFF, overflow at 0x10000, W1C flag clear,
//   future-target deferral, ZeroReturn gated on the target interrupt);
//   PS2tek EE Timers at source revision 1c9166066c3ab9ad089a4d12088acaa9476df4b6
//   (four 16-bit timers, MODE bit layout, edge-triggered interrupts);
//   PS2SDK timer.c at ac92a9f657d2e531dd8f060250b07f2a5ac6dea5 (InitTimer
//   programs CUE|CMPE|OVFE with COMP 0xFFFF and calls EnableIntc; EndTimer
//   stops by writing the two flags as 1 to clear them; the handler works
//   only while EQUF is set; extended time is (overflows << 16) | COUNT).
//
// Three operations stay separate on purpose:
//   guest_write  - what the guest MMIO store does (masking plus W1C);
//   add_ticks    - what the passing of counter ticks does (wrap plus flags);
//   restore      - what a snapshot load does (verbatim state, no effects).
// A restore never queues an interrupt and never runs the guest acknowledge
// path (decision 0028). Internal flag sets never pass through the guest
// W1C path either: writing a 1 to a flag from inside the device would clear
// it, so add_ticks sets flags by OR-ing them directly.

#include "gt4recomp/ee_device.hpp"
#include "gt4recomp/ee_state.hpp"

#include <cstdint>
#include <map>
#include <span>
#include <utility>
#include <vector>

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

    // MODE bits (PS2tek EE Timers; PCSX2 EECNT_MODE @81526d4).
    static constexpr std::uint32_t mode_clock_mask = 0x00000003;     // CLKS
    static constexpr std::uint32_t mode_gate_enable = 0x00000004;    // GATE
    static constexpr std::uint32_t mode_gate_source = 0x00000008;
    static constexpr std::uint32_t mode_gate_mode_mask = 0x00000030;
    static constexpr std::uint32_t mode_zero_return = 0x00000040;    // ZRET
    static constexpr std::uint32_t mode_count_enable = 0x00000080;  // CUE
    static constexpr std::uint32_t mode_compare_enable = 0x00000100;  // CMPE
    static constexpr std::uint32_t mode_overflow_enable = 0x00000200;  // OVFE
    static constexpr std::uint32_t mode_compare_flag = 0x00000400;   // EQUF
    static constexpr std::uint32_t mode_overflow_flag = 0x00000800;  // OVFF
    // Low 10 bits are control (writable); bits 10-11 are flags (W1C).
    static constexpr std::uint32_t mode_control_mask = 0x000003FF;
    static constexpr std::uint32_t mode_flag_mask = 0x00000C00;
    static constexpr std::uint32_t mode_read_mask = 0x00000FFF;

    static constexpr std::uint32_t count_mask = 0x0000FFFF;
    static constexpr std::uint32_t compare_mask = 0x0000FFFF;
    static constexpr std::uint32_t hold_mask = 0x0000FFFF;
    static constexpr std::uint32_t counter_modulo = 0x00010000;

    // What one internal tick advance did. An edge is reported only on the
    // 0 -> 1 transition of its flag (edge-triggered delivery, PS2tek), so a
    // second passage while the guest has not acknowledged the first reports
    // no new edge. Compare and overflow are independent: one advance can
    // report both (no else-if between them).
    struct TimerAdvance {
        bool compare_edge = false;
        bool overflow_edge = false;
    };

    // Routes the whole timer window of the memory into this unit. The unit
    // must outlive the memory (the callbacks capture it).
    void map_into(GuestMemory& memory);

    // One guest register access. Only 32-bit accesses are modeled; anything
    // else throws with the address, because the hardware definition of a
    // byte access to these registers is not part of the evidence. COUNT and
    // COMP move only their low 16 bits; MODE applies writable control plus
    // W1C flag acknowledge; HOLD is storage (real only on T0/T1 per PS2tek
    // and PCSX2 Hw.h, accepted as storage on T2/T3 and documented as such).
    [[nodiscard]] std::uint32_t read_register(std::uint32_t address,
                                              std::size_t width) const;
    void write_register(std::uint32_t address, std::size_t width,
                        std::uint32_t value);

    // The guest-visible value of one register; zero when never written.
    // COUNT/COMP/HOLD read back masked; MODE reads back masked to 12 bits.
    [[nodiscard]] std::uint32_t register_value(std::uint32_t address) const;

    // The device-internal tick advance: moves the counter by the given
    // number of counter ticks, wrapping at 16 bits, setting EQUF/OVFF by
    // direct OR (never through the guest W1C path) and applying ZeroReturn
    // (which resets only when the compare interrupt is enabled, per the
    // PCSX2 note tested on hardware). A compare behind the counter waits
    // for the next wrap: the advance lands exactly on each crossing, so no
    // persistent future-target bit is needed. Gated timers do not advance:
    // without an HBLANK-signal model the hold is the explicit limit
    // (decision 0030); ZeroReturn is fully treated and pinned by tests.
    TimerAdvance add_ticks(std::uint32_t index, std::uint32_t ticks);

    // A full ordered photo of the 16 typed registers for snapshots. The
    // restore writes storage directly and bypasses the guest write path: no
    // flag acknowledge and no interrupt is ever queued by a restore.
    [[nodiscard]] std::vector<std::pair<std::uint32_t, std::uint32_t>>
    registers_snapshot() const;
    void restore_registers(
        std::span<const std::pair<std::uint32_t, std::uint32_t>> entries);

private:
    struct TimerState {
        std::uint32_t count = 0;
        std::uint32_t mode = 0;
        std::uint32_t compare = 0;
        std::uint32_t hold = 0;
    };

    // Splits an address into a timer index and a register kind
    // (0 COUNT, 1 MODE, 2 COMP, 3 HOLD). False for offsets the timers do
    // not define; those stay plain storage so the boot keeps working.
    [[nodiscard]] static bool decode(std::uint32_t address,
                                     std::uint32_t& index,
                                     std::uint32_t& kind) noexcept;

    TimerState timers_[timer_count] = {};
    // Plain storage for window offsets that are not timer registers.
    std::map<std::uint32_t, std::uint32_t> spare_;
};

} // namespace gt4recomp::ee
