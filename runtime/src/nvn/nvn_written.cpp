// Compute results within a submit (docs/archive/nvn/GLITCH-PLAN.md G5). A storage buffer a compute shader writes is a copy in
// the upload buffer, written back to guest memory once the submit completed. Each binding used to get a copy of its
// own: later draws and dispatches of the submit read the guest bytes from before the dispatch, two dispatches writing
// one buffer overwrote each other at write-back, and indirect dispatch arguments were read before the GPU wrote them.
// Here the submit keeps one copy per written guest range (the ranges are disjoint, keyed by host address). Writers and
// readers of a range bind that copy; ranges that meet are merged into a new copy on the GPU (vkCmdCopyBuffer, so in
// command order); each range is written back once. NX_COMPUTE_SEPARATE=1: the previous behaviour.
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#if defined(__AVX2__)
#include <immintrin.h>
#endif
#include <cstring>
#include <condition_variable>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

#include "nvn_copy_behind.h"
#include "nvn_draw_state.h"
#include "nvn_compute_keep.h"
#include "nvn_merge.h"

namespace nx::nvn {
// Defined further down, next to the work pool it sizes. Declared here so written_log reports the count the helpers
// actually run with instead of re-deriving it -- that re-derivation is how the log came to say 3 while six ran.
unsigned copy_threads();
}  // namespace nx::nvn

namespace nx::nvn::replay {

using namespace nx::vk;

// NX_REPLAY_CUTS (nvn_vk.h has the values and the default): replay encoding cuts.
//   barrier: copy_regions ended every composed read with a TRANSFER -> ALL_COMMANDS memory barrier. In the Prologue
//     battle state that path runs ~2,530 times per frame (written_log's counters cover the 300-frame window
//     draw_log_stats prints; divide its "reads composed" by 300), so a frame held ~2,500 full GPU pipeline barriers
//     plus as many small copies, while the GPU's own passes are idle (NX_GPU_PROFILE). One barrier before the next
//     command that can read the copies does the same work, at one per draw that needs it.
//   cache: a composed read of the same (address, size, padded, align) as an earlier one of this submit, with no write to
//     any written range it composed since, is that earlier copy: the written parts of it are those ranges' versions and
//     the guest bytes outside them are reused under dedup_upload's rule (DEBUG.md #30; inline CPU writes restart the
//     cache, written_cpu_write below).
//   pipeline: the pipelined split's fast path (nvn_frames.cpp) needs no writable bind after the split, not no bind.
// Unset: barrier and cache on (measured: 12.4 -> 14.9 fps in the fight, copies+barriers 2,698 -> 9.4 per frame);
// "0": all off; a value naming cuts enables exactly those; "1"/"all" enables the three non-experimental ones.
uint32_t replay_cuts() {
    static const uint32_t v = [] {
        const char* e = std::getenv("NX_REPLAY_CUTS");
        if (!e || !e[0]) return uint32_t(kCutCopyBarrier | kCutComposedCache);
        if (e[0] == '0' && !e[1]) return 0u;  // the pre-cut behaviour, for A/B
        const bool barrier = std::strstr(e, "barrier") != nullptr, cache = std::strstr(e, "cache") != nullptr;
        const bool split = std::strstr(e, "pipeline") != nullptr || std::strstr(e, "split") != nullptr;
        if (barrier || cache || split)
            return uint32_t((barrier ? kCutCopyBarrier : 0u) | (cache ? kCutComposedCache : 0u) |
                            (split ? kCutSplitGuard : 0u));
        return uint32_t(kCutCopyBarrier | kCutComposedCache | kCutSplitGuard);  // "1", "all"
    }();
    return v;
}

namespace {

constexpr uint64_t kAlign = 256;          // the largest buffer offset alignment Vulkan allows (all descriptor kinds)
constexpr uint64_t kTail = 0x10000 + 16;  // zeros after a range: storage bindings are padded to 64 KB (nvn_draw.cpp)

// 8- and 4-byte reads that compile to a single load (std::memcmp per eight bytes was ~1.4 ms for 16 MiB in the merge).
inline uint64_t load64(const uint8_t* p) {
    uint64_t v;
    std::memcpy(&v, p, 8);
    return v;
}
inline uint32_t load32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

// NX_REPLAY_COUNTS=1: written_log reports, per 300-frame window, why the composed reads are composed, what they cost
// and how many of them repeat within a submit (the numbers that rank the fixes).
bool replay_counts() {
    static const bool on = std::getenv("NX_REPLAY_COUNTS") != nullptr;
    return on;
}

// Why containing() refused [p, p+size) as a sub-range of one written copy (NX_REPLAY_COUNTS): the composed path is taken
// for four different reasons and they need different fixes.
enum ContainedWhy {
    kWhyMany = 0,       // more than one written range overlaps the read (it crosses or meets another range)
    kWhyPastEnd = 1,    // it starts inside a range and runs past that range's guest bytes
    kWhyPastCap = 2,    // it fits the range but not the copy's tail capacity (padded/rounding)
    kWhyMisaligned = 3, // the copy's ring offset + this offset is not aligned enough for the descriptor
    kWhyNoRegion = 4    // no range at all: not a composed read (written_read returns before it)
};

uint64_t g_composed_bytes = 0, g_composed_small = 0, g_composed_small_bytes = 0;
uint64_t g_composed_why[5] = {};
uint64_t g_composed_size[6] = {};  // <256 B, <1 KiB, <4 KiB, <16 KiB, <64 KiB, 64 KiB and up
uint64_t g_composed_repeat = 0, g_composed_distinct = 0, g_composed_cache_hits = 0;
// NX_REPLAY_COUNTS, with the cache on: hits whose uncovered guest bytes changed since the copy was made. The cache's one
// assumption is dedup's (DEBUG.md #30); this counts how often the guest thread actually breaks it.
uint64_t g_cache_hazard = 0, g_cache_hazard_first_hit = 0;
const uint8_t* g_cache_hazard_at = nullptr;
uint64_t g_copy_calls = 0, g_copy_cmds = 0, g_copy_barriers = 0;
bool g_copies_pending = false;  // copies recorded since the last copy_write_barrier (NX_REPLAY_CUTS=barrier)

// NX_REPLAY_COUNTS: per-submit set of the (address, size) a composed read was made for, to report its repeat rate. Open
// addressing, emptied by bumping the generation at each submit (as the handle and set caches do).
struct ReadKey {
    uint64_t generation = 0;
    const uint8_t* p = nullptr;
    uint64_t size = 0;
};
constexpr uint32_t kReadKeys = 4096;
ReadKey g_read_keys[kReadKeys];
uint64_t g_read_generation = 1;
uint32_t g_read_used = 0;

void read_key_note(const uint8_t* p, uint64_t size) {
    if (g_read_used >= kReadKeys / 2) {  // a scene with more distinct reads than the table: start the window over
        g_read_generation++, g_read_used = 0;
    }
    uint64_t h = (uint64_t(p) >> 4) * 0x9E3779B97F4A7C15ull ^ size * 0xC2B2AE3D27D4EB4Full;
    for (uint32_t k = uint32_t(h >> 33);; k++) {
        ReadKey& s = g_read_keys[k & (kReadKeys - 1)];
        if (s.generation != g_read_generation) {
            s = {g_read_generation, p, size};
            g_read_used++, g_composed_distinct++;
            return;
        }
        if (s.p == p && s.size == size) {
            g_composed_repeat++;
            return;
        }
    }
}

// NX_REPLAY_CUTS=cache: one composed copy per (address, size, padded, align) per submit, reused while no write reaches
// the written ranges it composes. The key's written parts are those ranges (their `version` is bumped by every
// written_bind, written_count_bind and merge, and a merge installs a new region with a higher version), and the guest
// bytes outside them are the assumption dedup_upload already makes (DEBUG.md #30: bytes that do not change during the
// submit). `regions`/`lo`/`hi` pin the exact overlapping set: region boundaries never split, only merge away.
struct ComposedSlot {
    uint64_t generation = 0, version = 0, regions = 0;
    const uint8_t* p = nullptr, *lo = nullptr, *hi = nullptr;
    uint64_t size = 0, padded = 0, align = 0;
    Upload up;
};
constexpr uint32_t kComposedSlots = 4096;
ComposedSlot g_composed_slots[kComposedSlots];
uint64_t g_composed_generation = 1;
uint32_t g_composed_used = 0;

ComposedSlot* composed_slot(const uint8_t* p, uint64_t size, uint64_t padded, uint64_t align) {
    uint64_t h = uint64_t(p) * 0x9E3779B97F4A7C15ull ^ size * 0xC2B2AE3D27D4EB4Full ^ padded * 0x165667B19E3779F9ull ^
                 align * 0x27D4EB2F165667C5ull;
    for (uint32_t k = uint32_t(h >> 40);; k++) {
        ComposedSlot& s = g_composed_slots[k & (kComposedSlots - 1)];
        if (s.generation != g_composed_generation) return &s;
        if (s.p == p && s.size == size && s.padded == padded && s.align == align) return &s;
    }
}

struct Region {
    uint64_t size = 0;  // guest bytes from the key
    uint64_t cap = 0;   // bytes of the copy from up.offset (size, then zeros)
    uint64_t version = 0;  // latest possible GPU write, monotonic across submits
    Upload up;          // the copy of the key's first byte
    std::shared_ptr<std::vector<uint8_t>> before;  // the guest bytes copied in (what the GPU did not change)
    bool whole = false;  // an actual full-range fill may be written back without a snapshot
    // NX_HOST_IMPORT: up is the guest memory itself (VK_EXT_external_memory_host); the GPU's writes are already where
    // the game reads them, so this region has no snapshot and no write-back.
    bool direct = false;
    uint64_t wrote = 0;    // g_writable_binds when a write to it was last recorded (NX_CARRY)
    // NX_CARRY: the last submit's result, not yet written by this one. Its guest bytes come when that submit lands.
    bool carried = false;
};
using Regions = std::map<const uint8_t*, Region>;
using It = Regions::iterator;
Regions g_regions;
// [lo, hi) of every region, sorted by lo and rebuilt after the map changes: the answer to "does this range touch any
// region" for a read, which is what ~20,000 binds a frame ask and nearly all of them answer no. A submit has about 20
// regions and changes them a few dozen times, so a tree walk per bind (upper_bound, then std::prev) cost more than the
// rebuilds. Regions never change size; they only merge away.
struct RegionSpan {
    const uint8_t* lo;
    const uint8_t* hi;
};
std::vector<RegionSpan> g_spans;
bool g_spans_stale = false;
bool g_read = false;  // a copy was bound for reading since the last write_after_read
uint64_t g_binds = 0;  // written_bind calls, ever (nvn_frames.cpp: did a dispatch write storage after the split)
// The subset written_bind accounts for, which is the only one that can change a copy's contents after the split: a
// written_count_bind() bind only reads a range another writer filled, so the split fence already covers it.
uint64_t g_writable_binds = 0;
uint64_t g_write_version = 0;  // never reset: cached images outlive the submit's regions
uint64_t g_taken_version = 0;  // the newest version among the ranges written_take() last handed back
uint64_t g_shared = 0, g_merged = 0, g_composed = 0, g_indirect = 0;  // for written_log
uint64_t g_imported_binds = 0;  // NX_HOST_IMPORT binds that used the guest memory itself (for written_log)
uint64_t g_direct_reads = 0, g_direct_read_bytes = 0;  // NX_DIRECT_READS binds straight from guest memory
// Wall time of the whole write-back batch (NX_COPY_THREADS helps only if this goes down): logged every 60 frames.
std::atomic<uint64_t> g_wb_wall_ns{0}, g_wb_batches{0}, g_wb_ranges{0};
// NX_COPYIN_TIMING=1: guest->ring copy-in (written_bind's two copy sites).
// written_log uses the present interval for per-frame rates and reports submits separately.
std::atomic<uint64_t> g_copyin_ns{0}, g_copyin_bytes{0};
uint64_t g_log_submits = 0;  // submits since the last written_log
bool copyin_timing() {
    static const bool on = std::getenv("NX_COPYIN_TIMING") != nullptr;
    return on;
}

// Atomic: NX_COPY_THREADS copies a submit's write-backs on helper threads (apply_write_back below).
std::atomic<uint64_t> g_whole_writes{0}, g_whole_bytes{0};  // write-backs that took the whole-range path (for written_log)
std::atomic<uint64_t> g_merge_writes{0}, g_merge_bytes{0};  // write-backs merged against their snapshots (for written_log)
std::atomic<uint64_t> g_late_conflicts{0};  // words the GPU and a guest released early both changed (for written_log)

// NX_WRITE_BACK_MERGE retains its existing meaning: disable the actual-fill
// whole-copy path and merge against a snapshot instead.
bool learn_whole_ranges() {
    static const bool on = std::getenv("NX_WRITE_BACK_MERGE") == nullptr;
    return on;
}

// Bind-side snapshot bytes and time, reported per present window.
std::atomic<uint64_t> g_snap_ns{0}, g_snap_bytes{0};

// Recycle snapshot allocations, but replace their complete contents on every bind.
// The pool keeps at most kSnapPoolBytes / kSnapPoolMax.
std::vector<std::shared_ptr<std::vector<uint8_t>>> g_snap_pool;
std::mutex g_snap_mu;
uint64_t g_snap_pool_bytes = 0;
constexpr size_t kSnapPoolMax = 64;
constexpr uint64_t kSnapPoolBytes = 192ull << 20;

// Take an exclusively owned allocation with enough capacity, or return null on
// a miss. Exact-sized entries avoid a resize; otherwise use the smallest fit.
std::shared_ptr<std::vector<uint8_t>> snap_take(uint64_t bytes) {
    std::shared_ptr<std::vector<uint8_t>> v;
    {
        std::lock_guard lk(g_snap_mu);
        size_t best = g_snap_pool.size();
        bool best_exact = false;
        for (size_t k = 0; k < g_snap_pool.size(); k++) {
            const auto& s = g_snap_pool[k];
            if (!s || s.use_count() != 1 || s->capacity() < bytes) continue;
            const bool exact = s->size() == bytes;
            if (best == g_snap_pool.size() || (exact && !best_exact) ||
                (exact == best_exact && s->capacity() < g_snap_pool[best]->capacity())) {
                best = k;
                best_exact = exact;
            }
        }
        if (best == g_snap_pool.size()) return {};
        v = std::move(g_snap_pool[best]);
        g_snap_pool[best] = std::move(g_snap_pool.back());
        g_snap_pool.pop_back();
        g_snap_pool_bytes -= v->capacity();
    }
    // The caller replaces every byte before publishing the resized vector.
    v->resize(size_t(bytes));
    return v;
}

void snap_give(std::shared_ptr<std::vector<uint8_t>>& v) {
    if (!v) return;
    std::unique_lock lk(g_snap_mu);
    const uint64_t bytes = v->capacity();
    if (v.use_count() != 1 || g_snap_pool.size() > kSnapPoolMax ||
        g_snap_pool_bytes > kSnapPoolBytes || bytes > kSnapPoolBytes) return;
    if (g_snap_pool.size() < kSnapPoolMax && bytes <= kSnapPoolBytes - g_snap_pool_bytes) {
        g_snap_pool.push_back(std::move(v));
        g_snap_pool_bytes += bytes;
        return;
    }

    // Small plain snapshots can fill the pool before larger ones retire. Plan
    // a replacement using the smallest exclusively owned allocations, but do
    // not evict anything unless the incoming allocation can actually fit.
    std::array<size_t, kSnapPoolMax> victims{};
    size_t candidates = 0;
    for (size_t k = 0; k < g_snap_pool.size(); k++) {
        const auto& s = g_snap_pool[k];
        if (s && s.use_count() == 1 && s->capacity() < bytes) victims[candidates++] = k;
    }
    std::sort(victims.begin(), victims.begin() + candidates, [&](size_t a, size_t b) {
        return g_snap_pool[a]->capacity() < g_snap_pool[b]->capacity();
    });
    size_t selected = 0, count = g_snap_pool.size();
    uint64_t remaining = g_snap_pool_bytes;
    while (count >= kSnapPoolMax || bytes > kSnapPoolBytes - remaining) {
        if (selected == candidates) return;
        const size_t capacity = g_snap_pool[victims[selected++]]->capacity();
        if (capacity > remaining) return;  // fail closed if accounting is inconsistent
        remaining -= capacity;
        --count;
    }

    // Descending indices remain valid when each removed slot is filled from
    // the back. Destroy the displaced allocations after releasing the mutex.
    std::sort(victims.begin(), victims.begin() + selected, std::greater<size_t>());
    std::array<std::shared_ptr<std::vector<uint8_t>>, kSnapPoolMax> evicted;
    for (size_t i = 0; i < selected; i++) {
        const size_t k = victims[i];
        evicted[i] = std::move(g_snap_pool[k]);
        g_snap_pool[k] = std::move(g_snap_pool.back());
        g_snap_pool.pop_back();
    }
    g_snap_pool_bytes = remaining;
    g_snap_pool.push_back(std::move(v));
    g_snap_pool_bytes += bytes;
    lk.unlock();
}

// Room for a snapshot a queued copy fills (nvn_copy_behind.h).
std::vector<WriteBack> g_carry_pending;  // NX_CARRY: handed over by frame_begin, seeded by the submit it began
uint64_t g_carry_submits = 0, g_carry_ranges = 0, g_carry_bytes = 0, g_carry_refused_size = 0, g_carry_refused_guest = 0,
         g_carry_erased = 0;

std::shared_ptr<std::vector<uint8_t>> snapshot_room(uint64_t n) {
    auto v = snap_take(n);
    return v ? v : std::make_shared<std::vector<uint8_t>>(size_t(n));
}

std::shared_ptr<std::vector<uint8_t>> plain_snapshot(const uint8_t* up, uint64_t n) {
    auto v = snap_take(n);
    if (!v) return std::make_shared<std::vector<uint8_t>>(up, up + n);
    std::memcpy(v->data(), up, size_t(n));
    return v;
}

// NX_P2_KEPT_COPY=1: experimentally reuse host-coherent compute allocations.
// Every prior submitted part is retired before reuse, current guest bytes are
// compared in full before skipping a refill. Merge-path binds keep a complete
// fresh snapshot for the ordinary per-submit guest write-back. A write watch alone cannot
// detect guest reads, kernel writes, remapping, or prove any of those conditions.
struct KeptCopy {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint8_t* ptr = nullptr;  // host-visible, the whole buffer
    uint64_t cap = 0;        // buffer bytes: the guest range, then the tail zeros
    uint64_t alloc_size = 0; // actual allocation size, including Vulkan alignment
    uint64_t size = 0;       // the guest range it was made for
    bool valid = false;      // eligible for bytewise validation after full retirement
};
std::map<const uint8_t*, KeptCopy> g_kept;
uint64_t g_kept_bytes = 0, g_kept_hits = 0, g_kept_saved = 0, g_kept_refills = 0;
bool g_kept_evict_pending = false;
constexpr uint64_t kKeptMax = 128ull << 20;  // bounded until the next safe eviction point
// NX_P2_KEPT_COPY_MAX (MB): the same cap, lowered on purpose. The eviction path needs a scene change inside the cap
// window to run at all, so this is the only way to exercise it without one.
uint64_t kept_max() {
    static const uint64_t n = [] {
        const char* e = std::getenv("NX_P2_KEPT_COPY_MAX");
        const long mb = e ? std::strtol(e, nullptr, 10) : 0;
        return mb > 0 ? uint64_t(mb) << 20 : kKeptMax;
    }();
    return n;
}

// Off unless explicitly enabled. Async submissions provide both the per-part
// device->host barrier and the full-fence retirement needed by this experiment.
bool keep_copies() {
    static const bool on = [] {
        const char* e = std::getenv("NX_P2_KEPT_COPY");
        return e && e[0] == '1' && !e[1] && !written_separate();
    }();
    return on && async_gpu();
}

void kept_forget(KeptCopy& k) {
    if (k.ptr && k.memory) p_vkUnmapMemory(vk_device(), k.memory);
    if (k.buffer) p_vkDestroyBuffer(vk_device(), k.buffer, nullptr);
    if (k.memory) p_vkFreeMemory(vk_device(), k.memory, nullptr);
    k = KeptCopy{};
}

void kept_clear() {
    for (auto& [p, k] : g_kept) kept_forget(k);
    g_kept.clear(), g_kept_bytes = 0, g_kept_evict_pending = false;
}

// A host-visible storage buffer of its own (the upload ring's memory kinds, exactly as upload_alloc gets).
void kept_alloc(KeptCopy& k, uint64_t bytes, uint64_t budget) {
    VkDevice d = vk_device();
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = bytes;
    bi.usage = kUploadUsage;  // written_read can bind these as vertex/index/uniform/indirect too
    if (p_vkCreateBuffer(d, &bi, nullptr, &k.buffer) != VK_SUCCESS) return;
    VkMemoryRequirements req{};
    p_vkGetBufferMemoryRequirements(d, k.buffer, &req);
    if (req.size > budget) {
        // Recordings can reference existing entries, so reclaim at the next
        // full-retirement boundary instead of evicting here.
        g_kept_evict_pending = !g_kept.empty();
        kept_forget(k);
        return;
    }
    VkPhysicalDeviceMemoryProperties props{};
    p_vkGetPhysicalDeviceMemoryProperties(vk_physical(), &props);
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t type = props.memoryTypeCount;
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        const VkMemoryPropertyFlags flags = props.memoryTypes[i].propertyFlags;
        if (!(req.memoryTypeBits & (1u << i)) || (flags & want) != want) continue;
        if (type == props.memoryTypeCount) type = i;  // first coherent fallback
        // Both memcmp and the write-back snapshot read this mapping on the CPU.
        if (flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) { type = i; break; }
    }
    if (type == props.memoryTypeCount) {
        kept_forget(k);  // never reuse a non-coherent or incompatible mapping
        return;
    }
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    if (p_vkAllocateMemory(d, &ai, nullptr, &k.memory) != VK_SUCCESS || p_vkBindBufferMemory(d, k.buffer, k.memory, 0) ||
        p_vkMapMemory(d, k.memory, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&k.ptr)) != VK_SUCCESS) {
        kept_forget(k);
        return;
    }
    k.cap = bytes;
    k.alloc_size = req.size;
    // kept_alloc runs under gpu_lock; report the first successful choice once.
    static bool reported = false;
    if (!reported) {
        const VkMemoryPropertyFlags flags = props.memoryTypes[type].propertyFlags;
        log("nvn: kept copies memory type %u flags=0x%08x (HOST_CACHED=%s)",
            type, unsigned(flags), (flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) ? "yes" : "no");
        reported = true;
    }
}

// The kept copies meeting [p, p + size): what the GPU does outside them leaves them stale.
void kept_invalidate(const uint8_t* p, uint64_t size) {
    if (!p || !size) return;
    const uintptr_t start = reinterpret_cast<uintptr_t>(p);
    for (auto& [key, k] : g_kept) {
        const uintptr_t begin = reinterpret_cast<uintptr_t>(key);
        // Difference comparisons avoid endpoint overflow and comparisons
        // between pointers into unrelated guest allocations.
        if (begin <= start ? start - begin < k.size : begin - start < size) k.valid = false;
    }
}

// Called under gpu_lock when a split lands while later parts remain in flight.
void kept_invalidate_all() {
    for (auto& [key, k] : g_kept) k.valid = false;
}

// NX_REPLAY_COUNTS, cache on: the bytes of a composed copy that came from guest memory are the ones a CPU write would
// change (the covered parts come from the regions' copies, which carry a version). Compare them with guest memory now:
// a difference means the guest thread wrote a range it had already bound within this submit -- dedup_upload's rule
// (DEBUG.md #30) assumes it does not, and this counts how often that assumption actually breaks.
void cache_hazard_probe(const Upload& cached, const std::vector<It>& regions, const uint8_t* p, uint64_t size) {
    copy_behind::wait();
    bool changed = false;
    uint64_t at = 0;  // the bytes before `at` are covered by a written region and came from its copy
    for (It it : regions) {
        const uint64_t lo = uint64_t(std::max(it->first, p) - p);
        const uint64_t hi = uint64_t(std::min(it->first + it->second.size, p + size) - p);
        if (lo >= hi) continue;
        if (lo > at && std::memcmp(cached.ptr + at, p + at, size_t(lo - at)) != 0) changed = true;
        at = hi;
    }
    if (at < size && std::memcmp(cached.ptr + at, p + at, size_t(size - at)) != 0) changed = true;
    if (!changed) return;
    if (!g_cache_hazard) g_cache_hazard_at = p, g_cache_hazard_first_hit = g_composed_cache_hits;
    g_cache_hazard++;
}

bool touches_region(const uint8_t* p, uint64_t size) {
    if (g_spans_stale) {
        g_spans.clear();
        for (const auto& [lo, r] : g_regions) g_spans.push_back({lo, lo + r.size});
        g_spans_stale = false;
    }
    // The first region starting after p; the one before it is the only one that can reach over p.
    size_t a = 0, n = g_spans.size();
    while (n) {
        const size_t half = n / 2;
        if (g_spans[a + half].lo <= p) a += half + 1, n -= half + 1;
        else n = half;
    }
    if (a && g_spans[a - 1].hi > p) return true;
    return a < g_spans.size() && g_spans[a].lo < p + size;
}

// The regions overlapping [p, p + size), in address order. The vector is scratch, valid until the next call: written_read
// and written_bind each run ~3,000 times per frame in the battle state (the 300-frame window of written_log) and every
// call allocated and freed one (the "free win" of docs/archive/nvn/PERF-PLAN.md's cheaper lookups).
const std::vector<It>& overlapping(const uint8_t* p, uint64_t size) {
    thread_local std::vector<It> v;  // one thread replays one submit
    v.clear();
    if (!touches_region(p, size)) return v;
    It it = g_regions.upper_bound(p);
    if (it != g_regions.begin() && std::prev(it)->first + std::prev(it)->second.size > p) v.push_back(std::prev(it));
    for (; it != g_regions.end() && it->first < p + size; ++it) v.push_back(it);
    return v;
}

// `bytes` of upload memory whose byte `anchor` is kAlign aligned (so it can start a binding).
Upload alloc_anchored(uint64_t bytes, uint64_t anchor) {
    Upload u = upload_alloc(bytes + kAlign, kAlign);
    uint64_t shift = (kAlign - anchor % kAlign) % kAlign;
    u.offset += shift, u.ptr += shift;
    return u;
}

// The parts of `regions` inside [p, p + size) copied on the GPU into `to` (whose first byte is p's), then made
// visible to everything after. What the dispatches recorded so far wrote is there when the copy runs: dispatch()
// ends each one with a barrier to all later commands. Outside render passes.
// NX_REPLAY_CUTS=barrier: the visibility barrier is not recorded here but by the next consumer (copy_write_barrier),
// which is the command that reads the copies; one barrier then covers every copy recorded since the previous one.
void copy_regions(VkCommandBuffer cb, const std::vector<It>& regions, const uint8_t* p, uint64_t size, const Upload& to) {
    g_copy_calls++;
    for (It it : regions) {
        const uint8_t* lo = std::max(it->first, p);
        const uint8_t* hi = std::min(it->first + it->second.size, p + size);
        if (lo >= hi) continue;
        VkBufferCopy c{it->second.up.offset + uint64_t(lo - it->first), to.offset + uint64_t(lo - p), uint64_t(hi - lo)};
        p_vkCmdCopyBuffer(cb, it->second.up.buffer, to.buffer, 1, &c);
        g_copy_cmds++;
    }
    if (replay_cuts() & kCutCopyBarrier) {
        g_copies_pending = true;
        return;
    }
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, nullptr,
                           0, nullptr);
    g_copy_barriers++;
}

// A dispatch about to write a copy that earlier commands of the submit read (draws binding it as vertices, uniforms,
// storage): they finish first. Earlier writes are already ordered by dispatch()'s barrier.
void write_after_read(VkCommandBuffer cb) {
    if (!g_read) return;
    g_read = false;
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                           nullptr, 0, nullptr);
}

// The device's buffer descriptor offset alignment (kAlign at most, by the Vulkan spec).
uint64_t bind_align() {
    static const uint64_t a = [] {
        VkPhysicalDeviceProperties props{};
        p_vkGetPhysicalDeviceProperties(vk_physical(), &props);
        return std::max<uint64_t>({props.limits.minStorageBufferOffsetAlignment,
                                   props.limits.minUniformBufferOffsetAlignment, 16});
    }();
    return a;
}

Upload at(const It& it, uint64_t off) {
    Upload u{it->second.up.buffer, it->second.up.offset + off, it->second.up.ptr + off};
    u.direct = it->second.up.direct;
    return u;
}

// [p, p + size) as a sub-range of one region, bindable at `align` (at most the device's descriptor alignment; the
// callers ask 256 for descriptors) with `padded` bytes behind it: else end(). A copy is anchored at its first writer,
// so two pointers aligned in guest memory are aligned in the copy. `why` (NX_REPLAY_COUNTS) says which test refused it.
It containing(const uint8_t* p, uint64_t size, uint64_t padded, uint64_t align, const std::vector<It>& regions,
              ContainedWhy* why = nullptr) {
    if (regions.size() != 1) {
        if (why) *why = kWhyMany;
        return g_regions.end();
    }
    It it = regions[0];
    uint64_t off = uint64_t(p - it->first);
    if (p < it->first || off + size > it->second.size) {
        if (why) *why = kWhyPastEnd;
        return g_regions.end();
    }
    if (off + padded > it->second.cap) {
        if (why) *why = kWhyPastCap;
        return g_regions.end();
    }
    if ((it->second.up.offset + off) % std::clamp<uint64_t>(align, 1, bind_align())) {
        if (why) *why = kWhyMisaligned;
        return g_regions.end();
    }
    return it;
}

}  // namespace

// NX_REPLAY_CUTS=barrier: make the copies recorded since the last call visible to everything after them, in one barrier.
// The consumers call this immediately before they record the command that reads a composed copy: a draw (nvn_draw.cpp),
// a dispatch (nvn_draw.cpp), vkCmdDispatchIndirect (written_indirect, below), the indirect-argument copy
// (nvn_indirect.cpp), a texture upload's copy (nvn_upload.cpp), a buffer copy (nvn_buffer_copy.cpp), a ClearBuffer fill
// (nvn_cmd_replay.cpp) and the merge copies of written_bind (below). Sound because a composed copy is a fresh upload
// allocation nothing else binds: its only readers are the descriptors of the draw or dispatch that asked for it.
void copy_write_barrier(VkCommandBuffer cb) {
    if (!g_copies_pending) return;
    g_copies_pending = false;
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, nullptr,
                           0, nullptr);
    g_copy_barriers++;
}

bool written_separate() {
    static const bool on = std::getenv("NX_COMPUTE_SEPARATE") != nullptr;
    return on;
}

// Direct host imports are disabled until imported guest mappings have tracked lifetimes and their GPU work is
// included in every relevant wait. In particular, direct-only writes currently produce no write-back serial.
// Log once if either old presence-based switch was requested, then use the ordinary snapshot/copy path.
void warn_direct_import_disabled() {
    static const bool warned = [] {
        log("nvn: NX_HOST_IMPORT/NX_DIRECT_READS disabled: guest mapping lifetimes and GPU completion are not tracked; using copy path");
        return true;
    }();
    (void)warned;
}

bool host_import() {
    static const bool requested = std::getenv("NX_HOST_IMPORT") != nullptr;
    if (requested) warn_direct_import_disabled();
    return false;
}

uint64_t written_bind_count() { return g_binds; }
// The exact form of the split guard's question: the first part of a split submit carries all the compute results the
// write-backs need when no written range has been re-bound as a GPU target since the split. A region re-bound after the
// split may have been written by the later part, so its copy is only complete once the whole submit is done -- and
// `version` is bumped exactly there. A region created after the split is caught by the same test, since its version is
// newer than the one taken at the split.
uint64_t written_version() { return g_write_version; }
uint64_t written_newest_version() { return g_taken_version; }
bool written_complete_since(uint64_t version) { return g_taken_version <= version; }
uint64_t written_writable_bind_count() { return g_writable_binds; }
void written_count_bind() {
    g_binds++;
    // A reused graphics storage descriptor may write any tracked region. Its
    // address is not supplied here, so conservatively invalidate their images.
    const uint64_t version = ++g_write_version;
    if (!version) fatal("nvn: GPU write version overflow");
    for (auto& [_, r] : g_regions) r.version = version;
}

Upload written_bind(VkCommandBuffer cb, const uint8_t* p, uint64_t size, uint64_t padded, bool fills_range) {
    g_binds++, g_writable_binds++;
    const uint64_t version = ++g_write_version;
    if (!version) fatal("nvn: GPU write version overflow");
    const std::vector<It>& old = overlapping(p, size);  // live until the next overlapping() call: none below
    if (It it = containing(p, size, padded, bind_align(), old); it != g_regions.end()) {
        compute_write_barrier(cb);  // NX_LAZY_COMPUTE_BARRIER: the earlier dispatch's writes to this copy come first
        write_after_read(cb);
        it->second.version = version;
        it->second.wrote = g_writable_binds, it->second.carried = false;
        g_shared++;
        return at(it, uint64_t(p - it->first));
    }
    // A new range, or one meeting others (or misaligned in its copy): one copy covering all of them, anchored at p.
    // A carried range's snapshot and the guest bytes around it are composed below: its submit lands first.
    for (It it : old)
        if (it->second.carried) {
            frame_carry_wait();
            break;
        }
    const uint8_t* lo = p;
    const uint8_t* hi = p + size;
    for (It it : old) lo = std::min(lo, it->first), hi = std::max(hi, it->first + it->second.size);
    uint64_t n = uint64_t(hi - lo), off = uint64_t(p - lo);
    // NX_HOST_IMPORT: the guest bytes as a VkBuffer. Only a range no earlier work of this submit touched is imported;
    // an overlap keeps the ring path, because composing the earlier copy is what that path is for.
    if (host_import() && old.empty()) {
        Upload u = host_import_upload(lo, padded, n);
        if (u.buffer) {
            Region r;
            r.version = version;
            r.size = n;
            r.cap = padded;
            r.up = u;
            r.whole = true;
            r.direct = true;
            r.wrote = g_writable_binds;
            dedup_invalidate(lo, n);
            It it = g_regions.emplace(lo, r).first;
            g_spans_stale = true;
            g_imported_binds++;
            return at(it, 0);
        }
    }
    Region r;
    r.version = version;
    r.wrote = g_writable_binds;
    r.size = n;
    r.cap = std::max(n + kTail, off + padded);
    // A fill really writes the entire range. Learned history alone cannot
    // justify dropping the snapshot or replacing guest bytes wholesale.
    // NX_EARLY_COMPLETE: a late landing has to tell a guest write from a GPU one, which takes the snapshot.
    const bool whole = fills_range && old.empty() && learn_whole_ranges() && !early_complete();
    bool queued = false;
    KeptCopy* kept = nullptr;
    if (keep_copies() && old.empty() && r.cap <= kKeptMax) {
        auto it = g_kept.find(lo);
        if (it != g_kept.end() && (it->second.size != n || it->second.cap < r.cap)) {
            // Recorded commands in this submit can still reference it, even
            // if its original region was merged. Evict only at the next reset.
            g_kept_evict_pending = true;
        } else if (it == g_kept.end()) {
            if (r.cap > kept_max() - g_kept_bytes) {
                g_kept_evict_pending = true;
            } else {
                it = g_kept.emplace(lo, KeptCopy{}).first;
                it->second.size = n;
                kept_alloc(it->second, r.cap, kept_max() - g_kept_bytes);
                if (!it->second.buffer) {
                    g_kept.erase(it);
                    it = g_kept.end(); // never reuse the next entry for a different guest address
                } else {
                    g_kept_bytes += it->second.alloc_size;
                }
            }
        }
        if (it != g_kept.end() && it->second.size == n && it->second.cap >= r.cap)
            kept = &it->second;
    }
    if (kept) {
        // The address may be reused or remapped, and the guest may have been
        // written by CPU or kernel since landing. Compare actual bytes every
        // time: a write-only page watch cannot certify freshness.
        const bool same = kept->valid && std::memcmp(kept->ptr, lo, size_t(n)) == 0;
        if (!same) {
            const bool timed = copyin_timing();
            const uint64_t t0 = timed ? plat::monotonic_ns() : 0;
            std::memcpy(kept->ptr, lo, size_t(n));
            if (timed) g_copyin_ns.fetch_add(plat::monotonic_ns() - t0, std::memory_order_relaxed);
            if (timed) g_copyin_bytes.fetch_add(n, std::memory_order_relaxed);
            g_kept_refills++;
        } else {
            g_kept_hits++, g_kept_saved += n;
        }
        std::memset(kept->ptr + n, 0, size_t(kept->cap - n));
        kept->valid = true;
        r.cap = kept->cap;
        r.up = Upload{kept->buffer, 0, kept->ptr};
    } else {
        if (keep_copies() && !g_kept.empty()) kept_invalidate(lo, n);
        const bool timed = copyin_timing();
        const uint64_t t0 = timed ? plat::monotonic_ns() : 0;
        r.up = alloc_anchored(r.cap, off);
        queued = async_gpu() && copy_behind::wanted(n);
        if (queued) {
            if (!whole) r.before = snapshot_room(n);
            copy_behind::push(r.up.ptr, lo, n, r.cap - n, r.before ? r.before->data() : nullptr);
            g_snap_bytes.fetch_add(whole ? 0 : n, std::memory_order_relaxed);
        } else {
            std::memcpy(r.up.ptr, lo, n);
            std::memset(r.up.ptr + n, 0, r.cap - n);
        }
        if (timed) g_copyin_ns.fetch_add(plat::monotonic_ns() - t0, std::memory_order_relaxed);
        if (timed) g_copyin_bytes.fetch_add(n, std::memory_order_relaxed);
    }
    r.whole = whole;
    // Every merge-path range keeps a complete bind-time snapshot. Ranges that
    // meet earlier ones compose those earlier snapshots below.
    if (!whole && !queued) {
        const uint64_t snap_t0 = plat::monotonic_ns();
        r.before = plain_snapshot(r.up.ptr, n);
        g_snap_ns.fetch_add(plat::monotonic_ns() - snap_t0, std::memory_order_relaxed);
        g_snap_bytes.fetch_add(n, std::memory_order_relaxed);
    }
    if (!old.empty()) copy_behind::wait();  // the snapshots composed here, and the ones erased after it
    for (It it : old) {  // their parts: as they were before their own GPU writes
        if (!it->second.before || !r.before) continue;
        std::memcpy(r.before->data() + (it->first - lo), it->second.before->data(), it->second.size);
    }
    if (!old.empty()) {
        compute_write_barrier(cb);  // NX_LAZY_COMPUTE_BARRIER: the merge copies earlier dispatch results
        copy_write_barrier(cb);     // NX_REPLAY_CUTS=barrier: a chained merge reads what the previous one copied
        copy_regions(cb, old, lo, n, r.up);  // dispatch(): no render pass is open
        for (It it : old) g_regions.erase(it);
        g_spans_stale = true;
        g_merged++;
    }
    dedup_invalidate(lo, n);  // copies of these bytes made earlier in the submit are not what later work reads
    It it = g_regions.emplace(lo, r).first;
    g_spans_stale = true;
    return at(it, off);
}

bool written_read(VkCommandBuffer cb, const uint8_t* p, uint64_t size, uint64_t padded, uint64_t align, Upload& out) {
    if (!p || !size || g_regions.empty() || written_separate()) return false;
    // The common case by far is a range no dispatch wrote: answer it from the region map directly instead of
    // collecting every overlapping region into a vector first (overlapping() walks the whole span, and a vertex range
    // of tens of MB that straddles many regions made that walk the cost of the inputs phase). Only a range that does
    // touch a region pays for the full list, which written_read needs in order to compose it.
    static const bool no_fast = std::getenv("NX_NO_FAST_REJECT") != nullptr;  // the A/B arm
    if (!no_fast && !touches_region(p, size)) return false;  // nothing of it was written
    const std::vector<It>& regions = overlapping(p, size);
    if (regions.empty()) return false;
    ContainedWhy why = kWhyMany;
    if (It it = containing(p, size, padded, align, regions, &why); it != g_regions.end()) {
        compute_write_barrier(cb);  // NX_LAZY_COMPUTE_BARRIER: the reader sees what the dispatch wrote
        g_shared++, g_read = true;
        out = at(it, uint64_t(p - it->first));
        return true;
    }
    // Partly written, or not bindable in place: a copy of its own, the written parts copied in on the GPU. This is
    // what the range holds at this point of the submit (later dispatches do not change it).
    const uint32_t cuts = replay_cuts();
    if (replay_counts()) {
        g_composed_why[why]++;
        g_composed_bytes += size;
        if (size < (64u << 10)) g_composed_small++, g_composed_small_bytes += size;
        const uint64_t bucket = size < 256 ? 0 : size < (1u << 10) ? 1 : size < (4u << 10) ? 2 : size < (16u << 10) ? 3
                                                                   : size < (64u << 10) ? 4 : 5;
        g_composed_size[bucket]++;
        read_key_note(p, size);
    }
    // The shape of what this read composes: which ranges overlap it and how far their writes have got.
    uint64_t version = 0;
    for (It it : regions) version = std::max(version, it->second.version);
    if (cuts & kCutComposedCache) {
        ComposedSlot* slot = composed_slot(p, size, padded, align);
        if (slot->generation == g_composed_generation && slot->version == version &&
            slot->regions == regions.size() && slot->lo == regions.front()->first &&
            slot->hi == regions.back()->first) {
            g_composed_cache_hits++;  // no write reached them since: this submit's earlier copy is the same bytes
            if (replay_counts()) cache_hazard_probe(slot->up, regions, p, size);
            out = slot->up;
            return true;
        }
    }
    draw_end_rendering(cb, 2);
    compute_write_barrier(cb);  // NX_LAZY_COMPUTE_BARRIER: the composition copies what the dispatches wrote
    out = upload_alloc(std::max(padded, size), std::max<uint64_t>(align, 16));
    std::memcpy(out.ptr, p, size);
    if (padded > size) std::memset(out.ptr + size, 0, padded - size);
    copy_regions(cb, regions, p, size, out);
    g_composed++;
    if ((cuts & kCutComposedCache) && g_composed_used < kComposedSlots / 2) {
        ComposedSlot* slot = composed_slot(p, size, padded, align);
        if (slot->generation != g_composed_generation) g_composed_used++;
        *slot = ComposedSlot{};
        slot->generation = g_composed_generation;
        slot->version = version;
        slot->regions = regions.size();
        slot->p = p;
        slot->lo = regions.front()->first;
        slot->hi = regions.back()->first;
        slot->size = size;
        slot->padded = padded;
        slot->align = align;
        slot->up = out;
    }
    return true;
}

// Direct vertex/index reads have the same untracked mapping lifetime as direct writes, so they also fall back.
bool direct_reads() {
    static const bool requested = std::getenv("NX_DIRECT_READS") != nullptr;
    if (requested) warn_direct_import_disabled();
    return false;
}

Upload read_upload(VkCommandBuffer cb, const uint8_t* p, uint64_t size, uint64_t padded, uint64_t align) {
    Upload u;
    if (written_read(cb, p, size, padded, align, u)) return u;
    // align <= 16 is how the vertex (16) and index (1/2/4) paths call this; uniform blocks (256) keep their snapshot.
    if (direct_reads() && p && size && align <= 16) {
        const uint64_t need = std::max<uint64_t>(align, 4);
        Upload d = host_import_upload(p, std::max(size, padded), size);
        if (d.buffer && d.offset % need == 0) {
            g_direct_reads++;
            g_direct_read_bytes += size;
            return d;
        }
    }
    return dedup_upload(p, size, padded, align, async_gpu());
}

bool written_overlaps(const uint8_t* p, uint64_t size) {
    return p && size && !g_regions.empty() && !overlapping(p, size).empty();
}

uint64_t written_texture_version(const uint8_t* p, uint64_t size) {
    uint64_t version = 0;
    if (p && size && !written_separate())
        for (It it : overlapping(p, size)) version = std::max(version, it->second.version);
    return version;
}

bool written_texture_stale(const Texture& t, const GpuImage& img) {
    // Render targets and WriteTexels have authoritative image-only contents.
    return !img.rendered && !img.texels &&
           written_texture_version(reinterpret_cast<const uint8_t*>(t.memory), t.storage_size) > img.uploaded_write_version;
}

void require_host_readable(const uint8_t* p, uint64_t size, const char* operation) {
    if (!written_overlaps(p, size)) return;
    const std::vector<It>& regions = overlapping(p, size);
    if (std::all_of(regions.begin(), regions.end(), [](It it) { return it->second.carried; }))
        return frame_carry_wait();  // nothing of this submit wrote them: they are the guest's bytes once that one landed
    fatal("nvn: %s requires CPU conversion of GPU-written bytes (mid-submit readback unsupported)", operation);
}

bool written_indirect(VkCommandBuffer cb, const uint8_t* p) {
    Upload u;
    if (!written_read(cb, p, 12, 16, 4, u)) return false;
    copy_write_barrier(cb);  // NX_REPLAY_CUTS=barrier: the dispatch arguments this reads may be a composition's copy
    p_vkCmdDispatchIndirect(cb, u.buffer, u.offset);  // {x, y, z} as the earlier dispatches left them
    g_indirect++;
    return true;
}

// NX_INPUT_PROF=1: the three things draw_inputs does per stream -- the written-range scan, the upload lookup/copy
// and the index path -- timed apart, so "inputs 7.4 us a draw" can be acted on instead of guessed at.
bool input_prof() {
    static const bool on = std::getenv("NX_INPUT_PROF") != nullptr;
    return on;
}
uint64_t g_in_written_ns = 0, g_in_upload_ns = 0, g_in_index_ns = 0, g_in_calls = 0, g_in_streams = 0, g_in_bytes = 0;
void input_prof_log(uint64_t frames) {
    if (!input_prof() || !frames || !g_in_calls) return;
    log("nvn/input-prof: written %.2f ms/frame (%.0f ns/call) upload %.2f ms/frame (%.0f ns/call) index %.2f "
        "ms/frame, %.2f streams/draw, %.2f MB/frame of bound vertex range",
        double(g_in_written_ns) / 1e6 / double(frames), double(g_in_written_ns) / double(g_in_calls),
        double(g_in_upload_ns) / 1e6 / double(frames), double(g_in_upload_ns) / double(g_in_calls),
        double(g_in_index_ns) / 1e6 / double(frames), double(g_in_streams) / double(g_in_calls),
        double(g_in_bytes) / 1e6 / double(frames));
    g_in_written_ns = g_in_upload_ns = g_in_index_ns = g_in_calls = g_in_streams = g_in_bytes = 0;
}

bool draw_inputs(VkCommandBuffer cb, const ShaderInfo* vs, bool indexed, const Cmd& c, DrawInputs& in) {
    // Vertex buffers: the bound ranges of the streams the vertex shader reads (the pipeline declares no others).
    for (const ShaderIo& io : vs->refl.inputs)
        if (io.location < g_state.attribs.size() && g_state.attribs[io.location].stream < 15)
            in.used |= 1u << g_state.attribs[io.location].stream;
    const bool prof = input_prof();
    if (prof) g_in_calls++;
    for (uint32_t s = 0; s < 15; s++) {
        if (!(in.used >> s & 1)) continue;
        const Range& r = g_state.vertex_buffers[s];
        const uint8_t* p = r.address ? guest_bytes(r.address) : nullptr;
        uint64_t size = p ? std::min<uint64_t>(r.size, 64ull << 20) : 0;
        const uint64_t t0 = prof ? plat::monotonic_ns() : 0;
        in.vertex[s] = read_upload(cb, p, size, std::max<uint64_t>((size + 15) & ~15ull, 16), 16);
        if (prof) g_in_streams++, g_in_bytes += size, g_in_upload_ns += plat::monotonic_ns() - t0;
    }
    if (!indexed) return true;
    uint32_t count = uint32_t(c.i[0]);
    const uint8_t* p = guest_bytes(c.a[0]);
    if (!p) return false;
    in.index_type = vk_index_type(c.mask);
    if (in.index_type == VK_INDEX_TYPE_UINT8_EXT) {
        // 8-bit indices: Metal has none. CPU widening must not read the stale
        // guest backing of a dispatch/copy result still pending on the GPU.
        require_host_readable(p, count, "8-bit index widening");
        in.index = upload_alloc(uint64_t(count) * 2, 16);
        auto* d = reinterpret_cast<uint16_t*>(in.index.ptr);
        for (uint32_t k = 0; k < count; k++) d[k] = p[k];
        in.index_type = VK_INDEX_TYPE_UINT16;
        return true;
    }
    uint64_t bytes = uint64_t(count) * index_size(c.mask);
    if (!written_read(cb, p, bytes, bytes, index_size(c.mask), in.index)) in.index = upload(p, bytes, 16);
    return true;
}

// NX_INPUT_BYTES=1: how much of each bound vertex range this draw actually fetches. The upload path copies the whole
// bound range (draw_inputs above), so this ratio bounds what a fetch-extent upload could save and tells whether the
// 50+ MB a frame is genuinely demanded by the draws or is range slack.
void input_bytes_note(const ShaderInfo* vs, bool indexed, const Cmd& c, const DrawInputs& in) {
    static const bool on = std::getenv("NX_INPUT_BYTES") != nullptr;
    if (!on) return;
    static uint64_t bound_sum = 0, need_sum = 0, draws = 0, capped = 0;
    static std::map<uint64_t, uint64_t> histogram;  // bound/need ratio bucketed in eighths
    uint32_t count = uint32_t(indexed ? c.i[0] : c.i[1]);
    if (!count) return;
    uint64_t lo = 0, hi = 0;
    if (indexed) {
        const uint8_t* p = guest_bytes(c.a[0]);
        if (!p) return;
        uint32_t isz = index_size(c.mask), restart = isz == 1 ? 0xFF : isz == 2 ? 0xFFFF : 0xFFFFFFFFu;
        uint32_t mn = UINT32_MAX, mx = 0;
        for (uint32_t k = 0; k < count; k++) {
            uint32_t v = isz == 1 ? p[k] : isz == 2 ? reinterpret_cast<const uint16_t*>(p)[k] : reinterpret_cast<const uint32_t*>(p)[k];
            if (v == restart) continue;
            mn = std::min(mn, v), mx = std::max(mx, v);
        }
        if (mn == UINT32_MAX) return;
        lo = uint64_t(int64_t(mn) + c.i[1]), hi = uint64_t(int64_t(mx) + c.i[1]);
    } else {
        lo = uint32_t(c.i[0]), hi = uint64_t(uint32_t(c.i[0])) + count - 1;
    }
    uint64_t inst_hi = uint64_t(uint32_t(c.i[2])) + uint32_t(std::max(1, c.i[3])) - 1;
    draws++;
    for (const ShaderIo& io : vs->refl.inputs) {
        if (io.location >= g_state.attribs.size()) continue;
        const VertexAttrib& va = g_state.attribs[io.location];
        if (va.stream >= 15 || va.stream >= g_state.streams.size() || !(in.used >> va.stream & 1)) continue;
        const VertexStream& st = g_state.streams[va.stream];
        const Range& r = g_state.vertex_buffers[va.stream];
        const uint64_t bound = r.address && guest_bytes(r.address) ? std::min<uint64_t>(r.size, 64ull << 20) : 0;
        const uint64_t last = st.divisor ? inst_hi / st.divisor : hi;
        const uint64_t need = std::min(bound, last * st.stride + va.offset + vk_block_bytes(vk_vertex_format(va.format)));
        bound_sum += bound, need_sum += need;
        if (bound > (16u << 20)) capped++;
        if (need) {
            const uint64_t ratio8 = std::min<uint64_t>(bound / std::max<uint64_t>(need, 1), 64);
            histogram[ratio8]++;
        }
    }
    if (draws % 300 == 0) {
        std::string top;
        for (auto& [r, n] : histogram)
            if (n >= draws / 20) top += " " + std::to_string(r) + "x:" + std::to_string(n);
        log("nvn/input-bytes: %llu draws, bound %.1f MB/frame, needed %.1f MB/frame (%.0f%% fetched), over 16 MB: %llu; "
            "bound/need ratios (>=5%% of draws)%s",
            (unsigned long long)draws, double(bound_sum) / 1e6 / double(draws) * 1.0, double(need_sum) / 1e6 / double(draws),
            need_sum ? 100.0 * double(need_sum) / double(bound_sum) : 0.0, (unsigned long long)capped, top.c_str());
        bound_sum = need_sum = draws = capped = 0, histogram.clear();
    }
}

void check_vertex_ranges(const ShaderInfo* vs, const ShaderInfo* fs, bool indexed, const Cmd& c, const DrawInputs& in) {
    static const bool on = std::getenv("NX_CHECK_VERTS") != nullptr;
    static uint64_t found = 0;
    if (!on || !vs) return;
    // The vertices and instances the draw fetches.
    uint64_t lo = 0, hi = 0;  // vertex indices [lo, hi]
    uint32_t count = uint32_t(indexed ? c.i[0] : c.i[1]);
    if (!count) return;
    if (indexed) {
        const uint8_t* p = guest_bytes(c.a[0]);
        if (!p) return;
        uint32_t isz = index_size(c.mask), restart = isz == 1 ? 0xFF : isz == 2 ? 0xFFFF : 0xFFFFFFFFu;
        uint32_t mn = UINT32_MAX, mx = 0;
        for (uint32_t k = 0; k < count; k++) {
            uint32_t v = isz == 1 ? p[k] : isz == 2 ? reinterpret_cast<const uint16_t*>(p)[k] : reinterpret_cast<const uint32_t*>(p)[k];
            if (v == restart) continue;
            mn = std::min(mn, v), mx = std::max(mx, v);
        }
        if (mn == UINT32_MAX) return;
        lo = uint64_t(int64_t(mn) + c.i[1]), hi = uint64_t(int64_t(mx) + c.i[1]);
    } else {
        lo = uint32_t(c.i[0]), hi = uint64_t(uint32_t(c.i[0])) + count - 1;
    }
    uint64_t inst_hi = uint64_t(uint32_t(c.i[2])) + uint32_t(std::max(1, c.i[3])) - 1;
    for (const ShaderIo& io : vs->refl.inputs) {
        if (io.location >= g_state.attribs.size()) continue;
        const VertexAttrib& va = g_state.attribs[io.location];
        if (va.stream >= 15 || va.stream >= g_state.streams.size() || !(in.used >> va.stream & 1)) continue;
        const VertexStream& st = g_state.streams[va.stream];
        const Range& r = g_state.vertex_buffers[va.stream];
        uint64_t last = st.divisor ? inst_hi / st.divisor : hi;
        uint64_t end = last * st.stride + va.offset + vk_block_bytes(vk_vertex_format(va.format));
        uint64_t copied = r.address && guest_bytes(r.address) ? std::min<uint64_t>(r.size, 64ull << 20) : 0;
        if (end <= copied) continue;
        found++;
        static std::map<std::string, uint64_t> seen;
        std::string key = vs->hash + (fs ? fs->hash : std::string("-")) + char('a' + va.stream);
        uint64_t n = ++seen[key];
        if (n <= 3 || (n & (n - 1)) == 0)
            log("nvn: vertex fetch past the bound range (%llu so far; #%llu of vs %s fs %s): stream %u loc %u fmt %#x off %u "
                "stride %u div %u, vertices %llu..%llu instances ..%llu, needs %llu bytes, bound %llu at %llx (copied %llu)",
                (unsigned long long)found, (unsigned long long)n, vs->hash.c_str(), fs ? fs->hash.c_str() : "-", va.stream,
                io.location, va.format, va.offset, st.stride, st.divisor, (unsigned long long)lo, (unsigned long long)hi,
                (unsigned long long)inst_hi, (unsigned long long)end, (unsigned long long)r.size,
                (unsigned long long)r.address, (unsigned long long)copied);
        break;
    }
}

void written_cpu_write(const uint8_t* p, uint64_t size) {
    // A composed copy of these bytes is not what a later bind would read now: restart the submit's cache, exactly as the
    // dedup_invalidate() call beside this one drops dedup's copy (DEBUG.md #30). The cache holds a few keys per frame,
    // so a restart costs a couple of re-copies.
    if (replay_cuts() & kCutComposedCache) g_composed_generation++, g_composed_used = 0;
    // NX_CARRY: a range only carried over is the guest's again once its submit has landed, and later reads want these
    // bytes with it (a word the guest wrote before that landing stays the guest's).
    if (!g_regions.empty() && p && size) {
        bool landed = false;
        for (It it : std::vector<It>(overlapping(p, size))) {
            if (!it->second.carried) continue;
            if (!landed) frame_carry_wait(), landed = true;
            g_regions.erase(it);
            g_spans_stale = true;
            g_carry_erased++;
        }
    }
    // UpdateUniformBuffer / ReportCounter into a range a dispatch of this submit writes: the copy misses the bytes
    // (they reach guest memory at replay time) and the write-back replaces them. Not handled: logged.
    static int logs = 0;
    if (written_overlaps(p, size) && logs++ < 16)
        log("nvn: guest write into a compute-written range (%llu bytes): lost at write-back", (unsigned long long)size);
}

// NX_LATE_GUARD=1: the evidence NX_EARLY_COMPLETE rests on, from this runtime rather than from Witch-Time's. From the
// guest's release to the landing, the whole pages of every range are inaccessible. An access is recorded and let through;
// at the landing, a read of a page the GPU did not change saw the right bytes anyway, so what counts is the pages the
// write-back is about to change.
namespace {
uint64_t g_guard_ranges = 0, g_guard_bytes = 0, g_guard_total = 0, g_guard_hits = 0, g_guard_reads = 0, g_guard_writes = 0,
         g_guard_stale = 0;
int g_guard_logged = 0;
}  // namespace

void late_guard_arm(const std::vector<WriteBack>& v) {
    for (const WriteBack& w : v) {
        g_guard_ranges++, g_guard_total += w.size;
        g_guard_bytes += plat::late_guard_add(reinterpret_cast<uint64_t>(w.to), w.size);
    }
}

// NX_FENCE_AT_CMD: the guest runs from the FenceSync on, so the results of the dispatches before it are guarded from there.
void late_guard_arm_written() {
    for (const auto& [p, r] : g_regions) {
        if (r.direct) continue;
        g_guard_ranges++, g_guard_total += r.size;
        g_guard_bytes += plat::late_guard_add(reinterpret_cast<uint64_t>(p), r.size);
    }
}

void late_guard_land(const std::vector<WriteBack>& v) {
    plat::LateGuardEvent events[32];
    const uint32_t seen = plat::late_guard_open(events, 32);
    g_guard_hits += seen;
    for (uint32_t k = 0; k < std::min(seen, 32u); k++) {
        const plat::LateGuardEvent& e = events[k];
        (e.write ? g_guard_writes : g_guard_reads)++;
        bool stale = false;
        for (const WriteBack& w : v) {
            const uint64_t to = reinterpret_cast<uint64_t>(w.to);
            if (e.addr < to || e.addr >= to + w.size || !w.before) continue;
            const uint64_t page = (e.addr & ~uint64_t(4095)) - to;
            stale = std::memcmp(w.from + page, w.before->data() + page, 4096) != 0;
        }
        g_guard_stale += stale;
        if (g_guard_logged++ < 64) {
            char comm[32] = "?", path[64];
            std::snprintf(path, sizeof path, "/proc/self/task/%llu/comm", (unsigned long long)e.tid);
            if (FILE* f = std::fopen(path, "r")) {
                if (std::fgets(comm, sizeof comm, f)) comm[std::strcspn(comm, "\n")] = 0;
                std::fclose(f);
            }
            log("nvn: late guard: %s of %llx by thread %llu (%s) at host %llx: the GPU %s that page",
                e.write ? "write" : "read", (unsigned long long)e.addr, (unsigned long long)e.tid, comm,
                (unsigned long long)e.pc, stale ? "CHANGED" : "did not change");
        }
    }
}

std::vector<WriteBack> written_take() {
    std::vector<WriteBack> v;
    v.reserve(g_regions.size());
    // The newest version among the ranges this submit hands back. It is what makes the split guard exact
    // (written_complete_since): a range re-bound after the split was written by the later part, so this submit's copy of
    // it is only complete once the whole submit is done. Read here because g_regions is cleared below -- an earlier
    // version of this test walked g_regions from the caller, which the argument evaluation order had already emptied.
    g_taken_version = 0;
    for (auto& [p, r] : g_regions) {
        g_taken_version = std::max(g_taken_version, r.version);
        if (!r.direct)  // the GPU already wrote guest memory: nothing to copy back
            v.push_back({r.up.ptr, const_cast<uint8_t*>(p), r.size, r.before, r.whole, false, r.up.buffer, r.up.offset,
                         r.cap, r.wrote, {}});
    }
    g_regions.clear();
    g_spans_stale = true;
    return v;
}

void written_reset() {
    // Called under gpu_lock after frame_begin and before recording this submit.
    // A split may have landed write-backs but still have later parts in flight;
    // wait for the *entire* prior submit before reading, reusing or destroying
    // any retained buffer. The normal path pays none of this when disabled.
    if (keep_copies() && !g_kept.empty()) {
        frame_wait_all();
        if (g_kept_evict_pending) kept_clear();
    }
    g_regions.clear(), g_spans_stale = true, g_read = false;
    g_copies_pending = false;  // a new command buffer begins with a full barrier (frame_begin / begin_part)
    if (replay_counts()) g_read_generation++, g_read_used = 0;                 // the repeat rate is per submit
    if (replay_cuts() & kCutComposedCache) g_composed_generation++, g_composed_used = 0;  // the cache lives one submit
}

// NX_CARRY=1: `all` is a submit still in flight past its split, with every older one retired. What the parts after the
// split cannot have written is complete and goes to land_now, and the rest stays in `all` for its landing, each with a
// snapshot to fill then. False, with `all` untouched: the caller waits for the submit as before. The guest must not have
// written a carried range since that submit copied it: the next submit's copy would not have the bytes.
bool written_carry_begin(std::vector<WriteBack>& all, uint64_t writable_at_split, std::vector<WriteBack>& land_now) {
    static const uint64_t cap = [] {
        const char* e = std::getenv("NX_CARRY_MAX_KB");
        return (e ? uint64_t(std::strtoull(e, nullptr, 10)) : 8192ull) << 10;
    }();
    uint64_t bytes = 0;
    uint32_t ranges = 0;
    for (const WriteBack& w : all) {
        if (w.wrote <= writable_at_split) continue;
        bytes += w.size, ranges++;
        if (!w.before || !w.buffer) {
            g_carry_refused_guest++;
            return false;
        }
    }
    if (bytes > cap) {
        g_carry_refused_size++;
        return false;
    }
    for (const WriteBack& w : all)
        if (w.wrote > writable_at_split && std::memcmp(w.to, w.before->data(), size_t(w.size)) != 0) {
            g_carry_refused_guest++;
            return false;
        }
    std::vector<WriteBack> keep;
    keep.reserve(ranges);
    for (WriteBack& w : all) {
        if (w.wrote <= writable_at_split) {
            land_now.push_back(std::move(w));
            continue;
        }
        w.after = snapshot_room(w.size);
        keep.push_back(std::move(w));
    }
    all = std::move(keep);
    g_carry_pending = all;
    g_carry_submits++, g_carry_ranges += ranges, g_carry_bytes += bytes;
    return true;
}

void written_carry_seed(VkCommandBuffer cb) {
    if (g_carry_pending.empty()) return;
    for (WriteBack& w : g_carry_pending) {
        Region r;
        r.version = ++g_write_version;
        r.size = w.size;
        r.cap = w.cap;
        r.up = alloc_anchored(w.cap, kAlign - w.offset % kAlign);  // what is aligned in the old copy stays aligned
        std::memset(r.up.ptr + w.size, 0, size_t(w.cap - w.size));
        r.before = w.after;
        r.carried = true;
        const VkBufferCopy c{w.offset, r.up.offset, w.size};
        p_vkCmdCopyBuffer(cb, w.buffer, r.up.buffer, 1, &c);  // after the old submit: frame_begin's barrier
        g_regions.emplace(w.to, std::move(r));
    }
    g_carry_pending.clear();
    g_spans_stale = true;
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, nullptr, 0,
                           nullptr);
}

static void written_carry_log() {
    if (!g_carry_submits && !g_carry_refused_size && !g_carry_refused_guest) return;
    log("nvn: carried %llu submits, %.1f ranges and %.1f KB each; refused %llu for size, %llu for guest writes; %llu "
        "dropped for a replay-time write (NX_CARRY)",
        (unsigned long long)g_carry_submits, double(g_carry_ranges) / double(std::max<uint64_t>(g_carry_submits, 1)),
        double(g_carry_bytes) / 1024.0 / double(std::max<uint64_t>(g_carry_submits, 1)),
        (unsigned long long)g_carry_refused_size, (unsigned long long)g_carry_refused_guest,
        (unsigned long long)g_carry_erased);
    g_carry_submits = g_carry_ranges = g_carry_bytes = g_carry_refused_size = g_carry_refused_guest = g_carry_erased = 0;
}

void written_kept_invalidate_all() { kept_invalidate_all(); }

// Call only while holding gpu_lock, after every submit has retired and
// vkQueueWaitIdle has succeeded. Safe to call repeatedly and before re-init.
void written_kept_shutdown() { kept_clear(); }

// NX_COPYIN_TIMING: one count per replayed submit, from draw_begin_submit (nvn_draw.cpp).
void copyin_count_frame() { g_log_submits++; }

// Work is attributed to the interval in which it is recorded or retired. Async
// retirements may fall in a later interval than the submit that produced them.
void written_log(uint64_t window) {
    const double frames = double(window ? window : 1);
    written_carry_log();
    if (g_shared + g_merged + g_composed + g_indirect + g_imported_binds)
        log("nvn: compute ranges: %llu bindings of written copies, %llu merged, %llu reads composed on the GPU, %llu indirect, "
            "%llu host imports (NX_HOST_IMPORT)",
            (unsigned long long)g_shared, (unsigned long long)g_merged, (unsigned long long)g_composed,
            (unsigned long long)g_indirect, (unsigned long long)g_imported_binds);
    if (g_whole_writes.load() + g_merge_writes.load())
        log("nvn: write-backs: %llu whole (%.1f MB, no guest snapshot), %llu merged (%.1f MB)",
            (unsigned long long)g_whole_writes.load(), double(g_whole_bytes.load()) / 1e6,
            (unsigned long long)g_merge_writes.load(), double(g_merge_bytes.load()) / 1e6);
    if (copyin_timing() && window)
        log("nvn: copy-in %.2f ms/frame, %.1f MB/frame (guest->ring, NX_COPYIN_TIMING; %llu submits over %llu presents)",
            double(g_copyin_ns.load()) / 1e6 / frames,
            double(g_copyin_bytes.load()) / 1e6 / frames, (unsigned long long)g_log_submits, (unsigned long long)window);
    if (g_wb_batches.load() && window)
        log("nvn: write-back wall %.2f ms/frame per batch (%.2f ranges, %.1f MB, NX_COPY_THREADS=%u, window=%llu presents)",
            double(g_wb_wall_ns.load()) / 1e6 / frames,
            double(g_wb_ranges.load()) / double(g_wb_batches.load()),
            double(g_merge_bytes.load()) / 1e6 / frames,
            copy_threads(), (unsigned long long)window);
    if (g_kept_hits + g_kept_refills)
        log("nvn: kept copies (NX_P2_KEPT_COPY): %llu reuses (%.1f MB of GPU-buffer re-fills avoided), %llu re-fills, %zu ranges",
            (unsigned long long)g_kept_hits, double(g_kept_saved) / 1e6, (unsigned long long)g_kept_refills, g_kept.size());
    if (g_guard_ranges) {
        log("nvn: late guard: %llu ranges, %.1f of %.1f MB guarded: %llu accesses before their landing (%llu reads, %llu "
            "writes), %llu on pages the GPU changed",
            (unsigned long long)g_guard_ranges, double(g_guard_bytes) / 1e6, double(g_guard_total) / 1e6,
            (unsigned long long)g_guard_hits, (unsigned long long)g_guard_reads, (unsigned long long)g_guard_writes,
            (unsigned long long)g_guard_stale);
        g_guard_ranges = g_guard_bytes = g_guard_total = g_guard_hits = g_guard_reads = g_guard_writes = g_guard_stale = 0;
    }
    if (const uint64_t words = g_late_conflicts.exchange(0, std::memory_order_relaxed); words)
        log("nvn: write-backs after the guest was released kept %llu words the guest had changed (NX_EARLY_COMPLETE)",
            (unsigned long long)words);
    if (const uint64_t jobs = copy_behind::queue().take_jobs(); jobs && window) {
        const uint64_t bytes = copy_behind::queue().take_bytes(), waited = copy_behind::queue().take_wait_ns();
        log("nvn: copy-behind: %.1f MB/frame in %.0f slices/frame on %u helpers, the replay waited %.2f ms/frame (%.1f "
            "waits) and %.2f ms/frame for decodes",
            double(bytes) / 1e6 / frames, double(jobs) / frames, copy_behind::helpers(), double(waited) / 1e6 / frames,
            double(copy_behind::queue().take_waits()) / frames, double(copy_behind::lane().take_wait_ns()) / 1e6 / frames);
    }
    // Bytes and time spent snapshotting merge-path ranges in written_bind.
    if (g_snap_bytes.load() && window)
        log("nvn: write-back snapshots: %.1f MB/frame copied in %.2f ms/frame",
            double(g_snap_bytes.load()) / 1e6 / frames, double(g_snap_ns.load()) / 1e6 / frames);
    if (g_direct_reads && window)
        log("nvn: direct reads (NX_DIRECT_READS): %llu vertex/index binds, %.1f MB/frame straight from guest memory",
            (unsigned long long)g_direct_reads, double(g_direct_read_bytes) / 1e6 / frames);
    // NX_REPLAY_COUNTS: the composed-read path split by why containing() refused it, its sizes, the repeats of an
    // (address, size) within a submit (the cache's upside) and the copies/barriers the window recorded. Counts are the
    // window is the number of presents in this logging interval, not the number of submits.
    if (replay_counts()) {
        log("nvn: replay counts (NX_REPLAY_COUNTS, %llu-present window): %llu composed reads (%.2f MB/frame, %llu under "
            "64 KiB); why: %llu cross/meet, %llu past end, %llu past cap, %llu misaligned; size: %llu <256 B, %llu <1 K, "
            "%llu <4 K, %llu <16 K, %llu <64 K, %llu up; keys: %llu distinct (%llu repeats); %llu answered from the copy "
            "cache; %llu copy_regions calls, %llu vkCmdCopyBuffer, %llu transfer barriers",
            (unsigned long long)window, (unsigned long long)(g_composed + g_composed_cache_hits), double(g_composed_bytes) / 1e6 / frames,
            (unsigned long long)g_composed_small, (unsigned long long)g_composed_why[kWhyMany],
            (unsigned long long)g_composed_why[kWhyPastEnd], (unsigned long long)g_composed_why[kWhyPastCap],
            (unsigned long long)g_composed_why[kWhyMisaligned], (unsigned long long)g_composed_size[0],
            (unsigned long long)g_composed_size[1], (unsigned long long)g_composed_size[2],
            (unsigned long long)g_composed_size[3], (unsigned long long)g_composed_size[4],
            (unsigned long long)g_composed_size[5], (unsigned long long)g_composed_distinct,
            (unsigned long long)g_composed_repeat, (unsigned long long)g_composed_cache_hits,
            (unsigned long long)g_copy_calls, (unsigned long long)g_copy_cmds, (unsigned long long)g_copy_barriers);
        g_composed_bytes = g_composed_small = g_composed_small_bytes = 0;
        std::fill(std::begin(g_composed_why), std::end(g_composed_why), 0ull);
        std::fill(std::begin(g_composed_size), std::end(g_composed_size), 0ull);
        g_composed_repeat = g_composed_distinct = 0;  // the hit count belongs to the cache line below
        g_copy_calls = g_copy_cmds = g_copy_barriers = 0;
    }
    // NX_REPLAY_COUNTS with the cache on: how often the guest thread wrote bytes a composed copy had already taken, the
    // one assumption the cache shares with dedup_upload (DEBUG.md #30). 0 over a run: the cache is content-safe there.
    if (replay_counts() && (replay_cuts() & kCutComposedCache)) {
        log("nvn: composed cache (NX_REPLAY_CUTS=cache): %llu of %llu hits found guest bytes changed since the copy%s",
            (unsigned long long)g_cache_hazard, (unsigned long long)g_composed_cache_hits,
            g_cache_hazard ? " (the first key is in the line below)" : "");
        if (g_cache_hazard)
            log("nvn: composed cache: first changed key %p, at hit %llu of the window",
                static_cast<const void*>(g_cache_hazard_at), (unsigned long long)g_cache_hazard_first_hit);
        g_cache_hazard = 0, g_cache_hazard_at = nullptr, g_cache_hazard_first_hit = 0, g_composed_cache_hits = 0;
    } else {
        g_composed_cache_hits = 0;  // the cache is off, or the counters are: nothing reads the hits
    }
    g_shared = g_merged = g_composed = g_indirect = 0, g_imported_binds = 0;
    g_whole_writes = 0, g_whole_bytes = 0, g_merge_writes = 0, g_merge_bytes = 0;
    g_wb_wall_ns = 0, g_wb_batches = 0, g_wb_ranges = 0;
    g_copyin_ns = 0, g_copyin_bytes = 0, g_log_submits = 0;
    g_kept_hits = g_kept_saved = g_kept_refills = 0;
    g_direct_reads = g_direct_read_bytes = 0;
    g_snap_ns = 0, g_snap_bytes = 0;
}

}  // namespace nx::nvn::replay

namespace nx::nvn {

namespace {

// The whole-range path: a transfer filled the range, or a caller explicitly
// requested the NX_WRITE_BACK_WHOLE override.
bool write_back_whole(const WriteBack& w) {
    static const bool copy_whole = std::getenv("NX_WRITE_BACK_WHOLE") != nullptr;
    return copy_whole || w.whole || !w.before || w.before->size() < w.size;
}

// NX_WRITE_BACK_DIRECT=1 (default): compare against the GPU's copy in place instead of copying it into a host buffer
// first. The upload ring is host-cached memory on this host (nvn_vk.cpp logs the memory type), so a sequential read is
// as fast as the memcpy; NX_WRITE_BACK_DIRECT=0 keeps the copy for hosts whose GPU mappings are uncached.
bool write_back_direct() {
    static const bool direct = [] {
        const char* e = std::getenv("NX_WRITE_BACK_DIRECT");
        return e ? std::atoi(e) != 0 : true;
    }();
    return direct;
}

// NX_WRITE_BACK_SIMD=1: opt-in AVX2 lane comparisons and selective stores.
// Unset/0 retains the scalar kernel; non-AVX2 builds always use it. This changes
// neither snapshot selection nor cross-submit validity.
bool write_back_simd() {
    static const bool on = [] {
        const char* e = std::getenv("NX_WRITE_BACK_SIMD");
        return merge_detail::kHasAvx2 && e && e[0] == '1' && !e[1];
    }();
    return on;
}

// Merge one slice against the complete bind-time snapshot. Only GPU-changed
// four-byte words (and final tail bytes) replace guest bytes.
//
// A late landing runs after the guest saw the submit finish. A guest write since then comes after the GPU's, so a word
// that no longer holds its snapshot value stays the guest's. Writes the guest made while the submit ran are ordered the
// other way round on the GPU, and are the same conflict to this test; the game does not do that, and written_log counts
// any word this rule kept so a run shows it.
#if defined(__ARM_NEON) && !defined(__AVX2__)
// Whether a[0..n) == b[0..n), n a multiple of 32: XORs folded with OR, one horizontal max at the end.
inline bool neon_same(const uint8_t* a, const uint8_t* b, uint32_t n) {
    uint8x16_t x = vdupq_n_u8(0);
    for (uint32_t k = 0; k < n; k += 32) {
        x = vorrq_u8(x, veorq_u8(vld1q_u8(a + k), vld1q_u8(b + k)));
        x = vorrq_u8(x, veorq_u8(vld1q_u8(a + k + 16), vld1q_u8(b + k + 16)));
    }
    return vmaxvq_u32(vreinterpretq_u32_u8(x)) == 0;
}
#endif

void merge_slice(const WriteBack& w, uint64_t offset, uint64_t count) {
    const uint8_t* src = w.from + offset;
    const uint8_t* was = w.before->data() + offset;
    uint8_t* to = w.to + offset;
    std::vector<uint8_t> snapshot;
    if (!write_back_direct()) {
        snapshot.resize(size_t(count));
        std::memcpy(snapshot.data(), src, size_t(count));
        src = snapshot.data();
    }
    if (write_back_simd() && !w.late) {
        merge_detail::merge<false>(src, was, to, size_t(count), [](uint64_t) {});
        return;
    }
    uint64_t conflicts = 0;
    auto word = [&](const uint8_t* s, const uint8_t* c, uint8_t* d) {
        if (w.late && std::memcmp(d, c, 4) != 0) {
            conflicts++;
            static const bool diag = std::getenv("NX_CARRY_CHECK") != nullptr;
            static std::atomic<int> shown{0};
            if (diag && shown.fetch_add(1, std::memory_order_relaxed) < 48) {
                uint32_t gpu, was, guest;
                std::memcpy(&gpu, s, 4), std::memcpy(&was, c, 4), std::memcpy(&guest, d, 4);
                log("nvn: kept word +%llx of %p (%llu bytes): GPU %08x, snapshot %08x, guest %08x",
                    (unsigned long long)(d - w.to), static_cast<void*>(w.to), (unsigned long long)w.size, gpu, was, guest);
            }
            return;
        }
        std::memcpy(d, s, 4);
    };
    auto block32 = [&](const uint8_t* s, const uint8_t* c, uint8_t* d) {
        // Most blocks are unchanged, and a call per 32 bytes of 25 MB a frame is a measurable part of the replay thread.
#if defined(__AVX2__)
        const __m256i diff = _mm256_xor_si256(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(s)),
                                              _mm256_loadu_si256(reinterpret_cast<const __m256i*>(c)));
        if (_mm256_testz_si256(diff, diff)) return;
#elif defined(__ARM_NEON)
        if (neon_same(s, c, 32)) return;
#else
        if (std::memcmp(s, c, 32) == 0) return;
#endif
        for (uint32_t k = 0; k < 32; k += 8) {
            const uint64_t a = replay::load64(s + k), b = replay::load64(c + k);
            if (a == b) continue;
            if (uint32_t(a) != uint32_t(b)) word(s + k, c + k, d + k);
            if (uint32_t(a >> 32) != uint32_t(b >> 32)) word(s + k + 4, c + k + 4, d + k + 4);
        }
    };
    uint64_t k = 0;
#if defined(__ARM_NEON) && !defined(__AVX2__)
    // Most of a frame's write-back is unchanged: skip it 128 bytes at a time, inline (a memcmp call per 32 bytes was
    // __memcmp_aarch64 plus its call overhead on an Adreno 740 phone's replay thread, about 2% of it).
    for (; k + 128 <= count; k += 128) {
        if (neon_same(src + k, was + k, 128)) continue;
        for (uint32_t j = 0; j < 128; j += 32) block32(src + k + j, was + k + j, to + k + j);
    }
#endif
    for (; k + 32 <= count; k += 32) block32(src + k, was + k, to + k);
    for (; k + 8 <= count; k += 8) {
        const uint64_t a = replay::load64(src + k), b = replay::load64(was + k);
        if (a == b) continue;
        if (uint32_t(a) != uint32_t(b)) word(src + k, was + k, to + k);
        if (uint32_t(a >> 32) != uint32_t(b >> 32)) word(src + k + 4, was + k + 4, to + k + 4);
    }
    for (; k < count; ++k) {
        if (src[k] == was[k]) continue;
        if (w.late && to[k] != was[k]) conflicts++;
        else to[k] = src[k];
    }
    if (conflicts) replay::g_late_conflicts.fetch_add(conflicts, std::memory_order_relaxed);
}

struct Job {
    uint32_t wb;
    uint64_t off, count;
    bool whole;
};

}  // namespace

// NX_COPY_THREADS (default 6, 0 disables): a submit's compute results are independent guest ranges and the submitting
// thread waits for all of them, so the copies and the word merges run on a few helper threads -- and, inside one large
// range, on slices of it (a single 16 MiB range is otherwise one thread's whole job). The words written to guest memory
// are disjoint, one slice per job. NX_COPY_THREADS=0 is the sequential path.
// The write-back wall measured on the Prologue battle's heavy set (rc9 -> rc11, docs/archive/PERF-BATTLE-2026-10-27.md):
// 2.83 ms/frame with 3 helpers, 1.91 ms/frame with 6 -- the merge scan is memory-bandwidth bound but not saturated at
// four workers on VanGogh (3 helpers + the replay thread, which helps too). NX_COPY_THREADS still overrides: 0 and 1
// are the serial controls that proved the helpers are worth ~2.6 ms, and nothing above 6 is measured.
unsigned copy_threads() {
    static const unsigned n = [] {
        const char* e = std::getenv("NX_COPY_THREADS");
        if (e) return unsigned(std::max(0, std::atoi(e)));
        return 6u;
    }();
    return n;
}

namespace {

// A fixed pool of N helpers: run(count, fn) calls fn(i) for every i in [0, count) here and on the helpers, and returns
// when all are done. Allocated once and never destroyed (no static-destruction race at exit).
class WorkPool {
public:
    void run(uint64_t count, const std::function<void(uint64_t)>& fn) {
        if (!count) return;
        std::unique_lock lk(m_);
        if (!started_) {
            started_ = true;
            threads_ = copy_threads();
            for (unsigned k = 0; k < threads_; k++) std::thread([this] { worker(); }).detach();
        }
        // busy_ counts the helper threads still draining: this thread helps in drain() below and is not counted (a
        // counted-but-never-decremented caller deadlocks the guest: the first version of this pool did exactly that).
        // Runs are serialized. The prior run waited for every helper before a
        // new counter is installed; m_ publishes the remaining generation state.
        fn_ = &fn, count_ = count, busy_ = threads_;
        next_.store(0, std::memory_order_relaxed);
        ++gen_;
        cv_.notify_all();
        lk.unlock();
        drain();
        lk.lock();
        // A helper that never finishes must be visible, not a silent hang of the guest.
        while (!done_cv_.wait_for(lk, std::chrono::seconds(2), [&] { return busy_ == 0; }))
            log("nvn: write-back workers still busy after %llu s of waiting", 2ull);
    }

private:
    void drain() {
        for (;;) {
            // Only unique claims need atomics. m_/gen_ publish fn_ and count_,
            // and the busy_ wait makes all helper callback writes visible.
            const uint64_t i = next_.fetch_add(1, std::memory_order_relaxed);
            if (i >= count_) return;
            // fn_ was published before ++gen_ (under m_), so it is visible here and stable for this generation.
            (*fn_)(i);
        }
    }
    void worker() {
        plat::set_thread_name("nvn wb");  // distinguish helper samples from the replay thread
        uint64_t seen = 0;
        for (;;) {
            std::unique_lock lk(m_);
            cv_.wait(lk, [&] { return gen_ != seen; });
            seen = gen_;
            lk.unlock();
            drain();
            lk.lock();
            if (--busy_ == 0) done_cv_.notify_all();
        }
    }
    std::mutex m_;
    std::condition_variable cv_, done_cv_;
    const std::function<void(uint64_t)>* fn_ = nullptr;
    uint64_t count_ = 0, gen_ = 0, busy_ = 0;
    std::atomic<uint64_t> next_{0};
    unsigned threads_ = 0;
    bool started_ = false;
};

WorkPool& work_pool() {
    static WorkPool* pool = new WorkPool();  // leaked on purpose: the helpers outlive static destruction
    return *pool;
}

}  // namespace

bool write_backs_can_land_late(const std::vector<WriteBack>& v) {
    for (const WriteBack& w : v)
        if (write_back_whole(w)) return false;
    return true;
}

// Write back disjoint guest ranges, with disjoint 8-byte-aligned slices within
// each range. Workers must finish before any snapshot can be recycled.
void write_backs_apply(std::vector<WriteBack>& v) {
    if (v.empty()) return;
    const uint64_t t_start = plat::monotonic_ns();
    if (v[0].late && plat::late_guard_on()) replay::late_guard_land(v);
    for (const WriteBack& w : v) replay::trace_nan_write_back(w.from, w.to, w.size);
    std::vector<char> whole(v.size());
    std::vector<Job> jobs;
    for (uint32_t i = 0; i < v.size(); i++) {
        whole[i] = write_back_whole(v[i]) ? 1 : 0;
        const uint64_t chunk = whole[i] ? (2ull << 20) : (1ull << 20);
        for (uint64_t off = 0; off < v[i].size; off += chunk)
            jobs.push_back({i, off, std::min(chunk, v[i].size - off), whole[i] != 0});
    }
    auto run = [&](uint64_t j) {
        const Job& job = jobs[j];
        const WriteBack& w = v[job.wb];
        if (job.whole)
            std::memcpy(w.to + job.off, w.from + job.off, size_t(job.count));
        else
            merge_slice(w, job.off, job.count);
    };
    const unsigned threads = copy_threads();
    if (threads == 0 || jobs.size() < 2) {
        for (uint64_t j = 0; j < jobs.size(); j++) run(j);
    } else {
        work_pool().run(jobs.size(), run);
    }
    for (size_t i = 0; i < v.size(); i++) {
        if (whole[i]) {
            replay::g_whole_writes.fetch_add(1, std::memory_order_relaxed);
            replay::g_whole_bytes.fetch_add(v[i].size, std::memory_order_relaxed);
        } else {
            replay::g_merge_writes.fetch_add(1, std::memory_order_relaxed);
            replay::g_merge_bytes.fetch_add(v[i].size, std::memory_order_relaxed);
        }
    }
    for (WriteBack& w : v) {
        if (w.after) std::memcpy(w.after->data(), w.from, size_t(w.size));  // NX_CARRY
        replay::snap_give(w.before);
    }
    replay::g_wb_wall_ns.fetch_add(plat::monotonic_ns() - t_start, std::memory_order_relaxed);
    replay::g_wb_batches.fetch_add(1, std::memory_order_relaxed);
    replay::g_wb_ranges.fetch_add(v.size(), std::memory_order_relaxed);
    v.clear();
}

}  // namespace nx::nvn
