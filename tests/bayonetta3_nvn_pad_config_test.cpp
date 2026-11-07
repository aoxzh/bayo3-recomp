// The player 1 controller policy (runtime/src/pad_config.h): switches, deadzone, trigger, layout and camera.
#include "../runtime/src/pad_config.h"
#include <cstdio>
#include <stdexcept>
#include <string>

using namespace nx::plat;
namespace {
void check(bool ok, const char* why) {
    if (!ok) throw std::runtime_error(why);
}
constexpr uint64_t kA = 1ull << 0, kB = 1ull << 1, kX = 1ull << 2, kY = 1ull << 3, kL = 1ull << 4;

void defaults_match_the_old_hard_coded_values() {
    const PadConfig c = pad_config_from_env(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    check(c.deadzone == 7849, "historical deadzone");
    check(c.trigger == 12000, "historical trigger");
    check(!c.swap_face && !c.invert_camera_x && !c.invert_camera_y && c.index == 0, "historical layout, pad 0");
    // With the historical deadzone the axis still leaves the centre where it did: 7849 is inside, 7850 is not.
    check(pad_axis(7848, c.deadzone) == 0, "inside the deadzone");
    check(pad_axis(-7848, c.deadzone) == 0, "inside the deadzone, negative");
    check(pad_axis(7850, c.deadzone) > 0, "outside the deadzone");
    check(pad_axis(-7850, c.deadzone) < 0, "outside the deadzone, negative");
}

void deadzone_stretches_the_remaining_travel() {
    // Just past the deadzone is a small deflection, not a quarter of the range (the old code jumped to 24%).
    check(pad_axis(7850, 7849) < 100, "smooth start");
    check(pad_axis(kPadStickMax, 7849) == kPadStickMax, "full deflection is preserved");
    check(pad_axis(-kPadStickMax, 7849) == -kPadStickMax, "full deflection is preserved, negative");
    check(pad_axis(kPadStickMax, 0) == kPadStickMax, "deadzone 0 is the identity");
    check(pad_axis(1234, 0) == 1234, "deadzone 0 is the identity");
    check(pad_axis(0, 7849) == 0, "centred");
    check(pad_axis(40000, 7849) == kPadStickMax, "clamped past full deflection");
    // Monotonic across the whole range, and never outside it.
    int32_t last = -1;
    for (int raw = 0; raw <= 32767; ++raw) {
        const int32_t v = pad_axis(raw, 7849);
        check(v >= last, "monotonic");
        check(v >= 0 && v <= kPadStickMax, "in range");
        last = v;
    }
}

void triggers_and_layout() {
    check(!pad_trigger_pressed(12000, 12000), "threshold is exclusive, as before");
    check(pad_trigger_pressed(12001, 12000), "past the threshold");
    check(pad_trigger_pressed(1, 0), "threshold 0 presses on any travel");
    check(!pad_trigger_pressed(0, 0), "a released trigger stays released");
    check(!pad_trigger_pressed(32766, 32767), "maximum threshold waits for full travel");
    check(pad_trigger_pressed(32767, 32767), "maximum threshold remains reachable");
    check(pad_swap_face(kA | kX) == (kB | kY), "A<->B and X<->Y");
    check(pad_swap_face(kA | kB | kX | kY) == (kB | kA | kY | kX), "swapping twice is the identity");
    check(pad_swap_face(kL) == kL, "shoulders are untouched");
    check(pad_swap_face(pad_swap_face(kA | kY)) == (kA | kY), "involution");
}

void switches_are_read_and_validated() {
    check(pad_flag_from_env("1") && pad_flag_from_env("yes") && pad_flag_from_env("true"), "on spellings");
    check(!pad_flag_from_env("0") && !pad_flag_from_env("off") && !pad_flag_from_env("no") &&
              !pad_flag_from_env("false") && !pad_flag_from_env("") && !pad_flag_from_env(nullptr),
          "off spellings");
    const PadConfig c = pad_config_from_env("1", "12000", "9000", "2", "1", "1");
    check(c.swap_face && c.invert_camera_x && c.invert_camera_y, "flags read");
    check(c.deadzone == 12000 && c.trigger == 9000 && c.index == 2, "numbers read");
    // Junk and out-of-range values keep the default instead of reaching the pad.
    const PadConfig j = pad_config_from_env("", "junk", "99999", "9", "", "");
    check(j.deadzone == kPadDefaultDeadzone && j.trigger == kPadDefaultTrigger && j.index == 0, "junk ignored");
    check(pad_config_from_env(nullptr, "0", nullptr, nullptr, nullptr, nullptr).deadzone == 0, "deadzone 0 accepted");
    check(pad_config_from_env(nullptr, "20000", nullptr, nullptr, nullptr, nullptr).deadzone == 20000, "upper bound");
    check(pad_config_from_env(nullptr, "20001", nullptr, nullptr, nullptr, nullptr).deadzone == kPadDefaultDeadzone,
          "past the upper bound");
}

void camera_inversion() {
    PadConfig c;
    int32_t rx = 1000, ry = -2000;
    pad_camera(rx, ry, c);
    check(rx == 1000 && ry == -2000, "default camera");
    c.invert_camera_x = true;
    pad_camera(rx, ry, c);
    check(rx == -1000 && ry == -2000, "inverted x only");
    c.invert_camera_x = false, c.invert_camera_y = true;
    pad_camera(rx, ry, c);
    check(rx == -1000 && ry == 2000, "inverted y only, x keeps the previous flip");
}

}  // namespace

int main() {
    try {
        defaults_match_the_old_hard_coded_values();
        deadzone_stretches_the_remaining_travel();
        triggers_and_layout();
        switches_are_read_and_validated();
        camera_inversion();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "pad_config: %s\n", e.what());
        return 1;
    }
    std::puts("pad_config: ok");
    return 0;
}
