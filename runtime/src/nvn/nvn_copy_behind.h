// Guest bytes copied into the upload ring behind the replay thread (NX_COPY_BEHIND).
//
// A fight moves about 90 MB a frame through memcpy (67 MB guest->ring, 25 MB write-back snapshots), and that was the
// largest single cost of the replay thread, which is the frame's bottleneck: the ring allocation is what recording
// needs, not the bytes. The replay thread takes the allocation, records the command with its offset and queues the
// copy; the bytes only have to be there when the command buffer is queued. So:
//
//   - no recorded work reaches vkQueueSubmit before the copies queued ahead of it have finished: a part ended early
//     is parked until reached() (nvn_frames.cpp), and the submit's last part and every ring reuse are behind wait();
//   - a replay-thread write to guest memory first waits for the queued reads of those bytes (wait_for), so a copy
//     still gets the bytes it would have got had it run inline;
//   - anything that reads a destination on the CPU waits first (dedup's old-size compare, a merge of snapshots);
//   - only guest memory is queued: host scratch data (the stack blocks of nvn_sets.cpp) is gone by the time a helper
//     gets to it, so those callers stay inline;
//   - an ASTC decode (Lane, below) owns its input, so the same is true of it.
//
// Guest threads run concurrently with the replay already, so a copy made a few hundred microseconds later reads what
// the GPU itself would have read later on the real hardware; the rule the replay relies on (bytes of a submit do not
// change until the guest has seen that submit finish) is unchanged, because the fence it would wait on comes after
// wait().
//
// NX_COPY_BEHIND=<helpers> (default 2, 0 copies inline as before). NX_COPY_BEHIND_MIN=<bytes> (default 4096): below
// this a queue slot costs more than the copy. Producers are serialized by gpu_lock, helpers claim slots: a bounded
// Vyukov queue, whose per-slot sequence is what lets a slot be reused only once its own job has finished.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif
#if defined(__linux__)
#include <pthread.h>
#endif

namespace nx::nvn::replay::copy_behind {

inline void relax() {
#if defined(__x86_64__) || defined(_M_X64)
    _mm_pause();
#elif defined(__aarch64__)
    __asm__ volatile("yield");
#endif
}

class Queue {
public:
    static constexpr uint32_t kSlots = 1024;  // a power of two; wait_for scans all of them when work is pending
    static constexpr uint64_t kSlice = 1ull << 20;  // two helpers share a 16 MB range, and glibc's copy stays cached
    static constexpr uint32_t kSpinRounds = 4096;   // a little over 100 us: the gaps between a draw's jobs

    Queue() {
        for (uint32_t k = 0; k < kSlots; k++) slots_[k].sequence.store(k, std::memory_order_relaxed);
    }

    // to[0..bytes) = from[0..bytes), to[bytes..bytes+zeros) = 0, then snapshot[0..bytes) = to[0..bytes) when asked: the
    // snapshot comes from the copy, because a word the guest changes between the two reads would otherwise look like a
    // GPU write at write-back.
    void push(uint8_t* to, const uint8_t* from, uint64_t bytes, uint64_t zeros, uint8_t* snapshot, unsigned helpers) {
        start(helpers);
        bytes_.fetch_add(bytes, std::memory_order_relaxed);
        for (uint64_t at = 0;;) {
            const uint64_t n = std::min(bytes - at, kSlice);
            const bool last = at + n == bytes;
            put(to + at, from + at, n, last ? zeros : 0, snapshot ? snapshot + at : nullptr);
            at += n;
            if (last) return;
        }
    }

    // The queue is empty when this returns, for the thread that pushes (or holds what serializes those who do). The
    // waiter drains instead of spinning: it is the one being held up, and it has a core.
    void wait() {
        const uint64_t target = tail_.load(std::memory_order_relaxed);
        if (done_.load(std::memory_order_acquire) >= target) return;
        const auto start = std::chrono::steady_clock::now();
        while (run_one()) {}
        while (done_.load(std::memory_order_acquire) < target) relax();
        wait_ns_.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::steady_clock::now() - start).count()),
                           std::memory_order_relaxed);
        waits_.fetch_add(1, std::memory_order_relaxed);
    }

    // Any thread: returns once the jobs below `ticket` have finished, working through the queue meanwhile. Unlike
    // reached() it keeps no state, so it can run while the producer goes on queueing. A slot reads pos + 1 only while its
    // job is published and unfinished; a reused slot has moved on by at least kSlots.
    void help_through(uint64_t ticket) {
        for (uint64_t pos = ticket > kSlots ? ticket - kSlots : 0; pos < ticket; pos++)
            while (slots_[pos & (kSlots - 1)].sequence.load(std::memory_order_acquire) == pos + 1)
                if (!run_one()) relax();
    }

    // The jobs queued so far. reached(ticket): all of them have finished, whatever was queued after.
    uint64_t ticket() const { return tail_.load(std::memory_order_relaxed); }
    bool reached(uint64_t ticket) { return advance(ticket) >= ticket; }

    // Before the caller changes guest bytes [p, p + size): a queued copy of them gets the old bytes.
    void wait_for(const uint8_t* p, uint64_t size) {
        const uint64_t tail = tail_.load(std::memory_order_relaxed);
        if (done_.load(std::memory_order_acquire) >= tail) return;
        const auto lo = reinterpret_cast<uintptr_t>(p), hi = lo + size;
        for (uint64_t pos = advance(tail); pos < tail; pos++) {
            const Slot& s = slots_[pos & (kSlots - 1)];
            if (s.sequence.load(std::memory_order_acquire) != pos + 1) continue;  // finished
            const auto from = reinterpret_cast<uintptr_t>(s.from);  // only producers write these
            if (from < hi && lo < from + s.bytes) return wait();
        }
    }

    uint64_t take_bytes() { return bytes_.exchange(0, std::memory_order_relaxed); }
    uint64_t take_jobs() { return jobs_.exchange(0, std::memory_order_relaxed); }
    uint64_t take_wait_ns() { return wait_ns_.exchange(0, std::memory_order_relaxed); }
    uint64_t take_waits() { return waits_.exchange(0, std::memory_order_relaxed); }

private:
    struct Slot {
        std::atomic<uint64_t> sequence{0};
        uint8_t* to = nullptr;
        const uint8_t* from = nullptr;
        uint64_t bytes = 0, zeros = 0;
        uint8_t* snapshot = nullptr;
    };

    // The first unfinished job below `limit`, or limit. Amortized: a guest update per draw must not walk the slots.
    uint64_t advance(uint64_t limit) {
        const uint64_t tail = tail_.load(std::memory_order_relaxed);
        low_ = std::max(low_, tail > kSlots ? tail - kSlots : 0);  // a reused slot's old job has finished
        for (; low_ < limit; low_++)
            if (slots_[low_ & (kSlots - 1)].sequence.load(std::memory_order_acquire) == low_ + 1) break;
        return std::min(low_, limit);
    }

    void start(unsigned helpers) {
        if (started_.load(std::memory_order_acquire)) return;
        std::lock_guard lk(sleep_lock_);
        if (started_.load(std::memory_order_relaxed)) return;
        for (unsigned k = 0; k < helpers; k++) std::thread([this] { helper(); }).detach();
        started_.store(true, std::memory_order_release);
    }

    void put(uint8_t* to, const uint8_t* from, uint64_t bytes, uint64_t zeros, uint8_t* snapshot) {
        const uint64_t pos = tail_.load(std::memory_order_relaxed);
        Slot& s = slots_[pos & (kSlots - 1)];
        // Full: this slot's previous job has not finished. Work through the backlog rather than wait for it.
        while (s.sequence.load(std::memory_order_acquire) != pos)
            if (!run_one()) relax();
        s.to = to, s.from = from, s.bytes = bytes, s.zeros = zeros, s.snapshot = snapshot;
        s.sequence.store(pos + 1, std::memory_order_release);
        // seq_cst against helper()'s sleepers_ increment: either it sees this tail, or this sees the sleeper.
        tail_.store(pos + 1, std::memory_order_seq_cst);
        jobs_.fetch_add(1, std::memory_order_relaxed);
        // One wake per sleep: until the woken helper runs, further pushes would each pay a futex syscall for nothing
        // (on a Snapdragon 8 Gen 2 phone that notify was 40% of the replay thread's syscall time). The helper clears the flag when it
        // wakes, and re-checks the queue before it sleeps again, so no push is left behind.
        if (sleepers_.load(std::memory_order_seq_cst) && !wake_pending_.exchange(true, std::memory_order_seq_cst)) {
            { std::lock_guard lk(sleep_lock_); }
            wake_.notify_one();
        }
    }

    bool run_one() {
        uint64_t pos = head_.load(std::memory_order_relaxed);
        for (;;) {
            Slot& s = slots_[pos & (kSlots - 1)];
            const uint64_t sequence = s.sequence.load(std::memory_order_acquire);
            if (sequence == pos + 1) {
                if (head_.compare_exchange_weak(pos, pos + 1, std::memory_order_acq_rel, std::memory_order_relaxed)) {
                    std::memcpy(s.to, s.from, size_t(s.bytes));
                    if (s.zeros) std::memset(s.to + s.bytes, 0, size_t(s.zeros));
                    if (s.snapshot) std::memcpy(s.snapshot, s.to, size_t(s.bytes));
                    s.sequence.store(pos + kSlots, std::memory_order_release);
                    done_.fetch_add(1, std::memory_order_acq_rel);
                    return true;
                }
            } else if (sequence <= pos) {
                return false;  // nothing published at this position yet
            } else {
                pos = head_.load(std::memory_order_relaxed);
            }
        }
    }

    void helper() {
#if defined(__linux__)
        pthread_setname_np(pthread_self(), "nvn copy");
#endif
        for (;;) {
            for (uint32_t spin = 0; spin < kSpinRounds; spin++) {
                if (run_one())
                    spin = 0;
                else
                    relax();
            }
            sleepers_.fetch_add(1, std::memory_order_seq_cst);
            {
                std::unique_lock lk(sleep_lock_);
                wake_.wait(lk, [&] {
                    // Cleared before every look at the queue: a wake that finds it empty (the producer drained it)
                    // must not leave the flag set, or later pushes would skip the notify this helper sleeps on.
                    wake_pending_.store(false, std::memory_order_seq_cst);
                    return head_.load(std::memory_order_seq_cst) != tail_.load(std::memory_order_seq_cst);
                });
            }
            sleepers_.fetch_sub(1, std::memory_order_seq_cst);
        }
    }

    Slot slots_[kSlots];
    alignas(64) std::atomic<uint64_t> tail_{0};
    alignas(64) std::atomic<uint64_t> head_{0};
    alignas(64) std::atomic<uint64_t> done_{0};
    alignas(64) std::atomic<uint32_t> sleepers_{0};
    std::atomic<bool> wake_pending_{false};
    std::atomic<bool> started_{false};
    uint64_t low_ = 0;  // advance(): every job below it has finished
    std::mutex sleep_lock_;
    std::condition_variable wake_;
    std::atomic<uint64_t> bytes_{0}, jobs_{0}, wait_ns_{0}, waits_{0};
};

// Longer jobs that are not a copy of guest bytes (ASTC decodes), one at a time and in order: one queue slot would hold a
// copy helper for tens of milliseconds. Same rule: done before the command buffers that read the result are queued.
class Lane {
public:
    uint64_t ticket() const { return pushed_.load(std::memory_order_relaxed); }
    bool reached(uint64_t ticket) const { return done_.load(std::memory_order_acquire) >= ticket; }
    uint64_t backlog() const { return backlog_.load(std::memory_order_relaxed); }  // bytes held by waiting jobs

    void push(std::function<void()> job, uint64_t bytes) {
        std::lock_guard lk(lock_);
        if (!started_) {
            started_ = true;
            std::thread([this] { run(); }).detach();
        }
        backlog_.fetch_add(bytes, std::memory_order_relaxed);
        jobs_.push_back({std::move(job), bytes});
        pushed_.fetch_add(1, std::memory_order_relaxed);
        wake_.notify_one();
    }

    void wait() {
        const uint64_t target = ticket();
        if (reached(target)) return;
        const auto start = std::chrono::steady_clock::now();
        std::unique_lock lk(lock_);
        idle_.wait(lk, [&] { return reached(target); });
        wait_ns_.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::steady_clock::now() - start).count()),
                           std::memory_order_relaxed);
    }

    uint64_t take_wait_ns() { return wait_ns_.exchange(0, std::memory_order_relaxed); }

private:
    void run() {
#if defined(__linux__)
        pthread_setname_np(pthread_self(), "nvn decode");
#endif
        for (;;) {
            std::unique_lock lk(lock_);
            wake_.wait(lk, [&] { return !jobs_.empty(); });
            auto [job, bytes] = std::move(jobs_.front());
            jobs_.pop_front();
            lk.unlock();
            job();
            job = nullptr;
            backlog_.fetch_sub(bytes, std::memory_order_relaxed);
            lk.lock();
            done_.fetch_add(1, std::memory_order_release);
            idle_.notify_all();
        }
    }

    std::mutex lock_;
    std::condition_variable wake_, idle_;
    std::deque<std::pair<std::function<void()>, uint64_t>> jobs_;
    std::atomic<uint64_t> pushed_{0}, done_{0}, backlog_{0}, wait_ns_{0};
    bool started_ = false;
};

inline Queue& queue() {
    static Queue* q = new Queue();  // leaked on purpose: the helpers outlive static destruction
    return *q;
}

inline unsigned helpers() {
    static const unsigned n = [] {
        const char* e = std::getenv("NX_COPY_BEHIND");
        return e ? unsigned(std::max(0, std::atoi(e))) : 2u;
    }();
    return n;
}

// Whether a copy of `bytes` should be queued rather than made here.
inline bool wanted(uint64_t bytes) {
    static const uint64_t least = [] {
        const char* e = std::getenv("NX_COPY_BEHIND_MIN");
        return e ? std::strtoull(e, nullptr, 10) : 4096ull;
    }();
    return bytes >= least && helpers() != 0;
}

inline void push(uint8_t* to, const uint8_t* from, uint64_t bytes, uint64_t zeros, uint8_t* snapshot = nullptr) {
    queue().push(to, from, bytes, zeros, snapshot, helpers());
}

inline void wait() {
    if (helpers()) queue().wait();
}

inline Lane& lane() {
    static Lane* l = new Lane();  // leaked on purpose, as queue()
    return *l;
}

// Everything queued so far, of both kinds.
struct Ticket {
    uint64_t copies = 0, tasks = 0;
};
inline Ticket ticket() {
    if (!helpers()) return {};
    return {queue().ticket(), lane().ticket()};
}
inline bool reached(const Ticket& t) { return !helpers() || (queue().reached(t.copies) && lane().reached(t.tasks)); }

// wait() and a task: what a command buffer about to be queued can read.
inline void wait_all() {
    if (!helpers()) return;
    queue().wait();
    lane().wait();
}

inline void wait_for(const uint8_t* p, uint64_t size) {
    if (helpers()) queue().wait_for(p, size);
}

// `then` once the copies queued so far have landed, here if they already have, else on the lane, so the replay thread
// goes on recording instead of copying them itself.
inline void after_copies(std::function<void()> then) {
    if (!helpers()) return then();
    const uint64_t copies = queue().ticket();
    if (queue().reached(copies)) return then();
    lane().push([copies, then = std::move(then)] {
        queue().help_through(copies);
        then();
    }, 0);
}

}  // namespace nx::nvn::replay::copy_behind
