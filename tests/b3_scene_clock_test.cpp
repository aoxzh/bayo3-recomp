// SPDX-License-Identifier: GPL-3.0-or-later
// Exercises the production post-original Witch Time scene-clock adaptation.
#include "nx/b3_scene_clock.h"
#include "nx/b3_fps_counters.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <random>
#include <string_view>
#include <thread>

// The helper's probe logs through the runtime; the test links the helper alone.
namespace nx {
void log(const char*, ...) {}
}  // namespace nx

namespace {
constexpr float frame_ms = 1000.0f / 30.0f;

struct Clock {
    std::array<uint8_t, 0x100> bytes{};
    Clock() { bytes.fill(0xa5); }
    uint64_t address() { return uint64_t(reinterpret_cast<std::uintptr_t>(bytes.data())); }
    void put(size_t at, float value) { std::memcpy(bytes.data() + at, &value, sizeof(value)); }
    float get(size_t at) const {
        float value;
        std::memcpy(&value, bytes.data() + at, sizeof(value));
        return value;
    }
    float update(float ms, float original = 1.0f) {
        put(0xe0, original);
        put(0xf0, ms);
        nx::b3::scene_clock_after(address(), true);
        return get(0xe0);
    }
};

void reset() { nx::b3::scene_clock_after(0, false); }
bool near(float x, float y, float tolerance = 0.00001f) {
    return std::fabs(x - y) <= tolerance;
}

void stable_and_repayment() {
    reset();
    Clock c;
    // A long loading interval on first entry is not counted as scene time.
    assert(c.update(60000.0f) == 1.0f);
    for (int i = 0; i < 120; ++i) assert(c.update(frame_ms, 4.0f) == 4.0f);

    reset();
    assert(c.update(frame_ms) == 1.0f);
    assert(c.update(frame_ms * 2) == 1.0f);
    assert(c.update(frame_ms * 2) == 1.125f);
    c.put(0xe0, 4.0f);
    c.put(0xf0, frame_ms * 2);
    const auto before = c.bytes;
    nx::b3::scene_clock_after(c.address(), true);
    assert(c.get(0xe0) == 4.9375f);  // 4 * (1 + (2.875 - 1) / 8)
    for (size_t i = 0; i < c.bytes.size(); ++i)
        if (i < 0xe0 || i >= 0xe4) assert(c.bytes[i] == before[i]);
}

void low_rate_recovery() {
    reset();
    Clock c;
    constexpr int count = 240;
    constexpr float due = 30.0f / 22.0f;
    double total = 0;
    for (int i = 0; i < count; ++i) {
        const float step = c.update(1000.0f / 22.0f);
        assert(step >= 1.0f && step <= 2.0f);
        if (i > 150) assert(near(step, due));
        total += step;
    }
    // Entry skips one sample and upstream deliberately retains about four
    // frames of smoothing debt. The long-term rate still follows real time.
    assert(total > count * 1.30);
    assert(std::fabs(total - double(count) * due) < 4.5);
}

void long_stall() {
    reset();
    Clock c;
    assert(c.update(frame_ms) == 1.0f);
    assert(c.update(60000.0f) == 2.0f);
    assert(c.update(frame_ms) == 2.0f);
    float step = 0;
    for (int i = 0; i < 200; ++i) {
        step = c.update(frame_ms);
        assert(step >= 1.0f && step <= 2.0f);
    }
    // Without the upstream 90-frame cap a one-minute stall would still be
    // repaid at the 2.0 maximum hundreds of frames later.
    assert(near(step, 1.0f, 0.0001f));
}

void mode_and_object_reset() {
    reset();
    Clock a, b;
    assert(a.update(frame_ms) == 1.0f);
    assert(a.update(frame_ms * 2) == 1.0f);
    assert(a.update(frame_ms * 2) == 1.125f);
    assert(b.update(60000.0f) == 1.0f);
    assert(a.update(frame_ms * 2) == 1.0f);
    assert(a.update(frame_ms * 2) == 1.0f);
    assert(a.update(frame_ms * 2) == 1.125f);
    a.put(0xe0, 12.5f);
    a.put(0xf0, 42.0f);
    const auto before = a.bytes;
    nx::b3::scene_clock_after(a.address(), false);
    assert(a.bytes == before);
    // Measured mode does not dereference even an unusable clock address.
    nx::b3::scene_clock_after(1, false);
    assert(a.update(frame_ms * 2) == 1.0f);
}

void invalid_samples() {
    reset();
    Clock c;
    assert(c.update(frame_ms) == 1.0f);
    assert(c.update(frame_ms * 2) == 1.0f);
    assert(c.update(frame_ms * 2) == 1.125f);
    for (float ms : {0.0f, -1.0f, std::numeric_limits<float>::infinity(),
                     std::numeric_limits<float>::quiet_NaN()}) {
        assert(c.update(ms, 3.0f) == 3.0f);
        assert(c.update(frame_ms * 2) == 1.0f);
    }
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float after = c.update(frame_ms, nan);
    assert(std::bit_cast<uint32_t>(after) == std::bit_cast<uint32_t>(nan));
    assert(c.update(frame_ms * 2) == 1.0f);

    reset();
    assert(c.update(frame_ms) == 1.0f);
    assert(c.update(frame_ms * 2) == 1.0f);
    const float max = std::numeric_limits<float>::max();
    assert(c.update(frame_ms * 2, max) == max);  // corrected step would overflow
    assert(c.update(frame_ms * 2) == 1.0f);
    nx::b3::scene_clock_after(std::numeric_limits<uint64_t>::max(), true);
    assert(c.update(frame_ms * 2) == 1.0f);
}

void thread_isolation() {
    reset();
    Clock c;
    assert(c.update(frame_ms) == 1.0f);
    assert(c.update(frame_ms * 2) == 1.0f);
    assert(c.update(frame_ms * 2) == 1.125f);
    float other = 0;
    // The worker and caller touch this clock in sequence, not concurrently.
    std::thread t([&] { other = c.update(frame_ms * 2); });
    t.join();
    assert(other == 1.0f);  // another thread starts with no inherited debt
    assert(c.update(frame_ms * 2) > 1.0f);  // caller retained its own debt
}

void randomized_reference() {
    reset();
    Clock c;
    assert(c.update(100000.0f) == 1.0f);
    std::mt19937 rng(49017);
    long double debt = 0;
    for (int i = 0; i < 3000; ++i) {
        // Include sustained fast frames, slow frames, and a capped stall.
        const float ms = i == 2000 ? 60000.0f : i < 1000 ? float(10 + rng() % 71)
                                        : i < 2000 ? float(2 + rng() % 19)
                                                   : float(15 + rng() % 56);
        const float due = ms / frame_ms;
        debt = std::min(90.0L, debt + due - 1.0L);
        const long double excess = debt > 1 ? debt - 1 : debt < -1 ? debt + 1 : 0;
        const long double multiplier = std::min(2.0L, 1.0L + excess / 8.0L);
        debt += 1.0L - multiplier;
        assert(std::fabs(c.update(ms) - double(multiplier)) < 0.0001);
    }
}

void measured_step_rules() {
    const float per = 1000.0f / 60.0f;
    assert(near(nx::b3::measured_step(per / 2, per, 0.0f), 0.5f));        // 120 Hz: half a 60 Hz frame
    assert(near(nx::b3::measured_step(per * 1.5f, per, 0.0f), 1.5f));     // 40 Hz: as the game computes it
    assert(near(nx::b3::measured_step(0.1f, per, 0.0f), 1.0f / 16.0f));   // floor
    assert(near(nx::b3::measured_step(per * 5, per, 0.0f), 2.0f));        // the game's own ceiling
    assert(nx::b3::measured_step(per / 2, per, 0.25f) < 0.0f);            // the game's override wins
    assert(nx::b3::measured_step(per / 2, 0.0f, 0.0f) < 0.0f);
    assert(nx::b3::measured_step(std::numeric_limits<float>::quiet_NaN(), per, 0.0f) < 0.0f);
    assert(nx::b3::measured_step(0.0f, per, 0.0f) < 0.0f);
    // At 60 Hz (and with no display rate reported) a measured clock keeps the game's own step and carry.
    for (int hz : {0, 60}) {
        nx::b3::scene_clock_set_display_hz(hz);
        Clock c;
        c.put(0xe0, 1.0f);
        c.put(0xe4, 0.5f);
        c.put(0xec, per);
        c.put(0xf0, per / 2);
        nx::b3::scene_clock_after(c.address(), false);
        assert(c.get(0xe0) == 1.0f && c.get(0xe4) == 0.5f);
    }
}

// A 15-frame countdown visited once per clock update: how many updates until it reaches 0.
int window_updates(float step, bool enabled, int start = 15) {
    nx::b3::fps_counters_reset();
    uint32_t v = 0;
    int updates = 0;
    nx::b3::fps_counters_publish(step, enabled);
    v = nx::b3::fps_ci(1, 2, v, 1, -1) == 1 ? 0 : 0;  // a visit at 0 before the window opens
    v = uint32_t(start);
    while (int32_t(v) > 0 && updates < 1000) {
        nx::b3::fps_counters_publish(step, enabled);
        ++updates;
        v -= nx::b3::fps_ci(1, 2, v, 1, -1);
    }
    return updates;
}

void fps_counter_rules() {
    assert(window_updates(1.0f, true) == 15);   // 60 Hz
    const int at120 = window_updates(0.5f, true);  // 120 Hz: the same 0.25 s, give or take the half frame of phase
    assert(at120 >= 29 && at120 <= 30);
    assert(window_updates(0.5f, false) == 15);  // off: the stock 1 a visit
    const int at90 = window_updates(2.0f / 3.0f, true);
    assert(at90 >= 22 && at90 <= 23);
    // Below 60 (2 frames a visit) the window still lasts its frames, and the last visit stops at 0.
    assert(window_updates(2.0f, true) == 8);
    // A countdown waiting at 0 owes nothing: the next window starts whole.
    nx::b3::fps_counters_reset();
    for (int i = 0; i < 10; ++i) {
        nx::b3::fps_counters_publish(2.0f, true);
        assert(nx::b3::fps_ci(7, 8, 0, 1, -1) == 1);
    }
    nx::b3::fps_counters_publish(2.0f, true);
    assert(nx::b3::fps_ci(7, 8, 15, 1, -1) == 2);
    // An up-counter never moves more than 1 a visit; a second visit in one update moves as the first.
    nx::b3::fps_counters_reset();
    nx::b3::fps_counters_publish(2.0f, true);
    nx::b3::fps_ci(9, 10, 0, 1, 1);
    nx::b3::fps_counters_publish(2.0f, true);
    assert(nx::b3::fps_ci(9, 10, 1, 1, 1) == 1);
    nx::b3::fps_counters_reset();
    nx::b3::fps_counters_publish(0.5f, true);
    nx::b3::fps_ci(3, 4, 9, 1, -1);
    nx::b3::fps_counters_publish(0.5f, true);
    nx::b3::fps_counters_publish(0.5f, true);
    const uint32_t first = nx::b3::fps_ci(3, 4, 9, 1, -1);
    assert(nx::b3::fps_ci(3, 4, 9, 1, -1) == first);
    // Floats: a per-frame amount scales by the step; a -1.0 countdown by whole frames.
    nx::b3::fps_counters_reset();
    nx::b3::fps_counters_publish(0.5f, true);
    assert(nx::b3::fps_fs(5, 6) == 1.0f);  // first visit: as made
    nx::b3::fps_counters_publish(0.5f, true);
    assert(nx::b3::fps_fs(5, 6) == 0.5f);
    nx::b3::fps_counters_reset();
    assert(nx::b3::fps_fs(5, 6) == 1.0f && nx::b3::fps_cf(5, 7, 3.0f, -1) == 1 && nx::b3::fps_ci(5, 8, 3, 1, -1) == 1);
}

void disabled() {
    Clock c;
    for (int i = 0; i < 100; ++i) assert(c.update(frame_ms * 2, 3.0f) == 3.0f);
    // Disabled mode must avoid dereferencing the purported clock entirely.
    nx::b3::scene_clock_after(1, true);
}

void enabled() {
    Clock c;
    assert(c.update(frame_ms) == 1.0f);
    assert(c.update(frame_ms * 2) == 1.0f);
    assert(c.update(frame_ms * 2) == 1.125f);
}
}  // namespace

int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string_view mode(argv[1]);
    if (mode == "disabled") disabled();
    else if (mode == "enabled") enabled();
    else {
        assert(mode == "normal");
        measured_step_rules();
        fps_counter_rules();
        stable_and_repayment();
        low_rate_recovery();
        long_stall();
        mode_and_object_reset();
        invalid_samples();
        thread_isolation();
        randomized_reference();
    }
}
