#pragma once

// Explicit guest state for the R5900: a 32-entry 64-bit register file with the
// CPU's 32-bit sign-extension rule, and a byte-addressable little-endian
// memory region. Nothing here executes guest code; the state model defines
// what a value means and what counts as an invalid access.

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace gt4recomp::ee {

// One RAM region's bytes with its base, as regions_snapshot reports them.
// MMIO windows are not regions: device registers belong to the devices
// and are snapshotted with them (a later slice), never here.
struct MemoryRegion {
    std::uint32_t base = 0;
    std::vector<std::uint8_t> bytes;
};

// One contiguous region of the guest address space. Loads and stores require
// natural alignment for their width and must lie entirely inside the region;
// anything else throws std::runtime_error naming the address instead of
// silently wrapping or touching host memory.
class GuestMemory {public:
    // size_bytes must be nonzero, and base + size_bytes must fit the 32-bit
    // guest address space.
    GuestMemory(std::uint32_t base, std::size_t size_bytes);

    // Adds a second byte-addressable RAM region (for example the EE's 16 KiB
    // scratchpad at 0x70000000), independent of the main region. Regions
    // must not overlap each other or an MMIO window; the main region is
    // regions_.front() and keeps base()/size().
    void map_region(std::uint32_t base, std::size_t size_bytes);

    [[nodiscard]] std::uint32_t base() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool contains(std::uint32_t address, std::size_t width) const noexcept;

    // Segment aliasing: the EE maps KSEG0 (0x80000000) and KSEG1
    // (0xA0000000) to the low 512 MiB of physical space, and the uncached
    // KUSEG mirror at 0x20000000 does the same for user addresses. The
    // kernel's syscall-table search and the SIF code read and write through
    // those aliases. When enabled, addresses in [0x80000000, 0xC0000000) and
    // [0x20000000, 0x40000000) access the same bytes as their physical
    // address (address & 0x1FFFFFFF), bounded by the region. The default is
    // strict: nothing outside the region is mapped unless the caller asks.
    void enable_segment_alias() noexcept;
    [[nodiscard]] bool segment_alias_enabled() const noexcept;

    // Memory-mapped I/O: accesses inside a mapped window are routed to the
    // window's callbacks instead of the byte array. The model separates one
    // window per device (the EE's hardware register blocks); the callbacks
    // must handle every access width the guest uses and live at least as
    // long as the memory. Bytes outside every window stay strictly bounded
    // RAM. Overlapping windows are a caller bug.
    using MmioRead = std::function<std::uint32_t(std::uint32_t address, std::size_t width)>;
    using MmioWrite = std::function<void(std::uint32_t address, std::size_t width,
                                         std::uint32_t value)>;
    void map_mmio(std::uint32_t base, std::uint32_t size, MmioRead read, MmioWrite write);
    [[nodiscard]] bool is_mmio(std::uint32_t address, std::size_t width) const noexcept;

    [[nodiscard]] std::uint8_t read_byte(std::uint32_t address) const;
    [[nodiscard]] std::uint16_t read_halfword(std::uint32_t address) const;
    [[nodiscard]] std::uint32_t read_word(std::uint32_t address) const;
    [[nodiscard]] std::uint64_t read_doubleword(std::uint32_t address) const;
    void write_byte(std::uint32_t address, std::uint8_t value);
    void write_halfword(std::uint32_t address, std::uint16_t value);
    void write_word(std::uint32_t address, std::uint32_t value);
    void write_doubleword(std::uint32_t address, std::uint64_t value);

    // Bulk copy for loading images and test fixtures. Byte granularity: no
    // alignment requirement beyond the region bounds. An empty source is a no-op.
    void write_bytes(std::uint32_t address, std::span<const std::uint8_t> source);

    // A copy of every RAM region (base plus bytes) in map order, for
    // snapshots. MMIO windows are skipped: their contents are captured
    // with the devices, not with RAM.
    [[nodiscard]] std::vector<MemoryRegion> regions_snapshot() const;

private:
    struct MmioWindow {
        std::uint32_t base = 0;
        std::uint32_t size = 0;
        MmioRead read;
        MmioWrite write;
    };

    struct Region {
        std::uint32_t base = 0;
        std::vector<std::uint8_t> bytes;
    };

    void require_alignment(std::uint32_t address, std::size_t width) const;
    // The region containing the whole access, or null. The address is
    // already physical.
    [[nodiscard]] const Region* find_region(std::uint32_t physical,
                                            std::size_t width) const noexcept;
    [[nodiscard]] Region* find_region(std::uint32_t physical,
                                      std::size_t width) noexcept;
    // The region containing the access, or a bounded failure.
    [[nodiscard]] const Region& require_region(std::uint32_t address,
                                               std::size_t width) const;
    [[nodiscard]] Region& require_region(std::uint32_t address, std::size_t width);
    [[nodiscard]] std::uint32_t physical_address(std::uint32_t address) const noexcept;
    [[nodiscard]] const MmioWindow* find_mmio(std::uint32_t address,
                                              std::size_t width) const noexcept;

    std::vector<Region> regions_;
    bool segment_alias_ = false;
    std::vector<MmioWindow> mmio_windows_;
};

// The whole per-thread register state a context switch must carry: the
// register files, the pc and the coprocessor state, but not memory, which the
// threads share. The kernel saves and restores one of these per thread.
struct RegisterContext {
    std::array<std::uint64_t, 32> gpr{};
    std::array<std::uint64_t, 32> gpr_high{};
    std::array<std::uint32_t, 32> fpr{};
    std::uint64_t hi = 0;
    std::uint64_t lo = 0;
    std::uint64_t hi1 = 0;
    std::uint64_t lo1 = 0;
    std::uint32_t fpu_accumulator = 0;
    std::uint32_t fpu_control = 0;
    std::uint32_t shift_amount_cache = 0;
    std::array<std::uint32_t, 32> cp0{};
    std::array<std::array<std::uint32_t, 4>, 32> vu0_vf{};
    std::array<std::uint32_t, 32> vu0_vi{};
    std::uint32_t vu0_clip_flag = 0;
    std::array<std::uint32_t, 4> vu0_acc{};
    std::uint32_t vu0_mac_flag = 0;
    std::uint32_t vu0_status_flag = 0;
    std::uint32_t pc = 0;
};

// The register file and program counter. R0 reads as zero and ignores writes;
// 32-bit writes sign-extend into the 64-bit register, matching the CPU's rule
// for all 32-bit results.
class GuestState {
public:
    explicit GuestState(GuestMemory memory);

    // The register state as one value; restore_registers puts one back.
    // Together they are the thread-switch primitive the kernel uses.
    [[nodiscard]] RegisterContext save_registers() const noexcept;
    void restore_registers(const RegisterContext& context) noexcept;

    [[nodiscard]] std::uint64_t read_gpr64(std::uint8_t index) const;
    void write_gpr64(std::uint8_t index, std::uint64_t value);
    [[nodiscard]] std::uint32_t read_gpr32(std::uint8_t index) const;
    void write_gpr32(std::uint8_t index, std::uint32_t value);

    // LWR with a non-aligned address replaces only the low 32 bits and keeps
    // the upper half, which neither 32-bit nor 64-bit writes express; that
    // rule is why this accessor exists.
    void write_gpr_low32(std::uint8_t index, std::uint32_t value);

    // The R5900 register file is 128 bits wide; the accesses above view the
    // low half. MMI instructions read and write all four 32-bit lanes, so the
    // upper half is addressable separately. The alias register r0 is zero for
    // both halves and ignores writes.
    [[nodiscard]] std::uint64_t read_gpr_high64(std::uint8_t index) const;
    void write_gpr_high64(std::uint8_t index, std::uint64_t value);

    [[nodiscard]] std::uint32_t pc() const noexcept;
    void set_pc(std::uint32_t value) noexcept;

    [[nodiscard]] GuestMemory& memory() noexcept;
    [[nodiscard]] const GuestMemory& memory() const noexcept;

    // FPU registers keep single-precision values as their 32-bit bit patterns;
    // interpreting those patterns as floats happens in the interpreter.
    [[nodiscard]] std::uint32_t read_fpr(std::uint8_t index) const;
    void write_fpr(std::uint8_t index, std::uint32_t value);

    // The FPU accumulator is written by the ADDA/SUBA/MULA forms and read by
    // MADD/MSUB. FCR31 holds the compare condition (bit 23) plus cause bits.
    [[nodiscard]] std::uint32_t fpu_accumulator() const noexcept;
    void set_fpu_accumulator(std::uint32_t value) noexcept;
    [[nodiscard]] std::uint32_t fpu_control() const noexcept;
    void set_fpu_control(std::uint32_t value) noexcept;

    // HI/LO come in two 64-bit halves each: the "1" halves serve the MMI
    // variants (MFHI1/MTHI1/...) and the 128-bit shift staging.
    [[nodiscard]] std::uint64_t hi() const noexcept;
    void set_hi(std::uint64_t value) noexcept;
    [[nodiscard]] std::uint64_t lo() const noexcept;
    void set_lo(std::uint64_t value) noexcept;
    [[nodiscard]] std::uint64_t hi1() const noexcept;
    void set_hi1(std::uint64_t value) noexcept;
    [[nodiscard]] std::uint64_t lo1() const noexcept;
    void set_lo1(std::uint64_t value) noexcept;

    // Shift-amount cache written by MTSA/MTSAB/MTSAH and read by the shift
    // instructions that take their amount from state instead of the encoding.
    [[nodiscard]] std::uint32_t shift_amount_cache() const noexcept;
    void set_shift_amount_cache(std::uint32_t value) noexcept;

    // CP0: the system coprocessor's register file (Status, Cause, EPC and the
    // rest). The model starts from the live menu state the M14 observation
    // captured: Status reads 0x70030c11 (IE and EIE set, the interrupt mask
    // and CU2 usable) and everything else is zero, because the code we run
    // was captured from a running game.
    [[nodiscard]] std::uint32_t read_cp0(std::uint8_t index) const;
    void write_cp0(std::uint8_t index, std::uint32_t value);

    // VU0 macro-mode state: 32 vector registers of four 32-bit lanes, the
    // integer register file, and the clip flag the control moves reach. The
    // constant register 0 reads as (0, 0, 0, 1.0) and ignores writes, and the
    // integer register 0 is hardwired zero, like the hardware.
    [[nodiscard]] std::uint32_t read_vf_lane(std::uint8_t index, std::uint8_t lane) const;
    void write_vf_lane(std::uint8_t index, std::uint8_t lane, std::uint32_t value);
    [[nodiscard]] std::uint32_t read_vi(std::uint8_t index) const;
    void write_vi(std::uint8_t index, std::uint32_t value);
    [[nodiscard]] std::uint32_t vu0_clip_flag() const noexcept;
    void set_vu0_clip_flag(std::uint32_t value) noexcept;
    // The accumulator and the two flag registers the macro arithmetic writes
    // and the control moves read back through the integer file.
    [[nodiscard]] std::uint32_t read_acc_lane(std::uint8_t lane) const;
    void write_acc_lane(std::uint8_t lane, std::uint32_t value);
    [[nodiscard]] std::uint32_t vu0_mac_flag() const noexcept;
    void set_vu0_mac_flag(std::uint32_t value) noexcept;
    [[nodiscard]] std::uint32_t vu0_status_flag() const noexcept;
    void set_vu0_status_flag(std::uint32_t value) noexcept;
    // The FBRST reset bit clears the whole VU0 register file.
    void reset_vu0_registers() noexcept;

private:
    static void require_gpr_index(std::uint8_t index);
    static void require_fpr_index(std::uint8_t index);

    std::array<std::uint64_t, 32> gpr_{};
    std::array<std::uint64_t, 32> gpr_high_{};
    std::array<std::uint32_t, 32> fpr_{};
    std::uint64_t hi_ = 0;
    std::uint64_t lo_ = 0;
    std::uint64_t hi1_ = 0;
    std::uint64_t lo1_ = 0;
    std::uint32_t fpu_accumulator_ = 0;
    std::uint32_t fpu_control_ = 0;
    std::uint32_t shift_amount_cache_ = 0;
    std::array<std::uint32_t, 32> cp0_{};
    std::array<std::array<std::uint32_t, 4>, 32> vu0_vf_{};
    std::array<std::uint32_t, 32> vu0_vi_{};
    std::uint32_t vu0_clip_flag_ = 0;
    std::array<std::uint32_t, 4> vu0_acc_{};
    std::uint32_t vu0_mac_flag_ = 0;
    std::uint32_t vu0_status_flag_ = 0;
    std::uint32_t pc_ = 0;
    GuestMemory memory_;
};

} // namespace gt4recomp::ee
