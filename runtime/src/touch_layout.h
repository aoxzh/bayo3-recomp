// Drawn touch controls: the per-game button layout (touch_layout.cpp) and the geometry it uses (touch_zones.h).
#pragma once

#include <cstddef>
#include <cstdint>

#include "platform.h"
#include "touch_zones.h"

namespace nx::plat {

struct TouchButton {
    float x, y, r;  // centre across the landscape screen (0..1), radius as a fraction of its height
    uint64_t button;
};
// The game's layout (count 0: the game handles touch its own way).
const TouchButton* touch_buttons(size_t& count);
// Launcher settings (config.txt NX_TOUCH_*): drawn and accepted at all, stick on the right half, brightness scale.
bool touch_enabled();
// NX_TOUCH_SHOW=2 (launcher: Touch controls "Always"): still drawn while a controller is in use.
bool touch_always();
bool touch_mirrored();
float touch_opacity_scale();
// The button under (x, y) (0..1 across the screen; aspect = width / height), or 0.
uint64_t touch_hit(float x, float y, float aspect);
// Where a finger that lands on no button becomes a stick (NX_TOUCH_MIRROR already applied); invalid when the game
// wants none. The camera stick feeds the right stick, the movement stick the left one.
tz::StickArea touch_move_stick();
tz::StickArea touch_camera_stick();
// Wonder: a movement stick pushed nearly all the way also holds Y (dash), so running needs no second thumb.
bool touch_dash_full_tilt();

// The game's D-pad (NX_TOUCH_MIRROR applied; invalid when it has none), and where a stick is drawn at rest.
tz::Dpad touch_dpad();
void touch_stick_rest(bool camera, float& x, float& y);
// Motion: a damped spring (x moves towards a target, v its velocity). The defaults overshoot about 10% and settle in
// a quarter second, the feel of a soft press; the drawn controls step one per present.
struct Spring {
    float x = 0.0f, v = 0.0f;
    void step(float target, float dt, float k = 520.0f, float c = 26.0f);
};

// The sprites (touch_art.h) of the drawn controls, all with `shade` (touch_opacity_scale) as the opacity:
// - a button: its body (a round button, a pill for L/R, a trigger for ZL/ZR) and label; `press` (a spring, 0 up,
//   1 down, overshoot allowed) dips it and fades it to the dark pressed art; `ripple` (0..1, <0 none) is the age of
//   the ring a press sends out.
// - the D-pad: the cross and its arms, each lit by arms[i] (0..1; up, right, down, left).
// - a stick: the range centred at (x0, y0) (0..1 across the screen) and the thumb moved by (dx, dy) (fractions of
//   the screen's height); `active` (0..1) fades in the held art. aspect = width / height.
// Each returns how many sprites it wrote (invisible ones are skipped).
size_t button_sprites(const TouchButton& b, float press, float ripple, float shade, OverlaySprite* out, size_t max);
size_t dpad_sprites(const tz::Dpad& d, const float arms[4], float shade, OverlaySprite* out, size_t max);
size_t stick_sprites(float x0, float y0, float dx, float dy, float radius, float active, float aspect, float shade,
                     OverlaySprite* out, size_t max);

}  // namespace nx::plat
