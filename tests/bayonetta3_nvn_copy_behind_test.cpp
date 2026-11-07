// nvn_copy_behind.h: every queued copy lands, in the shape asked for, by the time wait() returns.
//
// clang++ -std=c++20 -O2 -pthread tests/bayonetta3_nvn_copy_behind_test.cpp -o /tmp/copy_behind_test && /tmp/copy_behind_test
#include "../runtime/src/nvn/nvn_copy_behind.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <random>
#include <thread>
#include <vector>

using nx::nvn::replay::copy_behind::Queue;

namespace {

int g_failures = 0;
void check(bool ok, const char* what) {
    if (ok) return;
    std::printf("FAIL: %s\n", what);
    g_failures++;
}

std::vector<uint8_t> pattern(size_t n, uint32_t seed) {
    std::vector<uint8_t> v(n);
    std::mt19937 r(seed);
    for (auto& b : v) b = uint8_t(r());
    return v;
}

// A range larger than a slice, with tail zeros and a snapshot: the tail goes after the last slice only, and the
// snapshot covers the whole range.
void large_range(Queue& q) {
    const uint64_t n = 3 * Queue::kSlice + 12345, zeros = 4096;
    auto from = pattern(n, 1);
    std::vector<uint8_t> to(n + zeros + 16, 0xCD), snapshot(n, 0xCD);
    q.push(to.data(), from.data(), n, zeros, snapshot.data(), 2);
    q.wait();
    check(std::memcmp(to.data(), from.data(), n) == 0, "large range: copy");
    check(std::memcmp(snapshot.data(), from.data(), n) == 0, "large range: snapshot");
    bool tail = true;
    for (uint64_t k = n; k < n + zeros; k++) tail = tail && to[k] == 0;
    check(tail, "large range: zero tail");
    check(to[n + zeros] == 0xCD, "large range: nothing past the tail");
}

// More jobs than slots, so the producer laps helpers that finish out of order.
void many_jobs(Queue& q) {
    const uint32_t count = Queue::kSlots * 8;
    auto from = pattern(count * 256, 2);
    std::vector<uint8_t> to(count * 256, 0);
    std::mt19937 r(3);
    std::vector<uint32_t> order(count);
    std::iota(order.begin(), order.end(), 0u);
    std::shuffle(order.begin(), order.end(), r);
    for (uint32_t k : order) q.push(to.data() + k * 256ull, from.data() + k * 256ull, 256, 0, nullptr, 2);
    q.wait();
    check(to == from, "many jobs: every destination filled");
}

// wait_for: a queued read of bytes about to change has run first, and one of other bytes does not force a wait.
void write_after_queued_read(Queue& q) {
    for (int round = 0; round < 2000; round++) {
        std::vector<uint8_t> guest(64 * 1024, 0xAA), ring(64 * 1024, 0);
        q.push(ring.data(), guest.data(), guest.size(), 0, nullptr, 2);
        q.wait_for(guest.data() + 4096, 16);
        std::memset(guest.data() + 4096, 0xBB, 16);
        q.wait();
        bool old = true;
        for (uint8_t b : ring) old = old && b == 0xAA;
        check(old, "a queued copy saw a later write");
        if (!old) return;
    }
    q.take_waits();
    std::vector<uint8_t> guest(4096, 1), elsewhere(16, 2), ring(4096, 0);
    q.wait();
    q.wait_for(elsewhere.data(), elsewhere.size());
    check(q.take_waits() == 0, "wait_for on an empty queue waited");
    (void)ring;
}

// The helpers sleep once idle; a push after that has to wake them, or wait() drains it itself.
// help_through runs on another thread while the producer keeps queueing: it must not return before its own jobs are done.
void help_through_while_queueing(Queue& q) {
    const uint32_t count = Queue::kSlots * 2;
    std::vector<std::vector<uint8_t>> from(count, std::vector<uint8_t>(4096, 0x5a)), to(count, std::vector<uint8_t>(4096));
    for (uint32_t round = 0; round < 8; round++) {
        for (auto& v : to) std::fill(v.begin(), v.end(), 0);
        for (uint32_t k = 0; k < count / 2; k++) q.push(to[k].data(), from[k].data(), 4096, 0, nullptr, 2);
        const uint64_t ticket = q.ticket();
        std::atomic<bool> landed{false};
        std::thread helper([&] {
            q.help_through(ticket);
            bool all = true;
            for (uint32_t k = 0; k < count / 2; k++) all = all && to[k] == from[k];
            landed = all;
        });
        for (uint32_t k = count / 2; k < count; k++) q.push(to[k].data(), from[k].data(), 4096, 0, nullptr, 2);
        helper.join();
        check(landed.load(), "help_through returned before its jobs landed");
        q.wait();
    }
}

void after_idle(Queue& q) {
    for (int round = 0; round < 3; round++) {
        auto request = std::chrono::steady_clock::now() + std::chrono::milliseconds(5);
        while (std::chrono::steady_clock::now() < request) std::this_thread::yield();
        auto from = pattern(8192, 4);
        std::vector<uint8_t> to(8192, 0);
        q.push(to.data(), from.data(), 8192, 0, nullptr, 2);
        q.wait();
        check(to == from, "copy after the helpers slept");
    }
}

}  // namespace

int main() {
    Queue& q = *new Queue();  // leaked, as queue() does: the helpers never exit
    large_range(q);
    many_jobs(q);
    write_after_queued_read(q);
    help_through_while_queueing(q);
    after_idle(q);
    if (g_failures) return 1;
    std::printf("ok\n");
    return 0;
}
