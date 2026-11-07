// Submits that are not waited for (the design, and what stays synchronous and why:
// private/wonder/patches/async_submit.md). The synchronous path waits for every submit's fence before returning to
// the game, so the submitting thread stalls for the whole GPU time of each frame. Here a submit or a present takes one
// of a few slots (command buffers, fences, descriptor pool, the stretch of the upload ring its replay filled, the
// compute results to copy back) and returns once queued. A slot is retired (its fences waited, 2 s at most, as the
// synchronous path) when it is reused, when the ring needs its memory back, or when something waits for it.
//
// Replays copy everything the GPU reads from guest memory into the ring at submit time, so the game may change its
// memory while a submit runs. Only compute results flow back (write-backs). Two modes use the slots:
//
// NX_PIPELINED (the default since DEBUG.md #61; docs/archive/nvn/PERF-PLAN.md step 3): "pipelined fences". The
// replay and vkQueueSubmit stay on the submitting thread; the fence of the frame's work is not waited for. A submit
// with dispatches is split after its last DispatchCompute into two command buffers: the first part is queued at the
// split, while the rest is still being replayed, and only its fence is waited for, after the rest is queued. Its
// write-backs are copied before gpu_submit returns, so guest memory holds the compute results exactly when it does
// synchronously, and nothing the game waits on (nvnSyncWait, nvnQueueFinish, pool invalidation and finalization) has
// anything left to wait for: gpu_serial() is 0, as synchronously, and those calls never take the GPU lock. At most
// g_max_replays submits are in flight (the oldest is waited for before another is queued).
// NX_PIPELINED_NO_SPLIT=1: no split (a submit with write-backs is waited for whole).
//
// Chunks (docs/archive/nvn/android-perf/PLAN.md R1): a submit's replay is also queued in parts while it is recorded, at the
// first point with no render pass open after NX_SUBMIT_CHUNK_DRAWS draws (at twice that, a pass is ended for it), so
// the GPU starts on a frame a few milliseconds into its replay instead of at its end (or at the split). At most
// kMaxParts command buffers per submit; each begins with the barrier that orders it after everything queued before.
// Default 150 draws everywhere but Apple (MoltenVK encodes Metal inside vkQueueSubmit, so parts give nothing there and
// no Mac measurement exists); NX_SUBMIT_CHUNK_DRAWS=0 turns it off. It is the largest lever measured in the Prologue
// battle besides the composed-read cuts: the GPU/CPU serialization it removes was 15.5-17.2 ms of every frame.
// NX_SUBMIT_PARTS=9..16: optional larger per-submit part budget, with the split reservation reclaimed once no split
// remains. Unset, 8 and invalid values keep the original eight allocations and unconditional two-part reservation.
// This changes chunk boundaries only; queue order, barriers, fences and write-back retirement are unchanged.
//
// NX_ASYNC_GPU=<frames in flight> (the older experiment): a submit that has write-backs is retired before the next
// replay begins (it may read them) and before a wait on it returns; the game's waits retire slots under the GPU lock.
//
// Also the images of destroyed textures (freed once the submits that could use them are retired) are handled here, in
// every mode.
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <tuple>
#include <utility>
#include <vector>

#include "nvn_vk.h"
#include "nvn_submit_diag.h"
#include "nvn_image_retire.h"
#include "nvn_frame_parts.h"
#include "nvn_copy_behind.h"
#include "nvn_draw_state.h"  // replay::written_kept_invalidate_all (NX_P2_KEPT_COPY)
#include "platform.h"

namespace nx::nvn {

using namespace nx::vk;

VkQueue vk_queue();                // nvn_vk.cpp
std::recursive_mutex& gpu_lock();  // nvn_vk.cpp

namespace {

constexpr uint32_t kMaxSlots = 8;
constexpr uint32_t kMaxParts = kFrameMaxParts;  // physical capacity; default allocates only eight
uint32_t g_part_count = kFrameDefaultParts;
constexpr uint64_t kFenceTimeout = 2'000'000'000ull;  // a frame's work takes milliseconds; longer is a hung shader

struct Slot {
    VkCommandPool pool = VK_NULL_HANDLE;  // its own (the parts reset together), unless NX_CB_RESET=1
    bool pool_used = false;               // parts were begun since the pool's last reset
    VkCommandBuffer cbs[kMaxParts] = {};  // the parts, queued in order
    VkFence fences[kMaxParts] = {};
    uint32_t part = 0;                    // the part being recorded
    uint32_t queued = 0;                  // bit k: part k was queued (its fence will signal)
    uint32_t waited = 0;                  // parts below this were waited for (the ring may reuse their data)
    int split_part = -1;                  // pipelined: the part that ends with the last dispatch
    bool ok = true;                       // the current part is recording (a failed begin leaves nothing to queue)
    VkSemaphore acquired = VK_NULL_HANDLE;          // a present's swapchain image
    VkDescriptorPool descriptors = VK_NULL_HANDLE;  // made on first use (presents need none)
    bool descriptors_used = false;
    uint32_t sets_used = 0, descs_used[16] = {};  // allocated from the pool since its reset (peak logged by frame_log)
    std::atomic<bool> reset_done{true};  // the janitor reset its pools (nvn_janitor.cpp); frame_begin waits for it
    uint64_t serial = 0;    // in flight until retired; 0: free
    uint64_t ring_end = 0;  // the ring position after this submit's data
    uint64_t ring_begin = 0;  // where this submit's data starts (NX_CARRY)
    std::vector<WriteBack> write_backs;
    bool replay = false;  // a submit's replay (not a present)
    uint64_t binds_at_split = 0;  // replay::written_bind_count() at the split
    // replay::written_writable_bind_count() at the split: the binds that can still change a written copy's contents
    // after it. NX_REPLAY_CUTS=pipeline uses this instead of binds_at_split, so a later read-only bind of a written
    // range (which written_count_bind() counts, nvn_sets.cpp) no longer forces the whole submit to be waited for.
    uint64_t writable_at_split = 0;
    // replay::written_version() at the split: the exact form of the same question (see written_complete_since). A
    // written range re-bound as a GPU target after the split may have been filled by the later part, so its copy is
    // only complete once the whole submit is done; if nothing was re-bound, the first part already holds every result.
    uint64_t written_version_at_split = 0;
    uint64_t begin_ns = 0;
    // Guest-visible completions that must not run until this submit's GPU work is done (the embedded command fences
    // of nvn_queue.cpp's arm_command_fences). They used to run on the replay thread right after queueing, which meant
    // gpu_finish_fenced_submit() -> frame_wait_all() waited for the submit that had just been queued: every frame
    // serialised record with GPU, so the "3 submits in flight" never happened (on Adreno the Piplined line read
    // "60 submits, 0 split" and the fence wait held 17-22 ms of every frame). Deferring them to retire time keeps the
    // same guarantee -- they only fire once the fence has actually signalled -- without blocking the replay.
    std::vector<std::function<void()>> completions;
    bool carried = false;  // NX_CARRY: what is left of write_backs goes on in the next submit's copies
};

Slot g_slots[kMaxSlots];
bool g_carry_next = false;          // NX_CARRY: the next replay takes copies from the submit before it
uint64_t g_tail_hold = UINT64_MAX;  // NX_CARRY: the ring is in use from here on until this slot's submit ran its copies
Slot* g_tail_held_by = nullptr;
uint32_t g_slot_count = 0, g_next = 0;
Slot* g_current = nullptr;               // the slot being recorded
VkSemaphore g_rendered[kMaxSlots] = {};  // per swapchain image: signaled for its present
std::atomic<uint64_t> g_submitted{0}, g_completed{0};
std::atomic<uint64_t> g_landing{0};  // NX_EARLY_COMPLETE: the newest submit whose write-backs may still be on their way
std::atomic<uint64_t> g_barrier{0};  // NX_ASYNC_GPU: the last submit with write-backs, retired before the next replay
FrameTimes g_times;
struct SubmitDiag {
    uint64_t calls = 0, end_ns = 0, reset_ns = 0, queue_ns = 0, queue_max_ns = 0;
    uint64_t waits = 0, wait_ns = 0, writeback_ns = 0, join_ns = 0, reuse_join_ns = 0;
    uint64_t tail_calls = 0, tail_ns = 0;
} g_diag;
uint64_t g_trace_submit = 0;
uint32_t g_max_replays = 2;                       // pipelined: submits in flight
uint32_t g_plan_dispatches = 0, g_seen_dispatches = 0;  // pipelined: the submit's DispatchCompute commands, replayed
// Pipelined, per log period: submits, split ones, write-backs after the first part's fence / after the whole submit,
// waits for the oldest submit (two in flight), and how far into the replay the split came.
struct Stats {
    uint64_t submits = 0, splits = 0, waited_first = 0, waited_whole = 0, throttled = 0, first_ns = 0, replay_ns = 0, deferred = 0;
    uint64_t chunks = 0;
    uint64_t early = 0;  // NX_EARLY_COMPLETE: submits that released the guest when recorded
    uint64_t writable_after = 0;  // max writable binds recorded after a split, per split submit of the window
} g_stats;
// A command pool per slot, reset whole when the slot is taken again (PHASE3.md C2): resetting each part's command
// buffer was 6-9% of the replay thread on Adreno (it frees the driver's per-pass objects). NX_CB_RESET=1: as before.
bool g_slot_pools = false;
bool g_ring_host_cached = false;  // actual selected ring memory type, supplied by frame_init
uint64_t g_chunk_draws = 0;           // NX_SUBMIT_CHUNK_DRAWS (frame_init); 0: no chunks
bool g_chunking = false;              // this submit is chunked (frame_plan_split)
uint64_t g_chunk_from = UINT64_MAX;   // the draw count at the last part's start (UINT64_MAX: the next call's)
std::atomic<uint64_t> g_game_waits{0}, g_game_wait_ns{0};
uint32_t g_peak_sets = 0, g_peak_descs[16] = {};  // the most one slot's descriptor pool held in a submit, per log period
uint64_t g_staging_count = 0, g_staging_bytes = 0;  // staging_alloc's buffers, per log period (PHASE4.md U1)  // the game's waits that blocked (NX_ASYNC_GPU only)

// Polling is opt-in while the effects of letting guest threads run during replay are measured. The counter is per
// replay, not per draw: a list may contain many commands without drawing anything.
constexpr uint32_t kReplayPollCommands = 256;
uint32_t g_replay_poll_commands = 0;
bool g_replay_poll_failed = false, g_poll_images_pending = false;
struct ReplayPollStats {
    uint64_t checks = 0, pending = 0, retired = 0, writeback_blocked = 0, errors = 0;
} g_replay_poll_stats;

bool replay_fence_poll_on() {
    static const bool on = [] {
        const char* e = std::getenv("NX_REPLAY_FENCE_POLL");
        return e && e[0] == '1' && e[1] == '\0';
    }();
    return on;
}

// The upload ring (the synchronous path's upload buffer). Positions only grow; position p is byte p % size. What
// lies before g_tail belongs to retired submits; g_begin is where the submit being recorded started.
VkBuffer g_ring = VK_NULL_HANDLE;
uint8_t* g_ring_ptr = nullptr;
uint64_t g_ring_size = 0, g_head = 0, g_tail = 0, g_begin = 0;

// Images of destroyed textures, with the serial of the last submit that may still use them.
std::mutex g_grave_lock;
std::vector<std::pair<uint64_t, std::shared_ptr<GpuImage>>> g_grave;
// staging_alloc's buffers. A texture burst in a fight stages 200-300 MB in 60 frames, and a buffer of its own for each was
// an allocation, a mapping and a page fault per 4 KB of the first write, on the replay's path: 10 ms a frame on windows
// that spent 2 ms decoding. Retired ones (retired like the grave's) wait here for the next texture of about their size,
// up to NX_STAGING_POOL_MB, still mapped.
struct StagingBlock {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint8_t* ptr = nullptr;
    uint64_t size = 0;
    bool cached = false;
};
std::vector<std::pair<uint64_t, StagingBlock>> g_staging;
std::vector<StagingBlock> g_staging_free;
uint64_t g_staging_free_bytes = 0, g_staging_reused = 0;
uint64_t staging_pool_bytes() {
    static const uint64_t bytes = [] {
        const char* e = std::getenv("NX_STAGING_POOL_MB");
#if defined(__ANDROID__)
        const uint64_t fallback = 128;
#else
        const uint64_t fallback = 512;
#endif
        return (e ? uint64_t(std::strtoull(e, nullptr, 10)) : fallback) << 20;
    }();
    return bytes;
}

// GPU-written images of destroyed textures, kept a few submits for a new texture over the same memory with the same
// shape. Wonder re-creates ~180 NVNtexture objects per frame over the same render-target memory: each got a new
// image, and its first draw ended the render pass to set it up (most of the ~300 passes per frame, each reloading
// and storing its targets; docs/archive/nvn/DEBUG.md #34).
struct ReuseKey {
    uint64_t memory = 0;
    uint32_t format = 0, width = 0, height = 0, depth = 0, levels = 0, target = 0, samples = 0, swizzle = 0;
    bool operator<(const ReuseKey& o) const {
        return std::tie(memory, format, width, height, depth, levels, target, samples, swizzle) <
               std::tie(o.memory, o.format, o.width, o.height, o.depth, o.levels, o.target, o.samples, o.swizzle);
    }
};
ReuseKey reuse_key(const Texture& t) {
    uint32_t swizzle = uint32_t(t.swizzle[0]) | uint32_t(t.swizzle[1]) << 8 | uint32_t(t.swizzle[2]) << 16 |
                  uint32_t(t.swizzle[3]) << 24;
    static const bool key_swizzle = std::getenv("NX_KEY_SWIZZLE") != nullptr;  // as image_of's key (#46)
    if (!key_swizzle) swizzle = 0;
    return {t.memory, t.format, t.width, t.height, t.depth, t.levels, t.target, t.samples, swizzle};
}
std::map<ReuseKey, std::pair<uint64_t, std::shared_ptr<GpuImage>>> g_reusable;  // key -> (kept at, image)
uint64_t g_reuse_clock = 0, g_reused = 0;

// A failed reset, recording end, or resource creation cannot be followed by a safe submit or slot reuse. In
// particular an old, still-signaled fence must never stand in for a newly submitted part.
void require_vk_success(VkResult r, const char* operation) {
    if (r == VK_SUCCESS) return;
    log("nvn/vk: %s failed (%d)", operation, int(r));
    gpu_hung(operation);
}

void wait_fences(const VkFence* fences, uint32_t n) {
    if (!n) return;
    submit_trace("gpu-wait-begin", n);
    uint64_t t0 = plat::monotonic_ns();
    VkResult r = p_vkWaitForFences(vk_device(), n, fences, VK_TRUE, kFenceTimeout);
    if (submit_diag_on()) { ++g_diag.waits; g_diag.wait_ns += plat::monotonic_ns() - t0; }
    submit_trace("gpu-wait-return", n);
    if (r == VK_TIMEOUT) gpu_hung("a submit did not finish in 2 s");
    if (r != VK_SUCCESS) {
        log("nvn/vk: vkWaitForFences failed (%d)", int(r));
        gpu_hung("vkWaitForFences failed");
    }
    g_times.fence_ns += plat::monotonic_ns() - t0;
}

// Compute results to guest memory: after the fence of the part that holds the submit's dispatches. The ranges are
// independent and the submitting thread waits for all of them: nvn_written.cpp copies them on NX_COPY_THREADS helpers
// (the NaN trace and the whole-range learning stay serial there).
void apply_write_backs(Slot& s) {
    if (s.write_backs.empty()) return;
    replay::copy_behind::wait();  // a queued copy has to read guest memory as it was before these
    submit_trace("writeback-begin", s.serial);
    const uint64_t start = submit_diag_on() ? plat::monotonic_ns() : 0;
    write_backs_apply(s.write_backs);
    if (submit_diag_on()) g_diag.writeback_ns += plat::monotonic_ns() - start;
    submit_trace("writeback-end", s.serial);
}

// Waits for the oldest submit in flight and finishes it: compute results to guest memory (if not there yet), its
// descriptor pool and ring stretch free again, images destroyed before it freed. False when nothing is in flight.
// Fences complete in submission order on the one queue, so retiring oldest first keeps write-backs in order.
// The fences of the queued parts below `upto`.
uint32_t queued_fences(const Slot& s, uint32_t upto, VkFence* out) {
    uint32_t n = 0;
    for (uint32_t k = 0; k < upto && k < g_part_count; k++)
        if (s.queued >> k & 1) out[n++] = s.fences[k];
    return n;
}

// Return only fully submitted slots: the slot being recorded has no serial yet. Both blocking retirement and replay
// polling use this same FIFO selection so that a newer submit cannot overtake an older submit's write-backs.
Slot* oldest_slot() {
    Slot* s = nullptr;
    for (uint32_t k = 0; k < g_slot_count; k++)
        if (g_slots[k].serial && (!s || g_slots[k].serial < s->serial)) s = &g_slots[k];
    return s;
}

// The caller has established that every queued part completed, either by waiting or polling. A pool reset queued to
// the janitor is safe to publish here: it is exclusive to this slot, and frame_begin joins it before any reuse.
void carry_check_finish();  // NX_CARRY_CHECK

void finish_retire(Slot& s, bool defer_images) {
    gpu_busy_retired(uint32_t(&s - g_slots), s.queued);  // NX_GPU_PROFILE=3
    if (s.carried) carry_check_finish();
    apply_write_backs(s);
    if (janitor_on()) {
        s.reset_done.store(false, std::memory_order_relaxed);
        janitor_run([slot = &s] {
            if (g_slot_pools && slot->pool_used)
                require_vk_success(p_vkResetCommandPool(vk_device(), slot->pool, 0), "vkResetCommandPool");
            if (slot->descriptors_used)
                require_vk_success(p_vkResetDescriptorPool(vk_device(), slot->descriptors, 0), "vkResetDescriptorPool");
            slot->pool_used = slot->descriptors_used = false;
            slot->sets_used = 0, std::fill(std::begin(slot->descs_used), std::end(slot->descs_used), 0u);
            slot->reset_done.store(true, std::memory_order_release);
        });
    } else {
        if (s.descriptors_used)
            require_vk_success(p_vkResetDescriptorPool(vk_device(), s.descriptors, 0), "vkResetDescriptorPool");
        s.descriptors_used = false;
        s.sets_used = 0, std::fill(std::begin(s.descs_used), std::end(s.descs_used), 0u);
    }
    if (&s == g_tail_held_by) g_tail_hold = UINT64_MAX, g_tail_held_by = nullptr;
    g_tail = std::max(g_tail, std::min(s.ring_end, g_tail_hold));
    // A replay may currently hold raw image and view pointers. Even though its allocations are tagged for the next
    // serial, avoid changing the shared image/view caches in the middle of replay. Clear the graveyard at the next
    // frame boundary instead. Pipelined mode already clears it at its normal submit/present boundaries.
    if (!pipelined()) {
        if (defer_images) g_poll_images_pending = true;
        else free_retired_images(s.serial), g_poll_images_pending = false;
    }
    // Write-backs and slot cleanup must precede the guest-visible ticket. In particular, GPU fence completion alone
    // does not make a deferred host write-back visible to the guest.
    if (!s.completions.empty()) {
        std::vector<std::function<void()>> done = std::move(s.completions);
        s.completions.clear();
        for (auto& fn : done) fn();
    }
    g_completed = s.serial;
    s.serial = 0;
    s.carried = false;
}

bool retire_oldest() {
    Slot* s = oldest_slot();
    if (!s) return false;
    VkFence fences[kMaxParts];
    const uint32_t n = queued_fences(*s, kMaxParts, fences);
    if (!n) gpu_hung("retiring a submit without queued fences");
    wait_fences(fences, n);
    finish_retire(*s, false);
    return true;
}

// Called only under the GPU lock while a different slot is being recorded. There may be outstanding write-backs in
// NX_DEFER_WRITEBACK mode: applying one part-way through another replay can change its guest-memory snapshots. Leave
// such a FIFO head to the existing safe wait/retirement paths, and never skip it to retire a newer submit.
void poll_oldest_ready() {
    if (g_replay_poll_failed) return;
    Slot* s = oldest_slot();
    if (!s || s == g_current) return;
    if (!s->write_backs.empty()) {
        ++g_replay_poll_stats.writeback_blocked;
        return;
    }
    if (!p_vkGetFenceStatus || !s->queued || (s->queued >> g_part_count)) {
        log("nvn/vk: replay fence polling disabled: missing vkGetFenceStatus or invalid queued parts");
        g_replay_poll_failed = true;
        ++g_replay_poll_stats.errors;
        return;
    }
    ++g_replay_poll_stats.checks;
    bool ready = true;
    for (uint32_t k = 0; k < g_part_count; k++) {
        if (!(s->queued & (1u << k))) continue;
        if (!s->fences[k]) {
            log("nvn/vk: replay fence polling disabled: null queued fence");
            g_replay_poll_failed = true;
            ++g_replay_poll_stats.errors;
            return;
        }
        const VkResult r = p_vkGetFenceStatus(vk_device(), s->fences[k]);
        if (r == VK_ERROR_DEVICE_LOST) gpu_hung("vkGetFenceStatus");
        if (r == VK_NOT_READY) ready = false;
        else if (r != VK_SUCCESS) {
            log("nvn/vk: vkGetFenceStatus failed (%d); replay polling disabled", int(r));
            g_replay_poll_failed = true;
            ++g_replay_poll_stats.errors;
            return;
        }
    }
    if (!ready) {
        ++g_replay_poll_stats.pending;
        return;
    }
    finish_retire(*s, true);
    ++g_replay_poll_stats.retired;
}

void retire_through(uint64_t serial) {
    while (g_completed < serial && retire_oldest()) {
    }
}

bool any_carried() {
    for (uint32_t k = 0; k < g_slot_count; k++)
        if (g_slots[k].serial && g_slots[k].carried) return true;
    return false;
}

// NX_CARRY_CHECK=1: the proof of the rule written_carry_begin splits by. What landed before the submit finished must not
// change in its copy while the later parts run: a difference is a range that landed without those parts' writes.
struct CarryCheck {
    const uint8_t* from;
    uint8_t* to;
    std::vector<uint8_t> bytes;
};
std::vector<CarryCheck> g_carry_check;
uint64_t g_carry_check_ranges = 0, g_carry_check_bytes = 0, g_carry_check_changed = 0, g_carry_check_submits = 0;
bool carry_check() {
    static const bool on = std::getenv("NX_CARRY_CHECK") != nullptr;
    return on;
}

void carry_check_finish() {
    if (!carry_check()) return;
    for (const CarryCheck& c : g_carry_check) {
        g_carry_check_ranges++, g_carry_check_bytes += c.bytes.size();
        if (std::memcmp(c.from, c.bytes.data(), c.bytes.size()) == 0) continue;
        if (g_carry_check_changed++ < 32)
            log("nvn: carry check: %llu bytes at %p changed after they landed", (unsigned long long)c.bytes.size(),
                static_cast<void*>(c.to));
    }
    g_carry_check.clear();
    if (++g_carry_check_submits % 300 == 0)
        log("nvn: carry check: %llu submits, %llu ranges and %.1f MB landed early, %llu changed later",
            (unsigned long long)g_carry_check_submits, (unsigned long long)g_carry_check_ranges,
            double(g_carry_check_bytes) / 1e6, (unsigned long long)g_carry_check_changed);
}

bool carry_on() {
    static const bool on = [] {
        const char* e = std::getenv("NX_CARRY");
        return !(e && e[0] == '0');
    }();
    return on && early_complete() && pipelined();
}

bool fences_passed(const Slot& s, uint32_t upto) {
    VkFence fences[kMaxParts];
    const uint32_t n = queued_fences(s, upto, fences);
    for (uint32_t k = 0; k < n; k++)
        if (p_vkGetFenceStatus(vk_device(), fences[k]) != VK_SUCCESS) return false;
    return true;
}

// On by default wherever NX_EARLY_COMPLETE is; NX_CARRY=0: off. The replay reads what the last submit wrote, so it used to wait for that submit's last part (its fence wait:
// 6-15 ms of a phone's 36-50 ms frame, while the GPU is busy for 17 of them). Nearly all of those bytes were ready at the
// split, 3% into the submit, and a few ranges written by fragment shaders after it are small: 131 KB in the fight. Those
// are copied on the GPU, behind the last part, into this submit's own copies of them, and the rest lands now.
void land_or_carry() {
    const uint64_t last = g_landing;
    for (Slot* s = oldest_slot(); s && s->serial < last; s = oldest_slot()) retire_oldest();
    Slot* s = oldest_slot();
    if (!s || s->serial != last) return;
    if (s->carried || s->split_part < 0 || !(s->queued >> s->split_part & 1) ||
        fences_passed(*s, kMaxParts)) return retire_through(last);
    std::vector<WriteBack> land_now;
    if (!replay::written_carry_begin(s->write_backs, s->writable_at_split, land_now)) return retire_through(last);
    s->carried = true;
    g_carry_next = true;
    g_tail_hold = s->ring_begin;  // the copies read the old ones: not free until they ran, whatever else retires
    VkFence fences[kMaxParts];
    wait_fences(fences, queued_fences(*s, uint32_t(s->split_part) + 1, fences));
    replay::copy_behind::wait();  // a queued copy has to read guest memory as it was before these
    if (carry_check())
        for (const WriteBack& w : land_now)
            g_carry_check.push_back({w.from, w.to, std::vector<uint8_t>(w.from, w.from + w.size)});
    write_backs_apply(land_now);
    if (plat::late_guard_on()) replay::late_guard_arm(s->write_backs);  // the landing opened every page
}

uint32_t replays_in_flight() {
    uint32_t n = 0;
    for (uint32_t k = 0; k < g_slot_count; k++) n += g_slots[k].serial && g_slots[k].replay;
    return n;
}

bool begin_part(VkCommandBuffer cb) {
    if (!g_slot_pools)
        require_vk_success(p_vkResetCommandBuffer(cb, 0), "vkResetCommandBuffer");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (p_vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) {
        log("nvn/vk: vkBeginCommandBuffer failed");
        return false;
    }
    // Earlier submits (and this submit's first part) may still run: their writes come before anything here (image
    // layouts are tracked in submission order, so the images already are where these commands expect them).
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0,
                           nullptr, 0, nullptr);
    return true;
}

void end_part(VkCommandBuffer cb) {
    uint64_t t0 = plat::monotonic_ns();
    // The host reads compute results after the fence.
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0,
                           nullptr);
    const bool diag = submit_diag_on();
    const uint64_t end_start = diag ? plat::monotonic_ns() : 0;
    require_vk_success(p_vkEndCommandBuffer(cb), "vkEndCommandBuffer");
    if (diag) g_diag.end_ns += plat::monotonic_ns() - end_start;
    g_times.encode_ns += plat::monotonic_ns() - t0;
}

// Queues an ended `cb` (Metal encoding happens inside vkQueueSubmit: MoltenVK's synchronous submits). False: nothing
// was queued, `fence` will not signal.
bool queue_part(VkCommandBuffer cb, VkFence fence, VkSemaphore wait, VkSemaphore signal) {
    uint64_t t0 = plat::monotonic_ns();
    const bool diag = submit_diag_on();
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    if (wait) {
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &wait;
        si.pWaitDstStageMask = &stage;
    }
    if (signal) {
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &signal;
    }
    const uint64_t reset_start = diag ? plat::monotonic_ns() : 0;
    require_vk_success(p_vkResetFences(vk_device(), 1, &fence), "vkResetFences");
    if (diag) g_diag.reset_ns += plat::monotonic_ns() - reset_start;
    const uint64_t trace_id = submit_trace_on() ? ++g_trace_submit : 0;
    submit_trace("queue-submit-begin", trace_id);
    const uint64_t queue_start = diag ? plat::monotonic_ns() : 0;
    VkResult r = p_vkQueueSubmit(vk_queue(), 1, &si, fence);
    if (diag) {
        const uint64_t ns = plat::monotonic_ns() - queue_start;
        ++g_diag.calls; g_diag.queue_ns += ns;
        g_diag.queue_max_ns = std::max(g_diag.queue_max_ns, ns);
    }
    submit_trace("queue-submit-end", trace_id);
    g_times.encode_ns += plat::monotonic_ns() - t0;
    if (r == VK_ERROR_DEVICE_LOST) gpu_hung("vkQueueSubmit");
    if (r != VK_SUCCESS) log("nvn/vk: vkQueueSubmit failed (%d)", int(r));
    return r == VK_SUCCESS;
}

// As replay_list walks it. A list's own commands are scanned once (a recorded list does not change): the walk over
// every command of the frame took ~4% of the replay thread on a Snapdragon 8 Gen 2.
uint32_t count_dispatches(const CmdList& list, int depth) {
    auto& l = const_cast<CmdList&>(list);
    if (l.scanned_size != l.cmds.size()) {
        l.own_dispatches = 0, l.calls.clear();
        for (const Cmd& c : l.cmds) {
            if (c.op == Cmd::DispatchCompute) l.own_dispatches++;
            if (c.op == Cmd::CallList) l.calls.push_back(c.a[0]);
        }
        l.scanned_size = l.cmds.size();
    }
    uint32_t n = l.own_dispatches;
    if (depth < 8)
        for (uint64_t h : l.calls)
            if (auto called = submit_command_list(h)) n += count_dispatches(*called, depth + 1);
    return n;
}

int env_int(const char* name) {
    const char* e = std::getenv(name);
    return e ? std::atoi(e) : 0;
}

// Parts of the slot being recorded that ended ahead of their ring bytes (nvn_copy_behind.h), oldest first. The replay
// thread goes on recording instead of waiting at each part: the first part alone, with the two large compute ranges,
// held it up about 5 ms a frame. Only the slot being recorded has any: frame_submit queues them ahead of its last part.
struct ParkedPart {
    Slot* slot;
    uint32_t part;
    replay::copy_behind::Ticket ticket;
};
std::vector<ParkedPart> g_parked;
size_t g_parked_first = 0;

void parked_queue_ready() {
    for (; g_parked_first < g_parked.size(); g_parked_first++) {
        const ParkedPart& p = g_parked[g_parked_first];
        if (!replay::copy_behind::reached(p.ticket)) return;
        if (queue_part(p.slot->cbs[p.part], p.slot->fences[p.part], VK_NULL_HANDLE, VK_NULL_HANDLE))
            p.slot->queued |= 1u << p.part;
    }
    g_parked.clear(), g_parked_first = 0;
}

void parked_queue_all() {
    replay::copy_behind::wait_all();
    parked_queue_ready();
}

// Ends the part being recorded (and its render pass; pending clears stay pending, as they are not tied to a command
// buffer: the next pass on their targets takes them as load operations), queues it once its ring bytes are there, and
// begins the next one.
VkCommandBuffer next_part(Slot& s) {
    pass_end_only(s.cbs[s.part], 0);
    gpu_busy_part_end(uint32_t(&s - g_slots), s.part, s.cbs[s.part]);
    if (s.ok) {
        end_part(s.cbs[s.part]);
        g_parked.push_back({&s, s.part, replay::copy_behind::ticket()});
        parked_queue_ready();
    }
    s.part++;
    s.ok = begin_part(s.cbs[s.part]);
    if (s.ok) gpu_busy_part_begin(uint32_t(&s - g_slots), s.part, s.cbs[s.part]);
    if (!s.ok) log("nvn/vk: a part of a submit could not begin: it is lost");
    g_chunk_from = UINT64_MAX;
    return s.cbs[s.part];
}

}  // namespace

// On by default on macOS only (course 1-1 headless: ~50 -> ~59 fps there; DEBUG.md #61): MoltenVK encodes Metal inside
// vkQueueSubmit, which is what the overlap hides. With native drivers (Windows, NVIDIA) the synchronous path played
// at 58 fps against 40 and had none of the exploded meshes the pipelined one shows in course 1-1 (DEBUG-2 #76).
// NX_PIPELINED=0: synchronous; 1..4: pipelined, that many submits in flight (2 at least). NX_ASYNC_GPU wins.
bool pipelined() {
    static const bool on = [] {
        if (env_int("NX_ASYNC_GPU") > 0) return false;
        const char* e = std::getenv("NX_PIPELINED");
#if defined(__APPLE__)
        return !e || std::atoi(e) > 0;
#else
        // An integrated GPU shares the CPU's memory and is the long side of a frame: with the fences not waited for,
        // the Intel UHD went from 26.2 to 31.3 fps docked and 37.8 to 40.1 at 720p in the Flower Kingdom intro
        // (docs/archive/nvn/studies/intel-igpu-1710.md). Pipelined is the default there; NX_PIPELINED=0 turns it off.
        if (!e) return gpu_is_integrated();
        return std::atoi(e) > 0;
#endif
    }();
    return on;
}

bool async_gpu() {
    static const bool on = pipelined() || env_int("NX_ASYNC_GPU") > 0;
    return on;
}

FrameTimes& frame_times() { return g_times; }
uint64_t frame_completed() { return g_completed; }
uint32_t frame_part_count() { return g_part_count; }

void frame_init(VkCommandPool pool, uint32_t family, VkBuffer ring, uint8_t* ring_ptr, uint64_t ring_size,
                bool ring_host_cached) {
    // NX_ASYNC_GPU=1 (or 2): two frames in flight; 3 or 4: that many. A frame is a submit and a present, a slot each.
    // NX_PIPELINED likewise (at most that many submits in flight, whatever the slots).
    // NX_PIPELINED=<n> (default 3): submits in flight. Measured in the Prologue battle (rc4 + NX_PIPELINED=3): the
    // replay thread's starvation fell from 9-12 ms to 6-7 ms per heavy frame with the fence already at ~4 ms, so the
    // guest may run one submit further ahead. NX_PIPELINED=0/2 keep their meaning, and the non-pipelined platforms
    // (discrete GPU, Apple) keep 2.
    uint32_t frames = uint32_t(std::clamp(
        pipelined() && !std::getenv("NX_PIPELINED") ? 3 : env_int(pipelined() ? "NX_PIPELINED" : "NX_ASYNC_GPU"), 2, 4));
    g_max_replays = frames;
    g_slot_count = std::min(kMaxSlots, frames * 2);
    g_part_count = frame_parts_from_env(std::getenv("NX_SUBMIT_PARTS"));
    // NX_EARLY_COMPLETE: the time the replay thread would have spent on a submit's fence and write-backs is what it spends
    // waiting for the guest's next submit anyway.
    if (early_complete() && pipelined()) worker_set_idle([] { gpu_wait(gpu_write_back_serial()); });
    if (g_part_count > kFrameDefaultParts)
        log("nvn/vk: submit part cap %u, reclaiming the split reservation after use (NX_SUBMIT_PARTS)", g_part_count);
    VkDevice d = vk_device();
    g_slot_pools = std::getenv("NX_CB_RESET") == nullptr;
    for (uint32_t k = 0; k < g_slot_count; k++) {
        if (g_slot_pools) {
            VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            pci.queueFamilyIndex = family;
            // Every slot must use the same reset strategy. Falling back after allocating any buffers from
            // per-slot pools would later reset those buffers individually without the required pool flag.
            require_vk_success(p_vkCreateCommandPool(d, &pci, nullptr, &g_slots[k].pool), "vkCreateCommandPool");
        }
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = g_slot_pools ? g_slots[k].pool : pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = g_part_count;
        require_vk_success(p_vkAllocateCommandBuffers(d, &cai, g_slots[k].cbs), "vkAllocateCommandBuffers");
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        for (uint32_t p = 0; p < g_part_count; p++)
            require_vk_success(p_vkCreateFence(d, &fci, nullptr, &g_slots[k].fences[p]), "vkCreateFence");
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        require_vk_success(p_vkCreateSemaphore(d, &sci, nullptr, &g_slots[k].acquired), "vkCreateSemaphore");
    }
    g_ring = ring, g_ring_ptr = ring_ptr, g_ring_size = ring_size;
    g_ring_host_cached = ring_host_cached;
#if defined(__APPLE__)
    // Default 150 draws per part, as on RADV. This used to be off with the note "MoltenVK encodes Metal inside each
    // vkQueueSubmit, so extra submits buy nothing there and no Mac measurement exists" -- the per-submit cost is real
    // but it is not what the parts are for: a part is queued as soon as it is recorded, so the GPU starts on the first
    // 150 draws instead of waiting for the whole frame's ~280. Measured on the M4 / MoltenVK in the Prologue battle
    // (NX_GPU_PROFILE=3, three runs, 700 s, the pinned save): fps 15.9 -> 18.3 / 18.9 / 18.9 and the GPU's own
    // span-minus-busy gap 12.38 -> 5.29 / 5.49 / 5.49 ms, utilisation 67% -> 77-79%, 0 FATAL, 0 CRASH, and the fight
    // renders correctly (frames/14341.ppm is VERSE 1 with its HUD and enemies). NX_SUBMIT_CHUNK_DRAWS=0 restores the
    // single-command-buffer behaviour.
    g_chunk_draws = std::getenv("NX_SUBMIT_CHUNK_DRAWS") ? uint64_t(env_int("NX_SUBMIT_CHUNK_DRAWS")) : 150;
#else
    // Default 150 draws per part. Measured on RADV/VANGHOG in the Prologue battle (rc3, clean arm): the whole-submit
    // wait fell 15.5-17.2 -> 2.8-3.3 ms and the fight went 15.6 -> 19-22.5 fps with ~5 chunks / 7 parts per frame;
    // NX_SUBMIT_CHUNK_DRAWS=0 restores the single-command-buffer behaviour.
    g_chunk_draws = std::getenv("NX_SUBMIT_CHUNK_DRAWS") ? uint64_t(env_int("NX_SUBMIT_CHUNK_DRAWS")) : 150;
#endif
    if (g_chunk_draws) log("nvn/vk: submits queued in parts every %llu draws (NX_SUBMIT_CHUNK_DRAWS)",
                           (unsigned long long)g_chunk_draws);
    if (pipelined())
        log("nvn/vk: pipelined fences (NX_PIPELINED): %u submits in flight, %u slots", frames, g_slot_count);
    else
        log("nvn/vk: asynchronous submits (NX_ASYNC_GPU): %u frames in flight, %u slots", frames, g_slot_count);
}

VkCommandBuffer frame_begin(bool replay) {
    // Replay polling leaves image/view caches untouched until the current replay has ended.
    if (g_poll_images_pending) {
        free_retired_images(g_completed);
        g_poll_images_pending = false;
    }
    g_replay_poll_commands = 0;
    // NX_EARLY_COMPLETE: a replay reads them from the guest, and within its first millisecond. The present between two
    // replays reads none of it, so it goes ahead and overlaps the wait.
    if (replay && g_landing > g_completed) carry_on() ? land_or_carry() : retire_through(g_landing);
    // NX_ASYNC_GPU: this replay may read the results of the last submit that had write-backs. Pipelined: they are
    // in guest memory since that submit returned.
    if (!pipelined() && replay && g_barrier > g_completed) retire_through(g_barrier);
    Slot& s = g_slots[g_next];
    if (s.serial) retire_through(s.serial);
    const uint64_t join_start = submit_diag_on() ? plat::monotonic_ns() : 0;
    janitor_wait(s.reset_done);
    if (submit_diag_on()) g_diag.reuse_join_ns += plat::monotonic_ns() - join_start;
    if (g_slot_pools && s.pool_used)  // none of its parts is in flight
        require_vk_success(p_vkResetCommandPool(vk_device(), s.pool, 0), "vkResetCommandPool");
    s.pool_used = true;
    g_next = (g_next + 1) % g_slot_count;
    g_begin = g_head;
    s.ring_begin = g_begin;
    s.replay = replay, s.part = s.queued = s.waited = 0, s.split_part = -1, s.ok = true;
    if (replay && std::exchange(g_carry_next, false)) g_tail_held_by = &s;
    s.begin_ns = plat::monotonic_ns();
    g_plan_dispatches = g_seen_dispatches = 0;
    g_chunking = false;
    if (!begin_part(s.cbs[0])) {
        g_current = nullptr;
        return VK_NULL_HANDLE;
    }
    g_current = &s;
    gpu_busy_part_begin(uint32_t(&s - g_slots), 0, s.cbs[0]);  // NX_GPU_PROFILE=3
    return s.cbs[0];
}

// ---- pipelined: the split after the last dispatch ----------------------------------------------------------------

uint64_t g_diag_no_dispatches = 0;

size_t frame_plan_split(const std::vector<std::shared_ptr<CmdList>>& lists) {
    static const bool off = std::getenv("NX_PIPELINED_NO_SPLIT") != nullptr;
    g_plan_dispatches = g_seen_dispatches = 0;
    g_chunking = g_chunk_draws && g_current, g_chunk_from = UINT64_MAX;  // not with the profile (not called then)
    // NX_COMPUTE_SEPARATE's write-backs are not counted by written_bind_count(): such a submit is waited for whole.
    if (!pipelined() || off || !g_current || replay::written_separate()) return SIZE_MAX;
    static const bool plan_diag = std::getenv("NX_SPLIT_DIAG") != nullptr;
    size_t last = SIZE_MAX;
    for (size_t k = 0; k < lists.size(); k++)
        if (uint32_t n = count_dispatches(*lists[k], 0)) g_plan_dispatches += n, last = k;
    if (plan_diag && last == SIZE_MAX) g_diag_no_dispatches++;
    return last;
}

namespace {
uint64_t g_split_diag_n = 0, g_split_diag_writable = 0, g_split_diag_all = 0;
}  // namespace

void split_diag_log() {
    if (g_diag_no_dispatches)
        log("nvn/vk: split diag: %llu submits had NO dispatch at all, so no split point exists (their write-backs come "
            "from transfers, not from compute, and there is no earlier part to wait for instead of the whole submit)",
            (unsigned long long)g_diag_no_dispatches);
    g_diag_no_dispatches = 0;
    if (!g_split_diag_n) return;
    log("nvn/vk: split diag: the fast path failed on %llu submits, %.0f writable binds and %.0f all binds after the "
        "split per submit (a split is placed at the last dispatch; any later write of a bound range needs the whole "
        "submit waited for, because only then are the compute results complete)",
        (unsigned long long)g_split_diag_n, double(g_split_diag_writable) / double(g_split_diag_n),
        double(g_split_diag_all) / double(g_split_diag_n));
    g_split_diag_n = g_split_diag_writable = g_split_diag_all = 0;
}

VkCommandBuffer frame_cb(VkCommandBuffer cb) { return g_current ? g_current->cbs[g_current->part] : cb; }

VkCommandBuffer frame_split(VkCommandBuffer cb) {
    Slot* s = g_current;
    if (!pipelined() || !s || s->split_part >= 0 || cb != s->cbs[s->part] || s->part + 1 >= g_part_count)
        return frame_cb(cb);
    s->split_part = int(s->part);
    s->binds_at_split = replay::written_bind_count();
    s->writable_at_split = replay::written_writable_bind_count();
    s->written_version_at_split = replay::written_version();
    g_stats.splits++, g_stats.first_ns += plat::monotonic_ns() - s->begin_ns;
    return next_part(*s);  // frame_submit reports a part that never queued
}

VkCommandBuffer frame_chunk(VkCommandBuffer cb, uint64_t draws, bool pass_open) {
    Slot* s = g_current;
    if (g_parked_first < g_parked.size()) parked_queue_ready();
    if (s && cb == s->cbs[s->part] && replay_fence_poll_on() &&
        ++g_replay_poll_commands == kReplayPollCommands) {
        g_replay_poll_commands = 0;
        poll_oldest_ready();
    }
    if (!g_chunking || !s || cb != s->cbs[s->part]) return cb;
    const bool split_pending = s->split_part < 0 && g_seen_dispatches < g_plan_dispatches;
    if (!frame_chunk_has_room(s->part, g_part_count, split_pending)) return cb;
    if (g_chunk_from == UINT64_MAX) g_chunk_from = draws;
    uint64_t since = draws - g_chunk_from;
    if (since < g_chunk_draws || (pass_open && since < 2 * g_chunk_draws)) return cb;
    g_stats.chunks++;
    return next_part(*s);
}

VkCommandBuffer frame_after_dispatch(VkCommandBuffer cb) {
    if (!g_plan_dispatches || ++g_seen_dispatches != g_plan_dispatches) return frame_cb(cb);
    return frame_split(cb);
}

VkSemaphore frame_acquire_semaphore() {  // the next slot's (frame_begin takes that slot)
    Slot& s = g_slots[g_next];
    if (s.serial) retire_through(s.serial);  // its last present waited on it
    return s.acquired;
}

VkSemaphore frame_rendered_semaphore(uint32_t image) {
    if (image >= kMaxSlots) image = kMaxSlots - 1;
    if (!g_rendered[image]) {
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        p_vkCreateSemaphore(vk_device(), &sci, nullptr, &g_rendered[image]);
    }
    return g_rendered[image];
}

void frame_submit(VkSemaphore wait, VkSemaphore signal, std::vector<WriteBack> write_backs, bool wait_done,
                  std::vector<std::function<void()>> completions) {
    if (!g_current) return;
    Slot& s = *g_current;
    g_current = nullptr;
    if (pipelined() && s.replay) {
        g_stats.submits++;
        if (s.split_part >= 0) g_stats.replay_ns += plat::monotonic_ns() - s.begin_ns;
    }
    if (s.ok) gpu_busy_part_end(uint32_t(&s - g_slots), s.part, s.cbs[s.part]);
    parked_queue_all();  // what the last part reads from the ring, and the parts ahead of it
    if (s.ok) end_part(s.cbs[s.part]);
    const bool has_last = s.ok && queue_part(s.cbs[s.part], s.fences[s.part], wait, signal);
    if (has_last) s.queued |= 1u << s.part;
    // A queued prefix alone does not represent the whole guest submit. If any part failed, retiring its successful
    // fences must not signal a ticket for commands that never ran (including when polling is disabled).
    const uint32_t all_parts = (1u << (s.part + 1)) - 1;
    if (s.queued != all_parts) gpu_hung("a submit part did not queue");
    s.serial = ++g_submitted;
    s.ring_end = g_head;
    s.write_backs = std::move(write_backs);
    s.completions = std::move(completions);
    // The dispatches were in a part that never ran: the buffers hold no results, and copying them back would put the
    // inputs (or older data) over guest memory.
    const bool has_first = s.split_part >= 0 && (s.queued >> s.split_part & 1);
    if (s.split_part >= 0 && !has_first) s.write_backs.clear();
    if (pipelined()) {
        // At most g_max_replays submits in flight, this one included: the oldest are waited for once this one is
        // queued (before, the rest waited behind them and the GPU idled between the two parts).
        struct Throttle {
            Slot& s;
            ~Throttle() {
                if (!s.replay) return;
                bool waited = false;
                while (replays_in_flight() > g_max_replays && retire_oldest()) waited = true;
                g_stats.throttled += waited;
            }
        } throttle{s};
        // Debug features read this submit's results right after it: all of it is waited for. Otherwise only compute
        // results are: they are in the first part when the split came after the last dispatch that wrote storage.
        // NX_REPLAY_CUTS=pipeline: the part with the dispatches carries every GPU write the write-back needs, so a bind
        // after the split only keeps us from waiting for the first part when its shader can still write the range
        // (written_bind). A read-only bind of a written range cannot change a copy's contents and no longer blocks the
        // overlap. Without the cut the old guard (no bind at all) stands.
        static const bool defer_writeback = [] {
            // OPT-IN, and deliberately so. Deferring the write-backs removes the replay thread's stall (fence wait
            // 6.7-6.9 -> 0.0 ms) but the wait reappears on the GUEST thread instead, which then sits on its own command
            // ticket until the submit retires: a controlled six-run A/B from the fight checkpoint measured 47.8 vs
            // 47.6 fps (25-30 MB windows, n=94 vs 92) and 47.7 vs 49.1 fps (30-45 MB, n=69 vs 73) -- no gain. The
            // earlier +19 % reading came from runs that reached different moments of the fight, which is exactly the
            // comparison this file's own notes warn about. The mechanism is kept because the deferred completion path
            // is what makes an overlap possible at all; the gain has to come from the split fast path below.
            const char* e = std::getenv("NX_DEFER_WRITEBACK");
            return e && e[0] == '1';
        }();
        const bool split_guard = (replay::replay_cuts() & replay::kCutSplitGuard) != 0;
        // NX_SPLIT_EXACT=1: ask the question in its exact form -- "is every write-back range complete in the first
        // part?" -- instead of "was any writable bind recorded after the split?". The count form is a proxy that a real
        // fight always fails (~336 writable binds per submit after the last dispatch), because a bind of an unrelated
        // range is counted the same as a write to a range that has a pending write-back.
        static const bool split_exact = [] {
            const char* e = std::getenv("NX_SPLIT_EXACT");
            return e && e[0] == '1';
        }();
        const bool split_covers_writes = split_exact && s.split_part >= 0
                                             ? replay::written_complete_since(s.written_version_at_split)
                                             : split_guard ? replay::written_writable_bind_count() == s.writable_at_split
                                                           : replay::written_bind_count() == s.binds_at_split;
        if (s.split_part >= 0)
            g_stats.writable_after =
                std::max(g_stats.writable_after, replay::written_writable_bind_count() - s.writable_at_split);
        // NX_SPLIT_DIAG=1: why the fast path cannot be taken. A split is placed at the last dispatch, so anything that
        // writes a bound range after it makes the first part insufficient to copy the compute results back.
        static const bool split_diag = std::getenv("NX_SPLIT_DIAG") != nullptr;
        if (split_diag && s.split_part >= 0 && !split_covers_writes)
            g_split_diag_n++, g_split_diag_writable += replay::written_writable_bind_count() - s.writable_at_split,
                g_split_diag_all += replay::written_bind_count() - s.binds_at_split;
        // NX_EARLY_COMPLETE=1: the guest waits on a fence of the submit it has just made, every frame, and that wait is
        // what made the frame record + GPU tail + write-backs + guest logic one after the other (24-29 ms of a 36 ms frame
        // on the guest's render thread in the fight). Its reason is the bytes: every one this submit reads from the guest
        // is in the ring by now (parked_queue_all above), so the guest is free to change them, and the fence is passed
        // here. The GPU's results land behind it (late: a word the guest has changed since stays the guest's), when the
        // replay thread goes idle (frame_init) and at the latest before anything is replayed (frame_begin). What the
        // guest loses is reading a compute result right after its fence. Witch-Time keeps the same ranges out of guest
        // memory altogether and trapped every access to them: none, on four stages of this game. An explicit CPU read,
        // the memory pool's invalidate or nvnQueueFinish, still waits for them (gpu_write_back_serial).
        if (early_complete() && !wait_done && has_last && write_backs_can_land_late(s.write_backs)) {
            for (WriteBack& w : s.write_backs) w.late = true;
            if (!s.write_backs.empty()) g_landing = s.serial;
            if (plat::late_guard_on()) replay::late_guard_arm(s.write_backs);
            std::vector<std::function<void()>> done = std::move(s.completions);
            s.completions.clear();
            for (auto& fn : done) fn();
            g_stats.early++;
            return;
        }
        if (wait_done) {
            retire_through(s.serial);
        } else if (!s.write_backs.empty()) {
            if (has_first && has_last && split_covers_writes) {
                VkFence fences[kMaxParts];
                wait_fences(fences, queued_fences(s, uint32_t(s.split_part) + 1, fences));
                apply_write_backs(s);
                // The rest of this submit is still in flight: a copy kept from submit to submit (NX_P2_KEPT_COPY) is
                // not safe to reuse until it has been re-filled from guest memory.
                replay::written_kept_invalidate_all();
                g_stats.waited_first++;
            } else if (defer_writeback && !s.completions.empty()) {
                // NX_DEFER_WRITEBACK=1: the submit is left in flight. Its write-backs and the guest-visible completions
                // run when it is retired -- by the next submission, by the throttle, or by a guest thread blocked on one
                // of its command fences (frame_retire_one). That is what turns `record + GPU` into `max(record, GPU)`:
                // the game is guaranteed to fence for compute results (it arms one command fence per FenceSync, 600
                // tickets per 600 submits), so nothing reads them before that retire.
                
                g_stats.deferred++;
            } else {
                retire_through(s.serial);
                g_stats.waited_whole++;
            }
        }
        return;
    }
    if (!s.write_backs.empty()) g_barrier = s.serial;
    // NX_ASYNC_COMPUTE_WAIT=1: submits with compute results are waited for at once, as synchronously (the game then
    // finds them in its memory right after nvnQueueSubmitCommands, without waiting on a sync).
    static const bool compute_wait = std::getenv("NX_ASYNC_COMPUTE_WAIT") != nullptr;
    if (wait_done || (compute_wait && !s.write_backs.empty())) retire_through(s.serial);
}

// A guest thread blocked on one of those deferred command fences must not simply sleep: the only other retire trigger
// is the next submission, and the guest cannot submit while it waits. So it drives retirement itself -- wait the oldest
// submit's fences, run its completions (which is what releases this thread), then look again.
bool frame_retire_one() {
    // NX_EARLY_COMPLETE: the replay completes the ticket, and retiring here only fights it for the GPU lock it holds for
    // the whole recording: the ticket passed at the end of recording, and the guest still came out after the write-backs.
    if (early_complete()) return true;
    std::lock_guard lk(gpu_lock());
    return retire_oldest();
}

void frame_wait_all() {
    retire_through(g_submitted);
    if (g_poll_images_pending && !g_current) {
        free_retired_images(g_completed);
        g_poll_images_pending = false;
    }
    const uint64_t start = submit_diag_on() ? plat::monotonic_ns() : 0;
    submit_trace("janitor-join-begin");
    for (uint32_t k = 0; k < g_slot_count; k++) janitor_wait(g_slots[k].reset_done);
    if (submit_diag_on()) g_diag.join_ns += plat::monotonic_ns() - start;
    submit_trace("janitor-join-end");
}

void frame_finish_fenced() {
    const uint64_t start = submit_diag_on() ? plat::monotonic_ns() : 0;
    submit_trace("guest-fence-tail-begin");
    frame_wait_all();
    if (submit_diag_on()) { ++g_diag.tail_calls; g_diag.tail_ns += plat::monotonic_ns() - start; }
    submit_trace("guest-fence-tail-end");
}

uint64_t frame_upload_used() { return g_head - g_begin; }
uint64_t frame_ring_size() { return async_gpu() ? g_ring_size : 0; }

void frame_log(uint64_t frames) {
    if (submit_diag_on()) {
        const auto& d = g_diag;
        auto ms = [frames](uint64_t ns) { return double(ns) / 1e6 / double(frames); };
        log("nvn/submit-diag: async API over %llu frames: calls %llu end %.3f reset %.3f queue %.3f ms/frame "
            "queue-max %.3f ms; all waits %llu %.3f writeback %.3f drain-janitor-join %.3f ms/frame; "
            "embedded-tail %llu %.3f ms/frame (tail overlaps waits/writeback/drain-join); reuse-janitor-join %.3f ms/frame",
            (unsigned long long)frames, (unsigned long long)d.calls, ms(d.end_ns), ms(d.reset_ns), ms(d.queue_ns),
            double(d.queue_max_ns) / 1e6, (unsigned long long)d.waits, ms(d.wait_ns), ms(d.writeback_ns), ms(d.join_ns),
            (unsigned long long)d.tail_calls, ms(d.tail_ns), ms(d.reuse_join_ns));
        g_diag = {};
    }
    gpu_busy_log(frames);
    if (uint64_t waits = g_game_waits.exchange(0))
        log("nvn/vk: the game's waits (syncs, finish, pool invalidate/finalize): %llu blocked, %.2f ms per frame",
            (unsigned long long)waits, double(g_game_wait_ns.exchange(0)) / 1e6 / double(frames));
    if (g_peak_sets) {
        log("nvn/vk: descriptor pool peak per submit: %u sets, ubo %u, ubo dynamic %u, ssbo %u, sampler %u, storage image %u, "
            "texel %u+%u (pool: 32768 sets, 65536 / 65536 / 16384 / 65536 / 4096 / 1024+1024)",
            g_peak_sets, g_peak_descs[VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER], g_peak_descs[VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC],
            g_peak_descs[VK_DESCRIPTOR_TYPE_STORAGE_BUFFER], g_peak_descs[VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER],
            g_peak_descs[VK_DESCRIPTOR_TYPE_STORAGE_IMAGE], g_peak_descs[VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER],
            g_peak_descs[VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER]);
        g_peak_sets = 0, std::fill(std::begin(g_peak_descs), std::end(g_peak_descs), 0u);
    }
    if (g_staging_count)
        log("nvn/vk: staging: %llu buffers (%llu reused, %.0f MB free), %.1f MB over %llu frames",
            (unsigned long long)g_staging_count, (unsigned long long)g_staging_reused, double(g_staging_free_bytes) / 1e6,
            double(g_staging_bytes) / 1e6, (unsigned long long)frames);
    upload_cost_log(frames);
    g_staging_count = g_staging_bytes = g_staging_reused = 0;
    if (replay_fence_poll_on()) {
        const auto& p = g_replay_poll_stats;
        log("nvn/vk: replay fence polling: %llu checks, %llu not ready, %llu retired, %llu blocked by write-back, "
            "%llu errors%s (NX_REPLAY_FENCE_POLL)",
            (unsigned long long)p.checks, (unsigned long long)p.pending, (unsigned long long)p.retired,
            (unsigned long long)p.writeback_blocked, (unsigned long long)p.errors,
            g_replay_poll_failed ? ", disabled" : "");
        g_replay_poll_stats = {};
    }
    if (!pipelined() || !g_stats.submits) return;
    const Stats& t = g_stats;
    log("nvn/vk: pipelined: %llu submits, %llu split (first part: %.0f%% of the replay), write-backs after the first "
        "part %llu, after the whole submit %llu, %llu deferred (NX_DEFER_WRITEBACK), %llu waited for the oldest in "
        "flight, %llu chunks queued early, max %llu writable binds after a split, %llu released early",
        (unsigned long long)t.submits, (unsigned long long)t.splits,
        t.replay_ns ? 100.0 * double(t.first_ns) / double(t.replay_ns) : 0.0, (unsigned long long)t.waited_first,
        (unsigned long long)t.waited_whole, (unsigned long long)t.deferred, (unsigned long long)t.throttled,
        (unsigned long long)t.chunks, (unsigned long long)t.writable_after,
        (unsigned long long)t.early);
    g_stats = {};
    split_diag_log();
}

Upload frame_upload_alloc(uint64_t size, uint64_t align) {
    uint64_t at = (g_head + align - 1) & ~(align - 1);
    if (at % g_ring_size + size > g_ring_size) at += g_ring_size - at % g_ring_size;  // never split: the next lap
    while (at + size > g_tail + g_ring_size)  // the oldest submits' data is still read: wait for them
        if (!retire_oldest()) {
            parked_queue_all();  // queued copies land before this lap writes over them
            // The submit's parts queued so far may still read its data: wait for them first.
            if (g_current && g_current->waited < g_current->part) {
                VkFence fences[kMaxParts];
                wait_fences(fences, queued_fences(*g_current, g_current->part, fences));
                g_current->waited = g_current->part;
            }
            log("nvn/vk: upload buffer full (%llu bytes in this submit)", (unsigned long long)(at + size - g_begin));
            g_tail = at + size - g_ring_size;  // this submit wraps over its own first data, as the synchronous path
            replay::dedup_reset();
            break;
        }
    g_head = at + size;
    uint64_t off = at % g_ring_size;
    return {g_ring, off, g_ring_ptr + off, false, g_ring_host_cached};
}

VkDescriptorSet frame_descriptor_set(VkDescriptorSetLayout layout) {
    if (!g_current) return VK_NULL_HANDLE;
    Slot& s = *g_current;
    if (!s.descriptors) s.descriptors = make_descriptor_pool();
    s.descriptors_used = true;
    {
        uint32_t c[16] = {};
        descriptor_layout_counts(layout, c);
        s.sets_used++;
        g_peak_sets = std::max(g_peak_sets, s.sets_used);
        for (int t = 0; t < 16; t++) s.descs_used[t] += c[t], g_peak_descs[t] = std::max(g_peak_descs[t], s.descs_used[t]);
    }
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = s.descriptors;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (p_vkAllocateDescriptorSets(vk_device(), &ai, &set) != VK_SUCCESS) log("nvn/vk: descriptor pool exhausted");
    return set;
}

// nvn.h: what nvnQueueFenceSync records and nvnSyncWait / nvnQueueFinish wait for. The synchronous path has nothing
// in flight when the game gets control back: 0, and no wait. Pipelined submits are in flight, but nothing the game
// can observe depends on them: compute results are in guest memory when gpu_submit returns, the GPU reads its inputs
// from the ring (never from guest memory), and images, descriptor pools and ring stretches are kept until their
// submit is retired. So it is 0 there too, and those calls neither wait nor take the GPU lock.
uint64_t gpu_serial() { return async_gpu() && !pipelined() ? g_submitted.load() : 0; }
void frame_carry_wait() {
    while (any_carried() && retire_oldest()) {
    }
}

uint64_t gpu_write_back_serial() {
    if (!async_gpu()) return 0;
    return pipelined() ? g_landing.load() : g_barrier.load();
}

bool gpu_wait(uint64_t serial) {
    if (!serial || g_completed >= serial) return false;  // pipelined: only ever a late write-back's serial
    uint64_t t0 = plat::monotonic_ns();
    std::lock_guard lk(gpu_lock());
    if (g_completed < serial) retire_through(serial);
    g_game_waits++, g_game_wait_ns += plat::monotonic_ns() - t0;
    return true;
}

void free_retired_images(uint64_t completed) {
    std::vector<std::shared_ptr<GpuImage>> dead;
    {
        std::lock_guard lk(g_grave_lock);
        g_reuse_clock++;  // once per submit and present
        for (auto it = g_reusable.begin(); it != g_reusable.end();)
            if (it->second.first + 8 < g_reuse_clock) {
                g_grave.push_back({g_submitted + 1, std::move(it->second.second)});
                it = g_reusable.erase(it);
            } else {
                ++it;
            }
        if (g_reuse_clock % 3600 == 0 && g_reused) log("nvn/vk: %llu images reused", (unsigned long long)g_reused);
        auto done = std::stable_partition(g_grave.begin(), g_grave.end(),
                                          [&](const auto& e) { return e.first > completed; });
        for (auto it = done; it != g_grave.end(); ++it) dead.push_back(std::move(it->second));
        g_grave.erase(done, g_grave.end());
    }
    std::vector<StagingBlock> staging;
    {
        std::lock_guard lk(g_grave_lock);
        auto done = std::stable_partition(g_staging.begin(), g_staging.end(),
                                          [&](const auto& e) { return e.first > completed; });
        for (auto it = done; it != g_staging.end(); ++it) {
            if (g_staging_free_bytes + it->second.size <= staging_pool_bytes()) {
                g_staging_free_bytes += it->second.size;
                g_staging_free.push_back(it->second);
            } else {
                staging.push_back(it->second);
            }
        }
        g_staging.erase(done, g_staging.end());
    }
    for (const StagingBlock& b : staging)
        p_vkDestroyBuffer(vk_device(), b.buffer, nullptr), p_vkFreeMemory(vk_device(), b.memory, nullptr);
    for (auto& img : dead) {
        if (!img || !img->image) continue;
        if (img.use_count() > 1) {
            // Drop this completed reference now: another entry in `dead` may
            // own the last reference and must be able to free the Vulkan image.
            img.reset();
            continue;
        }
        forget_image_views(*img);
        if (img->view) p_vkDestroyImageView(vk_device(), img->view, nullptr);
        p_vkDestroyImage(vk_device(), img->image, nullptr);
        if (img->memory) p_vkFreeMemory(vk_device(), img->memory, nullptr);
        img->image = VK_NULL_HANDLE, img->memory = VK_NULL_HANDLE, img->view = VK_NULL_HANDLE;
    }
}

void retire_image(std::shared_ptr<GpuImage> image) {
    if (!image) return;
    std::lock_guard lk(g_grave_lock);
    // Include the replay being recorded, just as Texture::~Texture does. Never
    // reuse an explicitly expired snapshot or free its handles on this thread.
    g_grave.push_back({g_submitted + 1, std::move(image)});
}

// Images of destroyed textures are freed once no submit can use them: the one being recorded now included (a replay
// on another thread may hold it), hence the serial after the last submitted. Before the graveyard no image was ever
// freed: the opening creates textures continuously and the process reached 85 GB (the machine froze).
Texture::~Texture() {
    if (watched) plat::write_watch_remove(memory);
    if (!image) return;
    std::lock_guard lk(g_grave_lock);
    if (image->rendered && image->image && memory) {
        auto& slot = g_reusable[reuse_key(*this)];
        if (slot.second) g_grave.push_back({g_submitted + 1, std::move(slot.second)});
        slot = {g_reuse_clock, std::move(image)};
        return;
    }
    g_grave.push_back({g_submitted + 1, std::move(image)});
}

// A host-visible buffer of its own for one big texture upload: the ring is 256 MB, and a scene load with ASTC decoded
// to RGBA8 staged 312 MB in one submit, wrapping over its own data before the GPU had copied it (DEBUG-2 #72). Freed
// once the submit being recorded is retired (as images in the grave). Falls back to the ring if allocation fails.
Upload staging_alloc(uint64_t size) {
    g_staging_count++, g_staging_bytes += size;
    {
        // The smallest free block that fits, unless it is more than twice what this needs.
        std::lock_guard lk(g_grave_lock);
        size_t best = g_staging_free.size();
        for (size_t k = 0; k < g_staging_free.size(); k++) {
            const uint64_t have = g_staging_free[k].size;
            if (have >= size && have / 2 <= size && (best == g_staging_free.size() || have < g_staging_free[best].size))
                best = k;
        }
        if (best != g_staging_free.size()) {
            const StagingBlock block = g_staging_free[best];
            g_staging_free[best] = g_staging_free.back();
            g_staging_free.pop_back();
            g_staging_free_bytes -= block.size;
            g_staging_reused++;
            g_staging.push_back({g_submitted + 1, block});
            return {block.buffer, 0, block.ptr, false, block.cached};
        }
    }
    VkDevice d = vk_device();
    // Rounded up, so that blocks come in sizes the next textures fit.
    uint64_t rounded = 1ull << 20;
    while (rounded < size) rounded = rounded * 3 / 2 >= size && rounded % 2 == 0 ? rounded * 3 / 2 : rounded * 2;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = rounded;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VkBuffer buffer = VK_NULL_HANDLE;
    if (p_vkCreateBuffer(d, &bi, nullptr, &buffer) != VK_SUCCESS) return upload_alloc(size, 16);
    VkMemoryRequirements req{};
    p_vkGetBufferMemoryRequirements(d, buffer, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    // The ring's type: host-cached where the ring is, so a decode can write straight into it (nvn_upload.cpp).
    ai.memoryTypeIndex = upload_memory_type(req.memoryTypeBits);
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* p = nullptr;
    if (p_vkAllocateMemory(d, &ai, nullptr, &memory) != VK_SUCCESS || p_vkBindBufferMemory(d, buffer, memory, 0) ||
        p_vkMapMemory(d, memory, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) {
        p_vkDestroyBuffer(d, buffer, nullptr);
        if (memory) p_vkFreeMemory(d, memory, nullptr);
        return upload_alloc(size, 16);
    }
    const bool cached = (memory_type_properties(ai.memoryTypeIndex) & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != 0;
    std::lock_guard lk(g_grave_lock);
    g_staging.push_back({g_submitted + 1, {buffer, memory, static_cast<uint8_t*>(p), rounded, cached}});
    return {buffer, 0, static_cast<uint8_t*>(p), false, cached};
}

std::shared_ptr<GpuImage> reuse_image(const Texture& t) {
    // Off unless NX_IMAGE_REUSE=1: a CPU-written texture placed over a freed render target's memory would sample the
    // old render, and swizzle/alpha_from_r come from the old texture (review of #34). The pass breaks it avoided came
    // mostly from destroyed pool textures (fixed separately).
    // On by default (NX_IMAGE_REUSE=0 turns it off): the course's player samples AO/ambient targets of a destroyed
    // texture object two frames later (DEBUG.md #39); without the reused image they were black (a dark Mario).
    static const bool off = [] {
        const char* e = std::getenv("NX_IMAGE_REUSE");
        return e && e[0] == '0';
    }();
    if (!t.memory || off) return nullptr;
    std::lock_guard lk(g_grave_lock);
    auto it = g_reusable.find(reuse_key(t));
    if (it == g_reusable.end()) return nullptr;
    std::shared_ptr<GpuImage> img = std::move(it->second.second);
    g_reusable.erase(it);
    g_reused++;
    return img;
}

// The anonymous namespace's replays_in_flight under a name nvn_queue.cpp can call (NX_SYNC_AHEAD). A declaration of
// the same name at nx::nvn scope would make the unqualified calls inside this file ambiguous, hence the new name.
uint32_t frame_replays_in_flight() { return replays_in_flight(); }

}  // namespace nx::nvn
