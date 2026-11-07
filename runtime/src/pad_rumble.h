#pragma once

#include <atomic>
#include <cstdint>

namespace nx::plat {

// Strengths occupy the low 32 bits; the high 32 bits are a device generation.
// The main thread advances the generation and clears both strengths whenever a
// physical controller closes or opens. HLE publishers use that generation in a
// CAS, so a send racing a controller change cannot revive the old snapshot.
inline std::atomic<uint64_t> g_pad_rumble{0};

inline uint32_t read_pad_rumble() {
    return uint32_t(g_pad_rumble.load(std::memory_order_relaxed));
}

inline uint32_t pad_rumble_generation() {
    return uint32_t(g_pad_rumble.load(std::memory_order_relaxed) >> 32);
}

inline bool try_publish_pad_rumble(uint32_t generation, uint16_t left, uint16_t right) {
    uint64_t current = g_pad_rumble.load(std::memory_order_relaxed);
    const uint64_t desired = (uint64_t(generation) << 32) | uint32_t(left) | (uint32_t(right) << 16);
    while (uint32_t(current >> 32) == generation) {
        if (g_pad_rumble.compare_exchange_weak(current, desired, std::memory_order_relaxed)) return true;
    }
    return false;
}

inline void reset_pad_rumble() {
    uint64_t current = g_pad_rumble.load(std::memory_order_relaxed);
    uint64_t desired;
    do {
        const uint32_t next = uint32_t(current >> 32) + 1u;
        desired = uint64_t(next) << 32;
    } while (!g_pad_rumble.compare_exchange_weak(current, desired, std::memory_order_relaxed));
}

}  // namespace nx::plat
