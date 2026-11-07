// The touch control geometry (runtime/src/touch_zones.h): layouts, hit testing, the stick rectangles and the
// launcher's NX_TOUCH_LAYOUT / NX_TOUCH_SIZE overrides. No phone, no Vulkan, no game.
#include "../runtime/src/touch_zones.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <stdexcept>

using namespace nx::plat;

namespace {
void check(bool ok, const char* why) {
    if (!ok) throw std::runtime_error(why);
}

// Test a wide landscape screen and a nearly square screen.
constexpr float kPhoneAspect = 2.22f;
constexpr float kSquareAspect = 1.20f;

size_t index_of(const char* name) {
    for (size_t i = 0; i < tz::kBayonetta3Count; i++)
        if (std::strcmp(tz::kBayonetta3Names[i], name) == 0) return i;
    throw std::runtime_error("no such zone");
}

size_t wonder_index_of(const char* name) {
    for (size_t i = 0; i < tz::kWonderCount; i++)
        if (std::strcmp(tz::kWonderNames[i], name) == 0) return i;
    throw std::runtime_error("no such Wonder zone");
}

void every_button_is_reachable_and_distinct() {
    // Bayonetta 3: each button answers at its own centre, and the answer is that button.
    for (size_t i = 0; i < tz::kBayonetta3Count; i++) {
        const tz::Zone& z = tz::kBayonetta3[i];
        check(tz::zone_hit(tz::kBayonetta3, tz::kBayonetta3Count, z.x, z.y, kPhoneAspect) == z.button,
              "a press on a button's centre hits that button");
    }
    // The four face buttons are distinct bits and no two overlap at their centres.
    uint64_t all = 0;
    for (size_t i = 0; i < tz::kBayonetta3Count; i++) {
        check((all & tz::kBayonetta3[i].button) == 0, "no button bit is used twice");
        all |= tz::kBayonetta3[i].button;
    }
    check((all & (tz::kA | tz::kB | tz::kX | tz::kY)) == (tz::kA | tz::kB | tz::kX | tz::kY), "all four faces");
    check((all & tz::kZR) != 0, "the dodge (ZR) has a button");
    check((all & (tz::kL | tz::kR | tz::kZL)) == (tz::kL | tz::kR | tz::kZL), "shoulders and the left trigger");
    check((all & (tz::kPlus | tz::kMinus)) == (tz::kPlus | tz::kMinus), "+ and -");
}

void the_buttons_stay_on_screen_and_apart() {
    for (size_t i = 0; i < tz::kBayonetta3Count; i++) {
        const tz::Zone& a = tz::kBayonetta3[i];
        check(a.x - a.r >= -0.02f && a.x + a.r <= 1.02f, "inside the screen horizontally");
        check(a.y - a.r >= -0.02f && a.y + a.r <= 1.02f, "inside the screen vertically");
        check(a.r > 0.03f && a.r < 0.12f, "a thumb-sized radius");
        for (size_t j = i + 1; j < tz::kBayonetta3Count; j++) {
            const tz::Zone& b = tz::kBayonetta3[j];
            // Centres apart by more than the two radii: the buttons do not overlap, so a press is unambiguous.
            const float d = tz::zone_distance(a, b.x, b.y, 1);
            const float back = tz::zone_distance(b, a.x, a.y, 1);
            check(d * a.r + back * b.r > a.r + b.r, "buttons do not overlap");
        }
    }
}

void the_faces_sit_where_a_right_thumb_expects() {
    // A Nintendo diamond: B bottom, A right, Y left, X top (the Switch's own arrangement).
    const tz::Zone& b = tz::kBayonetta3[index_of("B")];
    const tz::Zone& a = tz::kBayonetta3[index_of("A")];
    const tz::Zone& y = tz::kBayonetta3[index_of("Y")];
    const tz::Zone& x = tz::kBayonetta3[index_of("X")];
    // The diamond's two middle buttons (A right, Y left) share a height; B is below them, X above.
    check(b.y > a.y && a.y > x.y, "B is the lowest face, X the highest");
    check(a.y > y.y - 0.06f && a.y < y.y + 0.06f, "A and Y sit at the diamond's middle height");
    check(b.x < a.x && y.x < x.x && y.x < a.x, "Y is the leftmost face, A/B on the right");
    check(a.x > 0.8f && b.x > 0.7f, "the diamond is in the bottom-right corner");
    check(tz::kBayonetta3[index_of("ZR")].y < x.y, "the dodge sits above the diamond, under the thumb's rest");
    // The dodge is pressed far more often than any single face button, so it is not the smallest circle.
    float smallest_face = tz::kBayonetta3[index_of("X")].r;
    for (const char* f : {"A", "B", "Y"})
        if (tz::kBayonetta3[index_of(f)].r < smallest_face) smallest_face = tz::kBayonetta3[index_of(f)].r;
    check(tz::kBayonetta3[index_of("ZR")].r >= smallest_face, "the dodge is no smaller than the smallest face");
}

void the_sticks_do_not_swallow_the_buttons() {
    // A finger that lands on a button is that button, never a stick: the rectangles must exclude every button.
    for (size_t i = 0; i < tz::kBayonetta3Count; i++) {
        const tz::Zone& z = tz::kBayonetta3[i];
        check(tz::zone_hit(tz::kBayonetta3, tz::kBayonetta3Count, z.x, z.y, kPhoneAspect) != 0, "the button wins");
    }
    check(tz::stick_contains(tz::kBayonetta3MoveStick, 0.20f, 0.70f), "the left thumb's stick");
    check(!tz::stick_contains(tz::kBayonetta3MoveStick, 0.85f, 0.70f), "not on the face buttons");
    check(tz::stick_contains(tz::kBayonetta3CameraStick, 0.60f, 0.70f), "the camera stick");
    check(!tz::stick_contains(tz::kBayonetta3CameraStick, 0.20f, 0.70f), "the camera stick is not the left half");
    check(!tz::stick_contains(tz::kBayonetta3CameraStick, 0.60f, 0.20f), "not the top strip");
    // The two sticks never overlap, at any of these aspects (they are x-separated).
    check(tz::kBayonetta3MoveStick.x1 <= tz::kBayonetta3CameraStick.x0, "movement and camera rectangles are apart");
    // A game without a camera stick: the rectangle is invalid, so nothing becomes one.
    check(!tz::stick_contains(tz::kWonderNoCamera, 0.6f, 0.6f), "no camera stick for Wonder");
    check(tz::kWonderMoveStick.valid && tz::stick_contains(tz::kWonderMoveStick, 0.2f, 0.6f), "Wonder's stick");
}

void a_press_a_little_past_the_edge_still_counts() {
    const tz::Zone& a = tz::kBayonetta3[index_of("A")];
    // Below the button (the y direction is not aspect-weighted): 20% past the edge still presses, 2 radii away not.
    check(tz::zone_hit(tz::kBayonetta3, tz::kBayonetta3Count, a.x, a.y + a.r * 1.2f, kPhoneAspect) == tz::kA,
          "20% past the drawn edge still presses A");
    check(tz::zone_hit(tz::kBayonetta3, tz::kBayonetta3Count, a.x, a.y + a.r * 2.0f, kPhoneAspect) != tz::kA,
          "twice the radius away does not");
    // Empty screen: nothing.
    check(tz::zone_hit(tz::kBayonetta3, tz::kBayonetta3Count, 0.5f, 0.5f, kPhoneAspect) == 0, "empty middle");
}

void a_square_screen_does_not_break_the_hit_test() {
    // On a nearly square screen, the layout must still answer at each button's centre, and the
    // horizontal distance must be scaled by the aspect (otherwise the whole diamond would distort).
    for (size_t i = 0; i < tz::kBayonetta3Count; i++) {
        const tz::Zone& z = tz::kBayonetta3[i];
        check(tz::zone_hit(tz::kBayonetta3, tz::kBayonetta3Count, z.x, z.y, kSquareAspect) == z.button,
              "a square screen still hits");
    }
    // At aspect 1 the x difference is not scaled: a point one radius to the right is at the edge.
    const tz::Zone z{0.5f, 0.5f, 0.1f, tz::kA};
    check(tz::zone_hit(&z, 1, 0.599f, 0.5f, 1.0f) == tz::kA, "inside at aspect 1");
    check(tz::zone_hit(&z, 1, 0.65f, 0.5f, 1.0f) == 0, "past the tolerance at aspect 1");
    // At aspect 2 the same x distance is twice as far: outside, where at aspect 1 it was inside.
    check(tz::zone_hit(&z, 1, 0.57f, 0.5f, 1.0f) == tz::kA, "0.7 radii to the right is inside at aspect 1");
    check(tz::zone_hit(&z, 1, 0.57f, 0.5f, 2.0f) == 0, "the same touch is 1.4 radii away at aspect 2: the x "
                                                       "distance is weighted by the aspect");
}

void the_launcher_can_move_and_resize_buttons() {
    check(tz::size_scale(100) == 1.0f, "100% is unchanged");
    check(tz::size_scale(50) == 0.5f && tz::size_scale(160) == 1.6f, "the ends of the range");
    check(tz::size_scale(0) == 0.5f, "below the range is clamped");
    check(tz::size_scale(1000) == 1.6f, "above the range is clamped");

    tz::Zone z[2] = {{0.1f, 0.1f, 0.05f, tz::kA}, {0.9f, 0.9f, 0.05f, tz::kB}};
    const char* const names[2] = {"A", "B"};
    // A whole entry, a partial one (spacing), an unknown name, a broken number and an out-of-range radius.
    const size_t applied = tz::apply_layout_override(z, 2, names,
                                                     "A:0.2,0.3,0.06; B:0.5,0.5,0.07;C:0.5,0.5,0.05;A:x,y,z;"
                                                     "B:0.5,0.5,0.9;A:0.2,0.3");
    check(applied == 2, "two entries applied, the rest skipped");
    check(z[0].x == 0.2f && z[0].y == 0.3f && z[0].r == 0.06f, "A moved");
    check(z[1].x == 0.5f && z[1].y == 0.5f && z[1].r == 0.07f, "B moved; the out-of-range radius was skipped");
    check(tz::apply_layout_override(z, 2, names, "") == 0, "an empty setting changes nothing");
    check(tz::apply_layout_override(z, 2, names, nullptr) == 0, "an unset setting changes nothing");
    // A name that is a prefix of another must not match the wrong zone ("L" vs "PLUS").
    tz::Zone p[2] = {{0.1f, 0.1f, 0.05f, tz::kL}, {0.9f, 0.9f, 0.05f, tz::kPlus}};
    const char* const pn[2] = {"L", "PLUS"};
    check(tz::apply_layout_override(p, 2, pn, "PLUS:0.4,0.4,0.04") == 1, "exact names only");
    check(p[0].x == 0.1f && p[1].x == 0.4f, "PLUS moved, L did not");
    check(tz::apply_layout_override(p, 2, pn, "P:0.4,0.4,0.04") == 0, "a partial name does not match");

    tz::Zone s[1] = {{0.1f, 0.1f, 0.05f, tz::kA}};
    tz::apply_size_scale(s, 1, 160);
    check(s[0].r > 0.079f && s[0].r < 0.081f, "the size scale applies to the radius only");
    check(s[0].x == 0.1f && s[0].y == 0.1f, "the centre is untouched by the size scale");
    // The overrides are what the drawn overlay and the hit test share, so a moved button stays pressable.
    tz::Zone moved[1] = {{0.5f, 0.5f, 0.05f, tz::kA}};
    check(tz::zone_hit(moved, 1, 0.5f, 0.5f, kPhoneAspect) == tz::kA, "a moved button is hit where it is drawn");
}

void wonder_keeps_its_layout() {
    check(tz::kWonderCount == 9, "Wonder's nine buttons");
    for (size_t i = 0; i < tz::kWonderCount; i++) {
        const tz::Zone& z = tz::kWonder[i];
        check(tz::zone_hit(tz::kWonder, tz::kWonderCount, z.x, z.y, 1.777f) == z.button, "Wonder centre hits");
    }
    check(tz::kWonder[wonder_index_of("A")].r > tz::kWonder[wonder_index_of("X")].r, "Wonder's A is the big one");
    check(tz::kWonder[wonder_index_of("A")].button == tz::kA, "Wonder's big button is A");
}
}  // namespace

void the_dpad_reads_directions_and_diagonals() {
    const tz::Dpad& d = tz::kBayonetta3Dpad;
    const float a = kPhoneAspect, o = d.s * 0.7f;  // 0.7 of an arm out from the centre
    check(tz::dpad_hit(d, d.x, d.y - o, a) == tz::kUp, "up");
    check(tz::dpad_hit(d, d.x, d.y + o, a) == tz::kDown, "down");
    check(tz::dpad_hit(d, d.x - o / a, d.y, a) == tz::kLeft, "left");
    check(tz::dpad_hit(d, d.x + o / a, d.y, a) == tz::kRight, "right");
    check(tz::dpad_hit(d, d.x + o / a, d.y - o, a) == (tz::kUp | tz::kRight), "a diagonal presses both");
    check(tz::dpad_hit(d, d.x, d.y, a) == 0, "the dead centre presses nothing");
    check(tz::dpad_hit(d, d.x, d.y - d.s * 1.8f, a) == 0, "off the cross: nothing");
    check(tz::dpad_hit(d, d.x, d.y - d.s * 1.8f, a, true) == tz::kUp, "a held finger may slide a little off");
    // It sits below-right of the left stick's rest and clear of every button, on a wide and on a square screen.
    for (float asp : {kPhoneAspect, kSquareAspect})
        for (size_t i = 0; i < tz::kBayonetta3Count; i++) {
            const tz::Zone& z = tz::kBayonetta3[i];
            check(tz::dpad_hit(d, z.x, z.y, asp) == 0, "no button centre lies on the D-pad");
        }
    check(d.x > tz::kBayonetta3MoveRest[0] && d.y > tz::kBayonetta3MoveRest[1], "a left Joy-Con: stick above, D-pad below-right");
}

int main() {
    const struct {
        const char* name;
        void (*fn)();
    } tests[] = {
        {"every_button_is_reachable_and_distinct", every_button_is_reachable_and_distinct},
        {"the_buttons_stay_on_screen_and_apart", the_buttons_stay_on_screen_and_apart},
        {"the_faces_sit_where_a_right_thumb_expects", the_faces_sit_where_a_right_thumb_expects},
        {"the_sticks_do_not_swallow_the_buttons", the_sticks_do_not_swallow_the_buttons},
        {"a_press_a_little_past_the_edge_still_counts", a_press_a_little_past_the_edge_still_counts},
        {"a_square_screen_does_not_break_the_hit_test", a_square_screen_does_not_break_the_hit_test},
        {"the_launcher_can_move_and_resize_buttons", the_launcher_can_move_and_resize_buttons},
        {"wonder_keeps_its_layout", wonder_keeps_its_layout},
        {"the_dpad_reads_directions_and_diagonals", the_dpad_reads_directions_and_diagonals},
    };
    int failed = 0;
    for (const auto& t : tests) {
        try {
            t.fn();
            std::printf("ok   %s\n", t.name);
        } catch (const std::exception& e) {
            std::printf("FAIL %s: %s\n", t.name, e.what());
            failed++;
        }
    }
    std::printf("%zu tests, %d failed\n", sizeof(tests) / sizeof(tests[0]), failed);
    return failed ? 1 : 0;
}
