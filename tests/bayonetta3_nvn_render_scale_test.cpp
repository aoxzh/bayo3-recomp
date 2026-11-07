// The render-size policy (runtime/src/render_scale.h): console sizes, explicit NX_RENDER_RES, NX_RENDER_SCALE, bounds.
#include "../runtime/src/render_scale.h"
#include <cstdio>
#include <stdexcept>

using namespace nx::plat;
namespace {
void check(bool ok, const char* why) {
    if (!ok) throw std::runtime_error(why);
}
bool same(RenderSize a, uint32_t w, uint32_t h) { return a.w == w && a.h == h; }

void console_sizes() {
    check(same(console_render_size(false), 1920, 1080), "docked");
    check(same(console_render_size(true), 1280, 720), "handheld");
    // Unset switches keep exactly the sizes the runtime had before this policy.
    check(same(render_size(false, nullptr, nullptr), 1920, 1080), "docked default");
    check(same(render_size(true, nullptr, nullptr), 1280, 720), "handheld default");
}

void explicit_size_wins() {
    check(same(render_size(false, "1280x720", nullptr), 1280, 720), "explicit docked");
    check(same(render_size(true, "1920x1080", nullptr), 1920, 1080), "explicit handheld");
    check(same(render_size(false, "1280X720", nullptr), 1280, 720), "upper-case x");
    // An explicit size is not scaled further, and a bad one falls back to the console size.
    check(same(render_size(false, "1280x720", "0.5"), 1280, 720), "explicit beats scale");
    check(same(render_size(false, "junk", "0.5"), 960, 540), "bad explicit falls back to the scale");
    check(same(render_size(false, "64x64", nullptr), 1920, 1080), "too small is rejected");
    check(same(render_size(false, "99999x99999", nullptr), 1920, 1080), "too large is rejected");
    check(same(render_size(false, "1280x720x", nullptr), 1920, 1080), "trailing junk is rejected");
}

void scale_applies_to_the_console_size() {
    check(same(render_size(false, nullptr, "0.5"), 960, 540), "half of docked");
    check(same(render_size(true, nullptr, "0.5"), 640, 360), "half of handheld");
    check(same(render_size(false, nullptr, "1.5"), 2880, 1620), "one and a half");
    check(same(render_size(false, nullptr, "2"), 3840, 2160), "double");
    check(same(render_size(false, nullptr, "0.75"), 1440, 810), "three quarters");
    // Out of range, junk and empty are 1.0: a launcher cannot push the game into an unsupported size.
    check(same(render_size(false, nullptr, "0.1"), 1920, 1080), "below the minimum factor");
    check(same(render_size(false, nullptr, "3"), 1920, 1080), "above the maximum factor");
    check(same(render_size(false, nullptr, "junk"), 1920, 1080), "junk factor");
    check(same(render_size(false, nullptr, ""), 1920, 1080), "empty factor");
    check(render_scale_from_env("0.5") == 0.5f && render_scale_from_env(nullptr) == 1.0f, "factor parsing");
}

void auto_fits_sixteen_by_nine() {
    // `resolution=auto`: the largest 16:9 rectangle inside the display, so a phone, tablet or 16:10 panel shows a
    // correctly shaped picture (the compositor letterboxes the window itself).
    check(same(fit_aspect(1280, 800), 1280, 720), "16:10 panel");
    check(same(fit_aspect(2520, 1680), 2520, 1416), "wide desktop");
    check(same(fit_aspect(1920, 1080), 1920, 1080), "exact 16:9");
    check(same(fit_aspect(1080, 2400), 1080, 606), "phone in portrait");
    check(same(fit_aspect(2400, 1080), 1920, 1080), "phone in landscape");
    check(same(fit_aspect(0, 0), 1280, 720), "no display: the old default");
    // An odd fit is rounded down to even dimensions like every other size.
    check(fit_aspect(1281, 801).w % 2 == 0 && fit_aspect(1281, 801).h % 2 == 0, "even");
    check(fit_aspect(1024, 768, 4, 3) == (RenderSize{1024, 768}), "other aspects");
}

void bounds_and_parity() {
    // Odd results are rounded down to even, so every target stays 2-aligned.
    check(same(clamp_render_size({961, 541}), 960, 540), "even rounding");
    check(same(clamp_render_size({0, 0}), 320, 180), "minimum");
    check(same(clamp_render_size({9000, 9000}), 7680, 4320), "maximum");
    // Phone and tablet aspects keep the 16:9 render size: the window is a separate, freely shaped size.
    check(same(render_size(true, nullptr, "0.5"), 640, 360), "16:9 stays 16:9");
    RenderSize s{};
    check(parse_render_res("1600x900", s) && same(s, 1600, 900), "parse");
    check(!parse_render_res("1600", s) && !parse_render_res(nullptr, s), "reject incomplete");
}

}  // namespace

int main() {
    try {
        console_sizes();
        explicit_size_wins();
        scale_applies_to_the_console_size();
        auto_fits_sixteen_by_nine();
        bounds_and_parity();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "render_scale: %s\n", e.what());
        return 1;
    }
    std::puts("render_scale: ok");
    return 0;
}
