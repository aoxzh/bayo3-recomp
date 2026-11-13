// NVN queues, command buffers, windows and syncs (docs/NVN.md, N3). Synchronously every command completes at
// submit: fences are signaled right away, the window hands out its textures in turn, and presents are counted. With
// NX_ASYNC_GPU a fence is the submit it follows, and waiting for it waits for that submit (nvn_frames.cpp). With
// NX_PIPELINED the GPU work is in flight when a submit returns but its compute results are in guest memory, so
// syncs and nvnQueueFinish have nothing to wait for, as synchronously (gpu_serial() is 0). Return
// values follow the SDK driver (nvnSyncWait sdk+45a960: 0 once the fence has passed; nvnWindowAcquireTexture
// sdk+45e050: 0 on success).
#include <algorithm>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <thread>
#include <exception>
#include <array>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <atomic>
#include <vector>
#include <map>
#ifdef __linux__
#include <dirent.h>
#include <unistd.h>
#endif

#include "nvn.h"
#include "nx/b3_film_grain.h"
#include "nvn_draw_frame_stats.h"
#include "nvn_submit_diag.h"
#include "nvn_ultrawide.h"
#include "platform.h"

namespace nx {
void svc_profile_mark(int role);
void svc_profile_report(uint64_t frames, int role);
uint64_t display_period_ns();
extern std::atomic<uint64_t> g_tick_calls, g_yield_calls, g_sleep_calls;
}
namespace nx::nvn {

uint64_t recorded_draws();  // nvn_state.cpp: sampled at the guest present boundary

// Submits the game made and submits whose replay (and GPU work, write-backs) finished. The game records the next frame
// on one thread while another submits the last one, and our replay runs inside nvnQueueSubmitCommands: a fence the
// game waits on must not pass before the submits it follows were replayed, or the game rewrites the next frame's
// vertex and uniform data while this one is still read (the horizon band, exploded meshes, DEBUG.md #59).
// NX_SYNC_LOOSE=1: the old behaviour (fences passed at once).
namespace {
// NX_TRACE_TIMELINE=<present>: from that present on, the queue events (submit, replay, sync wait, present) of 8 frames
// with their times and threads are logged (where a frame's wall clock goes).
struct TlEvent { uint64_t ns; int64_t tid; const char* what; uint64_t arg; };
TlEvent g_tl[2048];
std::atomic<uint64_t> g_tl_n{0}, g_presents{0};
uint64_t g_tl_from = UINT64_MAX;
void tl(const char* what, uint64_t arg = 0) {
    static const bool init = [] {
        if (const char* e = std::getenv("NX_TRACE_TIMELINE")) g_tl_from = uint64_t(std::atoll(e));
        return true;
    }();
    (void)init;
    if (g_tl_from == UINT64_MAX || g_presents < g_tl_from) return;
    uint64_t n = g_tl_n.fetch_add(1);
    if (n < 2048) g_tl[n] = {plat::monotonic_ns(), plat::current_thread_id(), what, arg};
}
std::mutex g_sub_lock;
std::condition_variable g_sub_cv;
uint64_t g_sub_called = 0, g_sub_done = 0;
std::atomic<uint64_t> g_sub_blocked{0};

bool sync_loose() {
    static const bool on = std::getenv("NX_SYNC_LOOSE") != nullptr;
    return on;
}

void submit_done() {
    {
        std::lock_guard lk(g_sub_lock);
        g_sub_done++;
    }
    g_sub_cv.notify_all();
}

// Where the game waits for the replay (docs/nvn/android-perf/PHASE3.md I1): calls, the ones that blocked and for how
// long, per 600 presents; frame_ns is this main-thread frame's blocked time, for the hitch lines (frame_time_probe).
struct WaitSite {
    const char* name;
    std::atomic<uint64_t> calls{0}, blocked{0}, ns{0}, frame_ns{0};
};
WaitSite g_wait_sites[kWaitSites] = {{"sync wait"}, {"queue finish"}, {"pool invalidate"}, {"pool finalize"}};

bool extra_wait_timing() {
    static const bool enabled = std::getenv("NX_PRESENT_TIMING") != nullptr;
    return enabled;
}
// Totals between the existing 600-present reports, from all calling threads. Ticket time excludes wait_submits;
// sync-worker time includes its call overhead; acquire time is measured only when it actually sleeps.
std::atomic<uint64_t> g_ticket_calls{0}, g_ticket_pending{0}, g_ticket_ns{0};
std::atomic<uint64_t> g_sync_worker_calls{0}, g_sync_worker_ns{0}, g_acquire_sleeps{0}, g_acquire_ns{0};
std::atomic<uint64_t> g_embedded_tickets{0}, g_embedded_submits{0};
std::atomic<uint64_t> g_fence_tail_calls{0}, g_fence_tail_ns{0};

// Waits until the submits up to `seq` were replayed (2 s at most: a hung replay is logged, not waited forever).
void wait_submits(uint64_t seq, int site) {
    if (sync_loose() || !seq) return;
    WaitSite& w = g_wait_sites[site];
    w.calls++;
    std::unique_lock lk(g_sub_lock);
    if (g_sub_done >= seq) return;
    uint64_t n = ++g_sub_blocked;
    if (n == 1 || n % 1000 == 0) log("nvn: %llu CPU waits blocked on a submit still being replayed", (unsigned long long)n);
    uint64_t t0 = plat::monotonic_ns();
    if (!g_sub_cv.wait_for(lk, std::chrono::seconds(2), [seq] { return g_sub_done >= seq; }))
        log("nvn: a CPU wait gave up after 2 s on submit %llu (%llu done)", (unsigned long long)seq,
            (unsigned long long)g_sub_done);
    uint64_t dt = plat::monotonic_ns() - t0;
    w.blocked++, w.ns += dt, w.frame_ns += dt;
}

void log_wait_sites() {
    std::string line;
    for (WaitSite& w : g_wait_sites) {
        char b[128];
        std::snprintf(b, sizeof b, "; %s %llu calls, %llu blocked %.1f ms", w.name, (unsigned long long)w.calls.exchange(0),
                      (unsigned long long)w.blocked.exchange(0), double(w.ns.exchange(0)) / 1e6);
        line += b;
    }
    log("nvn: game waits on the replay over 600 presents%s", line.c_str());
    if (extra_wait_timing())
        log("nvn: extra waits over 600 presents (totals, all threads): ticket %llu calls, %llu pending %.2f ms; "
            "sync worker %llu calls %.2f ms; acquire %llu sleeps %.2f ms; embedded fences %llu tickets in %llu submits",
            (unsigned long long)g_ticket_calls.exchange(0, std::memory_order_relaxed),
            (unsigned long long)g_ticket_pending.exchange(0, std::memory_order_relaxed),
            double(g_ticket_ns.exchange(0, std::memory_order_relaxed)) / 1e6,
            (unsigned long long)g_sync_worker_calls.exchange(0, std::memory_order_relaxed),
            double(g_sync_worker_ns.exchange(0, std::memory_order_relaxed)) / 1e6,
            (unsigned long long)g_acquire_sleeps.exchange(0, std::memory_order_relaxed),
            double(g_acquire_ns.exchange(0, std::memory_order_relaxed)) / 1e6,
            (unsigned long long)g_embedded_tickets.exchange(0, std::memory_order_relaxed),
            (unsigned long long)g_embedded_submits.exchange(0, std::memory_order_relaxed));
    if (submit_diag_on())
        log("nvn/submit-diag: guest-fence completion over 600 presents: %llu calls %.3f ms total "
            "(GPU lock + drain + publication; overlaps embedded-tail)",
            (unsigned long long)g_fence_tail_calls.exchange(0, std::memory_order_relaxed),
            double(g_fence_tail_ns.exchange(0, std::memory_order_relaxed)) / 1e6);
}

uint64_t submits_called() {
    std::lock_guard lk(g_sub_lock);
    return g_sub_called;
}
}  // namespace

// MemoryPoolInvalidateMappedRange / Finalize (nvn_objects.cpp): the CPU reads or frees what submits made so far use.
void wait_submits_so_far(int site) { wait_submits(submits_called(), site); }

namespace {

// ---- syncs ------------------------------------------------------------------------------------------------------

// Each submit owns a separate fence ticket. Re-fencing/reinitializing the same
// guest address cannot let an older completion satisfy the newer generation.
struct CommandFence {
    std::mutex lock;
    std::condition_variable cv;
    bool complete = false;
    uint64_t serial = 0, seq = 0;  // Queue fences retain their own wait target across re-fencing.
};
// Android NDK libc++ does not provide atomic<shared_ptr>. Keep the same
// synchronized ticket publication without depending on that specialization.
struct AtomicFence {
    mutable std::mutex lock;
    std::shared_ptr<CommandFence> value;
    std::shared_ptr<CommandFence> load() const {
        std::lock_guard guard(lock);
        return value;
    }
    void store(std::shared_ptr<CommandFence> next) {
        std::lock_guard guard(lock);
        value = std::move(next);
    }
    std::shared_ptr<CommandFence> exchange(std::shared_ptr<CommandFence> next) {
        std::lock_guard guard(lock);
        value.swap(next);
        return next;
    }
};
struct Sync : Object {
    AtomicFence command_fence;
    std::atomic<bool> signaled{false};
    std::atomic<uint64_t> serial{0};  // NX_ASYNC_GPU: the submit it waits for (0: none; always 0 otherwise)
    std::atomic<uint64_t> job{0};     // the replay-thread job that fences it (0: none)
    std::atomic<uint64_t> seq{0};     // the submits it follows (wait_submits); 0: none
};

void SyncInitialize(CPU& c) {  // NVNboolean (NVNsync*, NVNdevice*)
    make<Sync>(c.x[0]);
    c.x[0] = 1;
}

void SyncFinalize(CPU& c) { destroy(c.x[0]); }

void signal_sync(uint64_t guest) {
    if (auto* s = get<Sync>(guest)) {
        s->command_fence.store(nullptr);
        s->serial = 0, s->job = 0, s->seq = 0, s->signaled = true;
    }
}

// NVNsyncWaitResult (const NVNsync*, u64 timeout). Always 0 (already signaled), as before: a fenced submit is waited
// for whatever the timeout, 2 s at most (then the GPU hung and the process ends). Correct results before latency.
void SyncWait(CPU& c) {
    auto* s = get<Sync>(c.x[0]);
    tl("syncwait>");
    // NX_FAST_SYNC=1 (EXPERIMENT, not a default): report the wait as already satisfied and let the guest run ahead.
    // This can improve overlap, but is only safe if the guest does not read data that an unfinished submit
    // is still writing. Compare output with the blocking path before relying on this experiment.
    static const bool fast_sync = [] {
        const char* e = std::getenv("NX_FAST_SYNC");
        return e && e[0] == '1';
    }();
    if (fast_sync) {
        c.x[0] = 0;
        return;
    }
    // NX_SYNC_AHEAD=<n> (EXPERIMENT): the bounded form of NX_FAST_SYNC. The guest may run ahead of the replay by at
    // most n submits; past that it waits as before. The window a stale read could happen in is then bounded by n
    // frames instead of unbounded, at the cost of some of the overlap. n=0 is the normal behaviour.
    static const int ahead = [] {
        const char* e = std::getenv("NX_SYNC_AHEAD");
        return e ? std::atoi(e) : 0;
    }();
    if (ahead > 0 && int(frame_replays_in_flight()) <= ahead) {
        c.x[0] = 0;
        return;
    }
    // Snapshot the ticket before waiting; completion of an older submitted
    // fence must not clear a later fence armed at the same Sync address.
    auto command = s ? s->command_fence.load() : nullptr;
    if (command) {
        std::unique_lock lock(command->lock);
        const bool timing = extra_wait_timing();
        if (timing) g_ticket_calls.fetch_add(1, std::memory_order_relaxed);
        const uint64_t wait_start = timing && !command->complete ? plat::monotonic_ns() : 0;
        // The completion runs at retire time now (frame_submit), and the only other retire trigger is a new
        // submission -- which this thread cannot make while it waits here. So retry with a timeout and drive
        // retirement ourselves; each pass either finishes a submit (releasing us) or waits a little longer.
        while (!command->complete) {
            if (command->cv.wait_for(lock, std::chrono::microseconds(500), [&] { return command->complete; }))
                break;
            lock.unlock();
            const bool more = frame_retire_one();
            lock.lock();                       // always re-acquire: the tail unlocks again below
            if (!more) break;                  // nothing in flight: the completion can never arrive
        }
        if (!command->complete) command->cv.wait(lock, [&] { return command->complete; });
        if (wait_start) {
            g_ticket_pending.fetch_add(1, std::memory_order_relaxed);
            g_ticket_ns.fetch_add(plat::monotonic_ns() - wait_start, std::memory_order_relaxed);
        }
        uint64_t serial = command->serial, seq = command->seq;
        lock.unlock();
        wait_submits(seq, kWaitSync);
        if (serial) gpu_wait(serial);
        tl("syncwait<");
        c.x[0] = 0;
        return;
    }
    if (s) wait_submits(s->seq.load(), kWaitSync);
    if (s) {
        const uint64_t job = s->job.load();
        const uint64_t wait_start = extra_wait_timing() && job ? plat::monotonic_ns() : 0;
        worker_wait(job);
        if (wait_start) {
            g_sync_worker_calls.fetch_add(1, std::memory_order_relaxed);
            g_sync_worker_ns.fetch_add(plat::monotonic_ns() - wait_start, std::memory_order_relaxed);
        }
    }
    tl("syncwait<");
    if (uint64_t serial = s ? s->serial.load() : 0) gpu_wait(serial);
    c.x[0] = 0;
}

// ---- queues -----------------------------------------------------------------------------------------------------


using CommandFences = std::vector<std::shared_ptr<CommandFence>>;
void arm_command_fences(const CmdList& list, const CommandListTable& captured, CommandFences& fences, int depth = 0) {
    for (const Cmd& c : list.cmds) {
        if (c.op == Cmd::CallList && depth < 8) {
            auto found = captured.find(c.a[0]);
            if (found != captured.end() && found->second) arm_command_fences(*found->second, captured, fences, depth + 1);
        } else if (c.op == Cmd::FenceSync) {
            auto* sync = get<Sync>(c.a[0]);
            if (!sync) fatal("nvn: FenceSync references an uninitialized sync");
            auto fence = std::make_shared<CommandFence>();
            sync->command_fence.store(fence);
            sync->signaled = false;
            sync->serial = 0; sync->job = 0; sync->seq = 0;
            fences.push_back(std::move(fence));
        }
    }
}
void complete_command_fences(const CommandFences& fences) {
    for (const auto& fence : fences) {
        { std::lock_guard lock(fence->lock); fence->complete = true; }
        fence->cv.notify_all();
    }
}

void QueueInitialize(CPU& c) { c.x[0] = 1; }  // NVNboolean (NVNqueue*, const NVNqueueBuilder*)
void QueueFinalize(CPU&) {}
void QueueSubmitCommands(CPU& c) {  // (NVNqueue*, int count, const NVNcommandHandle*)
    plat::perf_hint_thread();  // this thread replays: part of every frame (perf_hint.cpp)
    // NX_PRESENT_TIMING=1: submit-to-submit wall/CPU time on this guest thread. With the replay worker enabled,
    // the call timer covers capture/queueing; command recording and guest work happen outside it. These counters
    // alone cannot attribute replay idle to engine pacing: command-fence waits and recording can gate the next submit.
    static const bool sub_timing = std::getenv("NX_PRESENT_TIMING") != nullptr;
    static uint64_t sub_prev = 0, sub_cpu_prev = 0, sub_sum_period = 0, sub_sum_cpu = 0, sub_sum_call = 0, sub_n = 0;
    const uint64_t sub_t0 = sub_timing ? plat::monotonic_ns() : 0;
    uint64_t sub_cpu = 0;
    if (sub_timing) {
        long f[2], sw[2];
        plat::thread_counters(sub_cpu, f, sw);
        if (sub_prev && sub_cpu_prev) sub_sum_period += sub_t0 - sub_prev, sub_sum_cpu += sub_cpu - sub_cpu_prev;
        sub_prev = sub_t0, sub_cpu_prev = sub_cpu;
    }
    std::vector<std::shared_ptr<CmdList>> lists;
    std::vector<uint64_t> handles;
    for (uint32_t k = 0; k < uint32_t(c.x[1]); k++) handles.push_back(ld<uint64_t>(c.x[2] + 8 * k));
    auto captured = capture_command_lists(handles);
    static uint64_t submitted = 0, missing = 0;
    for (uint64_t handle : handles) {
        submitted++;
        auto l = captured->at(handle);
        if (l) lists.push_back(std::move(l));
        else if (++missing <= 5)
            log("nvn: submitted handle %llx is unknown", (unsigned long long)handle);
    }
    static const bool trace_lists = std::getenv("NX_NVN_LISTS") != nullptr;
    if (trace_lists && g_presents > 900 && g_presents < 905)
        for (uint32_t k = 0; k < uint32_t(c.x[1]); k++) {
            auto l = captured->at(handles[k]);
            log("nvn list: submit %llx handle %llu (%zu cmds)", (unsigned long long)c.x[0],
                (unsigned long long)ld<uint64_t>(c.x[2] + 8 * k), l ? l->cmds.size() : 0);
        }
    if ((submitted & 1023) == 0) log("nvn: %llu handles submitted, %llu unknown", (unsigned long long)submitted, (unsigned long long)missing);
    // NX_DEFERRED_SNAPSHOT: read the Deferred arrays here instead of when the submit is replayed. Required while the
    // replay runs on the worker thread (the game keeps writing its state after the submit returns), so it defaults on
    // for Bayonetta 3, where that replay thread is the default; NX_DEFERRED_SNAPSHOT=0 restores reading at replay time.
    static const bool snap = [] {
        const char* e = std::getenv("NX_DEFERRED_SNAPSHOT");
        if (e) return e[0] != '0';
#if defined(NX_BAYONETTA3)
        return true;
#else
        return false;
#endif
    }();
    auto deferred = snap ? std::make_shared<const DeferredValues>(deferred_snapshot(lists, captured)) : nullptr;
    {
        std::lock_guard lk(g_sub_lock);
        g_sub_called++;
    }
    // Arm before the worker can run or a CPU SyncWait can inspect the sync.
    // Completion is conservatively delayed to the containing submit's real GPU
    // fence, after all ordered writes have been made CPU-visible. Command-buffer
    // WaitSync is unsupported, so no later command in this submit waits on it.
    CommandFences fences;
    for (const auto& list : lists) arm_command_fences(*list, *captured, fences);
    if (extra_wait_timing() && !fences.empty()) {
        g_embedded_tickets.fetch_add(fences.size(), std::memory_order_relaxed);
        g_embedded_submits.fetch_add(1, std::memory_order_relaxed);
    }
    tl("submit-call");
    static std::atomic<uint64_t> trace_seq{0};
    const uint64_t trace_id = submit_trace_on() ? trace_seq.fetch_add(1, std::memory_order_relaxed) + 1 : 0;
    submit_trace("guest-enqueue", trace_id);
    // The embedded command fences are handed to the submit as completions instead of being completed here right
    // after queueing. Completing them here meant gpu_finish_fenced_submit() -> frame_wait_all() waited for the submit
    // that had just been queued, so recording never overlapped the GPU: on Adreno the frame carried a 17-22 ms fence
    // wait and the Pipelined line read "0 split", i.e. nothing was ever in flight together. frame_submit runs these
    // when the submit is retired, which is still after its fence signalled -- the same guarantee, without the stall.
    // NX_SYNC_FENCE_DEFER=0 restores the old behaviour (complete the fences on the replay thread, i.e. wait for the
    // submit just queued). Kept as the A/B arm in the same binary, because wall-clock driven runs land on different
    // scenes and are not comparable across binaries.
    static const bool defer_fences = [] {
        const char* e = std::getenv("NX_SYNC_FENCE_DEFER");
        return !(e && e[0] == '0');
    }();
    std::vector<std::function<void()>> completions;
    if (!defer_fences && !fences.empty()) {
        const uint64_t tail_start = submit_diag_on() ? plat::monotonic_ns() : 0;
        const bool tail_diag = submit_diag_on();
        completions.emplace_back([fences, tail_start, tail_diag, trace_id] {
            submit_trace("guest-fence-complete-begin", trace_id);
            complete_command_fences(fences);
            if (tail_diag) {
                g_fence_tail_calls.fetch_add(1, std::memory_order_relaxed);
                g_fence_tail_ns.fetch_add(plat::monotonic_ns() - tail_start, std::memory_order_relaxed);
            }
            submit_trace("guest-fences-published", trace_id);
        });
        // wait_done forces the submit to be retired before gpu_submit returns: same as the old inline path.
        std::vector<std::function<void()>> immediate = std::move(completions);
        completions.clear();
        worker_run([lists = std::move(lists), deferred, captured, immediate = std::move(immediate), trace_id] {
            tl("replay>");
            submit_trace("worker-begin", trace_id);
            gpu_submit(lists, deferred, captured);
            gpu_finish_fenced_submit();
            for (auto& fn : immediate) fn();
            submit_trace("replay-return", trace_id);
            tl("replay<");
            submit_done();
            submit_trace("worker-done", trace_id);
        });
        return;
    }
    // NX_FENCE_AT_CMD: each fence also passes where its command is in the replay (nvn_replay.cpp). The completion
    // below stays as the backstop for a command the replay never reached; completing twice is harmless.
    std::vector<std::function<void()>> hooks;
    if (fence_at_command())
        for (const auto& fence : fences)
            hooks.emplace_back([fence] {
                { std::lock_guard lock(fence->lock); fence->complete = true; }
                fence->cv.notify_all();
            });
    if (!fences.empty()) {
        const uint64_t tail_start = submit_diag_on() ? plat::monotonic_ns() : 0;
        const bool tail_diag = submit_diag_on();
        completions.emplace_back([fences = std::move(fences), tail_start, tail_diag, trace_id] {
            submit_trace("guest-fence-complete-begin", trace_id);
            complete_command_fences(fences);
            if (tail_diag) {
                g_fence_tail_calls.fetch_add(1, std::memory_order_relaxed);
                g_fence_tail_ns.fetch_add(plat::monotonic_ns() - tail_start, std::memory_order_relaxed);
            }
            submit_trace("guest-fences-published", trace_id);
        });
    }
    worker_run([lists = std::move(lists), deferred, captured, completions = std::move(completions), trace_id,
                hooks = std::move(hooks)]() mutable {
        tl("replay>");
        submit_trace("worker-begin", trace_id);
        replay_fence_hooks(hooks.empty() ? nullptr : &hooks);
        gpu_submit(lists, deferred, captured, std::move(completions));
        replay_fence_hooks(nullptr);
        submit_trace("replay-return", trace_id);
        tl("replay<");
        submit_done();
        submit_trace("worker-done", trace_id);
    });
    if (sub_timing) sub_sum_call += plat::monotonic_ns() - sub_t0;
    if (sub_timing && ++sub_n % 60 == 0) {
        // `call` is the guest-side wall time inside nvnQueueSubmitCommands, per frame: it is where the guest's command
        // recording runs, so it says whether recording serializes with the replay (a frame whose replay is 26 ms and
        // whose call is 20 ms leaves no room for three submits in flight) or not. It was never reset before, so it
        // printed a cumulative total (597.64 ms) and could not answer that.
        log("submit timing over 60: %.1f fps, period %.2f ms, guest-thread cpu %.2f ms, call %.2f ms",
            60.0e9 / double(std::max<uint64_t>(sub_sum_period, 1)), double(sub_sum_period) / 60 / 1e6,
            double(sub_sum_cpu) / 60 / 1e6, double(sub_sum_call) / 60 / 1e6);
        sub_sum_period = sub_sum_cpu = sub_sum_call = 0;
    }
}
void QueueFlush(CPU&) {}
void QueueFinish(CPU&) {
    wait_submits(submits_called(), kWaitFinish);
    worker_finish();
    gpu_wait(gpu_serial());
    gpu_wait(gpu_write_back_serial());  // NX_EARLY_COMPLETE
}
// (NVNqueue*, NVNsync*, NVNsyncCondition, int flags): every condition taken as "all previous work complete".
void QueueFenceSync(CPU& c) {
    uint64_t guest = c.x[1];  // Serial is taken after earlier replay jobs finish.
    auto ticket = std::make_shared<CommandFence>();
    ticket->seq = submits_called();
    Sync* identity = nullptr;
    {
        std::shared_lock lock(objects_lock());
        auto found = objects().find(guest);
        if (found == objects().end() || !(identity = dynamic_cast<Sync*>(found->second.get()))) return;
        identity->command_fence.store(ticket);
        identity->signaled = false;
        identity->serial = 0; identity->job = 0; identity->seq = ticket->seq;
    }
    uint64_t job = worker_run([guest, identity, ticket] {
        uint64_t serial = gpu_serial();
        {
            std::shared_lock lock(objects_lock());
            auto found = objects().find(guest);
            if (found != objects().end() && found->second.get() == identity &&
                identity->command_fence.load() == ticket) identity->serial = serial;
        }
        { std::lock_guard lock(ticket->lock); ticket->serial = serial; ticket->complete = true; }
        ticket->cv.notify_all();
    });
    // worker_run can run synchronously; never publish into a replacement Sync.
    std::shared_lock lock(objects_lock());
    auto found = objects().find(guest);
    if (found != objects().end() && found->second.get() == identity &&
        identity->command_fence.load() == ticket) identity->job = job;
}
void QueueWaitSync(CPU& c) { c.x[0] = 1; }  // NVNboolean (NVNqueue*, const NVNsync*): one queue, in order

// ---- windows ----------------------------------------------------------------------------------------------------

void WindowInitialize(CPU& c) {  // NVNboolean (NVNwindow*, const NVNwindowBuilder*)
    uint64_t guest = c.x[0], builder = c.x[1];
    auto& w = make<Window>(guest);
    int n = int(driver(c, "nvnWindowBuilderGetNumTextures", {builder}));
    for (int k = 0; k < n; k++) w.textures.push_back(driver(c, "nvnWindowBuilderGetTexture", {builder, uint64_t(k)}));
    w.active = int(driver(c, "nvnWindowBuilderGetNumActiveTextures", {builder}));
    if (w.active <= 0 || w.active > n) w.active = n;
    w.interval = int(driver(c, "nvnWindowBuilderGetPresentInterval", {builder}));
    log("nvn: window %llx: %d textures (%d active), present interval %d", (unsigned long long)guest, n, w.active, w.interval);
    c.x[0] = 1;
}

void WindowFinalize(CPU& c) { destroy(c.x[0]); }

// NVNwindowAcquireTextureResult (NVNwindow*, NVNsync* textureAvailableSync, int* textureIndex)
void acquire_wait(int active);  // the display clock's acquire side (defined with display_slot below)

void WindowAcquireTexture(CPU& c) {
    auto* w = get<Window>(c.x[0]);
    int index = 0;
    if (w && w->active > 0) {
        acquire_wait(w->active);  // the display's clock (display_slot): at most active-1 frames in flight
        index = w->next;
        w->next = (w->next + 1) % w->active;
    }
    if (c.x[2]) st<int32_t>(c.x[2], index);
    signal_sync(c.x[1]);
    c.x[0] = 0;
}

void WindowGetNumTextures(CPU& c) {
    auto* w = get<Window>(c.x[0]);
    c.x[0] = w ? w->textures.size() : 0;
}

void WindowGetNumActiveTextures(CPU& c) {
    auto* w = get<Window>(c.x[0]);
    c.x[0] = w ? uint64_t(w->active) : 0;
}

void WindowSetNumActiveTextures(CPU& c) {
    if (auto* w = get<Window>(c.x[0])) w->active = std::max(1, std::min(int(c.x[1]), int(w->textures.size())));
}

void WindowGetPresentInterval(CPU& c) {
    auto* w = get<Window>(c.x[0]);
    c.x[0] = w ? uint64_t(w->interval) : 1;
}

// NX_FPS_CAP=<fps> (the launcher's "Frame rate cap"): the lowest present interval the window accepts, 60/fps. Bayonetta
// 3's own frame rule lives in the sead framework's procFrame_ (main+e16ac), which this build does not instrument
// (NX_BAYONETTA3 is a separate build from the one frame_time_probe below belongs to), so neither that probe's
// NX_MAX_INTERVAL nor its governor exists here: the cap has to be applied where this build does see the interval,
// nvnWindowSetPresentInterval. The panel's clock follows it too (display_slot), so the frame time the game measures is
// the capped period. A value of 60 or an unset variable leaves the cap disabled.
uint32_t present_interval_floor() {
    static const uint32_t floor_interval = [] {
        const char* e = std::getenv("NX_FPS_CAP");
        const int fps = e ? std::atoi(e) : 0;
        // 60 is not a cap: the panel already refreshes at 60 (display_period_ns), so any interval above 1 is the game's
        // own 30 fps mode, not something this floor should hold it to. Returning 1 here made `capped` true, which hands
        // the panel's clock to the interval the game asks for -- and since the game asks for 2, "cap at 60" produced a
        // 33 ms panel period, i.e. 30 fps. Measured on Windows: NX_FPS_CAP=60 held 30.0 fps, unset held 59-60 fps.
        if (fps < 1 || fps >= 60) return 0u;
        // Round up, so a cap of 40 holds interval 2 (30 fps) rather than 1.5 (which cannot be asked for).
        const uint32_t iv = uint32_t((60 + fps - 1) / fps);
        log("nvn: NX_FPS_CAP=%s: present interval held at least %u (~%d fps)", e, iv, int(60 / iv));
        return iv;
    }();
    return floor_interval;
}

// Our governor over the game's (NX_GOVERNOR; on by default on Android unless NX_MAX_INTERVAL is set). The game's own
// rule (below) locks 30 fps after 3 frames over 15.83 ms; at 30 fps a phone lowers its clocks, every frame then takes
// longer, and it never leaves 30 (a Snapdragon 8 Gen 2 phone sat at 30 fps in course with frames that run at 60 when allowed).
// This one holds 60 (the game's maximum clamped to 1) while it is kept: it watches the main thread's loop period over
// 120-frame windows, and when two windows in a row had over 20% of their frames past 20 ms (the game would run
// visibly slow; one window is a scene load), it hands the choice back to the game (30 fps at the right speed) for a
// while, then tries 60 again, skipping the first 90 frames of a try (the clocks ramp up from the 30-fps level). The
// wait doubles after each failed try (10 s, 20 s, ... 80 s) and resets once a try holds. Returns 1 (clamp) or 0.
uint32_t governor_max(uint64_t now) {
    static const bool on = [] {
        const char* e = std::getenv("NX_GOVERNOR");
#if defined(__ANDROID__)
        return !(e && e[0] == '0');
#else
        return e && e[0] == '1';
#endif
    }();
    if (!on) return 0;
    static uint64_t last = 0, frames = 0, slow = 0, game_until = 0, warm_until = 0, backoff = 600, n = 0;
    static int bad_windows = 0, held_windows = 0;
    uint64_t period = last ? now - last : 0;
    last = now;
    n++;
    if (n < game_until) return 0;
    if (n == game_until) frames = slow = 0, bad_windows = 0, warm_until = n + 90;
    if (n < warm_until) return 1;
    frames++, slow += period > 20'000'000ull;
    if (frames < 120) return 1;
    bool held = slow * 5 <= frames;
    if (!held || held_windows++ % 30 == 0)
        log("nvn: governor: %llu of %llu frames over 20 ms%s", (unsigned long long)slow, (unsigned long long)frames,
            held ? ": 60 fps holds" : bad_windows ? ": back to the game's choice" : " (once: still 60)");
    frames = slow = 0;
    if (held) {
        bad_windows = 0, backoff = 600;
        return 1;
    }
    held_windows = 0;
    if (++bad_windows < 2) return 1;
    game_until = n + backoff;
    backoff = std::min<uint64_t>(backoff * 2, 4800);
    return 0;
}


#ifdef __linux__
// One line per 300 main-thread frames: wall time, CPU seconds used since the last line, % of one core, per thread.
static std::map<std::string, uint64_t> g_runq_wait, g_runq_prev;
static void cpu_log_tick() {
    static int n = 0;
    static uint64_t wall0 = 0;
    static std::map<std::string, uint64_t> prev, prev_sys;
    if (++n % 300) return;
    uint64_t now = plat::monotonic_ns();
    std::map<std::string, uint64_t> cur, cur_sys;
    if (DIR* d = opendir("/proc/self/task")) {
        while (dirent* e = readdir(d)) {
            if (e->d_name[0] == '.') continue;
            char path[96], buf[512];
            snprintf(path, sizeof path, "/proc/self/task/%s/stat", e->d_name);
            FILE* f = fopen(path, "r");
            if (!f) continue;
            size_t r = fread(buf, 1, sizeof buf - 1, f);
            fclose(f);
            buf[r] = 0;
            char* lp = strchr(buf, '(');
            char* rp = strrchr(buf, ')');
            if (!lp || !rp) continue;
            std::string name(lp + 1, rp);
            unsigned long ut = 0, st = 0;
            sscanf(rp + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &ut, &st);
            cur[name + "/" + e->d_name] = ut + st;
            cur_sys[name + "/" + e->d_name] = st;
            {  // schedstat: ns on a CPU, ns waiting on a run queue (ready but no CPU free), timeslices
                snprintf(path, sizeof path, "/proc/self/task/%s/schedstat", e->d_name);
                if (FILE* sf = fopen(path, "r")) {
                    unsigned long long run = 0, wait = 0, slices = 0;
                    if (fscanf(sf, "%llu %llu %llu", &run, &wait, &slices) == 3) g_runq_wait[name + "/" + e->d_name] = wait;
                    fclose(sf);
                }
            }
        }
        closedir(d);
    }
    double tick = 1.0 / double(sysconf(_SC_CLK_TCK));
    double dt = wall0 ? double(now - wall0) / 1e9 : 0;
    std::string line;
    double sum = 0;
    for (auto& [k, v] : cur) {
        double used = double(v - (prev.count(k) ? prev[k] : 0)) * tick;
        sum += used;
        if (dt > 0 && used / dt > 0.03) {
            char b[96];
            double sys = double(cur_sys[k] - (prev_sys.count(k) ? prev_sys[k] : 0)) * tick;
            double wq = double(g_runq_wait[k] - g_runq_prev[k]) / 1e9;
            snprintf(b, sizeof b, " %s=%.0f%%(sys %.0f, runq wait %.0f)", k.substr(0, k.find('/')).c_str(), 100 * used / dt, 100 * sys / dt, 100 * wq / dt);
            line += b;
        }
    }
    static uint64_t pt = 0, py = 0, ps = 0;
    uint64_t t_ = g_tick_calls, y_ = g_yield_calls, s_ = g_sleep_calls;
    if (dt > 0) log("cpu: per second: GetSystemTick %.0f, yield %.0f, SleepThread(ns>0) %.0f", double(t_ - pt) / dt, double(y_ - py) / dt, double(s_ - ps) / dt);
    pt = t_, py = y_, ps = s_;
    if (dt > 0) log("cpu: %.1f s wall, process %.0f%% of one core (%.2f cores);%s", dt, 100 * sum / dt, sum / dt, line.c_str());
    prev = cur;
    prev_sys = cur_sys;
    g_runq_prev = g_runq_wait;
    wall0 = now;
}
#else
static void cpu_log_tick() {}
#endif

// Wonder's sead::GameFramework::procFrame_ override (main+e15ec) picks the interval every frame from the main
// thread's last frame time, fw[0x118] (GetSystemTick ticks from the end of the previous present wait to the end of
// this frame's calc and draw recording): 2 when it reaches 15.83 ms, locked after 3 slow frames, back to 1 after 15
// fast ones (docs/nvn/DEBUG.md #43). From that call site (lr main+e16ac, x19 = the framework): a histogram of that
// time every 600 frames, and NX_MAX_INTERVAL=1 clamps the game's own maximum to 1 (fields 0x24c/0x264/0x268).
#if !defined(NX_BAYONETTA3)
void frame_time_probe(CPU& c) {
    const LoadedModule* m = module_at(c.x[30]);
    if (!m || c.x[30] - m->base != 0xe16ac || !c.x[19]) return;
    plat::perf_hint_thread();  // the game's main thread makes the frame too (ADPF, PLAN.md R2)
    plat::frame_thread_tick(1);
    {
        static thread_local bool marked = false;
        if (!marked) marked = true, svc_profile_mark(1);
        static uint64_t n = 0;
        if (++n % 600 == 0) svc_profile_report(600, 1);
    }
    // NX_FPS_CAP=30 (the launcher's "FPS limit"): the game is held at its own 30 fps mode, interval 2, always. The
    // game's logic steps by the interval it asked for, so the speed stays right; only the governors below are off.
    // 60 or unset: the game's own behaviour (60 when it can, 30 when heavy) plus our governor.
    static const bool cap30 = [] {
        const char* e = std::getenv("NX_FPS_CAP");
        bool on = e && std::atoi(e) > 0 && std::atoi(e) <= 30;
        if (on) log("nvn: NX_FPS_CAP=%s: present interval held at 2 (30 fps)", e);
        return on;
    }();
    if (cap30) {
        c.x[1] = 2;
        st<uint32_t>(c.x[19] + 0x264, 2);
        st<uint32_t>(c.x[19] + 0x268, 2);
    }
    static const char* max_env = std::getenv("NX_MAX_INTERVAL");
    static uint32_t game_max = 0;  // the game's own maximum (fw[0x24c]), restored when the governor lets go
    if (!game_max) game_max = ld<uint32_t>(c.x[19] + 0x24c);
    uint32_t max = cap30 ? 0 : max_env && std::atoi(max_env) >= 1 ? uint32_t(std::atoi(max_env)) : governor_max(plat::monotonic_ns());
    if (!max && !max_env && game_max) st<uint32_t>(c.x[19] + 0x24c, game_max);
    if (max >= 1) {
        st<uint32_t>(c.x[19] + 0x24c, max);
        if (ld<uint32_t>(c.x[19] + 0x268) > max) st<uint32_t>(c.x[19] + 0x268, max);
        if (ld<uint32_t>(c.x[19] + 0x264) > max) st<uint32_t>(c.x[19] + 0x264, max);
        if (int(c.x[1]) > int(max)) c.x[1] = max;
    }
    // NX_FW_PROBE=1: which words of the framework object count with the frames (study fps-unlock-1710).
    static const bool fwprobe = std::getenv("NX_FW_PROBE") != nullptr;
    if (fwprobe) {
        static uint32_t prev[0xc0] = {};
        static uint32_t changed[0xc0] = {}, sum[0xc0] = {};
        static int n = 0;
        for (int k = 0; k < 0xc0; k++) {
            uint32_t v = ld<uint32_t>(c.x[19] + 4 * k);
            if (v != prev[k]) changed[k]++, sum[k] += v - prev[k];
            prev[k] = v;
        }
        if (++n % 300 == 0) {
            for (int k = 0; k < 0xc0; k++)
                if (changed[k] > 150 && sum[k] < 100000)
                    log("fwprobe: fw[0x%x] changed %u/300, sum of deltas %u, now %u", 4 * k, changed[k], sum[k], prev[k]);
            memset(changed, 0, sizeof changed), memset(sum, 0, sizeof sum);
        }
        static uint64_t t0 = 0, ivsum = 0;
        if (!t0) t0 = plat::monotonic_ns();
        ivsum += uint32_t(c.x[1]);
        if (n % 300 == 0)
            log("fwprobe: %.2f s wall, %d frames, interval sum %llu, step %u", double(plat::monotonic_ns() - t0) / 1e9, n,
                (unsigned long long)ivsum, ld<uint32_t>(c.x[19] + 0x260));
    }
    // NX_CPU_LOG=1: process CPU use per 300 frames, total and by thread (Linux /proc), for the low-end study.
    static const bool cpulog = std::getenv("NX_CPU_LOG") != nullptr;
    if (cpulog) cpu_log_tick();
    static uint64_t bins[5] = {}, frames = 0;
    double ms = double(ld<uint64_t>(c.x[19] + 0x118)) / 19200.0;
    // A main-thread frame over 33 ms: what it waited on (PHASE3.md I1), its CPU time, faults and switches since the
    // last call, and how much of it a memory-guard read covered (PHASE4.md H0). NX_HITCH_LINES lines at most (300).
    static int hitches = 0;
    static const int max_hitches = std::getenv("NX_HITCH_LINES") ? std::atoi(std::getenv("NX_HITCH_LINES")) : 300;
    static uint64_t cpu_prev = 0;
    static long faults_prev[2] = {}, switches_prev[2] = {};
    uint64_t cpu = 0, now = plat::monotonic_ns();
    long faults[2], switches[2];
    plat::thread_counters(cpu, faults, switches);
    if (ms >= 33 && hitches < max_hitches && cpu_prev) {
        hitches++;
        uint64_t span = uint64_t(ms * 1e6);
        log("hitch: frame %llu, main thread %.1f ms (cpu %.1f), blocked: sync %.1f, finish %.1f, invalidate %.1f, "
            "finalize %.1f ms; faults %ld/%ld, switches %ld/%ld; guard read %.1f ms",
            (unsigned long long)g_frame.load(), ms, double(cpu - cpu_prev) / 1e6,
            double(g_wait_sites[0].frame_ns) / 1e6, double(g_wait_sites[1].frame_ns) / 1e6,
            double(g_wait_sites[2].frame_ns) / 1e6, double(g_wait_sites[3].frame_ns) / 1e6, faults[0] - faults_prev[0],
            faults[1] - faults_prev[1], switches[0] - switches_prev[0], switches[1] - switches_prev[1],
            double(plat::memguard_overlap_ns(now > span ? now - span : 0, now)) / 1e6);
    }
    cpu_prev = cpu;
    for (int k = 0; k < 2; k++) faults_prev[k] = faults[k], switches_prev[k] = switches[k];
    for (WaitSite& w : g_wait_sites) w.frame_ns = 0;
    bins[ms < 12 ? 0 : ms < 15.83 ? 1 : ms < 20 ? 2 : ms < 33 ? 3 : 4]++;
    if (++frames % 600 == 0) {
        log("nvn: main-thread frame time over 600 frames: <12 ms %llu, <15.8 %llu, <20 %llu, <33 %llu, more %llu",
            (unsigned long long)bins[0], (unsigned long long)bins[1], (unsigned long long)bins[2],
            (unsigned long long)bins[3], (unsigned long long)bins[4]);
        for (auto& b : bins) b = 0;
    }
}

#endif

void WindowSetPresentInterval(CPU& c) {
#if !defined(NX_BAYONETTA3)
    frame_time_probe(c);
#endif
    if (auto* w = get<Window>(c.x[0])) {
        int asked = int(c.x[1]);
        // A cap only ever lowers the frame rate: an interval the game asks for that is already slow enough is kept.
        const uint32_t floor_interval = present_interval_floor();
        const int interval = floor_interval && asked < int(floor_interval) ? int(floor_interval) : asked;
        static int logged = 0;
        if (w->interval != interval && logged++ < 50)
            log("nvn: window present interval %d -> %d (frame %llu)", w->interval, interval, (unsigned long long)g_frame.load());
        w->interval = interval;
    }
}

// (NVNqueue*, NVNwindow*, int textureIndex)
// Frame pacing is the display's clock, modelled where the console puts the wait: a present *queues* the frame into the
// next display slot and returns, and the following nvnWindowAcquireTexture waits for the slot its texture needs (the
// console's texture-available sync; ours is signalled at once). Pacing inside present instead -- what this used to do
// -- put the wait inside the frame time the game measures itself, so Bayonetta 3 read every frame as 33 ms, locked its
// 30 fps mode and never left it: measured here, paced at present the game sat at interval 2 for 579 of 600 presents,
// while unpaced it ran 50-112 fps and chose interval 1. NX_PACE_PRESENT=1 restores the old placement for bisecting.
// A late frame is not snapped to the next vsync (an 18 ms frame stays 18 ms, not 33). NX_NO_FRAME_LIMIT=1: unpaced.
constexpr int kDisplaySlots = 8;
uint64_t g_slot_time[kDisplaySlots];  // when each of the last presents reaches the display
uint64_t g_slots_presented = 0;
bool pace_at_present() {
    static const bool on = std::getenv("NX_PACE_PRESENT") != nullptr;
    return on;
}

void display_slot(int requested) {
    static const bool off = std::getenv("NX_NO_FRAME_LIMIT") != nullptr;
    // The display refreshes at its own rate, whatever the game asks for: a present takes the next display slot, so the
    // clock here is the panel's (NX_DISPLAY_HZ, 60 by default). NX_HONOR_PRESENT_INTERVAL=1 instead slows the display
    // clock to the requested interval, which is what this used to do: faithful to NVN, but it makes the frame time the
    // game measures equal to the period it asked for, so a game that dropped into its 30 fps mode never measures a
    // fast frame again -- measured with Bayonetta 3: 579 of 600 presents at interval >= 2 forever. Paced at the
    // panel's 60 Hz the same scene ran 55-60 fps and the game itself switched back to interval 1.
    static const bool honor_interval = std::getenv("NX_HONOR_PRESENT_INTERVAL") != nullptr;
    // A frame-rate cap (NX_FPS_CAP, present_interval_floor) is the one case where the panel's clock must follow the
    // interval too: holding the interval alone would leave the display at 60 Hz while the game stepped its logic by 2,
    // i.e. half speed. WindowSetPresentInterval has already raised `requested` to the cap's floor.
    const bool capped = present_interval_floor() != 0;
    const int interval = (honor_interval || capped) ? requested : 1;
    if (off || interval <= 0) return;
    static uint64_t presents = 0, at2 = 0;  // how often the game asks for 30 fps (interval 2), per 600 presents
    at2 += requested >= 2;  // the game's request, before the panel's clock replaces it (otherwise always 0)
    if (++presents % 600 == 0)
        log("nvn: %llu of the last 600 presents at interval >= 2", (unsigned long long)at2), at2 = 0, log_wait_sites();
    uint64_t now = plat::monotonic_ns();
    uint64_t period = uint64_t(interval) * display_period_ns();
    static uint64_t last = 0;
    uint64_t slot = (last > now || now - last < period ? last : now) + period;
    last = slot;
    if (pace_at_present()) {  // the old placement: block the present until its slot
        if (slot > now) plat::sleep_ns(int64_t(slot - now));
        return;
    }
    g_slot_time[g_slots_presented % kDisplaySlots] = slot;
    ++g_slots_presented;
}

// The console's texture-available sync: an acquire returns once a texture has been released by the display, so at most
// active-1 frames are in flight. Waiting for the slot of the oldest queued present is that rule in wall-clock terms.
void acquire_wait(int active) {
    static const bool off = std::getenv("NX_NO_FRAME_LIMIT") != nullptr;
    if (off || pace_at_present()) return;
    int in_flight = std::max(active, 1) - 1;  // the same rule as the console: the display retires a texture per slot
    if (in_flight <= 0) in_flight = 1;
    if (g_slots_presented < uint64_t(in_flight)) return;
    uint64_t slot = g_slot_time[(g_slots_presented - uint64_t(in_flight)) % kDisplaySlots];
    uint64_t now = plat::monotonic_ns();
    if (slot > now) {
        plat::sleep_ns(int64_t(slot - now));
        if (extra_wait_timing()) {
            g_acquire_sleeps.fetch_add(1, std::memory_order_relaxed);
            g_acquire_ns.fetch_add(plat::monotonic_ns() - now, std::memory_order_relaxed);
        }
    }
}

void QueuePresentTexture(CPU& c) {
    // The frame's work, from the end of the last present's pacing to now (perf_hint.cpp: ADPF on Android).
    static uint64_t work_from = 0;
    uint64_t t_in = plat::monotonic_ns();
    // NX_PRESENT_TIMING=1: where a guest frame's wall time goes. The game picks 30 fps from the frame time it measures
    // itself, so a frame budget has to be attributable: period between presents, this (guest) thread's CPU time, the
    // part of the call spent before pacing (the worker plus the texture it queued), and the pacing sleep.
    static const bool timing = std::getenv("NX_PRESENT_TIMING") != nullptr;
    static uint64_t t_prev = 0, cpu_prev = 0, sum_period = 0, sum_cpu = 0, sum_before = 0, sum_sleep = 0;
    static uint64_t sum_wait = 0, n_timing = 0, n_skipped = 0;
    uint64_t cpu_now = 0, t_before = 0, t_slept = 0;
    if (timing) {
        long f[2], sw[2];
        plat::thread_counters(cpu_now, f, sw);
    }
    tl("present>");
    {
        static thread_local bool marked = false;
        if (!marked) marked = true, svc_profile_mark(2);
        static uint64_t n = 0;
        if (++n % 600 == 0) svc_profile_report(600, 2);
    }
    plat::perf_hint_thread();
    if (work_from) plat::perf_hint_frame(int64_t(t_in - work_from));
    if (auto* w = get<Window>(c.x[1])) {
        if (timing) {  // the frame period as the guest sees it: between presents on this thread
            if (t_prev && cpu_prev) sum_period += t_in - t_prev, sum_cpu += cpu_now - cpu_prev;
            t_prev = t_in, cpu_prev = cpu_now;
        }
        // NX_PRESENT_QUEUE=<n> (default 2): how many presents the game may have queued ahead of the replay. Two held the
        // guest at most two frames ahead of the replay's *completion*, and in the Prologue battle the replay thread then
        // idled 9-12 ms of every heavy frame waiting for the next submit (it busy 34-43 ms of a 51-56 ms frame while the
        // guest's own CPU was ~12 ms) -- the guest was throttled, not the replay. Depth n lets the guest run n presents
        // ahead; the submit depth is NX_PIPELINED and the slot count is min(kMaxSlots=8, 2*frames) in nvn_frames.cpp, so
        // n = 3 (6 slots) and n = 4 (8 slots) are already covered, and asking for more is clamped rather than breaking
        // the ring: pressure still retires the oldest slot before its memory is reused, images/descriptors/pools are
        // held per slot until retired, and a submit's write-backs complete before the next replay starts.
        static const uint32_t depth = [] {
            const char* e = std::getenv("NX_PRESENT_QUEUE");
            return uint32_t(std::clamp(e ? std::atoi(e) : 2, 2, 4));
        }();
        static uint64_t queued[4] = {};
        static uint32_t next_slot = 0;
        uint64_t t0 = plat::monotonic_ns();
        worker_wait(queued[next_slot]);
        const uint64_t t_waited = plat::monotonic_ns() - t0;
        if (timing) t_before = t_waited;
        if (int(c.x[2]) >= 0 && size_t(c.x[2]) < w->textures.size()) {
            uint64_t texture = w->textures[size_t(c.x[2])];
            std::array<int32_t, 4> crop{w->crop[0], w->crop[1], w->crop[2], w->crop[3]};
            // Recording can run ahead of replay: take the guest sample before enqueueing this exact present.
            const uint64_t recorded = draw_frame_stats::enabled() ? recorded_draws() : 0;
            queued[next_slot] = worker_run([texture, crop, recorded] {
                if (draw_frame_stats::enabled()) draw_frame_stats::guest_sample() = recorded;
                gpu_present(texture, crop.data());
            });
            next_slot = (next_slot + 1) % depth;
            // The settings panel (F1) holds the game here, as a pause: this frame stays on screen and is presented
            // again every refresh, so the panel over it keeps up with its keys, until it closes.
            if (plat::settings_panel_open()) {
                log("settings panel: game held at its present");
                // Paced by a plain sleep, not display_slot: that is the game's vsync clock, and driving it from here
                // (it did not hold the rate either: 500-800 presents a second) left Bayonetta 3 presenting without
                // rendering once the panel closed ("no frame for 5 s"). The game's clock does not move while it waits.
                while (plat::settings_panel_open()) {
                    plat::PadState pad;  // the controller drives the panel even if the game's own polling waits too
                    plat::read_pad(pad);
                    std::this_thread::sleep_for(std::chrono::milliseconds(16));
                    worker_wait(queued[next_slot]);
                    queued[next_slot] = worker_run([texture, crop] { gpu_present(texture, crop.data()); });
                    next_slot = (next_slot + 1) % depth;
                }
                log("settings panel: game resumed");
            }
        }
        uint64_t t1 = plat::monotonic_ns();
        display_slot(w->interval);
        {  // The guest's present over 50 ms after its last: the game's own frame, or the wait for the replay/pacing.
            static uint64_t last_in = 0;
            static int lines = 0;
            const uint64_t t2 = plat::monotonic_ns();
            if (last_in && t_in - last_in > 50000000ull && lines < 200) {
                lines++;
                log("guest hitch: %.1f ms between the game's presents before present %llu; this call waited %.1f ms for "
                    "the replay, %.1f ms pacing (interval %d)", double(t_in - last_in) / 1e6,
                    (unsigned long long)g_presents + 1, double(t_waited) / 1e6, double(t2 - t1) / 1e6, w->interval);
            }
            last_in = t_in;
        }
        if (timing) {
            t_slept = plat::monotonic_ns() - t1;
            sum_before += t1 - t0;
            sum_sleep += t_slept;
            sum_wait += t_before;
            if (++n_timing % 60 == 0) {
                log("present timing over 60: %.1f fps, period %.2f ms, guest-thread cpu %.2f ms, in call %.2f ms "
                    "(worker wait %.2f), pacing sleep %.2f ms, interval %d, %llu presents without a window",
                    60.0e9 / double(std::max<uint64_t>(sum_period, 1)), double(sum_period) / 60 / 1e6,
                    double(sum_cpu) / 60 / 1e6, double(sum_before) / 60 / 1e6, double(sum_wait) / 60 / 1e6,
                    double(sum_sleep) / 60 / 1e6, w->interval, (unsigned long long)n_skipped);
                sum_period = sum_cpu = sum_before = sum_wait = sum_sleep = 0, n_skipped = 0;
            }
        }
    } else if (timing) {
        ++n_skipped;  // nvnQueuePresentTexture on a window this runtime never made (not the game's window)
    }
    work_from = plat::monotonic_ns();
    uint64_t n = ++g_presents;
    uw_scan(n);
    uw_cameras(n);
    nx::b3::film_grain_frame();
    tl("present<", n);
    if (g_tl_from != UINT64_MAX && n == g_tl_from + 8) {
        uint64_t cnt = std::min<uint64_t>(g_tl_n.load(), 2048), t0 = g_tl[0].ns;
        for (uint64_t k = 0; k < cnt; k++)
            log("timeline: %9.3f ms  t%lld  %s %llu", double(g_tl[k].ns - t0) / 1e6, (long long)g_tl[k].tid, g_tl[k].what,
                (unsigned long long)g_tl[k].arg);
    }
    if (n == 1 || (n & (n - 1)) == 0) log("nvn: present #%llu (texture %d)", (unsigned long long)n, int(c.x[2]));
    g_frame++;
}

// (NVNwindow*, int x, int y, int width, int height): the part of the window textures shown. In handheld mode
// (NX_HANDHELD) the game renders 1280x720 into its 1920x1080 window textures and crops to it.
void WindowSetCrop(CPU& c) {
    if (auto* w = get<Window>(c.x[0])) {
        int32_t v[4] = {int32_t(c.x[1]), int32_t(c.x[2]), int32_t(c.x[3]), int32_t(c.x[4])};
        uw_crop(v);  // NX_ULTRAWIDE
        if (std::memcmp(v, w->crop, sizeof v) != 0) {
            static int logged = 0;
            if (logged++ < 20) log("nvn: window crop %d,%d %dx%d", v[0], v[1], v[2], v[3]);
            std::memcpy(w->crop, v, sizeof v);
        }
    }
}

// B3 SDK+4b9170 writes four consecutive int32 values to NVNrectangle*.
void WindowGetCrop(CPU& c) {
    if (!c.x[1]) fatal("nvn: WindowGetCrop requires an output rectangle");
    std::shared_lock lk(objects_lock());
    auto it = objects().find(c.x[0]);
    auto* w = it == objects().end() ? nullptr : dynamic_cast<Window*>(it->second.get());
    if (!w) fatal("nvn: WindowGetCrop on an uninitialized window");
    for (uint32_t k = 0; k < 4; ++k) st<int32_t>(c.x[1] + k * 4, w->crop[k]);
}

}  // namespace

NX_HLE("nvnWindowGetCrop", WindowGetCrop);
NX_HLE("nvnWindowSetCrop", WindowSetCrop);
NX_HLE("nvnSyncInitialize", SyncInitialize);
NX_HLE("nvnSyncFinalize", SyncFinalize);
NX_HLE("nvnSyncWait", SyncWait);
NX_HLE("nvnQueueInitialize", QueueInitialize);
NX_HLE("nvnQueueFinalize", QueueFinalize);
NX_HLE("nvnQueueSubmitCommands", QueueSubmitCommands);
NX_HLE("nvnQueueFlush", QueueFlush);
NX_HLE("nvnQueueFinish", QueueFinish);
NX_HLE("nvnQueueFenceSync", QueueFenceSync);
NX_HLE("nvnQueueWaitSync", QueueWaitSync);
NX_HLE("nvnQueuePresentTexture", QueuePresentTexture);
NX_HLE("nvnWindowInitialize", WindowInitialize);
NX_HLE("nvnWindowFinalize", WindowFinalize);
NX_HLE("nvnWindowAcquireTexture", WindowAcquireTexture);
NX_HLE("nvnWindowGetNumTextures", WindowGetNumTextures);
NX_HLE("nvnWindowGetNumActiveTextures", WindowGetNumActiveTextures);
NX_HLE("nvnWindowSetNumActiveTextures", WindowSetNumActiveTextures);
NX_HLE("nvnWindowGetPresentInterval", WindowGetPresentInterval);
NX_HLE("nvnWindowSetPresentInterval", WindowSetPresentInterval);

}  // namespace nx::nvn
