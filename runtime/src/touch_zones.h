#pragma once
// Drawn touch controls, the geometry half: per-game button layouts in normalized coordinates, hit testing, and the
// launcher's overrides (NX_TOUCH_LAYOUT / NX_TOUCH_SIZE). A plain header with no platform, game or Vulkan headers, so
// tests/bayonetta3_nvn_touch_zones_test.cpp can check the arithmetic on any host without a phone.
//
// The drawing and the touch plumbing live elsewhere: touch_layout.cpp (which layout applies, the launcher switches),
// platform_android_input.cpp (turning a finger into buttons and sticks) and nvn_present.cpp (the circles).
//
// Coordinates: x, y are 0..1 across the landscape screen with y down; r is a fraction of the screen height. Distances
// are measured in screen heights, so a hit test multiplies the x difference by the screen's aspect (width / height).
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace nx::plat {
namespace tz {

// The NpadButton bits this header needs, duplicated from platform.h so the header stands alone. touch_layout.cpp
// static_asserts that they match.
enum : uint64_t {
    kA = 1ull << 0,
    kB = 1ull << 1,
    kX = 1ull << 2,
    kY = 1ull << 3,
    kL = 1ull << 6,
    kR = 1ull << 7,
    kZL = 1ull << 8,
    kZR = 1ull << 9,
    kPlus = 1ull << 10,
    kMinus = 1ull << 11,
    kLeft = 1ull << 12,
    kUp = 1ull << 13,
    kRight = 1ull << 14,
    kDown = 1ull << 15,
};

struct Zone {
    float x, y, r;  // centre and radius
    uint64_t button;
};

// Where a floating stick may start: a rectangle in the same coordinates (x0 < x1, y0 < y1). A finger that lands on
// no button inside it becomes that stick, relative to where it went down.
struct StickArea {
    float x0, y0, x1, y1;
    bool valid;  // false: the game wants no such stick on touch
};

inline bool stick_contains(const StickArea& a, float x, float y) {
    return a.valid && x >= a.x0 && x <= a.x1 && y >= a.y0 && y <= a.y1;
}

// A D-pad: one cross centred at (x, y) whose arms reach `s` screen heights from the centre. A finger anywhere on it
// presses the direction it points to; within 22.5 degrees of a diagonal it presses both neighbours, and sliding across
// the arms changes the direction (the finger is re-read every move).
struct Dpad {
    float x, y, s;
    bool valid;
};

// The direction bits under (x, y), or 0 when the finger is off the cross (outside its square, with a little slack, or
// right in its middle). `aspect` = width / height.
inline uint64_t dpad_hit(const Dpad& d, float x, float y, float aspect, bool held = false) {
    if (!d.valid) return 0;
    const float dx = (x - d.x) * aspect / d.s, dy = (y - d.y) / d.s;
    const float limit = held ? 2.2f : 1.15f;  // a held finger may slide off a little without letting go
    if (std::fabs(dx) > limit || std::fabs(dy) > limit) return 0;
    if (std::hypot(dx, dy) < 0.18f) return 0;  // the dead centre
    const float a = std::atan2(dy, dx);  // y down: -pi/2 is up
    const float k = 3.14159265f / 8.0f;
    // Eight sectors of 45 degrees: right, down-right, down, down-left, left, up-left, up, up-right.
    const int sector = int(std::floor((a + k) / (2 * k))) & 7;
    static constexpr uint64_t kBits[8] = {kRight, kRight | kDown, kDown, kDown | kLeft,
                                          kLeft,  kLeft | kUp,    kUp,   kUp | kRight};
    return kBits[sector];
}

// A finger's distance from a zone, in screen heights; the drawn circle is 1.
inline float zone_distance(const Zone& z, float x, float y, float aspect) {
    return std::hypot((x - z.x) * aspect, y - z.y) / (z.r > 0 ? z.r : 1e-6f);
}

// A little past the drawn edge still counts (1.35 radii), so a thumb does not have to be exact.
inline constexpr float kZoneTolerance = 1.35f;

// The nearest zone under (x, y), or 0. `aspect` = screen width / height.
inline uint64_t zone_hit(const Zone* zones, size_t count, float x, float y, float aspect) {
    uint64_t best = 0;
    float best_d = 1e9f;
    for (size_t i = 0; i < count; i++) {
        const float d = zone_distance(zones[i], x, y, aspect);
        if (d < kZoneTolerance && d < best_d) best = zones[i].button, best_d = d;
    }
    return best;
}

// NX_TOUCH_SIZE: a percentage of the drawn radii, 50..160 (100 is the layout as designed).
inline float size_scale(int percent) {
    const int p = percent < 50 ? 50 : percent > 160 ? 160 : percent;
    return float(p) / 100.0f;
}

// NX_TOUCH_LAYOUT="A:x,y,r;B:..." (";" between entries): replaces the position and radius of the named zones.
// Names are matched exactly against `names` (the same order as `zones`); an entry that does not parse, names no zone
// or is out of range is skipped, so a partially broken setting still gives a usable layout. Returns how many entries
// were applied. `names[i]` may be null for an unnamed zone.
inline size_t apply_layout_override(Zone* zones, size_t count, const char* const* names, const char* spec) {
    if (!spec || !*spec) return 0;
    size_t applied = 0;
    const char* p = spec;
    while (*p) {
        const char* end = p;
        while (*end && *end != ';') end++;
        // Item is [p, end): "NAME:x,y,r", with spaces around entries and names allowed.
        while (p < end && (*p == ' ' || *p == '\t')) p++;
        const char* colon = p;
        while (colon < end && *colon != ':') colon++;
        const char* name_end = colon;
        while (name_end > p && (name_end[-1] == ' ' || name_end[-1] == '\t')) name_end--;
        if (colon < end) {
            const size_t name_len = size_t(name_end - p);
            float x = 0, y = 0, r = 0;
            if (std::sscanf(colon + 1, "%f,%f,%f", &x, &y, &r) == 3 && x >= 0 && x <= 1 && y >= 0 && y <= 1 &&
                r > 0.01f && r < 0.3f) {
                for (size_t i = 0; i < count; i++) {
                    const char* name = names ? names[i] : nullptr;
                    if (!name) continue;
                    size_t n = 0;
                    while (name[n]) n++;  // the full name, so "P" never matches "PLUS"
                    if (n != name_len) continue;
                    bool same = true;
                    for (size_t k = 0; k < n; k++)
                        if (name[k] != p[k]) same = false;
                    if (!same) continue;
                    zones[i].x = x, zones[i].y = y, zones[i].r = r;
                    applied++;
                    break;
                }
            }
        }
        p = *end ? end + 1 : end;
    }
    return applied;
}

// The drawn radii after NX_TOUCH_SIZE.
inline void apply_size_scale(Zone* zones, size_t count, int percent) {
    const float s = size_scale(percent);
    for (size_t i = 0; i < count; i++) zones[i].r *= s;
}

// ---- Bayonetta 3 ----------------------------------------------------------------------------------------------
// Combat needs the four face buttons constantly (X punch, A kick, Y shoot, B jump), the dodge on ZR, and the camera
// under the right thumb, so the right side is a face diamond with the dodge above it and a floating camera stick in
// the space between the diamond and the screen's middle. Both index fingers get the shoulders and triggers at the
// top corners; + and - sit top centre.
inline constexpr Zone kBayonetta3[] = {
    // x, y, r, button
    {0.845f, 0.560f, 0.080f, kX},     // punch
    {0.918f, 0.700f, 0.084f, kA},     // kick
    {0.845f, 0.845f, 0.084f, kB},     // jump
    {0.765f, 0.700f, 0.076f, kY},     // shoot
    {0.885f, 0.245f, 0.078f, kZR},    // dodge: the most-used defensive button, so not a small one
    {0.925f, 0.075f, 0.044f, kR},     // right shoulder (inset: its pill must fit a 1.2:1 fold screen)
    {0.108f, 0.245f, 0.058f, kZL},    // left trigger (camera reset / lock-on)
    {0.075f, 0.075f, 0.046f, kL},     // left shoulder
    {0.445f, 0.062f, 0.038f, kMinus}, // - (menu)
    {0.555f, 0.062f, 0.038f, kPlus},  // + (pause)
};
inline constexpr const char* const kBayonetta3Names[] = {"X", "A", "B", "Y", "ZR", "R", "ZL", "L", "MINUS", "PLUS"};
inline constexpr size_t kBayonetta3Count = sizeof(kBayonetta3) / sizeof(kBayonetta3[0]);

// The left stick: anywhere on the left half that is not a button. The camera stick: the middle-right band between the
// screen's centre and the face diamond, below the top row (so a stray touch at the very edge is not a camera swing).
inline constexpr StickArea kBayonetta3MoveStick{0.0f, 0.10f, 0.50f, 1.0f, true};
inline constexpr StickArea kBayonetta3CameraStick{0.50f, 0.30f, 0.72f, 1.0f, true};
// The D-pad below and right of the left stick, as on a left Joy-Con (Bayonetta 3: weapon swaps, items, taunt).
inline constexpr Dpad kBayonetta3Dpad{0.30f, 0.83f, 0.115f, true};
// Where each stick is drawn while no thumb holds it: the left one above the D-pad, the right one below-left of the face
// diamond (a right Joy-Con's arrangement). x, y as zones.
inline constexpr float kBayonetta3MoveRest[2] = {0.15f, 0.58f};
inline constexpr float kBayonetta3CameraRest[2] = {0.615f, 0.83f};

// ---- Super Mario Bros. Wonder (unchanged geometry) -------------------------------------------------------------
inline constexpr Zone kWonder[] = {
    {0.905f, 0.74f, 0.105f, kA},     {0.775f, 0.86f, 0.085f, kB},    {0.775f, 0.60f, 0.085f, kY},
    {0.905f, 0.47f, 0.075f, kX},     {0.955f, 0.25f, 0.055f, kZR},   {0.955f, 0.07f, 0.045f, kR},
    {0.045f, 0.07f, 0.045f, kL},     {0.545f, 0.07f, 0.040f, kPlus}, {0.455f, 0.07f, 0.040f, kMinus},
};
inline constexpr const char* const kWonderNames[] = {"A", "B", "Y", "X", "ZR", "R", "L", "PLUS", "MINUS"};
inline constexpr size_t kWonderCount = sizeof(kWonder) / sizeof(kWonder[0]);
inline constexpr StickArea kWonderMoveStick{0.0f, 0.10f, 0.50f, 1.0f, true};
inline constexpr StickArea kWonderNoCamera{0, 0, 0, 0, false};
inline constexpr Dpad kWonderNoDpad{0, 0, 0, false};

}  // namespace tz
}  // namespace nx::plat
