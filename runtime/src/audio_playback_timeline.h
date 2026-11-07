// FIFO accounting of SDL source bytes after a successful stream submission.
// Used only by the serialized SDL audio callback, never by guest threads.
#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <memory>

#include "audio_playback.h"

namespace nx::plat::detail {

class AudioPlaybackTimeline {
public:
    static constexpr size_t kMaxSpans = 4096;

    // A null owner represents silence or audio from an untracked caller.
    void append(const std::shared_ptr<AudioPlayback>& owner, uint64_t bytes) {
        if (!bytes || lost_) return;
        const bool extends = !spans_.empty() && spans_.back().owner == owner;
        if (bytes > std::numeric_limits<uint64_t>::max() - queued_ ||
            (!extends && spans_.size() == kMaxSpans)) {
            // If queue queries never recover, exact ownership is unbounded.
            // Freeze instead of guessing, and keep audio playing normally.
            spans_.clear();
            partial_.clear();
            queued_ = 0;
            lost_ = true;
            return;
        }
        if (extends)
            spans_.back().bytes += bytes;
        else
            spans_.push_back({owner, bytes});
        queued_ += bytes;
    }

    // SDL_GetAudioStreamQueued is input bytes not yet consumed. Do not also
    // subtract SDL_GetAudioStreamAvailable: those output bytes overlap them.
    void observe(int queued) {
        // SDL clamps larger byte queues to INT_MAX; that value is not exact.
        if (lost_ || queued < 0 || queued == std::numeric_limits<int>::max() || uint64_t(queued) > queued_) return;
        uint64_t consumed = queued_ - uint64_t(queued);
        queued_ = uint64_t(queued);
        while (consumed) {
            Span& s = spans_.front();
            const uint64_t take = std::min(consumed, s.bytes);
            if (s.owner) advance(s.owner, take);
            s.bytes -= take;
            consumed -= take;
            if (!s.bytes) spans_.pop_front();
        }
        // A partial frame needs a remainder even between submissions. Weak
        // owners allow closed outputs to disappear once their spans retire.
        for (auto it = partial_.begin(); it != partial_.end();) {
            if (it->first.expired())
                it = partial_.erase(it);
            else
                ++it;
        }
    }

    size_t span_count() const { return spans_.size(); }
    uint64_t queued_bytes() const { return queued_; }
    bool lost() const { return lost_; }

private:
    struct Span {
        std::shared_ptr<AudioPlayback> owner;
        uint64_t bytes;
    };

    void advance(const std::shared_ptr<AudioPlayback>& owner, uint64_t bytes) {
        const std::weak_ptr<AudioPlayback> key(owner);
        auto it = partial_.find(key);
        const uint64_t tail = (it == partial_.end() ? 0u : it->second) + bytes % 4;
        const uint64_t frames = bytes / 4 + tail / 4;
        const auto remainder = uint8_t(tail % 4);
        if (remainder)
            partial_[key] = remainder;
        else if (it != partial_.end())
            partial_.erase(it);
        if (frames) {
            // An HLE timer can also publish to this cursor. Saturating CAS keeps
            // the value monotonic even at the uint64_t limit.
            uint64_t old = owner->frames.load(std::memory_order_relaxed);
            for (;;) {
                const uint64_t max = std::numeric_limits<uint64_t>::max();
                const uint64_t next = frames > max - old ? max : old + frames;
                if (owner->frames.compare_exchange_weak(old, next, std::memory_order_relaxed)) break;
            }
        }
    }

    std::deque<Span> spans_;
    std::map<std::weak_ptr<AudioPlayback>, uint8_t, std::owner_less<std::weak_ptr<AudioPlayback>>> partial_;
    uint64_t queued_ = 0;
    bool lost_ = false;
};

}  // namespace nx::plat::detail
