// MIT License. Copyright (c) 2026 João Alano and GT4Recomp contributors.
// Written from docs/hw-renderer-spec/CORE2-SPEC.md and CORE2-DESIGN.md (TASK-027 core rewrite), clean room
// (decision 0070).

#include "core2_upload.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

namespace hwgs::core2 {
namespace {

using memory::BufferView;

// IN-414: per PSM code, the offset inside its page of the byte holding each pixel's lowest bit (bits 0-15) and the bit
// shift of a 4-bit pixel in that byte (bits 16-18), with the Z block swap folded in. It holds for a buffer whose base pointer is page
// aligned: the page part of the block number and the in-page part then never carry into each other.
struct InPageOffsets {
    std::array<std::vector<std::uint32_t>, 64> perPsm;
};

const InPageOffsets &InPageOffsetTables() {
    static const InPageOffsets offsets = [] {
        InPageOffsets built;
        const memory::GpuAddressTables tables = memory::gpuAddressTables();
        for (std::uint32_t code = 0; code < 64u; ++code) {
            const std::uint32_t tableOffset = tables.directory[code * 4u];
            const std::uint32_t pageWidth = tables.directory[code * 4u + 1u];
            const std::uint32_t pageHeight = tables.directory[code * 4u + 2u];
            const bool zXor = tables.directory[code * 4u + 3u] != 0;
            if (pageWidth == 0 || pageHeight == 0) continue;
            std::vector<std::uint32_t> &out = built.perPsm[code];
            out.resize(static_cast<std::size_t>(pageWidth) * pageHeight);
            for (std::uint32_t i = 0; i < out.size(); ++i) {
                const std::uint32_t entry = tables.entries[tableOffset + i];
                std::uint32_t block = entry & 31u;
                if (zXor) block ^= 24u;
                // The byte holding the pixel's lowest bit: T8H, T4HL and T4HH sit in the top byte of a 32-bit word
                // (shift 24 or 28 from the word's first byte).
                const std::uint32_t shift = (entry >> 13) & 31u;
                const std::uint32_t byteInPage = block * memory::kBlockBytes + ((entry >> 5) & 255u) + shift / 8u;
                out[i] = byteInPage | ((shift % 8u) << 16);
            }
        }
        return built;
    }();
    return offsets;
}

// IN-414: `count` consecutive host pixels (from pixel `index` of `data`) stored at their in-page offsets in one page.
template <std::uint32_t Bpp>
void StoreRun(std::uint8_t *page, const std::uint32_t *offsets, const std::uint8_t *data, std::uint64_t index,
              std::uint32_t count) {
    for (std::uint32_t i = 0; i < count; ++i, ++index) {
        std::uint8_t *out = page + (offsets[i] & 0xFFFFu);
        if constexpr (Bpp == 32) {
            std::memcpy(out, data + index * 4u, 4);
        } else if constexpr (Bpp == 24) {
            std::memcpy(out, data + index * 3u, 3);
        } else if constexpr (Bpp == 16) {
            std::memcpy(out, data + index * 2u, 2);
        } else if constexpr (Bpp == 8) {
            *out = data[index];
        } else {  // 4 bits, low nibble first in the host stream
            const std::uint32_t shift = offsets[i] >> 16;
            const std::uint8_t nibble =
                (index & 1u) ? static_cast<std::uint8_t>(data[index / 2] >> 4) : (data[index / 2] & 0x0Fu);
            *out = static_cast<std::uint8_t>((*out & ~(0x0Fu << shift)) | (nibble << shift));
        }
    }
}

template <std::uint32_t Bpp>
void HostToLocalPageAligned(std::uint8_t *memoryBytes, const BufferView &view, std::uint32_t x0, std::uint32_t y0,
                            std::uint32_t width, std::uint32_t height, const std::uint8_t *data,
                            const std::uint32_t *pageOffsets, std::uint32_t pageWidth, std::uint32_t pageHeight) {
    const std::uint32_t perRow = std::max<std::uint32_t>(1u, view.bufferWidth * 64u / pageWidth);
    std::uint64_t index = 0;
    for (std::uint32_t row = 0; row < height; ++row) {
        const std::uint32_t y = (y0 + row) % memory::kTransferCoordinateWrap;
        const std::uint32_t pageRow = (y / pageHeight) * perRow;
        const std::uint32_t *rowOffsets = pageOffsets + (y % pageHeight) * pageWidth;
        std::uint32_t column = 0;
        while (column < width) {
            // One run per page crossed (and at the transfer coordinate wrap): divisions once per run, not per pixel.
            const std::uint32_t x = (x0 + column) % memory::kTransferCoordinateWrap;
            const std::uint32_t xInPage = x % pageWidth;
            const std::uint32_t run = std::min({width - column, pageWidth - xInPage, memory::kTransferCoordinateWrap - x});
            const std::uint32_t pageBlock = view.basePointer + memory::kBlocksPerPage * (pageRow + x / pageWidth);
            std::uint8_t *page = memoryBytes + ((pageBlock * memory::kBlockBytes) & (memory::kLocalMemoryBytes - 1u));
            StoreRun<Bpp>(page, rowOffsets + xInPage, data, index, run);
            column += run;
            index += run;
        }
    }
}

}  // namespace

bool FastHostToLocal(std::uint8_t *memoryBytes, const BufferView &view, std::uint32_t x0, std::uint32_t y0,
                     std::uint32_t width, std::uint32_t height, const std::uint8_t *data, std::size_t byteCount,
                     UploadSwizzle swizzle) {
    static const memory::GpuAddressTables tables = memory::gpuAddressTables();
    const memory::PsmInfo &info = memory::psmInfo(view.psm);
    const std::uint32_t bpp = info.transferBpp;
    if (static_cast<std::uint64_t>(width) * height * bpp > static_cast<std::uint64_t>(byteCount) * 8u) return false;
    const std::uint32_t code = static_cast<std::uint32_t>(view.psm) & 63u;
    const std::uint32_t tableOffset = tables.directory[code * 4u];
    const std::uint32_t pageWidth = tables.directory[code * 4u + 1u];
    const std::uint32_t pageHeight = tables.directory[code * 4u + 2u];
    const bool zXor = tables.directory[code * 4u + 3u] != 0;
    if (pageWidth == 0 || pageHeight == 0) return false;
    if (swizzle == UploadSwizzle::PageRuns && view.basePointer % memory::kBlocksPerPage == 0) {
        const std::uint32_t *pageOffsets = InPageOffsetTables().perPsm[code].data();
        switch (bpp) {
            case 32: HostToLocalPageAligned<32>(memoryBytes, view, x0, y0, width, height, data, pageOffsets, pageWidth, pageHeight); break;
            case 24: HostToLocalPageAligned<24>(memoryBytes, view, x0, y0, width, height, data, pageOffsets, pageWidth, pageHeight); break;
            case 16: HostToLocalPageAligned<16>(memoryBytes, view, x0, y0, width, height, data, pageOffsets, pageWidth, pageHeight); break;
            case 8: HostToLocalPageAligned<8>(memoryBytes, view, x0, y0, width, height, data, pageOffsets, pageWidth, pageHeight); break;
            default: HostToLocalPageAligned<4>(memoryBytes, view, x0, y0, width, height, data, pageOffsets, pageWidth, pageHeight); break;
        }
        return true;
    }
    const std::uint32_t perRow = std::max<std::uint32_t>(1u, view.bufferWidth * 64u / pageWidth);
    const std::uint32_t *entries = tables.entries.data() + tableOffset;
    std::uint64_t index = 0;
    for (std::uint32_t row = 0; row < height; ++row) {
        const std::uint32_t y = (y0 + row) % memory::kTransferCoordinateWrap;
        const std::uint32_t pageRow = (y / pageHeight) * perRow;
        const std::uint32_t *rowEntries = entries + (y % pageHeight) * pageWidth;
        for (std::uint32_t column = 0; column < width; ++column, ++index) {
            const std::uint32_t x = (x0 + column) % memory::kTransferCoordinateWrap;
            const std::uint32_t entry = rowEntries[x % pageWidth];
            std::uint32_t block = view.basePointer + memory::kBlocksPerPage * (pageRow + x / pageWidth) + (entry & 31u);
            if (zXor) block ^= 24u;
            block &= memory::kBlocksTotal - 1u;
            // The byte holding the pixel's lowest bit (T8H, T4HL, T4HH: the word's top byte) and the shift in it.
            const std::uint32_t wordShift = (entry >> 13) & 31u;
            const std::uint32_t at =
                (block * memory::kBlockBytes + ((entry >> 5) & 255u) + wordShift / 8u) & (memory::kLocalMemoryBytes - 1u);
            const std::uint32_t shift = wordShift % 8u;
            std::uint8_t *out = memoryBytes + at;
            switch (bpp) {
                case 32: std::memcpy(out, data + index * 4u, 4); break;
                case 24: std::memcpy(out, data + index * 3u, 3); break;
                case 16: std::memcpy(out, data + index * 2u, 2); break;
                case 8: *out = data[index]; break;
                default: {  // 4 bits, low nibble first in the host stream
                    const std::uint8_t nibble =
                        (index & 1u) ? static_cast<std::uint8_t>(data[index / 2] >> 4) : (data[index / 2] & 0x0Fu);
                    *out = static_cast<std::uint8_t>((*out & ~(0x0Fu << shift)) | (nibble << shift));
                    break;
                }
            }
        }
    }
    return true;
}

}  // namespace hwgs::core2
