// MIT License. Copyright (c) 2026 João Alano and GT4Recomp contributors.
// Written from docs/hw-renderer-spec/CORE2-SPEC.md and CORE2-DESIGN.md (TASK-027 core rewrite), clean room
// (decision 0070).
//
// HOST->LOCAL uploads into the CPU copy of GS memory through precomputed in-page tables (memory::gpuAddressTables,
// the same numbers as memory::pixelSlot and memory::hostToLocal).

#pragma once

#include <cstddef>
#include <cstdint>

#include "hw_memory.hpp"

namespace hwgs::core2 {

enum class UploadSwizzle : std::uint8_t {
    PerPixel,  // every pixel addressed on its own (three divisions and a format switch per pixel)
    PageRuns,  // IN-414: page-aligned buffers in runs of one page row, one table read per pixel
};

// Writes `width` x `height` host pixels (chapter 02.7 layout) at (x0, y0) of `view` into the 4 MiB image
// `memoryBytes`, wrapping transfer coordinates at 2048. Returns false (nothing written) when `byteCount` does not
// cover the whole rectangle or the PSM has no table; the caller then takes the generic path. PageRuns falls back to
// PerPixel for a base pointer that is not page aligned. Both give the same bytes as memory::hostToLocal.
// IN-450: `changed`, when given, is set to false when the PageRuns path wrote only pixels equal to the ones already
// there (GT4 uploads most of its textures again every frame); the PerPixel path always reports a change.
bool FastHostToLocal(std::uint8_t *memoryBytes, const memory::BufferView &view, std::uint32_t x0, std::uint32_t y0,
                     std::uint32_t width, std::uint32_t height, const std::uint8_t *data, std::size_t byteCount,
                     UploadSwizzle swizzle, bool *changed = nullptr);

}  // namespace hwgs::core2
