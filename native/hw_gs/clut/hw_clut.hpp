// HWR CLUT unit: 1 KiB palette buffer, CLD load rules, CSM1/CSM2 layouts,
// palette decode and TEXA expansion (chapters 01.5, 01.6, 07.1 to 07.5).
// Copyright 2026 GT4Recomp contributors. Licensed under the MIT License (see LICENSE).
// Written from the TASK-026 specification (docs/hw-renderer-spec) (clean room).
//
// The pixel location functions here cover the three CLUT source formats only
// (CT32, CT16, CT16S). The memory model (M2) owns the general swizzle library;
// when the two are merged both must agree (their tests cover the same tables).
#pragma once

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <vector>
#include <functional>

#include "hw_gs_counters.hpp"
#include "hw_gs_types.hpp"

namespace hwgs {
namespace clut {

// Byte location of one pixel in local memory (chapter 01.3).
struct PixelLocation {
    uint32_t block = 0;          // 0..16383
    uint32_t byteInBlock = 0;    // 0..255
    uint32_t PhysicalByte() const { return block * kBlockBytes + byteInBlock; }
};

// Location of pixel (x, y) of a CT32, CT16 or CT16S buffer. Returns false for
// other formats.
bool LocatePixel(uint32_t format, uint32_t bufferPointer, uint32_t bufferWidth,
                 uint32_t x, uint32_t y, PixelLocation& out);

// Position of palette entry index inside the CSM1 image (chapter 07.4).
// eightBitIndex selects the 16x16 image (256 entries); otherwise 8x2 (16 entries).
void CsmEntryPosition(uint32_t index, bool eightBitIndex, uint32_t& x, uint32_t& y);

// TEXA fields (chapter 01.5).
struct TexaState {
    uint32_t ta0 = 0;
    bool aem = false;
    uint32_t ta1 = 0;
    static TexaState FromRegister(uint64_t texa);
};

// Expands one 16-bit value to RGBA8 (packed R | G<<8 | B<<16 | A<<24).
// wholeValueRule selects the chapter 07.5 AEM test (the whole 16-bit value is
// zero) instead of the chapter 01.5 test (R = G = B = 0).
uint32_t ExpandRgb16(uint16_t value, const TexaState& texa, bool wholeValueRule);

// 24-bit texel alpha: TA0, or 0 when AEM and RGB are zero (chapter 01.5).
uint32_t ExpandRgb24(uint32_t value, const TexaState& texa);

// Trigger description for one TEX0 or TEX2 write (chapter 07.2).
struct Tex0Write {
    uint32_t psm = 0;
    uint32_t cbp = 0;
    uint32_t cpsm = 0;
    uint32_t csm = 0;
    uint32_t csa = 0;
    uint32_t cld = 0;
    uint32_t texclutCbw = 0;     // TEXCLUT.CBW (CSM2 row width)
    uint32_t texclutCou = 0;     // TEXCLUT.COU (16-pixel units)
    uint32_t texclutCov = 0;     // TEXCLUT.COV
};

struct ClutDecision {
    bool load = false;
    bool setCbp0 = false;
    bool setCbp1 = false;
    uint32_t newCbp = 0;
};

// Decoded palette for a draw (chapter 07.5).
struct Palette {
    uint32_t entries[256] = {};  // RGBA8 packed; count valid entries
    uint32_t count = 0;          // 16 or 256
    uint8_t minAlpha = 255;
    uint8_t maxAlpha = 0;
};

class ClutUnit {
public:
    explicit ClutUnit(CounterSet& counters);

    // Pure decision for a write (chapter 07.2 table).
    ClutDecision Decide(const Tex0Write& write) const;

    // Applies a TEX0/TEX2 write: CLD side effects and, when a load happens, the
    // redundancy filter and the copy. flushBeforeLoad runs before a real copy
    // (chapter 07.3 step 1). Returns true when the buffer was rewritten.
    bool ProcessWrite(const Tex0Write& write, const uint8_t* localMemory,
                      const std::function<void()>& flushBeforeLoad);

    // Marks loaded data stale when memory it read was written (chapter 07.2).
    void NoteBlocksWritten(uint32_t blockBegin, uint32_t blockCount);
    // IN-353: with false, a performed load still takes every decision and records its source blocks but does not
    // copy the entries (the GPU CLUT is authoritative, IN-349); the CPU buffer is then not meaningful until
    // SetHalfwords.
    void SetCopiesData(bool copies) { copiesData_ = copies; }
    bool CopiesData() const { return copiesData_; }
    void SetHalfwords(const uint16_t* halfwords);

    // Decodes the palette a draw sees (chapter 07.5). psm is the draw's format.
    Palette Decode(uint32_t psm, uint32_t cpsm, uint32_t csa, uint64_t texa) const;

    // Incremented whenever the buffer contents change (batch snapshots use it).
    uint32_t Generation() const { return generation_; }
    uint32_t Cbp0() const { return cbp0_; }
    uint32_t Cbp1() const { return cbp1_; }
    const uint16_t* Halfwords() const { return halfwords_; }

    // A/B switch GT4_HW_CLUT_ALWAYS_LOAD=1 disables the redundancy filter.
    void SetAlwaysLoad(bool alwaysLoad) { alwaysLoad_ = alwaysLoad; }

    void Reset();

private:
    void Copy(const Tex0Write& write, const uint8_t* localMemory);
    void ApplySideEffects(const ClutDecision& decision);

    CounterSet& counters_;
    uint16_t halfwords_[512] = {};
    uint32_t cbp0_ = 0;
    uint32_t cbp1_ = 0;
    uint32_t generation_ = 0;
    bool alwaysLoad_ = false;

    // Redundancy filter state (chapter 07.2).
    bool haveLast_ = false;
    bool stale_ = true;
    Tex0Write last_{};
    std::bitset<kBlocksTotal> sourceBlocks_;
    uint32_t sourceFirst_ = kBlocksTotal;  // lowest and highest block set in sourceBlocks_ (IN-353 early-out)
    uint32_t sourceLast_ = 0;
    bool copiesData_ = true;
    // IN-360: source blocks of recent copy-free loads, keyed by everything that places the entries (GT4 reloads
    // the same few palettes about a thousand times per frame).
    struct SourceMemo {
        uint64_t key = ~0ull;
        std::vector<uint32_t> blocks;
        uint32_t first = 0, last = 0;
        uint64_t lastUse = 0;
    };
    std::array<SourceMemo, 16> sourceMemo_{};
    // IN-484: GT4 makes 1500 CLUT loads per VBlank from a few places. Clearing the 2 KiB of sourceBlocks_ each time was the
    // largest part of them on a small cache: only the blocks set are cleared, and a load from the same place changes nothing.
    std::vector<uint32_t> setBlocks_;
    const SourceMemo* activeMemo_ = nullptr;
    void ClearSourceBlocks();
    uint64_t memoUses_ = 0;
};

}  // namespace clut
}  // namespace hwgs
