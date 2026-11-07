// NVN rasterizer state the game sets on the command buffer rather than in a state object (docs/NVN.md, N6): stencil
// reference and masks, polygon offset, depth bounds and depth clamp. Recorded here (they were trace stubs, so the
// stencil reference stayed 0 and polygon offset was never applied), kept in the replay state, and applied to each
// draw as Vulkan dynamic state (stencil, depth bias) or through the pipeline key (depth clamp).
// Signatures from Wonder's call sites (nvn_trace.txt):
//   SetStencilRef / SetStencilMask / SetStencilValueMask(cmd, NVNface faces, int value): faces 1 front, 2 back,
//     3 front and back, the same bits as VkStencilFaceFlags. ValueMask is the compare mask (glStencilFuncSeparate's
//     mask), Mask the write mask (glStencilMaskSeparate). Wonder: ValueMask(3, 0xff), Mask(3, 0xff), Ref(2, 0),
//     Ref(1, 0).
//   SetPolygonOffsetClamp(cmd, float factor [s0], float units [s1], float clamp [s2])  (glPolygonOffsetClamp)
//   SetDepthBounds(cmd, NVNboolean enable, float min [s0], float max [s1])  (Wonder: disabled)
//   SetDepthClamp(cmd, NVNboolean enable)  (Wonder: 0)
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <algorithm>

#include "nvn_draw_state.h"

namespace nx::nvn {

using namespace nx::vk;

namespace {

void stencil(CPU& c, Cmd::Op op) {
    Cmd cmd{op};
    cmd.mask = uint32_t(c.x[1]) & 3;   // NVNface
    cmd.n = uint32_t(c.x[2]) & 0xFF;   // stencil buffers are 8 bits
    record(c.x[0], cmd);
}

void SetStencilRef(CPU& c) { stencil(c, Cmd::SetStencilRef); }
void SetStencilMask(CPU& c) { stencil(c, Cmd::SetStencilMask); }
void SetStencilValueMask(CPU& c) { stencil(c, Cmd::SetStencilValueMask); }

void SetPolygonOffsetClamp(CPU& c) {
    Cmd cmd{Cmd::SetPolygonOffsetClamp};
    cmd.f[0] = c.v[0].f32[0], cmd.f[1] = c.v[1].f32[0], cmd.f[2] = c.v[2].f32[0];
    record(c.x[0], cmd);
}

void SetDepthBounds(CPU& c) {
    Cmd cmd{Cmd::SetDepthBounds};
    cmd.n = uint8_t(c.x[1]) != 0;  // original SDK tests the entire NVNboolean byte
    cmd.f[0] = c.v[0].f32[0], cmd.f[1] = c.v[1].f32[0];
    record(c.x[0], cmd);
}

void SetDepthClamp(CPU& c) {
    Cmd cmd{Cmd::SetDepthClamp};
    cmd.n = uint32_t(c.x[1] & 1);
    record(c.x[0], cmd);
}

}  // namespace

namespace {
float equal_slack_units() {
    static const char* env = std::getenv("NX_EQUAL_SLACK");
    static const float units = env ? float(std::atof(env)) : 4.0f;
    return units;
}
float equal_slack_factor() {
    // A slope-sized tolerance exposes hidden surfaces at grazing angles (clothes,
    // seams and deck edges). Keep only the constant tolerance by default; factor=1
    // reproduces the old four-unit configuration for visual comparisons.
    static const char* env = std::getenv("NX_EQUAL_SLACK_FACTOR");
    static const float factor = env ? float(std::atof(env)) : 0.0f;
    return factor;
}
}  // namespace

// The colour pass after a pre-pass tests EQUAL (taken as LESS_OR_EQUAL, nvn_pipeline.cpp) against depth another
// vertex shader wrote: Metal compiles the two apart, and where the colour pass lands a few ulps behind, the pixel
// keeps the pre-pass's clear (black silhouettes, black/white wedges on slopes). A bias toward the camera of a few
// units covers small rounding differences. Do not add a slope-sized tolerance: it can expose hidden surfaces
// much farther behind the pre-pass at grazing angles. NX_EQUAL_SLACK_FACTOR is a diagnostic override.
// GREATER without depth writes is the same case the other way (DEBUG-2.md #65): the world map's "behind scenery"
// silhouette draws the player again with GREATER after the G-buffer wrote it with LESS, and must fail where the player
// is visible. With the two vertex shaders a few ulps apart it passed over the whole player (a flat purple figure, no
// Luigi). The same bias toward the camera makes those ties fail. Off unless NX_GREATER_SLACK=1: it did not bring the
// player back (the map composites it from its own target, issue #50), so it stays an experiment.
bool equal_slack_pass(const DepthStencilInfo& d) {
    static const bool greater = [] {
        const char* e = std::getenv("NX_GREATER_SLACK");
        return e && e[0] == '1';
    }();
    if (d.depth_write || equal_slack_units() == 0.0f) return false;
    return d.depth_func == 3 || (d.depth_func == 5 && greater);
}

namespace replay {

bool raster_command(const Cmd& c) {
    State& s = g_state;
    switch (c.op) {
        case Cmd::SetStencilRef:
        case Cmd::SetStencilMask:
        case Cmd::SetStencilValueMask: {
            uint8_t* v = c.op == Cmd::SetStencilRef    ? s.stencil_ref
                         : c.op == Cmd::SetStencilMask ? s.stencil_write_mask
                                                       : s.stencil_compare_mask;
            for (int f = 0; f < 2; f++)
                if (c.mask >> f & 1) v[f] = uint8_t(c.n);
            return true;
        }
        case Cmd::SetPolygonOffsetClamp:
            s.bias_factor = c.f[0], s.bias_units = c.f[1], s.bias_clamp = c.f[2];
            return true;
        case Cmd::SetDepthBounds: {
            if (c.n && (std::isnan(c.f[0]) || std::isnan(c.f[1])))
                fatal("nvn: enabled NaN depth bounds require hardware comparison semantics");
            s.depth_bounds = c.n != 0, s.depth_bounds_range[0] = c.f[0], s.depth_bounds_range[1] = c.f[1];
            return true;
        }
        case Cmd::SetDepthClamp: s.depth_clamp = c.n != 0; return true;
        case Cmd::SetAlphaRef:
            std::memcpy(&s.alpha_ref, &c.f[0], sizeof(float));
            return true;
        case Cmd::SetLineWidth: s.line_width = c.f[0]; return true;
        case Cmd::SetPatchSize: s.patch_size = c.n; return true;
        default: return false;
    }
}

// Before each draw (every pipeline declares these dynamic, nvn_pipeline.cpp, so they must all be set).
void raster_dynamic(VkCommandBuffer cb, bool slack) {
    const State& s = g_state;
    if (vk_features().depthBounds) {
        // Disabled state may retain arbitrary NVN bounds; supply legal values
        // until the enabled pipeline consumes the requested interval.
        float lo = s.depth_bounds ? s.depth_bounds_range[0] : 0.0f;
        float hi = s.depth_bounds ? s.depth_bounds_range[1] : 1.0f;
        if (s.depth_bounds && !vk_depth_range_unrestricted()) {
            // Without unrestricted depth ranges, attachment depths lie in
            // [0,1]. Intersect the interval; independently clamping both ends
            // would incorrectly admit depth0/1 for an entirely disjoint range.
            lo = std::max(lo, 0.0f);
            hi = std::min(hi, 1.0f);
            if (lo > hi) lo = 1.0f, hi = 0.0f;  // valid Vulkan bounds, empty test
        }
        p_vkCmdSetDepthBounds(cb, lo, hi);
    } else if (s.depth_bounds) {
        // MoltenVK/Metal has no depth-bounds test (no VK_EXT_depth_bounds_test), so the interval cannot be expressed:
        // the pipeline is built without the test (nvn_pipeline.cpp) and fragments outside the interval survive.
        // Report the exact interval once -- an interval covering [0,1] is a no-op anyway, a real restriction is a
        // fidelity loss that must be visible in the log rather than a silent difference.
        static bool warned = false;
        if (!warned) {
            log("nvn: depth bounds %.4f..%.4f enabled; this GPU has no depthBounds feature, the test is dropped%s",
                s.depth_bounds_range[0], s.depth_bounds_range[1],
                s.depth_bounds_range[0] <= 0.0f && s.depth_bounds_range[1] >= 1.0f ? " (interval covers [0,1])" : "");
            warned = true;
        }
    }
    auto set = [&](PFN_vkCmdSetStencilReference fn, const uint8_t* v) {  // the three setters share the signature
        if (v[0] == v[1]) {
            fn(cb, VK_STENCIL_FACE_FRONT_AND_BACK, v[0]);
        } else {
            fn(cb, VK_STENCIL_FACE_FRONT_BIT, v[0]);
            fn(cb, VK_STENCIL_FACE_BACK_BIT, v[1]);
        }
    };
    static const bool old = std::getenv("NX_OLD_STENCIL") != nullptr;  // bisecting: reference 0, masks 0xFF
    static const uint8_t zero[2] = {0, 0}, all[2] = {0xFF, 0xFF};
    set(p_vkCmdSetStencilReference, old ? zero : s.stencil_ref);
    set(p_vkCmdSetStencilCompareMask, old ? all : s.stencil_compare_mask);
    set(p_vkCmdSetStencilWriteMask, old ? all : s.stencil_write_mask);
    // Depth bias. NVN's call has glPolygonOffsetClamp's meaning, which is Vulkan's: units in minimum resolvable
    // depth differences (r), factor per unit of slope. Ryujinx and yuzu halve the units, but that is of the Maxwell
    // register the driver writes (units * 2); the API value needs no scaling. What does differ: D24 targets are
    // D32 float here (nvn_format.cpp), where r is not 2^-24 but 2^(exponent of the primitive's depth - 23): the
    // same for depths in 0.5..1, smaller below. NX_DEPTH_BIAS_UNITS=<scale> multiplies the units, to tune that if
    // decals or shadows show it (yuzu rescales them when it emulates D24 the same way).
    static const char* scale_env = std::getenv("NX_DEPTH_BIAS_UNITS");
    static const float units_scale = scale_env ? float(std::atof(scale_env)) : 1.0f;
    // A non-zero clamp needs the depthBiasClamp feature (MoltenVK has it: Metal's setDepthBias clamp).
    float clamp = vk_features().depthBiasClamp ? s.bias_clamp : 0.0f;
    float units = s.bias_units * units_scale, factor = s.bias_factor;
    if (slack) units -= equal_slack_units(), factor -= equal_slack_factor(), clamp = 0.0f;
    p_vkCmdSetDepthBias(cb, units, clamp, factor);
}

}  // namespace replay

NX_HLE("nvnCommandBufferSetStencilRef", SetStencilRef);
NX_HLE("nvnCommandBufferSetStencilMask", SetStencilMask);
NX_HLE("nvnCommandBufferSetStencilValueMask", SetStencilValueMask);
NX_HLE("nvnCommandBufferSetPolygonOffsetClamp", SetPolygonOffsetClamp);
NX_HLE("nvnCommandBufferSetDepthBounds", SetDepthBounds);
NX_HLE("nvnCommandBufferSetDepthClamp", SetDepthClamp);

}  // namespace nx::nvn
