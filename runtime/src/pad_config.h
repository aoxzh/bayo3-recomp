#pragma once
// Player 1's controller settings, shared by every platform so the three backends cannot drift apart: the button
// layout, the stick deadzone and the trigger threshold a launcher writes, and the mapping helpers they use.
//
// Self-contained (no platform.h, no SDL, no Vulkan): tests/bayonetta3_nvn_pad_config_test.cpp compiles it on its own.
//
// Defaults reproduce the historical hard-coded values exactly (deadzone 7849, trigger 12000, Nintendo layout), so an
// unset environment behaves as before. A switch is read once, at the first pad sample.
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace nx::plat {

// The Npad stick range (platform.h kStickMax): the guest ABI's full deflection.
inline constexpr int32_t kPadStickMax = 0x7FFF;
inline constexpr int kPadDefaultDeadzone = 7849;
inline constexpr int kPadDefaultTrigger = 12000;

struct PadConfig {
    bool swap_face = false;      // NX_PAD_SWAP=1: A<->B and X<->Y, for players used to Xbox labels
    bool invert_camera_x = false;  // NX_PAD_INVERT_X=1: right stick x
    bool invert_camera_y = false;  // NX_PAD_INVERT_Y=1: right stick y
    int deadzone = kPadDefaultDeadzone;  // NX_PAD_DEADZONE=0..20000, the radial deadzone per axis
    int trigger = kPadDefaultTrigger;    // NX_PAD_TRIGGER=0..32767, the ZL/ZR press threshold
    int index = 0;                       // NX_PAD_INDEX=0..7, which connected controller is player 1
};

// A decimal switch in [lo, hi]; anything else (unset, empty, junk, out of range) keeps `fallback`.
inline int pad_int_from_env(const char* value, int fallback, int lo, int hi) {
    if (!value || !*value) return fallback;
    char* end = nullptr;
    errno = 0;
    const long v = std::strtol(value, &end, 10);
    if (errno || end == value || *end || v < lo || v > hi) return fallback;
    return int(v);
}

// A presence switch: 0, false, off, no and empty mean off, like scripts/b3_play.sh treats its settings.
inline bool pad_flag_from_env(const char* value) {
    if (!value || !*value) return false;
    return !(std::strcmp(value, "0") == 0 || std::strcmp(value, "false") == 0 || std::strcmp(value, "off") == 0 ||
             std::strcmp(value, "no") == 0);
}

inline PadConfig pad_config_from_env(const char* swap = std::getenv("NX_PAD_SWAP"),
                                     const char* deadzone = std::getenv("NX_PAD_DEADZONE"),
                                     const char* trigger = std::getenv("NX_PAD_TRIGGER"),
                                     const char* index = std::getenv("NX_PAD_INDEX"),
                                     const char* invert_x = std::getenv("NX_PAD_INVERT_X"),
                                     const char* invert_y = std::getenv("NX_PAD_INVERT_Y")) {
    PadConfig c;
    c.swap_face = pad_flag_from_env(swap);
    c.invert_camera_x = pad_flag_from_env(invert_x);
    c.invert_camera_y = pad_flag_from_env(invert_y);
    c.deadzone = pad_int_from_env(deadzone, kPadDefaultDeadzone, 0, 20000);
    c.trigger = pad_int_from_env(trigger, kPadDefaultTrigger, 0, 32767);
    c.index = pad_int_from_env(index, 0, 0, 7);
    return c;
}

// One analog axis: inside the deadzone nothing, past it the remaining travel is stretched to full deflection, so a
// stick does not jump to a quarter of its range as it leaves the deadzone. deadzone 0 is the identity.
inline int32_t pad_axis(int raw, int deadzone, int32_t max = kPadStickMax) {
    const int32_t limit = std::max<int32_t>(1, max);
    const int dz = std::clamp(deadzone, 0, int(limit) - 1);
    if (raw > -dz && raw < dz) return 0;
    const int32_t mag = std::min<int32_t>(raw < 0 ? -int32_t(raw) : int32_t(raw), limit);
    const int64_t travel = int64_t(mag - dz) * limit / (limit - dz);
    return int32_t(raw < 0 ? -travel : travel);
}

// Preserve the historical exclusive threshold, including 0 for an idle trigger.
// The maximum configured threshold means a fully pressed SDL trigger, not an
// unreachable value above the axis maximum.
inline bool pad_trigger_pressed(int raw, int threshold) {
    const int limit = std::clamp(threshold, 0, 32767);
    return raw > limit || (limit == 32767 && raw == limit);
}

// A<->B and X<->Y. The Npad bits are platform.h's kA..kY.
inline uint64_t pad_swap_face(uint64_t buttons) {
    constexpr uint64_t a = 1ull << 0, b = 1ull << 1, x = 1ull << 2, y = 1ull << 3;
    uint64_t out = buttons & ~(a | b | x | y);
    if (buttons & a) out |= b;
    if (buttons & b) out |= a;
    if (buttons & x) out |= y;
    if (buttons & y) out |= x;
    return out;
}

// The camera axes of a right stick reading, after the player's inversion switches.
inline void pad_camera(int32_t& rx, int32_t& ry, const PadConfig& c) {
    if (c.invert_camera_x) rx = -rx;
    if (c.invert_camera_y) ry = -ry;
}

}  // namespace nx::plat
