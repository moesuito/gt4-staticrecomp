// SPDX-License-Identifier: MIT
//
// HWR uber pixel shader (TASK-027, M4). Copyright (c) 2026 GT4Recomp contributors.
//
// Written from docs/hw-renderer-spec chapters 03.4, 03.5, 08.8 and 12.6, clean room (decision 0069).
//
// Destination reads use storage images (r32ui frame words and depth words) rather than input
// attachments: the target is written by imageStore in the same invocation after the read, so no
// colour attachment is needed (fragmentStoresAndAtomics, chapter 12.6). Overlap ordering between
// primitives is handled by the caller (one draw per primitive with a barrier between draws).
//
// Texture source (M4, write-back mode): every texel is decoded on the CPU from the shadow local
// memory into RGBA8 (palette and TEXA already applied) and stored as one packed u32 per texel in
// an r32ui image of 2^TW x 2^TH. This shader does the coordinate step (FST, or STQ with the
// per-pixel divide), the wrap modes of 03.5 and nearest or 4-tap bilinear filtering with the
// integer 4-bit weights of the CPU reference (IN-305), then the TFX and fog stages of 03.4.
// Colour is truncated like the CPU reference (IN-304, tolerance IN-500).

#version 450
#extension GL_GOOGLE_include_directive : require
#if HWR_INTERLOCK
// IN-345: with pixel interlock the read-modify-write of the frame and depth words below runs in primitive
// order for overlapping fragments, so a whole batch is one draw with no barriers between its primitives.
#extension GL_ARB_fragment_shader_interlock : require
layout(pixel_interlock_ordered) in;
#endif
#include "hw_pixel_core.glsl"

#if HWR_FLAT_COLOUR
layout(location = 2) flat in vec4 in_rgba;
#else
layout(location = 2) in vec4 in_rgba;
#endif
layout(location = 0) in vec2 in_st;
layout(location = 1) in float in_q;
layout(location = 3) in float in_fog;
layout(location = 4) in vec2 in_uv;
layout(location = 5) noperspective in float in_z;

layout(std140, set = 0, binding = 0) uniform PixelStateBlock {
    uint format;
    uint fbmsk;
    uint fba;
    uint dither;
    uint dimx_lo;
    uint dimx_hi;
    uint ate;
    uint atst;
    uint afail;
    uint aref;
    uint date_on;
    uint datm;
    uint ztest;
    uint ztst;
    uint zmsk;
    uint zformat;
    uint blend_on;
    uint pabe;
    uint blend_a;
    uint blend_b;
    uint blend_c;
    uint blend_d;
    uint fix;
    uint colclamp;
    uint tex_on;
    uint tfx;
    uint tcc;
    uint texel_kind;   // HW_TEXEL_RGBA8 (CPU-expanded) or raw CT16/CT24 copied from a target (IN-346)
    uint ta0;
    uint ta1;
    uint aem;
    uint fog_on;
    uint fog_r;
    uint fog_g;
    uint fog_b;
    uint tex_fst;      // TEX0.FST: coordinates are UV (10.4) instead of STQ
    uint tex_linear;   // TEX1.MMAG = 1: bilinear
    uint wrap_s;       // WMS: 0 repeat, 1 clamp, 2 region clamp, 3 region repeat
    uint wrap_t;       // WMT
    uint wrap_min_s;   // MINU
    uint wrap_max_s;   // MAXU
    uint wrap_min_t;   // MINV
    uint wrap_max_t;   // MAXV
    uint tex_log2_w;   // TW
    uint tex_log2_h;   // TH
    uint tex_origin_x; // the chunk's region in the texel atlas (IN-352)
    uint tex_origin_y;
    // CORE2: 1 when the frame image is shared by the 32- and 24-bit views of the same memory (one target per
    // memory, CORE2-SPEC 1.3), so a CT24 write keeps the stored top byte as GS memory does. 0 in the old core.
    uint keep_top_byte;
} state;

#if HWR_INTERLOCK
// Inside the interlock the next fragment of the same pixel must see this one's stores: coherent images.
layout(set = 0, binding = 1, r32ui) coherent uniform uimage2D hw_frame;
layout(set = 0, binding = 2, r32ui) coherent uniform uimage2D hw_depth;
#else
layout(set = 0, binding = 1, r32ui) uniform uimage2D hw_frame;
layout(set = 0, binding = 2, r32ui) uniform uimage2D hw_depth;
#endif
layout(set = 0, binding = 3, r32ui) uniform readonly uimage2D hw_texels;

// IN-384 follow-up: set from the top bit of the tex_scale push word (the GPU drawer's GT4_HW_SCALED_FILTER=linear).
bool g_force_linear = false;
uint g_tex_scale = 1u;  // pc.tex_scale without the filter/dither bits
// IN-384 follow-up (Q-GSDX-COMPARISON section 2): the reference indexes the dither matrix with the raw fragment
// coordinate (one cell = 4x4 OUTPUT pixels) and only offers the per-native-pixel pattern as an option; ours always
// divided by the scale, so one cell covered s x s output pixels (24x24 at 6x, doubled vertically on a 480i field:
// the owner's 6 x 12 lattice). Fine is the default at scale > 1; native stays available for fidelity checks.
bool g_dither_fine = true;

// The vertex stage's push constants (hw_vertex.vert), read here for the internal scale (IN-371): the native dither
// mode of 06.5 indexes DIMX by the native pixel, so each dither cell covers s x s sub-pixels.
layout(push_constant) uniform VertexPush {
    vec2 viewport_size;
    float scale;
    float scale_offset;
    int offset_x;
    int offset_y;
    uint z_max;
    uint tex_scale;  // IN-374: 1 = the texel atlas (1x texels); s = hw_texels_scaled holds the texture at scale s
} pc;

// IN-374 (06.8): a texture copied from a resident target's scaled image, s x s texels per native texel.
layout(set = 0, binding = 4, r32ui) uniform readonly uimage2D hw_texels_scaled;

// CPU ToTexel104: floor of the scaled coordinate, saturated far outside the 1024 range.
int to_texel_104(float value) {
    return int(clamp(floor(value), -1.0e9, 1.0e9));
}

// pixel::wrap_texel for one axis (mode values are the GS WMS/WMT encoding).
int wrap_texel(int texel, uint log2_size, uint mode, uint minimum, uint maximum) {
    int last = int((1u << log2_size) - 1u);
    if (mode == 0u) {
        return texel & last;
    }
    if (mode == 1u) {
        return clamp(texel, 0, last);
    }
    if (mode == 2u) {
        return clamp(texel, int(minimum), int(maximum));
    }
    return (texel & int(minimum)) | int(maximum);
}

// Texel colour: RGBA8 words decoded on the CPU, or (IN-346) raw CT16/CT24 words copied from a resident target
// on the GPU, expanded here with TEXA exactly as the CPU decode does (hw_texel_colour, verified bit-exact).
ivec4 fetch_texel(int x, int y) {
    uint raw = imageLoad(hw_texels, ivec2(x + int(state.tex_origin_x), y + int(state.tex_origin_y))).x;
    return hw_texel_colour(state.texel_kind, raw, state.ta0, state.ta1, state.aem != 0u);
}

// IN-374: wrap of a scaled texel coordinate. The native texel (floor division by s) takes the 03.5 wrap of its
// axis; the sub-texel is kept for the repeat modes and pinned to the clamped edge for the clamp modes.
int wrap_scaled(int texel, int s, uint log2_size, uint mode, uint minimum, uint maximum) {
    int native = texel >= 0 ? texel / s : -((s - 1 - texel) / s);
    int sub = texel - native * s;
    int wrapped = wrap_texel(native, log2_size, mode, minimum, maximum);
    if ((mode == 1u || mode == 2u) && wrapped != native) {
        sub = native > wrapped ? s - 1 : 0;
    }
    return wrapped * s + sub;
}

ivec4 fetch_texel_scaled(int x, int y, int s) {
    // IN-384 follow-up: the scaled texel image holds exactly this texture at (0, 0) (the copy in Execute places it
    // there, sized to the sampled footprint), so the 1x atlas origin must NOT be added here: adding it made every
    // texture after the first in a run sample the wrong region (the leaking cuts on the car at 6x).
    uint raw = imageLoad(hw_texels_scaled, ivec2(x, y)).x;
    return hw_texel_colour(state.texel_kind, raw, state.ta0, state.ta1, state.aem != 0u);
}

// The texture at s x s texels per native texel: the coordinate times s, the same 4-bit weights.
ivec4 sample_texture_scaled(int uvs, int uvt, int s) {
    if (state.tex_linear == 0u && !g_force_linear) {
        int x = wrap_scaled((uvs * s) >> 4, s, state.tex_log2_w, state.wrap_s, state.wrap_min_s, state.wrap_max_s);
        int y = wrap_scaled((uvt * s) >> 4, s, state.tex_log2_h, state.wrap_t, state.wrap_min_t, state.wrap_max_t);
        return fetch_texel_scaled(x, y, s);
    }
    int bs = uvs * s - 8;
    int bt = uvt * s - 8;
    int x0 = wrap_scaled(bs >> 4, s, state.tex_log2_w, state.wrap_s, state.wrap_min_s, state.wrap_max_s);
    int x1 = wrap_scaled((bs >> 4) + 1, s, state.tex_log2_w, state.wrap_s, state.wrap_min_s, state.wrap_max_s);
    int y0 = wrap_scaled(bt >> 4, s, state.tex_log2_h, state.wrap_t, state.wrap_min_t, state.wrap_max_t);
    int y1 = wrap_scaled((bt >> 4) + 1, s, state.tex_log2_h, state.wrap_t, state.wrap_min_t, state.wrap_max_t);
    int fx = bs & 15;
    int fy = bt & 15;
    ivec4 c00 = fetch_texel_scaled(x0, y0, s);
    ivec4 c10 = fetch_texel_scaled(x1, y0, s);
    ivec4 c01 = fetch_texel_scaled(x0, y1, s);
    ivec4 c11 = fetch_texel_scaled(x1, y1, s);
    int w00 = (16 - fx) * (16 - fy);
    int w10 = fx * (16 - fy);
    int w01 = (16 - fx) * fy;
    int w11 = fx * fy;
    return (c00 * w00 + c10 * w10 + c01 * w01 + c11 * w11) >> 8;
}

// SampleTexture of the CPU reference: nearest, or bilinear with the 4-bit weights (IN-305).
ivec4 sample_texture(int uvs, int uvt) {
    // IN-384 follow-up (owner test 11): GT4_HW_SCALED_FILTER=linear forces the bilinear path for a texture the game
    // asks to sample nearest, but only at an internal scale > 1 and only for the 1x texel atlas. A 1x texture
    // magnified 6x with nearest shows the source's 6-pixel steps (the menu background grid, the map's dashes); the
    // reference renderer smooths them with its upscaling filter. Off by default (faithful to the GS).
    if (state.tex_linear == 0u && !(g_force_linear && g_tex_scale <= 1u)) {
        int x = wrap_texel(uvs >> 4, state.tex_log2_w, state.wrap_s, state.wrap_min_s, state.wrap_max_s);
        int y = wrap_texel(uvt >> 4, state.tex_log2_h, state.wrap_t, state.wrap_min_t, state.wrap_max_t);
        return fetch_texel(x, y);
    }
    int bs = uvs - 8;
    int bt = uvt - 8;
    int x0 = wrap_texel(bs >> 4, state.tex_log2_w, state.wrap_s, state.wrap_min_s, state.wrap_max_s);
    int x1 = wrap_texel((bs >> 4) + 1, state.tex_log2_w, state.wrap_s, state.wrap_min_s, state.wrap_max_s);
    int y0 = wrap_texel(bt >> 4, state.tex_log2_h, state.wrap_t, state.wrap_min_t, state.wrap_max_t);
    int y1 = wrap_texel((bt >> 4) + 1, state.tex_log2_h, state.wrap_t, state.wrap_min_t, state.wrap_max_t);
    int fx = bs & 15;
    int fy = bt & 15;
    ivec4 c00 = fetch_texel(x0, y0);
    ivec4 c10 = fetch_texel(x1, y0);
    ivec4 c01 = fetch_texel(x0, y1);
    ivec4 c11 = fetch_texel(x1, y1);
    int w00 = (16 - fx) * (16 - fy);
    int w10 = fx * (16 - fy);
    int w01 = (16 - fx) * fy;
    int w11 = fx * fy;
    return (c00 * w00 + c10 * w10 + c01 * w01 + c11 * w11) >> 8;
}

void main() {
    ivec2 pixel = ivec2(floor(gl_FragCoord.xy));
    // IN-384 follow-up: the top bit of the tex_scale push word requests the forced linear filter for 1x textures.
    g_force_linear = (pc.tex_scale & 0x80000000u) != 0u;
    g_dither_fine = (pc.tex_scale & 0x40000000u) != 0u;
    // IN-388: bit 29 is the vertex stage's native-alignment snap, not part of the scale; IN-392: bit 28 asks for the
    // position depth below.
    g_tex_scale = pc.tex_scale & 0x01FFFFFFu;  // bits 27/26/25: GT4_HW_SCALED_DIAG=noalpha/notex/noz (diagnostic)
    const bool depth_from_position = (pc.tex_scale & 0x10000000u) != 0u;

    HwPixelState s;
    s.format = state.format;
    s.fbmsk = state.fbmsk;
    s.fba = state.fba;
    s.dither = state.dither;
    s.dimx_lo = state.dimx_lo;
    s.dimx_hi = state.dimx_hi;
    s.ate = ((pc.tex_scale & 0x08000000u) != 0u && state.afail == 0u && state.atst != 0u) ? 0u : state.ate;
    s.atst = state.atst;
    s.afail = state.afail;
    s.aref = state.aref;
    s.date_on = state.date_on;
    s.datm = state.datm;
    s.ztest = state.ztest;
    s.ztst = (pc.tex_scale & 0x02000000u) != 0u && state.ztst >= 2u ? 1u : state.ztst;  // diagnostic: GT4_HW_SCALED_DIAG=noz
    s.zmsk = state.zmsk;
    s.zformat = state.zformat;
    s.blend_on = state.blend_on;
    s.pabe = state.pabe;
    s.blend_a = state.blend_a;
    s.blend_b = state.blend_b;
    s.blend_c = state.blend_c;
    s.blend_d = state.blend_d;
    s.fix = state.fix;
    s.colclamp = state.colclamp;

    // Colour and fog: truncation of the interpolated 0..255 values (IN-304). The 0.001 tolerance
    // absorbs float interpolation noise on exact integer vertices (IN-500).
    ivec4 frag = ivec4(floor(clamp(in_rgba, 0.0, 255.0) + vec4(0.001)));
    ivec4 colour = frag;
    if (state.tex_on != 0u) {
        int uvs;
        int uvt;
        if (state.tex_fst != 0u) {
            uvs = to_texel_104(in_uv.x);
            uvt = to_texel_104(in_uv.y);
        } else {
            // Per-pixel divide by Q; a zero Q is treated as 1 (IN-310).
            float q = in_q == 0.0 ? 1.0 : in_q;
            float scale_s = float(1u << state.tex_log2_w) * 16.0;
            float scale_t = float(1u << state.tex_log2_h) * 16.0;
            uvs = to_texel_104((in_st.x / q) * scale_s);
            uvt = to_texel_104((in_st.y / q) * scale_t);
        }
        ivec4 texel = g_tex_scale > 1u ? sample_texture_scaled(uvs, uvt, int(g_tex_scale)) : sample_texture(uvs, uvt);
        if ((pc.tex_scale & 0x04000000u) != 0u) texel = ivec4(128, 128, 128, 128);  // diagnostic: GT4_HW_SCALED_DIAG=notex
        colour = hw_texture_function(state.tfx, state.tcc != 0u, texel, frag);
    }
    if (state.fog_on != 0u) {
        int fog = int(clamp(floor(clamp(in_fog, 0.0, 255.0) + 0.001), 0.0, 255.0));
        ivec3 fog_colour = ivec3(int(state.fog_r), int(state.fog_g), int(state.fog_b));
        colour.r = (fog * colour.r + (255 - fog) * fog_colour.r) >> 8;
        colour.g = (fog * colour.g + (255 - fog) * fog_colour.g) >> 8;
        colour.b = (fog * colour.b + (255 - fog) * fog_colour.b) >> 8;
    }

    HwFragment f;
    // Dither index. IN-384 follow-up: the reference's default at scale is the raw fragment coordinate (4x4 output
    // pixels per cell); the 06.5 native mode (one cell per s x s block) is the optional one. At 1x both are identical.
    uint pixel_scale = max(uint(pc.scale + 0.5), 1u);
    f.x = g_dither_fine ? uint(pixel.x) : uint(pixel.x) / pixel_scale;
    f.y = g_dither_fine ? uint(pixel.y) : uint(pixel.y) / pixel_scale;
    f.colour = colour;
    // IN-392: at scale, the rasteriser's depth (the vertex stage writes Z * 2^-32, clamped to the format maximum)
    // floored to an integer, as the reference does: identical vertices give bit-identical depth in every pass
    // (position invariance), so a GEQUAL tie between passes holds at every sub-pixel. 1x keeps the varying.
    f.z = depth_from_position ? uint(min(floor(gl_FragCoord.z * 4294967296.0), 4294967040.0))
                              : uint(clamp(in_z, 0.0, 4294967040.0));

#if HWR_INTERLOCK
    beginInvocationInterlockARB();
#endif
    uint frame_old = imageLoad(hw_frame, pixel).x;
    uint depth_old = imageLoad(hw_depth, pixel).x;
    HwOutcome o = hw_process_pixel(s, f, frame_old, depth_old);
    if (o.frame_written) {
        if (state.keep_top_byte != 0u && state.format == HW_FORMAT_CT24) {
            o.frame_stored = (o.frame_stored & 0x00FFFFFFu) | (frame_old & 0xFF000000u);
        }
        imageStore(hw_frame, pixel, uvec4(o.frame_stored));
    }
    if (o.depth_written) {
        imageStore(hw_depth, pixel, uvec4(o.depth_stored));
    }
#if HWR_INTERLOCK
    endInvocationInterlockARB();
#endif
}
