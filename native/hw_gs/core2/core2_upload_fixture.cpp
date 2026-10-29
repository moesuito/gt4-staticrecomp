// MIT License. Copyright (c) 2026 João Alano and GT4Recomp contributors.
// Written from docs/hw-renderer-spec/CORE2-SPEC.md and CORE2-DESIGN.md (TASK-027 core rewrite), clean room
// (decision 0070).
//
// CTest hw_core2_upload_swizzle (IN-414): core 2's HOST->LOCAL fast path, in both modes (PerPixel and PageRuns),
// writes exactly the bytes of the generic memory::hostToLocal for every PSM: page-aligned and unaligned base
// pointers, several buffer widths, odd sizes, rectangles across pages and across the 2048 transfer wrap.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <vector>

#include "core2/core2_upload.hpp"
#include "hw_memory.hpp"

namespace {

using hwgs::core2::FastHostToLocal;
using hwgs::core2::UploadSwizzle;
using namespace hwgs::memory;

int failures = 0;
int cases = 0;

std::uint64_t NextRandom(std::uint64_t &state) {  // splitmix64
    std::uint64_t z = (state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

void CheckCase(std::uint64_t &state, Psm psm, std::uint32_t bp, std::uint32_t bw, std::uint32_t x, std::uint32_t y,
               std::uint32_t width, std::uint32_t height) {
    // The same random background in all three memories, so untouched bytes (and the other nibble of 4-bit pixels)
    // are compared too.
    LocalMemory reference;
    for (std::uint32_t i = 0; i < kLocalMemoryBytes; i += 8) {
        const std::uint64_t word = NextRandom(state);
        std::memcpy(reference.bytes().data() + i, &word, 8);
    }
    std::vector<std::uint8_t> data(hostBytesFor(psm, width, height));
    for (std::uint8_t &byte : data) byte = static_cast<std::uint8_t>(NextRandom(state));
    std::vector<std::uint8_t> perPixel(reference.bytes().begin(), reference.bytes().end());
    std::vector<std::uint8_t> pageRuns = perPixel;

    const BufferView view{bp, bw, psm};
    hostToLocal(reference, view, TransferRect{x, y, width, height}, std::span<const std::uint8_t>(data));
    const bool okPerPixel =
        FastHostToLocal(perPixel.data(), view, x, y, width, height, data.data(), data.size(), UploadSwizzle::PerPixel);
    const bool okPageRuns =
        FastHostToLocal(pageRuns.data(), view, x, y, width, height, data.data(), data.size(), UploadSwizzle::PageRuns);
    ++cases;
    const bool samePerPixel = okPerPixel && std::memcmp(perPixel.data(), reference.bytes().data(), kLocalMemoryBytes) == 0;
    const bool samePageRuns = okPageRuns && std::memcmp(pageRuns.data(), reference.bytes().data(), kLocalMemoryBytes) == 0;
    if (!samePerPixel || !samePageRuns) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s bp=%u bw=%u rect=%u,%u %ux%u per_pixel=%s page_runs=%s\n", psmInfo(psm).name, bp,
                     bw, x, y, width, height, samePerPixel ? "same" : "differs", samePageRuns ? "same" : "differs");
    }
}

}  // namespace

int main() {
    const Psm psms[] = {Psm::Ct32, Psm::Ct24, Psm::Ct16, Psm::Ct16S, Psm::T8,  Psm::T4,   Psm::T8H,
                        Psm::T4Hl, Psm::T4Hh, Psm::Z32,  Psm::Z24,   Psm::Z16, Psm::Z16S};
    std::uint64_t state = 0x1A414u;
    for (const Psm psm : psms) {
        // Fixed shapes: one page, a rectangle crossing pages at odd offsets, one row, one column, and the wrap.
        CheckCase(state, psm, 0, 1, 0, 0, psmInfo(psm).pageWidth, psmInfo(psm).pageHeight);
        CheckCase(state, psm, 64, 4, 13, 7, 211, 45);
        CheckCase(state, psm, 96, 10, 5, 3, 640, 1);
        CheckCase(state, psm, 32, 2, 77, 0, 1, 130);
        CheckCase(state, psm, 512, 2, 2040, 2041, 17, 13);  // both coordinates wrap at 2048
        CheckCase(state, psm, 7, 3, 9, 4, 50, 21);         // base pointer not page aligned: PerPixel fallback
        // Random shapes.
        for (int i = 0; i < 6; ++i) {
            const bool aligned = (NextRandom(state) & 3u) != 0;
            const std::uint32_t bp = static_cast<std::uint32_t>(NextRandom(state) % kBlocksTotal) & (aligned ? ~31u : ~0u);
            const std::uint32_t bw = 1u + static_cast<std::uint32_t>(NextRandom(state) % 16u);
            const std::uint32_t x = static_cast<std::uint32_t>(NextRandom(state) % 2048u);
            const std::uint32_t y = static_cast<std::uint32_t>(NextRandom(state) % 2048u);
            const std::uint32_t width = 1u + static_cast<std::uint32_t>(NextRandom(state) % 300u);
            const std::uint32_t height = 1u + static_cast<std::uint32_t>(NextRandom(state) % 80u);
            CheckCase(state, psm, bp, bw, x, y, width, height);
        }
    }
    // Too little data: nothing written, the caller takes the generic path.
    std::vector<std::uint8_t> memory(kLocalMemoryBytes, 0xA5);
    const std::uint8_t few[16] = {};
    if (FastHostToLocal(memory.data(), BufferView{0, 1, Psm::Ct32}, 0, 0, 8, 8, few, sizeof few, UploadSwizzle::PageRuns)) {
        ++failures;
        std::fprintf(stderr, "FAIL: a short upload was accepted\n");
    }
    std::printf("hw_core2_upload_swizzle: %d cases, %d failures\n", cases, failures);
    return failures == 0 ? 0 : 1;
}
