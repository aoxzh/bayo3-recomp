// The replay thread (NX_REPLAY_THREAD=0 turns it off). nvnQueueSubmitCommands and nvnQueuePresentTexture used to
// replay into Vulkan and wait for the GPU on the game's render thread, and the game's main thread waits for that
// thread every frame: the frame was game logic + recording + replay + GPU, one after the other (~30 fps in a course
// with every part under 16 ms; docs/archive/nvn/DEBUG.md #40). Like a real GPU, the work now goes to a queue that one host
// thread replays in order; the game only waits where NVN makes it wait: nvnSyncWait on a fenced sync, nvnQueueFinish,
// memory pool invalidation and finalization, and a present queue at most two presents deep.
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

#include "nvn.h"
#include "platform.h"

namespace nx::nvn {

namespace {

std::mutex g_lock;
std::condition_variable g_cv, g_done_cv;
std::deque<std::pair<uint64_t, std::function<void()>>> g_jobs;
uint64_t g_enqueued = 0, g_done = 0;
bool g_started = false;
// Where the replay thread's wall time goes: running a job, or waiting for the next one. A frame that is 44 ms while the
// replay's own timers add up to 35 ms is either a replay thread starved for work (the guest has not queued the next
// submit yet: idle is high) or a guest thread blocked on something else while the replay has nothing to do. Timed per
// job; one submit per frame, so two clock reads.
std::atomic<uint64_t> g_idle_ns{0}, g_busy_ns{0};
uint64_t g_jobs_run = 0;
void (*g_on_idle)() = nullptr;  // worker_set_idle: only ever set from a job, so on this thread

bool enabled() {
    static const bool on = [] {
        const char* e = std::getenv("NX_REPLAY_THREAD");
        if (e) return e[0] == '1';
#if defined(NX_BAYONETTA3)
        // On for Bayonetta 3 by default. The replay, the fence wait and the compute write-backs of a frame are ~20-25 ms
        // on the submitting thread, while the guest's own ~8 ms of game logic can run in parallel with them: measured on
        // RADV/VANGOGH the heavy 3D scene went from 29-33 fps to 39-40 fps (p10 22-30 -> 38-39, 0/80 windows under
        // 30) with the same per-frame pass count (docs/archive/PERF-2026-10-23.md §3). The Wonder-era objection (passes
        // multiplied with the replay a frame behind, DEBUG.md #40) does not reproduce here. NX_REPLAY_THREAD=0 restores
        // the synchronous replay.
        return true;
#else
        // Off elsewhere unless asked for: with the replay a frame behind, Wonder's course render passes multiplied
        // (1.3 M ended by target changes against 125 k) and it ran slower (docs/archive/nvn/DEBUG.md #40).
        return false;
#endif
    }();
    return on;
}

void run() {
    plat::set_thread_name("nvn replay");
    for (;;) {
        std::pair<uint64_t, std::function<void()>> job;
        const uint64_t t_ready = plat::monotonic_ns();
        {
            std::unique_lock lk(g_lock);
            if (g_jobs.empty() && g_on_idle) {
                lk.unlock();
                g_on_idle();
                lk.lock();
            }
            g_cv.wait(lk, [] { return !g_jobs.empty(); });
            job = std::move(g_jobs.front());
            g_jobs.pop_front();
        }
        const uint64_t t_start = plat::monotonic_ns();
        g_idle_ns.fetch_add(t_start - t_ready, std::memory_order_relaxed);
        job.second();
        g_busy_ns.fetch_add(plat::monotonic_ns() - t_start, std::memory_order_relaxed);
        {
            std::lock_guard lk(g_lock);
            g_done = job.first;
        }
        g_jobs_run++;
        g_done_cv.notify_all();
    }
}

}  // namespace

void worker_set_idle(void (*fn)()) { g_on_idle = fn; }

uint64_t worker_run(std::function<void()> job) {
    if (!enabled()) {
        job();
        return 0;
    }
    uint64_t serial;
    {
        std::lock_guard lk(g_lock);
        if (!g_started) {
            g_started = true;
            std::thread(run).detach();
        }
        serial = ++g_enqueued;
        g_jobs.push_back({serial, std::move(job)});
    }
    g_cv.notify_one();
    return serial;
}

void worker_wait(uint64_t serial) {
    if (!serial) return;
    std::unique_lock lk(g_lock);
    g_done_cv.wait(lk, [&] { return g_done >= serial; });
}

uint64_t worker_last() {
    std::lock_guard lk(g_lock);
    return g_enqueued;
}

void worker_finish() { worker_wait(worker_last()); }

// Per timing window (nvn_vk.cpp log_timing): the replay thread's time running jobs vs waiting for one. Idle is the
// answer to "is the replay thread starved, or is the guest thread blocked somewhere else while the GPU waits too".
void worker_log(uint64_t frames) {
    const uint64_t idle = g_idle_ns.exchange(0, std::memory_order_relaxed);
    const uint64_t busy = g_busy_ns.exchange(0, std::memory_order_relaxed);
    const uint64_t jobs = g_jobs_run;
    g_jobs_run = 0;
    if (!idle && !busy) return;
    const double total = double(idle + busy);
    log("nvn/vk: replay thread over %llu: %.1f jobs/frame, busy %.2f ms/frame, idle %.2f ms/frame (%.0f%% waiting for "
        "a job)",
        (unsigned long long)frames, double(jobs) / double(frames), double(busy) / 1e6 / double(frames),
        double(idle) / 1e6 / double(frames), total ? 100.0 * double(idle) / total : 0.0);
}

}  // namespace nx::nvn
