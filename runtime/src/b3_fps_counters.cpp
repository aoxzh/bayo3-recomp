// SPDX-License-Identifier: GPL-3.0-or-later
// Frame counters above 60 Hz (nx/b3_fps_counters.h). The visit rules follow Witch Time runtime/game_shim_eh.c
// fps_ci / fps_cf / fps_fs at 7e7af36250b6845eb8a5ebd20744a9858c8e9d28; see docs/WITCH-TIME-NOTICES.md.
#include "nx/b3_fps_counters.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "nx/runtime.h"  // log

namespace nx::b3 {
namespace {

constexpr int kMaxDuePerUpdate = 4;  // frames one update may make due (a frame of 4 x 16.7 ms or longer)
constexpr int64_t kMaxCarry = 8;     // what a limit holds back, owed to the next visits
constexpr size_t kRing = 256;
constexpr size_t kSlots = 4096;
constexpr int kProbe = 8;

struct Counter {
    uint64_t key = 0, field = 0;
    uint64_t update = ~0ull;  // the clock update of the last visit
    uint64_t due_at = 0;      // the clock's frames due at that visit
    int64_t now = 0, carry = 0;
    float factor = 1.0f;
};

struct State {
    std::atomic<uint64_t> updates{0};  // clock updates published so far
    std::array<uint64_t, kRing> due{};  // frames due after update n, at [n % kRing]
    std::array<float, kRing> step{};    // the step of update n
    double acc = 0.0;                   // fractional frames not yet due (clock thread only)
    uint64_t total = 0;
    std::atomic<bool> enabled{false};
    std::atomic_flag lock = ATOMIC_FLAG_INIT;
    std::array<Counter, kSlots> slots{};
    // NX_FPS_COUNTERS_PROBE=<n>: every n updates, how many visits there were and how far they moved in all.
    uint64_t visits = 0;
    double moved = 0.0;
};

State& state() {
    static State s;
    return s;
}

bool allowed() {
    static const bool on = [] {
        const char* e = std::getenv("NX_FPS_COUNTERS");
        return !(e && std::strcmp(e, "0") == 0);
    }();
    return on;
}

long probe_every() {
    static const long n = [] {
        const char* e = std::getenv("NX_FPS_COUNTERS_PROBE");
        long v = e ? std::atol(e) : 0;
        return v > 0 ? v : 0;
    }();
    return n;
}

struct Guard {
    State& s;
    explicit Guard(State& st) : s(st) {
        while (s.lock.test_and_set(std::memory_order_acquire)) {
        }
    }
    ~Guard() { s.lock.clear(std::memory_order_release); }
};

uint64_t mix(uint64_t a) {
    a ^= a >> 33;
    a *= 0xff51afd7ed558ccdull;
    a ^= a >> 33;
    return a;
}

// The counter's record (lock held): its slot, or the least recently visited of its 8 probes.
Counter& find(State& s, uint64_t key, uint64_t field) {
    const uint64_t h = mix(key * 0x9E3779B97F4A7C15ull ^ field);
    Counter* victim = nullptr;
    for (size_t i = 0; i < 8; ++i) {
        Counter& r = s.slots[(h + i) % kSlots];
        if (r.key == key && r.field == field) return r;
        if (!r.key) {
            r = Counter{key, field};
            return r;
        }
        if (!victim || r.update + 1 < victim->update + 1) victim = &r;  // ~0 (never) sorts first
    }
    *victim = Counter{key, field};
    return *victim;
}

// Frames due for this visit (lock held). A counter that also ran in the previous update moves by the frames that
// came due since; a second visit in one update moves as the first did; any other visit (the first, a one-off
// event) moves 1, as the game made it.
int64_t visit(State& s, Counter& r, uint64_t n) {
    const uint64_t due = s.due[n % kRing];
    if (r.update != n) {
        int64_t de;
        if (r.update == n - 1) {
            de = int64_t(due) - int64_t(r.due_at);
            de = de < 0 ? 0 : de > kMaxDuePerUpdate ? kMaxDuePerUpdate : de;
            de += r.carry;
        } else {
            de = 1;
        }
        r.carry = 0;
        if (de > kMaxCarry) r.carry = de - kMaxCarry, de = kMaxCarry;
        r.now = de;
    }
    r.due_at = due;
    r.update = n;
    return r.now;
}

void limit(Counter& r, int64_t& de, int64_t keep) {
    if (de > keep) {
        r.carry += de - keep;
        if (r.carry > kMaxCarry) r.carry = kMaxCarry;
        de = keep;
    }
}

void count(State& s, double moved) {
    if (!probe_every()) return;
    ++s.visits;
    s.moved += moved;
}

// NX_FPS_COUNTERS_WATCH=<hex store site>: each time that countdown is set and runs down to 0, log how long it took
// in wall time and in visits (a 22-frame window should take 367 ms at any frame rate). Runs with the counters off
// too, so stock and converted runs can be compared.
void watch(uint64_t key, uint32_t before, uint32_t amount) {
    static const uint64_t site = [] {
        const char* e = std::getenv("NX_FPS_COUNTERS_WATCH");
        return e ? std::strtoull(e, nullptr, 16) : 0ull;
    }();
    if (!site || key != site) return;
    static int64_t last = 0, top = 0, visits = 0;
    static std::chrono::steady_clock::time_point start;
    const int64_t w = int32_t(before), after = w - int64_t(amount);
    if (w > last + 1) {  // set (again) since the last visit left it at `last`
        top = w, visits = 0;
        start = std::chrono::steady_clock::now();
    }
    if (top) {
        ++visits;
        if (after <= 0) {
            const double ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            log("fps counters: watched %llx ran %lld frames down in %.1f ms over %lld visits (60 Hz: %.1f ms)",
                static_cast<unsigned long long>(key), static_cast<long long>(top), ms, static_cast<long long>(visits),
                double(top) * 1000.0 / 60.0);
            top = 0;
        }
    }
    last = after;
}

bool on(State& s, uint64_t& n) {
    n = s.updates.load(std::memory_order_acquire);
    return n && s.enabled.load(std::memory_order_relaxed) && allowed();
}

}  // namespace

uint32_t fps_ci(uint64_t key, uint64_t field, uint32_t before, int k, int sign) {
    State& s = state();
    uint64_t n;
    if (!on(s, n)) {
        if (sign < 0) watch(key, before, uint32_t(k));
        return uint32_t(k);
    }
    Guard g(s);
    Counter& r = find(s, key, field);
    int64_t de = visit(s, r, n);
    const int64_t w = int32_t(before);
    // A countdown at 0 takes the stock step and owes nothing: steps that piled up there below 60 fps would cut the
    // next window short.
    if (sign < 0 && w == 0) {
        r.carry = 0;
        count(s, 1.0);
        watch(key, before, uint32_t(k));
        return uint32_t(k);
    }
    // A countdown never passes 0 in one visit (a test of the result for 0 still fires); an up-counter moves at most
    // 1 a visit (a test for an exact value is not passed over).
    limit(r, de, sign < 0 ? (w > k ? 1 + (w - k) / k : 1) : 1);
    count(s, double(de));
    if (sign < 0) watch(key, before, uint32_t(int64_t(k) * de));
    return uint32_t(int64_t(k) * de);
}

int fps_cf(uint64_t key, uint64_t field, float before, int sign) {
    State& s = state();
    uint64_t n;
    if (!on(s, n)) return 1;
    Guard g(s);
    Counter& r = find(s, key, field);
    int64_t de = visit(s, r, n);
    limit(r, de, sign < 0 && before >= 2.0f ? 1 + int64_t(before - 1.0f) : 1);
    count(s, double(de));
    return int(de);
}

float fps_fs(uint64_t key, uint64_t field) {
    State& s = state();
    uint64_t n;
    if (!on(s, n)) return 1.0f;
    Guard g(s);
    Counter& r = find(s, key, field);
    const float f = r.update == n ? r.factor : r.update == n - 1 ? s.step[n % kRing] : 1.0f;
    r.factor = f;
    r.update = n;
    r.due_at = s.due[n % kRing];
    count(s, double(f));
    return f;
}

void fps_counters_publish(float step, bool enabled) {
    State& s = state();
    const bool usable = std::isfinite(step) && step > 0.0f;
    s.acc += usable ? double(step) : 0.0;
    int t = int(s.acc + 1e-3);
    if (t > kMaxDuePerUpdate) t = kMaxDuePerUpdate, s.acc = kMaxDuePerUpdate;
    s.acc -= t;
    if (s.acc < 0.0) s.acc = 0.0;
    s.total += uint64_t(t);
    const uint64_t n = s.updates.load(std::memory_order_relaxed) + 1;
    {
        Guard g(s);  // a visit reads update n's slots after it reads n
        s.due[n % kRing] = s.total;
        s.step[n % kRing] = usable ? step : 1.0f;
        s.enabled.store(enabled, std::memory_order_relaxed);
        if (const long every = probe_every(); every && n % uint64_t(every) == 0) {
            log("fps counters: %s, update %llu, %llu visits moved %.3f frames each on average", enabled ? "on" : "off",
                static_cast<unsigned long long>(n), static_cast<unsigned long long>(s.visits),
                s.visits ? s.moved / double(s.visits) : 0.0);
            s.visits = 0;
            s.moved = 0.0;
        }
    }
    s.updates.store(n, std::memory_order_release);
}

void fps_counters_reset() {
    State& s = state();
    Guard g(s);
    s.updates.store(0, std::memory_order_relaxed);
    s.due.fill(0);
    s.step.fill(0.0f);
    s.acc = 0.0;
    s.total = 0;
    s.enabled.store(false, std::memory_order_relaxed);
    s.slots.fill(Counter{});
    s.visits = 0;
    s.moved = 0.0;
}

}  // namespace nx::b3
