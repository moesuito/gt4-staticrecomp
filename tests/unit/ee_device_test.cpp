#include "gt4recomp/ee_checkpoint.hpp"
#include "gt4recomp/ee_device.hpp"
#include "gt4recomp/ee_timer.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace gt4recomp::ee;

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

    // A plain bank round-trips its registers and drops anything written
    // after the snapshot.
    {
        RegisterBank bank(0x1000E000u, 0x100u);
        bank.write_register(0x1000E010u, 4, 0x12345678);
        bank.write_register(0x1000E014u, 4, 0x9ABCDEF0);
        const BankRegisters saved = bank.registers_snapshot();
        check(saved.size() == 2 && saved[0].first == 0x1000E010u
                  && saved[0].second == 0x12345678u,
              "the snapshot lists registers ordered");
        bank.write_register(0x1000E010u, 4, 0);
        bank.write_register(0x1000E020u, 4, 1);
        bank.restore_registers(saved);
        check(bank.register_value(0x1000E010u) == 0x12345678u
                  && bank.register_value(0x1000E014u) == 0x9ABCDEF0u
                  && bank.register_value(0x1000E020u) == 0,
              "restoring replaces the registers wholesale");
    }

    // A DMA channel restores through storage only: no completion fires.
    {
        int fires = 0;
        DmaChannel channel(0x1000C000u, 0x100u, 5,
                           [&](std::uint32_t) { ++fires; });
        const BankRegisters with_interrupt = {
            {0x1000C000u, DmaChannel::interrupt_enable}};
        channel.restore_registers(with_interrupt);
        const BankRegisters saved = channel.registers_snapshot();
        int restored_fires = 0;
        DmaChannel restored(0x1000C000u, 0x100u, 5,
                            [&](std::uint32_t) { ++restored_fires; });
        restored.restore_registers(saved);
        check(restored.register_value(0x1000C000u)
                      == DmaChannel::interrupt_enable
                  && fires == 0 && restored_fires == 0,
              "a channel restore is side-effect free");
    }

    // The timer unit forwards its bank.
    {
        TimerUnit timer;
        timer.write_register(TimerUnit::window_base + TimerUnit::count_offset,
                             4, 0x11111111);
        timer.write_register(TimerUnit::window_base + TimerUnit::mode_offset,
                             4, 0x782);
        timer.write_register(TimerUnit::window_base + TimerUnit::compare_offset,
                             4, 0x7D573500);
        const BankRegisters saved = timer.registers_snapshot();
        TimerUnit restored;
        restored.restore_registers(saved);
        check(restored.register_value(TimerUnit::window_base
                                          + TimerUnit::count_offset)
                      == 0x11111111u
                  && restored.register_value(TimerUnit::window_base
                                                 + TimerUnit::mode_offset)
                         == 0x782u
                  && restored.register_value(TimerUnit::window_base
                                                 + TimerUnit::compare_offset)
                         == 0x7D573500u,
              "the timer registers restore");
    }

    // The bank section codec round-trips an ordered bank list and rejects
    // malformed blobs.
    {
        RegisterBank first(0x1000E000u, 0x100u);
        first.write_register(0x1000E010u, 4, 1);
        RegisterBank second(0x1000F000u, 0x100u);
        const std::vector<std::uint8_t> blob = save_bank_section(
            {first.registers_snapshot(), second.registers_snapshot()});
        const std::vector<BankRegisters> banks = load_bank_section(blob);
        check(banks.size() == 2 && banks[0].size() == 1
                  && banks[0][0].first == 0x1000E010u
                  && banks[0][0].second == 1u && banks[1].empty(),
              "the bank section parses back in order");
        RegisterBank target(0x1000E000u, 0x100u);
        target.restore_registers(banks[0]);
        check(target.register_value(0x1000E010u) == 1,
              "a parsed bank restores into a live bank");
        std::vector<std::uint8_t> bad_magic = blob;
        bad_magic[0] = 'X';
        check(throws([&] { (void)load_bank_section(bad_magic); }),
              "a bad bank magic throws");
        check(throws([&] { (void)load_bank_section({blob.data(), 9}); }),
              "a truncated bank section throws");
        std::vector<std::uint8_t> trailing = blob;
        trailing.push_back(0);
        check(throws([&] { (void)load_bank_section(trailing); }),
              "trailing bank bytes throw");
    }

    return failures == 0 ? 0 : 1;
}
