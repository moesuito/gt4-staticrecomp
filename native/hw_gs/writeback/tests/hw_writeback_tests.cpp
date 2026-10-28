// MIT License. Copyright (c) 2026 João Alano and GT4Recomp contributors.
// Written from docs/hw-renderer-spec (TASK-027 M4, chapters 01.4, 03.2, 03.3, 03.4, 03.5, 03.7, 03.8,
// 15), clean room (decision 0069). CTest entry points of the M4 write-back reference renderer.
//
// Usage: hw_writeback_tests <test name>. GPU tests need a Vulkan 1.2 device with graphics and compute
// queues; without one they exit 77 (CTest reports Skipped, never a pass).
// Equivalence tests compare the GPU result with the CPU reference (cpuref::DrawBatch) pixel by pixel.
// Edges are placed on pixel boundaries where the two rasterisers are specified to agree; the diagonal
// and Gouraud cases are reported, not asserted (IN-500).

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../../cpuref/cpuref_draw.hpp"
#include "../../cpuref/cpuref_memory.hpp"
#include "../../memory/hw_memory.hpp"
#include "../../clut/hw_clut.hpp"
#include "../../frontend/hw_gs_registers.hpp"
#include "../../pixel/hw_gs_pixel.hpp"
#include "../../frontend/hw_gs_types.hpp"
#include "../../hw_gs_vulkan_loader.hpp"
#include "../hw_gs_vulkan_device_table.hpp"
#include "../hw_gs_present_scheduler.hpp"
#include "../hw_gs_writeback.hpp"
#include "../hw_writeback_replay_target.hpp"
#include "../../cpuref/cpuref_renderer.hpp"
#include "../../cpuref/cpuref_replay.hpp"

namespace {

using namespace hwgs;

// ---- Headless device -----------------------------------------------------------------------------

struct Headless {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = 0;
    bool wantInterlock = true;
    bool interlock = false;

    bool Create(std::string& error) {
        if (!gt4::hw::vulkan_loader::load_loader(error)) return false;
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "hw_writeback_tests";
        app.apiVersion = VK_API_VERSION_1_2;
        VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        info.pApplicationInfo = &app;
        if (gt4_hw_vkCreateInstance(&info, nullptr, &instance) != VK_SUCCESS) {
            error = "vkCreateInstance failed";
            return false;
        }
        std::vector<std::string> missing;
        if (!gt4::hw::vulkan_loader::load_headless_entry_points(instance, missing)) {
            error = "instance entry points missing:";
            for (const std::string& name : missing) error += " " + name;
            return false;
        }
        uint32_t count = 0;
        gt4_hw_vkEnumeratePhysicalDevices(instance, &count, nullptr);
        std::vector<VkPhysicalDevice> devices(count);
        gt4_hw_vkEnumeratePhysicalDevices(instance, &count, devices.data());
        for (VkPhysicalDevice candidate : devices) {
            VkPhysicalDeviceProperties props{};
            gt4_hw_vkGetPhysicalDeviceProperties(candidate, &props);
            if (props.apiVersion < VK_API_VERSION_1_2) continue;
            uint32_t families = 0;
            gt4_hw_vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, nullptr);
            std::vector<VkQueueFamilyProperties> props_q(families);
            gt4_hw_vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, props_q.data());
            for (uint32_t f = 0; f < families; ++f) {
                const VkQueueFlags wanted = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
                if ((props_q[f].queueFlags & wanted) != wanted) continue;
                physical = candidate;
                family = f;
                break;
            }
            if (physical) break;
        }
        if (!physical) {
            error = "no Vulkan 1.2 device with graphics and compute";
            return false;
        }
        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue.queueFamilyIndex = family;
        queue.queueCount = 1;
        queue.pQueuePriorities = &priority;
        // IN-345: enable pixel interlock when the device has it (the batch-per-draw path), unless the
        // caller asked for the per-primitive path (A/B: --no-interlock / GT4_HW_TEST_NO_INTERLOCK=1).
        interlock = wantInterlock && gt4::hw::vulkan_loader::pixel_interlock_supported(physical);
        VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT interlockFeatures{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT};
        interlockFeatures.fragmentShaderPixelInterlock = VK_TRUE;
        const char* interlockExtension = VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME;
        VkDeviceCreateInfo dev{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dev.queueCreateInfoCount = 1;
        dev.pQueueCreateInfos = &queue;
        if (interlock) {
            dev.pNext = &interlockFeatures;
            dev.enabledExtensionCount = 1;
            dev.ppEnabledExtensionNames = &interlockExtension;
        }
        if (gt4_hw_vkCreateDevice(physical, &dev, nullptr, &device) != VK_SUCCESS) {
            error = "vkCreateDevice failed";
            return false;
        }
        gt4_hw_vkGetDeviceQueue(device, family, 0, &queue_handle());
        return gt4::hw::vulkan_device::load_device_entry_points(instance, missing);
    }

    VkQueue &queue_handle() { return queue; }
};

// ---- Batch construction --------------------------------------------------------------------------

struct Setup {
    uint32_t fbp = 0;     // FBP in pages
    uint32_t fbw = 10;    // FBW in 64-pixel units
    uint32_t fpsm = 0;    // CT32
    uint32_t fbmsk = 0;
    uint32_t zbp = 70;    // ZBP in pages
    uint32_t zpsm = 0;    // Z32 low bits (0x30 implied)
    bool zmsk = true;
    bool zte = false;
    uint32_t ztst = 1;    // ALWAYS
    bool ate = false;
    uint32_t atst = 1;    // ALWAYS
    uint32_t aref = 0x80;
    uint32_t afail = 0;   // KEEP
    int scax0 = 0, scax1 = 639, scay0 = 0, scay1 = 447;
    bool tme = false;
    bool fst = false;
    bool iip = true;      // Gouraud
    bool fge = false;
    uint32_t fogcol = 0;
    uint32_t tbp = 40;    // texture base in pages
    uint32_t tbw = 4;
    uint32_t tpsm = 0;
    uint32_t tw = 3, th = 3;
    uint32_t tfx = 0;
    bool tcc = false;
    bool mmag = false;
    uint32_t wms = 0, wmt = 0;
    uint32_t minu = 0, maxu = 0, minv = 0, maxv = 0;
    bool abe = false;        // PRIM.ABE: run the blend unit
    uint64_t alpha_reg = 0;  // ALPHA register (0x42), see pixel::encode_alpha_register
    bool pabe = false;       // skip the blend when source alpha MSB is 0
    bool colclamp = false;   // COLCLAMP: 0 wraps the low 8 bits
    bool fba = false;        // force the alpha byte to 0x80 on write
    bool dthe = false;       // dithering (16-bit frames)
    uint64_t dimx = 0;       // DIMX, the dither matrix
    bool date = false;       // DATE (destination alpha test)
    bool datm = false;       // DATE expected MSB
    PrimType type = PrimType::Triangle;
};

uint64_t Pack(uint64_t value, unsigned lo) { return value << lo; }

Batch MakeBatch(const Setup& s, const std::vector<Vertex>& vertices, const std::vector<PrimType>& prims) {
    Batch batch;
    batch.state.ctx.frame = Pack(s.fbp, 0) | Pack(s.fbw, 16) | Pack(s.fpsm, 24) | Pack(s.fbmsk, 32);
    batch.state.ctx.zbuf = Pack(s.zbp, 0) | Pack(s.zpsm, 24) | Pack(s.zmsk ? 1 : 0, 32);
    batch.state.ctx.test = Pack(s.ate ? 1 : 0, 0) | Pack(s.atst, 1) | Pack(s.aref, 4) | Pack(s.afail, 12) |
                           Pack(s.date ? 1 : 0, 14) | Pack(s.datm ? 1 : 0, 15) | Pack(s.zte ? 1 : 0, 16) |
                           Pack(s.ztst, 17);
    batch.state.ctx.alpha = s.alpha_reg;
    batch.state.ctx.fba = Pack(s.fba ? 1 : 0, 0);
    batch.state.env.pabe = s.pabe ? 1 : 0;
    batch.state.env.colclamp = s.colclamp ? 1 : 0;
    batch.state.env.dthe = s.dthe ? 1 : 0;
    batch.state.env.dimx = s.dimx;
    batch.state.ctx.scissor = Pack(static_cast<uint64_t>(s.scax0), 0) | Pack(static_cast<uint64_t>(s.scax1), 16) |
                              Pack(static_cast<uint64_t>(s.scay0), 32) | Pack(static_cast<uint64_t>(s.scay1), 48);
    batch.state.ctx.tex0 = Pack(s.tbp, 0) | Pack(s.tbw, 14) | Pack(s.tpsm, 20) | Pack(s.tw, 26) | Pack(s.th, 30) |
                           Pack(s.tcc ? 1 : 0, 34) | Pack(s.tfx, 35);
    batch.state.ctx.tex1 = Pack(s.mmag ? 1 : 0, 5);
    batch.state.ctx.clamp = Pack(s.wms, 0) | Pack(s.wmt, 2) | Pack(s.minu, 4) | Pack(s.maxu, 14) | Pack(s.minv, 24) |
                            Pack(s.maxv, 34);
    batch.state.env.fogcol = s.fogcol;
    uint64_t prim = static_cast<uint64_t>(s.type == PrimType::Triangle ? 3 : static_cast<uint8_t>(s.type));
    prim = prim | (s.iip ? 1ull << primbit::kIip : 0) | (s.tme ? 1ull << primbit::kTme : 0) |
           (s.fge ? 1ull << primbit::kFge : 0) | (s.abe ? 1ull << primbit::kAbe : 0) |
           (s.fst ? 1ull << primbit::kFst : 0);
    batch.state.effectivePrim = prim;
    batch.cls = ClassOfType(static_cast<uint32_t>(prim & 7u));
    batch.vertices = vertices;
    uint32_t first = 0;
    for (PrimType type : prims) {
        const uint32_t count = type == PrimType::Point ? 1 : (type == PrimType::Line || type == PrimType::LineStrip) ? 2
                             : type == PrimType::Sprite ? 2 : 3;
        PrimRecord record;
        record.type = type;
        record.first = first;
        record.count = count;
        int32_t x0 = 1 << 30, y0 = 1 << 30, x1 = -(1 << 30), y1 = -(1 << 30);
        for (uint32_t i = 0; i < count; ++i) {
            const Vertex& v = vertices[first + i];
            x0 = std::min(x0, v.x4 >> 4);
            y0 = std::min(y0, v.y4 >> 4);
            x1 = std::max(x1, v.x4 >> 4);
            y1 = std::max(y1, v.y4 >> 4);
        }
        record.rect = PixelRect{x0, y0, x1, y1};
        batch.prims.push_back(record);
        first += count;
    }
    return batch;
}

Vertex MakeVertex(int32_t x, int32_t y, uint8_t r, uint8_t g, uint8_t b, uint8_t a, float s = 0.0f, float t = 0.0f,
                  uint32_t u = 0, uint32_t v = 0, uint32_t z = 0, uint8_t fog = 255) {
    Vertex out;
    out.x4 = x * 16;
    out.y4 = y * 16;
    out.r = r;
    out.g = g;
    out.b = b;
    out.a = a;
    out.s = s;
    out.t = t;
    out.q = 1.0f;
    out.u = u;
    out.v = v;
    out.z = z;
    out.fog = fog;
    return out;
}

// ---- Equivalence harness -------------------------------------------------------------------------

// Deterministic fill of local memory: a texture pattern and a frame pattern in the given views.
void FillPattern(memory::LocalMemory& local, const Setup& s, uint32_t seed) {
    uint32_t state = seed * 2654435761u + 1u;
    auto next = [&] {
        state = state * 1664525u + 1013904223u;
        return state >> 8;
    };
    const auto tpsm = memory::decodePsm(s.tpsm);
    if (tpsm) {
        const memory::BufferView view{memory::blockOfPage(s.tbp), s.tbw, *tpsm};
        for (uint32_t y = 0; y < (1u << s.th); ++y) {
            for (uint32_t x = 0; x < (1u << s.tw); ++x) local.writePixel(view, x, y, next() | 0x40000000u);
        }
    }
    const auto fpsm = memory::decodePsm(s.fpsm);
    if (fpsm) {
        const memory::BufferView view{memory::blockOfPage(s.fbp), s.fbw, *fpsm};
        for (uint32_t y = 0; y < 448; ++y) {
            for (uint32_t x = 0; x < 640; ++x) local.writePixel(view, x, y, next());
        }
    }
    const auto zpsm = memory::decodePsm(0x30u | s.zpsm);
    if (zpsm) {
        const memory::BufferView view{memory::blockOfPage(s.zbp), s.fbw, *zpsm};
        for (uint32_t y = 0; y < 448; ++y) {
            for (uint32_t x = 0; x < 640; ++x) local.writePixel(view, x, y, next() & 0x00FFFFFFu);
        }
    }
}

struct CaseResult {
    uint64_t compared = 0;
    uint64_t mismatches = 0;
    uint64_t firstX = 0, firstY = 0;
    bool gpuError = false;
    std::string error;
};

// Runs the batch through the CPU reference and through the GPU path, then compares frame and depth words
// over the batch rectangle. Both start from identical memory.
CaseResult RunCase(gpu::GpuDrawer& drawer, const Setup& s, const Batch& batch, uint32_t seed) {
    CaseResult result;
    memory::LocalMemory cpuMemory;
    memory::LocalMemory gpuMemory;
    FillPattern(cpuMemory, s, seed);
    FillPattern(gpuMemory, s, seed);
    CounterSet counterSet;
    clut::ClutUnit clut(counterSet);

    cpuref::DrawStats stats;
    cpuref::DrawBatch(batch, cpuMemory, clut, stats);

    writeback::BatchTranslation translation;
    writeback::Counters counters;
    writeback::TranslateBatch(batch, gpuMemory, clut, translation, counters);
    if (translation.skippedTexture) {
        result.error = "texture skipped";
        return result;
    }
    if (translation.draws.empty()) {
        // Nothing drawn: the reference must not have drawn either.
        return result;
    }
    if (!writeback::ExecuteBatch(drawer, translation, gpuMemory, counters, result.error)) {
        result.gpuError = true;
        return result;
    }
    const memory::BufferView frameCpu{memory::blockOfPage(s.fbp), s.fbw, *memory::decodePsm(s.fpsm)};
    const memory::BufferView zCpu{memory::blockOfPage(s.zbp), s.fbw, *memory::decodePsm(0x30u | s.zpsm)};
    const PixelRect& r = translation.rect;
    for (int32_t y = r.y0; y <= r.y1; ++y) {
        for (int32_t x = r.x0; x <= r.x1; ++x) {
            const uint32_t fc = cpuMemory.readPixel(frameCpu, x, y);
            const uint32_t fg = gpuMemory.readPixel(frameCpu, x, y);
            const uint32_t zc = cpuMemory.readPixel(zCpu, x, y);
            const uint32_t zg = gpuMemory.readPixel(zCpu, x, y);
            result.compared++;
            if (fc != fg || zc != zg) {
                if (result.mismatches == 0) {
                    result.firstX = static_cast<uint64_t>(x);
                    result.firstY = static_cast<uint64_t>(y);
                }
                // Bounded value report: the first few differences name the stage that diverged.
                if (result.mismatches < 4) {
                    std::printf("  mismatch at %d,%d: frame cpu=%08X gpu=%08X | depth cpu=%08X gpu=%08X\n", x, y, fc,
                                fg, zc, zg);
                }
                result.mismatches++;
            }
        }
    }
    return result;
}

int Report(const char* name, const CaseResult& r) {
    std::printf("%s: compared %llu pixels, mismatches %llu", name, static_cast<unsigned long long>(r.compared),
                static_cast<unsigned long long>(r.mismatches));
    if (r.mismatches) {
        std::printf(" (first at %llu,%llu)", static_cast<unsigned long long>(r.firstX),
                    static_cast<unsigned long long>(r.firstY));
    }
    std::printf("\n");
    if (r.gpuError) std::printf("%s: GPU error: %s\n", name, r.error.c_str());
    return (r.gpuError || r.mismatches != 0) ? 1 : 0;
}

// Quad on pixel boundaries: two triangles over [x0, x1) x [y0, y1) with one flat colour.
std::vector<Vertex> FlatQuad(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint8_t r, uint8_t g, uint8_t b,
                             uint8_t a, float s0 = 0, float t0 = 0, float s1 = 0, float t1 = 0) {
    return {MakeVertex(x0, y0, r, g, b, a, s0, t0), MakeVertex(x1, y0, r, g, b, a, s1, t0),
            MakeVertex(x0, y1, r, g, b, a, s0, t1), MakeVertex(x1, y0, r, g, b, a, s1, t0),
            MakeVertex(x0, y1, r, g, b, a, s0, t1), MakeVertex(x1, y1, r, g, b, a, s1, t1)};
}

// ---- CPU translation tests (no GPU) --------------------------------------------------------------

int TestFlatProvoking() {
    Setup s;
    s.iip = false;  // flat: the last vertex supplies the colour (chapter 03.2)
    const std::vector<Vertex> v = {MakeVertex(0, 0, 10, 20, 30, 40), MakeVertex(16, 0, 50, 60, 70, 80),
                                   MakeVertex(0, 16, 90, 100, 110, 120)};
    Batch batch = MakeBatch(s, v, {PrimType::Triangle});
    writeback::BatchTranslation t;
    writeback::Counters counters;
    memory::LocalMemory local;
    CounterSet counterSet;
    clut::ClutUnit clut(counterSet);
    writeback::TranslateBatch(batch, local, clut, t, counters);
    if (t.vertices.size() != 3) return 1;
    for (const gpu::GpuVertex& g : t.vertices) {
        if (g.rgba[0] != 90 || g.rgba[1] != 100 || g.rgba[2] != 110 || g.rgba[3] != 120) {
            std::printf("hw_flat_provoking: triangle vertex not de-indexed to the last colour\n");
            return 1;
        }
    }
    // A line in flat mode takes the second vertex's colour.
    Setup line = s;
    line.type = PrimType::Line;
    Batch lineBatch = MakeBatch(line, {MakeVertex(0, 0, 1, 2, 3, 4), MakeVertex(8, 8, 5, 6, 7, 8)}, {PrimType::Line});
    writeback::BatchTranslation tl;
    writeback::TranslateBatch(lineBatch, local, clut, tl, counters);
    for (const gpu::GpuVertex& g : tl.vertices) {
        if (g.rgba[0] != 5 || g.rgba[3] != 8) {
            std::printf("hw_flat_provoking: line vertex not de-indexed\n");
            return 1;
        }
    }
    // Gouraud keeps every vertex colour.
    Setup smooth = s;
    smooth.iip = true;
    Batch smoothBatch = MakeBatch(smooth, v, {PrimType::Triangle});
    writeback::BatchTranslation ts;
    writeback::TranslateBatch(smoothBatch, local, clut, ts, counters);
    if (ts.vertices[0].rgba[0] != 10 || ts.vertices[2].rgba[0] != 90) {
        std::printf("hw_flat_provoking: smooth colours changed\n");
        return 1;
    }
    std::printf("hw_flat_provoking: triangle and line de-indexed to the last vertex, Gouraud unchanged\n");
    return 0;
}

int TestSpriteCorners() {
    Setup s;
    s.type = PrimType::Sprite;
    // First vertex at (2, 3) with S/T/U/V (4, 6)/(40, 60); second at (10, 9) with S/T (8, 12)/(80, 120).
    std::vector<Vertex> v = {MakeVertex(2, 3, 1, 1, 1, 1, 4, 6, 40, 60), MakeVertex(10, 9, 200, 201, 202, 203, 8, 12, 80, 120)};
    Batch batch = MakeBatch(s, v, {PrimType::Sprite});
    memory::LocalMemory local;
    CounterSet counterSet;
    clut::ClutUnit clut(counterSet);
    writeback::BatchTranslation t;
    writeback::Counters counters;
    writeback::TranslateBatch(batch, local, clut, t, counters);
    if (t.vertices.size() != 6 || counters.spritesExpanded != 1) {
        std::printf("hw_sprite_corners: expected 6 vertices, got %zu\n", t.vertices.size());
        return 1;
    }
    // Corner order of the two triangles: 0, 1, 2, 1, 2, 3 (chapter 03.2).
    const int32_t expectX[4] = {2, 10, 2, 10};
    const int32_t expectY[4] = {3, 3, 9, 9};
    const int order[6] = {0, 1, 2, 1, 2, 3};
    for (int i = 0; i < 6; ++i) {
        const gpu::GpuVertex& g = t.vertices[i];
        const int c = order[i];
        if (static_cast<int32_t>(g.x) != expectX[c] * 16 || static_cast<int32_t>(g.y) != expectY[c] * 16) {
            std::printf("hw_sprite_corners: corner %d at the wrong position\n", c);
            return 1;
        }
        if (g.rgba[0] != 200 || g.rgba[3] != 203) {
            std::printf("hw_sprite_corners: colour not taken from the second vertex\n");
            return 1;
        }
    }
    // S comes from the first vertex at X of the first corner and the second at the second corner.
    if (t.vertices[0].s != 4.0f || t.vertices[1].s != 8.0f || t.vertices[2].t != 12.0f) {
        std::printf("hw_sprite_corners: texture coordinates not per corner\n");
        return 1;
    }
    std::printf("hw_sprite_corners: six vertices, corner rule and second-vertex colour as chapter 03.2\n");
    return 0;
}

// ---- GPU tests -----------------------------------------------------------------------------------

int TestCoverageVectors(gpu::GpuDrawer& drawer) {
    // Chapter 03.3 vectors at 1x (sprite spans, pixel n covered when its centre is inside [min, max)):
    // 0.0..2.0 covers 0 and 1; 0.5..1.5 covers 1; 0.0625..1.0 covers nothing.
    Setup s;
    s.type = PrimType::Sprite;
    s.iip = false;
    const std::vector<Vertex> v = {
        MakeVertex(0, 0, 255, 0, 0, 128), MakeVertex(2, 1, 255, 0, 0, 128),   // row 0: x 0..2
        MakeVertex(0, 2, 0, 255, 0, 128), MakeVertex(1, 3, 0, 255, 0, 128),   // row 2: x 0..1
    };
    // Sprites with sub-pixel edges (12.4 units): 0.5..1.5 on row 4, 0.0625..1.0 on row 6.
    std::vector<Vertex> w = {MakeVertex(0, 4, 0, 0, 255, 128), MakeVertex(1, 5, 0, 0, 255, 128)};
    w[0].x4 = 8;
    w[1].x4 = 24;
    std::vector<Vertex> z = {MakeVertex(0, 6, 9, 9, 9, 128), MakeVertex(1, 7, 9, 9, 9, 128)};
    z[0].x4 = 1;
    z[1].x4 = 16;
    std::vector<Vertex> all;
    all.insert(all.end(), v.begin(), v.end());
    all.insert(all.end(), w.begin(), w.end());
    all.insert(all.end(), z.begin(), z.end());
    Batch batch = MakeBatch(s, all, {PrimType::Sprite, PrimType::Sprite, PrimType::Sprite});
    CaseResult result = RunCase(drawer, s, batch, 7);
    // Expected coverage from the vectors, checked against the GPU output directly.
    memory::LocalMemory gpuMemory;
    FillPattern(gpuMemory, s, 7);
    writeback::BatchTranslation t;
    writeback::Counters counters;
    CounterSet counterSet;
    clut::ClutUnit clut(counterSet);
    writeback::TranslateBatch(batch, gpuMemory, clut, t, counters);
    std::string error;
    if (!writeback::ExecuteBatch(drawer, t, gpuMemory, counters, error)) {
        std::printf("hw_gpu_coverage_vectors: GPU error %s\n", error.c_str());
        return 1;
    }
    const memory::BufferView frame{memory::blockOfPage(s.fbp), s.fbw, memory::Psm::Ct32};
    auto covered = [&](uint32_t x, uint32_t y) {
        return (gpuMemory.readPixel(frame, x, y) & 0xFFu) == 255u;  // red channel from the first sprite
    };
    auto coveredBlue = [&](uint32_t x, uint32_t y) { return ((gpuMemory.readPixel(frame, x, y) >> 16) & 0xFFu) == 255u; };
    bool ok = covered(0, 0) && covered(1, 0) && !covered(2, 0);
    ok = ok && !covered(0, 1) && !covered(1, 1);
    ok = ok && coveredBlue(1, 4) && !coveredBlue(0, 4) && !coveredBlue(2, 4);
    ok = ok && !coveredBlue(0, 6) && !coveredBlue(1, 6);
    if (!ok) {
        std::printf("hw_gpu_coverage_vectors: coverage differs from the chapter 03.3 vectors\n");
        return 1;
    }
    return Report("hw_gpu_coverage_vectors", result);
}

int TestEquivalenceFlat(gpu::GpuDrawer& drawer) {
    Setup s;
    s.iip = false;
    Batch batch = MakeBatch(s, FlatQuad(0, 0, 32, 32, 12, 200, 40, 255), {PrimType::Triangle, PrimType::Triangle});
    return Report("hw_gpu_equivalence_flat", RunCase(drawer, s, batch, 1));
}

int TestEquivalenceTexture(gpu::GpuDrawer& drawer) {
    // Textured quad, STQ coordinates on the pixel grid (decal, modulate, nearest, repeat).
    int failures = 0;
    for (uint32_t tfx = 0; tfx < 2; ++tfx) {
        Setup s;
        s.iip = false;
        s.tme = true;
        s.tpsm = 0;  // CT32 texture
        s.tw = 3;
        s.th = 3;
        s.tfx = tfx;
        s.fst = true;  // FST: UV in 1/16 texel units (chapter 03.5)
        std::vector<Vertex> q = {MakeVertex(0, 0, 128, 128, 128, 128, 0, 0, 0, 0),
                                 MakeVertex(40, 0, 128, 128, 128, 128, 0, 0, 40 * 16, 0),
                                 MakeVertex(0, 40, 128, 128, 128, 128, 0, 0, 0, 40 * 16),
                                 MakeVertex(40, 0, 128, 128, 128, 128, 0, 0, 40 * 16, 0),
                                 MakeVertex(0, 40, 128, 128, 128, 128, 0, 0, 0, 40 * 16),
                                 MakeVertex(40, 40, 128, 128, 128, 128, 0, 0, 40 * 16, 40 * 16)};
        Batch batch = MakeBatch(s, q, {PrimType::Triangle, PrimType::Triangle});
        failures += Report(tfx == 0 ? "hw_gpu_equivalence_texture_modulate" : "hw_gpu_equivalence_texture_decal",
                           RunCase(drawer, s, batch, 2 + tfx));
    }
    return failures ? 1 : 0;
}

int TestEquivalenceWrapRegion(gpu::GpuDrawer& drawer) {
    // STQ sprite over a 8x8 texture with region repeat and region clamp (chapter 03.5 table).
    int failures = 0;
    for (uint32_t mode = 2; mode <= 3; ++mode) {
        Setup s;
        s.tme = true;
        s.tw = 3;
        s.th = 3;
        s.wms = mode;
        s.wmt = mode;
        s.minu = mode == 3 ? 3 : 1;   // region repeat mask / region clamp minimum
        s.maxu = mode == 3 ? 1 : 6;   // region repeat fixed bits / region clamp maximum
        s.minv = s.minu;
        s.maxv = s.maxu;
        s.type = PrimType::Sprite;
        s.iip = false;
        s.fst = true;
        std::vector<Vertex> q = {MakeVertex(0, 0, 0, 0, 0, 0, 0, 0, 0, 0), MakeVertex(40, 40, 0, 0, 0, 0, 0, 0, 40 * 16, 40 * 16)};
        Batch batch = MakeBatch(s, q, {PrimType::Sprite});
        failures += Report(mode == 2 ? "hw_gpu_equivalence_region_clamp" : "hw_gpu_equivalence_region_repeat",
                           RunCase(drawer, s, batch, 4 + mode));
    }
    return failures ? 1 : 0;
}

int TestEquivalenceBilinear(gpu::GpuDrawer& drawer) {
    Setup s;
    s.tme = true;
    s.mmag = true;
    s.fst = true;
    s.iip = false;
    s.tw = 4;
    s.th = 4;
    std::vector<Vertex> q = {MakeVertex(0, 0, 0, 0, 0, 0, 0, 0, 8, 8), MakeVertex(32, 32, 0, 0, 0, 0, 0, 0, 8 + 32 * 16, 8 + 32 * 16)};
    s.type = PrimType::Sprite;
    Batch batch = MakeBatch(s, q, {PrimType::Sprite});
    return Report("hw_gpu_equivalence_bilinear", RunCase(drawer, s, batch, 9));
}

int TestEquivalenceTfxFog(gpu::GpuDrawer& drawer) {
    Setup s;
    s.iip = false;
    s.tme = false;
    s.tw = 3;
    s.th = 3;
    s.fst = true;
    s.tfx = 1;  // decal
    s.fge = true;
    s.fogcol = 0x402010;
    std::vector<Vertex> q = FlatQuad(0, 0, 40, 40, 200, 100, 50, 128, 0, 0, 40 * 16, 40 * 16);
    for (Vertex& v : q) v.fog = 90;
    Batch batch = MakeBatch(s, q, {PrimType::Triangle, PrimType::Triangle});
    return Report("hw_gpu_equivalence_tfx_fog", RunCase(drawer, s, batch, 11));
}

int TestEquivalenceDepthAlpha(gpu::GpuDrawer& drawer) {
    // Depth GEQUAL with a stored Z, then ALWAYS with Z writes, then the alpha test (KEEP).
    Setup s;
    s.iip = false;
    s.zte = true;
    s.ztst = 2;  // GEQUAL
    s.zmsk = false;
    Batch batch = MakeBatch(s, FlatQuad(0, 0, 32, 32, 90, 90, 90, 90), {PrimType::Triangle, PrimType::Triangle});
    int failures = Report("hw_gpu_equivalence_depth", RunCase(drawer, s, batch, 13));
    Setup a;
    a.iip = false;
    a.ate = true;
    a.atst = 6;   // GREATER
    a.aref = 0x20;
    a.afail = 0;  // KEEP
    Batch alpha = MakeBatch(a, FlatQuad(16, 16, 48, 48, 1, 2, 3, 0x40), {PrimType::Triangle, PrimType::Triangle});
    failures += Report("hw_gpu_equivalence_alpha_keep", RunCase(drawer, a, alpha, 14));
    return failures ? 1 : 0;
}

int TestEquivalenceScissor(gpu::GpuDrawer& drawer) {
    Setup s;
    s.iip = false;
    s.scax0 = 8;
    s.scax1 = 20;
    s.scay0 = 4;
    s.scay1 = 12;
    Batch batch = MakeBatch(s, FlatQuad(0, 0, 40, 40, 77, 66, 55, 255), {PrimType::Triangle, PrimType::Triangle});
    return Report("hw_gpu_equivalence_scissor", RunCase(drawer, s, batch, 15));
}

// ---- M5: blend unit, alpha test, DATE, FBA/FBMASK/DIMX through the raster path ------------------
// Every case compares the GPU result with the CPU reference over the whole batch rectangle, like the
// M4 equivalence cases: the CPU side is the bit-exact pixel library of chapter 08.

uint64_t AlphaRegister(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint8_t fix) {
    return pixel::encode_alpha_register(static_cast<pixel::BlendSource>(a), static_cast<pixel::BlendSource>(b),
                                        static_cast<pixel::BlendFactor>(c), static_cast<pixel::BlendSource>(d), fix);
}

// 81 blend modes of chapter 08.2 (A, B, C, D over Cs/Cd/Zero and As/Ad/FIX), with COLCLAMP both ways.
// The destination is the deterministic frame pattern, so every mode blends against real destination
// colours and alphas.
int TestRasterBlendModes(gpu::GpuDrawer& drawer) {
    uint64_t compared = 0;
    uint64_t mismatches = 0;
    for (uint32_t clamp = 0; clamp < 2; ++clamp) {
        for (uint32_t a = 0; a < 3; ++a) {
            for (uint32_t b = 0; b < 3; ++b) {
                for (uint32_t c = 0; c < 3; ++c) {
                    for (uint32_t d = 0; d < 3; ++d) {
                        Setup s;
                        s.iip = false;
                        s.abe = true;
                        s.colclamp = clamp != 0;
                        s.alpha_reg = AlphaRegister(a, b, c, d, 0x40);
                        Batch batch = MakeBatch(s, FlatQuad(0, 0, 8, 8, 120, 60, 30, 0x90),
                                                {PrimType::Triangle, PrimType::Triangle});
                        CaseResult r = RunCase(drawer, s, batch, 21 + a * 27 + b * 9 + c * 3 + d);
                        if (r.gpuError) {
                            std::printf("hw_gpu_raster_blend: GPU error: %s\n", r.error.c_str());
                            return 1;
                        }
                        compared += r.compared;
                        mismatches += r.mismatches;
                    }
                }
            }
        }
    }
    std::printf("hw_gpu_raster_blend: compared %llu pixels over 162 modes, mismatches %llu\n",
                static_cast<unsigned long long>(compared), static_cast<unsigned long long>(mismatches));
    return mismatches == 0 ? 0 : 1;
}

// PABE: with PABE = 1 the blend is skipped while the source alpha MSB is 0 (08.2).
int TestRasterPabe(gpu::GpuDrawer& drawer) {
    int failures = 0;
    for (uint32_t alpha : {0x40u, 0xC0u}) {
        Setup s;
        s.iip = false;
        s.abe = true;
        s.pabe = true;
        s.alpha_reg = AlphaRegister(0, 1, 0, 1, 0x00);  // Cs*Ad + Cd*(1-Ad)
        Batch batch = MakeBatch(s, FlatQuad(0, 0, 8, 8, 200, 40, 90, static_cast<uint8_t>(alpha)),
                                {PrimType::Triangle, PrimType::Triangle});
        failures += Report("hw_gpu_raster_pabe", RunCase(drawer, s, batch, 31 + alpha));
    }
    return failures ? 1 : 0;
}

// Alpha test: the eight comparisons of 08.6 and the four AFAIL actions, with alphas on both sides of
// the reference so both the pass and the fail path run.
int TestRasterAlphaTest(gpu::GpuDrawer& drawer) {
    uint64_t compared = 0;
    uint64_t mismatches = 0;
    for (uint32_t atst = 0; atst < 8; ++atst) {
        for (uint32_t afail = 0; afail < 4; ++afail) {
            Setup s;
            s.iip = false;
            s.ate = true;
            s.atst = atst;
            s.afail = afail;
            s.aref = 0x50;
            s.zte = true;
            s.ztst = 1;
            std::vector<Vertex> quad = FlatQuad(0, 0, 8, 8, 70, 80, 90, 0x30);
            std::vector<Vertex> other = FlatQuad(8, 0, 16, 8, 20, 30, 40, 0x70);
            quad.insert(quad.end(), other.begin(), other.end());
            Batch batch = MakeBatch(s, quad, {PrimType::Triangle, PrimType::Triangle, PrimType::Triangle,
                                              PrimType::Triangle});
            CaseResult r = RunCase(drawer, s, batch, 41 + atst * 4 + afail);
            if (r.gpuError) {
                std::printf("hw_gpu_raster_atest: GPU error: %s\n", r.error.c_str());
                return 1;
            }
            compared += r.compared;
            mismatches += r.mismatches;
        }
    }
    std::printf("hw_gpu_raster_atest: compared %llu pixels over 32 combinations, mismatches %llu\n",
                static_cast<unsigned long long>(compared), static_cast<unsigned long long>(mismatches));
    return mismatches == 0 ? 0 : 1;
}

// DATE (08.7): the destination alpha MSB decides whether a pixel is written. A first quad writes,
// a second one with the opposite alpha tests against the pixels the first one produced.
int TestRasterDate(gpu::GpuDrawer& drawer) {
    uint64_t compared = 0;
    uint64_t mismatches = 0;
    for (uint32_t datm = 0; datm < 2; ++datm) {
        Setup s;
        s.iip = false;
        s.date = true;
        s.datm = datm != 0;
        std::vector<Vertex> quad = FlatQuad(0, 0, 8, 8, 90, 40, 10, 0x80);
        std::vector<Vertex> inner = FlatQuad(2, 2, 6, 6, 10, 200, 30, 0x40);
        quad.insert(quad.end(), inner.begin(), inner.end());
        Batch batch = MakeBatch(s, quad, {PrimType::Triangle, PrimType::Triangle, PrimType::Triangle,
                                          PrimType::Triangle});
        CaseResult r = RunCase(drawer, s, batch, 51 + datm);
        if (r.gpuError) {
            std::printf("hw_gpu_raster_date: GPU error: %s\n", r.error.c_str());
            return 1;
        }
        compared += r.compared;
        mismatches += r.mismatches;
    }
    std::printf("hw_gpu_raster_date: compared %llu pixels, mismatches %llu\n",
                static_cast<unsigned long long>(compared), static_cast<unsigned long long>(mismatches));
    return mismatches == 0 ? 0 : 1;
}

// FBA (alpha forced to 0x80), FBMSK on a CT24 target and DIMX dithering on a CT16 target (05.6, 08.8).
int TestRasterFrameWrites(gpu::GpuDrawer& drawer) {
    int failures = 0;
    {
        Setup s;
        s.iip = false;
        s.fba = true;
        Batch batch = MakeBatch(s, FlatQuad(0, 0, 8, 8, 30, 60, 90, 0x12),
                                {PrimType::Triangle, PrimType::Triangle});
        failures += Report("hw_gpu_raster_fba", RunCase(drawer, s, batch, 61));
    }
    {
        Setup s;
        s.iip = false;
        s.fpsm = 1;  // CT24
        s.fbmsk = 0x00FFFFFF;
        Batch batch = MakeBatch(s, FlatQuad(0, 0, 8, 8, 240, 120, 60, 0xFF),
                                {PrimType::Triangle, PrimType::Triangle});
        failures += Report("hw_gpu_raster_fbmsk", RunCase(drawer, s, batch, 62));
    }
    {
        Setup s;
        s.iip = false;
        s.fpsm = 2;  // CT16
        s.dthe = true;
        s.dimx = 0x13579BDF2468ACE0ull & 0xFFFFFFFFFFFFull;  // 16 four-bit entries, values -4..+3
        Batch batch = MakeBatch(s, FlatQuad(0, 0, 8, 8, 130, 70, 210, 0xC0),
                                {PrimType::Triangle, PrimType::Triangle});
        failures += Report("hw_gpu_raster_dither", RunCase(drawer, s, batch, 63));
    }
    return failures ? 1 : 0;
}

// ---- M6 interface: present scheduler, auxiliary frames, per-pass timings ------------------------

// Default policy: a frame end is presented once and then re-presented on later ticks without a queue.
int TestPresentScheduler() {
    present::Scheduler s;
    if (s.Tick() != present::Action::none) {
        std::printf("hw_present_scheduler: a tick without any frame presented something\n");
        return 1;
    }
    present::FrameStamp first;
    first.guestFrame = 7;
    first.hasImage = true;
    s.SubmitFrame(first);
    if (s.Tick() != present::Action::presentNew || s.lastShown().guestFrame != 7) {
        std::printf("hw_present_scheduler: the submitted frame was not presented\n");
        return 1;
    }
    if (s.Tick() != present::Action::presentHeld) {
        std::printf("hw_present_scheduler: the held image was not re-presented\n");
        return 1;
    }
    if (s.presentedNew() != 1 || s.presentedHeld() != 1 || s.dropped() != 0) {
        std::printf("hw_present_scheduler: counters %llu/%llu/%llu\n",
                    static_cast<unsigned long long>(s.presentedNew()),
                    static_cast<unsigned long long>(s.presentedHeld()),
                    static_cast<unsigned long long>(s.dropped()));
        return 1;
    }
    // A frame end without a guest image still reaches the display (chapter 12.4 shows black).
    present::FrameStamp blank;
    blank.guestFrame = 8;
    blank.hasImage = false;
    s.SubmitFrame(blank);
    if (s.Tick() != present::Action::presentNew || s.hasImage()) {
        std::printf("hw_present_scheduler: an image-less frame end was mishandled\n");
        return 1;
    }
    std::printf("hw_present_scheduler: new/held/dropped = 2/1/0, newest frame %llu\n",
                static_cast<unsigned long long>(s.lastShown().guestFrame));
    return 0;
}

// Holding disabled: nothing is presented until a new frame end arrives, and a backlog keeps only the
// newest frame (never a queue).
int TestPresentSchedulerHold() {
    present::Scheduler s;
    present::Policy policy;
    policy.hold_last_image = false;
    s.SetPolicy(policy);
    present::FrameStamp stamp;
    stamp.hasImage = true;
    stamp.guestFrame = 1;
    s.SubmitFrame(stamp);
    if (s.Tick() != present::Action::presentNew) {
        std::printf("hw_present_scheduler_hold: first frame not presented\n");
        return 1;
    }
    if (s.Tick() != present::Action::none) {
        std::printf("hw_present_scheduler_hold: presented again with holding disabled\n");
        return 1;
    }
    stamp.guestFrame = 2;
    s.SubmitFrame(stamp);
    stamp.guestFrame = 3;
    s.SubmitFrame(stamp);
    if (s.dropped() != 1) {
        std::printf("hw_present_scheduler_hold: backlog was not collapsed (dropped %llu)\n",
                    static_cast<unsigned long long>(s.dropped()));
        return 1;
    }
    if (s.Tick() != present::Action::presentNew || s.lastShown().guestFrame != 3) {
        std::printf("hw_present_scheduler_hold: the newest frame did not win\n");
        return 1;
    }
    std::printf("hw_present_scheduler_hold: hold=none, newest wins, 1 dropped\n");
    return 0;
}

// Host rate above the guest rate: two ticks per guest frame give one new present and one re-present,
// and the late-input hook runs on every tick.
int TestPresentSchedulerRate() {
    present::Scheduler s;
    int hookCalls = 0;
    s.SetLateInputHook([](void* context) { ++*static_cast<int*>(context); }, &hookCalls);
    uint64_t newPresents = 0;
    uint64_t heldPresents = 0;
    for (uint64_t frame = 10; frame < 13; ++frame) {
        present::FrameStamp stamp;
        stamp.guestFrame = frame;
        stamp.hasImage = true;
        s.SubmitFrame(stamp);
        for (int tick = 0; tick < 2; ++tick) {
            const present::Action action = s.Tick();
            if (action == present::Action::presentNew) ++newPresents;
            if (action == present::Action::presentHeld) ++heldPresents;
        }
    }
    if (newPresents != 3 || heldPresents != 3) {
        std::printf("hw_present_scheduler_rate: %llu new / %llu held at 2x\n",
                    static_cast<unsigned long long>(newPresents),
                    static_cast<unsigned long long>(heldPresents));
        return 1;
    }
    if (hookCalls != 6) {
        std::printf("hw_present_scheduler_rate: late input hook ran %d times, expected 6\n", hookCalls);
        return 1;
    }
    std::printf("hw_present_scheduler_rate: 3 frames over 6 ticks = 3 new + 3 held, 6 late-input calls\n");
    return 0;
}

// GIF packet helpers for the auxiliary-frame test: one A+D packet writing registers, then an IMAGE
// packet carrying host->local data (chapter 02.1).
Qword PackedTag(uint32_t nloop, bool eop, uint32_t format, const std::vector<uint8_t>& regs) {
    Qword tag;
    tag.lo = uint64_t{nloop} | (uint64_t{eop ? 1u : 0u} << 15) | (uint64_t{format} << 58);
    tag.hi = 0;
    for (size_t i = 0; i < regs.size() && i < 16; ++i) {
        tag.hi |= uint64_t{regs[i]} << (4 * i);
    }
    if (!regs.empty()) {
        tag.lo |= uint64_t{static_cast<uint32_t>(regs.size())} << 60;
    }
    return tag;
}

// Uploads `bytes` (qword multiple) to GS page 0 with an 8x8 CT32 transfer.
void UploadToPage0(writeback::Renderer& renderer, uint32_t value) {
    std::vector<Qword> stream;
    stream.push_back(PackedTag(4, false, 0, {0xE}));
    const uint64_t bitbltbuf = (uint64_t{1} << 16) | (uint64_t{1} << 48);  // SBW = DBW = 1, CT32
    const uint64_t trxpos = 0;
    const uint64_t trxreg = uint64_t{8} | (uint64_t{8} << 32);
    stream.push_back(Qword{bitbltbuf, 0x50});
    stream.push_back(Qword{trxpos, 0x51});
    stream.push_back(Qword{trxreg, 0x52});
    stream.push_back(Qword{0, 0x53});  // TRXDIR = 0: host -> local
    const uint32_t qwords = 16;        // 8 x 8 CT32 = 256 bytes
    stream.push_back(PackedTag(qwords, true, 2, {}));
    for (uint32_t i = 0; i < qwords; ++i) {
        Qword data;
        data.lo = uint64_t{value} | (uint64_t{value} << 32);
        data.hi = uint64_t{value} | (uint64_t{value} << 32);
        stream.push_back(data);
    }
    renderer.GifTransfer(1, reinterpret_cast<const uint8_t*>(stream.data()), stream.size() * sizeof(Qword));
    renderer.Flush();
}

uint64_t SlmHash(const writeback::Renderer& renderer) {
    const std::uint8_t* bytes = renderer.Slm();
    uint64_t hash = 1469598103934665603ull;  // FNV-1a
    for (size_t i = 0; i < 4u * 1024u * 1024u; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

// Requirement 2: an auxiliary frame may draw, upload and clear, but the guest's memory is restored and
// a guest readback is never answered from it.
int TestAuxFrameGuestUntouched() {
    writeback::Renderer renderer;
    UploadToPage0(renderer, 0x11223344u);
    const uint64_t before = SlmHash(renderer);
    if (!renderer.BeginAuxiliaryFrame()) {
        std::printf("hw_aux_frame_guest_untouched: begin refused\n");
        return 1;
    }
    UploadToPage0(renderer, 0xAABBCCDDu);
    if (SlmHash(renderer) == before) {
        std::printf("hw_aux_frame_guest_untouched: the auxiliary upload changed nothing (test is blind)\n");
        renderer.EndAuxiliaryFrame();
        return 1;
    }
    std::uint8_t scratch[64] = {};
    if (renderer.ReadFifo(scratch, 4) != 0 || renderer.GetCounters().auxiliaryReadRefused != 1) {
        std::printf("hw_aux_frame_guest_untouched: a readback during an auxiliary frame was answered\n");
        renderer.EndAuxiliaryFrame();
        return 1;
    }
    renderer.EndAuxiliaryFrame();
    if (SlmHash(renderer) != before) {
        std::printf("hw_aux_frame_guest_untouched: guest memory differs after the auxiliary frame\n");
        return 1;
    }
    if (renderer.AuxiliaryActive() || renderer.GetCounters().auxiliaryFrames != 1) {
        std::printf("hw_aux_frame_guest_untouched: state not restored\n");
        return 1;
    }
    if (!renderer.BeginAuxiliaryFrame() || renderer.BeginAuxiliaryFrame()) {
        std::printf("hw_aux_frame_guest_untouched: nested auxiliary frame was accepted\n");
        return 1;
    }
    renderer.EndAuxiliaryFrame();
    std::printf("hw_aux_frame_guest_untouched: guest SLM identical, readback refused, nested begin refused\n");
    return 0;
}

// Requirement 4: per-pass CPU timers exist and are honest. Without a Vulkan device the drawer refuses
// the batch before any GPU work, so nothing is timed and the failure is reported instead of a fake zero
// being presented as a measurement. The compose pass is measured on its own.
int TestPassTimings() {
    writeback::Renderer renderer;
    UploadToPage0(renderer, 0x55AA55AAu);
    writeback::FrameOutput frame;
    renderer.ComposeFrame(frame);
    const writeback::PassTimings timings = renderer.Timings();  // copy: a later flush overwrites it
    if (!(timings.composeUs > 0.0)) {
        std::printf("hw_pass_timings: compose was not measured (%.3f us)\n", timings.composeUs);
        return 1;
    }
    if (timings.executeGpuUs != 0.0) {
        std::printf("hw_pass_timings: the GPU field is reserved and must stay 0\n");
        return 1;
    }

    // A draw without a device must fail cleanly and leave the draw timers at zero.
    std::vector<Qword> draw;
    draw.push_back(PackedTag(7, true, 0, {0xE}));
    draw.push_back(Qword{3, 0x00});                                          // PRIM = triangle
    draw.push_back(Qword{uint64_t{639} << 16 | uint64_t{447} << 48, 0x40});  // SCISSOR_1
    draw.push_back(Qword{uint64_t{1} << 16, 0x4C});                          // FRAME_1: FBW 1
    draw.push_back(Qword{0, 0x18});                                          // XYOFFSET_1
    draw.push_back(Qword{uint64_t{20} | (uint64_t{20} << 16) | (uint64_t{0x100} << 32), 0x05});
    draw.push_back(Qword{uint64_t{80} | (uint64_t{20} << 16) | (uint64_t{0x100} << 32), 0x05});
    draw.push_back(Qword{uint64_t{20} | (uint64_t{80} << 16) | (uint64_t{0x100} << 32), 0x05});
    renderer.GifTransfer(1, reinterpret_cast<const std::uint8_t *>(draw.data()), draw.size() * sizeof(Qword));
    renderer.Flush();
    if (!renderer.Failed() || renderer.LastError().empty()) {
        std::printf("hw_pass_timings: a draw without a device was not reported as a failure\n");
        return 1;
    }
    const writeback::PassTimings afterDraw = renderer.Timings();
    if (afterDraw.translateUs != 0.0 || afterDraw.executeUs != 0.0) {
        std::printf("hw_pass_timings: draw timers moved without a device (%.3f/%.3f us)\n",
                    afterDraw.translateUs, afterDraw.executeUs);
        return 1;
    }
    if (renderer.TimedFrames() < 2) {
        std::printf("hw_pass_timings: %llu timed frames, expected at least 2\n",
                    static_cast<unsigned long long>(renderer.TimedFrames()));
        return 1;
    }
    std::printf("hw_pass_timings: compose %.1f us, draw timers 0 without a device, %llu timed frames\n",
                timings.composeUs, static_cast<unsigned long long>(renderer.TimedFrames()));
    return 0;
}

// IN-335: the frame and depth buffers may share local memory (a 2D pass whose unused ZBUF points at the
// other display buffer, or at the frame itself). The write-back must not put the pre-draw depth words of
// the rectangle back over the pixels the batch just drew. The GPU result must equal the CPU reference,
// which stores the frame word and then the depth word per pixel. Cases: ZBUF = FRAME with Z writes
// masked, ZBUF = FRAME with Z written, and a ZBUF that starts one page row (32 lines) above FRAME, so
// only the lower rows of the depth rectangle alias the upper rows of the drawn frame rectangle (rows the
// write-back visits earlier, which is what exposes a stale depth word written over them).
// Z written into a buffer that aliases the frame is reported, not asserted: Z32 and CT32 swizzle the same
// page differently, so the last word stored at an address depends on the order in which the rasteriser
// visits pixels (CPU reference: primitive by primitive; write-back: one raster pass over the rectangle),
// which the specification does not define (IN-335).
int TestWritebackAliasedDepth(gpu::GpuDrawer& drawer) {
    struct AliasCase {
        const char* label;
        int32_t zbpOffsetPages;
        bool zmsk;
        bool asserted;
    };
    const AliasCase cases[] = {{"same pages, Z masked", 0, true, true},
                               {"same pages, Z written (report only)", 0, false, false},
                               {"one page row above, Z masked", -10, true, true}};
    int failures = 0;
    for (const AliasCase& c : cases) {
        Setup s;
        s.iip = false;
        s.fbp = 30;
        s.zbp = static_cast<uint32_t>(static_cast<int32_t>(s.fbp) + c.zbpOffsetPages);  // FBW 10: 10 pages per row
        s.zmsk = c.zmsk;
        s.zte = true;
        s.ztst = 1;  // ALWAYS
        Batch batch = MakeBatch(s, FlatQuad(4, 20, 60, 60, 200, 30, 90, 255), {PrimType::Triangle, PrimType::Triangle});
        const CaseResult r = RunCase(drawer, s, batch, static_cast<uint32_t>(41 + c.zbpOffsetPages));
        std::printf("  case: %s\n", c.label);
        const int failed = Report("hw_gpu_writeback_aliased_depth", r);
        if (c.asserted || r.gpuError) failures += failed;
    }
    return failures == 0 ? 0 : 1;
}

// Chapter 10.2: a LOCAL->HOST read observes every earlier draw, including draws still waiting in the open
// batch, and the data is consumed sequentially across FIFO reads. Two sprites of different colours fill
// rows 0..3 and 4..7 of an 8 x 8 CT32 rectangle; it is read back in two halves of 128 bytes with no VBlank
// flush in between, so the first half must hold only the top colour and the second only the bottom one.
int TestReadbackObservesPendingDraw(const gpu::VulkanHandles& handles) {
    writeback::Renderer renderer;
    std::string error;
    if (!renderer.Init(handles, error)) {
        std::printf("hw_readback_pending_draw: GPU drawer init failed: %s\n", error.c_str());
        return 1;
    }
    const uint32_t top = 0x80402010u;     // A 0x80, B 0x40, G 0x20, R 0x10
    const uint32_t bottom = 0x7F0A0B0Cu;  // A 0x7F, B 0x0A, G 0x0B, R 0x0C
    auto rgbaq = [](uint32_t colour) { return uint64_t{colour} | (uint64_t{0x3F800000u} << 32); };  // Q = 1.0
    auto xyz = [](uint32_t x, uint32_t y) { return uint64_t{x * 16} | (uint64_t{y * 16} << 16); };
    std::vector<Qword> stream;
    stream.push_back(PackedTag(16, true, 0, {0xE}));
    stream.push_back(Qword{uint64_t{0} | (uint64_t{1} << 16), 0x4C});            // FRAME_1: FBP 0, FBW 1, CT32
    stream.push_back(Qword{uint64_t{64} | (uint64_t{1} << 32), 0x4E});           // ZBUF_1: ZBP 64, ZMSK
    stream.push_back(Qword{(uint64_t{63} << 16) | (uint64_t{63} << 48), 0x40});  // SCISSOR_1: 0..63 x 0..63
    stream.push_back(Qword{0, 0x18});                                             // XYOFFSET_1 = 0
    stream.push_back(Qword{(uint64_t{1} << 16) | (uint64_t{1} << 17), 0x47});    // TEST_1: ZTE, ZTST ALWAYS
    stream.push_back(Qword{6, 0x00});                                             // PRIM: sprite, context 1
    stream.push_back(Qword{rgbaq(top), 0x01});
    stream.push_back(Qword{xyz(0, 0), 0x05});
    stream.push_back(Qword{xyz(8, 4), 0x05});  // kick: rows 0..3
    stream.push_back(Qword{rgbaq(bottom), 0x01});
    stream.push_back(Qword{xyz(0, 4), 0x05});
    stream.push_back(Qword{xyz(8, 8), 0x05});                                     // kick: rows 4..7
    stream.push_back(Qword{uint64_t{0} | (uint64_t{1} << 16), 0x50});             // BITBLTBUF: SBP 0, SBW 1, CT32
    stream.push_back(Qword{0, 0x51});                                             // TRXPOS 0, 0
    stream.push_back(Qword{uint64_t{8} | (uint64_t{8} << 32), 0x52});             // TRXREG 8 x 8
    stream.push_back(Qword{1, 0x53});                                             // TRXDIR 1: local -> host
    if (!renderer.GifTransfer(1, reinterpret_cast<const uint8_t*>(stream.data()), stream.size() * sizeof(Qword))) {
        std::printf("hw_readback_pending_draw: GIF transfer refused\n");
        return 1;
    }
    std::vector<uint32_t> words(64, 0);
    const std::size_t first = renderer.ReadFifo(words.data(), 128);
    const std::size_t second = renderer.ReadFifo(words.data() + 32, 128);
    uint32_t pastEnd[4] = {};
    const std::size_t third = renderer.ReadFifo(pastEnd, sizeof(pastEnd));  // past the end: zeros, one overrun
    uint32_t wrong = 0;
    for (size_t i = 0; i < 32; ++i) wrong += words[i] != top ? 1u : 0u;
    for (size_t i = 32; i < 64; ++i) wrong += words[i] != bottom ? 1u : 0u;
    std::printf("hw_readback_pending_draw: read %zu + %zu bytes (+%zu past the end), %u of 64 words wrong, "
                "words 0/32 = %08X/%08X, overruns %llu\n",
                first, second, third, wrong, words[0], words[32],
                static_cast<unsigned long long>(renderer.GetCounters().fifoOverruns));
    if (renderer.Failed()) {
        std::printf("hw_readback_pending_draw: renderer failed: %s\n", renderer.LastError().c_str());
        return 1;
    }
    return (first == 128 && second == 128 && wrong == 0 && renderer.GetCounters().fifoOverruns == 1) ? 0 : 1;
}

// IN-335 replay control: a double-buffered 2D scene in which every frame draws into the back buffer
// while ZBUF (Z writes masked) stays at page 0, so on every other frame the unused depth buffer shares
// the pages of the buffer being drawn: the pattern of GT4's boot and menu screens (ZBUF = 0x100000000).
// The legacy capture is replayed through the CPU reference and through the GPU write-back renderer; every
// frame must give the same composed image and the same 4 MiB local memory hash. Only sprites are drawn,
// whose coverage and attributes both rasterisers are specified to agree on exactly, so any difference is
// a write-back or flush defect. Before the IN-335 fix the frames drawn into page 0 lost part of their draws here.
int TestReplayParityDoubleBuffer(const gpu::VulkanHandles& handles) {
    constexpr uint32_t kFrames = 6;
    constexpr uint32_t kBufferPages[2] = {0, 2};  // 64 x 64 CT32 buffers, FBW 1: two pages each
    cpuref::CaptureWriter capture;
    capture.Privileged(0x12000000u, 1u | (1u << 5) | (0xFFu << 8));                // PMODE: circuit 1, ALP 0xFF
    capture.Privileged(0x12000010u, uint64_t{2} << 13);                             // SMODE1: CMOD 2 (NTSC)
    capture.Privileged(0x12000080u, (uint64_t{63} << 32) | (uint64_t{63} << 44));  // DISPLAY1: 64 x 64
    auto rgbaq = [](uint32_t colour) { return uint64_t{colour} | (uint64_t{0x3F800000u} << 32); };  // Q = 1.0
    auto xyz = [](uint32_t x, uint32_t y) { return uint64_t{x * 16} | (uint64_t{y * 16} << 16); };
    for (uint32_t frame = 0; frame < kFrames; ++frame) {
        const uint32_t back = kBufferPages[frame & 1u];
        const uint32_t background = 0x80000000u | (frame * 0x101010u + 0x203040u);
        const uint32_t text = 0x60000000u | (0xF0E0D0u - frame * 0x010203u);
        // A+D pairs: data, register address.
        const std::vector<uint64_t> words = {
            uint64_t{back} | (uint64_t{1} << 16), 0x4C,                         // FRAME_1: back buffer, FBW 1, CT32
            uint64_t{0} | (uint64_t{1} << 32), 0x4E,                            // ZBUF_1: page 0, ZMSK
            (uint64_t{63} << 16) | (uint64_t{63} << 48), 0x40,                  // SCISSOR_1: 0..63 x 0..63
            0, 0x18,                                                            // XYOFFSET_1 = 0
            (uint64_t{1} << 16) | (uint64_t{1} << 17), 0x47,                    // TEST_1: ZTE, ZTST ALWAYS
            (uint64_t{1} << 2) | (uint64_t{1} << 6), 0x42,                      // ALPHA_1: (Cs - Cd) * As + Cd
            6, 0x00,                                                            // PRIM: sprite
            rgbaq(background), 0x01, xyz(0, 0), 0x05, xyz(64, 64), 0x05,       // full-buffer clear
            6u | (1u << 6), 0x00,                                               // PRIM: sprite, ABE
            rgbaq(text), 0x01, xyz(4 + frame, 8), 0x05, xyz(40 + frame, 14), 0x05,  // a "text line"
        };
        capture.Gif(1, cpuref::CaptureWriter::Packet(static_cast<uint32_t>(words.size() / 2), true, false, 0, 0,
                                                     {0xE}, words));
        capture.Privileged(0x12000070u, uint64_t{back} | (uint64_t{1} << 9));  // DISPFB1: show the back buffer
        capture.FrameEnd(frame);
    }

    struct FrameRecord {
        bool valid = false;
        std::vector<uint8_t> rgb;
        uint64_t slmHash = 0;
    };
    std::vector<FrameRecord> cpuFrames;
    std::vector<FrameRecord> gpuFrames;
    auto record = [](std::vector<FrameRecord>& out, const cpuref::FrameImage& image, const uint8_t* slm) {
        FrameRecord r;
        r.valid = image.valid;
        if (image.valid) {
            for (const display::Rgba8& p : image.image.pixels) {
                r.rgb.push_back(p.r);
                r.rgb.push_back(p.g);
                r.rgb.push_back(p.b);
            }
        }
        r.slmHash = cpuref::HashLocalMemory(slm, 4u * 1024u * 1024u);
        out.push_back(std::move(r));
    };

    std::string error;
    cpuref::CpuRenderer cpu;
    cpuref::ReplaySummary cpuSummary;
    if (!cpuref::ReplayCapture(capture.Bytes(), cpu, cpuSummary,
                               [&](uint64_t, const cpuref::FrameImage& image) {
                                   record(cpuFrames, image, cpu.Memory().Local().bytes().data());
                               },
                               error)) {
        std::printf("hw_replay_parity_double_buffer: CPU replay failed: %s\n", error.c_str());
        return 1;
    }
    writeback::Renderer renderer;
    if (!renderer.Init(handles, error)) {
        std::printf("hw_replay_parity_double_buffer: GPU drawer init failed: %s\n", error.c_str());
        return 1;
    }
    writeback::WritebackReplayTarget target(renderer);
    cpuref::ReplaySummary gpuSummary;
    const bool replayed = cpuref::ReplayCapture(capture.Bytes(), target, gpuSummary,
                                                [&](uint64_t, const cpuref::FrameImage& image) {
                                                    record(gpuFrames, image, renderer.Slm());
                                                },
                                                error);
    if (!replayed || renderer.Failed()) {
        std::printf("hw_replay_parity_double_buffer: GPU replay failed: %s %s\n", error.c_str(),
                    renderer.LastError().c_str());
        return 1;
    }
    if (cpuFrames.size() != kFrames || gpuFrames.size() != kFrames) {
        std::printf("hw_replay_parity_double_buffer: frame counts %zu / %zu, expected %u\n", cpuFrames.size(),
                    gpuFrames.size(), kFrames);
        return 1;
    }
    int failures = 0;
    for (uint32_t frame = 0; frame < kFrames; ++frame) {
        const FrameRecord& c = cpuFrames[frame];
        const FrameRecord& g = gpuFrames[frame];
        const bool imageEqual = c.valid == g.valid && c.rgb == g.rgb;
        const bool memoryEqual = c.slmHash == g.slmHash;
        std::printf("  frame %u (%s): image %s (valid %d/%d, %zu bytes), memory %016llx / %016llx %s\n", frame,
                    (frame & 1u) ? "odd" : "even", imageEqual ? "equal" : "DIFFERENT", c.valid ? 1 : 0,
                    g.valid ? 1 : 0, c.rgb.size(), static_cast<unsigned long long>(c.slmHash),
                    static_cast<unsigned long long>(g.slmHash), memoryEqual ? "equal" : "DIFFERENT");
        if (!imageEqual || !memoryEqual) ++failures;
        // The fixture must not be blind: every frame changes memory and shows a non-empty image.
        if (frame > 0 && c.slmHash == cpuFrames[frame - 1].slmHash) {
            std::printf("  frame %u left memory unchanged (fixture is blind)\n", frame);
            ++failures;
        }
        if (!c.valid || c.rgb.empty()) {
            std::printf("  frame %u has no composed image (fixture is blind)\n", frame);
            ++failures;
        }
    }
    std::printf("hw_replay_parity_double_buffer: %u frames, %d failures\n", kFrames, failures);
    return failures == 0 ? 0 : 1;
}

// M6 output stage (13.3 step 3, 13.4, 12.9): the presentation compose follows the VBlank field, the
// reference compose (the donor-matching `vram` image) never does. A static 64 x 32 field buffer (one colour
// per row) is shown through a 480i NTSC circuit with FFMD = 1, so every source row covers two output rows.
//   - Reference: identical for even and odd fields; no deinterlace counters move.
//   - Deinterlace off: the presentation equals the reference for both fields.
//   - Weave over an even/odd pair: the odd field is shifted down one row (13.3 step 3) and woven, which
//     for static content must rebuild exactly the line-doubled reference image (no one-line bob).
//   - Weave of two field buffers (each with its own lines) gives the progressive frame (IN-339 phase).
//   - Bob: the CSR.FIELD = 1 field is shifted down one row (13.4, IN-339); the other is not.
// No Vulkan device is needed: uploads go to the shadow memory and composition is CPU work.
int TestPresentationFieldPhase() {
    constexpr uint32_t kWidth = 64;
    constexpr uint32_t kRows = 32;
    // Colour of progressive frame line `line` (448-line frame; field buffers hold every other line).
    auto lineColour = [](uint32_t line) {
        return 0x80000000u | ((line * 7u + 3u) & 0xFFu) | (((line * 13u) & 0xFFu) << 8);
    };
    // Field buffer: row k holds frame line 2k + fieldLine (fieldLine 0 = even field, 1 = odd field).
    auto upload = [&](writeback::Renderer& renderer, uint32_t fieldLine = 0) {
        std::vector<Qword> stream;
        stream.push_back(PackedTag(4, false, 0, {0xE}));
        stream.push_back(Qword{(uint64_t{1} << 16) | (uint64_t{1} << 48), 0x50});  // BITBLTBUF: DBP 0, DBW 1, CT32
        stream.push_back(Qword{0, 0x51});                                           // TRXPOS 0, 0
        stream.push_back(Qword{uint64_t{kWidth} | (uint64_t{kRows} << 32), 0x52});  // TRXREG 64 x 32
        stream.push_back(Qword{0, 0x53});                                           // TRXDIR 0: host -> local
        const uint32_t qwords = kWidth * kRows / 4;
        stream.push_back(PackedTag(qwords, true, 2, {}));
        for (uint32_t i = 0; i < qwords; ++i) {
            const uint32_t row = (i * 4) / kWidth;
            const uint32_t colour = lineColour(2 * row + fieldLine);
            stream.push_back(Qword{uint64_t{colour} | (uint64_t{colour} << 32), uint64_t{colour} | (uint64_t{colour} << 32)});
        }
        renderer.GifTransfer(1, reinterpret_cast<const uint8_t*>(stream.data()), stream.size() * sizeof(Qword));
    };
    auto setup = [&](writeback::Renderer& renderer, display::DeinterlaceMode mode) {
        writeback::OutputSettings settings;
        settings.deinterlace = mode;
        renderer.SetOutputSettings(settings);
        upload(renderer);
        renderer.WritePrivileged(0x12000000u, 1u | (1u << 5) | (0xFFu << 8));  // PMODE: RC1, MMOD 1, ALP 255
        renderer.WritePrivileged(0x12000010u, uint64_t{2} << 13);              // SMODE1: NTSC
        renderer.WritePrivileged(0x12000020u, 3u);                             // SMODE2: INT, FFMD
        renderer.WritePrivileged(0x12000060u, 1u);                             // SYNCV: interlaced signal (13.1)
        renderer.WritePrivileged(0x12000070u, uint64_t{1} << 9);               // DISPFB1: FBP 0, FBW 1, CT32
        renderer.WritePrivileged(0x12000080u, (uint64_t{3} << 23) | (uint64_t{4 * kWidth - 1} << 32) |
                                                  (uint64_t{2 * kRows - 1} << 44));  // DISPLAY1: MAGH 3, 64 x 64
    };
    auto frame = [](writeback::Renderer& renderer, bool odd, writeback::ComposeKind kind) {
        renderer.SetDisplayField(odd);
        renderer.Flush();
        return renderer.Compose(kind);
    };
    using writeback::ComposeKind;
    int failures = 0;
    auto check = [&](bool ok, const char* what) {
        std::printf("  %s: %s\n", what, ok ? "ok" : "FAILED");
        if (!ok) ++failures;
    };

    writeback::Renderer off;
    setup(off, display::DeinterlaceMode::Off);
    const writeback::FrameOutput refEven = frame(off, false, ComposeKind::Reference);
    const writeback::FrameOutput refOdd = frame(off, true, ComposeKind::Reference);
    const writeback::FrameOutput offEven = frame(off, false, ComposeKind::Presentation);
    const writeback::FrameOutput offOdd = frame(off, true, ComposeKind::Presentation);
    std::printf("  reference %ux%u valid %d\n", refEven.width, refEven.height, refEven.valid ? 1 : 0);
    check(refEven.valid && refEven.height >= 2 * kRows, "reference image exists and is full height");
    check(refEven.pixels == refOdd.pixels, "reference is independent of the field");
    check(offEven.pixels == refEven.pixels && offOdd.pixels == refEven.pixels, "deinterlace off presents the reference");
    check(off.GetCounters().deinterlacedFrames == 0 && off.GetCounters().interlaceOffsetFrames == 0,
          "deinterlace off moves no deinterlace counter");

    writeback::Renderer weave;
    setup(weave, display::DeinterlaceMode::Weave);
    frame(weave, false, ComposeKind::Presentation);
    const writeback::FrameOutput woven = frame(weave, true, ComposeKind::Presentation);
    check(woven.pixels == refEven.pixels, "weave of an even/odd pair rebuilds the line-doubled image");
    check(weave.GetCounters().interlaceOffsetFrames == 1 && weave.GetCounters().deinterlacedFrames == 2,
          "weave: one field offset (the CSR.FIELD = 0 field), two deinterlaced fields");

    // Field phase (IN-339): each field buffer holds its own lines, as GT4's field rendering does. The frame
    // whose display field is CSR.FIELD = 1 holds the upper lines (2k), the CSR.FIELD = 0 frame the lower
    // lines (2k + 1). Weaving the pair must give the progressive frame, line y = lineColour(y); the IN-35
    // reading (field 1 shifted down) puts every line two rows from its partner and fails here.
    writeback::Renderer phase;
    setup(phase, display::DeinterlaceMode::Weave);
    upload(phase, 1);
    frame(phase, false, ComposeKind::Presentation);
    upload(phase, 0);
    const writeback::FrameOutput progressive = frame(phase, true, ComposeKind::Presentation);
    bool progressiveOk = progressive.valid && progressive.height >= 2 * kRows && progressive.width >= kWidth;
    uint32_t wrongRows = 0;
    for (uint32_t y = 0; progressiveOk && y < 2 * kRows; ++y) {
        const uint32_t expected = lineColour(y) & 0x00FFFFFFu;
        const uint32_t got = progressive.pixels[static_cast<size_t>(y) * progressive.width] & 0x00FFFFFFu;
        if (got != expected) ++wrongRows;
    }
    std::printf("  progressive weave: %u of %u rows wrong\n", wrongRows, 2 * kRows);
    check(progressiveOk && wrongRows == 0, "weave of two field buffers gives the progressive frame");

    writeback::Renderer bob;
    setup(bob, display::DeinterlaceMode::Bob);
    // 13.4 bob shifts the row-0 field (CSR.FIELD = 1, IN-339) down by one row and leaves the other as is.
    const writeback::FrameOutput bobUpper = frame(bob, true, ComposeKind::Presentation);
    const writeback::FrameOutput bobLower = frame(bob, false, ComposeKind::Presentation);
    check(bobUpper.valid && bobUpper.pixels != refEven.pixels, "bob shifts the CSR.FIELD = 1 field by one row");
    check(bobLower.pixels == refEven.pixels, "bob leaves the CSR.FIELD = 0 field in place");

    std::printf("hw_presentation_field_phase: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}

// M7 gate (15.2: cache mode equals write-back mode), IN-340. A synthetic capture drives both sync modes of the
// GPU renderer through the same replay reader; every frame must give the same composed image and the same
// 4 MiB local memory hash. The frames exercise every coherence path of the resident targets:
//   - depth-tested draws with a separate depth buffer (colour and depth targets resident),
//   - a colour target switch (buffer A to buffer B) and back,
//   - a texture read from a target drawn in the same frame (feedback: written back before decoding),
//   - a host -> local upload into a drawn target (write back, then drop),
//   - a local -> local move out of a drawn target into another,
//   - a local -> host readback of a drawn target,
//   - a CLUT load from memory a draw just wrote, used by a T8 texture,
//   - a draw whose ZBUF shares the colour buffer's pages (the IN-335 case: write-back fallback).
// It also proves the cache is in use (resident batches, loads, write-backs) so a silently disabled cache fails.
int TestCacheEqualsWriteback(const gpu::VulkanHandles& handles) {
    constexpr uint32_t kFrames = 5;
    constexpr uint64_t kA = 0, kB = 2, kZ = 8, kT8 = 12, kClut = 14;  // pages
    auto rgbaq = [](uint32_t colour) { return uint64_t{colour} | (uint64_t{0x3F800000u} << 32); };
    auto xyz = [](uint32_t x, uint32_t y, uint32_t z = 0) {
        return uint64_t{x * 16} | (uint64_t{y * 16} << 16) | (uint64_t{z} << 32);
    };
    auto uv = [](uint32_t u, uint32_t v) { return uint64_t{u * 16} | (uint64_t{v * 16} << 16); };
    auto frameReg = [](uint64_t page) { return page | (uint64_t{1} << 16); };  // FBW 1, CT32
    auto zbufReg = [](uint64_t page, bool masked) { return page | (masked ? (uint64_t{1} << 32) : 0); };  // Z32
    auto tex0 = [](uint64_t blocks, uint64_t psm, uint64_t log2, uint64_t cbpBlocks, bool load) {
        return blocks | (uint64_t{1} << 14) | (psm << 20) | (log2 << 26) | (log2 << 30) | (uint64_t{1} << 34) |
               (cbpBlocks << 37) | (uint64_t{load ? 1u : 0u} << 61);  // TBW 1, TCC, TFX modulate, CPSM CT32, CSM1
    };
    const uint64_t kSprite = 6, kTriangle = 3, kIip = 1u << 3, kTme = 1u << 4, kFst = 1u << 8;

    cpuref::CaptureWriter capture;
    capture.Privileged(0x12000000u, 1u | (1u << 5) | (0xFFu << 8));                // PMODE: RC1, MMOD 1, ALP 255
    capture.Privileged(0x12000010u, uint64_t{2} << 13);                             // SMODE1: NTSC
    capture.Privileged(0x12000070u, uint64_t{kA * 32} | (uint64_t{1} << 9));        // DISPFB1: buffer A
    capture.Privileged(0x12000080u, (uint64_t{63} << 32) | (uint64_t{63} << 44));  // DISPLAY1: 64 x 64
    auto ad = [&](const std::vector<uint64_t>& words) {
        capture.Gif(1, cpuref::CaptureWriter::Packet(static_cast<uint32_t>(words.size() / 2), true, false, 0, 0, {0xE},
                                                     words));
    };
    auto image = [&](const std::vector<uint64_t>& data) {
        capture.Gif(1, cpuref::CaptureWriter::Packet(static_cast<uint32_t>(data.size() / 2), true, false, 0, 2, {}, data));
    };
    auto blit = [](uint64_t sbp, uint64_t spsm, uint64_t dbp, uint64_t dpsm) {
        return sbp | (uint64_t{1} << 16) | (spsm << 24) | (dbp << 32) | (uint64_t{1} << 48) | (dpsm << 56);
    };
    // Common state: scissor 0..63, no offset, depth test GEQUAL with writes (separate Z buffer).
    const std::vector<uint64_t> common = {(uint64_t{63} << 16) | (uint64_t{63} << 48), 0x40, 0, 0x18,
                                          (uint64_t{1} << 16) | (uint64_t{2} << 17), 0x47,  // TEST: ZTE, GEQUAL
                                          0, 0x42};                                          // ALPHA_1 (unused)
    // T8 texture data (64 x 64, one page) uploaded once: index = (x + 3y) & 255.
    {
        std::vector<uint64_t> words = {blit(0, 0, kT8 * 32, 0x13), 0x50, 0, 0x51, uint64_t{64} | (uint64_t{64} << 32), 0x52,
                                       0, 0x53};
        ad(words);
        std::vector<uint64_t> data;
        for (uint32_t i = 0; i < 64 * 64 / 8; ++i) {
            uint64_t word = 0;
            for (uint32_t b = 0; b < 8; ++b) {
                const uint32_t p = i * 8 + b;
                word |= uint64_t{((p % 64) + 3 * (p / 64)) & 255u} << (8 * b);
            }
            data.push_back(word);
        }
        image(data);
    }
    for (uint32_t frame = 0; frame < kFrames; ++frame) {
        std::vector<uint64_t> w = common;
        // 1. Buffer A, depth buffer Z: a clear sprite and two depth-tested Gouraud triangles.
        w.insert(w.end(), {frameReg(kA), 0x4C, zbufReg(kZ, false), 0x4E, kSprite, 0x00,
                           rgbaq(0x80102030u + frame), 0x01, xyz(0, 0, 0), 0x05, xyz(64, 64, 0), 0x05,
                           kTriangle | kIip, 0x00, rgbaq(0x80FF0000u), 0x01, xyz(3, 2, 500 + frame), 0x05,
                           rgbaq(0x8000FF00u), 0x01, xyz(60, 10, 900), 0x05, rgbaq(0x800000FFu), 0x01,
                           xyz(20 + frame, 61, 700), 0x05});
        // 2. Buffer B: clear, then a sprite textured from buffer A (drawn above, this frame).
        w.insert(w.end(), {frameReg(kB), 0x4C, zbufReg(kZ, true), 0x4E, kSprite, 0x00, rgbaq(0x80405060u), 0x01,
                           xyz(0, 0), 0x05, xyz(64, 64), 0x05, tex0(kA * 32, 0x00, 6, 0, false), 0x06, 0, 0x08,
                           kSprite | kTme | kFst, 0x00, rgbaq(0x80808080u), 0x01, uv(0, 0), 0x03, xyz(8, 8), 0x05,
                           uv(48, 48), 0x03, xyz(56, 56), 0x05});
        // 3. A palette drawn into memory (CT32 16 x 16 at page kClut), then a T8 texture using it, into A.
        w.insert(w.end(), {frameReg(kClut), 0x4C, kSprite | kIip, 0x00, rgbaq(0x80000000u | (frame * 0x111111u)), 0x01,
                           xyz(0, 0), 0x05, rgbaq(0x80FFFFFFu - frame), 0x01, xyz(16, 16), 0x05,
                           frameReg(kA), 0x4C, tex0(kT8 * 32, 0x13, 6, kClut * 32, true), 0x06,
                           kSprite | kTme | kFst, 0x00, rgbaq(0x80808080u), 0x01, uv(0, 0), 0x03, xyz(30, 30), 0x05,
                           uv(30, 30), 0x03, xyz(60, 60), 0x05});
        // 3b. Eighteen more small colour targets (pages 20, 24, ..., 44, then 52, 56, ..., 92, around page 48 used
        //     below): more targets than resident slots (IN-400: 15), so the least recently used ones are evicted
        //     (written back) and reloaded; then one is read as a texture.
        for (uint64_t k = 0; k < 18; ++k) {
            const uint64_t page = k < 7 ? 20 + 4 * k : 52 + 4 * (k - 7);
            w.insert(w.end(), {frameReg(page), 0x4C, kSprite, 0x00, rgbaq(0x80000000u + 0x0F0F0Fu * (k + 1) + frame),
                               0x01, xyz(2 * k, 1), 0x05, xyz(20 + 2 * k, 20), 0x05});
        }
        w.insert(w.end(), {frameReg(kA), 0x4C, tex0(20 * 32, 0x00, 5, 0, false), 0x06, kSprite | kTme | kFst, 0x00,
                           rgbaq(0x80808080u), 0x01, uv(0, 0), 0x03, xyz(40, 40), 0x05, uv(24, 24), 0x03,
                           xyz(64, 64), 0x05});
        // 3c. The same texture again after a state change (blending on, PRIM.ABE): a new batch whose GLM decode
        //     equals the previous one (IN-369 decode reuse).
        w.insert(w.end(), {kSprite | kTme | kFst | (uint64_t{1} << 6), 0x00, rgbaq(0x80808080u), 0x01, uv(0, 0), 0x03,
                           xyz(0, 16), 0x05, uv(24, 14), 0x03, xyz(24, 30), 0x05});
        ad(w);
        // 3d. An upload into that texture's page, then the texture once more (blending off again): the decode must
        //     not be reused, since the GLM changed in between (IN-369).
        ad({blit(0, 0, 20 * 32, 0x00), 0x50, uint64_t{0}, 0x51, uint64_t{8} | (uint64_t{8} << 32), 0x52, 0, 0x53});
        {
            std::vector<uint64_t> texels;
            for (uint32_t i = 0; i < 32; ++i) texels.push_back((uint64_t{0x80A0B0C0u ^ (i * 0x01030507u)} << 32) | (0x80405060u + i + frame));
            image(texels);
        }
        ad({kSprite | kTme | kFst, 0x00, rgbaq(0x80808080u), 0x01, uv(0, 0), 0x03, xyz(54, 22), 0x05, uv(10, 10), 0x03,
            xyz(64, 32), 0x05});
        // 3e. Deferred CLUT loads (IN-370), depth test ALWAYS. (1) A T4 load from the palette page with CSA 0, then
        //     an upload over that palette before the draw: the draw must use the palette as loaded (the load is
        //     recorded before the upload writes the GLM). (2) A T4 load of the uploaded palette with CSA 1 and a draw
        //     (palette read from the GLM). (3) A CSA 0 draw without a load: entries from load (1), CLUT buffer path.
        {
            auto t4 = [&](uint64_t cbpBlocks, uint64_t csa, bool load) {
                return tex0(kT8 * 32, 0x14, 4, cbpBlocks, load) | (csa << 56);
            };
            auto sprite = [&](uint32_t x0) {
                return std::vector<uint64_t>{kSprite | kTme | kFst, 0x00, rgbaq(0x80808080u), 0x01, uv(0, 0), 0x03,
                                             xyz(x0, 16), 0x05, uv(16, 16), 0x03, xyz(x0 + 8, 28), 0x05};
            };
            ad({frameReg(kA), 0x4C, (uint64_t{1} << 16) | (uint64_t{1} << 17), 0x47, t4(kClut * 32, 0, true), 0x06});
            ad({blit(0, 0, kClut * 32, 0x00), 0x50, uint64_t{0}, 0x51, uint64_t{8} | (uint64_t{2} << 32), 0x52, 0, 0x53});
            std::vector<uint64_t> palette;
            for (uint32_t i = 0; i < 8; ++i) {
                palette.push_back((uint64_t{0x80F00000u + 0x1020u * i + frame} << 32) | (0x800000F0u + 0x10200u * i));
            }
            image(palette);
            ad(sprite(26));
            std::vector<uint64_t> second = {t4(kClut * 32, 1, true), 0x06};
            for (uint64_t word : sprite(34)) second.push_back(word);
            ad(second);
            std::vector<uint64_t> third = {t4(kClut * 32, 0, false), 0x06};
            for (uint64_t word : sprite(42)) third.push_back(word);
            third.push_back((uint64_t{1} << 16) | (uint64_t{2} << 17));  // TEST back to GEQUAL
            third.push_back(0x47);
            ad(third);
        }
        // 4. Host -> local: 8 x 8 CT32 block into buffer A at (4 + frame, 4).
        ad({blit(0, 0, kA * 32, 0x00), 0x50, (uint64_t{4 + frame} << 32) | (uint64_t{4} << 48), 0x51,
            uint64_t{8} | (uint64_t{8} << 32), 0x52, 0, 0x53});
        std::vector<uint64_t> block;
        for (uint32_t i = 0; i < 32; ++i) block.push_back((uint64_t{0x11223344u + i + frame} << 32) | (0x55667788u - i));
        image(block);
        // 5. Local -> local: A (0,0) 16 x 16 to B (40, 40).
        ad({blit(kA * 32, 0x00, kB * 32, 0x00), 0x50, uint64_t{0} | (uint64_t{40} << 32) | (uint64_t{40} << 48), 0x51,
            uint64_t{16} | (uint64_t{16} << 32), 0x52, 2, 0x53});
        // 6. Local -> host: 8 x 8 of B at (36, 36) (drawn by the textured sprite).
        ad({blit(kB * 32, 0x00, 0, 0x00), 0x50, uint64_t{36} | (uint64_t{36} << 16), 0x51,
            uint64_t{8} | (uint64_t{8} << 32), 0x52, 1, 0x53});
        capture.FifoRead(16);
        // 7. A draw into A whose ZBUF is A itself with Z written (test ALWAYS): the write-back fallback.
        ad({frameReg(kA), 0x4C, zbufReg(kA, false), 0x4E, uint64_t{1} << 16 | (uint64_t{1} << 17), 0x47, kSprite, 0x00,
            rgbaq(0x80EEDDCCu), 0x01, xyz(50, 2), 0x05, xyz(62, 12), 0x05});
        // 8. GT4's buffer reuse (IN-350): page 48 drawn 128 wide (FBW 2), then 64 wide at the same base (the first
        //    target is parked in the GLM and the second loads from it), then partial CT16 and T4 uploads into it
        //    (merged into the GLM-owned pages), then read as a CT32 texture into A.
        ad({(uint64_t{1} << 16) | (uint64_t{2} << 17), 0x47, frameReg(48) | (uint64_t{1} << 16), 0x4C,
            zbufReg(kZ, true), 0x4E, kSprite, 0x00, rgbaq(0x80123456u + frame), 0x01, xyz(0, 0), 0x05, xyz(64, 40), 0x05,
            frameReg(48), 0x4C, kSprite, 0x00, rgbaq(0x80654321u), 0x01, xyz(5 + frame, 3), 0x05, xyz(30, 50), 0x05});
        ad({blit(0, 0, 48 * 32, 0x02), 0x50, (uint64_t{10} << 32) | (uint64_t{3 + frame} << 48), 0x51,
            uint64_t{8} | (uint64_t{4} << 32), 0x52, 0, 0x53});
        image({0x1111222233334444ull + frame, 0x5555666677778888ull, 0x99990000AAAABBBBull, 0xCCCCDDDDEEEEFFFFull,
               0x0123456789ABCDEFull, 0xFEDCBA9876543210ull, 0x0F0F0F0FF0F0F0F0ull, 0x1357924680ACE13Bull});
        ad({blit(0, 0, 48 * 32, 0x14), 0x50, (uint64_t{6} << 32) | (uint64_t{40} << 48), 0x51,
            uint64_t{8} | (uint64_t{4} << 32), 0x52, 0, 0x53});
        image({0x89ABCDEF01234567ull ^ frame, 0x76543210FEDCBA98ull});
        ad({frameReg(kA), 0x4C, tex0(48 * 32, 0x00, 6, 0, false), 0x06, kSprite | kTme | kFst, 0x00,
            rgbaq(0x80808080u), 0x01, uv(0, 0), 0x03, xyz(0, 32), 0x05, uv(64, 32), 0x03, xyz(64, 64), 0x05});
        capture.FrameEnd(frame);
    }

    struct FrameRecord {
        bool valid = false;
        std::vector<uint8_t> rgb;
        uint64_t slmHash = 0;
        std::vector<uint8_t> slm;
    };
    uint64_t decodesReused = 0;  // IN-369, cache run
    uint64_t decodesGlmPalette = 0;  // IN-370, cache run
    uint64_t clutMaterialisations = 0;
    auto replay = [&](bool cacheMode, bool cpuSprites, std::vector<FrameRecord>& out, writeback::Counters& counters,
                      std::string& error) -> bool {
        writeback::Renderer renderer;
        if (!renderer.Init(handles, error)) return false;
        renderer.SetSyncMode(cacheMode);
        renderer.SetCpuSmallSprites(cpuSprites);
        renderer.SetGlmLoadsAlways(cpuSprites);  // the second cache run loads every slot through the GLM (IN-350)
        renderer.SetGpuUploads(!cpuSprites);     // ... and writes uploads on the CPU (GLM merges, IN-350/351)
        writeback::WritebackReplayTarget target(renderer);
        cpuref::ReplaySummary summary;
        const bool ok = cpuref::ReplayCapture(capture.Bytes(), target, summary,
                                              [&](uint64_t, const cpuref::FrameImage& image) {
                                                  FrameRecord r;
                                                  r.valid = image.valid;
                                                  for (const display::Rgba8& p : image.image.pixels) {
                                                      r.rgb.push_back(p.r);
                                                      r.rgb.push_back(p.g);
                                                      r.rgb.push_back(p.b);
                                                  }
                                                  const uint8_t* slm = renderer.Slm();
                                                  r.slmHash = cpuref::HashLocalMemory(slm, 4u * 1024u * 1024u);
                                                  r.slm.assign(slm, slm + 4u * 1024u * 1024u);
                                                  out.push_back(std::move(r));
                                              },
                                              error);
        counters = renderer.GetCounters();
        if (cacheMode && !cpuSprites) {
            decodesReused = renderer.GpuCounters().decodesReused;
            decodesGlmPalette = renderer.GpuCounters().decodesGlmPalette;
            clutMaterialisations = renderer.GpuCounters().clutMaterialisations;
        }
        if (renderer.Failed()) error += " " + renderer.LastError();
        return ok && !renderer.Failed();
    };
    // The cache run without the CPU sprite path (IN-348) keeps every small target on the GPU (evictions); the
    // second cache run with it must give the same images and memory too.
    std::vector<FrameRecord> reference;
    std::vector<FrameRecord> cached;
    std::vector<FrameRecord> cachedCpuSprites;
    writeback::Counters referenceCounters;
    writeback::Counters cacheCounters;
    writeback::Counters cpuSpriteCounters;
    std::string error;
    if (!replay(false, false, reference, referenceCounters, error) || !replay(true, false, cached, cacheCounters, error) ||
        !replay(true, true, cachedCpuSprites, cpuSpriteCounters, error)) {
        std::printf("hw_cache_equals_writeback: replay failed: %s\n", error.c_str());
        return 1;
    }
    int failures = 0;
    if (reference.size() != kFrames || cached.size() != kFrames) {
        std::printf("hw_cache_equals_writeback: frame counts %zu / %zu\n", reference.size(), cached.size());
        return 1;
    }
    for (uint32_t frame = 0; frame < kFrames; ++frame) {
        const bool image = reference[frame].valid == cached[frame].valid && reference[frame].rgb == cached[frame].rgb;
        const bool memory = reference[frame].slmHash == cached[frame].slmHash &&
                            cachedCpuSprites.size() == kFrames &&
                            reference[frame].slmHash == cachedCpuSprites[frame].slmHash &&
                            reference[frame].rgb == cachedCpuSprites[frame].rgb;
        std::printf("  frame %u: image %s, memory %016llx / %016llx %s\n", frame, image ? "equal" : "DIFFERENT",
                    static_cast<unsigned long long>(reference[frame].slmHash),
                    static_cast<unsigned long long>(cached[frame].slmHash), memory ? "equal" : "DIFFERENT");
        if (!memory) {
            // Name the 8 KiB GS pages that differ, so a failure points at the buffer involved.
            std::string pages;
            for (uint32_t page = 0; page < 512; ++page) {
                const size_t base = static_cast<size_t>(page) * 8192u;
                if (std::memcmp(reference[frame].slm.data() + base, cached[frame].slm.data() + base, 8192) != 0) {
                    pages += " " + std::to_string(page);
                }
            }
            std::printf("    differing pages:%s\n", pages.c_str());
        }
        if (!image || !memory) ++failures;
        if (frame > 0 && reference[frame].slmHash == reference[frame - 1].slmHash) {
            std::printf("  frame %u left memory unchanged (fixture is blind)\n", frame);
            ++failures;
        }
    }
    std::printf("  cache: resident %llu, fallback %llu, loads %llu, write-backs %llu\n",
                static_cast<unsigned long long>(cacheCounters.cacheResidentBatches),
                static_cast<unsigned long long>(cacheCounters.cacheFallbackBatches),
                static_cast<unsigned long long>(cacheCounters.cacheLoads),
                static_cast<unsigned long long>(cacheCounters.cacheWritebacks));
    std::printf("  cache: GLM upload merges %llu (%llu words, CPU-upload run), GPU host uploads %llu\n",
                static_cast<unsigned long long>(cpuSpriteCounters.glmUploadMerges),
                static_cast<unsigned long long>(cpuSpriteCounters.glmMergedWords),
                static_cast<unsigned long long>(cacheCounters.glmHostUploads));
    std::printf("  cache: GLM parks %llu, GLM slot loads %llu / %llu (always), GLM downloads %llu\n",
                static_cast<unsigned long long>(cacheCounters.glmParks),
                static_cast<unsigned long long>(cacheCounters.glmSlotLoads),
                static_cast<unsigned long long>(cpuSpriteCounters.glmSlotLoads),
                static_cast<unsigned long long>(cacheCounters.glmDownloads));
    std::printf("  cache: GLM decodes reused %llu, palettes from the GLM %llu, CLUT materialisations %llu\n",
                static_cast<unsigned long long>(decodesReused), static_cast<unsigned long long>(decodesGlmPalette),
                static_cast<unsigned long long>(clutMaterialisations));
    std::printf("  cache: evictions %llu, GPU CLUT loads %llu, GLM textures %llu, CPU sprite batches %llu\n",
                static_cast<unsigned long long>(cacheCounters.cacheEvictions),
                static_cast<unsigned long long>(cacheCounters.gpuClutLoads),
                static_cast<unsigned long long>(cacheCounters.texturesFromGlm),
                static_cast<unsigned long long>(cpuSpriteCounters.cpuSpriteBatches));
    if (cacheCounters.cacheResidentBatches == 0 || cacheCounters.cacheFallbackBatches == 0 ||
        cacheCounters.cacheLoads == 0 || cacheCounters.cacheWritebacks == 0 || cacheCounters.cacheEvictions == 0 ||
        referenceCounters.cacheResidentBatches != 0 || cpuSpriteCounters.cpuSpriteBatches == 0 ||
        cacheCounters.gpuClutLoads == 0 || cacheCounters.texturesFromGlm == 0 || cacheCounters.glmParks == 0 ||
        cacheCounters.glmSlotLoads == 0 || cacheCounters.glmDownloads == 0 || cpuSpriteCounters.glmUploadMerges == 0 || cacheCounters.glmBatchedWritebacks == 0 ||
        cacheCounters.glmHostUploads == 0 ||
        (handles.pixelInterlock && decodesReused == 0) ||  // runs (and their decode reuse) need interlock
        decodesGlmPalette == 0 || clutMaterialisations == 0 ||
        cpuSpriteCounters.glmSlotLoads < cpuSpriteCounters.cacheLoads) {
        std::printf("  the cache paths were not all exercised (fixture is blind)\n");
        ++failures;
    }
    // IN-355: presentation from the asynchronous capture equals the synchronous presentation one VBlank later
    // (nothing is shown at the first VBlank), and memory is unchanged by it.
    {
        uint64_t gpuComposeEqual = 0;
        uint64_t gpuComposeDifferent = 0;
        auto present = [&](bool async, std::vector<FrameRecord>& out, writeback::Counters& counters,
                           std::string& error) -> bool {
            writeback::Renderer renderer;
            if (!renderer.Init(handles, error)) return false;
            renderer.SetSyncMode(true);
            renderer.SetAsyncPresentation(async);
            renderer.SetGpuCompose(!async);  // the async run checks the CPU capture path (interlaced frames' path)
            writeback::WritebackReplayTarget target(renderer, writeback::ComposeKind::Presentation);
            target.SetGpuComposeCheck(!async);  // IN-364: the synchronous run also composes every frame on the GPU
            cpuref::ReplaySummary summary;
            const bool ok = cpuref::ReplayCapture(capture.Bytes(), target, summary,
                                                  [&](uint64_t, const cpuref::FrameImage& image) {
                                                      FrameRecord r;
                                                      r.valid = image.valid;
                                                      for (const display::Rgba8& p : image.image.pixels) {
                                                          r.rgb.push_back(p.r);
                                                          r.rgb.push_back(p.g);
                                                          r.rgb.push_back(p.b);
                                                      }
                                                      r.slmHash = cpuref::HashLocalMemory(renderer.Slm(),
                                                                                          4u * 1024u * 1024u);
                                                      out.push_back(std::move(r));
                                                  },
                                                  error);
            counters = renderer.GetCounters();
            if (!async) {
                gpuComposeEqual = target.GpuComposeEqual();
                gpuComposeDifferent = target.GpuComposeDifferent();
            }
            if (renderer.Failed()) error += " " + renderer.LastError();
            return ok && !renderer.Failed();
        };
        std::vector<FrameRecord> sync;
        std::vector<FrameRecord> async;
        writeback::Counters syncCounters;
        writeback::Counters asyncCounters;
        if (!present(false, sync, syncCounters, error) || !present(true, async, asyncCounters, error)) {
            std::printf("  async presentation: replay failed: %s\n", error.c_str());
            ++failures;
        } else if (sync.size() != kFrames || async.size() != kFrames) {
            std::printf("  async presentation: frame counts %zu / %zu\n", sync.size(), async.size());
            ++failures;
        } else {
            if (async[0].valid) {
                std::printf("  async presentation: frame 0 shows an image (expected none)\n");
                ++failures;
            }
            for (uint32_t frame = 0; frame < kFrames; ++frame) {
                const bool memory = sync[frame].slmHash == async[frame].slmHash;
                const bool image = frame == 0 || (async[frame].valid == sync[frame - 1].valid &&
                                                  async[frame].rgb == sync[frame - 1].rgb);
                std::printf("  async presentation frame %u: image %s previous VBlank's, memory %s\n", frame,
                            image ? "equals" : "DIFFERS from", memory ? "equal" : "DIFFERS");
                if (!image || !memory) ++failures;
            }
            std::printf("  GPU compose (IN-364): %llu frames equal to the CPU compose, %llu different\n",
                        static_cast<unsigned long long>(gpuComposeEqual),
                        static_cast<unsigned long long>(gpuComposeDifferent));
            if (gpuComposeEqual != kFrames || gpuComposeDifferent != 0) ++failures;
            uint32_t changing = 0;  // the shift is only visible when consecutive frames differ
            for (uint32_t frame = 1; frame < kFrames; ++frame) changing += sync[frame].rgb != sync[frame - 1].rgb ? 1u : 0u;
            if (changing < kFrames - 1 || asyncCounters.displayCaptures == 0 ||
                asyncCounters.presentationsFromCapture + 1 != kFrames ||
                syncCounters.presentationsFromCapture != 0) {
                std::printf("  async presentation: captures %llu, composes from captures %llu (fixture is blind)\n",
                            static_cast<unsigned long long>(asyncCounters.displayCaptures),
                            static_cast<unsigned long long>(asyncCounters.presentationsFromCapture));
                ++failures;
            }
        }
    }
    // IN-371/IN-372 (chapter 06): the same capture at an internal scale of 3. The 1x words stay the only authority,
    // so every frame's memory and 1x reference image equal the 1x cache run; the scaled GPU compose is 3x the size
    // and its 06.7 box downsample stays close to the 1x image (06.9 metric; exact equality is not expected where
    // triangles and filtered textures cover sub-pixels differently).
    {
        writeback::Renderer renderer;
        renderer.SetScale(3, 3.0f);
        std::vector<FrameRecord> scaledRun;
        std::vector<double> psnrs;
        uint32_t composed = 0;
        bool ok = renderer.Init(handles, error);
        if (ok) {
            renderer.SetSyncMode(true);
            writeback::WritebackReplayTarget target(renderer, writeback::ComposeKind::Reference);
            cpuref::ReplaySummary summary;
            ok = cpuref::ReplayCapture(
                capture.Bytes(), target, summary,
                [&](uint64_t, const cpuref::FrameImage& image) {
                    FrameRecord r;
                    r.valid = image.valid;
                    for (const display::Rgba8& p : image.image.pixels) {
                        r.rgb.push_back(p.r);
                        r.rgb.push_back(p.g);
                        r.rgb.push_back(p.b);
                    }
                    r.slmHash = cpuref::HashLocalMemory(renderer.Slm(), 4u * 1024u * 1024u);
                    writeback::GpuFrame gpuFrame;
                    std::vector<uint32_t> pixels;
                    if (image.valid && renderer.ComposeFrameGpu(gpuFrame) && renderer.ReadGpuFrame(gpuFrame, pixels) &&
                        gpuFrame.width == 3 * image.image.width && gpuFrame.height == 3 * image.image.height) {
                        ++composed;
                        double squared = 0.0;
                        for (uint32_t y = 0; y < image.image.height; ++y) {
                            for (uint32_t x = 0; x < image.image.width; ++x) {
                                uint32_t sums[3] = {0, 0, 0};
                                for (uint32_t dy = 0; dy < 3; ++dy) {
                                    for (uint32_t dx = 0; dx < 3; ++dx) {
                                        const uint32_t p = pixels[(y * 3 + dy) * gpuFrame.width + x * 3 + dx];
                                        sums[0] += p & 0xFFu;
                                        sums[1] += (p >> 8) & 0xFFu;
                                        sums[2] += (p >> 16) & 0xFFu;
                                    }
                                }
                                const display::Rgba8& native = image.image.at(x, y);
                                const int boxed[3] = {static_cast<int>((sums[0] + 4) / 9), static_cast<int>((sums[1] + 4) / 9),
                                                      static_cast<int>((sums[2] + 4) / 9)};
                                const int expected[3] = {native.r, native.g, native.b};
                                for (int c = 0; c < 3; ++c) squared += double(boxed[c] - expected[c]) * (boxed[c] - expected[c]);
                            }
                        }
                        const double mse = squared / (3.0 * image.image.width * image.image.height);
                        psnrs.push_back(mse == 0.0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse));
                    }
                    scaledRun.push_back(std::move(r));
                },
                error);
            if (renderer.Failed()) error += " " + renderer.LastError();
            ok = ok && !renderer.Failed();
        }
        if (!ok || scaledRun.size() != kFrames) {
            std::printf("  scale 3: replay failed or %zu frames: %s\n", scaledRun.size(), error.c_str());
            ++failures;
        } else {
            for (uint32_t frame = 0; frame < kFrames; ++frame) {
                const bool same = scaledRun[frame].slmHash == cached[frame].slmHash && scaledRun[frame].rgb == cached[frame].rgb;
                if (!same) {
                    std::printf("  scale 3 frame %u: memory or 1x image DIFFERS from the 1x cache run\n", frame);
                    ++failures;
                }
            }
            const gpu::GpuStats& stats = renderer.GpuCounters();
            const writeback::Counters& c = renderer.GetCounters();
            double worst = 99.0;
            for (double value : psnrs) {
                worst = value < worst ? value : worst;
                std::printf("  scale 3: box PSNR %.2f dB\n", value);
            }
            std::printf("  scale 3: memory and 1x images equal to the 1x run; %u scaled composes, worst box PSNR %.2f dB; "
                        "scaled draws %llu, refreshes %llu, circuits from slots %llu\n",
                        composed, worst, static_cast<unsigned long long>(stats.scaledDraws),
                        static_cast<unsigned long long>(stats.scaledRefreshes),
                        static_cast<unsigned long long>(c.scaledCircuitsFromSlots));
            // This capture samples a noise-like T8 pattern and target copies with U = X (no texel-centre offset), so
            // sub-pixels sample neighbouring texels at scale and the box average differs from 1x (about 29 dB at
            // 3x, recorded in IN-372); hw_scale_exact checks the exact cases. Below 25 dB means a real defect.
            if (composed != kFrames || worst < 25.0 || stats.scaledDraws == 0 || stats.scaledRefreshes == 0 ||
                c.scaledCircuitsFromSlots == 0) {
                std::printf("  scale 3: composes, PSNR or scaled paths outside the expectation\n");
                ++failures;
            }
        }
    }
    std::printf("hw_cache_equals_writeback: %u frames, %d failures\n", kFrames, failures);
    return failures == 0 ? 0 : 1;
}

// hw_scale_exact (IN-371..IN-374, chapter 06): flat sprites on native pixel edges into buffer A, then A copied 1:1
// into buffer B as a nearest-sampled texture with the texel-centre coordinates U = X + 0.5 (the texture comes from
// the resident slot, so the scaled pass samples A's scaled image), and B displayed. At scales 2, 3 and 4 such content
// has an exact answer: memory equals the 1x run, every s x s block of the scaled compose is uniform (no seams, no
// half-pixel shift, no edge halos) and equals the 1x pixel, so the 06.7 box downsample equals the 1x image exactly.
int TestScaleExact(const gpu::VulkanHandles& handles) {
    auto rgbaq = [](uint32_t colour) { return uint64_t{colour} | (uint64_t{0x3F800000u} << 32); };
    auto xy = [](uint32_t x, uint32_t y) { return uint64_t{x * 16} | (uint64_t{y * 16} << 16); };
    auto uvRaw = [](uint32_t u16, uint32_t v16) { return uint64_t{u16} | (uint64_t{v16} << 16); };
    auto frameReg = [](uint64_t page) { return page | (uint64_t{1} << 16); };  // FBW 1, CT32
    const uint64_t kSprite = 6, kTme = 1u << 4, kFst = 1u << 8, kA = 0, kB = 2, kZ = 8;
    cpuref::CaptureWriter capture;
    capture.Privileged(0x12000000u, 1u | (1u << 5) | (0xFFu << 8));                // PMODE: RC1, MMOD 1, ALP 255
    capture.Privileged(0x12000010u, uint64_t{2} << 13);                             // SMODE1: NTSC
    capture.Privileged(0x12000070u, uint64_t{kB} | (uint64_t{1} << 9));        // DISPFB1: buffer B (FBP in pages)
    capture.Privileged(0x12000080u, (uint64_t{3} << 23) | (uint64_t{255} << 32) | (uint64_t{63} << 44));  // 64 x 64
    auto ad = [&](const std::vector<uint64_t>& words) {
        capture.Gif(1, cpuref::CaptureWriter::Packet(static_cast<uint32_t>(words.size() / 2), true, false, 0, 0, {0xE},
                                                     words));
    };
    constexpr uint32_t kFrames = 3;
    for (uint32_t frame = 0; frame < kFrames; ++frame) {
        ad({(uint64_t{63} << 16) | (uint64_t{63} << 48), 0x40, 0, 0x18,
            (uint64_t{1} << 16) | (uint64_t{1} << 17), 0x47,  // TEST: ZTE, ALWAYS
            frameReg(kA), 0x4C, kZ | (uint64_t{1} << 32), 0x4E,  // ZBUF masked
            kSprite, 0x00, rgbaq(0x80102030u + frame), 0x01, xy(0, 0), 0x05, xy(64, 64), 0x05,
            kSprite, 0x00, rgbaq(0x80E04010u), 0x01, xy(5 + frame, 7), 0x05, xy(20, 30), 0x05,
            kSprite, 0x00, rgbaq(0x8010E040u), 0x01, xy(33, 2), 0x05, xy(60, 9 + frame), 0x05,
            kSprite, 0x00, rgbaq(0x804010E0u), 0x01, xy(10, 40), 0x05, xy(50, 61), 0x05,
            frameReg(kB), 0x4C,
            uint64_t{kA * 32} | (uint64_t{1} << 14) | (uint64_t{6} << 26) | (uint64_t{6} << 30) | (uint64_t{1} << 34) |
                (uint64_t{1} << 35),
            0x06,  // TEX0: A, TBW 1, CT32, 64 x 64, TCC, TFX decal
            0, 0x14,  // TEX1_1: nearest
            kSprite | kTme | kFst, 0x00, rgbaq(0x80808080u), 0x01, uvRaw(8, 8), 0x03, xy(0, 0), 0x05,
            uvRaw(64 * 16 + 8, 64 * 16 + 8), 0x03, xy(64, 64), 0x05});
        capture.FrameEnd(frame);
    }

    struct Run {
        std::vector<uint64_t> slm;
        std::vector<std::vector<uint8_t>> native;    // 1x reference images
        std::vector<std::vector<uint32_t>> scaled;   // scaled GPU composes
        std::vector<uint32_t> width, height;
    };
    auto replay = [&](uint32_t scale, Run& run, gpu::GpuStats& stats, writeback::Counters& counters,
                      std::string& error) -> bool {
        writeback::Renderer renderer;
        renderer.SetScale(scale, static_cast<float>(scale));
        if (!renderer.Init(handles, error)) return false;
        renderer.SetSyncMode(true);
        renderer.SetCpuSmallSprites(false);  // the flat sprites go through the GPU (and its scaled pass)
        writeback::WritebackReplayTarget target(renderer, writeback::ComposeKind::Reference);
        cpuref::ReplaySummary summary;
        const bool ok = cpuref::ReplayCapture(capture.Bytes(), target, summary,
                                              [&](uint64_t, const cpuref::FrameImage& image) {
                                                  std::vector<uint8_t> rgb;
                                                  for (const display::Rgba8& p : image.image.pixels) {
                                                      rgb.push_back(p.r);
                                                      rgb.push_back(p.g);
                                                      rgb.push_back(p.b);
                                                  }
                                                  run.native.push_back(rgb);
                                                  run.width.push_back(image.image.width);
                                                  run.height.push_back(image.image.height);
                                                  run.slm.push_back(cpuref::HashLocalMemory(renderer.Slm(), 4u * 1024u * 1024u));
                                                  writeback::GpuFrame gpuFrame;
                                                  std::vector<uint32_t> pixels;
                                                  if (renderer.ComposeFrameGpu(gpuFrame) &&
                                                      renderer.ReadGpuFrame(gpuFrame, pixels) &&
                                                      gpuFrame.width == scale * image.image.width &&
                                                      gpuFrame.height == scale * image.image.height) {
                                                      run.scaled.push_back(std::move(pixels));
                                                  } else {
                                                      run.scaled.emplace_back();
                                                  }
                                              },
                                              error);
        stats = renderer.GpuCounters();
        counters = renderer.GetCounters();
        if (renderer.Failed()) error += " " + renderer.LastError();
        return ok && !renderer.Failed();
    };
    Run base;
    gpu::GpuStats baseStats;
    writeback::Counters baseCounters;
    std::string error;
    if (!replay(1, base, baseStats, baseCounters, error) || base.native.size() != kFrames) {
        std::printf("hw_scale_exact: 1x replay failed: %s\n", error.c_str());
        return 1;
    }
    int failures = 0;
    for (uint32_t scale = 2; scale <= 4; ++scale) {
        Run run;
        gpu::GpuStats stats;
        writeback::Counters counters;
        if (!replay(scale, run, stats, counters, error) || run.native.size() != kFrames) {
            std::printf("hw_scale_exact: scale %u replay failed: %s\n", scale, error.c_str());
            ++failures;
            continue;
        }
        uint64_t blockErrors = 0, pixelErrors = 0;
        bool sameMemory = true, composed = true;
        for (uint32_t frame = 0; frame < kFrames; ++frame) {
            sameMemory = sameMemory && run.slm[frame] == base.slm[frame] && run.native[frame] == base.native[frame];
            const std::vector<uint32_t>& pixels = run.scaled[frame];
            const uint32_t w = base.width[frame], h = base.height[frame];
            if (pixels.size() != static_cast<size_t>(w) * h * scale * scale) {
                composed = false;
                continue;
            }
            // 06.3 native mode: GS coordinate N lands on the centre of sub-pixel block N, so a sprite edge on a native
            // pixel edge falls floor(s/2) sub-pixels into the block: scaled pixel (sx, sy) shows the native pixel
            // ((sx - h) / s, (sy - h) / s) with h = floor(s/2), and the compose stands the first drawn sub-pixel in for
            // the h undrawn rows and columns at the top-left edge (IN-372).
            const uint32_t half = scale / 2;
            for (uint32_t sy = 0; sy < h * scale; ++sy) {
                for (uint32_t sx = 0; sx < w * scale; ++sx) {
                    const uint32_t nx = (std::max(sx, half) - half) / scale;
                    const uint32_t ny = (std::max(sy, half) - half) / scale;
                    const uint32_t got = pixels[static_cast<size_t>(sy) * w * scale + sx] & 0xFFFFFFu;
                    const uint8_t* n = &base.native[frame][(static_cast<size_t>(ny) * w + nx) * 3];
                    const uint32_t expected = uint32_t{n[0]} | (uint32_t{n[1]} << 8) | (uint32_t{n[2]} << 16);
                    if (got != expected) {
                        if (pixelErrors < 6) {
                            std::printf("    scale %u frame %u sub-pixel %u,%u: scaled %06x, expected (native %u,%u) %06x\n",
                                        scale, frame, sx, sy, got, nx, ny, expected);
                        }
                        ++pixelErrors;
                    }
                }
            }
            // The 06.7 box downsample equals 1x everywhere except on the half-pixel band around edges: counted only.
            for (uint32_t y = 0; y < h; ++y) {
                for (uint32_t x = 0; x < w; ++x) {
                    const uint32_t first = pixels[(y * scale) * w * scale + x * scale] & 0xFFFFFFu;
                    for (uint32_t dy = 0; dy < scale; ++dy) {
                        for (uint32_t dx = 0; dx < scale; ++dx) {
                            if ((pixels[(y * scale + dy) * w * scale + x * scale + dx] & 0xFFFFFFu) != first) ++blockErrors;
                        }
                    }
                }
            }
        }
        std::printf("  scale %u: memory %s, composes %s, non-uniform blocks (edge bands) %llu, sub-pixels off the 06.3 "
                    "expectation %llu, "
                    "scaled draws %llu, scaled texture copies %llu, circuits from slots %llu, scaled passes %llu, "
                    "clipped %llu\n",
                    scale, sameMemory ? "equal" : "DIFFERENT", composed ? "all" : "MISSING",
                    static_cast<unsigned long long>(blockErrors), static_cast<unsigned long long>(pixelErrors),
                    static_cast<unsigned long long>(stats.scaledDraws),
                    static_cast<unsigned long long>(stats.scaledTextureCopies),
                    static_cast<unsigned long long>(counters.scaledCircuitsFromSlots),
                    static_cast<unsigned long long>(stats.scaledPasses),
                    static_cast<unsigned long long>(stats.scaledDrawsClipped));
        // Blindness: the displayed 1x image must show the clear colour and the three sprites (4 colours or more).
        std::vector<uint32_t> colours;
        for (size_t i = 0; i + 2 < base.native[0].size(); i += 3) {
            const uint32_t c = uint32_t{base.native[0][i]} | (uint32_t{base.native[0][i + 1]} << 8) |
                               (uint32_t{base.native[0][i + 2]} << 16);
            if (std::find(colours.begin(), colours.end(), c) == colours.end()) colours.push_back(c);
        }
        if (colours.size() < 4) {
            std::printf("  scale %u: the displayed image has %zu colours (fixture is blind)\n", scale, colours.size());
        }
        if (colours.size() < 4 || !sameMemory || !composed || blockErrors == 0 || pixelErrors != 0 || stats.scaledDraws == 0 ||
            stats.scaledTextureCopies == 0 || counters.scaledCircuitsFromSlots != kFrames) {
            ++failures;
        }
    }
    std::printf("hw_scale_exact: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}

// hw_scale_target_extent (IN-384, parity plan Part A): a scaled image covers only its slot's real extent and grows on
// demand. Three shapes in one capture, all flat CT32 sprites so the 06.3 expectation is exact (as in hw_scale_exact):
//   - frame 0: target A at page 0, FBW 1 (64 wide), 64 rows, shown;
//   - frame 1: the same view grows to 128 rows (a load and a draw outside the old extent), so A's scaled image has to
//     grow while it already holds the previous frame's content, and the whole 64 x 128 picture must be presented;
//   - frame 2: another buffer B at page 8 with FBW 2 (128 wide), 64 rows: a different width, its own image;
//   - frame 3: A shown again, so the grown image is reused after the other view.
// Every scaled frame must equal the 06.3 expectation of its 1x image, memory must equal the 1x run, no scaled draw may
// leave its pair's images (clipped 0), the images must have grown, and their device memory must stay a small fraction
// of the fixed 1024 x 1024 - per - slot squares (184 MB at scale 2).
int TestScaleTargetExtent(const gpu::VulkanHandles& handles) {
    auto rgbaq = [](uint32_t colour) { return uint64_t{colour} | (uint64_t{0x3F800000u} << 32); };
    auto xy = [](uint32_t x, uint32_t y) { return uint64_t{x * 16} | (uint64_t{y * 16} << 16); };
    auto frameReg = [](uint64_t page, uint64_t fbw) {
        return page | (fbw << 16);  // FBP in pages, FBW in 64-pixel units, PSM CT32
    };
    auto dispFb = [](uint64_t page, uint64_t fbw) { return page | (fbw << 9); };
    auto dispSize = [](uint64_t width, uint64_t height) {
        return (uint64_t{3} << 23) | ((width * 4 - 1) << 32) | ((height - 1) << 44);
    };
    const uint64_t kSprite = 6, kZ = 8;
    constexpr uint32_t kFrames = 4;
    cpuref::CaptureWriter capture;
    capture.Privileged(0x12000000u, 1u | (1u << 5) | (0xFFu << 8));  // PMODE: RC1, MMOD 1, ALP 255
    capture.Privileged(0x12000010u, uint64_t{2} << 13);              // SMODE1: NTSC
    auto ad = [&](const std::vector<uint64_t>& words) {
        capture.Gif(1, cpuref::CaptureWriter::Packet(static_cast<uint32_t>(words.size() / 2), true, false, 0, 0, {0xE},
                                                     words));
    };
    // Frame 0: A (page 0, FBW 1) 64 x 64, clear plus one sprite, shown as 64 x 64.
    ad({(uint64_t{63} << 16) | (uint64_t{63} << 48), 0x40, 0, 0x18,
        (uint64_t{1} << 16) | (uint64_t{1} << 17), 0x47,  // TEST: ZTE, ALWAYS
        frameReg(0, 1), 0x4C, kZ | (uint64_t{1} << 32), 0x4E,
        kSprite, 0x00, rgbaq(0x80102030u), 0x01, xy(0, 0), 0x05, xy(63, 63), 0x05,
        kSprite, 0x00, rgbaq(0x80E04010u), 0x01, xy(8, 8), 0x05, xy(40, 40), 0x05});
    capture.Privileged(0x12000070u, dispFb(0, 1));
    capture.Privileged(0x12000080u, dispSize(64, 64));
    capture.FrameEnd(0);
    // Frame 1: the same view over 128 rows: the load and the draw reach past the 64-row extent.
    ad({(uint64_t{63} << 16) | (uint64_t{127} << 48), 0x40, 0, 0x18,
        (uint64_t{1} << 16) | (uint64_t{1} << 17), 0x47,
        frameReg(0, 1), 0x4C, kZ | (uint64_t{1} << 32), 0x4E,
        kSprite, 0x00, rgbaq(0x80102030u), 0x01, xy(0, 0), 0x05, xy(63, 63), 0x05,
        kSprite, 0x00, rgbaq(0x804010E0u), 0x01, xy(16, 64), 0x05, xy(47, 111), 0x05});
    capture.Privileged(0x12000070u, dispFb(0, 1));
    capture.Privileged(0x12000080u, dispSize(64, 128));
    capture.FrameEnd(1);
    // Frame 2: buffer B (page 8, FBW 2 = 128 wide, 64 rows): another width, its own scaled image.
    ad({(uint64_t{127} << 16) | (uint64_t{63} << 48), 0x40, 0, 0x18,
        (uint64_t{1} << 16) | (uint64_t{1} << 17), 0x47,
        frameReg(8, 2), 0x4C, kZ | (uint64_t{1} << 32), 0x4E,
        kSprite, 0x00, rgbaq(0x80304090u), 0x01, xy(0, 0), 0x05, xy(127, 63), 0x05,
        kSprite, 0x00, rgbaq(0x80C05020u), 0x01, xy(32, 16), 0x05, xy(96, 47), 0x05});
    capture.Privileged(0x12000070u, dispFb(8, 2));
    capture.Privileged(0x12000080u, dispSize(128, 64));
    capture.FrameEnd(2);
    // Frame 3: A again, 64 x 128, with a different picture (the grown image is reused after the other view).
    ad({(uint64_t{63} << 16) | (uint64_t{127} << 48), 0x40, 0, 0x18,
        (uint64_t{1} << 16) | (uint64_t{1} << 17), 0x47,
        frameReg(0, 1), 0x4C, kZ | (uint64_t{1} << 32), 0x4E,
        kSprite, 0x00, rgbaq(0x802060B0u), 0x01, xy(0, 0), 0x05, xy(63, 127), 0x05,
        kSprite, 0x00, rgbaq(0x8010E040u), 0x01, xy(0, 0), 0x05, xy(31, 31), 0x05});
    capture.Privileged(0x12000070u, dispFb(0, 1));
    capture.Privileged(0x12000080u, dispSize(64, 128));
    capture.FrameEnd(3);

    struct Run {
        std::vector<uint64_t> slm;
        std::vector<std::vector<uint8_t>> native;
        std::vector<std::vector<uint32_t>> scaled;
        std::vector<uint32_t> width, height;
    };
    auto replay = [&](uint32_t scale, Run& run, gpu::GpuStats& stats, std::string& error) -> bool {
        writeback::Renderer renderer;
        renderer.SetScale(scale, static_cast<float>(scale));
        if (!renderer.Init(handles, error)) return false;
        renderer.SetSyncMode(true);
        renderer.SetCpuSmallSprites(false);
        writeback::WritebackReplayTarget target(renderer, writeback::ComposeKind::Reference);
        cpuref::ReplaySummary summary;
        const bool ok = cpuref::ReplayCapture(capture.Bytes(), target, summary,
                                              [&](uint64_t, const cpuref::FrameImage& image) {
                                                  std::vector<uint8_t> rgb;
                                                  for (const display::Rgba8& p : image.image.pixels) {
                                                      rgb.push_back(p.r);
                                                      rgb.push_back(p.g);
                                                      rgb.push_back(p.b);
                                                  }
                                                  run.native.push_back(rgb);
                                                  run.width.push_back(image.image.width);
                                                  run.height.push_back(image.image.height);
                                                  run.slm.push_back(cpuref::HashLocalMemory(renderer.Slm(), 4u * 1024u * 1024u));
                                                  writeback::GpuFrame gpuFrame;
                                                  std::vector<uint32_t> pixels;
                                                  if (renderer.ComposeFrameGpu(gpuFrame) &&
                                                      renderer.ReadGpuFrame(gpuFrame, pixels) &&
                                                      gpuFrame.width == scale * image.image.width &&
                                                      gpuFrame.height == scale * image.image.height) {
                                                      run.scaled.push_back(std::move(pixels));
                                                  } else {
                                                      run.scaled.emplace_back();
                                                  }
                                              },
                                              error);
        stats = renderer.GpuCounters();
        if (renderer.Failed()) error += " " + renderer.LastError();
        return ok && !renderer.Failed();
    };
    Run base;
    gpu::GpuStats baseStats;
    std::string error;
    if (!replay(1, base, baseStats, error) || base.native.size() != kFrames) {
        std::printf("hw_scale_target_extent: 1x replay failed: %s\n", error.c_str());
        return 1;
    }
    if (base.width[1] != 64 || base.height[1] != 128 || base.width[2] != 128 || base.height[2] != 64) {
        std::printf("hw_scale_target_extent: the 1x frames are not the shapes the fixture draws (%ux%u, %ux%u)\n",
                    base.width[1], base.height[1], base.width[2], base.height[2]);
        return 1;
    }
    int failures = 0;
    for (uint32_t scale = 2; scale <= 4; scale += 2) {
        Run run;
        gpu::GpuStats stats;
        if (!replay(scale, run, stats, error) || run.native.size() != kFrames) {
            std::printf("hw_scale_target_extent: scale %u replay failed: %s\n", scale, error.c_str());
            ++failures;
            continue;
        }
        uint64_t pixelErrors = 0;
        bool sameMemory = true, composed = true;
        for (uint32_t frame = 0; frame < kFrames; ++frame) {
            sameMemory = sameMemory && run.slm[frame] == base.slm[frame] && run.native[frame] == base.native[frame];
            const std::vector<uint32_t>& pixels = run.scaled[frame];
            const uint32_t w = base.width[frame], h = base.height[frame];
            if (pixels.size() != static_cast<size_t>(w) * h * scale * scale) {
                composed = false;
                continue;
            }
            const uint32_t half = scale / 2;
            for (uint32_t sy = 0; sy < h * scale; ++sy) {
                for (uint32_t sx = 0; sx < w * scale; ++sx) {
                    const uint32_t nx = (std::max(sx, half) - half) / scale;
                    const uint32_t ny = (std::max(sy, half) - half) / scale;
                    const uint32_t got = pixels[static_cast<size_t>(sy) * w * scale + sx] & 0xFFFFFFu;
                    const uint8_t* n = &base.native[frame][(static_cast<size_t>(ny) * w + nx) * 3];
                    const uint32_t expected = uint32_t{n[0]} | (uint32_t{n[1]} << 8) | (uint32_t{n[2]} << 16);
                    if (got != expected) {
                        if (pixelErrors < 6) {
                            std::printf("    scale %u frame %u sub-pixel %u,%u: scaled %06x, expected (native %u,%u) "
                                        "%06x\n",
                                        scale, frame, sx, sy, got, nx, ny, expected);
                        }
                        ++pixelErrors;
                    }
                }
            }
        }
        // The fixed 1024 x 1024 per-slot squares of IN-371 cost 11 images x (1024 * scale)^2 x 4 bytes; this capture's
        // slots are at most 128 x 128 native, so a few MB is all that may be allocated.
        const double fixedSquaresMb = 11.0 * 1024.0 * 1024.0 * 4.0 * scale * scale / 1e6;
        const double imageMb = static_cast<double>(stats.scaledImageBytes) / 1e6;
        std::printf("  scale %u: memory %s, composes %s, sub-pixels off the 06.3 expectation %llu, scaled draws %llu, "
                    "scaled passes %llu, grows %llu, clipped %llu, scaled image %.2f MB (fixed squares %.0f MB)\n",
                    scale, sameMemory ? "equal" : "DIFFERENT", composed ? "all" : "MISSING",
                    static_cast<unsigned long long>(pixelErrors),
                    static_cast<unsigned long long>(stats.scaledDraws),
                    static_cast<unsigned long long>(stats.scaledPasses),
                    static_cast<unsigned long long>(stats.scaledImageGrows),
                    static_cast<unsigned long long>(stats.scaledDrawsClipped), imageMb, fixedSquaresMb);
        if (!sameMemory || !composed || pixelErrors != 0 || stats.scaledDraws == 0 || stats.scaledImageGrows == 0 ||
            stats.scaledDrawsClipped != 0 || imageMb >= fixedSquaresMb / 8.0) {
            ++failures;
        }
    }
    std::printf("hw_scale_target_extent: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}

// hw_scale_display_shadow (owner test 6, IN-377): GT4's race double buffer at an internal scale. Each frame first draws
// a small target whose depth buffer lies on the buffer the next VBlank shows (finished one frame earlier), then the
// back buffer, then shows the other buffer: the depth load drops the shown buffer's colour slot, so before IN-377 the
// presented frame was composed from the GLM at 1x (no visible upscaling). Two variants:
//   - read-only depth (ZMSK = 1): memory of the shown buffer does not change. Every shown frame from the third on
//     must come from a display shadow and match the 06.3 exact expectation of the 1x image at scale (the same
//     check as hw_scale_exact), so the presented picture is really upscaled.
//   - depth writes (ZMSK = 0, ZTST ALWAYS): part of the shown buffer's memory changes. Every native pixel the write
//     changed must be shown from memory (1x), every other one still from the shadow at scale.
// Memory must equal the 1x run in both variants (the 1x words stay the only authority).
int TestScaleDisplayShadow(const gpu::VulkanHandles& handles) {
    auto rgbaq = [](uint32_t colour) { return uint64_t{colour} | (uint64_t{0x3F800000u} << 32); };
    auto xyz = [](uint32_t x, uint32_t y, uint32_t z) { return uint64_t{x * 16} | (uint64_t{y * 16} << 16) | (uint64_t{z} << 32); };
    auto frameReg = [](uint64_t page) { return page | (uint64_t{1} << 16); };  // FBW 1, CT32
    const uint64_t kSprite = 6;
    const uint64_t kBuffers[2] = {0, 2};  // 64 x 64 CT32 buffers (two pages each)
    const uint64_t kSmallTarget = 8;      // the small target drawn first each frame
    constexpr uint32_t kFrames = 6;
    constexpr uint32_t kFirstChecked = 3;  // the shown buffer was shown before (a display candidate) from here on

    auto makeCapture = [&](bool depthWrites) {
        cpuref::CaptureWriter capture;
        capture.Privileged(0x12000000u, 1u | (1u << 5) | (0xFFu << 8));  // PMODE: RC1, MMOD 1, ALP 255
        capture.Privileged(0x12000010u, uint64_t{2} << 13);               // SMODE1: NTSC
        capture.Privileged(0x12000080u, (uint64_t{3} << 23) | (uint64_t{255} << 32) | (uint64_t{63} << 44));  // 64 x 64
        for (uint32_t frame = 0; frame < kFrames; ++frame) {
            const uint64_t back = kBuffers[frame & 1u];
            const uint64_t front = kBuffers[(frame + 1u) & 1u];
            // ZTE with GEQUAL (reads depth) and ZMSK, or ALWAYS with depth writes.
            const uint64_t test = depthWrites ? ((uint64_t{1} << 16) | (uint64_t{1} << 17))
                                              : ((uint64_t{1} << 16) | (uint64_t{2} << 17));
            const uint64_t zbuf = front | (depthWrites ? 0 : (uint64_t{1} << 32));  // Z32 on the shown buffer
            const std::vector<uint64_t> words = {
                (uint64_t{63} << 16) | (uint64_t{63} << 48), 0x40, 0, 0x18,
                test, 0x47, frameReg(kSmallTarget), 0x4C, zbuf, 0x4E,
                kSprite, 0x00, rgbaq(0x80305070u + frame), 0x01, xyz(4, 4, 0x00ABCDEFu), 0x05, xyz(28, 20, 0x00ABCDEFu), 0x05,
                (uint64_t{1} << 16) | (uint64_t{1} << 17), 0x47,  // TEST: ZTE, ALWAYS
                frameReg(back), 0x4C, uint64_t{16} | (uint64_t{1} << 32), 0x4E,  // ZBUF elsewhere, masked
                kSprite, 0x00, rgbaq(0x80102030u + frame), 0x01, xyz(0, 0, 0), 0x05, xyz(64, 64, 0), 0x05,
                kSprite, 0x00, rgbaq(0x80E04010u), 0x01, xyz(5 + frame, 7, 0), 0x05, xyz(20, 30, 0), 0x05,
                kSprite, 0x00, rgbaq(0x8010E040u), 0x01, xyz(33, 2, 0), 0x05, xyz(60, 9 + frame, 0), 0x05,
                kSprite, 0x00, rgbaq(0x804010E0u), 0x01, xyz(10, 40, 0), 0x05, xyz(50, 61, 0), 0x05};
            capture.Gif(1, cpuref::CaptureWriter::Packet(static_cast<uint32_t>(words.size() / 2), true, false, 0, 0, {0xE},
                                                         words));
            capture.Privileged(0x12000070u, front | (uint64_t{1} << 9));  // DISPFB1: the buffer finished last frame
            capture.FrameEnd(frame);
        }
        return capture;
    };

    struct Run {
        std::vector<uint64_t> slm;
        std::vector<std::vector<uint8_t>> native;   // 1x reference images
        std::vector<std::vector<uint32_t>> scaled;  // scaled GPU composes (the presented frames)
        uint32_t width = 0, height = 0;
        writeback::Counters counters;
    };
    auto replay = [&](const cpuref::CaptureWriter& capture, uint32_t scale, Run& run, std::string& error) -> bool {
        writeback::Renderer renderer;
        renderer.SetScale(scale, static_cast<float>(scale));
        if (!renderer.Init(handles, error)) return false;
        renderer.SetSyncMode(true);
        renderer.SetCpuSmallSprites(false);  // the flat sprites go through the GPU (and its scaled pass)
        writeback::WritebackReplayTarget target(renderer, writeback::ComposeKind::Reference);
        cpuref::ReplaySummary summary;
        const bool ok = cpuref::ReplayCapture(capture.Bytes(), target, summary,
                                              [&](uint64_t, const cpuref::FrameImage& image) {
                                                  std::vector<uint8_t> rgb;
                                                  for (const display::Rgba8& p : image.image.pixels) {
                                                      rgb.push_back(p.r);
                                                      rgb.push_back(p.g);
                                                      rgb.push_back(p.b);
                                                  }
                                                  run.native.push_back(rgb);
                                                  run.width = image.image.width;
                                                  run.height = image.image.height;
                                                  run.slm.push_back(cpuref::HashLocalMemory(renderer.Slm(), 4u * 1024u * 1024u));
                                                  writeback::GpuFrame gpuFrame;
                                                  std::vector<uint32_t> pixels;
                                                  if (scale > 1 && renderer.ComposeFrameGpu(gpuFrame) &&
                                                      renderer.ReadGpuFrame(gpuFrame, pixels) &&
                                                      gpuFrame.width == scale * image.image.width &&
                                                      gpuFrame.height == scale * image.image.height) {
                                                      run.scaled.push_back(std::move(pixels));
                                                  } else {
                                                      run.scaled.emplace_back();
                                                  }
                                              },
                                              error);
        run.counters = renderer.GetCounters();
        if (renderer.Failed()) error += " " + renderer.LastError();
        return ok && !renderer.Failed() && run.native.size() == kFrames;
    };
    auto rgbAt = [](const std::vector<uint8_t>& image, uint32_t width, uint32_t x, uint32_t y) {
        const uint8_t* n = &image[(static_cast<size_t>(y) * width + x) * 3];
        return uint32_t{n[0]} | (uint32_t{n[1]} << 8) | (uint32_t{n[2]} << 16);
    };

    int failures = 0;
    std::string error;
    const cpuref::CaptureWriter readOnly = makeCapture(false);
    const cpuref::CaptureWriter writes = makeCapture(true);
    Run readOnlyBase, writesBase;
    if (!replay(readOnly, 1, readOnlyBase, error) || !replay(writes, 1, writesBase, error)) {
        std::printf("hw_scale_display_shadow: 1x replay failed: %s\n", error.c_str());
        return 1;
    }
    // Blindness: the depth writes must change what the shown buffer holds, and the read-only depth must not.
    uint64_t changedNative = 0;
    for (uint32_t frame = kFirstChecked; frame < kFrames; ++frame) {
        for (uint32_t y = 0; y < readOnlyBase.height; ++y) {
            for (uint32_t x = 0; x < readOnlyBase.width; ++x) {
                if (rgbAt(readOnlyBase.native[frame], readOnlyBase.width, x, y) !=
                    rgbAt(writesBase.native[frame], writesBase.width, x, y)) {
                    ++changedNative;
                }
            }
        }
    }
    if (changedNative == 0) {
        std::printf("hw_scale_display_shadow: the depth writes change no shown pixel (fixture is blind)\n");
        ++failures;
    }
    for (uint32_t scale : {2u, 4u}) {
        for (const bool depthWrites : {false, true}) {
            const Run& base = depthWrites ? writesBase : readOnlyBase;
            Run run;
            if (!replay(depthWrites ? writes : readOnly, scale, run, error)) {
                std::printf("hw_scale_display_shadow: scale %u replay failed: %s\n", scale, error.c_str());
                ++failures;
                continue;
            }
            const uint32_t w = base.width, h = base.height, half = scale / 2;
            uint64_t pixelErrors = 0, fromMemory = 0, fromScale = 0, framesWithoutEdges = 0;
            bool sameMemory = true, composed = true;
            for (uint32_t frame = 0; frame < kFrames; ++frame) {
                sameMemory = sameMemory && run.slm[frame] == base.slm[frame] && run.native[frame] == base.native[frame];
                if (frame < kFirstChecked) continue;
                const std::vector<uint32_t>& pixels = run.scaled[frame];
                if (pixels.size() != static_cast<size_t>(w) * h * scale * scale) {
                    composed = false;
                    continue;
                }
                uint64_t mixedBlocks = 0;
                for (uint32_t sy = 0; sy < h * scale; ++sy) {
                    for (uint32_t sx = 0; sx < w * scale; ++sx) {
                        // The native pixel the 1x compose shows here; if the depth writes changed it, memory (1x) wins.
                        // The shadow is used only where neither that pixel nor the one the sub-pixel was drawn for
                        // (06.3) changed.
                        const uint32_t px = sx / scale, py = sy / scale;
                        const uint32_t dx = (std::max(sx, half) - half) / scale, dy = (std::max(sy, half) - half) / scale;
                        const bool changed =
                            rgbAt(base.native[frame], w, px, py) != rgbAt(readOnlyBase.native[frame], w, px, py) ||
                            rgbAt(base.native[frame], w, dx, dy) != rgbAt(readOnlyBase.native[frame], w, dx, dy);
                        uint32_t expected = 0;
                        if (changed) {
                            expected = rgbAt(base.native[frame], w, px, py);
                            ++fromMemory;
                        } else {
                            // 06.3 native mode (hw_scale_exact): sub-pixel (sx, sy) shows native ((sx - h) / s, (sy - h) / s).
                            expected = rgbAt(readOnlyBase.native[frame], w, dx, dy);
                            ++fromScale;
                        }
                        const uint32_t got = pixels[static_cast<size_t>(sy) * w * scale + sx] & 0xFFFFFFu;
                        if (got != expected) {
                            if (pixelErrors < 6) {
                                std::printf("    scale %u %s frame %u sub-pixel %u,%u: %06x, expected %06x (%s)\n", scale,
                                            depthWrites ? "writes" : "read-only", frame, sx, sy, got, expected,
                                            changed ? "memory changed: 1x" : "unchanged: scaled");
                            }
                            ++pixelErrors;
                        }
                    }
                }
                for (uint32_t y = 0; y < h; ++y) {
                    for (uint32_t x = 0; x < w; ++x) {
                        const uint32_t first = pixels[(y * scale) * w * scale + x * scale] & 0xFFFFFFu;
                        bool uniform = true;
                        for (uint32_t dy = 0; dy < scale && uniform; ++dy) {
                            for (uint32_t dx = 0; dx < scale && uniform; ++dx) {
                                uniform = (pixels[(y * scale + dy) * w * scale + x * scale + dx] & 0xFFFFFFu) == first;
                            }
                        }
                        if (!uniform) ++mixedBlocks;
                    }
                }
                if (mixedBlocks == 0) ++framesWithoutEdges;  // a presented frame that is 1x-sourced
            }
            const uint64_t shownChecked = kFrames - kFirstChecked;
            std::printf("  scale %u %s: memory %s, composes %s, sub-pixels off expectation %llu (from memory %llu, at "
                        "scale %llu), 1x-sourced frames %llu, circuits from shadows %llu / slots %llu / GLM %llu, "
                        "shadow saves %llu\n",
                        scale, depthWrites ? "depth writes" : "read-only depth", sameMemory ? "equal" : "DIFFERENT",
                        composed ? "all" : "MISSING", static_cast<unsigned long long>(pixelErrors),
                        static_cast<unsigned long long>(fromMemory), static_cast<unsigned long long>(fromScale),
                        static_cast<unsigned long long>(framesWithoutEdges),
                        static_cast<unsigned long long>(run.counters.scaledCircuitsFromShadows),
                        static_cast<unsigned long long>(run.counters.scaledCircuitsFromSlots),
                        static_cast<unsigned long long>(run.counters.scaledCircuitsFromGlm),
                        static_cast<unsigned long long>(run.counters.scaledShadowSaves));
            if (!sameMemory || !composed || pixelErrors != 0 || framesWithoutEdges != 0 ||
                run.counters.scaledCircuitsFromShadows < shownChecked || (depthWrites && fromMemory == 0) ||
                (!depthWrites && fromMemory != 0)) {
                ++failures;
            }
        }
    }
    std::printf("hw_scale_display_shadow: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}

// hw_scale_fullscreen_passes (owner test 7, IN-379/IN-380): the two full-screen passes that made GT4's race look 1x
// at every scale, each in a minimal capture, checked on the presented scaled frame against the 06.3 exact expectation
// of the final 1x image (the hw_scale_exact rule: sub-pixel (sx, sy) shows native ((sx - h) / s, (sy - h) / s)).
//   - "copy": the frame is copied into the shown buffer through a REGION_CLAMP texture whose power-of-two size
//     (256 x 128) is larger than the rendered area (100 x 60), as GT4's CT16 conversion does (640 x 448 through
//     1024 x 512). The copy must sample the target's scaled image (IN-380), not the 1x words.
//   - "tone": after the frame is drawn, a tone sequence reads it through an 8-bit (T8) view into a temporary page and
//     writes a result back into the same memory through a CT24 view with T8-textured, FIX-blended sprites. The
//     result is a per-channel function of the frame (Cd - Cd * FIX / 128), like GT4's RGB tone curve, so the scaled
//     image after the sequence must be that function of the scaled image before it (IN-379).
// Memory must equal the 1x run (the 1x words stay the only authority). Both cases fail without the fix.
int TestScaleFullscreenPasses(const gpu::VulkanHandles& handles) {
    auto rgbaq = [](uint32_t colour) { return uint64_t{colour} | (uint64_t{0x3F800000u} << 32); };
    auto xy = [](uint32_t x, uint32_t y) { return uint64_t{x * 16} | (uint64_t{y * 16} << 16); };
    auto uvRaw = [](uint32_t u16, uint32_t v16) { return uint64_t{u16} | (uint64_t{v16} << 16); };
    const uint64_t kSprite = 6, kTme = 1u << 4, kAbe = 1u << 6, kFst = 1u << 8;
    auto tex0 = [](uint64_t tbpBlocks, uint64_t tbw, uint64_t psm, uint64_t twLog2, uint64_t thLog2, uint64_t cbpBlocks,
                   uint64_t cld) {
        return tbpBlocks | (tbw << 14) | (psm << 20) | (twLog2 << 26) | (thLog2 << 30) | (uint64_t{1} << 34) |
               (uint64_t{1} << 35) | (cbpBlocks << 37) | (cld << 61);  // TCC, TFX decal, CPSM CT32, CSM 0
    };

    struct Case {
        const char* name;
        cpuref::CaptureWriter capture;
        uint32_t width, height;
    };
    std::vector<Case> cases;
    {
        // copy: A = page 0 (CT32, FBW 2, 128 wide), shown buffer B = page 8 (CT32, FBW 2); 100 x 60 picture.
        Case c{"copy", {}, 100, 60};
        c.capture.Privileged(0x12000000u, 1u | (1u << 5) | (0xFFu << 8));  // PMODE: RC1, MMOD 1, ALP 255
        c.capture.Privileged(0x12000010u, uint64_t{2} << 13);               // SMODE1: NTSC
        c.capture.Privileged(0x12000070u, uint64_t{8} | (uint64_t{2} << 9));  // DISPFB1: page 8, FBW 2, CT32
        c.capture.Privileged(0x12000080u, (uint64_t{3} << 23) | (uint64_t{399} << 32) | (uint64_t{59} << 44));
        const std::vector<uint64_t> words = {
            (uint64_t{127} << 16) | (uint64_t{63} << 48), 0x40, 0, 0x18,
            (uint64_t{1} << 16) | (uint64_t{1} << 17), 0x47,                // TEST: ZTE, ALWAYS
            uint64_t{0} | (uint64_t{2} << 16), 0x4C, uint64_t{30} | (uint64_t{1} << 32), 0x4E,  // FRAME A, ZBUF masked
            kSprite, 0x00, rgbaq(0x80203040u), 0x01, xy(0, 0), 0x05, xy(100, 60), 0x05,
            kSprite, 0x00, rgbaq(0x80E04010u), 0x01, xy(7, 5), 0x05, xy(31, 40), 0x05,
            kSprite, 0x00, rgbaq(0x8010E040u), 0x01, xy(50, 12), 0x05, xy(93, 27), 0x05,
            uint64_t{8} | (uint64_t{2} << 16), 0x4C,                        // FRAME B
            tex0(0, 2, 0x00, 8, 7, 0, 0), 0x06,                             // A as CT32, 256 x 128
            0, 0x14,                                                        // TEX1: nearest
            uint64_t{2} | (uint64_t{2} << 2) | (uint64_t{99} << 14) | (uint64_t{59} << 34), 0x08,  // REGION_CLAMP
            kSprite | kTme | kFst, 0x00, rgbaq(0x80808080u), 0x01, uvRaw(8, 8), 0x03, xy(0, 0), 0x05,
            uvRaw(100 * 16 + 8, 60 * 16 + 8), 0x03, xy(100, 60), 0x05};
        c.capture.Gif(1, cpuref::CaptureWriter::Packet(static_cast<uint32_t>(words.size() / 2), true, false, 0, 0, {0xE},
                                                       words));
        c.capture.FrameEnd(0);
        cases.push_back(std::move(c));
    }
    {
        // tone: A = page 0 (CT32, FBW 1, 64 x 64, shown); temporary T = page 8; a zero T8 texture at page 16 and a
        // zero CLUT at page 20, so every textured source colour is 0 and the blend gives Cd - Cd * 0x50 / 128.
        Case c{"tone", {}, 64, 64};
        c.capture.Privileged(0x12000000u, 1u | (1u << 5) | (0xFFu << 8));
        c.capture.Privileged(0x12000010u, uint64_t{2} << 13);
        c.capture.Privileged(0x12000070u, uint64_t{0} | (uint64_t{1} << 9));  // DISPFB1: page 0, FBW 1, CT32
        c.capture.Privileged(0x12000080u, (uint64_t{3} << 23) | (uint64_t{255} << 32) | (uint64_t{63} << 44));
        const uint64_t blendFix = 0 | (uint64_t{1} << 2) | (uint64_t{2} << 4) | (uint64_t{1} << 6) | (uint64_t{0x50} << 32);
        const std::vector<uint64_t> words = {
            (uint64_t{63} << 16) | (uint64_t{63} << 48), 0x40, 0, 0x18,
            (uint64_t{1} << 16) | (uint64_t{1} << 17), 0x47,
            uint64_t{0} | (uint64_t{1} << 16), 0x4C, uint64_t{30} | (uint64_t{1} << 32), 0x4E,  // FRAME A (CT32)
            kSprite, 0x00, rgbaq(0x80406080u), 0x01, xy(0, 0), 0x05, xy(64, 64), 0x05,
            kSprite, 0x00, rgbaq(0x80E04010u), 0x01, xy(5, 7), 0x05, xy(20, 30), 0x05,
            kSprite, 0x00, rgbaq(0x8010E040u), 0x01, xy(33, 2), 0x05, xy(60, 11), 0x05,
            kSprite, 0x00, rgbaq(0x804010E0u), 0x01, xy(10, 40), 0x05, xy(50, 61), 0x05,
            // The sequence: A read through a T8 view into T ...
            uint64_t{8} | (uint64_t{1} << 16), 0x4C,                        // FRAME T
            tex0(0, 2, 0x13, 7, 6, 20 * 32, 1), 0x06,                       // A as T8 (128 x 64), CLUT load
            0, 0x14, 0, 0x08,                                               // nearest, REPEAT
            kSprite | kTme | kFst, 0x00, rgbaq(0x80808080u), 0x01, uvRaw(8, 8), 0x03, xy(0, 0), 0x05,
            uvRaw(64 * 16 + 8, 32 * 16 + 8), 0x03, xy(64, 32), 0x05,
            // ... then the result written back into A through a CT24 view, T8-textured and FIX-blended.
            uint64_t{0} | (uint64_t{1} << 16) | (uint64_t{1} << 24), 0x4C,  // FRAME A as CT24
            tex0(16 * 32, 2, 0x13, 7, 6, 20 * 32, 0), 0x06,                 // the zero T8 page
            blendFix, 0x42,
            kSprite | kTme | kAbe | kFst, 0x00, rgbaq(0x80808080u), 0x01, uvRaw(8, 8), 0x03, xy(0, 0), 0x05,
            uvRaw(64 * 16 + 8, 64 * 16 + 8), 0x03, xy(64, 64), 0x05};
        c.capture.Gif(1, cpuref::CaptureWriter::Packet(static_cast<uint32_t>(words.size() / 2), true, false, 0, 0, {0xE},
                                                       words));
        c.capture.FrameEnd(0);
        cases.push_back(std::move(c));
    }

    struct Run {
        uint64_t slm = 0;
        std::vector<uint8_t> native;
        std::vector<uint32_t> scaled;
        uint32_t width = 0, height = 0;
        writeback::Counters counters;
        gpu::GpuStats stats;
    };
    auto replay = [&](const Case& c, uint32_t scale, Run& run, std::string& error) -> bool {
        writeback::Renderer renderer;
        renderer.SetScale(scale, static_cast<float>(scale));
        if (!renderer.Init(handles, error)) return false;
        renderer.SetSyncMode(true);
        renderer.SetCpuSmallSprites(false);
        writeback::WritebackReplayTarget target(renderer, writeback::ComposeKind::Reference);
        cpuref::ReplaySummary summary;
        bool got = false;
        const bool ok = cpuref::ReplayCapture(c.capture.Bytes(), target, summary,
                                              [&](uint64_t, const cpuref::FrameImage& image) {
                                                  for (const display::Rgba8& p : image.image.pixels) {
                                                      run.native.push_back(p.r);
                                                      run.native.push_back(p.g);
                                                      run.native.push_back(p.b);
                                                  }
                                                  run.width = image.image.width;
                                                  run.height = image.image.height;
                                                  run.slm = cpuref::HashLocalMemory(renderer.Slm(), 4u * 1024u * 1024u);
                                                  writeback::GpuFrame frame;
                                                  got = scale == 1 || (renderer.ComposeFrameGpu(frame) &&
                                                                       renderer.ReadGpuFrame(frame, run.scaled) &&
                                                                       frame.width == scale * image.image.width &&
                                                                       frame.height == scale * image.image.height);
                                              },
                                              error);
        run.counters = renderer.GetCounters();
        run.stats = renderer.GpuCounters();
        if (renderer.Failed()) error += " " + renderer.LastError();
        return ok && got && !renderer.Failed();
    };

    int failures = 0;
    for (const Case& c : cases) {
        Run base;
        std::string error;
        if (!replay(c, 1, base, error) || base.width != c.width || base.height != c.height) {
            std::printf("hw_scale_fullscreen_passes: %s 1x replay failed (%ux%u): %s\n", c.name, base.width, base.height,
                        error.c_str());
            ++failures;
            continue;
        }
        // Blindness: the shown picture has several colours.
        std::vector<uint32_t> colours;
        for (size_t i = 0; i + 2 < base.native.size(); i += 3) {
            const uint32_t v = uint32_t{base.native[i]} | (uint32_t{base.native[i + 1]} << 8) |
                               (uint32_t{base.native[i + 2]} << 16);
            if (std::find(colours.begin(), colours.end(), v) == colours.end()) colours.push_back(v);
        }
        if (colours.size() < 3) {
            std::printf("hw_scale_fullscreen_passes: %s shows %zu colours (fixture is blind)\n", c.name, colours.size());
            ++failures;
        }
        for (uint32_t scale = 2; scale <= 4; scale += 2) {
            Run run;
            if (!replay(c, scale, run, error)) {
                std::printf("hw_scale_fullscreen_passes: %s scale %u replay failed: %s\n", c.name, scale, error.c_str());
                ++failures;
                continue;
            }
            const uint32_t w = base.width, h = base.height, half = scale / 2;
            uint64_t pixelErrors = 0, mixedBlocks = 0;
            for (uint32_t sy = 0; sy < h * scale; ++sy) {
                for (uint32_t sx = 0; sx < w * scale; ++sx) {
                    const uint32_t nx = (std::max(sx, half) - half) / scale;
                    const uint32_t ny = (std::max(sy, half) - half) / scale;
                    const uint8_t* n = &base.native[(static_cast<size_t>(ny) * w + nx) * 3];
                    const uint32_t expected = uint32_t{n[0]} | (uint32_t{n[1]} << 8) | (uint32_t{n[2]} << 16);
                    const uint32_t got = run.scaled[static_cast<size_t>(sy) * w * scale + sx] & 0xFFFFFFu;
                    if (got != expected) {
                        if (pixelErrors < 4) {
                            std::printf("    %s scale %u sub-pixel %u,%u: %06x, expected (native %u,%u) %06x\n", c.name,
                                        scale, sx, sy, got, nx, ny, expected);
                        }
                        ++pixelErrors;
                    }
                }
            }
            for (uint32_t y = 0; y < h; ++y) {
                for (uint32_t x = 0; x < w; ++x) {
                    const uint32_t first = run.scaled[(y * scale) * w * scale + x * scale] & 0xFFFFFFu;
                    bool uniform = true;
                    for (uint32_t dy = 0; dy < scale && uniform; ++dy) {
                        for (uint32_t dx = 0; dx < scale && uniform; ++dx) {
                            uniform = (run.scaled[(y * scale + dy) * w * scale + x * scale + dx] & 0xFFFFFFu) == first;
                        }
                    }
                    if (!uniform) ++mixedBlocks;
                }
            }
            const bool sameMemory = run.slm == base.slm && run.native == base.native;
            std::printf("  %s scale %u: memory %s, sub-pixels off the 06.3 expectation %llu, mixed blocks %llu, scaled "
                        "texture copies %llu, tone runs %llu, tone carries %llu\n",
                        c.name, scale, sameMemory ? "equal" : "DIFFERENT", static_cast<unsigned long long>(pixelErrors),
                        static_cast<unsigned long long>(mixedBlocks),
                        static_cast<unsigned long long>(run.stats.scaledTextureCopies),
                        static_cast<unsigned long long>(run.counters.toneRuns),
                        static_cast<unsigned long long>(run.counters.toneCarries));
            const bool toneCase = std::string(c.name) == "tone";
            if (!sameMemory || pixelErrors != 0 || mixedBlocks == 0 ||
                (toneCase ? run.counters.toneCarries == 0 : run.stats.scaledTextureCopies == 0)) {
                ++failures;
            }
        }
    }
    std::printf("hw_scale_fullscreen_passes: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}

// hw_tone_hle_exact (IN-383, parity plan item 2; chapter 09.4 variant A). The tone sequence's strip template in a
// synthetic capture: three 64 x 32 frame-buffer pages converted through eight decal T8 strips each (page 0 and page 1
// with the sequence's palette bases, page 2 with a palette base that breaks the template), a 64 x 32 temporary page
// of varied CT32 data per page (uploaded, then drawn into so it is GPU-newer and must be swizzled), and an 8 x 32 T8
// index texture of varied bytes. Replayed with the strip dispatch (default) and with the batches themselves
// (SetToneHle(false)). For every frame:
//   - the two 4 MiB local memories are identical: the dispatch is exact against the batches (the 1x and memory gate);
//   - every pixel of pages 0 and 1 equals the CPU form of the strip rule read from the same memory (index texel ->
//     07.4 CSM1 palette entry of the strip's quadrant -> RGB), and every alpha byte is unchanged, so a wrong
//     quadrant, a wrong CSM1 position, a wrong index texel or a wrong destination address fails the fixture;
//   - the dispatch ran for pages 0 and 1 only, and page 2 (refused) still changed memory.
int TestToneHleExact(const gpu::VulkanHandles& handles) {
    constexpr uint32_t kWidth = 64;
    constexpr uint32_t kRows = 32;
    constexpr uint32_t kFrames = 2;
    constexpr uint32_t kDestBp[3] = {0u, 32u, 64u};      // a CT32 page drawn first, then the strips' CT24 view
    constexpr uint32_t kTempBp[2] = {96u, 128u};         // the temporary pages (CT32, FBW 1)
    constexpr uint32_t kIndexBp = 160u;                  // the T8 index texture (TBW 1, 8 x 32)
    static constexpr uint32_t kQuadrantBlock[8] = {0, 4, 16, 20, 8, 12, 24, 28};  // 01.3 block table, 07.4
    static constexpr uint32_t kBranchPage = 2;           // the page whose palette base breaks the template

    // Deterministic pseudo-random words (a fixture, not a generator): both runs get the same capture.
    uint32_t state = 0x12345u;
    auto next = [&state]() {
        state = state * 1664525u + 1013904223u;
        return state;
    };

    cpuref::CaptureWriter capture;
    // A display that shows the first converted page (not compared here).
    capture.Privileged(0x12000000u, 1u | (1u << 5) | (0xFFu << 8));
    capture.Privileged(0x12000010u, uint64_t{2} << 13);
    capture.Privileged(0x12000070u, uint64_t{1} << 9);                 // DISPFB1: FBP 0, FBW 1, CT32
    capture.Privileged(0x12000080u, (uint64_t{3} << 23) | (uint64_t{127} << 32) | (uint64_t{63} << 44));

    // Emits one recorded GIF path-1 write (qword aligned).
    const auto emit = [&capture](const std::vector<Qword>& qwords) {
        std::vector<uint8_t> bytes(qwords.size() * sizeof(Qword));
        std::memcpy(bytes.data(), qwords.data(), bytes.size());
        capture.Gif(1, bytes);
    };
    // One host -> local transfer of `bytes` (qword multiple) into (bp, bw, psm) at (x, y), w x h pixels.
    const auto upload = [&emit](uint32_t bp, uint32_t bw, uint32_t psm, uint32_t x, uint32_t y, uint32_t w,
                                uint32_t h, const std::vector<uint8_t>& bytes) {
        std::vector<Qword> qwords;
        qwords.push_back(PackedTag(4, false, 0, {0xE}));  // A+D: four registers
        qwords.push_back(Qword{uint64_t{bp} << 32 | uint64_t{bw} << 48 | uint64_t{psm} << 56, 0x50});  // BITBLTBUF
        qwords.push_back(Qword{uint64_t{x} << 32 | uint64_t{y} << 48, 0x51});                          // TRXPOS
        qwords.push_back(Qword{uint64_t{w} | uint64_t{h} << 32, 0x52});                                // TRXREG
        qwords.push_back(Qword{0, 0x53});                                                             // TRXDIR: host -> local
        const uint32_t count = static_cast<uint32_t>(bytes.size() / 16);
        qwords.push_back(PackedTag(count, true, 2, {}));  // the image packet
        for (uint32_t i = 0; i < count; ++i) {
            Qword data;
            std::memcpy(&data.lo, bytes.data() + i * 16, 8);
            std::memcpy(&data.hi, bytes.data() + i * 16 + 8, 8);
            qwords.push_back(data);
        }
        emit(qwords);
    };

    // --- one frame: uploads, a draw into each page (so the targets are GPU-newer), then the strips -----------------
    for (uint32_t frame = 0; frame < kFrames; ++frame) {
        // Temporary pages and index texture: varied CT32 words and varied bytes.
        for (uint32_t t = 0; t < 2; ++t) {
            std::vector<uint8_t> page(kWidth * kRows * 4);
            for (uint32_t word = 0; word < kWidth * kRows; ++word) {
                const uint32_t value = next() | 0xFF000000u;  // alpha 0xFF: the strips must leave it alone
                std::memcpy(page.data() + word * 4, &value, 4);
            }
            upload(kTempBp[t], 1, 0x00, 0, 0, kWidth, kRows, page);
        }
        std::vector<uint8_t> index(kWidth / 8 * kRows);  // 8 x 32 T8 texels
        for (uint8_t& texel : index) texel = static_cast<uint8_t>(next());
        upload(kIndexBp, 1, 0x13, 0, 0, 8, kRows, index);
        // The three destination pages as CT32 words (alpha 0x80), then a flat sprite over each so a resident target
        // holds them (the dispatch parks it in the GLM).
        for (uint32_t page = 0; page < 3; ++page) {
            std::vector<uint8_t> words(kWidth * kRows * 4);
            for (uint32_t word = 0; word < kWidth * kRows; ++word) {
                const uint32_t value = (next() & 0x00FFFFFFu) | (0x80u << 24);
                std::memcpy(words.data() + word * 4, &value, 4);
            }
            upload(kDestBp[page], 1, 0x00, 0, 0, kWidth, kRows, words);
        }
        const auto drawRegs = [&capture](const std::vector<uint64_t>& words) {
            capture.Gif(1, cpuref::CaptureWriter::Packet(static_cast<uint32_t>(words.size() / 2), true, false, 0, 0, {0xE},
                                                        words));
        };
        const auto xy = [](uint32_t x, uint32_t y) {
            return uint64_t{x * 16} | (uint64_t{y * 16} << 16);  // XYZ2/F2 in 12.4 window coordinates
        };
        drawRegs({uint64_t{63} << 16 | uint64_t{63} << 48, 0x40,       // SCISSOR 0..63 x 0..63
                  (uint64_t{1} << 16) | (uint64_t{1} << 17), 0x47,      // TEST: ZTE, ALWAYS
                  uint64_t{1} << 32, 0x4E,                              // ZBUF: depth writes masked (ZMSK = 1)
                  (uint64_t{6} | (uint64_t{1} << 8)), 0x00,             // sprite, FST
                  0x80808080u | (uint64_t{0x3F800000u} << 32), 0x01,          // RGBAQ: a full grey quad
                  xy(0, 0), 0x05, xy(8, 8), 0x05});
        // A draw into each page over the upload, so a resident target holds it: the dispatch must park it in the GLM
        // (destination pages) or swizzle it (the temporary pages) instead of reading the shadow memory.
        for (uint32_t page = 0; page < 3; ++page) {
            drawRegs({uint64_t{kDestBp[page] / 32u} | (uint64_t{1} << 16), 0x4C,  // FRAME: the page (FBP in pages), CT32
                      0x80808080u | (uint64_t{0x3F800000u} << 32), 0x01, xy(1, 1), 0x05, xy(6, 6), 0x05});
        }
        for (uint32_t t = 0; t < 2; ++t) {
            drawRegs({uint64_t{kTempBp[t] / 32u} | (uint64_t{1} << 16), 0x4C,
                      0x80808080u | (uint64_t{0x3F800000u} << 32), 0x01, xy(2, 2), 0x05, xy(7, 7), 0x05});
        }
        // The strips of one page: eight decal T8 sprites, one texel per pixel, 8 x 32 each.
        const auto strips = [&](uint32_t destinationBp, uint32_t tempBp, bool brokenPalette) {
            for (uint32_t strip = 0; strip < 8; ++strip) {
                const uint32_t quadrant = strip;  // the template's order: quadrant q at strip q
                uint32_t cbp = tempBp + kQuadrantBlock[quadrant];
                if (brokenPalette && strip == 3) cbp += 1;  // not a quadrant base: the page must be refused
                const uint64_t tex0 = uint64_t{kIndexBp} | (uint64_t{1} << 14) | (uint64_t{0x13} << 20) |
                                      (uint64_t{3} << 26) | (uint64_t{5} << 30) | (uint64_t{1} << 34) |
                                      (uint64_t{1} << 35) | (uint64_t{cbp} << 37) | (uint64_t{5} << 61);
                drawRegs({uint64_t{destinationBp / 32u} | (uint64_t{1} << 16) | (uint64_t{1} << 24) |
                              (uint64_t{0xFF} << 56),  // FRAME: CT24, FBMSK 0xFF000000 (alpha protected)
                          0x4C,
                          tex0, 0x06,
                          0, 0x14,  // TEX1: nearest
                          0, 0x08,  // CLAMP: repeat
                          uint64_t{6} | (uint64_t{1} << 4) | (uint64_t{1} << 8), 0x00,  // sprite, textured, FST
                          0x80808080u | (uint64_t{0x3F800000u} << 32), 0x01,
                          uint64_t{0}, 0x03,  // UV (0, 0)
                          xy(strip * 8, 0), 0x05,
                          uint64_t{8 * 16} | (uint64_t{32 * 16} << 16), 0x03,  // UV (8, 32) texels
                          xy(strip * 8 + 8, 32), 0x05});
            }
        };
        strips(kDestBp[0], kTempBp[0], false);
        strips(kDestBp[1], kTempBp[1], false);
        strips(kDestBp[kBranchPage], kTempBp[0], true);
        capture.FrameEnd(frame);
    }

    struct Run {
        std::vector<std::vector<uint8_t>> memory;  // the 4 MiB image per frame
        writeback::Counters counters;
    };
    auto replay = [&](bool toneHle, Run& run, std::string& error) -> bool {
        writeback::Renderer renderer;
        if (!renderer.Init(handles, error)) return false;
        renderer.SetSyncMode(true);
        renderer.SetCpuSmallSprites(false);
        renderer.SetToneHle(toneHle);
        writeback::WritebackReplayTarget target(renderer, writeback::ComposeKind::Reference);
        cpuref::ReplaySummary summary;
        const bool ok = cpuref::ReplayCapture(
            capture.Bytes(), target, summary,
            [&](uint64_t, const cpuref::FrameImage&) {
                run.memory.emplace_back(renderer.Slm(), renderer.Slm() + 4u * 1024u * 1024u);
            },
            error);
        run.counters = renderer.GetCounters();
        if (renderer.Failed()) error += " " + renderer.LastError();
        return ok && !renderer.Failed();
    };

    int failures = 0;
    Run hle, exact;
    std::string error;
    if (!replay(true, hle, error) || !replay(false, exact, error)) {
        std::printf("hw_tone_hle_exact: replay failed: %s\n", error.c_str());
        return 1;
    }
    if (hle.memory.size() != kFrames || exact.memory.size() != kFrames) {
        std::printf("hw_tone_hle_exact: %zu and %zu frames replayed, expected %u\n", hle.memory.size(),
                    exact.memory.size(), kFrames);
        return 1;
    }
    uint64_t differingFrames = 0;
    for (uint32_t frame = 0; frame < kFrames; ++frame) {
        differingFrames += hle.memory[frame] != exact.memory[frame] ? 1u : 0u;
    }
    std::printf("  memory: %llu of %u frames differ between the dispatch and the batches\n",
                static_cast<unsigned long long>(differingFrames), kFrames);
    if (differingFrames != 0) ++failures;

    // The dispatch ran for pages 0 and 1 and for page 2's decoded prefix (strips 0..2; strip 3 names a block that is
    // not a palette quadrant and is refused, and the strips after it start at a non-page origin).
    const uint64_t pagesExpected = 3ull * kFrames;
    const uint64_t batchesExpected = 19ull * kFrames;   // 8 + 8 + 3
    const uint64_t deferredExpected = 19ull * kFrames;  // page 2's strips 3..7 are never collected
    std::printf("  counters: pages %llu (expected %llu), batches %llu (expected %llu), deferred %llu (expected %llu)\n",
                static_cast<unsigned long long>(hle.counters.toneStripPages),
                static_cast<unsigned long long>(pagesExpected),
                static_cast<unsigned long long>(hle.counters.toneStripBatches),
                static_cast<unsigned long long>(batchesExpected),
                static_cast<unsigned long long>(hle.counters.toneStripBatchesDeferred),
                static_cast<unsigned long long>(deferredExpected));
    if (hle.counters.toneStripPages != pagesExpected || hle.counters.toneStripBatches != batchesExpected ||
        hle.counters.toneStripBatchesDeferred != deferredExpected) {
        ++failures;
    }
    if (exact.counters.toneStripPages != 0) ++failures;

    // The strip rule read from the same memory (09.4 variant A): index texel -> 07.4 CSM1 palette entry of the
    // strip's quadrant -> RGB, alpha untouched.
    memory::LocalMemory memoryModel;
    uint64_t pixelErrors = 0, alphaErrors = 0, changed = 0;
    const std::vector<uint8_t>& finalMemory = hle.memory[kFrames - 1];
    std::copy(finalMemory.begin(), finalMemory.end(), memoryModel.bytes().begin());
    // The first bytes that differ between the two runs (diagnostic of a coherence or ordering error).
    uint64_t firstDifference = UINT64_MAX;
    uint64_t differingBytes = 0;
    for (size_t i = 0; i < finalMemory.size(); ++i) {
        if (finalMemory[i] != exact.memory[kFrames - 1][i]) {
            if (firstDifference == UINT64_MAX) firstDifference = i;
            ++differingBytes;
        }
    }
    std::printf("  bytes: %llu differ, first at 0x%llx (page %llu)\n", static_cast<unsigned long long>(differingBytes),
                static_cast<unsigned long long>(firstDifference),
                static_cast<unsigned long long>(firstDifference == UINT64_MAX ? 0 : firstDifference / 8192u));
    for (uint32_t page = 0; page < 2; ++page) {
        for (uint32_t strip = 0; strip < 8; ++strip) {
            const uint32_t quadrant = strip;
            for (uint32_t row = 0; row < kRows; ++row) {
                for (uint32_t column = 0; column < 8; ++column) {
                    const uint32_t index =
                        memoryModel.readPixel(memory::BufferView{kIndexBp, 1, memory::Psm::T8}, column, row);
                    const uint32_t entryX = (index & 7u) + 8u * ((index >> 4) & 1u);
                    const uint32_t entryY = 2u * (index >> 5) + ((index >> 3) & 1u);
                    const uint32_t entry = memoryModel.readPixel(
                        memory::BufferView{kTempBp[page], 1, memory::Psm::Ct32}, (quadrant & 3u) * 16u + entryX,
                        (quadrant >> 2) * 16u + entryY);
                    const uint32_t got = memoryModel.readPixel(
                        memory::BufferView{kDestBp[page], 1, memory::Psm::Ct24}, strip * 8u + column, row);
                    // The alpha byte of the same address (a CT24 pixel does not cover it; the CT32 view does).
                    const uint32_t stored = memoryModel.readPixel(
                        memory::BufferView{kDestBp[page], 1, memory::Psm::Ct32}, strip * 8u + column, row);
                    if ((got & 0x00FFFFFFu) != (entry & 0x00FFFFFFu)) {
                        if (pixelErrors < 4) {
                            std::printf("    page %u strip %u pixel %u,%u: %06x, expected %06x\n", page, strip,
                                        strip * 8u + column, row, got & 0x00FFFFFFu, entry & 0x00FFFFFFu);
                        }
                        ++pixelErrors;
                    }
                    if (((stored >> 24) & 0xFFu) != 0x80u) {
                        if (alphaErrors < 5) {
                            std::printf("    alpha page %u pixel %u,%u: %08x\n", page, strip * 8u + column, row, stored);
                        }
                        ++alphaErrors;
                    }
                    if ((got & 0x00FFFFFFu) != 0) ++changed;
                }
            }
        }
    }
    std::printf("  rule: %llu pixels differ, %llu alpha bytes wrong, %llu non-zero pixels\n",
                static_cast<unsigned long long>(pixelErrors), static_cast<unsigned long long>(alphaErrors),
                static_cast<unsigned long long>(changed));
    if (pixelErrors != 0 || alphaErrors != 0 || changed < 2u * 8u * kRows * 8u) ++failures;

    std::printf("hw_tone_hle_exact: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}

// hw_scale_interlaced_present (IN-381): 480i field-mode frames (FFMD = 1, as GT4's menus) presented at an internal
// scale go through the GPU compose and the GPU deinterlace of 13.3/13.4 counted in native rows.
//   - Exactness: the field buffer is filled by EE uploads (1x words, so its scaled image is every native pixel as an
//     s x s block) with per-field lines and a moving half. For every mode (auto = adaptive, weave, bob, blend) and
//     eight VBlanks of alternating fields, the scaled presentation must equal the CPU presentation compose at 1x
//     (field offset and stateful deinterlacer) blown up by nearest, pixel for pixel.
//   - Detail: a sprite drawn into the field buffer must reach the scaled presentation as sub-pixel detail (mixed
//     s x s blocks), which a 1x presentation (candidate 271 for every 480i frame) cannot have.
int TestScaleInterlacedPresent(const gpu::VulkanHandles& handles) {
    // IN-386: this fixture pins the IN-381 interpretation (one algorithm row = one NATIVE row, so a 1x-block image
    // gives exactly the CPU deinterlacer's result blown up). The shipped default is now one OUTPUT row, the
    // reference's behaviour; the fixture sets the old mode explicitly so both stay covered.
    _putenv_s("GT4_HW_DEINTERLACE_ROWS", "native");
    constexpr uint32_t kWidth = 64;
    constexpr uint32_t kRows = 32;  // field buffer rows; the frame is 64 rows
    auto lineColour = [](uint32_t line, uint32_t x, uint32_t frame) {
        const uint32_t moving = x >= kWidth / 2 ? frame * 0x00302010u : 0u;  // the right half changes every VBlank
        return 0x80000000u | (((line * 7u + 3u) & 0xFFu) | (((line * 13u) & 0xFFu) << 8) | ((x * 3u) << 16)) + moving;
    };
    auto upload = [&](writeback::Renderer& renderer, uint32_t fieldLine, uint32_t frame) {
        std::vector<Qword> stream;
        stream.push_back(PackedTag(4, false, 0, {0xE}));
        stream.push_back(Qword{(uint64_t{1} << 16) | (uint64_t{1} << 48), 0x50});  // BITBLTBUF: DBP 0, DBW 1, CT32
        stream.push_back(Qword{0, 0x51});
        stream.push_back(Qword{uint64_t{kWidth} | (uint64_t{kRows} << 32), 0x52});
        stream.push_back(Qword{0, 0x53});
        const uint32_t qwords = kWidth * kRows / 4;
        stream.push_back(PackedTag(qwords, true, 2, {}));
        for (uint32_t i = 0; i < qwords; ++i) {
            const uint32_t row = (i * 4) / kWidth;
            const uint32_t x = (i * 4) % kWidth;
            const uint32_t c0 = lineColour(2 * row + fieldLine, x, frame);
            const uint32_t c1 = lineColour(2 * row + fieldLine, x + 1, frame);
            const uint32_t c2 = lineColour(2 * row + fieldLine, x + 2, frame);
            const uint32_t c3 = lineColour(2 * row + fieldLine, x + 3, frame);
            stream.push_back(Qword{uint64_t{c0} | (uint64_t{c1} << 32), uint64_t{c2} | (uint64_t{c3} << 32)});
        }
        renderer.GifTransfer(1, reinterpret_cast<const uint8_t*>(stream.data()), stream.size() * sizeof(Qword));
    };
    auto drawSprite = [](writeback::Renderer& renderer) {
        const uint64_t rgbaq = uint64_t{0x8020E0F0u} | (uint64_t{0x3F800000u} << 32);
        const std::vector<std::pair<uint64_t, uint64_t>> ad = {
            {(uint64_t{63} << 16) | (uint64_t{31} << 48), 0x40},             // SCISSOR 64 x 32
            {0, 0x18},                                                       // XYOFFSET 0
            {(uint64_t{1} << 16) | (uint64_t{1} << 17), 0x47},               // TEST: ZTE, ALWAYS
            {uint64_t{0} | (uint64_t{1} << 16), 0x4C},                       // FRAME: page 0, FBW 1, CT32
            {uint64_t{30} | (uint64_t{1} << 32), 0x4E},                      // ZBUF masked
            {6, 0x00},                                                       // PRIM: sprite
            {rgbaq, 0x01},
            {uint64_t{9 * 16} | (uint64_t{5 * 16} << 16), 0x05},
            {uint64_t{40 * 16} | (uint64_t{21 * 16} << 16), 0x05}};
        std::vector<Qword> stream;
        stream.push_back(PackedTag(static_cast<uint32_t>(ad.size()), true, 0, {0xE}));
        for (const auto& [data, reg] : ad) stream.push_back(Qword{data, reg});
        renderer.GifTransfer(1, reinterpret_cast<const uint8_t*>(stream.data()), stream.size() * sizeof(Qword));
    };
    auto setup = [&](writeback::Renderer& renderer, display::DeinterlaceMode mode) {
        writeback::OutputSettings settings;
        settings.deinterlace = mode;
        renderer.SetOutputSettings(settings);
        renderer.WritePrivileged(0x12000000u, 1u | (1u << 5) | (0xFFu << 8));  // PMODE: RC1, MMOD 1, ALP 255
        renderer.WritePrivileged(0x12000010u, uint64_t{2} << 13);              // SMODE1: NTSC
        renderer.WritePrivileged(0x12000020u, 3u);                             // SMODE2: INT, FFMD
        renderer.WritePrivileged(0x12000060u, 1u);                             // SYNCV: interlaced signal (13.1)
        renderer.WritePrivileged(0x12000070u, uint64_t{1} << 9);               // DISPFB1: FBP 0, FBW 1, CT32
        renderer.WritePrivileged(0x12000080u, (uint64_t{3} << 23) | (uint64_t{4 * kWidth - 1} << 32) |
                                                  (uint64_t{2 * kRows - 1} << 44));  // DISPLAY1: MAGH 3, 64 x 64
    };
    int failures = 0;
    const struct {
        display::DeinterlaceMode mode;
        const char* name;
    } modes[] = {{display::DeinterlaceMode::Auto, "auto (adaptive)"},
                 {display::DeinterlaceMode::Weave, "weave"},
                 {display::DeinterlaceMode::Bob, "bob"},
                 {display::DeinterlaceMode::Blend, "blend"}};
    for (uint32_t scale : {2u, 4u}) {
        for (const auto& m : modes) {
            writeback::Renderer native;
            writeback::Renderer scaled;
            std::string error;
            scaled.SetScale(scale, static_cast<float>(scale));
            if (!native.Init(handles, error) || !scaled.Init(handles, error)) {
                std::printf("hw_scale_interlaced_present: init failed: %s\n", error.c_str());
                return 1;
            }
            native.SetSyncMode(true);
            scaled.SetSyncMode(true);
            setup(native, m.mode);
            setup(scaled, m.mode);
            uint64_t pixelErrors = 0, gpuFrames = 0, compared = 0;
            for (uint32_t frame = 0; frame < 8; ++frame) {
                const bool odd = (frame & 1u) != 0;
                upload(native, odd ? 1u : 0u, frame);
                upload(scaled, odd ? 1u : 0u, frame);
                native.SetDisplayField(odd);
                scaled.SetDisplayField(odd);
                native.Flush();
                scaled.Flush();
                const writeback::FrameOutput reference = native.Compose(writeback::ComposeKind::Presentation);
                writeback::GpuFrame gpuFrame;
                std::vector<uint32_t> pixels;
                if (!scaled.ComposeFrameGpu(gpuFrame) || !scaled.ReadGpuFrame(gpuFrame, pixels)) continue;
                ++gpuFrames;
                if (!reference.valid || gpuFrame.width != reference.width * scale ||
                    gpuFrame.height != reference.height * scale) {
                    std::printf("    %s scale %u frame %u: size %ux%u vs 1x %ux%u\n", m.name, scale, frame,
                                gpuFrame.width, gpuFrame.height, reference.width, reference.height);
                    ++pixelErrors;
                    continue;
                }
                ++compared;
                for (uint32_t y = 0; y < gpuFrame.height; ++y) {
                    for (uint32_t x = 0; x < gpuFrame.width; ++x) {
                        const uint32_t got = pixels[static_cast<size_t>(y) * gpuFrame.width + x] & 0xFFFFFFu;
                        const uint32_t expected =
                            reference.pixels[static_cast<size_t>(y / scale) * reference.width + x / scale] & 0xFFFFFFu;
                        if (got != expected) {
                            if (pixelErrors < 4) {
                                std::printf("    %s scale %u frame %u pixel %u,%u: %06x, expected %06x\n", m.name, scale,
                                            frame, x, y, got, expected);
                            }
                            ++pixelErrors;
                        }
                    }
                }
            }
            std::printf("  %s scale %u: GPU frames %llu, compared %llu, pixels off the 1x deinterlacer %llu, scaled "
                        "deinterlaces %llu\n",
                        m.name, scale, static_cast<unsigned long long>(gpuFrames),
                        static_cast<unsigned long long>(compared), static_cast<unsigned long long>(pixelErrors),
                        static_cast<unsigned long long>(scaled.GpuCounters().scaledDeinterlaces));
            const bool expectsDeinterlace = true;  // every mode here deinterlaces (auto resolves to adaptive: FFMD)
            if (gpuFrames != 8 || compared != 8 || pixelErrors != 0 ||
                (expectsDeinterlace && scaled.GpuCounters().scaledDeinterlaces != 8)) {
                ++failures;
            }
            if (native.Failed() || scaled.Failed()) {
                std::printf("    renderer failed: %s %s\n", native.LastError().c_str(), scaled.LastError().c_str());
                ++failures;
            }
        }
        // Detail: a sprite drawn into the field buffer shows sub-pixel edges at scale.
        writeback::Renderer scaled;
        std::string error;
        scaled.SetScale(scale, static_cast<float>(scale));
        if (!scaled.Init(handles, error)) return 1;
        scaled.SetSyncMode(true);
        scaled.SetCpuSmallSprites(false);
        setup(scaled, display::DeinterlaceMode::Auto);
        uint64_t mixed = 0;
        bool composed = false;
        for (uint32_t frame = 0; frame < 2; ++frame) {
            upload(scaled, frame & 1u, 0);
            drawSprite(scaled);
            scaled.SetDisplayField((frame & 1u) != 0);
            scaled.Flush();
            writeback::GpuFrame gpuFrame;
            std::vector<uint32_t> pixels;
            composed = scaled.ComposeFrameGpu(gpuFrame) && scaled.ReadGpuFrame(gpuFrame, pixels);
            if (!composed) break;
            mixed = 0;
            for (uint32_t by = 0; by < gpuFrame.height / scale; ++by) {
                for (uint32_t bx = 0; bx < gpuFrame.width / scale; ++bx) {
                    const uint32_t first = pixels[static_cast<size_t>(by * scale) * gpuFrame.width + bx * scale] & 0xFFFFFFu;
                    bool uniform = true;
                    for (uint32_t dy = 0; dy < scale && uniform; ++dy) {
                        for (uint32_t dx = 0; dx < scale && uniform; ++dx) {
                            uniform = (pixels[static_cast<size_t>(by * scale + dy) * gpuFrame.width + bx * scale + dx] &
                                       0xFFFFFFu) == first;
                        }
                    }
                    if (!uniform) ++mixed;
                }
            }
        }
        std::printf("  drawn sprite scale %u: GPU compose %s, mixed blocks %llu\n", scale, composed ? "yes" : "NO",
                    static_cast<unsigned long long>(mixed));
        if (!composed || mixed == 0 || scaled.Failed()) ++failures;
    }
    std::printf("hw_scale_interlaced_present: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}

// hw_postfx_fxaa (IN-376, chapter 13.5): a dark buffer with one bright flat triangle whose long edges are steep
// and shallow, composed on the GPU at 1x and 2x with and without FXAA. Edge-only FXAA must leave every pixel away
// from the edges unchanged, change pixels along them, and only produce colours between the two flat colours.
int TestPostfxFxaa(const gpu::VulkanHandles& handles) {
    auto rgbaq = [](uint32_t colour) { return uint64_t{colour} | (uint64_t{0x3F800000u} << 32); };
    auto xy = [](uint32_t x16, uint32_t y16) { return uint64_t{x16} | (uint64_t{y16} << 16); };
    const uint64_t kSprite = 6, kTriangle = 3;
    cpuref::CaptureWriter capture;
    capture.Privileged(0x12000000u, 1u | (1u << 5) | (0xFFu << 8));                // PMODE: RC1, MMOD 1, ALP 255
    capture.Privileged(0x12000010u, uint64_t{2} << 13);                             // SMODE1: NTSC
    capture.Privileged(0x12000070u, uint64_t{0} | (uint64_t{1} << 9));             // DISPFB1: page 0, FBW 1
    capture.Privileged(0x12000080u, (uint64_t{3} << 23) | (uint64_t{255} << 32) | (uint64_t{63} << 44));  // 64 x 64
    std::vector<uint64_t> words = {(uint64_t{63} << 16) | (uint64_t{63} << 48), 0x40, 0, 0x18,
                                   (uint64_t{1} << 16) | (uint64_t{1} << 17), 0x47,  // TEST: ZTE, ALWAYS
                                   uint64_t{1} << 16, 0x4C, 8 | (uint64_t{1} << 32), 0x4E,
                                   kSprite, 0x00, rgbaq(0x80101010u), 0x01, xy(0, 0), 0x05, xy(64 * 16, 64 * 16), 0x05,
                                   kTriangle, 0x00, rgbaq(0x80F0F0F0u), 0x01, xy(4 * 16 + 3, 3 * 16), 0x05,
                                   xy(60 * 16, 21 * 16 + 5), 0x05, xy(17 * 16 + 9, 61 * 16), 0x05};
    capture.Gif(1, cpuref::CaptureWriter::Packet(static_cast<uint32_t>(words.size() / 2), true, false, 0, 0, {0xE}, words));
    capture.FrameEnd(0);

    auto compose = [&](uint32_t scale, bool fxaa, std::vector<uint32_t>& pixels, uint32_t& width, std::string& error) {
        writeback::Renderer renderer;
        renderer.SetScale(scale, static_cast<float>(scale));
        if (!renderer.Init(handles, error)) return false;
        renderer.SetSyncMode(true);
        renderer.SetFxaa(fxaa);
        writeback::WritebackReplayTarget target(renderer, writeback::ComposeKind::Reference);
        cpuref::ReplaySummary summary;
        bool got = false;
        const bool ok = cpuref::ReplayCapture(capture.Bytes(), target, summary,
                                              [&](uint64_t, const cpuref::FrameImage&) {
                                                  writeback::GpuFrame frame;
                                                  got = renderer.ComposeFrameGpu(frame) && frame.fxaa == fxaa &&
                                                        renderer.ReadGpuFrame(frame, pixels);
                                                  width = frame.width;
                                              },
                                              error);
        if (renderer.Failed()) error += " " + renderer.LastError();
        return ok && got && !renderer.Failed();
    };
    int failures = 0;
    for (uint32_t scale : {1u, 2u}) {
        std::vector<uint32_t> plain, smooth;
        uint32_t width = 0, width2 = 0;
        std::string error;
        if (!compose(scale, false, plain, width, error) || !compose(scale, true, smooth, width2, error) ||
            plain.size() != smooth.size() || width != width2 || width == 0) {
            std::printf("hw_postfx_fxaa: scale %u compose failed: %s\n", scale, error.c_str());
            ++failures;
            continue;
        }
        const uint32_t height = static_cast<uint32_t>(plain.size() / width);
        uint64_t changed = 0, changedAwayFromEdges = 0, outOfRange = 0;
        for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = 0; x < width; ++x) {
                const uint32_t a = plain[y * width + x] & 0xFFFFFFu;
                const uint32_t b = smooth[y * width + x] & 0xFFFFFFu;
                if (a == b) continue;
                ++changed;
                // Edge pixels: some 3 x 3 neighbour differs in the plain image.
                bool edge = false;
                for (int dy = -1; dy <= 1 && !edge; ++dy) {
                    for (int dx = -1; dx <= 1 && !edge; ++dx) {
                        const int nx = std::clamp(static_cast<int>(x) + dx, 0, static_cast<int>(width) - 1);
                        const int ny = std::clamp(static_cast<int>(y) + dy, 0, static_cast<int>(height) - 1);
                        edge = (plain[static_cast<size_t>(ny) * width + nx] & 0xFFFFFFu) != a;
                    }
                }
                if (!edge) ++changedAwayFromEdges;
                for (int c = 0; c < 3; ++c) {
                    const uint32_t v = (b >> (8 * c)) & 0xFFu;
                    if (v < 0x10u || v > 0xF0u) ++outOfRange;
                }
            }
        }
        std::printf("  scale %u: %ux%u, %llu pixels smoothed, %llu changed away from edges, %llu channels outside "
                    "the two colours\n", scale, width, height, static_cast<unsigned long long>(changed),
                    static_cast<unsigned long long>(changedAwayFromEdges), static_cast<unsigned long long>(outOfRange));
        if (changed < 20 || changedAwayFromEdges != 0 || outOfRange != 0) ++failures;
    }
    std::printf("hw_postfx_fxaa: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}

// hw_glm_unswizzle (IN-350): random local memory in the GLM is loaded into a colour or depth slot at a target
// layout; the slot must then hold LocalMemory::readRect of the same rectangle (what a load from the shadow memory
// would upload). Then a GLM download of a few page ranges must give back the GLM bytes at the same offsets.
int TestGlmUnswizzle(gpu::GpuDrawer& drawer) {
    struct Case {
        memory::Psm psm;
        bool depth;
        uint32_t bp;
        uint32_t bw;
    };
    const Case cases[] = {{memory::Psm::Ct32, false, 64, 10},  {memory::Psm::Ct24, false, 320, 4},
                          {memory::Psm::Ct16, false, 1000, 3}, {memory::Psm::Ct16S, false, 37, 3},
                          {memory::Psm::Z32, true, 640, 10},   {memory::Psm::Z24, true, 2048, 4},
                          {memory::Psm::Z16, true, 5000, 3},   {memory::Psm::Z16S, true, 7001, 3}};
    uint64_t state = 0x0B5Eu;
    auto next = [&] {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<uint32_t>(state >> 32);
    };
    int failures = 0;
    memory::LocalMemory local;
    for (const Case& c : cases) {
        for (uint32_t address = 0; address < 4u * 1024u * 1024u; address += 4) {
            const uint32_t word = next();
            for (uint32_t b = 0; b < 4; ++b) local.setByte(address + b, static_cast<uint8_t>(word >> (8 * b)));
        }
        std::string error;
        if (!drawer.UploadGlm(local.bytes().data(), {{0u, 4u * 1024u * 1024u}}, error)) {
            std::printf("hw_glm_unswizzle: upload failed: %s\n", error.c_str());
            return 1;
        }
        const int32_t x0 = 3, y0 = 5, x1 = 140, y1 = 77;
        const uint32_t width = static_cast<uint32_t>(x1 - x0 + 1);
        const uint32_t height = static_cast<uint32_t>(y1 - y0 + 1);
        gpu::GpuChunk chunk;
        chunk.rectX0 = x0;
        chunk.rectY0 = y0;
        chunk.rectX1 = x1;
        chunk.rectY1 = y1;
        chunk.colourSlot = 2;
        chunk.depthSlot = 1;
        chunk.uploadFrame = chunk.uploadDepth = false;
        chunk.readbackFrame = !c.depth;
        chunk.readbackDepth = c.depth;
        (c.depth ? chunk.depthWords : chunk.frameWords).resize(static_cast<size_t>(width) * height);
        if (!drawer.LoadSlotFromGlm(c.depth, c.depth ? 1u : 2u, c.bp, c.bw, static_cast<uint32_t>(c.psm), x0, y0, x1, y1,
                                    error) ||
            !drawer.Execute(chunk, error)) {
            std::printf("hw_glm_unswizzle: GPU error: %s\n", error.c_str());
            return 1;
        }
        std::vector<uint32_t> expected(static_cast<size_t>(width) * height);
        local.readRect(memory::BufferView{c.bp, c.bw, c.psm}, static_cast<uint32_t>(x0), static_cast<uint32_t>(y0), width,
                       height, expected.data());
        const std::vector<uint32_t>& got = c.depth ? chunk.depthWords : chunk.frameWords;
        size_t differing = 0;
        for (size_t i = 0; i < expected.size(); ++i) differing += got[i] != expected[i] ? 1u : 0u;
        std::printf("  psm %02x (%s slot): %zu of %zu pixels differ from readRect\n", static_cast<unsigned>(c.psm),
                    c.depth ? "depth" : "colour", differing, expected.size());
        if (differing != 0) ++failures;
    }
    // Download round trip: three page ranges (one at the end of memory) into a zeroed buffer.
    std::vector<uint8_t> downloaded(4u * 1024u * 1024u, 0);
    const std::vector<std::pair<uint32_t, uint32_t>> ranges = {
        {0u, 8192u}, {37u * 8192u, 3u * 8192u}, {4u * 1024u * 1024u - 8192u, 8192u}};
    std::string error;
    if (!drawer.DownloadGlm(ranges, downloaded.data(), error)) {
        std::printf("hw_glm_unswizzle: download failed: %s\n", error.c_str());
        return 1;
    }
    size_t wrong = 0;
    for (size_t i = 0; i < downloaded.size(); ++i) {
        bool inside = false;
        for (const auto& range : ranges) inside = inside || (i >= range.first && i < range.first + range.second);
        const uint8_t expected = inside ? local.bytes()[i] : uint8_t{0};
        wrong += downloaded[i] != expected ? 1u : 0u;
    }
    std::printf("  download: %zu of %zu bytes wrong\n", wrong, downloaded.size());
    if (wrong != 0) ++failures;
    std::printf("hw_glm_unswizzle: %d failing cases\n", failures);
    return failures == 0 ? 0 : 1;
}

// hw_glm_upload_merge (IN-350): random (word, value, bit mask) triples merged into a random GLM must give
// glm = (glm & ~mask) | (value & mask) for exactly those words, every other word unchanged.
int TestGlmUploadMerge(gpu::GpuDrawer& drawer) {
    uint64_t state = 0x3E26u;
    auto next = [&] {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<uint32_t>(state >> 32);
    };
    std::vector<uint8_t> memory(4u * 1024u * 1024u);
    for (uint8_t& byte : memory) byte = static_cast<uint8_t>(next());
    std::string error;
    if (!drawer.UploadGlm(memory.data(), {{0u, 4u * 1024u * 1024u}}, error)) {
        std::printf("hw_glm_upload_merge: upload failed: %s\n", error.c_str());
        return 1;
    }
    std::vector<uint32_t> expected(memory.size() / 4);
    std::memcpy(expected.data(), memory.data(), memory.size());
    std::vector<uint32_t> triples;
    std::vector<bool> used(expected.size(), false);
    for (int i = 0; i < 20000; ++i) {
        const uint32_t word = next() % static_cast<uint32_t>(expected.size());
        if (used[word]) continue;
        used[word] = true;
        const uint32_t masks[] = {0xFFFFFFFFu, 0x0000FFFFu, 0xFFFF0000u, 0x0000000Fu, 0xF0F00F0Fu, next()};
        const uint32_t mask = masks[next() % 6u];
        const uint32_t value = next();
        triples.insert(triples.end(), {word, value, mask});
        expected[word] = (expected[word] & ~mask) | (value & mask);
    }
    std::vector<uint32_t> glm;
    if (!drawer.MergeIntoGlm(triples, error) || !drawer.ReadGlm(glm, error)) {
        std::printf("hw_glm_upload_merge: GPU error: %s\n", error.c_str());
        return 1;
    }
    size_t wrong = 0;
    for (size_t i = 0; i < expected.size(); ++i) wrong += glm[i] != expected[i] ? 1u : 0u;
    std::printf("  %zu merged words, %zu of %zu GLM words wrong\n", triples.size() / 3, wrong, expected.size());
    std::printf("hw_glm_upload_merge: %s\n", wrong == 0 ? "pass" : "FAIL");
    return wrong == 0 ? 0 : 1;
}

// hw_glm_host_upload (IN-351): EE->GS uploads written into the GLM by the upload kernel must equal
// memory::hostToLocal on the same bytes, for every transfer size (32, 24, 16, 8, 4 bits, the high-byte and
// high/low-nibble 32-bit formats, depth formats), odd origins and widths, and a partial transfer that ends inside
// the rectangle. The whole 4 MiB is compared after each upload.
int TestGlmHostUpload(gpu::GpuDrawer& drawer) {
    struct Case {
        uint32_t psm;
        uint32_t bp;
        uint32_t bw;
        uint32_t x;
        uint32_t y;
        uint32_t width;
        uint32_t height;
        bool partial;
    };
    const Case cases[] = {{0x00, 64, 10, 3, 5, 61, 17, false},   {0x01, 320, 4, 1, 2, 33, 9, false},
                          {0x02, 1000, 3, 7, 1, 25, 40, false},  {0x0A, 37, 3, 2, 3, 30, 31, true},
                          {0x13, 2048, 4, 5, 9, 77, 21, false},  {0x14, 5000, 2, 3, 7, 101, 13, false},
                          {0x1B, 7001, 2, 9, 4, 50, 19, false},  {0x24, 7200, 2, 1, 1, 63, 11, false},
                          {0x2C, 7400, 2, 4, 6, 64, 8, true},    {0x30, 9000, 10, 6, 3, 300, 12, false},
                          {0x31, 9400, 4, 2, 2, 129, 7, false},  {0x3A, 9800, 2, 3, 1, 90, 30, false}};
    uint64_t state = 0x7A11u;
    auto next = [&] {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<uint32_t>(state >> 32);
    };
    memory::LocalMemory local;
    for (uint32_t address = 0; address < 4u * 1024u * 1024u; address += 4) {
        const uint32_t word = next();
        for (uint32_t b = 0; b < 4; ++b) local.setByte(address + b, static_cast<uint8_t>(word >> (8 * b)));
    }
    std::string error;
    if (!drawer.UploadGlm(local.bytes().data(), {{0u, 4u * 1024u * 1024u}}, error)) {
        std::printf("hw_glm_host_upload: upload failed: %s\n", error.c_str());
        return 1;
    }
    int failures = 0;
    for (const Case& c : cases) {
        const auto psm = memory::decodePsm(c.psm);
        if (!psm) {
            std::printf("  psm %02x: not a PSM\n", c.psm);
            ++failures;
            continue;
        }
        const uint32_t bpp = memory::psmInfo(*psm).transferBpp;
        size_t bytes = (static_cast<size_t>(c.width) * c.height * bpp + 7) / 8;
        if (c.partial) bytes = bytes / 2 + 3;  // ends inside the rectangle, not on a texel boundary
        std::vector<uint8_t> data(bytes);
        for (uint8_t& byte : data) byte = static_cast<uint8_t>(next());
        const uint32_t pixels = static_cast<uint32_t>(
            std::min<uint64_t>(static_cast<uint64_t>(bytes) * 8u / bpp, static_cast<uint64_t>(c.width) * c.height));
        if (!drawer.UploadHostToGlm(c.bp, c.bw, c.psm, c.x, c.y, c.width, pixels, bpp, data.data(), data.size(), error)) {
            std::printf("hw_glm_host_upload: GPU error: %s\n", error.c_str());
            return 1;
        }
        memory::hostToLocal(local, memory::BufferView{c.bp, c.bw, *psm}, memory::TransferRect{c.x, c.y, c.width, c.height},
                            std::span<const uint8_t>(data.data(), data.size()));
        std::vector<uint32_t> glm;
        if (!drawer.ReadGlm(glm, error)) {
            std::printf("hw_glm_host_upload: read failed: %s\n", error.c_str());
            return 1;
        }
        size_t differing = 0;
        for (size_t i = 0; i < glm.size(); ++i) {
            uint32_t expected = 0;
            std::memcpy(&expected, local.bytes().data() + 4 * i, 4);
            differing += glm[i] != expected ? 1u : 0u;
        }
        std::printf("  psm %02x %ux%u at (%u,%u)%s: %zu GLM words differ from hostToLocal\n", c.psm, c.width, c.height,
                    c.x, c.y, c.partial ? " partial" : "", differing);
        if (differing != 0) ++failures;
    }
    std::printf("hw_glm_host_upload: %d failing cases\n", failures);
    return failures == 0 ? 0 : 1;
}

// IN-347 GLM kernels against the CPU memory model (chapter 04.9 fixture hw_glm_equals_writeback, split in two).
// hw_glm_decode: random local memory uploaded to the GLM; for every PSM, several bases and widths, the GPU
// decode of a 96 x 80 extent equals LocalMemory::readPixel (indexed formats: through a random palette).
int TestGlmDecode(gpu::GpuDrawer& drawer) {
    memory::LocalMemory local;
    uint64_t state = 0x61A5u;
    auto next = [&] {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<uint32_t>(state >> 32);
    };
    for (uint32_t address = 0; address < 4u * 1024u * 1024u; address += 4) {
        const uint32_t word = next();
        for (uint32_t b = 0; b < 4; ++b) local.setByte(address + b, static_cast<uint8_t>(word >> (8 * b)));
    }
    std::string error;
    if (!drawer.UploadGlm(local.bytes().data(), {{0u, 4u * 1024u * 1024u}}, error)) {
        std::printf("hw_glm_decode: upload failed: %s\n", error.c_str());
        return 1;
    }
    std::vector<uint32_t> palette(256);
    for (uint32_t& entry : palette) entry = next();
    const memory::Psm psms[] = {memory::Psm::Ct32, memory::Psm::Ct24, memory::Psm::Ct16, memory::Psm::Ct16S,
                                memory::Psm::T8,   memory::Psm::T4,   memory::Psm::T8H,  memory::Psm::T4Hl,
                                memory::Psm::T4Hh, memory::Psm::Z32,  memory::Psm::Z24,  memory::Psm::Z16,
                                memory::Psm::Z16S};
    const uint32_t bases[] = {0u, 37u, 9000u, 16370u};
    const uint32_t widths[] = {1u, 2u, 10u};
    constexpr uint32_t kW = 96, kH = 80;
    uint64_t compared = 0, mismatches = 0;
    for (memory::Psm psm : psms) {
        const bool indexed = memory::psmInfo(psm).isIndexed;
        for (uint32_t bp : bases) {
            for (uint32_t bw : widths) {
                gpu::GpuChunk chunk;
                chunk.rectX0 = chunk.rectY0 = 0;
                chunk.rectX1 = chunk.rectY1 = 0;
                chunk.uploadFrame = chunk.uploadDepth = chunk.readbackFrame = chunk.readbackDepth = false;
                chunk.hasTexture = true;
                chunk.glmTexture = true;
                chunk.glmBp = bp;
                chunk.glmBw = bw;
                chunk.glmPsm = static_cast<uint32_t>(psm);
                chunk.glmIndexed = indexed;
                chunk.palette = palette;
                chunk.texWidth = kW;
                chunk.texHeight = kH;
                std::vector<uint32_t> texels;
                if (!drawer.Execute(chunk, error) || !drawer.ReadTexels(kW, kH, texels, error)) {
                    std::printf("hw_glm_decode: GPU error: %s\n", error.c_str());
                    return 1;
                }
                const memory::BufferView view{bp, bw, psm};
                for (uint32_t y = 0; y < kH; ++y) {
                    for (uint32_t x = 0; x < kW; ++x) {
                        uint32_t expected = local.readPixel(view, x, y);
                        if (indexed) expected = palette[expected & 255u];
                        const uint32_t got = texels[static_cast<size_t>(y) * kW + x];
                        ++compared;
                        if (got != expected) {
                            if (mismatches < 4) {
                                std::printf("  mismatch psm=%02x bp=%u bw=%u at %u,%u: gpu %08X cpu %08X\n",
                                            static_cast<unsigned>(psm), bp, bw, x, y, got, expected);
                            }
                            ++mismatches;
                        }
                    }
                }
            }
        }
    }
    std::printf("hw_glm_decode: compared %llu texels over 13 PSMs, mismatches %llu\n",
                static_cast<unsigned long long>(compared), static_cast<unsigned long long>(mismatches));
    return mismatches == 0 ? 0 : 1;
}

// hw_glm_swizzle: random target words in a colour or depth slot are swizzled into the GLM at the target's own
// (BP, BW, PSM); the whole 4 MiB GLM must then equal the local memory after LocalMemory::writeRect of the same
// words (only the pixel bits change; neighbouring sub-word pixels survive).
// The rectangle stays inside the buffer width: wider rectangles wrap into the next page row, two pixels then
// share an address, and the last writer depends on the visiting order (a parallel swizzle has none).
int TestGlmSwizzle(gpu::GpuDrawer& drawer) {
    struct Case {
        memory::Psm psm;
        bool depth;
        uint32_t bp;
        uint32_t bw;
    };
    const Case cases[] = {{memory::Psm::Ct32, false, 64, 10},  {memory::Psm::Ct24, false, 320, 4},
                          {memory::Psm::Ct16, false, 1000, 3}, {memory::Psm::Ct16S, false, 37, 3},
                          {memory::Psm::Z32, true, 640, 10},   {memory::Psm::Z24, true, 2048, 4},
                          {memory::Psm::Z16, true, 5000, 3},   {memory::Psm::Z16S, true, 7001, 3}};
    uint64_t state = 0x5A11u;
    auto next = [&] {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<uint32_t>(state >> 32);
    };
    int failures = 0;
    for (const Case& c : cases) {
        memory::LocalMemory local;
        for (uint32_t address = 0; address < 4u * 1024u * 1024u; address += 4) {
            const uint32_t word = next();
            for (uint32_t b = 0; b < 4; ++b) local.setByte(address + b, static_cast<uint8_t>(word >> (8 * b)));
        }
        std::string error;
        if (!drawer.UploadGlm(local.bytes().data(), {{0u, 4u * 1024u * 1024u}}, error)) {
            std::printf("hw_glm_swizzle: upload failed: %s\n", error.c_str());
            return 1;
        }
        const int32_t x0 = 3, y0 = 5, x1 = 140, y1 = 77;
        const size_t words = static_cast<size_t>(x1 - x0 + 1) * static_cast<size_t>(y1 - y0 + 1);
        std::vector<uint32_t> values(words);
        for (uint32_t& v : values) v = next();
        gpu::GpuChunk chunk;
        chunk.rectX0 = x0;
        chunk.rectY0 = y0;
        chunk.rectX1 = x1;
        chunk.rectY1 = y1;
        chunk.colourSlot = 1;
        chunk.depthSlot = 1;
        chunk.uploadFrame = !c.depth;
        chunk.uploadDepth = c.depth;
        chunk.readbackFrame = chunk.readbackDepth = false;
        (c.depth ? chunk.depthWords : chunk.frameWords) = values;
        std::vector<uint32_t> glm;
        if (!drawer.Execute(chunk, error) ||
            !drawer.SwizzleToGlm(c.depth, 1, c.bp, c.bw, static_cast<uint32_t>(c.psm), x0, y0, x1, y1, error) ||
            !drawer.ReadGlm(glm, error)) {
            std::printf("hw_glm_swizzle: GPU error: %s\n", error.c_str());
            return 1;
        }
        local.writeRect(memory::BufferView{c.bp, c.bw, c.psm}, static_cast<uint32_t>(x0), static_cast<uint32_t>(y0),
                        static_cast<uint32_t>(x1 - x0 + 1), static_cast<uint32_t>(y1 - y0 + 1), values.data());
        size_t differing = 0;
        for (size_t i = 0; i < glm.size(); ++i) {
            uint32_t expected = 0;
            std::memcpy(&expected, local.bytes().data() + 4 * i, 4);
            if (glm[i] != expected) ++differing;
        }
        std::printf("  psm %02x (%s slot): %zu of %zu GLM words differ from the write-back\n",
                    static_cast<unsigned>(c.psm), c.depth ? "depth" : "colour", differing, glm.size());
        if (differing != 0) ++failures;
    }
    std::printf("hw_glm_swizzle: %d failing cases\n", failures);
    return failures == 0 ? 0 : 1;
}

// IN-349 GPU CLUT (chapter 07.7) against the CPU CLUT unit: a sequence of 40 random CLUT loads (CT32, CT16,
// CT16S; CSM1 and CSM2; T8 and T4 palettes; CSA 0..31; aligned and unaligned CBP) is applied to the CPU
// clut::ClutUnit from local memory and to the GPU CLUT buffer from the GLM; after every load the 512 halfwords
// must be equal. Then a T8 and a T4 texture decoded on the GPU through the GPU CLUT (with TEXA for CT16) must
// equal the CPU decode through ClutUnit::Decode.
int TestGlmClut(gpu::GpuDrawer& drawer) {
    memory::LocalMemory local;
    uint64_t state = 0xC107u;
    auto next = [&] {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<uint32_t>(state >> 32);
    };
    for (uint32_t address = 0; address < 4u * 1024u * 1024u; address += 4) {
        const uint32_t word = next();
        for (uint32_t b = 0; b < 4; ++b) local.setByte(address + b, static_cast<uint8_t>(word >> (8 * b)));
    }
    std::string error;
    if (!drawer.UploadGlm(local.bytes().data(), {{0u, 4u * 1024u * 1024u}}, error)) {
        std::printf("hw_glm_clut: upload failed: %s\n", error.c_str());
        return 1;
    }
    CounterSet counterSet;
    clut::ClutUnit cpu(counterSet);
    if (!drawer.UploadClut(cpu.Halfwords(), error)) {
        std::printf("hw_glm_clut: CLUT upload failed: %s\n", error.c_str());
        return 1;
    }
    const uint32_t cpsms[] = {0x00u, 0x02u, 0x0Au};
    int failures = 0;
    clut::Tex0Write last;
    for (int load = 0; load < 40; ++load) {
        clut::Tex0Write write;
        write.psm = (next() & 1u) ? 0x13u : 0x14u;  // T8 or T4
        write.cpsm = cpsms[next() % 3u];
        write.cbp = (load % 3 == 0) ? (next() % 16000u) : (next() % 500u) * 32u;
        write.csm = (load % 5 == 4) ? 1u : 0u;
        write.csa = next() & 31u;
        write.cld = 1;
        write.texclutCbw = 1u + next() % 4u;
        write.texclutCou = next() % 4u;
        write.texclutCov = next() % 300u;
        cpu.ProcessWrite(write, local.bytes().data(), {});
        if (!drawer.LoadClutFromGlm(write.cbp, write.cpsm, write.csm, write.csa, write.psm, write.texclutCbw,
                                    write.texclutCou, write.texclutCov, error)) {
            std::printf("hw_glm_clut: GPU load failed: %s\n", error.c_str());
            return 1;
        }
        std::vector<uint16_t> gpu;
        if (!drawer.ReadClut(gpu, error)) {
            std::printf("hw_glm_clut: read failed: %s\n", error.c_str());
            return 1;
        }
        uint32_t differing = 0;
        for (uint32_t i = 0; i < 512; ++i) differing += gpu[i] != cpu.Halfwords()[i] ? 1u : 0u;
        if (differing != 0) {
            std::printf("  load %d (cpsm %02x csm %u csa %u psm %02x cbp %u): %u halfwords differ\n", load, write.cpsm,
                        write.csm, write.csa, write.psm, write.cbp, differing);
            ++failures;
        }
        last = write;
    }
    // Indexed decode through the GPU CLUT equals the CPU palette for both index sizes and both palette formats.
    const uint64_t texa = uint64_t{0x55} | (uint64_t{1} << 15) | (uint64_t{0xAA} << 32);  // TA0, AEM, TA1
    const struct {
        uint32_t psm;
        uint32_t cpsm;
        uint32_t csa;
    } decodes[] = {{0x13u, 0x00u, 3u}, {0x14u, 0x00u, 9u}, {0x13u, 0x02u, 7u}, {0x14u, 0x0Au, 21u}};
    for (const auto& d : decodes) {
        clut::Tex0Write write;
        write.psm = d.psm;
        write.cpsm = d.cpsm;
        write.cbp = 4096u;
        write.csa = d.csa;
        write.cld = 1;
        cpu.ProcessWrite(write, local.bytes().data(), {});
        if (!drawer.LoadClutFromGlm(write.cbp, write.cpsm, 0, write.csa, write.psm, 0, 0, 0, error)) return 1;
        const clut::Palette palette = cpu.Decode(d.psm, d.cpsm, d.csa, texa);
        const clut::TexaState texaState = clut::TexaState::FromRegister(texa);
        gpu::GpuChunk chunk;
        chunk.rectX0 = chunk.rectY0 = chunk.rectX1 = chunk.rectY1 = 0;
        chunk.uploadFrame = chunk.uploadDepth = chunk.readbackFrame = chunk.readbackDepth = false;
        chunk.hasTexture = true;
        chunk.glmTexture = true;
        chunk.glmBp = 8000u;
        chunk.glmBw = 2u;
        chunk.glmPsm = d.psm;
        chunk.glmIndexed = true;
        chunk.glmGpuClut = true;
        chunk.glmCpsm = d.cpsm;
        chunk.glmCsa = d.csa;
        chunk.glmTa0 = texaState.ta0;
        chunk.glmTa1 = texaState.ta1;
        chunk.glmAem = texaState.aem ? 1u : 0u;
        chunk.texWidth = 64;
        chunk.texHeight = 32;
        std::vector<uint32_t> texels;
        if (!drawer.Execute(chunk, error) || !drawer.ReadTexels(64, 32, texels, error)) {
            std::printf("hw_glm_clut: decode failed: %s\n", error.c_str());
            return 1;
        }
        uint32_t wrong = 0;
        const memory::BufferView view{8000u, 2u, *memory::decodePsm(d.psm)};
        for (uint32_t y = 0; y < 32; ++y) {
            for (uint32_t x = 0; x < 64; ++x) {
                const uint32_t expected = palette.entries[local.readPixel(view, x, y) & 255u];
                wrong += texels[static_cast<size_t>(y) * 64 + x] != expected ? 1u : 0u;
            }
        }
        std::printf("  decode psm %02x cpsm %02x csa %u: %u of 2048 texels differ\n", d.psm, d.cpsm, d.csa, wrong);
        if (wrong != 0) ++failures;
    }
    (void)last;
    std::printf("hw_glm_clut: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}

int TestGouraudReport(gpu::GpuDrawer& drawer) {
    // Report only (IN-500): Gouraud interpolation rounding is not asserted yet.
    Setup s;
    s.iip = true;
    std::vector<Vertex> tri = {MakeVertex(0, 0, 255, 0, 0, 255), MakeVertex(40, 0, 0, 255, 0, 255),
                               MakeVertex(0, 40, 0, 0, 255, 255)};
    Batch batch = MakeBatch(s, tri, {PrimType::Triangle});
    CaseResult r = RunCase(drawer, s, batch, 16);
    Report("hw_gpu_gouraud_report", r);
    return r.gpuError ? 1 : 0;
}

} // namespace

int main(int argc, char** argv) {
    const std::string name = argc > 1 ? argv[1] : "";
    if (name == "hw_flat_provoking") return TestFlatProvoking();
    if (name == "hw_sprite_corners") return TestSpriteCorners();
    if (name == "hw_present_scheduler") return TestPresentScheduler();
    if (name == "hw_present_scheduler_hold") return TestPresentSchedulerHold();
    if (name == "hw_present_scheduler_rate") return TestPresentSchedulerRate();
    if (name == "hw_aux_frame_guest_untouched") return TestAuxFrameGuestUntouched();
    if (name == "hw_pass_timings") return TestPassTimings();
    if (name == "hw_presentation_field_phase") return TestPresentationFieldPhase();

    Headless headless;
    std::string error;
    if (const char* off = std::getenv("GT4_HW_TEST_NO_INTERLOCK"); off && std::strcmp(off, "1") == 0) {
        headless.wantInterlock = false;  // IN-345: run the per-primitive path
    }
    if (!headless.Create(error)) {
        std::printf("%s: skipped (%s)\n", name.c_str(), error.c_str());
        return 77;
    }
    gpu::VulkanHandles handles;
    handles.instance = headless.instance;
    handles.physical = headless.physical;
    handles.device = headless.device;
    handles.queue = headless.queue;
    handles.queueFamily = headless.family;
    handles.pixelInterlock = headless.interlock;
    gpu::GpuDrawer drawer;
    if (!drawer.Init(handles, error)) {
        std::printf("%s: GPU drawer init failed: %s\n", name.c_str(), error.c_str());
        return 1;
    }
    if (name == "hw_gpu_coverage_vectors") return TestCoverageVectors(drawer);
    if (name == "hw_gpu_equivalence_flat") return TestEquivalenceFlat(drawer);
    if (name == "hw_gpu_equivalence_texture") return TestEquivalenceTexture(drawer);
    if (name == "hw_gpu_equivalence_region") return TestEquivalenceWrapRegion(drawer);
    if (name == "hw_gpu_equivalence_bilinear") return TestEquivalenceBilinear(drawer);
    if (name == "hw_gpu_equivalence_tfx_fog") return TestEquivalenceTfxFog(drawer);
    if (name == "hw_gpu_equivalence_depth_alpha") return TestEquivalenceDepthAlpha(drawer);
    if (name == "hw_gpu_equivalence_scissor") return TestEquivalenceScissor(drawer);
    if (name == "hw_gpu_raster_blend") return TestRasterBlendModes(drawer);
    if (name == "hw_gpu_raster_pabe") return TestRasterPabe(drawer);
    if (name == "hw_gpu_raster_atest") return TestRasterAlphaTest(drawer);
    if (name == "hw_gpu_raster_date") return TestRasterDate(drawer);
    if (name == "hw_gpu_raster_frame") return TestRasterFrameWrites(drawer);
    if (name == "hw_gpu_gouraud_report") return TestGouraudReport(drawer);
    if (name == "hw_glm_decode") return TestGlmDecode(drawer);
    if (name == "hw_glm_swizzle") return TestGlmSwizzle(drawer);
    if (name == "hw_glm_unswizzle") return TestGlmUnswizzle(drawer);
    if (name == "hw_glm_upload_merge") return TestGlmUploadMerge(drawer);
    if (name == "hw_glm_host_upload") return TestGlmHostUpload(drawer);
    if (name == "hw_glm_clut") return TestGlmClut(drawer);
    if (name == "hw_gpu_writeback_aliased_depth") return TestWritebackAliasedDepth(drawer);
    if (name == "hw_readback_pending_draw") return TestReadbackObservesPendingDraw(handles);
    if (name == "hw_replay_parity_double_buffer") return TestReplayParityDoubleBuffer(handles);
    if (name == "hw_cache_equals_writeback") return TestCacheEqualsWriteback(handles);
    if (name == "hw_scale_exact") return TestScaleExact(handles);
    if (name == "hw_scale_target_extent") return TestScaleTargetExtent(handles);
    if (name == "hw_postfx_fxaa") return TestPostfxFxaa(handles);
    if (name == "hw_scale_display_shadow") return TestScaleDisplayShadow(handles);
    if (name == "hw_scale_fullscreen_passes") return TestScaleFullscreenPasses(handles);
    if (name == "hw_tone_hle_exact") return TestToneHleExact(handles);
    if (name == "hw_scale_interlaced_present") return TestScaleInterlacedPresent(handles);
    std::printf("unknown test: %s\n", name.c_str());
    return 2;
}
