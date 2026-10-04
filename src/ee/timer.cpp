#include "gt4recomp/ee_timer.hpp"

#include <stdexcept>

namespace gt4recomp::ee {

void TimerUnit::map_into(GuestMemory& memory) {
    memory.map_mmio(
        window_base, window_size,
        [this](std::uint32_t address, std::size_t width) {
            return read_register(address, width);
        },
        [this](std::uint32_t address, std::size_t width, std::uint32_t value) {
            write_register(address, width, value);
        });
}

bool TimerUnit::decode(std::uint32_t address, std::uint32_t& index,
                       std::uint32_t& kind) noexcept {
    if (address < window_base || address >= window_base + window_size) {
        return false;
    }
    const std::uint32_t relative = address - window_base;
    index = relative / timer_stride;
    if (index >= timer_count) {
        return false;
    }
    const std::uint32_t offset = relative % timer_stride;
    if (offset == count_offset) {
        kind = 0;
        return true;
    }
    if (offset == mode_offset) {
        kind = 1;
        return true;
    }
    if (offset == compare_offset) {
        kind = 2;
        return true;
    }
    if (offset == hold_offset) {
        kind = 3;
        return true;
    }
    return false;
}

std::uint32_t TimerUnit::read_register(std::uint32_t address,
                                       std::size_t width) const {
    if (width != 4) {
        throw std::runtime_error(
            "Timer register access with a non-32-bit width is not modeled");
    }
    return register_value(address);
}

void TimerUnit::write_register(std::uint32_t address, std::size_t width,
                               std::uint32_t value) {
    if (width != 4) {
        throw std::runtime_error(
            "Timer register access with a non-32-bit width is not modeled");
    }
    std::uint32_t index = 0;
    std::uint32_t kind = 0;
    if (!decode(address, index, kind)) {
        spare_[address] = value;
        return;
    }
    TimerState& timer = timers_[index];
    switch (kind) {
    case 0:  // COUNT: the counter takes the low 16 bits.
        timer.count = value & count_mask;
        return;
    case 1:  // MODE: writable control plus W1C flag acknowledge
        // (PCSX2 rcntWmode @81526d4: clear the flags the value names with
        // 1, then take the low 10 control bits).
        timer.mode &= ~(value & mode_flag_mask);
        timer.mode = (timer.mode & mode_flag_mask) | (value & mode_control_mask);
        return;
    case 2:  // COMP: the target takes the low 16 bits.
        timer.compare = value & compare_mask;
        return;
    default:  // HOLD: storage (real only on T0/T1; see the header).
        timer.hold = value & hold_mask;
        return;
    }
}

std::uint32_t TimerUnit::register_value(std::uint32_t address) const {
    std::uint32_t index = 0;
    std::uint32_t kind = 0;
    if (!decode(address, index, kind)) {
        const auto found = spare_.find(address);
        return found == spare_.end() ? 0 : found->second;
    }
    const TimerState& timer = timers_[index];
    switch (kind) {
    case 0:
        return timer.count & count_mask;
    case 1:
        return timer.mode & mode_read_mask;
    case 2:
        return timer.compare & compare_mask;
    default:
        return timer.hold & hold_mask;
    }
}

TimerUnit::TimerAdvance TimerUnit::add_ticks(std::uint32_t index,
                                             std::uint32_t ticks) {
    TimerAdvance advanced;
    if (index >= timer_count) {
        throw std::runtime_error("Timer index out of range for add_ticks");
    }
    TimerState& timer = timers_[index];
    if ((timer.mode & mode_count_enable) == 0) {
        return advanced;  // not counting
    }
    if ((timer.mode & mode_gate_enable) != 0) {
        return advanced;  // gated timers hold: explicit limit, decision 0030
    }
    std::uint32_t remaining = ticks;
    while (remaining > 0) {
        // Land exactly on the next overflow, the next compare passage, or
        // the end of the advance, whichever comes first. A compare at or
        // behind the counter resolves to a full cycle away, so it waits for
        // the next wrap instead of firing immediately.
        const std::uint32_t distance_overflow = counter_modulo - timer.count;
        std::uint32_t distance_compare;
        if (timer.compare > timer.count) {
            distance_compare = timer.compare - timer.count;
        } else {
            distance_compare = counter_modulo - timer.count + timer.compare;
        }
        std::uint32_t chunk = remaining;
        if (chunk > distance_overflow) {
            chunk = distance_overflow;
        }
        if (chunk > distance_compare) {
            chunk = distance_compare;
        }
        timer.count += chunk;
        remaining -= chunk;
        if (timer.count == counter_modulo) {
            timer.count = 0;
            // The flag is set only when its interrupt is enabled
            // (PCSX2 Counters.h @81526d4); the wrap itself always happens.
            if ((timer.mode & mode_overflow_enable) != 0
                && (timer.mode & mode_overflow_flag) == 0) {
                timer.mode |= mode_overflow_flag;
                advanced.overflow_edge = true;
            }
        }
        if (timer.count == timer.compare) {
            if ((timer.mode & mode_compare_enable) != 0) {
                if ((timer.mode & mode_compare_flag) == 0) {
                    timer.mode |= mode_compare_flag;
                    advanced.compare_edge = true;
                }
                // ZeroReturn resets only with the compare interrupt
                // enabled (PCSX2 note, tested on hardware).
                if ((timer.mode & mode_zero_return) != 0) {
                    timer.count = 0;
                }
            }
        }
    }
    return advanced;
}

std::vector<std::pair<std::uint32_t, std::uint32_t>>
TimerUnit::registers_snapshot() const {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> out;
    out.reserve(timer_count * 4);
    for (std::uint32_t index = 0; index < timer_count; ++index) {
        const std::uint32_t base = window_base + index * timer_stride;
        out.emplace_back(base + count_offset, timers_[index].count & count_mask);
        out.emplace_back(base + mode_offset, timers_[index].mode & mode_read_mask);
        out.emplace_back(base + compare_offset,
                         timers_[index].compare & compare_mask);
        out.emplace_back(base + hold_offset, timers_[index].hold & hold_mask);
    }
    for (const auto& entry : spare_) {
        out.emplace_back(entry);
    }
    return out;
}

void TimerUnit::restore_registers(
    std::span<const std::pair<std::uint32_t, std::uint32_t>> entries) {
    // Straight into storage, never through write_register: a restore must
    // not run the guest acknowledge path and must not queue anything. The
    // values are masked to the logical widths on the way in.
    for (TimerState& timer : timers_) {
        timer = TimerState{};
    }
    spare_.clear();
    for (const auto& [address, value] : entries) {
        std::uint32_t index = 0;
        std::uint32_t kind = 0;
        if (!decode(address, index, kind)) {
            spare_[address] = value;
            continue;
        }
        TimerState& timer = timers_[index];
        switch (kind) {
        case 0:
            timer.count = value & count_mask;
            break;
        case 1:
            timer.mode = value & mode_read_mask;
            break;
        case 2:
            timer.compare = value & compare_mask;
            break;
        default:
            timer.hold = value & hold_mask;
            break;
        }
    }
}

} // namespace gt4recomp::ee
