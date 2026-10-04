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

    // A live normal-mode start moves MADR/QWC from RAM into the device sink
    // and only then completes: STR clears, QWC drains, MADR walks past the
    // moved bytes, and the channel's DMAC cause fires once. Restoring the
    // same value fires nothing and keeps the bits verbatim.
    {
        int live_fires = 0;
        std::uint32_t live_cause = 0xFFFFFFFFu;
        DmaChannel live(0x1000C000u, 0x100u, 5,
                        [&](std::uint32_t cause) {
                            ++live_fires;
                            live_cause = cause;
                        });
        GuestMemory memory(0, 0x2000);
        live.map_into(memory);
        std::uint8_t source[32];
        for (std::uint32_t index = 0; index < 32; ++index) {
            source[index] = static_cast<std::uint8_t>(index * 3 + 1);
        }
        memory.write_bytes(0x1000u, source);
        memory.write_word(0x1000C010u, 0x1000u);  // MADR
        memory.write_word(0x1000C020u, 2u);       // QWC: two quadwords
        memory.write_word(0x1000C000u, DmaChannel::direction_bit
                                           | DmaChannel::start_bit
                                           | DmaChannel::interrupt_enable);
        check(live_fires == 1 && live_cause == 5,
              "a live start fires once with the channel cause");
        check(live.payload_bytes().size() == 32
                   && live.payload_byte_count() == 32,
              "a live start moves QWC quadwords into the sink");
        bool payload_matches = live.payload_bytes().size() == 32;
        for (std::uint32_t index = 0; payload_matches && index < 32; ++index) {
            payload_matches = live.payload_bytes()[index] == source[index];
        }
        check(payload_matches, "the moved payload matches the source bytes");
        check(live.register_value(0x1000C000u)
                      == (DmaChannel::direction_bit
                          | DmaChannel::interrupt_enable)
                   && live.register_value(0x1000C010u) == 0x1020u
                   && live.register_value(0x1000C020u) == 0,
              "STR clears, MADR walks past the payload and QWC drains");
        check(live.starts().size() == 1 && live.starts()[0].completed
                   && live.starts()[0].bytes_moved == 32
                   && live.starts()[0].tags_walked == 0,
              "the start log records the completed normal transfer");
        const BankRegisters completed = live.registers_snapshot();
        int replay_fires = 0;
        DmaChannel replay(0x1000C000u, 0x100u, 5,
                          [&](std::uint32_t) { ++replay_fires; });
        replay.restore_registers(completed);
        check(replay_fires == 0
                   && replay.register_value(0x1000C000u)
                    == (DmaChannel::direction_bit
                        | DmaChannel::interrupt_enable)
                   && replay.starts().empty()
                   && replay.payload_bytes().empty()
                   && replay.payload_byte_count() == 0,
              "restoring a completed transfer fires nothing and restarts diagnostics");
        const BankRegisters armed = {
            {0x1000C000u, DmaChannel::start_bit
                              | DmaChannel::interrupt_enable}};
        int armed_fires = 0;
        DmaChannel armed_channel(0x1000C000u, 0x100u, 5,
                                 [&](std::uint32_t) { ++armed_fires; });
        armed_channel.restore_registers(armed);
        check(armed_fires == 0
                  && armed_channel.register_value(0x1000C000u)
                    == (DmaChannel::start_bit
                        | DmaChannel::interrupt_enable),
              "a restore keeps an armed start bit verbatim without firing");
    }

    // The timer unit forwards its bank, masked to the logical widths: a
    // guest MODE write carries control plus W1C acknowledge, so 0x782
    // (with the EQUF bit set) stores control 0x382 and clears no flag.
    {
        TimerUnit timer;
        timer.write_register(TimerUnit::window_base + TimerUnit::count_offset,
                             4, 0x11111111);
        timer.write_register(TimerUnit::window_base + TimerUnit::mode_offset,
                             4, 0x382);
        timer.write_register(TimerUnit::window_base + TimerUnit::compare_offset,
                             4, 0x7D573500);
        const BankRegisters saved = timer.registers_snapshot();
        TimerUnit restored;
        restored.restore_registers(saved);
        check(restored.register_value(TimerUnit::window_base
                                          + TimerUnit::count_offset)
                      == 0x1111u
                  && restored.register_value(TimerUnit::window_base
                                                 + TimerUnit::mode_offset)
                         == 0x382u
                  && restored.register_value(TimerUnit::window_base
                                                 + TimerUnit::compare_offset)
                         == 0x3500u,
              "the timer registers restore masked to 16 bits");
    }

    // The INTC unit: W1C status, toggling mask, internal occurrence and
    // enable paths, and a side-effect-free restore.
    {
        IntcUnit intc;
        GuestMemory memory(0x1000F000u, 0x100u);
        intc.map_into(memory);
        check(memory.read_word(0x1000F000u) == 0
                  && memory.read_word(0x1000F010u) == 0,
              "untouched INTC status and mask read as zero");
        intc.set_pending_internal(2);
        intc.set_pending_internal(5);
        intc.enable_internal(2);
        check(memory.read_word(0x1000F000u) == 0x24u
                  && intc.is_pending(2) && intc.is_pending(5)
                  && !intc.is_pending(3) && intc.mask_allows(2)
                  && !intc.mask_allows(5),
              "internal occurrence and enable set status and mask bits");
        memory.write_word(0x1000F000u, 1u << 2);  // acknowledge cause 2
        check(memory.read_word(0x1000F000u) == 0x20u
                  && !intc.is_pending(2) && intc.is_pending(5),
              "acknowledging one cause leaves the other pending");
        memory.write_word(0x1000F000u, 0);  // a zero write preserves
        check(memory.read_word(0x1000F000u) == 0x20u,
              "a zero status write preserves pending causes");
        memory.write_word(0x1000F010u, 1u << 5);  // toggle mask bit 5 on
        check(intc.mask_allows(5), "a mask write toggles the bit on");
        memory.write_word(0x1000F010u, 1u << 5);  // toggle it back off
        check(!intc.mask_allows(5), "a mask write toggles the bit off");
        memory.write_word(0x1000F010u, 1u << 2);  // toggle bit 2 off
        check(!intc.mask_allows(2) && intc.is_pending(5),
              "toggling the mask never touches pending status");
        const BankRegisters saved = intc.registers_snapshot();
        IntcUnit restored;
        restored.restore_registers(saved);
        check(restored.register_value(0x1000F000u) == 0x20u
                  && restored.register_value(0x1000F010u) == 0,
              "the INTC state restores verbatim");
        // Restoring an armed status/mask pair runs no guest path: the bits
        // land as stored, with no acknowledge and no toggle.
        const BankRegisters armed = {{0x1000F000u, 0xFFFFFFFFu},
                                     {0x1000F010u, 0x0000FFFFu}};
        IntcUnit armed_unit;
        armed_unit.restore_registers(armed);
        check(armed_unit.register_value(0x1000F000u) == 0xFFFFFFFFu
                  && armed_unit.register_value(0x1000F010u) == 0xFFFFu,
              "an INTC restore keeps armed bits without guest effects");
    }

    // The DMAC status unit: W1C completion status, toggling mask, internal
    // completion and enable paths, and a side-effect-free restore.
    {
        DmacStatusUnit dmac;
        GuestMemory memory(0x1000E000u, 0x100u);
        dmac.map_into(memory);
        check(memory.read_word(0x1000E010u) == 0,
              "untouched DMAC status reads as zero");
        dmac.set_completion_internal(2);
        dmac.set_completion_internal(5);
        dmac.enable_internal(5);
        check(memory.read_word(0x1000E010u) == 0x00200024u
                  && dmac.completion_pending(2) && dmac.completion_pending(5)
                  && dmac.mask_allows(5) && !dmac.mask_allows(2),
              "internal completions and enables set CIS and CIM bits");
        memory.write_word(0x1000E010u, 1u << 2);  // acknowledge channel 2
        check(memory.read_word(0x1000E010u) == 0x00200020u
                  && !dmac.completion_pending(2)
                  && dmac.completion_pending(5),
              "acknowledging one channel leaves the other pending");
        memory.write_word(0x1000E010u, 0);  // a zero write preserves
        check(memory.read_word(0x1000E010u) == 0x00200020u,
              "a zero status write preserves pending completions");
        memory.write_word(0x1000E010u, 2u << 16);  // toggle CIM bit 1 on
        check(dmac.mask_allows(1), "a mask write toggles the CIM bit on");
        memory.write_word(0x1000E010u, 2u << 16);  // toggle it back off
        check(!dmac.mask_allows(1)
                  && dmac.completion_pending(5),
              "toggling the mask never touches completion status");
        const BankRegisters saved = dmac.registers_snapshot();
        DmacStatusUnit restored;
        restored.restore_registers(saved);
        check(restored.register_value(0x1000E010u) == 0x00200020u,
              "the DMAC state restores verbatim");
    }

    // A CNT to END chain without TIE still walks and completes: the first
    // tag's data follows the tag, the second tag follows the first payload,
    // and TADR stays on the END tag while the CHCR TAG field names it.
    {
        int fires = 0;
        DmaChannel channel(0x10009000u, 0x1000u, 1,
                           [&](std::uint32_t) { ++fires; });
        GuestMemory memory(0, 0x2000);
        channel.map_into(memory);
        const auto tag_word = [](std::uint32_t qwc, std::uint32_t id,
                                 std::uint32_t irq, std::uint32_t address) {
            return (static_cast<std::uint64_t>(address & 0x7FFFFFFFu) << 32)
                | (static_cast<std::uint64_t>(irq & 1u) << 31)
                | (static_cast<std::uint64_t>(id & 7u) << 28)
                | (qwc & 0xFFFFu);
        };
        memory.write_doubleword(0x1000u,
                                tag_word(1, DmaChannel::tag_cnt, 0, 0));
        std::uint8_t first[16];
        for (std::uint32_t index = 0; index < 16; ++index) {
            first[index] = static_cast<std::uint8_t>(0xA0 + index);
        }
        memory.write_bytes(0x1010u, first);
        memory.write_doubleword(0x1020u,
                                tag_word(1, DmaChannel::tag_end, 0, 0));
        std::uint8_t second[16];
        for (std::uint32_t index = 0; index < 16; ++index) {
            second[index] = static_cast<std::uint8_t>(0xB0 + index);
        }
        memory.write_bytes(0x1030u, second);
        memory.write_word(0x10009030u, 0x1000u);  // TADR
        memory.write_word(0x10009000u, DmaChannel::direction_bit
                                           | (DmaChannel::mode_chain << 2)
                                           | DmaChannel::start_bit);
        check(fires == 1, "a chain without TIE still completes");
        check(channel.payload_bytes().size() == 32
                   && channel.starts().size() == 1
                   && channel.starts()[0].completed
                   && channel.starts()[0].tags_walked == 2
                   && channel.starts()[0].bytes_moved == 32,
              "the chain walks both tags and moves both payloads");
        bool chain_matches = channel.payload_bytes().size() == 32;
        for (std::uint32_t index = 0; chain_matches && index < 16; ++index) {
            chain_matches = channel.payload_bytes()[index] == first[index]
                && channel.payload_bytes()[16 + index] == second[index];
        }
        check(chain_matches, "chained payloads land in transfer order");
        check(channel.register_value(0x10009030u) == 0x1020u
                   && channel.register_value(0x10009010u) == 0x1040u
                   && channel.register_value(0x10009020u) == 0
                   && channel.register_value(0x10009000u) == 0x70000005u,
              "TADR stays on END, MADR walks on, QWC drains, TAG names END");
        check(channel.starts()[0].first_tag_id == DmaChannel::tag_cnt
                   && channel.starts()[0].last_tag_id == DmaChannel::tag_end,
              "the start records the chain's end tag ids");
    }

    // The boot's VIF1 start value (DIR, chain, TTE, TIE, STR): a tag IRQ
    // with TIE set ends the walk after that tag's payload, and the tag's
    // upper bytes precede the payload in the sink.
    {
        int fires = 0;
        DmaChannel channel(0x10009000u, 0x1000u, 1,
                           [&](std::uint32_t) { ++fires; });
        GuestMemory memory(0, 0x2000);
        channel.map_into(memory);
        memory.write_word(0x1100u, 0x90000001u);  // CNT, QWC 1, IRQ
        memory.write_word(0x1104u, 0);
        std::uint8_t tag_upper[8];
        for (std::uint32_t index = 0; index < 8; ++index) {
            tag_upper[index] = static_cast<std::uint8_t>(0x70 + index);
        }
        memory.write_bytes(0x1108u, tag_upper);
        std::uint8_t payload[16];
        for (std::uint32_t index = 0; index < 16; ++index) {
            payload[index] = static_cast<std::uint8_t>(0xC0 + index);
        }
        memory.write_bytes(0x1110u, payload);
        memory.write_word(0x1120u, 0x70000009u);  // END, QWC 9, never reached
        memory.write_word(0x10009030u, 0x1100u);
        memory.write_word(0x10009000u, 0x1C5u);  // the boot's VIF1 CHCR
        check(fires == 1, "an IRQ tag with TIE completes the walk");
        check(channel.starts().size() == 1
                   && channel.starts()[0].tags_walked == 1
                   && channel.starts()[0].bytes_moved == 24,
              "the walk ends after the IRQ tag");
        bool irq_matches = channel.payload_bytes().size() == 24;
        for (std::uint32_t index = 0; irq_matches && index < 8; ++index) {
            irq_matches = channel.payload_bytes()[index] == tag_upper[index];
        }
        for (std::uint32_t index = 0; irq_matches && index < 16; ++index) {
            irq_matches = channel.payload_bytes()[8 + index] == payload[index];
        }
        check(irq_matches, "TTE bytes precede the tag payload in the sink");
        check(channel.register_value(0x10009030u) == 0x1120u
                   && channel.register_value(0x10009000u) == 0x900000C5u,
              "TADR advances past the IRQ tag and TAG names it");
        check(channel.starts()[0].first_tag_id == DmaChannel::tag_cnt
                   && channel.starts()[0].last_tag_id == DmaChannel::tag_cnt,
              "the cut walk records its single tag on both ends");
    }

    // IRQ without TIE walks on: the same layout with TIE clear reaches the
    // END tag and moves its payload too.
    {
        int fires = 0;
        DmaChannel channel(0x10009000u, 0x1000u, 1,
                           [&](std::uint32_t) { ++fires; });
        GuestMemory memory(0, 0x3000);
        channel.map_into(memory);
        memory.write_word(0x1100u, 0x90000001u);  // CNT, QWC 1, IRQ
        memory.write_word(0x1104u, 0);
        std::uint8_t payload[16] = {0};
        memory.write_bytes(0x1110u, payload);
        memory.write_word(0x1120u, 0x70000001u);  // END, QWC 1
        std::uint8_t tail[16];
        for (std::uint32_t index = 0; index < 16; ++index) {
            tail[index] = static_cast<std::uint8_t>(0xD0 + index);
        }
        memory.write_bytes(0x1130u, tail);
        memory.write_word(0x10009030u, 0x1100u);
        memory.write_word(0x10009000u, DmaChannel::direction_bit
                                           | (DmaChannel::mode_chain << 2)
                                           | DmaChannel::start_bit);
        check(fires == 1 && channel.starts().size() == 1
                   && channel.starts()[0].tags_walked == 2
                   && channel.starts()[0].bytes_moved == 32,
              "IRQ without TIE walks past the tag to END");
        bool tail_matches = channel.payload_bytes().size() == 32;
        for (std::uint32_t index = 0; tail_matches && index < 16; ++index) {
            tail_matches = channel.payload_bytes()[16 + index] == tail[index];
        }
        check(tail_matches, "the END payload follows the IRQ tag payload");
    }

    // CALL pushes the return onto the address stack and RET pops it: the
    // sub-chain runs inline, ASP returns to zero, and the payloads stay in
    // transfer order.
    {
        int fires = 0;
        DmaChannel channel(0x1000A000u, 0x1000u, 2,
                           [&](std::uint32_t) { ++fires; });
        GuestMemory memory(0, 0x3000);
        channel.map_into(memory);
        const auto tag_word = [](std::uint32_t qwc, std::uint32_t id,
                                 std::uint32_t address) {
            return (static_cast<std::uint64_t>(address & 0x7FFFFFFFu) << 32)
                | (static_cast<std::uint64_t>(id & 7u) << 28)
                | (qwc & 0xFFFFu);
        };
        memory.write_doubleword(0x1200u,
                                tag_word(0, DmaChannel::tag_call, 0x1260));
        memory.write_doubleword(0x1210u,
                                tag_word(1, DmaChannel::tag_ref, 0x1290));
        memory.write_doubleword(0x1220u,
                                tag_word(0, DmaChannel::tag_end, 0));
        memory.write_doubleword(0x1260u,
                                tag_word(1, DmaChannel::tag_cnt, 0));
        std::uint8_t inner[16];
        for (std::uint32_t index = 0; index < 16; ++index) {
            inner[index] = static_cast<std::uint8_t>(0xE0 + index);
        }
        memory.write_bytes(0x1270u, inner);
        memory.write_doubleword(0x1280u,
                                tag_word(0, DmaChannel::tag_ret, 0));
        std::uint8_t outer[16];
        for (std::uint32_t index = 0; index < 16; ++index) {
            outer[index] = static_cast<std::uint8_t>(0xF0 + index);
        }
        memory.write_bytes(0x1290u, outer);
        memory.write_word(0x1000A030u, 0x1200u);
        memory.write_word(0x1000A000u, DmaChannel::direction_bit
                                           | (DmaChannel::mode_chain << 2)
                                           | DmaChannel::start_bit);
        check(fires == 1 && channel.starts().size() == 1
                   && channel.starts()[0].completed
                   && channel.starts()[0].tags_walked == 5
                   && channel.starts()[0].bytes_moved == 32,
              "CALL, CNT, RET, REF and END walk as one transfer");
        bool call_matches = channel.payload_bytes().size() == 32;
        for (std::uint32_t index = 0; call_matches && index < 16; ++index) {
            call_matches = channel.payload_bytes()[index] == inner[index]
                && channel.payload_bytes()[16 + index] == outer[index];
        }
        check(call_matches, "the sub-chain payload precedes the REF payload");
        check(channel.register_value(0x1000A030u) == 0x1220u
                   && channel.register_value(0x1000A040u) == 0
                   && channel.register_value(0x1000A000u) == 0x70000005u,
              "TADR ends on END, the stack pops clean, TAG names END");
    }

    // A normal start with QWC 0 moves 0x10000 quadwords (the
    // hardware-tested PCSX2 DmaExec rule), not zero bytes.
    {
        int fires = 0;
        DmaChannel channel(0x10008000u, 0x1000u, 0,
                           [&](std::uint32_t) { ++fires; });
        GuestMemory memory(0, 0x100000);
        channel.map_into(memory);
        memory.write_word(0x10008010u, 0u);  // MADR at the zero page
        memory.write_word(0x10008020u, 0u);  // QWC 0
        memory.write_word(0x10008000u, DmaChannel::direction_bit
                                           | DmaChannel::start_bit);
        check(fires == 1 && channel.payload_byte_count() == 0x100000u
                   && channel.register_value(0x10008010u) == 0x100000u
                   && channel.register_value(0x10008020u) == 0,
              "QWC 0 in normal mode moves 0x10000 quadwords");
    }

    // Outside the implemented subset the engine stops loudly with the
    // channel context: no completion, STR still set, the failed start kept
    // with completed false. Never an invented END.
    {
        int fires = 0;
        DmaChannel channel(0x10008000u, 0x1000u, 0,
                           [&](std::uint32_t) { ++fires; });
        GuestMemory memory(0, 0x3000);
        channel.map_into(memory);
        check(throws([&] {
                  memory.write_word(0x10008000u, DmaChannel::start_bit);
              })
                   && fires == 0
                   && channel.register_value(0x10008000u)
                        == DmaChannel::start_bit
                   && channel.starts().size() == 1
                   && !channel.starts()[0].completed,
              "DIR clear stops without completing");
        memory.write_word(0x10008010u, 0x1000u);
        memory.write_word(0x10008020u, 1u);
        memory.write_word(0x10008030u, 0x1000u);
        check(throws([&] {
                  memory.write_word(0x10008000u,
                                    DmaChannel::direction_bit
                                        | (DmaChannel::mode_chain << 2)
                                        | DmaChannel::start_bit);
              })
                   && fires == 0,
              "a chain start with QWC set stops (no resume rule)");
        memory.write_word(0x10008020u, 0u);
        memory.write_word(0x10008030u, 0x9000u);  // outside the RAM
        check(throws([&] {
                  memory.write_word(0x10008000u,
                                    DmaChannel::direction_bit
                                        | (DmaChannel::mode_chain << 2)
                                        | DmaChannel::start_bit);
              })
                   && fires == 0,
              "a tag outside mapped memory stops");
        memory.write_word(0x10008030u, 0x1000u);
        check(throws([&] {
                  memory.write_word(0x10008000u,
                                    DmaChannel::direction_bit
                                        | (DmaChannel::mode_interleave << 2)
                                        | DmaChannel::start_bit);
              })
                   && fires == 0,
              "interleave mode stops");
        // A NEXT loop back to itself walks into the tag budget and stops
        // instead of hanging the test process.
        memory.write_word(0x1000u, 0x20000000u);  // NEXT, QWC 0
        memory.write_word(0x1004u, 0x00001000u);  // ADDR back to itself
        memory.write_word(0x10008030u, 0x1000u);
        check(throws([&] {
                  memory.write_word(0x10008000u,
                                    DmaChannel::direction_bit
                                        | (DmaChannel::mode_chain << 2)
                                        | DmaChannel::start_bit);
              })
                   && fires == 0
                   && channel.starts().back().tags_walked
                        == DmaChannel::max_chain_tags
                   && !channel.starts().back().completed,
              "a tag loop stops at the budget without completing");
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
