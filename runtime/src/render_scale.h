#pragma once
// The size the game renders at, decided in one place for every platform: the console mode's base size, optionally
// replaced by an explicit NX_RENDER_RES or scaled by NX_RENDER_SCALE. The window is a separate size (platform's
// `resolution`), and the present step fits one into the other keeping the aspect, so a phone or tablet at 20:9, 16:10
// or 4:3 shows the picture with bars instead of a stretched one.
//
// Self-contained (no Vulkan, no platform.h): tests/bayonetta3_nvn_render_scale_test.cpp compiles it on its own.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace nx::plat {

struct RenderSize {
    uint32_t w = 1280, h = 720;
    bool operator==(const RenderSize& o) const { return w == o.w && h == o.h; }
};

inline constexpr uint32_t kRenderMinWidth = 320, kRenderMinHeight = 180;
inline constexpr uint32_t kRenderMaxWidth = 7680, kRenderMaxHeight = 4320;

// The console's own sizes: handheld 1280x720, docked 1920x1080.
inline RenderSize console_render_size(bool handheld) { return handheld ? RenderSize{1280, 720} : RenderSize{1920, 1080}; }

// "1280x720" (either case, no trailing junk): false for anything else, including sizes outside the supported range.
inline bool parse_render_res(const char* value, RenderSize& out) {
    if (!value || !*value) return false;
    char* end = nullptr;
    const unsigned long w = std::strtoul(value, &end, 10);
    if (end == value || (*end != 'x' && *end != 'X')) return false;
    const char* rest = end + 1;
    const unsigned long h = std::strtoul(rest, &end, 10);
    if (end == rest || *end) return false;
    if (w < kRenderMinWidth || h < kRenderMinHeight || w > kRenderMaxWidth || h > kRenderMaxHeight) return false;
    out.w = uint32_t(w), out.h = uint32_t(h);
    return true;
}

// "0.5".."2": the factor applied to the console's size. Anything else (unset, empty, junk, out of range) is 1.
inline float render_scale_from_env(const char* value) {
    if (!value || !*value) return 1.0f;
    char* end = nullptr;
    const float v = std::strtof(value, &end);
    if (end == value || *end || !(v >= 0.25f && v <= 2.0f)) return 1.0f;
    return v;
}

// Even dimensions: some drivers and the game's own target sizing are happier, and it halves no pixels that matter.
inline uint32_t render_even(uint32_t v) { return v & 1u ? v - 1u : v; }

inline RenderSize clamp_render_size(RenderSize s) {
    s.w = render_even(s.w < kRenderMinWidth ? kRenderMinWidth : s.w > kRenderMaxWidth ? kRenderMaxWidth : s.w);
    s.h = render_even(s.h < kRenderMinHeight ? kRenderMinHeight : s.h > kRenderMaxHeight ? kRenderMaxHeight : s.h);
    return s;
}

// The largest `num:den` (16:9 by default) rectangle that fits in w x h: `resolution=auto` uses it so that a phone,
// tablet or 16:10 panel gets a correctly shaped picture (the window is then letterboxed by the compositor) instead of
// a stretched one. The game sizes its render targets from the window, so the shape of the window is the shape of the
// render.
inline RenderSize fit_aspect(uint32_t w, uint32_t h, uint32_t num = 16, uint32_t den = 9) {
    if (!num || !den || !w || !h) return RenderSize{1280, 720};
    RenderSize s;
    if (uint64_t(w) * den >= uint64_t(h) * num)  // wider than num:den: height is the limit
        s.h = h, s.w = uint32_t(uint64_t(h) * num / den);
    else
        s.w = w, s.h = uint32_t(uint64_t(w) * den / num);
    return clamp_render_size(s);
}

// NX_RENDER_RES wins; otherwise the console size times NX_RENDER_SCALE.
inline RenderSize render_size(bool handheld, const char* explicit_res, const char* scale) {
    RenderSize s;
    if (parse_render_res(explicit_res, s)) return clamp_render_size(s);
    s = console_render_size(handheld);
    const float f = render_scale_from_env(scale);
    if (f != 1.0f) {
        s.w = uint32_t(float(s.w) * f + 0.5f);
        s.h = uint32_t(float(s.h) * f + 0.5f);
    }
    return clamp_render_size(s);
}

}  // namespace nx::plat
