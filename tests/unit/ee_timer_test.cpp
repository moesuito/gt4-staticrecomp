// Unit tests for the EE timer register window, the generic device register
// bank and the explicit MMIO routing that carries them, with no game data.
#include "gt4recomp/ee_checkpoint.hpp"
#include "gt4recomp/ee_device.hpp"
#include "gt4recomp/ee_timer.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

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
        memory.write_word(TimerUnit::window_base + 0x10, 0x182u);
        check(memory.read_word(0x1000E010u) == 0x00000020u
                  && timer.register_value(TimerUnit::window_base + 0x10) == 0x182u,
              "the two windows stay independent");
        check(throws_runtime([&] { (void)memory.read_byte(0x1000E010u); }),
              "the register bank rejects non-32-bit widths");
    }

    // A DMA channel completes a started transfer at once and always
    // reports the channel's DMAC cause: TIE is stored but never gates the
    // completion (ps2autotests dmac/tagintr @97469ff).
    {
        GuestMemory memory(ram_base, ram_size);
        std::vector<std::uint32_t> causes;
        DmaChannel vif1(0x10009000u, 0x1000u, 1,
                        [&causes](std::uint32_t cause) { causes.push_back(cause); });
        vif1.map_into(memory);
        check(memory.is_mmio(0x10009000u, 4) && !memory.is_mmio(0x1000A000u, 4),
              "the channel window is mapped and bounded");
        memory.write_word(0x10009010u, 0x00100000u);  // TADR stores normally
        check(memory.read_word(0x10009010u) == 0x00100000u,
              "the transfer address register stores");
        memory.write_word(0x10009000u, 0x000001C5u);  // TIE | STR and mode bits
        check(memory.read_word(0x10009000u) == 0x000000C5u
                  && causes.size() == 1 && causes[0] == 1,
              "a started transfer completes and reports the DMAC channel");
        memory.write_word(0x10009000u, 0x00000145u);  // STR without TIE
        check(memory.read_word(0x10009000u) == 0x00000045u
                  && causes.size() == 2 && causes[1] == 1,
              "a transfer without TIE still reports its completion");
    }

    // COUNT and COMP are 16-bit logical counters: the upper half of a
    // 32-bit access never extends the counter (PCSX2 Counters.cpp @81526d4,
    // PS2tek EE Timers @1c91660).
    {
        TimerUnit timer;
        timer.write_register(TimerUnit::window_base + TimerUnit::count_offset,
                             4, 0xABCD1234u);
        timer.write_register(TimerUnit::window_base + TimerUnit::compare_offset,
                             4, 0xABCD5678u);
        check(timer.register_value(TimerUnit::window_base
                                       + TimerUnit::count_offset)
                      == 0x1234u
                  && timer.register_value(TimerUnit::window_base
                                              + TimerUnit::compare_offset)
                         == 0x5678u,
              "COUNT and COMP keep only the low 16 bits");
        timer.write_register(TimerUnit::window_base + TimerUnit::mode_offset,
                             4, 0xFFFFFFFFu);
        check(timer.register_value(TimerUnit::window_base
                                       + TimerUnit::mode_offset)
                      == 0x3FFu,
              "MODE keeps the 10 control bits and drops the rest");
    }

    // MODE flags acknowledge with write-1-to-clear: a zero write preserves
    // a pending flag, a one write clears only the named flag.
    {
        TimerUnit timer;
        constexpr std::uint32_t mode_address =
            TimerUnit::window_base + TimerUnit::mode_offset;
        // Seed pending flags through the restore path (the internal set the
        // tick advance performs, without involving the guest path).
        const BankRegisters pending = {
            {mode_address, 0x00000C80u}};  // CUE + EQUF + OVFF
        timer.restore_registers(pending);
        check(timer.register_value(mode_address) == 0x00000C80u,
              "a restore carries pending flags verbatim");
        timer.write_register(mode_address, 4, 0x00000080u);  // CUE, flags 0
        check(timer.register_value(mode_address) == 0x00000C80u,
              "a zero flag write preserves pending flags");
        timer.write_register(mode_address, 4, 0x00000480u);  // CUE + EQUF
        check(timer.register_value(mode_address) == 0x00000880u,
              "acknowledging EQUF leaves OVFF pending");
        timer.write_register(mode_address, 4, 0x00000880u);  // CUE + OVFF
        check(timer.register_value(mode_address) == 0x00000080u,
              "acknowledging OVFF clears the last flag");
    }

    // The internal tick advance: 16-bit wrap, edge-triggered flags, and
    // combined compare-plus-overflow in one advance. Each case runs on a
    // fresh unit: flags persist until the guest acknowledges them (W1C),
    // so cases must not share flag state.
    {
        constexpr std::uint32_t base = TimerUnit::window_base;
        // A plain compare crossing sets EQUF once; repeating the advance
        // with the flag still set reports no new edge.
        {
            TimerUnit timer;
            timer.write_register(base + TimerUnit::count_offset, 4, 0);
            timer.write_register(base + TimerUnit::compare_offset, 4, 0x100u);
            timer.write_register(base + TimerUnit::mode_offset, 4, 0x180u);
            TimerUnit::TimerAdvance crossed = timer.add_ticks(0, 0x100u);
            check(timer.register_value(base + TimerUnit::count_offset)
                          == 0x100u
                      && (timer.register_value(base + TimerUnit::mode_offset)
                              & 0x400u)
                             != 0
                      && crossed.compare_edge && !crossed.overflow_edge,
                  "crossing COMP sets EQUF with an edge");
            crossed = timer.add_ticks(0, 1);
            check(!crossed.compare_edge && !crossed.overflow_edge,
                  "no new edge while the flag stays unacknowledged");
        }
        // A compare behind the counter waits for the next wrap.
        {
            TimerUnit timer;
            timer.write_register(base + TimerUnit::count_offset, 4, 0x100u);
            timer.write_register(base + TimerUnit::compare_offset, 4, 0x50u);
            timer.write_register(base + TimerUnit::mode_offset, 4, 0x180u);
            const TimerUnit::TimerAdvance crossed = timer.add_ticks(0, 0x10u);
            check(timer.register_value(base + TimerUnit::count_offset)
                          == 0x110u
                      && (timer.register_value(base + TimerUnit::mode_offset)
                              & 0x400u)
                             == 0
                      && !crossed.compare_edge,
                  "a compare behind the counter does not fire");
        }
        // The 16-bit wrap: 0xFFF0 + 0x10 lands on 0x0000 with OVFF.
        {
            TimerUnit timer;
            timer.write_register(base + TimerUnit::count_offset, 4, 0xFFF0u);
            timer.write_register(base + TimerUnit::compare_offset, 4, 0);
            timer.write_register(base + TimerUnit::mode_offset, 4, 0x280u);
            const TimerUnit::TimerAdvance crossed = timer.add_ticks(0, 0x10u);
            check(timer.register_value(base + TimerUnit::count_offset) == 0
                      && (timer.register_value(base + TimerUnit::mode_offset)
                              & 0x800u)
                             != 0
                      && crossed.overflow_edge && !crossed.compare_edge,
                  "0xFFF0 + 0x10 wraps to zero with OVFF");
        }
        // Both events in one advance: wrap plus a compare at the landing.
        {
            TimerUnit timer;
            timer.write_register(base + TimerUnit::count_offset, 4, 0xFFF0u);
            timer.write_register(base + TimerUnit::compare_offset, 4, 0x0005u);
            timer.write_register(base + TimerUnit::mode_offset, 4, 0x380u);
            const TimerUnit::TimerAdvance crossed = timer.add_ticks(0, 0x20u);
            check(timer.register_value(base + TimerUnit::count_offset)
                          == 0x10u
                      && (timer.register_value(base + TimerUnit::mode_offset)
                              & 0xC00u)
                             == 0xC00u
                      && crossed.compare_edge && crossed.overflow_edge,
                  "one advance reports a combined compare and overflow");
        }
        // A stopped timer and a gated timer do not move.
        {
            TimerUnit timer;
            timer.write_register(base + TimerUnit::count_offset, 4, 0x100u);
            timer.write_register(base + TimerUnit::mode_offset, 4, 0x000u);
            TimerUnit::TimerAdvance crossed = timer.add_ticks(0, 0x100u);
            check(timer.register_value(base + TimerUnit::count_offset)
                          == 0x100u
                      && !crossed.compare_edge && !crossed.overflow_edge,
                  "a stopped timer does not advance");
            timer.write_register(base + TimerUnit::mode_offset, 4, 0x084u);
            crossed = timer.add_ticks(0, 0x100u);
            check(timer.register_value(base + TimerUnit::count_offset)
                          == 0x100u
                      && !crossed.compare_edge && !crossed.overflow_edge,
                  "a gated timer does not advance (P03 owns the gate modes)");
        }
        // ZeroReturn restarts the period on a compare passage.
        {
            TimerUnit timer;
            timer.write_register(base + TimerUnit::count_offset, 4, 0);
            timer.write_register(base + TimerUnit::compare_offset, 4, 0x10u);
            timer.write_register(base + TimerUnit::mode_offset, 4, 0x1C0u);
            const TimerUnit::TimerAdvance crossed = timer.add_ticks(0, 0x10u);
            check(timer.register_value(base + TimerUnit::count_offset) == 0
                      && crossed.compare_edge,
                  "ZeroReturn restarts the counter on a compare");
        }
    }

    if (failures != 0) {
        return 1;
    }
    std::cout << "timer registers and MMIO routing behave as specified\n";
    return 0;
}
