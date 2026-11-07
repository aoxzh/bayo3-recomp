#pragma once

#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>

namespace nx::movie_timing {

using Clock = std::chrono::steady_clock;

// Round upward to the first representable clock tick at which this frame is
// due. A truncated fractional deadline can otherwise make wait_until spin.
inline Clock::time_point deadline(Clock::time_point origin, uint64_t index, double fps) {
    if (!(fps > 0.0) || !std::isfinite(fps)) return Clock::time_point::max();
    using Duration = Clock::duration;
    using Rep = Duration::rep;
    const long double ticks = std::ceil((static_cast<long double>(index) / static_cast<long double>(fps)) *
                                        Duration::period::den / Duration::period::num);
    if (!std::isfinite(ticks) || ticks > static_cast<long double>(std::numeric_limits<Rep>::max()))
        return Clock::time_point::max();
    const Duration delta(static_cast<Rep>(ticks));
    if (origin > Clock::time_point::max() - delta) return Clock::time_point::max();
    return origin + delta;
}

inline uint64_t target(Clock::time_point origin, Clock::time_point now, double fps) {
    if (now <= origin || !(fps > 0.0) || !std::isfinite(fps)) return 0;
    const long double frames = std::chrono::duration<long double>(now - origin).count() * fps;
    constexpr uint64_t max = std::numeric_limits<uint64_t>::max();
    if (frames >= static_cast<long double>(max)) return max;
    uint64_t index = static_cast<uint64_t>(frames);
    // Make the target and the rounded deadline agree even at fractional-fps
    // boundaries where floating-point multiplication rounds in either direction.
    if (index && deadline(origin, index, fps) > now) --index;
    if (index < max && deadline(origin, index + 1, fps) <= now) ++index;
    return index;
}

// Called with the session mutex held. Keep the newest due frame as well as
// every future frame: a late producer may still be filling the gap to target.
template <class Frames>
inline bool prune_due(Frames& ring, uint64_t target_index) {
    bool changed = false;
    while (ring.size() >= 2 && ring[1].index <= target_index) {
        ring.pop_front();
        changed = true;
    }
    return changed;
}

// Source timestamps can start away from zero and have variable frame spacing.
// Retain the newest frame at or before the guest target; never discard it just
// because decoding is still catching up, and never promote a future frame.
template <class Frames>
inline bool prune_pts(Frames& ring, int64_t target_pts_us) {
    bool changed = false;
    while (ring.size() >= 2 && ring[1].pts_us != std::numeric_limits<int64_t>::min() &&
           ring[1].pts_us <= target_pts_us) {
        ring.pop_front();
        changed = true;
    }
    return changed;
}

}  // namespace nx::movie_timing
