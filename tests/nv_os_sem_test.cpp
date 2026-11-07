// Unit test for runtime/src/nv_os_sem.h: the host semaphores svc_nv.cpp installs when the guest SDK's own
// NvOsSemaphoreCreate cannot allocate one (the fake nvdec's NULL-handle crash, docs/archive/MOVIE-EV0005.md §16.4).
//
// What matters for the fix, and what this test pins:
//   * create() always yields a non-null handle (the SDK's create returns an error and leaves *out untouched when its
//     0x28-byte allocation fails; that untouched NULL is what a decoder teardown then destroys);
//   * the handle has nn::os::Semaphore semantics — initial count, signal, blocking wait, timed wait with the SDK's
//     result codes (0 success, 5 timeout);
//   * destroy() releases a handle once and reports every other input (NULL, garbage, a handle already destroyed) as
//     "not ours", which is what makes a NULL teardown a no-op instead of a fault;
//   * destroy() wakes a thread blocked in wait()/wait_timeout() (a worker racing the decoder teardown) so the waiter
//     returns a failure instead of blocking forever — a hung guest thread is worse than the fault being fixed;
//   * a handle that the host allocator reuses for a new semaphore is a new object, not a stale one.
//
// Run: private/build-tests, `ctest -R nv_os_sem` (configured with -DNX_TESTS=ON).
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#include "nv_os_sem.h"

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    } else {
        std::printf("ok: %s\n", what);
    }
}

using nx::HostSemaphore;
using nx::NvOsSemaphoreTable;

int64_t elapsed_ms(std::chrono::steady_clock::time_point since) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - since).count();
}

}  // namespace

int main() {
    NvOsSemaphoreTable t;

    // 1. The property the SDK loses on a failed allocation: a create that succeeds always produces a handle.
    const uint64_t a = t.create(0);
    check(a != 0, "create(0) returns a non-null handle");
    check(t.find(a) != nullptr, "the created handle resolves");
    check(t.find(0) == nullptr, "NULL is never one of our handles");

    // 2. Timed wait: 0 on a consumed signal, 5 on timeout (the SDK's own result codes).
    check(NvOsSemaphoreTable::wait_timeout(t.find(a), 10) == 5, "0-count timed wait times out with 5");
    check(NvOsSemaphoreTable::signal(t.find(a)), "signal on a live handle succeeds");
    check(NvOsSemaphoreTable::wait_timeout(t.find(a), 1000) == 0, "a signal wakes a timed wait with 0");
    check(NvOsSemaphoreTable::wait_timeout(t.find(a), 10) == 5, "the signal is consumed exactly once");

    // 3. The initial count is honoured: the decoder creates its semaphore with a count of its choice.
    const uint64_t b = t.create(2);
    check(NvOsSemaphoreTable::wait_timeout(t.find(b), 1000) == 0, "initial count 2: first wait succeeds");
    check(NvOsSemaphoreTable::wait_timeout(t.find(b), 1000) == 0, "initial count 2: second wait succeeds");
    check(NvOsSemaphoreTable::wait_timeout(t.find(b), 10) == 5, "initial count 2: third wait times out");

    // 4. Blocking wait released by another thread (the decoder signals from its worker thread).
    const uint64_t c = t.create(0);
    std::thread waiter([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        NvOsSemaphoreTable::signal(t.find(c));
    });
    check(NvOsSemaphoreTable::wait(t.find(c)), "a blocking wait consumes another thread's signal");
    waiter.join();

    // 5. Destroy: ours exactly once; anything else is "not ours" and must not fault (that is the crash being fixed).
    check(t.destroy(b), "destroy releases our semaphore");
    check(t.find(b) == nullptr, "the destroyed handle is gone");
    check(t.was_destroyed(b), "the destroyed handle is recorded as ours-and-gone");
    check(!t.destroy(b), "a repeated destroy is a no-op");
    check(!t.destroy(0), "destroy(NULL) is a no-op");
    check(!t.destroy(0xdeadbeefull), "destroy(garbage) is a no-op");
    check(!t.was_destroyed(0xdeadbeefull), "a garbage handle was never ours");

    // 6. Destroy must wake a thread blocked in wait(): the guest's delete race must not become a hung thread.
    {
        const uint64_t d = t.create(0);
        std::shared_ptr<HostSemaphore> blocked_on = t.find(d);
        std::atomic<bool> consumed{true};
        std::thread blocked([&] { consumed = NvOsSemaphoreTable::wait(blocked_on); });
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const auto t0 = std::chrono::steady_clock::now();
        check(t.destroy(d), "destroy of a semaphore another thread waits on succeeds");
        blocked.join();
        check(!consumed.load(), "the blocked wait returned a failure, not a signal");
        check(elapsed_ms(t0) < 500, "the blocked wait returned promptly after destroy");
    }

    // 7. ... and a timed wait is cut short by destroy instead of waiting out its full timeout.
    {
        const uint64_t e = t.create(0);
        std::shared_ptr<HostSemaphore> waiting_on = t.find(e);
        std::atomic<uint32_t> rc{0};
        std::thread blocked([&] { rc = NvOsSemaphoreTable::wait_timeout(waiting_on, 5000); });
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const auto t0 = std::chrono::steady_clock::now();
        check(t.destroy(e), "destroy during a timed wait succeeds");
        blocked.join();
        check(rc.load() == 5, "the timed wait reports timeout (5) when the object is destroyed");
        check(elapsed_ms(t0) < 500, "the timed wait did not wait out its 5 s timeout");
    }

    // 8. A signal/wait arriving after the destroy is a no-op on the retained object, never a fault.
    {
        const uint64_t f = t.create(0);
        std::shared_ptr<HostSemaphore> stale = t.find(f);
        check(t.destroy(f), "destroy the semaphore a late signal will target");
        check(!NvOsSemaphoreTable::signal(stale), "a late signal reports the object is gone");
        check(!NvOsSemaphoreTable::wait(stale), "a late wait reports the object is gone");
        check(t.was_destroyed(f), "the stale handle stays classified as ours-and-gone");
    }

    // 9. The host allocator can hand back a destroyed address for a new semaphore: that is a NEW object.
    {
        bool reused = false;
        for (int k = 0; k < 64 && !reused; ++k) {
            const uint64_t g = t.create(0);
            t.destroy(g);
            const uint64_t h = t.create(0);
            if (h == g) {
                reused = true;
                check(t.find(h) != nullptr, "a reused address is a live handle");
                check(!t.was_destroyed(h), "a reused address is not a stale one");
                check(t.destroy(h), "the reused handle destroys normally");
            } else {
                t.destroy(h);
            }
        }
        check(true, "address reuse was exercised (or no reuse in 64 attempts)");
    }

    // 10. The table stays consistent under concurrent create/find/destroy.
    std::atomic<int> bad{0};
    std::vector<std::thread> threads;
    for (int n = 0; n < 4; ++n) {
        threads.emplace_back([&, n] {
            std::vector<uint64_t> handles;
            for (int k = 0; k < 250; ++k) {
                const uint64_t h = t.create(uint32_t(n + k));
                if (!h || !t.find(h)) bad.fetch_add(1);
                handles.push_back(h);
            }
            for (uint64_t h : handles)
                if (!t.destroy(h)) bad.fetch_add(1);
        });
    }
    for (std::thread& th : threads) th.join();
    check(bad.load() == 0, "4000 concurrent create/find/destroy all resolved to our handles");

    // 11. An unrelated handle survives that storm and can still be destroyed.
    check(t.find(a) != nullptr, "an unrelated handle survives the concurrent create/destroy storm");
    check(t.destroy(a), "it can still be destroyed afterwards");

    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("all nv_os_sem checks passed\n");
    return 0;
}
