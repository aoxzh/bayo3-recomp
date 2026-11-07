// The frame rate on screen (NX_FPS_OVERLAY=1, on by default on Android): a seven-segment readout in the top left
// corner of the picture, drawn by the present pass (nvn_present.cpp) from the rate this file measures.
//
// Why here and not the game's own overlay: the game's HUD is the guest's (and it has none for a host frame rate), so
// the only place that sees every presented frame is gpu_present. The digits are the present shader's, not a texture:
// there is no font to load on the render thread, and the pass already draws the touch circles there.
#include <cstdlib>

#include "nvn_vk.h"
#include "platform.h"

namespace nx::nvn {

namespace {

// Present-to-present intervals, averaged over a second: the panel's 30 or 60 fps steps make a one-frame rate jump
// around, and a rate that flickers is harder to read than one that lags by a frame.
constexpr uint64_t kWindowNs = 1'000'000'000ull;

bool overlay_on() {
    static const bool on = [] {
        const char* e = std::getenv("NX_FPS_OVERLAY");
        if (e) return e[0] == '1';
#if defined(__ANDROID__)
        return true;  // phones have no other way to see it; nothing else draws over the picture
#else
        return false;
#endif
    }();
    return on;
}

struct Meter {
    uint64_t from = 0, frames = 0;
    double rate = 0;
} g_meter;

}  // namespace

// Called once per presented frame (gpu_present, after the present was queued).
void fps_overlay_frame() {
    if (!overlay_on()) return;
    const uint64_t now = plat::monotonic_ns();
    if (!g_meter.from) {
        g_meter.from = now;
        return;
    }
    g_meter.frames++;
    const uint64_t span = now - g_meter.from;
    if (span < kWindowNs) return;
    const double rate = double(g_meter.frames) * 1e9 / double(span);
    // A first window stands on its own; after that a quarter of the new rate, so the digits settle instead of
    // toggling between two values on alternate windows.
    g_meter.rate = g_meter.rate > 0 ? g_meter.rate * 0.75 + rate * 0.25 : rate;
    g_meter.from = now, g_meter.frames = 0;
}

// The rate to show, or 0 while the readout is off or has not seen a second yet. The caller places it: it knows where
// the picture sits on the target (nvn_present.cpp's box), this file only knows how often frames came out.
double fps_overlay_rate() {
    if (!overlay_on()) return 0.0;
    return g_meter.rate;
}

}  // namespace nx::nvn
