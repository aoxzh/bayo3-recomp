// Per-present draw accounting for NX_DRAW_FRAME_STATS. The replay side is serialized by the GPU lock.
#pragma once

#include <array>
#include <cstdint>
#include <cstdlib>
#include <unordered_set>

namespace nx::nvn::draw_frame_stats {

// Early summaries occur at 60, 120, 180, 240, and 300 presents; the 300th
// closes another 60-present interval, followed by 300-present intervals.
inline uint64_t log_window(uint64_t presents) { return presents <= 300 ? 60 : 300; }

inline bool enabled() {
    static const bool value = [] {
        const char* e = std::getenv("NX_DRAW_FRAME_STATS");
        return e && *e && !(e[0] == '0' && e[1] == '\0');
    }();
    return value;
}

enum class Kind : unsigned { Arrays, Elements, Indirect, MultiIndirect, Count };

struct Sample {
    uint64_t sequence = 0;
    uint64_t recorded = 0, replayed = 0, emitted = 0;
    std::array<uint64_t, unsigned(Kind::Count)> kinds{};
    uint64_t nested_draws = 0, repeated_branch_draws = 0;
    uint64_t calls = 0, repeated_calls = 0;
    bool first = false, ordered = true;
};

class Tracker {
public:
    // A CallList handle seen before this replay-present boundary is a repeat. The flag is
    // inherited by descendants; it doesn't imply those draws are redundant or can be skipped.
    void call(uint64_t handle, unsigned parent_depth) {
        if (parent_depth >= kMaxDepth) return;
        const bool repeated = !called_.insert(handle).second;
        ++calls_;
        repeated_calls_ += repeated;
        repeated_at_depth_[parent_depth + 1] = repeated_at_depth_[parent_depth] || repeated;
    }

    void draw(Kind kind, unsigned depth) {
        const unsigned index = unsigned(kind);
        if (index >= unsigned(Kind::Count)) return;
        ++kinds_[index];
        if (depth) ++nested_draws_;
        if (depth <= kMaxDepth && repeated_at_depth_[depth]) ++repeated_branch_draws_;
    }

    Sample present(uint64_t recorded, uint64_t replayed, uint64_t emitted) {
        Sample result;
        result.sequence = ++sequence_;
        result.first = sequence_ == 1;
        result.ordered = recorded >= previous_recorded_ && replayed >= previous_replayed_ && emitted >= previous_emitted_;
        // Recording snapshots can arrive out of order if different guest threads enqueue presents.
        // Keep each high-water mark independently: a late guest sample must not discard valid
        // replay counts or count the same recorded draws again at the next present.
        const auto delta = [](uint64_t now, uint64_t& previous) {
            if (now < previous) return uint64_t{0};
            const uint64_t value = now - previous;
            previous = now;
            return value;
        };
        result.recorded = delta(recorded, previous_recorded_);
        result.replayed = delta(replayed, previous_replayed_);
        result.emitted = delta(emitted, previous_emitted_);
        result.kinds = kinds_;
        result.nested_draws = nested_draws_;
        result.repeated_branch_draws = repeated_branch_draws_;
        result.calls = calls_;
        result.repeated_calls = repeated_calls_;
        kinds_ = {};
        nested_draws_ = repeated_branch_draws_ = calls_ = repeated_calls_ = 0;
        repeated_at_depth_ = {};
        called_.clear();
        return result;
    }

private:
    static constexpr unsigned kMaxDepth = 8;
    uint64_t sequence_ = 0;
    uint64_t previous_recorded_ = 0, previous_replayed_ = 0, previous_emitted_ = 0;
    std::array<uint64_t, unsigned(Kind::Count)> kinds_{};
    uint64_t nested_draws_ = 0, repeated_branch_draws_ = 0, calls_ = 0, repeated_calls_ = 0;
    std::array<bool, kMaxDepth + 1> repeated_at_depth_{};
    std::unordered_set<uint64_t> called_;
};

inline Tracker& tracker() { static Tracker value; return value; }
// Carries the recording-side sample with the queued present, including synchronous worker mode.
inline uint64_t& guest_sample() { static thread_local uint64_t value = 0; return value; }

}  // namespace nx::nvn::draw_frame_stats
