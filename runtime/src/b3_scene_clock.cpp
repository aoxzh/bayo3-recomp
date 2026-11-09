// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from Witch Time runtime/scene_step.h and the fixed-step branch of
// runtime/game_shim_eh.c, commit beffd6bd250d2da359b25a289cbb5bc5bccc4750.
// See docs/WITCH-TIME-NOTICES.md for attribution and distribution terms.
#include "nx/b3_scene_clock.h"

#include "nx/b3_fps_counters.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>

#include "nx/cpu.h"      // NX_BASE
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

std::atomic<int> g_display_hz{0};

long probe_every() {
    static const long n = [] {
        const char* e = std::getenv("NX_SCENE_PROBE");
        long v = e ? std::atol(e) : 0;
        return v > 0 ? v : 0;
    }();
    return n;
}

// NX_FPS_STEP: "0" keeps the game's own measured step, "1" always applies measured_step(); unset applies it only
// when the display clock is above 60 Hz, so a 60 Hz (or slower) host runs the console's arithmetic unchanged.
bool fps_step_enabled() {
    static const int forced = [] {
        const char* e = std::getenv("NX_FPS_STEP");
        return e && *e ? (std::strcmp(e, "0") == 0 ? 0 : 1) : -1;
    }();
    return forced >= 0 ? forced == 1 : g_display_hz.load(std::memory_order_relaxed) > 60;
}

// The game's step override (main+0xBA3300 returns *(*(main+0x24AAEA0))): above 0 it replaces the measured step.
float guest_step_override() {
    uint64_t p = 0;
    std::memcpy(&p, reinterpret_cast<const void*>(std::uintptr_t(NX_BASE + 0x24AAEA0ull)), sizeof(p));
    if (!p) return 0.0f;
    float v = 0.0f;
    std::memcpy(&v, reinterpret_cast<const void*>(std::uintptr_t(p)), sizeof(v));
    return v;
}

// Measured (gameplay) clocks: game-time per wall-time over NX_SCENE_PROBE calls, logged with the step it ended at.
void probe_measured(uint64_t clock) {
    const long every = probe_every();
    if (!every) return;
    static thread_local double game_ms = 0.0, wall_ms = 0.0;
    static thread_local long calls = 0;
    const auto base = static_cast<std::uintptr_t>(clock);
    float step, per, ms;
    std::memcpy(&step, reinterpret_cast<const void*>(base + 0xe0), sizeof(step));
    std::memcpy(&per, reinterpret_cast<const void*>(base + 0xec), sizeof(per));
    std::memcpy(&ms, reinterpret_cast<const void*>(base + 0xf0), sizeof(ms));
    if (!std::isfinite(step) || !std::isfinite(per) || !std::isfinite(ms) || ms <= 0.0f || ms > 200.0f) return;
    game_ms += double(step) * double(per);
    wall_ms += double(ms);
    if (++calls % every == 0) {
        log("measured probe: %ld calls, step %.4f, per %.3f ms, frame %.3f ms, game/wall %.3f over the last %ld",
            calls, double(step), double(per), double(ms), wall_ms > 0.0 ? game_ms / wall_ms : 0.0, every);
        game_ms = wall_ms = 0.0;
    }
}

}  // namespace

float measured_step(float ms, float per, float override_step) {
    if (override_step > 0.0f || !(per > 0.0f) || !std::isfinite(ms) || !(ms > 0.0f)) return -1.0f;
    return std::clamp(ms / per, 1.0f / 16.0f, 2.0f);
}

void scene_clock_set_display_hz(int hz) { g_display_hz.store(hz, std::memory_order_relaxed); }

namespace {

void correct_clock(uint64_t clock, bool fixed) {
    auto& state = g_scene_clock;
    if (!fixed && clock && scene_time_enabled() && fps_step_enabled()) {
        // Above 60 Hz the game's measured step floors at 1.0 (one 60 Hz frame) and banks the rest as carry at +0xe4,
        // so every frame at 120 still advances a full 1/60 s: gameplay ran at the frame rate's multiple of real time.
        // Witch Time game_shim_eh.c h_frame_clock (frame_rate above 60) replaces it with this frame's ms / the 60 Hz
        // frame's ms (+0xec), floored at 1/16 and with no carry; the 2.0 ceiling and the game's override stay.
        const auto base = static_cast<std::uintptr_t>(clock);
        float ms, per;
        std::memcpy(&ms, reinterpret_cast<const void*>(base + 0xf0), sizeof(ms));
        std::memcpy(&per, reinterpret_cast<const void*>(base + 0xec), sizeof(per));
        const float s = measured_step(ms, per, guest_step_override());
        if (s > 0.0f) {
            const float zero = 0.0f;
            std::memcpy(reinterpret_cast<void*>(base + 0xe0), &s, sizeof(s));
            std::memcpy(reinterpret_cast<void*>(base + 0xe4), &zero, sizeof(zero));
        }
    }
    if (!fixed && clock) probe_measured(clock);
    // NX_SCENE_PROBE=<n>: every n calls, log what the game thinks the elapsed scene time is (base+0xf0, ms) and the
    // step it applies (base+0xe0, seconds), so the question "does the simulation follow wall time or the frame
    // counter" can be answered by measurement instead of inference. At 60 Hz a delta-timed game reports ~16.7 ms
    // here; a fixed-step game keeps reporting its own step and would run at 2x above 60. Read once; unset or 0 off.
    const long probe_every = nx::b3::probe_every();
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

}  // namespace

void scene_clock_after(uint64_t clock, bool fixed) {
    correct_clock(clock, fixed);
    // The frame counters of recomp/b3_hooks.py FPS_SITES move by the frames this update's step makes due.
    const bool above_60 = clock && scene_time_enabled() && fps_step_enabled();
    float step = 1.0f;
    if (above_60) std::memcpy(&step, reinterpret_cast<const void*>(std::uintptr_t(clock) + 0xe0), sizeof(step));
    fps_counters_publish(step, above_60);
}

}  // namespace nx::b3
