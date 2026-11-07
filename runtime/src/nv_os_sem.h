// Host semaphores standing in for NvOsSemaphore objects the guest SDK could not create (svc_nv.cpp owns the HLE).
//
// The movie library in subsdk0 (NvMMLite, the fake nvdec's guest side) creates one NvOsSemaphore per decoder context
// with NvOsSemaphoreCreate(&ctx[0x4020], 0) and destroys it unconditionally during teardown. The SDK's create
// (sdk+0x41d780) allocates 0x28 bytes through the game's 512 MB graphics allocator and, when that allocation fails,
// returns an error *without writing the out parameter* — the context keeps a NULL handle and the teardown's
// FinalizeSemaphore(NULL) faults at data address 0x10. This table is what svc_nv.cpp hands the guest instead of that
// NULL: a plain counting semaphore with nn::os::Semaphore semantics (wait/block, signal, timed wait).
//
// A handle is the object's address. The host heap is disjoint from the guest address space, so a handle can never be
// mistaken for a guest pointer; looking a handle up is how destroy/wait/signal decide whether the object is ours.
// Destroying a handle that is not in the table is a no-op ("not ours"), which is what makes a NULL teardown harmless.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>

namespace nx {

struct HostSemaphore {
    std::mutex m;
    std::condition_variable cv;
    uint32_t count = 0;
    bool destroyed = false;
};

class NvOsSemaphoreTable {
public:
    // The initial count is the argument the SDK passes to nn::os::InitializeSemaphore; the table never fails (a host
    // allocation of a few dozen bytes is not a resource the guest can exhaust), so a create always produces a handle.
    uint64_t create(uint32_t initial) {
        auto sem = std::make_shared<HostSemaphore>();
        sem->count = initial;
        const uint64_t h = reinterpret_cast<uint64_t>(sem.get());
        std::lock_guard g(m_);
        // The host allocator may hand back an address we released earlier; it is a new object, not a stale one.
        destroyed_.erase(h);
        sems_[h] = std::move(sem);
        return h;
    }

    // True when `h` was one of ours and has now been released; false means the handle is not in this table (a valid
    // SDK handle must still go to the SDK, and a NULL one has to be tolerated by the caller).
    //
    // Waiters are woken here: the object can be kept alive by a thread blocked in wait()/wait_timeout() (the guest's
    // own object would be a use-after-free in that race), and that thread has to return an error rather than block
    // forever once the context it waited for is gone.
    bool destroy(uint64_t h) {
        std::lock_guard g(m_);
        auto it = sems_.find(h);
        if (it == sems_.end()) return false;
        {
            std::lock_guard l(it->second->m);
            it->second->destroyed = true;
        }
        it->second->cv.notify_all();
        sems_.erase(it);
        if (destroyed_.size() > 4096) destroyed_.erase(destroyed_.begin());
        destroyed_.insert(h);
        return true;
    }

    // The object, or null when `h` is not ours. The shared_ptr keeps it alive for a waiter even if the guest destroys
    // its handle concurrently (see destroy()).
    std::shared_ptr<HostSemaphore> find(uint64_t h) const {
        std::lock_guard g(m_);
        auto it = sems_.find(h);
        return it == sems_.end() ? nullptr : it->second;
    }

    // True when `h` is one of ours and has already been destroyed. A worker thread can still signal/wait the handle it
    // was given while the decoder teardown runs; forwarding that call to the SDK would pass it a host pointer, so the
    // caller must treat it as a no-op/failure instead.
    bool was_destroyed(uint64_t h) const {
        std::lock_guard g(m_);
        return destroyed_.count(h) != 0;
    }

    // False when the object was already destroyed (the signal went nowhere).
    static bool signal(const std::shared_ptr<HostSemaphore>& sem) {
        {
            std::lock_guard g(sem->m);
            if (sem->destroyed) return false;
            ++sem->count;
        }
        sem->cv.notify_one();
        return true;
    }

    // True when a signal was consumed, false when the object was destroyed first (the caller reports a failure). The
    // predicate includes `destroyed`, so destroy() cannot leave a waiter blocked on a count that will never come.
    static bool wait(const std::shared_ptr<HostSemaphore>& sem) {
        std::unique_lock lk(sem->m);
        sem->cv.wait(lk, [&] { return sem->count > 0 || sem->destroyed; });
        if (sem->count == 0) return false;
        --sem->count;
        return true;
    }

    // NvOsSemaphoreWaitTimeout's timeout is milliseconds (the SDK multiplies w1 by 1e6 for a nanosecond wait) and its
    // result is 0 on success, 5 on timeout: the SDK path is `tst w0, #1; csel w0, wzr, w5, ne` (sdk+0x41d7f0).
    // A destroy wakes the wait early and also reports 5.
    static uint32_t wait_timeout(const std::shared_ptr<HostSemaphore>& sem, uint32_t ms) {
        std::unique_lock lk(sem->m);
        if (!sem->cv.wait_for(lk, std::chrono::milliseconds(ms), [&] { return sem->count > 0 || sem->destroyed; }))
            return 5;
        if (sem->count == 0) return 5;
        --sem->count;
        return 0;
    }

private:
    mutable std::mutex m_;
    std::map<uint64_t, std::shared_ptr<HostSemaphore>> sems_;
    mutable std::set<uint64_t> destroyed_;  // host addresses already released (bounded)
};

}  // namespace nx
