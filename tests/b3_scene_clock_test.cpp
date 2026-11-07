// SPDX-License-Identifier: GPL-3.0-or-later
// Exercises the production post-original Witch Time scene-clock adaptation.
#include "nx/b3_scene_clock.h"

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
        stable_and_repayment();
        low_rate_recovery();
        long_stall();
        mode_and_object_reset();
        invalid_samples();
        thread_isolation();
        randomized_reference();
    }
}
