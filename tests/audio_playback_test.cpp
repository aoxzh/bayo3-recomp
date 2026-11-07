// Model tests for SDL source-byte playback accounting (no audio device).
#include "../runtime/src/audio_playback_timeline.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <limits>
#include <memory>

using nx::plat::AudioPlayback;
using nx::plat::detail::AudioPlaybackTimeline;

static uint64_t frames(const std::shared_ptr<AudioPlayback>& p) {
    return p->frames.load(std::memory_order_relaxed);
}

static void basic() {
    AudioPlaybackTimeline t;
    auto a = std::make_shared<AudioPlayback>();
    auto b = std::make_shared<AudioPlayback>();
    t.append(a, 3);
    t.append(b, 4);
    t.append({}, 5);
    t.append(a, 5);
    assert(t.queued_bytes() == 17 && t.span_count() == 4);
    t.observe(-1);
    t.observe(18);
    assert(frames(a) == 0 && frames(b) == 0 && t.queued_bytes() == 17);
    t.observe(16); // 1 byte of A
    t.observe(14); // next 2 bytes of A: still an incomplete frame
    assert(frames(a) == 0 && frames(b) == 0);
    t.observe(10); // all of B
    assert(frames(a) == 0 && frames(b) == 1);
    t.observe(5); // untracked silence cannot advance either output
    assert(frames(a) == 0 && frames(b) == 1);
    t.observe(0); // A's old 3 bytes combine with these 5
    assert(frames(a) == 2 && frames(b) == 1);
    assert(t.queued_bytes() == 0 && t.span_count() == 0);

    // A consumed partial byte does not retain a closed output forever.
    std::weak_ptr<AudioPlayback> weak;
    {
        auto old = std::make_shared<AudioPlayback>();
        weak = old;
        t.append(old, 1);
        old.reset();
        assert(!weak.expired());
        t.observe(0);
        assert(weak.expired());
    }
    auto reopened = std::make_shared<AudioPlayback>();
    t.append(reopened, 4);
    t.observe(0);
    assert(frames(reopened) == 1);

    auto saturated = std::make_shared<AudioPlayback>();
    saturated->frames.store(std::numeric_limits<uint64_t>::max() - 1);
    t.append(saturated, 12);
    t.observe(0);
    assert(frames(saturated) == std::numeric_limits<uint64_t>::max());

    // SDL clamps large queued values to INT_MAX. One span models a queue
    // larger than that without allocating gigabytes of sample data.
    auto large = std::make_shared<AudioPlayback>();
    const uint64_t max_queue = uint64_t(std::numeric_limits<int>::max());
    t.append(large, max_queue + 128);
    t.observe(std::numeric_limits<int>::max());
    assert(frames(large) == 0 && t.queued_bytes() == max_queue + 128);
    t.observe(128);
    assert(frames(large) == max_queue / 4);
    t.observe(0);
    assert(frames(large) == (max_queue + 128) / 4);
}

static void bounded_failure() {
    AudioPlaybackTimeline t;
    auto a = std::make_shared<AudioPlayback>();
    auto b = std::make_shared<AudioPlayback>();
    for (size_t i = 0; i < AudioPlaybackTimeline::kMaxSpans; ++i)
        t.append(i % 2 ? a : b, 1);
    assert(!t.lost() && t.span_count() == AudioPlaybackTimeline::kMaxSpans);
    t.observe(-1);
    t.append(b, 1); // alternate ownership over the limit
    assert(t.lost() && t.span_count() == 0 && t.queued_bytes() == 0);
    t.observe(0);
    t.append(a, 32);
    t.observe(0);
    assert(frames(a) == 0 && frames(b) == 0);
    t = AudioPlaybackTimeline{}; // a new SDL stream
    t.append(a, 4);
    t.observe(0);
    assert(!t.lost() && frames(a) == 1);
}

static uint32_t rng = 0x19ad345bu;
static uint32_t random32() {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static void randomized() {
    AudioPlaybackTimeline t;
    std::shared_ptr<AudioPlayback> owners[] = {std::make_shared<AudioPlayback>(),
                                               std::make_shared<AudioPlayback>(),
                                               std::make_shared<AudioPlayback>()};
    // Independent byte-by-byte oracle: 0..2 owners, -1 untracked.
    std::deque<int> journal;
    uint64_t actual_queued = 0;
    uint64_t consumed[3]{};
    for (unsigned step = 0; step < 25000; ++step) {
        int owner = int(random32() % 4) - 1;
        unsigned n = random32() % 24;
        t.append(owner < 0 ? std::shared_ptr<AudioPlayback>{} : owners[owner], n);
        for (unsigned j = 0; j < n; ++j) journal.push_back(owner);
        actual_queued += n;
        if (actual_queued) actual_queued -= random32() % (actual_queued + 1);
        const unsigned event = random32() % 5;
        if (event == 0) {
            t.observe(-1);
        } else if (event == 1) {
            t.observe(int(journal.size() + 1));
        } else {
            t.observe(int(actual_queued));
            while (journal.size() > actual_queued) {
                int who = journal.front();
                journal.pop_front();
                if (who >= 0) ++consumed[who];
            }
        }
        assert(!t.lost() && t.queued_bytes() == journal.size());
        for (unsigned i = 0; i < 3; ++i) assert(frames(owners[i]) == consumed[i] / 4);
    }
    t.observe(0);
    while (!journal.empty()) {
        int who = journal.front();
        journal.pop_front();
        if (who >= 0) ++consumed[who];
    }
    for (unsigned i = 0; i < 3; ++i) assert(frames(owners[i]) == consumed[i] / 4);
    assert(t.span_count() == 0 && t.queued_bytes() == 0);
}

int main() {
    basic();
    bounded_failure();
    randomized();
    std::puts("audio playback timeline: deterministic, overflow, and randomized tests passed");
}
