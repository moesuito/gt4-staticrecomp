#include "gt4recomp/ee_device.hpp"

#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace gt4recomp::ee {
namespace {

std::string access_text(const char* name, std::uint32_t address, std::size_t width) {
    std::ostringstream message;
    message << "Device register " << name << " access at 0x" << std::hex
            << std::setfill('0') << std::setw(8) << address << " (width "
            << std::dec << width << ") is not modeled";
    return message.str();
}

} // namespace

RegisterBank::RegisterBank(std::uint32_t base, std::uint32_t size)
    : base_(base), size_(size) {
    if (size == 0) {
        throw std::runtime_error("A device register bank needs a nonzero size");
    }
}

void RegisterBank::map_into(GuestMemory& memory) {
    memory.map_mmio(
        base_, size_,
        [this](std::uint32_t address, std::size_t width) {
            return read_register(address, width);
        },
        [this](std::uint32_t address, std::size_t width, std::uint32_t value) {
            write_register(address, width, value);
        });
}

std::uint32_t RegisterBank::register_value(std::uint32_t address) const {
    const auto found = registers_.find(address);
    return found == registers_.end() ? 0 : found->second;
}

std::uint32_t RegisterBank::read_register(std::uint32_t address,
                                          std::size_t width) const {
    if (width != 4) {
        throw std::runtime_error(access_text("read", address, width));
    }
    return register_value(address);
}

void RegisterBank::write_register(std::uint32_t address, std::size_t width,
                                  std::uint32_t value) {
    if (width != 4) {
        throw std::runtime_error(access_text("write", address, width));
    }
    registers_[address] = value;
}

std::uint32_t RegisterBank::base() const noexcept {
    return base_;
}

std::uint32_t RegisterBank::size() const noexcept {
    return size_;
}

std::vector<std::pair<std::uint32_t, std::uint32_t>>
RegisterBank::registers_snapshot() const {
    return {registers_.begin(), registers_.end()};
}

void RegisterBank::restore_registers(
    std::span<const std::pair<std::uint32_t, std::uint32_t>> entries) {
    // Straight into storage, never through write_register: a restore is a
    // verbatim photo, so future guest-write effects (flag acknowledge,
    // masks, completions) must not run here. The device contract keeps its
    // write path; only this restore path bypasses it (decision 0028).
    registers_.clear();
    for (const auto& [address, value] : entries) {
        registers_[address] = value;
    }
}

DmaChannel::DmaChannel(std::uint32_t base, std::uint32_t size,
                       std::uint32_t cause,
                       std::function<void(std::uint32_t)> raise)
    : bank_(base, size), base_(base), cause_(cause), raise_(std::move(raise)) {
    if (!raise_) {
        throw std::runtime_error("A DMA channel needs a completion callback");
    }
}

void DmaChannel::map_into(GuestMemory& memory) {
    memory_ = &memory;
    memory.map_mmio(
        base_, bank_.size(),
        [this](std::uint32_t address, std::size_t width) {
            return read_register(address, width);
        },
        [this](std::uint32_t address, std::size_t width, std::uint32_t value) {
            write_register(address, width, value);
        });
}

void DmaChannel::rebind_memory(GuestMemory& memory) noexcept {
    memory_ = &memory;
}

std::uint32_t DmaChannel::register_value(std::uint32_t address) const {
    return bank_.register_value(address);
}

std::uint32_t DmaChannel::base() const noexcept {
    return base_;
}

std::uint32_t DmaChannel::size() const noexcept {
    return bank_.size();
}

std::vector<std::pair<std::uint32_t, std::uint32_t>>
DmaChannel::registers_snapshot() const {
    return bank_.registers_snapshot();
}

void DmaChannel::restore_registers(
    std::span<const std::pair<std::uint32_t, std::uint32_t>> entries) {
    // Straight into the bank's storage: a live write would run a transfer
    // whose start bit is set, which a restore must never fire. The transfer
    // diagnostics restart empty: they describe this run's transfers, and a
    // resumed run re-records from the resume point.
    bank_.restore_registers(entries);
    starts_.clear();
    payload_.clear();
    payload_bytes_moved_ = 0;
    payload_hash_ = 2166136261u;
}

const std::vector<DmaChannel::StartRecord>& DmaChannel::starts() const noexcept {
    return starts_;
}

const std::vector<std::uint8_t>& DmaChannel::payload_bytes() const noexcept {
    return payload_;
}

std::uint64_t DmaChannel::payload_byte_count() const noexcept {
    return payload_bytes_moved_;
}

std::uint32_t DmaChannel::payload_hash() const noexcept {
    return payload_hash_;
}

std::uint32_t DmaChannel::read_register(std::uint32_t address,
                                        std::size_t width) const {
    return bank_.read_register(address, width);
}

std::uint32_t DmaChannel::load_register(std::uint32_t offset) const {
    return bank_.register_value(base_ + offset);
}

void DmaChannel::store_register(std::uint32_t offset, std::uint32_t value) {
    bank_.write_register(base_ + offset, 4, value);
}

void DmaChannel::stop_transfer(const std::string& reason) const {
    std::ostringstream message;
    message << "DMA channel at 0x" << std::hex << std::setfill('0')
            << std::setw(8) << base_ << ": " << reason;
    throw std::runtime_error(message.str());
}

void DmaChannel::move_bytes(std::uint32_t address, std::uint64_t byte_count,
                            StartRecord& start) {
    if (byte_count == 0) {
        return;
    }
    if (memory_ == nullptr) {
        stop_transfer("a transfer needs mapped guest memory");
    }
    if (memory_->is_mmio(address, static_cast<std::size_t>(byte_count))) {
        stop_transfer("a device-window source is not a modeled transfer");
    }
    if (!memory_->contains(address, static_cast<std::size_t>(byte_count))) {
        std::ostringstream reason;
        reason << "source 0x" << std::hex << std::setfill('0') << std::setw(8)
               << address << " plus 0x" << byte_count
               << " bytes leaves the mapped guest memory";
        stop_transfer(reason.str());
    }
    for (std::uint64_t offset = 0; offset < byte_count; ++offset) {
        const std::uint8_t byte =
            memory_->read_byte(address + static_cast<std::uint32_t>(offset));
        // The retained tap is bounded: counting and hashing stream every
        // byte, but only the first payload_retain_cap bytes stay resident,
        // so a long march cannot grow host memory without bound.
        if (payload_.size() < payload_retain_cap) {
            payload_.push_back(byte);
        }
        payload_hash_ ^= byte;
        payload_hash_ *= 16777619u;
    }
    payload_bytes_moved_ += byte_count;
    start.bytes_moved += byte_count;
}

void DmaChannel::run_normal_transfer(StartRecord& start) {
    if ((start.madr & scratchpad_select) != 0) {
        stop_transfer("a scratchpad MADR source is not modeled");
    }
    // PCSX2's DmaExec carries a hardware-tested rule: a normal-mode start
    // with QWC 0 transfers 1 quadword, underflows, and then moves another
    // 0xFFFF quadwords, so the engine counts 0x10000.
    const std::uint64_t quadwords =
        start.qwc == 0 ? 0x10000u : start.qwc;
    const std::uint32_t source = start.madr & address_mask;
    move_bytes(source, quadwords * 16, start);
    // No tag was read, so the CHCR TAG field is untouched: only STR clears.
    // MADR walks past the moved bytes and QWC drains to zero.
    store_register(madr_offset, source + static_cast<std::uint32_t>(quadwords * 16));
    store_register(qwc_offset, 0);
    store_register(chcr_offset, start.chcr & ~start_bit);
}

void DmaChannel::run_chain_transfer(StartRecord& start) {
    // A chain start carries QWC 0 with the first tag at TADR (ps2sdk
    // dma_channel_send_chain programs exactly that). A start with QWC set is
    // the PS2Tek resume path (the movie library's trick), which needs the
    // CHCR TAG field as evidence and stays unmodeled.
    if (start.qwc != 0) {
        stop_transfer("a chain start with QWC set needs the resume rule");
    }
    if ((start.tadr & scratchpad_select) != 0) {
        stop_transfer("a scratchpad tag stream is not modeled");
    }
    if ((start.tadr & 0xFu) != 0) {
        stop_transfer("a TADR outside a 16-byte boundary is not a tag");
    }
    if (((start.chcr & address_stack_mask) >> 4) != 0) {
        stop_transfer("a chain start with a kept address stack is not modeled");
    }
    const bool tie = (start.chcr & interrupt_enable) != 0;
    const bool tag_transfer = (start.chcr & tag_transfer_enable) != 0;
    std::uint32_t chcr = start.chcr;
    std::uint32_t tadr = start.tadr & address_mask;
    std::uint32_t madr = 0;
    std::uint32_t address_stack = 0;
    while (true) {
        if (start.tags_walked >= max_chain_tags) {
            stop_transfer("a chain past the tag budget looks like a loop");
        }
        if (memory_ == nullptr) {
            stop_transfer("a transfer needs mapped guest memory");
        }
        if (memory_->is_mmio(tadr, 16)) {
            std::ostringstream reason;
            reason << "tag at 0x" << std::hex << std::setfill('0')
                   << std::setw(8) << tadr << " inside a device window"
                   << " (chcr 0x" << std::setw(8) << start.chcr << " madr 0x"
                   << std::setw(8) << start.madr << " qwc 0x" << std::setw(8)
                   << start.qwc << ") is not a modeled transfer";
            stop_transfer(reason.str());
        }
        if (!memory_->contains(tadr, 16)) {
            std::ostringstream reason;
            reason << "tag at 0x" << std::hex << std::setfill('0')
                   << std::setw(8) << tadr << " leaves the mapped guest memory";
            stop_transfer(reason.str());
        }
        const std::uint32_t tag_low = memory_->read_word(tadr);
        const std::uint32_t tag_mid = memory_->read_word(tadr + 4);
        // The tag's own address: TTE moves the upper 8 bytes from here, and
        // the switch below advances tadr past them.
        const std::uint32_t tag_location = tadr;
        const std::uint64_t tag =
            (static_cast<std::uint64_t>(tag_mid) << 32) | tag_low;
        const std::uint32_t quadwords =
            static_cast<std::uint32_t>(tag & 0xFFFFu);
        const std::uint32_t id =
            static_cast<std::uint32_t>((tag >> 28) & 0x7u);
        const bool irq = ((tag >> 31) & 0x1u) != 0;
        const std::uint32_t tag_address =
            static_cast<std::uint32_t>((tag >> 32) & address_mask);
        const bool tag_scratchpad = ((tag >> 63) & 0x1u) != 0;
        // Bits 16-31 of the tag land in the CHCR TAG field (PS2Tek DMAC I/O),
        // so a stop read names the last tag the engine saw.
        chcr = (chcr & 0x0000FFFFu)
            | static_cast<std::uint32_t>(tag & 0xFFFF0000u);
        store_register(chcr_offset, chcr);
        const auto use_address = [&](const char* what) {
            if (tag_scratchpad) {
                std::string reason(what);
                reason += " from the scratchpad is not modeled";
                stop_transfer(reason);
            }
            if ((tag_address & 0xFu) != 0) {
                std::ostringstream reason;
                reason << what << " 0x" << std::hex << std::setfill('0')
                       << std::setw(8) << tag_address
                       << " outside a 16-byte boundary is not a tag address";
                stop_transfer(reason.str());
            }
        };
        bool end_after = false;
        if (start.tags_walked == 0) {
            start.first_tag_id = id;
        }
        start.last_tag_id = id;
        switch (id) {
            case tag_refe:
                use_address("REFE");
                madr = tag_address;
                tadr += 16;
                end_after = true;
                break;
            case tag_cnt:
                madr = tadr + 16;
                tadr = madr + quadwords * 16;
                break;
            case tag_next:
                use_address("NEXT");
                madr = tadr + 16;
                tadr = tag_address;
                break;
            case tag_ref:
            case tag_refs:
                // REFS moves its payload like REF; its stall-control
                // handshake against D_CTRL stays a documented limit.
                use_address(id == tag_ref ? "REF" : "REFS");
                madr = tag_address;
                tadr += 16;
                break;
            case tag_call:
                use_address("CALL");
                madr = tadr + 16;
                if (address_stack == 0) {
                    store_register(asr0_offset, madr + quadwords * 16);
                    address_stack = 1;
                } else if (address_stack == 1) {
                    store_register(asr1_offset, madr + quadwords * 16);
                    address_stack = 2;
                } else {
                    stop_transfer("CALL with a full address stack is not modeled");
                }
                tadr = tag_address;
                break;
            case tag_ret:
                madr = tadr + 16;
                if (address_stack == 2) {
                    tadr = load_register(asr1_offset);
                    store_register(asr1_offset, 0);
                    address_stack = 1;
                } else if (address_stack == 1) {
                    tadr = load_register(asr0_offset);
                    store_register(asr0_offset, 0);
                    address_stack = 0;
                } else {
                    end_after = true;
                }
                break;
            case tag_end:
                madr = tadr + 16;
                // TADR stays on the END tag: the reference keeps it so a
                // resume names the last tag (Soul Calibur II and III).
                end_after = true;
                break;
            default:
                stop_transfer("a tag id outside 0-7 is not a DMA tag");
                break;
        }
        chcr = (chcr & ~address_stack_mask) | (address_stack << 4);
        store_register(chcr_offset, chcr);
        // With TTE the tag's upper 8 bytes reach the device before the
        // payload (PS2Tek chain mode); with plain tags only QWC moves.
        if (tag_transfer) {
            move_bytes(tag_location + 8, 8, start);
        }
        move_bytes(madr, static_cast<std::uint64_t>(quadwords) * 16, start);
        store_register(madr_offset, madr + quadwords * 16);
        ++start.tags_walked;
        // A tag IRQ with TIE set ends the walk after the payload and the
        // chain update (PCSX2 VIF1/GIF order); IRQ without TIE walks on.
        if (end_after || (irq && tie)) {
            break;
        }
    }
    store_register(tadr_offset, tadr);
    store_register(qwc_offset, 0);
    store_register(chcr_offset, chcr & ~start_bit);
}

void DmaChannel::run_transfer(StartRecord& start) {
    if ((start.chcr & direction_bit) == 0) {
        stop_transfer("DIR clear is not a modeled transfer on this channel");
    }
    const std::uint32_t mode = (start.chcr & mode_mask) >> 2;
    if (mode == mode_normal) {
        run_normal_transfer(start);
        return;
    }
    if (mode == mode_chain) {
        run_chain_transfer(start);
        return;
    }
    stop_transfer("a mode outside normal and chain is not modeled");
}

void DmaChannel::write_register(std::uint32_t address, std::size_t width,
                                std::uint32_t value) {
    if (address == base_ + qwc_offset && width == 4) {
        // Only the low 16 bits count (PS2Tek QWC, PCSX2's masked QWC write).
        value &= qwc_mask;
    }
    if (address == base_ + chcr_offset && width == 4
        && (value & start_bit) != 0) {
        if (memory_ == nullptr) {
            stop_transfer("a start with no mapped memory is not a transfer");
        }
        // The programmed registers land first, so a loud stop leaves the
        // channel exactly as the guest armed it (STR still set, like a hung
        // channel). The start log keeps the failed start with completed
        // false; only a concluded transfer reports its DMAC completion.
        bank_.write_register(address, width, value);
        starts_.push_back(StartRecord{value, load_register(madr_offset),
                                      load_register(qwc_offset),
                                      load_register(tadr_offset), 0, 0,
                                      no_tag_walked, no_tag_walked, false});
        run_transfer(starts_.back());
        starts_.back().completed = true;
        raise_(cause_);
        return;
    }
    bank_.write_register(address, width, value);
}

void IntcUnit::map_into(GuestMemory& memory) {
    memory.map_mmio(
        window_base, window_size,
        [this](std::uint32_t address, std::size_t width) {
            return read_register(address, width);
        },
        [this](std::uint32_t address, std::size_t width, std::uint32_t value) {
            write_register(address, width, value);
        });
}

std::uint32_t IntcUnit::register_value(std::uint32_t address) const {
    if (address == window_base + stat_offset) {
        return stat_;
    }
    if (address == window_base + mask_offset) {
        return mask_;
    }
    const auto found = rest_.find(address);
    return found == rest_.end() ? 0 : found->second;
}

std::uint32_t IntcUnit::read_register(std::uint32_t address,
                                      std::size_t width) const {
    if (width != 4) {
        throw std::runtime_error(access_text("read", address, width));
    }
    return register_value(address);
}

void IntcUnit::write_register(std::uint32_t address, std::size_t width,
                              std::uint32_t value) {
    if (width != 4) {
        throw std::runtime_error(access_text("write", address, width));
    }
    if (address == window_base + stat_offset) {
        // Write-1-to-clear: acknowledging one cause leaves the others.
        stat_ &= ~value;
        return;
    }
    if (address == window_base + mask_offset) {
        // The mask register toggles the named low 16 bits.
        mask_ ^= (value & mask_write_bits);
        return;
    }
    rest_[address] = value;
}

std::vector<std::pair<std::uint32_t, std::uint32_t>>
IntcUnit::registers_snapshot() const {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> out;
    out.emplace_back(window_base + stat_offset, stat_);
    out.emplace_back(window_base + mask_offset, mask_);
    for (const auto& entry : rest_) {
        out.emplace_back(entry);
    }
    return out;
}

void IntcUnit::restore_registers(
    std::span<const std::pair<std::uint32_t, std::uint32_t>> entries) {
    // Straight into storage, never through write_register: a restore must
    // not acknowledge flags nor toggle masks.
    stat_ = 0;
    mask_ = 0;
    rest_.clear();
    for (const auto& [address, value] : entries) {
        if (address == window_base + stat_offset) {
            stat_ = value;
        } else if (address == window_base + mask_offset) {
            mask_ = value;
        } else {
            rest_[address] = value;
        }
    }
}

void IntcUnit::set_pending_internal(std::uint32_t cause) {
    if (cause < 32) {
        stat_ |= (1u << cause);
    }
}

void IntcUnit::enable_internal(std::uint32_t cause) {
    if (cause < 32) {
        mask_ |= (1u << cause);
    }
}

void IntcUnit::disable_internal(std::uint32_t cause) {
    if (cause < 32) {
        mask_ &= ~(1u << cause);
    }
}

bool IntcUnit::is_pending(std::uint32_t cause) const noexcept {
    return cause < 32 && (stat_ & (1u << cause)) != 0;
}

bool IntcUnit::mask_allows(std::uint32_t cause) const noexcept {
    return cause < 32 && (mask_ & (1u << cause)) != 0;
}

std::uint32_t IntcUnit::base() const noexcept {
    return window_base;
}

std::uint32_t IntcUnit::size() const noexcept {
    return window_size;
}

void DmacStatusUnit::map_into(GuestMemory& memory) {
    memory.map_mmio(
        window_base, window_size,
        [this](std::uint32_t address, std::size_t width) {
            return read_register(address, width);
        },
        [this](std::uint32_t address, std::size_t width, std::uint32_t value) {
            write_register(address, width, value);
        });
}

std::uint32_t DmacStatusUnit::register_value(std::uint32_t address) const {
    if (address == window_base + stat_offset) {
        return (completion_mask_ << 16) | completion_status_;
    }
    const auto found = rest_.find(address);
    return found == rest_.end() ? 0 : found->second;
}

std::uint32_t DmacStatusUnit::read_register(std::uint32_t address,
                                            std::size_t width) const {
    if (width != 4) {
        throw std::runtime_error(access_text("read", address, width));
    }
    return register_value(address);
}

void DmacStatusUnit::write_register(std::uint32_t address, std::size_t width,
                                    std::uint32_t value) {
    if (width != 4) {
        throw std::runtime_error(access_text("write", address, width));
    }
    if (address == window_base + stat_offset) {
        // Low half W1C (acknowledge completions one by one), high half
        // toggle (mask bits flip where the value names 1).
        completion_status_ &= ~(value & status_bits);
        completion_mask_ ^= (value >> 16) & status_bits;
        return;
    }
    rest_[address] = value;
}

std::vector<std::pair<std::uint32_t, std::uint32_t>>
DmacStatusUnit::registers_snapshot() const {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> out;
    out.emplace_back(window_base + stat_offset,
                     (completion_mask_ << 16) | completion_status_);
    for (const auto& entry : rest_) {
        out.emplace_back(entry);
    }
    return out;
}

void DmacStatusUnit::restore_registers(
    std::span<const std::pair<std::uint32_t, std::uint32_t>> entries) {
    // Straight into storage, never through write_register: a restore must
    // not acknowledge completions nor toggle masks.
    completion_status_ = 0;
    completion_mask_ = 0;
    rest_.clear();
    for (const auto& [address, value] : entries) {
        if (address == window_base + stat_offset) {
            completion_status_ = value & status_bits;
            completion_mask_ = (value >> 16) & status_bits;
        } else {
            rest_[address] = value;
        }
    }
}

void DmacStatusUnit::set_completion_internal(std::uint32_t channel) {
    if (channel < 16) {
        completion_status_ |= (1u << channel);
    }
}

void DmacStatusUnit::enable_internal(std::uint32_t channel) {
    if (channel < 16) {
        completion_mask_ |= (1u << channel);
    }
}

void DmacStatusUnit::disable_internal(std::uint32_t channel) {
    if (channel < 16) {
        completion_mask_ &= ~(1u << channel);
    }
}

bool DmacStatusUnit::completion_pending(std::uint32_t channel) const noexcept {
    return channel < 16 && (completion_status_ & (1u << channel)) != 0;
}

bool DmacStatusUnit::mask_allows(std::uint32_t channel) const noexcept {
    return channel < 16 && (completion_mask_ & (1u << channel)) != 0;
}

std::uint32_t DmacStatusUnit::base() const noexcept {
    return window_base;
}

std::uint32_t DmacStatusUnit::size() const noexcept {
    return window_size;
}

} // namespace gt4recomp::ee
