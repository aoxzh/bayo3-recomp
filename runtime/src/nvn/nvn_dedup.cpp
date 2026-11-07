// Per-submit upload reuse (docs/archive/nvn/DEBUG.md #30). A draw's buffers are copied from guest memory into the upload
// ring when it is replayed; the same camera block, scene constants and vertex buffers were copied again for every
// draw that bound them (~130 KB per draw, 50-70 MB per frame). Within one submit guest memory only changes through
// inline uniform updates (UpdateUniformBuffer, which invalidates what it overlaps) and compute results (ranges a
// dispatch writes are read from their written copy, nvn_written.cpp, and invalidated here): a copy made once can
// serve every later draw of the submit that binds the same bytes.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "nvn_copy_behind.h"
#include "nvn_draw_state.h"

namespace nx::nvn::replay {

namespace {

struct Entry {
    uint64_t size = 0, padded = 0;
    Upload upload;
};
// A bounded number of logical sizes per address; same-size padding/alignment
// changes replace that size's entry. No extra upload bytes are allocated here.
constexpr size_t kVariantsPerAddress = 4;
struct Variants {
    std::array<Entry, kVariantsPerAddress> entries{};
    size_t count = 0, next = 0;
    // Only this size is covered by the original no-comparison reuse contract.
    // Zero means that the current entry was invalidated; all survivors are old.
    uint64_t current_size = 0;
};
// Nodes from a free list: a fight frame inserts ~3000 and drops them all at the next reset, and each was a trip to malloc
// on the replay thread. They are never returned; the map is the only user and its size is bounded by a submit's binds.
template <class T>
struct NodePool {
    using value_type = T;
    NodePool() = default;
    template <class U> NodePool(const NodePool<U>&) {}
    template <class U> bool operator==(const NodePool<U>&) const { return true; }
    template <class U> bool operator!=(const NodePool<U>&) const { return false; }
    T* allocate(size_t n) {
        if (n != 1) return static_cast<T*>(::operator new(n * sizeof(T)));
        if (free_node()) {
            void* p = free_node();
            free_node() = *static_cast<void**>(p);
            return static_cast<T*>(p);
        }
        return static_cast<T*>(::operator new(sizeof(T)));
    }
    void deallocate(T* p, size_t n) {
        if (n != 1) return ::operator delete(p);
        *reinterpret_cast<void**>(p) = free_node();
        free_node() = p;
    }
    static void*& free_node() {
        static void* head = nullptr;
        return head;
    }
};
using CopyMap = std::map<const uint8_t*, Variants, std::less<const uint8_t*>,
                         NodePool<std::pair<const uint8_t* const, Variants>>>;
CopyMap g_copies;
// Conservative maximum of logical payloads inserted since the last reset.
// It may stay high after replacement or invalidation, but never underestimates a live entry.
uint64_t g_max_logical = 0;
// dedup_invalidate walks back from the end of a range to whatever could reach into it. One 16 MB buffer among thousands of
// small blocks made that walk 16 MB long for every inline uniform update of the submit, and this bound and the list below
// keep it to the small ones: payloads over kLargePayload are few, and are asked about one by one. (A uniform block is
// 64 KB at most, and there can be thousands of those: they stay on the walk's side.)
constexpr uint64_t kLargePayload = 64u << 10;
uint64_t g_max_small = 0;
std::vector<std::pair<const uint8_t*, uint64_t>> g_large;  // (address, largest payload there)
uint64_t g_saved = 0, g_copied = 0;

bool dedup_stats() {
    static const bool on = [] {
        const char* e = std::getenv("NX_DEDUP_STATS");
        return e && e[0] == '1' && e[1] == '\0';
    }();
    return on;
}
bool padded_reuse() {
    static const bool on = [] {
        const char* e = std::getenv("NX_DEDUP_PAD_REUSE");
        return e && e[0] == '1' && e[1] == '\0';
    }();
    return on;
}

// Address hits in the front/map counters below are not necessarily reusable.
// These outcomes count actual copies/reuses, with one first-failing reason per
// miss. All counters belong to the replay thread, like g_copies and g_saved.
enum class DedupOutcome { Equal, LargerPadding, Absent, Size, Alignment, GrowPadding, ShrinkPadding, Changed, Disabled, Empty, Count };
struct DedupCount { uint64_t calls = 0, logical = 0, zeros = 0; };
struct DedupStats {
    std::array<DedupCount, size_t(DedupOutcome::Count)> outcomes{};
    uint64_t resets = 0, invalidations = 0, erased = 0, erased_bytes = 0;
} g_dedup_stats;
void count_outcome(DedupOutcome outcome, uint64_t size, uint64_t padded) {
    if (!dedup_stats()) return;
    auto& count = g_dedup_stats.outcomes[size_t(outcome)];
    count.calls++;
    count.logical += size;
    count.zeros += padded > size ? padded - size : 0;
}

// In front of the map (a tree walk per bound buffer of every draw, and most are hits): a slot per address hash with a
// copy of that address's current variant. Insertion or successful old-size validation refreshes it, invalidation
// clears it, and reset starts a new generation.
struct Front {
    const uint8_t* p = nullptr;
    uint64_t generation = 0;
    Entry entry;
};
// NX_MICRO: four times the slots and a multiply-shift index instead of the xor of two shifted addresses (that xor put a
// submit's uniform blocks -- addresses at fixed strides -- onto the same few slots, and each such miss fell into a tree
// walk: 1.7% of the replay thread in the battle profile). One slot per address, no probing, no tombstones: a collision
// costs a map lookup, never a scan, so there is no load at which this is slower than the shipped 1024-slot front.
constexpr uint32_t kFrontSlots = 4096;
Front g_front[kFrontSlots];
uint64_t g_generation = 1;
uint64_t g_lookups = 0, g_front_hits = 0, g_map_hits = 0;  // NX_MICRO: printed with the uploads line, reset per submit
Front& front_of(const uint8_t* p) {
    static const bool wide = micro_dedup();
    if (wide) return g_front[(((uint64_t(p) >> 4) * 0x9E3779B97F4A7C15ull) >> 52) & (kFrontSlots - 1)];
    return g_front[((uintptr_t(p) >> 4 ^ uintptr_t(p) >> 14) & 1023)];  // the shipped slots are the low 1024
}

}  // namespace

Upload dedup_upload(const uint8_t* p, uint64_t size, uint64_t padded, uint64_t align, bool behind) {
    if (!p || !size) {
        count_outcome(DedupOutcome::Empty, 0, padded);
        Upload u = upload_alloc(padded, align);
        std::memset(u.ptr, 0, padded);
        return u;
    }
    static const bool off = std::getenv("NX_NO_DEDUP") != nullptr;  // bisecting: a fresh copy every time
    static const bool wide = micro_dedup();
    Front& f = front_of(p);
    const Entry* e = nullptr;
    Variants* matched_variants = nullptr;
    bool has_address = false, current = false;
    if (!off && f.generation == g_generation && f.p == p && f.entry.size == size) {
        // The front can only hold the current variant, so this is the original fast path.
        e = &f.entry;
        has_address = current = true;
        if (wide) g_front_hits++;
    } else if (auto it = off ? g_copies.end() : g_copies.find(p); it != g_copies.end()) {
        matched_variants = &it->second;
        has_address = true;
        for (size_t i = 0; i < matched_variants->count; ++i) {
            if (matched_variants->entries[i].size == size) {
                e = &matched_variants->entries[i];
                current = matched_variants->current_size == size;
                break;
            }
        }
        if (wide) g_map_hits++;
    }
    if (wide && !off) g_lookups++;
    const bool compatible = e && (e->padded == padded || (padded_reuse() && e->padded > padded)) &&
                            e->upload.offset % align == 0;
    bool unchanged = current;
    if (compatible && !current) {
        // Only a retained older size needs validation; guest bytes might have
        // changed without invalidation while a different size was current.
#ifdef NX_DEDUP_TEST_COMPARE
        NX_DEDUP_TEST_COMPARE(size);
#endif
        copy_behind::wait();  // e's own copy may still be queued
        unchanged = std::memcmp(p, e->upload.ptr, size) == 0;
    }
    if (compatible && unchanged) {
        if (matched_variants) {
            matched_variants->current_size = size;
            f = {p, g_generation, *e};
            e = &f.entry;
        }
        count_outcome(e->padded == padded ? DedupOutcome::Equal : DedupOutcome::LargerPadding, size, padded);
        g_saved += padded;
        return e->upload;
    }
    if (dedup_stats()) {
        const auto outcome = off ? DedupOutcome::Disabled : !has_address ? DedupOutcome::Absent :
            !e ? DedupOutcome::Size : e->upload.offset % align ? DedupOutcome::Alignment :
            e->padded < padded ? DedupOutcome::GrowPadding : e->padded > padded && !padded_reuse() ?
            DedupOutcome::ShrinkPadding : DedupOutcome::Changed;
        count_outcome(outcome, size, padded);
    }
    // upload_alloc can reset the cache when the ring wraps. Do not use e or a map iterator after it.
    Upload u = upload_alloc(padded, align);
    if (behind && copy_behind::wanted(size)) {
        copy_behind::push(u.ptr, p, size, padded > size ? padded - size : 0);
    } else {
        std::memcpy(u.ptr, p, size);
        if (padded > size) std::memset(u.ptr + size, 0, padded - size);
    }
    auto& variants = g_copies[p];
    size_t slot = 0;
    while (slot < variants.count && variants.entries[slot].size != size) ++slot;
    if (slot == variants.count) {
        if (variants.count < kVariantsPerAddress) {
            ++variants.count;
        } else {
            slot = variants.next++ % kVariantsPerAddress;
        }
    }
    variants.entries[slot] = {size, padded, u};
    variants.current_size = size;
    g_max_logical = std::max(g_max_logical, size);
    if (size <= kLargePayload) g_max_small = std::max(g_max_small, size);
    else g_large.push_back({p, size});
    f = {p, g_generation, variants.entries[slot]};
    g_copied += padded;
    return u;
}

uint64_t dedup_generation() { return g_generation; }

void dedup_reset() {
    if (dedup_stats()) g_dedup_stats.resets++;
    g_copies.clear();
    g_max_logical = g_max_small = 0;
    g_large.clear();
    g_generation++;
}

void dedup_invalidate(const uint8_t* p, uint64_t size) {
    if (dedup_stats()) g_dedup_stats.invalidations++;
    // Work in integer addresses: subtraction below is only done in the direction
    // known not to underflow, and an invalidation beyond the address space scans
    // to the end instead of overflowing p + size.
    const uintptr_t begin = reinterpret_cast<uintptr_t>(p);
    const uintptr_t remaining = std::numeric_limits<uintptr_t>::max() - begin;
    // Returns the entry before `it`'s, which erasing leaves valid.
    auto visit = [&](CopyMap::iterator it) {
        const uintptr_t at = reinterpret_cast<uintptr_t>(it->first);
        const uintptr_t distance = at < begin ? begin - at : 0;
        auto& variants = it->second;
        bool erased = false;
        for (size_t i = 0; i < variants.count;) {
            const Entry& entry = variants.entries[i];
            // An entry that ends at begin is disjoint.
            if (at < begin && distance >= entry.size) {
                ++i;
                continue;
            }
            if (dedup_stats()) {
                g_dedup_stats.erased++;
                g_dedup_stats.erased_bytes += entry.size;
            }
            if (entry.size == variants.current_size) variants.current_size = 0;
            variants.entries[i] = variants.entries[--variants.count];
            erased = true;
        }
        if (erased) {
            if (Front& f = front_of(it->first); f.p == it->first) f.generation = 0;
            if (!variants.count) it = g_copies.erase(it);
        }
        return it;
    };
    auto it = size > remaining ? g_copies.end() :
        g_copies.lower_bound(reinterpret_cast<const uint8_t*>(begin + uintptr_t(size)));
    while (it != g_copies.begin()) {
        --it;
#ifdef NX_DEDUP_TEST_SCAN_VISIT
        NX_DEDUP_TEST_SCAN_VISIT();
#endif
        const uintptr_t at = reinterpret_cast<uintptr_t>(it->first);
        // Every earlier entry starts farther away and holds nothing small enough to be left out of this bound.
        if (at < begin && begin - at >= g_max_small) break;
        it = visit(it);
    }
    for (const auto& [address, payload] : g_large) {
        const uintptr_t at = reinterpret_cast<uintptr_t>(address);
        if (at >= begin || begin - at >= payload) continue;  // the walk above saw it, or it ends before begin
#ifdef NX_DEDUP_TEST_SCAN_VISIT
        NX_DEDUP_TEST_SCAN_VISIT();
#endif
        if (auto found = g_copies.find(address); found != g_copies.end()) visit(found);
    }
}

// c[0], the NVN driver's constants, as far as shaders read them (docs/nvn/TRANSLATOR.md): buffer descriptors
// {u64 GPU address; u32 size} of what is bound, which compute shaders search to find the buffer an address falls in
// (they wrote garbage with a zero table), and the maximum point size.
std::array<uint8_t, 0x1000> driver_constants(ShaderStage stage, int st) {
    std::array<uint8_t, 0x1000> c0{};
    auto put = [&](uint32_t at, const Range& r) {
        if (at + 12 > c0.size()) return;
        std::memcpy(c0.data() + at, &r.address, 8);
        uint32_t size = uint32_t(std::min<uint64_t>(r.size, 0xFFFFFFFFu));
        std::memcpy(c0.data() + at + 8, &size, 4);
    };
    if (stage == ShaderStage::Compute) {
        for (int n = 0; n < 16; n++) put(0x210 + 0x10 * n, g_state.uniforms[st][n]);
        for (int n = 0; n < 16; n++) put(0x310 + 0x10 * n, g_state.storage[st][n]);
    } else {
        int hw = stage == ShaderStage::Fragment ? 4 : stage == ShaderStage::TessellationControl ? 1 :
                 stage == ShaderStage::TessellationEvaluation ? 2 : 0;  // Maxwell stage numbers
        for (int n = 0; n < 16; n++) put(0x110 + 0x100 * hw + 0x10 * n, g_state.storage[st][n]);
        float max_point = 64.0f;
        std::memcpy(c0.data() + 0x610, &max_point, 4);
    }
    return c0;
}

void dedup_log() {
    if (micro_dedup()) {
        log("nvn: uploads copied %.1f MB, reused %.1f MB, front %llu of %llu lookups hit (%llu through the map)",
            double(g_copied) / 1e6, double(g_saved) / 1e6, (unsigned long long)g_front_hits,
            (unsigned long long)g_lookups, (unsigned long long)g_map_hits);
        g_lookups = g_front_hits = g_map_hits = 0;
    } else if (g_copied + g_saved) {
        log("nvn: uploads copied %.1f MB, reused %.1f MB", double(g_copied) / 1e6, double(g_saved) / 1e6);
    }
    g_copied = g_saved = 0;
    if (dedup_stats()) {
        static constexpr const char* names[] = {
            "reuse equal", "reuse larger-padding", "miss absent", "miss logical-size", "miss alignment",
            "miss padding-grow", "miss padding-shrink", "miss content changed", "bypass disabled", "bypass empty"
        };
        for (size_t i = 0; i < g_dedup_stats.outcomes.size(); i++) {
            const auto& c = g_dedup_stats.outcomes[i];
            if (!c.calls) continue;
            const bool reused = i <= size_t(DedupOutcome::LargerPadding);
            log("nvn: dedup stats since prior log: %s %llu calls, logical %s %.3f MB, zero-fill %s %.3f MB (NX_DEDUP_STATS)",
                names[i], (unsigned long long)c.calls, reused ? "avoided" : "copied", double(c.logical) / 1e6,
                reused ? "avoided" : "written", double(c.zeros) / 1e6);
        }
        log("nvn: dedup stats since prior log: %llu resets, %llu invalidations, %llu entries erased (%.3f MB logical; NX_DEDUP_STATS)",
            (unsigned long long)g_dedup_stats.resets, (unsigned long long)g_dedup_stats.invalidations,
            (unsigned long long)g_dedup_stats.erased, double(g_dedup_stats.erased_bytes) / 1e6);
        g_dedup_stats = {};
    }
}

}  // namespace nx::nvn::replay
