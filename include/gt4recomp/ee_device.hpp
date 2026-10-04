#pragma once

// A plain device register bank: 32-bit storage for one hardware register
// window. Reads return the last written value and untouched registers read
// as zero. No side effects, no ticking, no interrupts — the honest default
// for device blocks the verified paths have not needed to drive yet.
//
// The timer unit keeps its own typed class on top of the same storage; DMAC
// and SIF use a bank directly. Only 32-bit accesses are modeled; any other
// width stops with the address instead of guessing a byte order.

#include "gt4recomp/ee_state.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace gt4recomp::ee {

class RegisterBank {
public:
    RegisterBank(std::uint32_t base, std::uint32_t size);

    // Routes the bank's window of the memory to this bank. The bank must
    // outlive the memory (the callbacks capture it).
    void map_into(GuestMemory& memory);

    [[nodiscard]] std::uint32_t read_register(std::uint32_t address,
                                              std::size_t width) const;
    void write_register(std::uint32_t address, std::size_t width,
                        std::uint32_t value);

    // The stored value of one register; zero when never written.
    [[nodiscard]] std::uint32_t register_value(std::uint32_t address) const;

    // A copy of every stored register (address/value, ordered) for
    // snapshots. Restoring replaces the stored registers wholesale,
    // writing storage directly and bypassing the guest write path: no
    // flag acknowledge, mask or completion effect ever runs on restore.
    // MMIO routing is untouched.
    [[nodiscard]] std::vector<std::pair<std::uint32_t, std::uint32_t>>
    registers_snapshot() const;
    void restore_registers(
        std::span<const std::pair<std::uint32_t, std::uint32_t>> entries);

    [[nodiscard]] std::uint32_t base() const noexcept;
    [[nodiscard]] std::uint32_t size() const noexcept;

private:
    std::uint32_t base_ = 0;
    std::uint32_t size_ = 0;
    std::map<std::uint32_t, std::uint32_t> registers_;
};

// A DMA channel control window (VIF0, VIF1, GIF, ...). Like RegisterBank it
// stores 32-bit writes and returns them on reads, with one modeled behavior:
// a write to CHCR that sets the start bit (STR, 0x100) runs the transfer the
// programmed registers describe, then reports the completion. Register layout
// and chain semantics follow PS2Tek DMAC I/O and Chain Mode (revision
// 295bc61) with the tag ids of ps2sdk dma_tags.h: CHCR at +0x00 (DIR bit 0,
// MOD bits 2-3, ASP bits 4-5, TTE bit 6, TIE bit 7, STR bit 8, TAG bits
// 16-31), MADR at +0x10, QWC at +0x20 (low 16 bits, like PCSX2's masked QWC
// write), TADR at +0x30, ASR0/ASR1 at +0x40/+0x50.
//
// Normal mode (MOD 0) moves QWC quadwords from MADR to the channel's device
// sink (a started transfer with QWC 0 moves 0x10000 quadwords, the
// hardware-tested PCSX2 DmaExec rule). Chain mode (MOD 1) walks the source
// chain at TADR: CNT, NEXT, REF, REFE, END, CALL and RET update MADR/TADR
// and the address stack exactly like the reference tables; REFS moves its
// payload like REF but its stall-control handshake (D_CTRL) stays unmodeled.
// Every tag's payload lands in the sink in transfer order, with the tag's
// upper 8 bytes first when TTE is set. A tag carrying IRQ with TIE set ends
// the walk after its payload, and the completion always exists (TIE never
// gates it, per ps2autotests dmac/tagintr @97469ff and decision 0029).
//
// Completion (the channel's DMAC cause through raise_) fires only after the
// implemented transfer concludes: STR clears, QWC reads 0, MADR/TADR hold
// the post-transfer addresses, CHCR's TAG field carries bits 16-31 of the
// last tag read, and ASP tracks the call stack. Anything outside the
// implemented subset stops loudly with the channel and register context
// instead of consuming anything: DIR clear, interleave or reserved modes, a
// chain start with QWC set (the PS2Tek resume rule), scratchpad-selected
// addresses, unaligned or unmapped tag and data addresses, a CALL with a
// full stack, and a chain longer than max_chain_tags. In particular the
// engine never invents TAG END: an unfinished walk never completes.
//
// The sink keeps the moved bytes (with an FNV-1a hash) only as a runtime
// diagnostic for the graphics-packet capture (requirements M33): no VIF/GIF
// consumer drains it yet, so later stages will replace the retention with a
// streaming handoff. The start log and the sink are diagnostics, never guest
// state: restoring registers replaces the bank wholesale and restarts the
// diagnostics empty, and snapshots keep the bank-only format, so checkpoint
// compatibility is unchanged by this slice (decision 0033).
class DmaChannel {
public:
    static constexpr std::uint32_t chcr_offset = 0x00;
    static constexpr std::uint32_t madr_offset = 0x10;
    static constexpr std::uint32_t qwc_offset = 0x20;
    static constexpr std::uint32_t tadr_offset = 0x30;
    static constexpr std::uint32_t asr0_offset = 0x40;
    static constexpr std::uint32_t asr1_offset = 0x50;
    static constexpr std::uint32_t direction_bit = 0x00000001;  // DIR
    static constexpr std::uint32_t mode_mask = 0x0000000C;      // MOD
    static constexpr std::uint32_t mode_normal = 0;
    static constexpr std::uint32_t mode_chain = 1;
    static constexpr std::uint32_t mode_interleave = 2;
    static constexpr std::uint32_t address_stack_mask = 0x00000030;  // ASP
    static constexpr std::uint32_t tag_transfer_enable = 0x00000040;  // TTE
    static constexpr std::uint32_t interrupt_enable = 0x00000080;  // TIE
    static constexpr std::uint32_t start_bit = 0x00000100;         // STR
    static constexpr std::uint32_t tag_field_mask = 0xFFFF0000;    // TAG
    static constexpr std::uint32_t qwc_mask = 0x0000FFFF;
    static constexpr std::uint32_t address_mask = 0x7FFFFFFF;
    static constexpr std::uint32_t scratchpad_select = 0x80000000;
    static constexpr std::uint32_t tag_refe = 0;
    static constexpr std::uint32_t tag_cnt = 1;
    static constexpr std::uint32_t tag_next = 2;
    static constexpr std::uint32_t tag_ref = 3;
    static constexpr std::uint32_t tag_refs = 4;
    static constexpr std::uint32_t tag_call = 5;
    static constexpr std::uint32_t tag_ret = 6;
    static constexpr std::uint32_t tag_end = 7;
    // Anti-hang guard for a corrupt NEXT loop: a model policy, not hardware.
    static constexpr std::uint32_t max_chain_tags = 65536;

    // One started transfer as the guest programmed it, with what the engine
    // did with it. completed stays false when the start stopped loudly. The
    // tag ids name the walked chain's ends (no_tag_walked when normal mode
    // reads no tag); they are the packet-capture hook M33 builds on.
    static constexpr std::uint32_t no_tag_walked = 0xFFFFFFFFu;
    struct StartRecord {
        std::uint32_t chcr = 0;
        std::uint32_t madr = 0;
        std::uint32_t qwc = 0;
        std::uint32_t tadr = 0;
        std::uint32_t tags_walked = 0;
        std::uint64_t bytes_moved = 0;
        std::uint32_t first_tag_id = no_tag_walked;
        std::uint32_t last_tag_id = no_tag_walked;
        bool completed = false;
    };

    DmaChannel(std::uint32_t base, std::uint32_t size, std::uint32_t cause,
               std::function<void(std::uint32_t)> raise);

    // Routes the channel's window of the memory to this channel. The
    // channel must outlive the memory (the callbacks capture it).
    void map_into(GuestMemory& memory);

    // Re-points the transfer engine at a moved memory: make_boot_state maps
    // the devices into a local GuestMemory and then moves it into the
    // GuestState, so the engine must follow the move. MMIO routing moves
    // with the memory itself; only this back-pointer needs the fixup.
    void rebind_memory(GuestMemory& memory) noexcept;

    [[nodiscard]] std::uint32_t register_value(std::uint32_t address) const;
    [[nodiscard]] std::uint32_t base() const noexcept;
    [[nodiscard]] std::uint32_t size() const noexcept;

    // Snapshot passthrough to the channel's bank. Restoring never fires a
    // completion: it writes the bank's storage directly, bypassing the
    // start-bit behavior of a live write, and restarts the transfer
    // diagnostics empty (a resumed run re-records from the resume point).
    [[nodiscard]] std::vector<std::pair<std::uint32_t, std::uint32_t>>
    registers_snapshot() const;
    void restore_registers(
        std::span<const std::pair<std::uint32_t, std::uint32_t>> entries);

    // The transfer diagnostics: every STR write in order (completed or
    // stopped), the first payload_retain_cap device-bound bytes, the full
    // moved count, and their FNV-1a hash over every byte. Runtime only,
    // never checkpointed, never compared across engines.
    [[nodiscard]] const std::vector<StartRecord>& starts() const noexcept;
    [[nodiscard]] const std::vector<std::uint8_t>& payload_bytes() const noexcept;
    [[nodiscard]] std::uint64_t payload_byte_count() const noexcept;
    [[nodiscard]] std::uint32_t payload_hash() const noexcept;

private:
    [[nodiscard]] std::uint32_t read_register(std::uint32_t address,
                                              std::size_t width) const;
    void write_register(std::uint32_t address, std::size_t width,
                        std::uint32_t value);

    // Runs the transfer a STR write programmed: normal mode moves QWC
    // quadwords from MADR, chain mode walks the tags at TADR. Updates the
    // registers along the way and throws with the channel context when the
    // programmed transfer leaves the implemented subset.
    void run_transfer(StartRecord& start);
    void run_normal_transfer(StartRecord& start);
    void run_chain_transfer(StartRecord& start);
    // Moves byte_count bytes from the guest address into the device sink,
    // stopping loudly when the source is not plain mapped RAM.
    void move_bytes(std::uint32_t address, std::uint64_t byte_count,
                    StartRecord& start);
    [[nodiscard]] std::uint32_t load_register(std::uint32_t offset) const;
    void store_register(std::uint32_t offset, std::uint32_t value);
    [[noreturn]] void stop_transfer(const std::string& reason) const;

    RegisterBank bank_;
    std::uint32_t base_ = 0;
    // The channel's DMAC cause (VIF0 = 0, VIF1 = 1, GIF = 2, ...), reported
    // through raise_ only after an implemented transfer concludes. Never an
    // INTC cause: VIF command IRQs 4/5 stay INTC-side for the future VIF
    // consumer, and a GIF completion never touches Timer0's INTC 9.
    std::uint32_t cause_ = 0;
    std::function<void(std::uint32_t)> raise_;
    // The mapped guest memory the engine reads tags and payloads from. Set
    // by map_into; a start with no mapped memory stops loudly.
    GuestMemory* memory_ = nullptr;
    std::vector<StartRecord> starts_;
    // How many moved bytes stay resident for inspection. Counting and
    // hashing stream everything; this only bounds the tap's host memory.
    static constexpr std::size_t payload_retain_cap = 1u << 20;
    std::vector<std::uint8_t> payload_;
    std::uint64_t payload_bytes_moved_ = 0;
    std::uint32_t payload_hash_ = 2166136261u;  // FNV-1a 32-bit basis
};

// The EE interrupt controller window (INTC_STAT at +0x00, INTC_MASK at
// +0x10, inside the 0x1000F000 block). Guest and internal operations stay
// separate:
//   guest write to STAT clears the named bits (W1C, PCSX2 HwWrite.cpp
//     @81526d4: psHu32(INTC_STAT) &= ~value);
//   guest write to MASK toggles the named low 16 bits (PCSX2 HwWrite.cpp
//     @81526d4: psHu32(INTC_MASK) ^= (u16)value);
//   internal set/enable OR bits; internal disable clears a mask bit.
// A restore writes storage directly and never toggles or acknowledges.
// Pending (STAT) exists whether or not a handler is registered or the mask
// allows delivery; choosing whether to call a handler is the dispatch step.
class IntcUnit {
public:
    static constexpr std::uint32_t window_base = 0x1000F000;
    static constexpr std::uint32_t window_size = 0x100;
    static constexpr std::uint32_t stat_offset = 0x00;
    static constexpr std::uint32_t mask_offset = 0x10;
    static constexpr std::uint32_t mask_write_bits = 0x0000FFFF;

    IntcUnit() = default;

    // Routes the window of the memory to this unit. The unit must outlive
    // the memory (the callbacks capture it).
    void map_into(GuestMemory& memory);

    [[nodiscard]] std::uint32_t read_register(std::uint32_t address,
                                              std::size_t width) const;
    void write_register(std::uint32_t address, std::size_t width,
                        std::uint32_t value);

    // The stored value of one register; zero when never written.
    [[nodiscard]] std::uint32_t register_value(std::uint32_t address) const;

    // Snapshot/restore pair. Restoring replaces the state wholesale,
    // bypassing the guest W1C/toggle path (decision 0028).
    [[nodiscard]] std::vector<std::pair<std::uint32_t, std::uint32_t>>
    registers_snapshot() const;
    void restore_registers(
        std::span<const std::pair<std::uint32_t, std::uint32_t>> entries);

    // Device-internal operations (occurrence, privileged enable/disable).
    void set_pending_internal(std::uint32_t cause);
    void enable_internal(std::uint32_t cause);
    void disable_internal(std::uint32_t cause);
    [[nodiscard]] bool is_pending(std::uint32_t cause) const noexcept;
    [[nodiscard]] bool mask_allows(std::uint32_t cause) const noexcept;

    [[nodiscard]] std::uint32_t base() const noexcept;
    [[nodiscard]] std::uint32_t size() const noexcept;

private:
    std::uint32_t stat_ = 0;
    std::uint32_t mask_ = 0;
    std::map<std::uint32_t, std::uint32_t> rest_;
};

// The DMAC status window (D_STAT at +0x10 inside the 0x1000E000 block).
// The low 16 bits are the per-channel completion status (CIS), the high 16
// are the per-channel mask (CIM):
//   guest write clears CIS bits named with 1 (W1C) and toggles the named
//     CIM bits (PCSX2 Dmac.cpp @81526d4: cis &= ~(value & 0xffff),
//     cim ^= (value >> 16));
//   internal completion ORs a CIS bit; internal enable/disable set/clear a
//     CIM bit.
// A restore writes storage directly. A completion stays pending whether or
// not a handler is registered; the mask decides delivery, never existence.
// The global DMAE/D_ENABLER gates that PCSX2 checks in dmacInterrupt are
// NOT modeled: nothing in the verified boot path programs them yet, so
// requiring them would refuse completions the game demonstrably consumes.
// That gate is P06 territory and stays an explicit limit here.
class DmacStatusUnit {
public:
    static constexpr std::uint32_t window_base = 0x1000E000;
    static constexpr std::uint32_t window_size = 0x100;
    static constexpr std::uint32_t stat_offset = 0x10;
    static constexpr std::uint32_t status_bits = 0x0000FFFF;

    DmacStatusUnit() = default;

    // Routes the window of the memory to this unit. The unit must outlive
    // the memory (the callbacks capture it).
    void map_into(GuestMemory& memory);

    [[nodiscard]] std::uint32_t read_register(std::uint32_t address,
                                              std::size_t width) const;
    void write_register(std::uint32_t address, std::size_t width,
                        std::uint32_t value);

    // The stored value of one register; zero when never written.
    [[nodiscard]] std::uint32_t register_value(std::uint32_t address) const;

    // Snapshot/restore pair. Restoring replaces the state wholesale,
    // bypassing the guest W1C/toggle path (decision 0028).
    [[nodiscard]] std::vector<std::pair<std::uint32_t, std::uint32_t>>
    registers_snapshot() const;
    void restore_registers(
        std::span<const std::pair<std::uint32_t, std::uint32_t>> entries);

    // Device-internal operations (completion, privileged enable/disable).
    void set_completion_internal(std::uint32_t channel);
    void enable_internal(std::uint32_t channel);
    void disable_internal(std::uint32_t channel);
    [[nodiscard]] bool completion_pending(std::uint32_t channel) const noexcept;
    [[nodiscard]] bool mask_allows(std::uint32_t channel) const noexcept;

    [[nodiscard]] std::uint32_t base() const noexcept;
    [[nodiscard]] std::uint32_t size() const noexcept;

private:
    std::uint32_t completion_status_ = 0;  // CIS, low 16 bits
    std::uint32_t completion_mask_ = 0;    // CIM, low 16 bits
    std::map<std::uint32_t, std::uint32_t> rest_;
};

} // namespace gt4recomp::ee
