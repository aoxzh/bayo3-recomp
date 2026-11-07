// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from Witch Time runtime/scene_step.h and the fixed-step branch of
// runtime/game_shim_eh.c, commit beffd6bd250d2da359b25a289cbb5bc5bccc4750.
// See docs/WITCH-TIME-NOTICES.md for attribution and distribution terms.
#include "nx/b3_scene_clock.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>

#include "nx/runtime.h"  // log

namespace nx::b3 {
namespace {

struct SceneClockState {
    uint64_t clock = 0;
    float owed = 0.0f;
    bool active = false;
};

thread_local SceneClockState g_scene_clock;

bool scene_time_enabled() {
    // Read once: only the exact value "0" opts out. Unset or other values opt in.
    static const bool enabled = [] {
        const char* value = std::getenv("NX_SCENE_TIME");
        return !value || std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

// Witch Time scene_step.h: debt is in 30 Hz frames. Keep its 90-frame cap,
// one-eighth repayment and 2.0 step cap for the ordinary fixed-30 path.
float scene_step(float* owed, float due, float even) {
    float o = *owed + due - even, k = even;
    if (o > 90.0f) o = 90.0f;
    if (o > even) k += (o - even) / 8.0f; else if (o < -even) k += (o + even) / 8.0f;
    if (k > 2.0f) k = 2.0f;
    *owed = o + even - k;
    return k;
}

}  // namespace

void scene_clock_after(uint64_t clock, bool fixed) {
    auto& state = g_scene_clock;
    // NX_SCENE_PROBE=<n>: every n calls, log what the game thinks the elapsed scene time is (base+0xf0, ms) and the
    // step it applies (base+0xe0, seconds), so the question "does the simulation follow wall time or the frame
    // counter" can be answered by measurement instead of inference. At 60 Hz a delta-timed game reports ~16.7 ms
    // here; a fixed-step game keeps reporting its own step and would run at 2x above 60. Read once; unset or 0 off.
    static const long probe_every = [] {
        const char* e = std::getenv("NX_SCENE_PROBE");
        long n = e ? std::atol(e) : 0;
        return n > 0 ? n : 0;
    }();
    // Measured/gameplay clocks and disabled operation keep the original result.
    // On mode or object changes, do not carry a previous scene's timing debt.
    if (!fixed || !clock || clock > std::numeric_limits<std::uintptr_t>::max() - 0xf3 ||
        !scene_time_enabled()) {
        state = {};
        return;
    }
    if (state.clock != clock) state = SceneClockState{clock, 0.0f, false};

    const auto base = static_cast<std::uintptr_t>(clock);
    float step, ms;
    std::memcpy(&step, reinterpret_cast<const void*>(base + 0xe0), sizeof(step));
    std::memcpy(&ms, reinterpret_cast<const void*>(base + 0xf0), sizeof(ms));
    // Invalid input or output must leave the original game value untouched.
    // Reset debt too, so a bad sample cannot affect the next valid entry.
    if (!std::isfinite(step) || !std::isfinite(ms) || ms <= 0.0f) {
        state = {};
        return;
    }
    if (!state.active) {
        state.active = true;  // The time spent loading before entry is not scene time.
        return;
    }
    const float due = ms / (1000.0f / 30.0f);
    float owed = state.owed;
    if (probe_every) {
        static long calls = 0, clock_changes = 0;
        static uint64_t last_clock = 0;
        if (clock != last_clock) last_clock = clock, ++clock_changes;
        if (++calls % probe_every == 0)
            log("scene probe: %ld calls, %ld clock objects, scene ms %.3f, step %.5f s (%.1f Hz), owed %.3f",
                calls, clock_changes, double(ms), double(step), ms > 0.0f ? 1000.0 / double(ms) : 0.0,
                double(owed));
    }
    const float k = scene_step(&owed, due, 1.0f);
    // Check in double precision before the float multiplication, so even an
    // extreme finite guest step cannot raise float overflow or be corrupted.
    const double wide = double(step) * double(k);
    if (!std::isfinite(owed) || !std::isfinite(k) || !std::isfinite(wide) ||
        std::fabs(wide) > std::numeric_limits<float>::max()) {
        state = {};
        return;
    }
    const float corrected = step * k;
    state.owed = owed;
    std::memcpy(reinterpret_cast<void*>(base + 0xe0), &corrected, sizeof(corrected));
}

}  // namespace nx::b3
