// Drawn touch controls (docs/archive/nvn/ANDROID-PORT.md A4): which layout applies to the running game, and the launcher's
// switches over it. The geometry itself (zones, sticks, hit testing, the NX_TOUCH_LAYOUT parser) is touch_zones.h,
// which the CPU-only test checks on any host; this file only picks a layout and reads the environment.
//
// The layouts are per game: Super Mario Bros. Wonder (a 2D platformer: a big A, three more face buttons, no camera
// stick) and Bayonetta 3 (a 3D action game: the face diamond for punch/kick/jump/shoot, dodge on ZR, and a floating
// camera stick under the right thumb). Games with no layout here keep their own touch handling (Hades: zones around
// the thumb, decided by the game's code, not by this file).
//
// Launcher settings (config.txt, which becomes the environment, all read once):
//   NX_TOUCH_SHOW=0      draw nothing and ignore touches
//   NX_TOUCH_LAYOUT=...  move buttons: "A:x,y,r;B:..." in 0..1 screen coordinates
//   NX_TOUCH_SIZE=50..160 percent of the drawn radii (100 = as designed)
//   NX_TOUCH_MIRROR=1    left-handed: the movement stick takes the right half, the camera the middle-left
//   NX_TOUCH_OPACITY    10..100, the drawn brightness (85 = the original look)
//
// Elsewhere touch_sprites() draws nothing, except with NX_OVERLAY_DEMO=1 (the buttons at rest), to check the look on
// the Mac with NX_PRESENT_TEST.
#include "touch_layout.h"

#include "touch_art.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <nx/game.h>

namespace nx::plat {

// The bits are compared as integers: touch_zones.h cannot include platform.h (the CPU-only test has no game
// headers), so it repeats the two values it needs and this check keeps the two copies honest.
static_assert(unsigned(tz::kA) == unsigned(kA) && unsigned(tz::kB) == unsigned(kB) &&
                  unsigned(tz::kX) == unsigned(kX) && unsigned(tz::kY) == unsigned(kY) &&
                  unsigned(tz::kZL) == unsigned(kZL) && unsigned(tz::kZR) == unsigned(kZR) &&
                  unsigned(tz::kPlus) == unsigned(kPlus) && unsigned(tz::kMinus) == unsigned(kMinus) &&
                  unsigned(tz::kLeft) == unsigned(kLeft) && unsigned(tz::kUp) == unsigned(kUp) &&
                  unsigned(tz::kRight) == unsigned(kRight) && unsigned(tz::kDown) == unsigned(kDown),
              "touch_zones.h duplicates the NpadButton bits from platform.h and must stay in step with it");

namespace {

int env_int(const char* name, int fallback) {
    const char* e = std::getenv(name);
    return e && *e ? std::atoi(e) : fallback;
}

bool game_is(const char* id) { return std::strcmp(game::kInfo.id, id) == 0; }

struct Layout {
    const tz::Zone* base;
    const char* const* names;
    size_t count;
    tz::StickArea move;    // where a finger becomes the movement stick
    tz::StickArea camera;  // where a finger becomes the camera stick (invalid: this game has none)
    bool dash_full_tilt;   // a movement stick pushed nearly all the way also holds Y (Wonder's dash)
    tz::Dpad dpad;         // invalid: no D-pad
    float move_rest[2], camera_rest[2];  // where the sticks are drawn while no thumb holds them
};

const Layout kNone{nullptr, nullptr, 0, {0, 0, 0, 0, false}, {0, 0, 0, 0, false}, false, {0, 0, 0, false},
                   {0.15f, 0.70f}, {0, 0}};

const Layout& layout() {
    static const Layout l = [] {
        if (game_is("wonder"))
            return Layout{tz::kWonder, tz::kWonderNames, tz::kWonderCount, tz::kWonderMoveStick,
                          tz::kWonderNoCamera, true, tz::kWonderNoDpad, {0.15f, 0.70f}, {0, 0}};
        if (game_is("bayonetta3"))
            return Layout{tz::kBayonetta3, tz::kBayonetta3Names, tz::kBayonetta3Count, tz::kBayonetta3MoveStick,
                          tz::kBayonetta3CameraStick, false, tz::kBayonetta3Dpad,
                          {tz::kBayonetta3MoveRest[0], tz::kBayonetta3MoveRest[1]},
                          {tz::kBayonetta3CameraRest[0], tz::kBayonetta3CameraRest[1]}};
        return kNone;
    }();
    return l;
}

tz::StickArea mirror(const tz::StickArea& a) {
    return tz::StickArea{1.0f - a.x1, a.y0, 1.0f - a.x0, a.y1, a.valid};
}

// The buttons as drawn and hit-tested: the game's layout, with NX_TOUCH_LAYOUT positions and the NX_TOUCH_SIZE scale
// applied. Read once.
const std::vector<tz::Zone>& zones() {
    static const std::vector<tz::Zone> v = [] {
        const Layout& l = layout();
        std::vector<tz::Zone> out(l.base, l.base + l.count);
        tz::apply_layout_override(out.data(), out.size(), l.names, std::getenv("NX_TOUCH_LAYOUT"));
        tz::apply_size_scale(out.data(), out.size(), env_int("NX_TOUCH_SIZE", 100));
        return out;
    }();
    return v;
}

}  // namespace

// NX_TOUCH_SHOW=0 (launcher: On-screen buttons off): nothing is drawn and touches are ignored.
bool touch_enabled() {
    static const bool on = [] {
        const char* e = std::getenv("NX_TOUCH_SHOW");
        return !(e && e[0] == '0');
    }();
    return on;
}

bool touch_always() {
    static const bool on = [] {
        const char* e = std::getenv("NX_TOUCH_SHOW");
        return e && e[0] == '2';
    }();
    return on;
}

// NX_TOUCH_MIRROR=1 (left-handed): the stick takes the right half of the screen instead of the left.
bool touch_mirrored() {
    static const bool on = [] {
        const char* e = std::getenv("NX_TOUCH_MIRROR");
        return e && e[0] == '1';
    }();
    return on;
}

// NX_TOUCH_OPACITY (10..100, default 85): the drawn buttons' brightness relative to the original look.
float touch_opacity_scale() {
    static const float s = float(std::clamp(env_int("NX_TOUCH_OPACITY", 85), 10, 100)) / 85.0f;
    return s;
}

const TouchButton* touch_buttons(size_t& count) {
    static const std::vector<TouchButton> v = [] {
        std::vector<TouchButton> out;
        for (const tz::Zone& z : zones()) out.push_back({z.x, z.y, z.r, z.button});
        return out;
    }();
    count = v.size();
    return v.empty() ? nullptr : v.data();
}

tz::StickArea touch_move_stick() {
    const tz::StickArea a = layout().move;
    return touch_mirrored() ? mirror(a) : a;
}

tz::StickArea touch_camera_stick() {
    const tz::StickArea a = layout().camera;
    return touch_mirrored() ? mirror(a) : a;
}

bool touch_dash_full_tilt() { return layout().dash_full_tilt; }

tz::Dpad touch_dpad() {
    tz::Dpad d = layout().dpad;
    if (touch_mirrored()) d.x = 1.0f - d.x;
    return d;
}

void touch_stick_rest(bool camera, float& x, float& y) {
    const float* r = camera ? layout().camera_rest : layout().move_rest;
    x = touch_mirrored() ? 1.0f - r[0] : r[0], y = r[1];
}

uint64_t touch_hit(float x, float y, float aspect) {
    const std::vector<tz::Zone>& v = zones();
    return tz::zone_hit(v.data(), v.size(), x, y, aspect);
}

void Spring::step(float target, float dt, float k, float c) {
    // Semi-implicit Euler in steps of at most 4 ms: stable for these stiffnesses at any frame rate.
    dt = std::min(dt, 0.1f);
    while (dt > 0.0f) {
        const float h = std::min(dt, 0.004f);
        v += (k * (target - x) - c * v) * h;
        x += v * h;
        dt -= h;
    }
}

namespace {
// A label's tint: dark on a light button, white on a pressed (dark) one.
constexpr float kLabelDark = 0.16f;
// Push a sprite unless it is invisible or there is no room.
size_t put(OverlaySprite* out, size_t k, size_t max, const OverlaySprite& s) {
    if (k >= max || s.alpha <= 0.003f) return k;
    out[k] = s;
    return k + 1;
}
}  // namespace

size_t button_sprites(const TouchButton& b, float press, float ripple, float shade, OverlaySprite* out, size_t max) {
    const uint64_t k = b.button;
    const bool trigger = k == kZL || k == kZR;
    const bool shoulder = k == kL || k == kR;
    int glyph = art::kGlyphA;
    float g = b.r * 0.42f;  // the label's half-height
    if (k == kA) glyph = art::kGlyphA;
    else if (k == kB) glyph = art::kGlyphB;
    else if (k == kX) glyph = art::kGlyphX;
    else if (k == kY) glyph = art::kGlyphY;
    else if (k == kL) glyph = art::kGlyphL, g = b.r * 0.40f;
    else if (k == kR) glyph = art::kGlyphR, g = b.r * 0.40f;
    else if (k == kZL) glyph = art::kGlyphZL, g = b.r * 0.32f;
    else if (k == kZR) glyph = art::kGlyphZR, g = b.r * 0.32f;
    else if (k == kPlus) glyph = art::kGlyphPlus, g = b.r * 0.50f;
    else if (k == kMinus) glyph = art::kGlyphMinus, g = b.r * 0.50f;
    else return 0;
    const float a = std::min(1.0f, shade);
    const float p = std::clamp(press, 0.0f, 1.0f);
    const float s = 1.0f - 0.08f * press;  // the dip follows the spring, overshoot included
    float hw = b.r, hh = b.r;
    int rest = art::kDisc, down = art::kDiscHeld;
    if (trigger) {  // ZL / ZR: a trigger whose long, rounded edge faces the screen's top corner
        const bool left = b.x < 0.5f;
        hw = b.r * 1.45f, hh = b.r * 0.86f;
        rest = left ? art::kTriggerL : art::kTriggerR, down = left ? art::kTriggerLHeld : art::kTriggerRHeld;
    } else if (shoulder) {
        hw = b.r * 1.55f, hh = b.r * 0.78f, rest = art::kPill, down = art::kPillHeld;
    }
    size_t n = 0;
    if (ripple >= 0.0f && ripple < 1.0f) {  // the press ripple: a ring growing out of the button and fading
        const float e = 1.0f - (1.0f - ripple) * (1.0f - ripple) * (1.0f - ripple);  // ease out
        const float rr = std::max(hw, hh) * (1.0f + 0.55f * e);
        n = put(out, n, max, {b.x, b.y, rr, rr, art::kRing, a * 0.55f * (1.0f - ripple), 1.0f});
    }
    n = put(out, n, max, {b.x, b.y, hw * s, hh * s, rest, a * (1.0f - p), 1.0f});
    n = put(out, n, max, {b.x, b.y, hw * s, hh * s, down, a * p, 1.0f});
    const float gw = (glyph == art::kGlyphZL || glyph == art::kGlyphZR) ? g * 2.0f : g;
    n = put(out, n, max, {b.x, b.y + (trigger ? b.r * 0.08f : 0.0f), gw * s, g * s, glyph, a,
                          kLabelDark + (1.0f - kLabelDark) * p});
    return n;
}

size_t dpad_sprites(const tz::Dpad& d, const float arms[4], float shade, OverlaySprite* out, size_t max) {
    if (!d.valid) return 0;
    const float a = std::min(1.0f, shade);
    float any = 0.0f;
    for (int i = 0; i < 4; i++) any = std::max(any, arms[i]);
    const float s = (1.0f - 0.04f * any) * d.s * 1.05f;  // the whole cross gives a little under the thumb
    size_t n = put(out, 0, max, {d.x, d.y, s, s, art::kDpad, a, 1.0f});
    static constexpr int kArms[4] = {art::kDpadUp, art::kDpadRight, art::kDpadDown, art::kDpadLeft};
    for (int i = 0; i < 4; i++) n = put(out, n, max, {d.x, d.y, s, s, kArms[i], a * std::clamp(arms[i], 0.0f, 1.0f), 1.0f});
    return n;
}

size_t stick_sprites(float x0, float y0, float dx, float dy, float radius, float active, float aspect, float shade,
                     OverlaySprite* out, size_t max) {
    const float len = std::hypot(dx, dy), lim = radius * 1.08f, s = len > lim ? lim / len : 1.0f;  // overshoot room
    const float a = std::min(1.0f, shade), p = std::clamp(active, 0.0f, 1.0f);
    const float base = radius * 1.08f * (1.0f + 0.03f * active), thumb = radius * 0.50f * (1.0f + 0.06f * active);
    size_t n = put(out, 0, max, {x0, y0, base, base, art::kStickBase, a * (1.0f - p), 1.0f});
    n = put(out, n, max, {x0, y0, base, base, art::kStickBaseActive, a * p, 1.0f});
    const float tx = x0 + dx * s / aspect, ty = y0 + dy * s;
    n = put(out, n, max, {tx, ty, thumb, thumb, art::kStickThumb, a * (1.0f - p), 1.0f});
    n = put(out, n, max, {tx, ty, thumb, thumb, art::kStickThumbActive, a * p, 1.0f});
    return n;
}

#if !defined(__ANDROID__)
// NX_OVERLAY_DEMO=1: the controls as a player sees them mid-fight -- A pressed with its ripple half out, the D-pad's up
// held, the left stick pushed up-right, the right stick at rest -- to check the look on the desktop with
// NX_PRESENT_TEST=<w>x<h>.
size_t touch_sprites(OverlaySprite* out, size_t max) {
    static const bool demo = std::getenv("NX_OVERLAY_DEMO") != nullptr;
    if (!demo) return 0;
    static const float aspect = [] {
        unsigned w = 16, h = 9;
        if (const char* e = std::getenv("NX_PRESENT_TEST")) std::sscanf(e, "%ux%u", &w, &h);
        return h ? float(w) / float(h) : 16.0f / 9.0f;
    }();
    const float shade = touch_opacity_scale();
    size_t n = 0, k = 0;
    const TouchButton* b = touch_buttons(n);
    for (size_t i = 0; i < n; i++) {
        const bool a = b[i].button == kA;
        k += button_sprites(b[i], a ? 1.0f : 0.0f, a ? 0.4f : -1.0f, shade, out + k, max - k);
    }
    const float arms[4] = {1, 0, 0, 0};
    k += dpad_sprites(touch_dpad(), arms, shade, out + k, max - k);
    float x = 0, y = 0;
    touch_stick_rest(false, x, y);
    k += stick_sprites(x, y, 0.06f, -0.05f, 0.12f, 1.0f, aspect, shade, out + k, max - k);
    touch_stick_rest(true, x, y);
    k += stick_sprites(x, y, 0.0f, 0.0f, 0.10f, 0.0f, aspect, shade, out + k, max - k);
    return k;
}
#endif

}  // namespace nx::plat
