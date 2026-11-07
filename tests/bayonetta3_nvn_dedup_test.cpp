// Production dedup code with a bounded CPU upload ring: no Vulkan device or game.
#include "../runtime/src/nvn/nvn_draw_state.h"
#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
std::vector<uint8_t> ring(8192);
uint64_t head = 16, capacity = ring.size(), allocations = 0, wraps = 0;
std::vector<std::string> logs;
uint64_t digest = 1469598103934665603ull;
uint64_t scan_visits = 0, compare_calls = 0, compare_bytes = 0;
void check(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
bool exact_one(const char* name) { const char* e = std::getenv(name); return e && std::strcmp(e, "1") == 0; }
const bool stats = exact_one("NX_DEDUP_STATS");
const bool reuse = exact_one("NX_DEDUP_PAD_REUSE");
const bool disabled = std::getenv("NX_NO_DEDUP") != nullptr;
}
namespace nx {
void log(const char* format, ...) {
    std::array<char, 1024> text{};
    va_list args;
    va_start(args, format);
    std::vsnprintf(text.data(), text.size(), format, args);
    va_end(args);
    logs.emplace_back(text.data());
}
}
namespace nx::nvn {
Upload upload_alloc(uint64_t bytes, uint64_t align) {
    check(align && bytes + 16 <= capacity, "test allocation fits the ring");
    uint64_t offset = (head + align - 1) / align * align;
    if (offset + bytes > capacity) {
        replay::dedup_reset();  // production's overwrite path invalidates all cached uploads
        std::fill(ring.begin(), ring.end(), 0xcc);
        head = 16;
        offset = (head + align - 1) / align * align;
        wraps++;
    }
    check(offset + bytes <= capacity, "aligned allocation fits the test ring");
    head = offset + bytes;
    allocations++;
    std::memset(ring.data() + offset, 0xcc, bytes);
    return {reinterpret_cast<VkBuffer>(ring.data()), offset, ring.data() + offset};
}
namespace replay { State g_state; }
}
#define NX_DEDUP_TEST_SCAN_VISIT() (++::scan_visits)
#define NX_DEDUP_TEST_COMPARE(size) (++::compare_calls, ::compare_bytes += (size))
#include "../runtime/src/nvn/nvn_dedup.cpp"
#undef NX_DEDUP_TEST_COMPARE
#undef NX_DEDUP_TEST_SCAN_VISIT

using namespace nx::nvn;
using namespace nx::nvn::replay;
namespace {
void fresh() {
    dedup_reset();
    std::fill(ring.begin(), ring.end(), 0xcc);
    scan_visits = compare_calls = compare_bytes = 0;
    head = 16;
    capacity = ring.size();
    allocations = wraps = 0;
    g_dedup_stats = {};
    g_saved = g_copied = g_lookups = g_front_hits = g_map_hits = 0;
    logs.clear();
}
bool same(Upload a, Upload b) { return a.buffer == b.buffer && a.offset == b.offset; }
void bytes(Upload u, const uint8_t* source, uint64_t size, uint64_t padded) {
    for (uint64_t i = 0; i < padded; i++) {
        const uint8_t want = source && i < size ? source[i] : 0;
        check(u.ptr[i] == want, "payload and zero tail must match the request");
        digest = (digest ^ u.ptr[i]) * 1099511628211ull;
    }
}
void outcome(DedupOutcome kind, uint64_t calls, uint64_t logical, uint64_t zeros) {
    const auto& c = g_dedup_stats.outcomes[size_t(kind)];
    check(c.calls == (stats ? calls : 0), "outcome call count");
    check(c.logical == (stats ? logical : 0), "outcome logical byte count");
    check(c.zeros == (stats ? zeros : 0), "outcome zero-fill byte count");
}
void total(uint64_t want) {
    uint64_t calls = 0;
    for (const auto& c : g_dedup_stats.outcomes) calls += c.calls;
    check(calls == (stats ? want : 0), "every request has exactly one outcome");
}
void grow_shrink_grow() {
    fresh();
    std::array<uint8_t, 64> source;
    source.fill(0x6d);
    auto a = dedup_upload(source.data(), 16, 16, 16); bytes(a, source.data(), 16, 16);
    auto b = dedup_upload(source.data(), 16, 64, 16); bytes(b, source.data(), 16, 64);
    auto c = dedup_upload(source.data(), 16, 32, 16); bytes(c, source.data(), 16, 32);
    auto d = dedup_upload(source.data(), 16, 64, 16); bytes(d, source.data(), 16, 64);
    auto e = dedup_upload(source.data(), 16, 64, 16); bytes(e, source.data(), 16, 64);
    check(!same(a, b), "larger padding requires a larger allocation");
    check(same(b, c) == (reuse && !disabled), "smaller padding reuse is opt-in");
    check(same(b, d) == (reuse && !disabled), "larger entry survives a smaller request");
    check(same(d, e) == !disabled, "equal padded reuse keeps the existing contract");
    check(allocations == (disabled ? 5 : reuse ? 2 : 4), "grow-shrink-grow allocation count");
    check(g_copied == (disabled ? 240 : reuse ? 80 : 176), "legacy copied bytes remain correct");
    check(g_saved == (disabled ? 0 : reuse ? 160 : 64), "legacy reused bytes use requested size");
    outcome(DedupOutcome::Absent, disabled ? 0 : 1, disabled ? 0 : 16, 0);
    outcome(DedupOutcome::GrowPadding, disabled ? 0 : reuse ? 1 : 2,
            disabled ? 0 : reuse ? 16 : 32, disabled ? 0 : reuse ? 48 : 96);
    outcome(DedupOutcome::ShrinkPadding, !disabled && !reuse, !disabled && !reuse ? 16 : 0,
            !disabled && !reuse ? 16 : 0);
    outcome(DedupOutcome::LargerPadding, !disabled && reuse, !disabled && reuse ? 16 : 0,
            !disabled && reuse ? 16 : 0);
    outcome(DedupOutcome::Equal, disabled ? 0 : reuse ? 2 : 1,
            disabled ? 0 : reuse ? 32 : 16, disabled ? 0 : reuse ? 96 : 48);
    outcome(DedupOutcome::Disabled, disabled ? 5 : 0, disabled ? 80 : 0, disabled ? 160 : 0);
    total(5);
    dedup_log();
    bool has_stats = false;
    for (const auto& line : logs) has_stats |= line.find("NX_DEDUP_STATS") != std::string::npos;
    check(has_stats == stats, "statistics require the exact switch value");
    total(0);
    check(g_copied == 0 && g_saved == 0, "log resets the existing byte counters");
    const auto before = allocations;
    auto f = dedup_upload(source.data(), 16, 64, 16); bytes(f, source.data(), 16, 64);
    check(allocations == before + uint64_t(disabled), "logging does not clear the cache");
    total(1);
}
void logical_size_and_alignment() {
    fresh();
    std::array<uint8_t, 64> source;
    source.fill(0x79);
    auto a = dedup_upload(source.data(), 32, 64, 16); bytes(a, source.data(), 32, 64);
    auto b = dedup_upload(source.data(), 16, 64, 16); bytes(b, source.data(), 16, 64);
    auto c = dedup_upload(source.data(), 32, 64, 16); bytes(c, source.data(), 32, 64);
    check(!same(a, b) && same(a, c) == !disabled, "alternating logical sizes retain independent uploads");
    outcome(DedupOutcome::Size, disabled ? 0 : 1, disabled ? 0 : 16, disabled ? 0 : 48);
    outcome(DedupOutcome::Equal, disabled ? 0 : 1, disabled ? 0 : 32, disabled ? 0 : 32);
    total(3);
    fresh();
    a = dedup_upload(source.data(), 32, 128, 16); bytes(a, source.data(), 32, 128);
    b = dedup_upload(source.data(), 32, 64, 256); bytes(b, source.data(), 32, 64);
    c = dedup_upload(source.data(), 32, 64, 16); bytes(c, source.data(), 32, 64);
    check(a.offset % 256 != 0 && b.offset % 256 == 0 && !same(a, b), "stricter alignment must allocate");
    check(same(b, c) == !disabled, "a stronger existing alignment satisfies a weaker request");
    outcome(DedupOutcome::Alignment, disabled ? 0 : 1, disabled ? 0 : 32, disabled ? 0 : 32);
    outcome(DedupOutcome::ShrinkPadding, 0, 0, 0);  // alignment is the first failing reason
    total(3);
}
void invalidation_and_generation() {
    fresh();
    std::array<uint8_t, 256> source;
    source.fill(0x31);
    auto a = dedup_upload(source.data() + 32, 32, 128, 16);
    auto b = dedup_upload(source.data() + 64, 32, 32, 16);
    auto c = dedup_upload(source.data() + 160, 32, 32, 16);
    dedup_invalidate(source.data() + 100, 16);  // in a's padding, outside all source payloads
    auto a0 = dedup_upload(source.data() + 32, 32, 128, 16);
    check(same(a, a0) == !disabled, "zero padding is not a dependency on guest bytes");
    dedup_invalidate(source.data() + 48, 32);  // overlap two entries, including a predecessor of p
    std::fill(source.begin() + 48, source.begin() + 80, 0x52);
    auto a1 = dedup_upload(source.data() + 32, 32, 128, 16); bytes(a1, source.data() + 32, 32, 128);
    auto b1 = dedup_upload(source.data() + 64, 32, 32, 16); bytes(b1, source.data() + 64, 32, 32);
    auto c1 = dedup_upload(source.data() + 160, 32, 32, 16); bytes(c1, source.data() + 160, 32, 32);
    check(!same(a0, a1) && !same(b, b1), "overlapping invalidation clears map and front entries");
    check(same(c, c1) == !disabled, "nonoverlapping entries remain reusable");
    check(g_dedup_stats.invalidations == (stats ? 2 : 0), "invalidation call count");
    check(g_dedup_stats.erased == (stats ? 2 : 0) && g_dedup_stats.erased_bytes == (stats ? 64 : 0),
          "invalidation records erased logical ranges");
    total(7);
    fresh();
    a = dedup_upload(source.data(), 24, 64, 16);
    dedup_reset();
    source[0] = 0x7a;
    b = dedup_upload(source.data(), 24, 64, 16); bytes(b, source.data(), 24, 64);
    check(!same(a, b), "a new generation rejects stale front entries");
    check(g_dedup_stats.resets == (stats ? 1 : 0), "generation reset count");
    outcome(DedupOutcome::Absent, disabled ? 0 : 2, disabled ? 0 : 48, disabled ? 0 : 80);
    total(2);
}
void collision_and_wrap() {
    fresh();
    std::vector<uint8_t> source(kFrontSlots * 32, 0x6c);
    std::array<const uint8_t*, kFrontSlots> seen{};
    const uint8_t* a = nullptr;
    const uint8_t* b = nullptr;
    for (size_t i = 0; i + 32 <= source.size(); i += 16) {
        const auto* p = source.data() + i;
        const size_t slot = size_t(&front_of(p) - g_front);
        if (seen[slot]) { a = seen[slot]; b = p; break; }
        seen[slot] = p;
    }
    check(a && b, "find a front-cache collision for this address hash");
    auto u = dedup_upload(a, 16, 32, 16);
    auto v = dedup_upload(b, 16, 16, 16); bytes(v, b, 16, 16);
    auto w = dedup_upload(a, 16, 16, 16); bytes(w, a, 16, 16);
    check(same(u, w) == (reuse && !disabled), "map fallback applies the same padding rule");
    if (micro_dedup() && !disabled) check(g_map_hits == 1, "collision reaches the map path");
    dedup_invalidate(a, 16);
    auto x = dedup_upload(a, 16, 16, 16); bytes(x, a, 16, 16);
    check(!same(w, x), "invalidation after a collision cannot retain a stale front hit");
    total(4);
    fresh();
    capacity = 128;
    std::array<uint8_t, 64> guest;
    guest.fill(0x2b);
    dedup_upload(guest.data(), 32, 32, 16);
    dedup_upload(guest.data() + 32, 32, 32, 16);
    auto grown = dedup_upload(guest.data() + 32, 32, 64, 16); bytes(grown, guest.data() + 32, 32, 64);
    check(wraps == 1, "an incompatible cached entry triggers ring wrap during allocation");
    guest[0] = 0x64;
    auto reread = dedup_upload(guest.data(), 32, 32, 16); bytes(reread, guest.data(), 32, 32);
    check(allocations == 4 && wraps == 1, "ring wrap discards other entries before reuse");
    check(g_dedup_stats.resets == (stats ? 1 : 0), "ring reset is counted");
    outcome(DedupOutcome::GrowPadding, disabled ? 0 : 1, disabled ? 0 : 32, disabled ? 0 : 32);
    outcome(DedupOutcome::Absent, disabled ? 0 : 3, disabled ? 0 : 96, 0);
    total(4);
}
void bounded_invalidation() {
    fresh();
    std::array<uint8_t, 4096> source;
    source.fill(0x42);
    constexpr size_t count = 200, stride = 16, logical = 8;
    for (size_t i = 0; i < count; ++i)
        dedup_upload(source.data() + i * stride, logical, 12, 16);
    check(g_copies.size() == count && g_max_logical == logical, "logical, not padded, maximum");

    // This address is beyond all 200 entries but within the old 64 MiB window.
    // Only the nearest predecessor may be inspected before the bound stops it.
    scan_visits = 0;
    dedup_invalidate(source.data() + count * stride + 7, 1);
    check(scan_visits == 1 && g_copies.size() == count, "disjoint entries stop at the first predecessor");

    auto* last = source.data() + (count - 1) * stride;
    scan_visits = 0;
    dedup_invalidate(last + logical, 1);
    check(scan_visits == 1 && g_copies.count(last), "touching payload end is disjoint");
    scan_visits = 0;
    dedup_invalidate(last + logical - 1, 1);
    check(scan_visits == 2 && !g_copies.count(last), "one-byte overlap erases then stops at next predecessor");
    check(front_of(last).p != last || front_of(last).generation != g_generation,
          "erasing the last entry invalidates its front slot");

    auto* middle = source.data() + 80 * stride;
    scan_visits = 0;
    dedup_invalidate(middle, stride);
    check(scan_visits == 2 && !g_copies.count(middle) && g_copies.count(middle + stride) &&
          g_copies.count(middle - stride), "invalidation is exclusive at its upper bound");
}

void long_predecessor_and_replacement() {
    fresh();
    std::array<uint8_t, 512> source;
    source.fill(0x26);
    auto original = dedup_upload(source.data(), 400, 400, 16);
    dedup_upload(source.data() + 128, 16, 32, 16);
    dedup_upload(source.data() + 320, 16, 16, 16);
    scan_visits = 0;
    dedup_invalidate(source.data() + 384, 1);
    check(scan_visits == 3 && !g_copies.count(source.data()) && g_copies.count(source.data() + 128) &&
          g_copies.count(source.data() + 320), "long predecessor survives two disjoint intervening starts");
    source[384] = 0x71;
    auto replacement = dedup_upload(source.data(), 400, 400, 16);
    check(!same(original, replacement) && replacement.ptr[384] == source[384],
          "overlapping predecessor cannot reuse stale upload bytes");

    fresh();
    dedup_upload(source.data(), 240, 240, 16);
    auto small = dedup_upload(source.data(), 8, 64, 16);
    check(g_copies.at(source.data()).count == 2 && g_max_logical == 240,
          "different sizes coexist below the conservative maximum");
    dedup_invalidate(source.data() + 12, 1);
    check(g_copies.count(source.data()) && g_copies.at(source.data()).count == 1,
          "write outside the small payload removes only the longer variant");
    auto small_again = dedup_upload(source.data(), 8, 64, 16);
    check(same(small, small_again) == !disabled, "padding-only write leaves the smaller variant reusable");
    dedup_invalidate(source.data() + 7, 1);
    check(!g_copies.count(source.data()), "replacement invalidates for a logical overlap");
    dedup_reset();
    check(g_copies.empty() && g_max_logical == 0, "explicit reset clears map and bound");
}

void invalidation_edge_arithmetic() {
    fresh();
    std::array<uint8_t, 128> source;
    source.fill(0x37);
    dedup_upload(source.data() + 16, 8, 8, 16);
    dedup_upload(source.data() + 40, 8, 8, 16);
    dedup_upload(source.data() + 48, 8, 8, 16);
    dedup_upload(source.data() + 64, 8, 8, 16);
    dedup_invalidate(source.data() + 48, std::numeric_limits<uint64_t>::max());
    check(g_copies.count(source.data() + 16) && g_copies.count(source.data() + 40) &&
          !g_copies.count(source.data() + 48) && !g_copies.count(source.data() + 64),
          "overflowing upper address scans to the end without wrapping");

    fresh();
    dedup_upload(source.data() + 16, 32, 32, 16);
    dedup_upload(source.data() + 64, 8, 8, 16);
    dedup_invalidate(source.data() + 24, 0);
    check(!g_copies.count(source.data() + 16) && g_copies.count(source.data() + 64),
          "zero length preserves the original predecessor-point behavior");
}

void max_after_ring_wrap() {
    fresh();
    capacity = 128;
    std::array<uint8_t, 160> source;
    source.fill(0x15);
    dedup_upload(source.data(), 80, 80, 16);
    check(g_max_logical == 80, "first allocation sets the bound");
    dedup_upload(source.data() + 80, 64, 64, 16);  // allocation resets before inserting this entry
    check(wraps == 1 && g_copies.size() == 1 && g_max_logical == 64,
          "post-wrap bound describes the newly inserted entry");
    dedup_invalidate(source.data() + 112, 1);
    check(g_copies.empty(), "post-wrap bound catches an overlapping predecessor");
}

void variants_and_mutation() {
    fresh();
    std::array<uint8_t, 128> source;
    source.fill(0x52);
    auto a = dedup_upload(source.data(), 16, 32, 16);
    auto b = dedup_upload(source.data(), 32, 48, 16);
    auto a_again = dedup_upload(source.data(), 16, 32, 16);
    check(!same(a, b) && same(a, a_again) == !disabled, "A/B/A reuses the first logical size");
    check(compare_calls == uint64_t(!disabled) && compare_bytes == (disabled ? 0u : 16u),
          "switching to an older compatible size compares its whole logical payload");
    auto a_repeat = dedup_upload(source.data(), 16, 32, 16);
    check(same(a_again, a_repeat) == !disabled && compare_calls == uint64_t(!disabled),
          "a verified variant becomes current and its front hit needs no comparison");
    source[20] ^= 1;  // outside current A but inside the retained B, without explicit invalidation
    auto a_unchanged = dedup_upload(source.data(), 16, 32, 16);
    check(same(a, a_unchanged) == !disabled, "unchanged logical bytes can still reuse A");
    auto b_changed = dedup_upload(source.data(), 32, 48, 16);
    bytes(b_changed, source.data(), 32, 48);
    check(!same(b, b_changed), "changed retained B is copied again");
    source[15] ^= 3;  // last byte of retained A while B is current
    auto a_changed = dedup_upload(source.data(), 16, 32, 16);
    bytes(a_changed, source.data(), 16, 32);
    check(!same(a, a_changed), "changed retained A is copied again");
    check(compare_calls == (disabled ? 0u : 3u) && compare_bytes == (disabled ? 0u : 64u),
          "old compatible candidates compare all their logical bytes");
    source[4] ^= 7;
    dedup_invalidate(source.data() + 4, 1);  // original contract: writes invalidate the current entry
    auto a_after_invalidation = dedup_upload(source.data(), 16, 32, 16);
    bytes(a_after_invalidation, source.data(), 16, 32);
    check(!same(a_changed, a_after_invalidation) && compare_calls == (disabled ? 0u : 3u),
          "an explicit write invalidation makes the current entry allocate without comparing");
    outcome(DedupOutcome::Changed, disabled ? 0 : 2, disabled ? 0 : 48, disabled ? 0 : 32);

    fresh();
    std::array<Upload, kVariantsPerAddress + 1> u;
    for (size_t i = 0; i < u.size(); ++i) {
        const size_t n = 8 * (i + 1);
        u[i] = dedup_upload(source.data(), n, n, 16);
        check(g_copies.at(source.data()).count <= kVariantsPerAddress, "per-address variants stay bounded");
    }
    check(g_copies.at(source.data()).count == kVariantsPerAddress, "full bucket retains exactly its budget");
    auto survivor = dedup_upload(source.data(), 16, 16, 16);
    check(same(u[1], survivor) == !disabled, "un-evicted size remains reusable");
    auto evicted = dedup_upload(source.data(), 8, 8, 16);
    check(!same(u[0], evicted), "evicted size cannot survive in the front cache");
    bytes(evicted, source.data(), 8, 8);
}

void variants_invalidation() {
    fresh();
    std::array<uint8_t, 128> source;
    source.fill(0x17);
    auto a = dedup_upload(source.data(), 16, 16, 16);
    auto b = dedup_upload(source.data(), 32, 32, 16);
    auto c = dedup_upload(source.data(), 64, 64, 16);
    auto neighbor = dedup_upload(source.data() + 20, 16, 16, 16);
    auto again = dedup_upload(source.data(), 16, 16, 16);
    check(same(a, again) == !disabled, "front selects a surviving short variant");
    dedup_invalidate(source.data() + 24, 1);
    source[24] ^= 1;
    check(g_copies.at(source.data()).count == 1 && !g_copies.count(source.data() + 20),
          "invalidation removes every overlapping variant and neighbor only");
    auto short_again = dedup_upload(source.data(), 16, 16, 16);
    check(same(a, short_again) == !disabled, "invalidation preserves disjoint shorter variant");
    auto long_again = dedup_upload(source.data(), 32, 32, 16);
    auto longer_again = dedup_upload(source.data(), 64, 64, 16);
    auto neighbor_again = dedup_upload(source.data() + 20, 16, 16, 16);
    bytes(long_again, source.data(), 32, 32);
    bytes(longer_again, source.data(), 64, 64);
    bytes(neighbor_again, source.data() + 20, 16, 16);
    check(!same(b, long_again) && !same(c, longer_again) && !same(neighbor, neighbor_again),
          "every overlapping variant is reuploaded after mutation");
    dedup_invalidate(source.data(), 1);
    check(!g_copies.count(source.data()), "all variants starting in invalidated range are erased");
}

void variant_front_collision_and_wrap() {
    fresh();
    std::vector<uint8_t> source(kFrontSlots * 32, 0x63);
    std::array<const uint8_t*, kFrontSlots> seen{};
    const uint8_t *a = nullptr, *b = nullptr;
    for (size_t i = 0; i + 64 <= source.size(); i += 64) {
        const auto* candidate = source.data() + i;
        size_t slot = size_t(&front_of(candidate) - g_front);
        if (seen[slot]) { a = seen[slot]; b = candidate; break; }
        seen[slot] = candidate;
    }
    check(a && b, "find colliding addresses for variant front test");
    auto a16 = dedup_upload(a, 16, 16, 16);
    auto a32 = dedup_upload(a, 32, 32, 16);
    auto b16 = dedup_upload(b, 16, 16, 16);
    auto a16_map = dedup_upload(a, 16, 16, 16);
    check(same(a16, a16_map) == !disabled, "front collision falls back to the correct variant");
    dedup_invalidate(b, 1);
    auto a16_front = dedup_upload(a, 16, 16, 16);
    check(same(a16, a16_front) == !disabled, "invalidating colliding B does not erase A's front");
    auto a32_map = dedup_upload(a, 32, 32, 16);
    check(same(a32, a32_map) == !disabled, "other A variant survives colliding invalidation");
    auto b16_new = dedup_upload(b, 16, 16, 16);
    check(!same(b16, b16_new), "colliding B cannot reuse its invalidated upload");
    dedup_reset();
    auto a_after_reset = dedup_upload(a, 32, 32, 16);
    check(!same(a32, a_after_reset), "generation prevents a stale variant after reset");

    fresh();
    capacity = 128;
    std::array<uint8_t, 80> guest;
    guest.fill(0x29);
    auto before_wrap = dedup_upload(guest.data(), 16, 16, 16);
    dedup_upload(guest.data(), 32, 32, 16);
    dedup_upload(guest.data(), 64, 64, 16);
    auto wraps_on_miss = dedup_upload(guest.data(), 48, 48, 16);
    bytes(wraps_on_miss, guest.data(), 48, 48);
    check(wraps == 1 && g_copies.size() == 1 && g_copies.at(guest.data()).count == 1 && g_max_logical == 48,
          "reset during allocation discards every old size and restores the new bound");
    guest[0] ^= 1;
    auto after_wrap = dedup_upload(guest.data(), 16, 16, 16);
    bytes(after_wrap, guest.data(), 16, 16);
    check(!same(before_wrap, after_wrap), "old front and map variants are invalid after an allocation wrap");
}

void current_variant_lifetime() {
    fresh();
    std::vector<uint8_t> source(kFrontSlots * 32, 0x65);
    std::array<const uint8_t*, kFrontSlots> seen{};
    const uint8_t *a = nullptr, *b = nullptr;
    for (size_t i = 0; i + 64 <= source.size(); i += 64) {
        const auto* candidate = source.data() + i;
        size_t slot = size_t(&front_of(candidate) - g_front);
        if (seen[slot]) { a = seen[slot]; b = candidate; break; }
        seen[slot] = candidate;
    }
    check(a && b, "find front collision for current marker");
    auto a16 = dedup_upload(a, 16, 16, 16);
    dedup_upload(b, 16, 16, 16);
    auto a16_map = dedup_upload(a, 16, 16, 16);
    check(same(a16, a16_map) == !disabled && compare_calls == 0,
          "a current variant recovered after collision never scans its payload");
    dedup_upload(a, 32, 32, 16);
    source[size_t(a - source.data()) + 15] ^= 3;
    dedup_upload(b, 16, 16, 16);  // evict the front slot, not A's map entries
    auto changed_old = dedup_upload(a, 16, 16, 16);
    bytes(changed_old, a, 16, 16);
    check(!same(a16, changed_old) && compare_calls == uint64_t(!disabled) &&
          compare_bytes == (disabled ? 0u : 16u),
          "a front collision cannot make an old changed size current");

    fresh();
    std::array<uint8_t, 64> guest;
    guest.fill(0x37);
    auto short_copy = dedup_upload(guest.data(), 16, 16, 16);
    dedup_upload(guest.data(), 32, 32, 16);
    dedup_invalidate(guest.data() + 24, 1);  // removes current 32, not old 16
    check(g_copies.at(guest.data()).current_size == 0, "removing current does not promote a survivor");
    guest[15] ^= 1;
    auto checked = dedup_upload(guest.data(), 16, 16, 16);
    bytes(checked, guest.data(), 16, 16);
    check(!same(short_copy, checked) && compare_calls == uint64_t(!disabled),
          "survivor is validated before becoming current after partial invalidation");

    fresh();
    dedup_upload(guest.data(), 32, 32, 16);
    auto current_short = dedup_upload(guest.data(), 16, 16, 16);
    dedup_invalidate(guest.data() + 24, 1);  // removes old 32, preserves current 16
    check(g_copies.at(guest.data()).current_size == 16, "disjoint invalidation preserves the current marker");
    auto short_map = dedup_upload(guest.data(), 16, 16, 16);
    check(same(current_short, short_map) == !disabled && compare_calls == 0,
          "surviving current variant remains a no-scan map hit");

    fresh();
    auto short_with_padding = dedup_upload(guest.data(), 16, 32, 16);
    dedup_upload(guest.data(), 32, 48, 16);
    guest[24] ^= 1;  // outside the older variant's logical payload
    auto restored = dedup_upload(guest.data(), 16, 32, 16);
    bytes(restored, guest.data(), 16, 32);
    check(same(short_with_padding, restored) == !disabled && compare_bytes == (disabled ? 0u : 16u),
          "older variants compare logical bytes only and preserve their zero tail");

    fresh();
    dedup_upload(guest.data(), 16, 16, 16);
    dedup_upload(guest.data(), 32, 32, 16);
    auto larger_tail = dedup_upload(guest.data(), 16, 32, 16);
    bytes(larger_tail, guest.data(), 16, 32);
    check(compare_calls == 0, "incompatible older padding is copied without reading the prior upload");
}

void variant_budget_many_addresses() {
    fresh();
    std::array<uint8_t, 4096> source;
    source.fill(0x35);
    for (size_t address = 0; address < 32; ++address)
        for (size_t n : {size_t(8), size_t(16), size_t(24), size_t(32), size_t(40)})
            dedup_upload(source.data() + address * 128, n, n, 16);
    check(!wraps && g_copies.size() == 32, "budget test keeps all distinct addresses in this generation");
    size_t count = 0;
    for (const auto& [address, variants] : g_copies) {
        (void)address;
        check(variants.count == kVariantsPerAddress, "each address respects its fixed variant budget");
        count += variants.count;
    }
    check(count == 32 * kVariantsPerAddress, "total retained variants are bounded by four per address");
}

void differential_invalidation() {
    // Independent linear reference: no address tree, no front cache, snapshots
    // rather than reading cached upload bytes for the reuse decision.
    struct Copy { size_t size, padded; Upload upload; std::vector<uint8_t> snapshot; };
    struct Address { size_t offset, next = 0, current = 0; std::vector<Copy> copies; };
    fresh();
    std::array<uint8_t, 4096> source{};
    std::vector<Address> model;
    uint32_t state = 0x89abcdef;
    const auto next = [&]() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    };
    constexpr size_t sizes[] = {8, 16, 24, 32, 40, 48};
    for (size_t step = 0; step < 3000; ++step) {
        const size_t offset = (next() % 40) * 32;
        const size_t op = next() % 20;
        if (op < 4) {
            const size_t n = next() % 33;  // includes legacy point invalidation
            std::fill(source.begin() + offset, source.begin() + offset + n, uint8_t(next()));
            dedup_invalidate(source.data() + offset, n);
            for (auto a = model.begin(); a != model.end();) {
                for (size_t i = 0; i < a->copies.size();) {
                    if (a->offset < offset + n && a->offset + a->copies[i].size > offset) {
                        if (a->copies[i].size == a->current) a->current = 0;
                        a->copies[i] = std::move(a->copies.back());
                        a->copies.pop_back();
                    } else {
                        ++i;
                    }
                }
                if (a->copies.empty()) a = model.erase(a); else ++a;
            }
        } else if (op < 6) {
            source[offset + next() % 48] ^= uint8_t(1 + next() % 255);  // no invalidation
        } else if (op == 6) {
            dedup_reset();
            model.clear();
        } else {
            const size_t n = sizes[next() % std::size(sizes)];
            const size_t padded = n + (next() % 3) * 16;
            const size_t align = size_t(16) << (next() % 3);
            auto a = std::find_if(model.begin(), model.end(), [&](const Address& x) { return x.offset == offset; });
            Copy* old = nullptr;
            if (a != model.end()) {
                for (auto& copy : a->copies) if (copy.size == n) old = &copy;
            }
            const bool compatible = old && old->upload.offset % align == 0 &&
                (old->padded == padded || (reuse && old->padded > padded));
            const bool compare = !disabled && compatible && a->current != n;
            const bool expect_hit = !disabled && compatible &&
                (!compare || std::equal(old->snapshot.begin(), old->snapshot.end(), source.begin() + offset));
            const Upload previous = old ? old->upload : Upload{};
            const auto prior_allocations = allocations, prior_wraps = wraps;
            const auto prior_compares = compare_calls, prior_bytes = compare_bytes;
            const auto upload = dedup_upload(source.data() + offset, n, padded, align);
            check(allocations == prior_allocations + !expect_hit, "linear model predicts reuse or fresh upload");
            check(compare_calls == prior_compares + compare && compare_bytes == prior_bytes + (compare ? n : 0),
                  "linear model predicts exactly which retained variants need byte checks");
            if (expect_hit) {
                check(same(upload, previous), "reuse returns the reference upload");
                check(std::memcmp(upload.ptr, old->snapshot.data(), n) == 0, "reuse returns the original snapshot");
                a->current = n;
            } else {
                check(std::memcmp(upload.ptr, source.data() + offset, n) == 0, "fresh upload matches guest payload");
            }
            for (size_t i = n; i < padded; ++i) check(upload.ptr[i] == 0, "upload tail remains zero");
            if (wraps != prior_wraps) model.clear();
            if (!expect_hit) {
                a = std::find_if(model.begin(), model.end(), [&](const Address& x) { return x.offset == offset; });
                if (a == model.end()) {
                    model.push_back({offset, 0, 0, {}});
                    a = std::prev(model.end());
                }
                size_t i = 0;
                while (i < a->copies.size() && a->copies[i].size != n) ++i;
                if (i == a->copies.size()) {
                    if (a->copies.size() < kVariantsPerAddress) a->copies.push_back({});
                    else i = a->next++ % kVariantsPerAddress;
                }
                a->copies[i] = {n, padded, upload, std::vector<uint8_t>(source.begin() + offset, source.begin() + offset + n)};
                a->current = n;
            }
        }
        check(g_copies.size() == model.size(), "tree and linear model address counts match");
        for (const auto& address : model) {
            const auto it = g_copies.find(source.data() + address.offset);
            check(it != g_copies.end() && it->second.count == address.copies.size(), "address variants match");
            check(it->second.current_size == address.current, "current-size marker matches the independent model");
            for (const auto& copy : address.copies) {
                bool found = false;
                for (size_t i = 0; i < it->second.count; ++i) {
                    const auto& entry = it->second.entries[i];
                    if (entry.size == copy.size && entry.padded == copy.padded && same(entry.upload, copy.upload)) found = true;
                }
                check(found, "variant matches the independent linear model");
            }
        }
    }
}

void large_predecessor() {
    fresh();
    constexpr size_t mib64 = size_t(64) << 20;
    constexpr size_t logical = mib64 + 512;
    std::vector<uint8_t> source(logical + 64, 0x19);
    ring.resize(logical + 4096);
    capacity = ring.size();
    dedup_upload(source.data(), logical, logical, 16);
    auto small = dedup_upload(source.data(), 16, 16, 16);
    dedup_upload(source.data() + 32, 16, 16, 16);
    check(g_max_logical == logical, "real large upload registers its full logical payload");
    scan_visits = 0;
    dedup_invalidate(source.data() + mib64 + 128, 1);
    check(scan_visits == 2 && g_copies.count(source.data()) && g_copies.at(source.data()).count == 1 &&
          g_copies.count(source.data() + 32),
          "a >64 MiB predecessor is removed while disjoint variants survive");
    auto small_again = dedup_upload(source.data(), 16, 16, 16);
    check(same(small, small_again) == !disabled, "the small variant survives the distant invalidation");
    source[mib64 + 128] = 0x7c;
    const auto before = allocations;
    auto updated = dedup_upload(source.data(), logical, logical, 16);
    check(allocations == before + 1 && updated.ptr[mib64 + 128] == source[mib64 + 128],
          "large overlap cannot reuse stale upload bytes");
    check(wraps == 1 && g_max_logical == logical, "large post-wrap upload restores the bound");
}

void empty_inputs() {
    fresh();
    std::array<uint8_t, 32> source;
    source.fill(0xee);
    auto a = dedup_upload(nullptr, 99, 64, 16); bytes(a, nullptr, 0, 64);
    auto b = dedup_upload(nullptr, 0, 16, 16); bytes(b, nullptr, 0, 16);
    auto c = dedup_upload(source.data(), 0, 32, 16); bytes(c, nullptr, 0, 32);
    check(allocations == 3 && g_copies.empty(), "empty inputs always produce independent zero buffers");
    outcome(DedupOutcome::Empty, 3, 0, 112);
    total(3);
}
}
int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::strcmp(argv[1], "--large") == 0) {
            large_predecessor();
            std::puts("PASS large dedup predecessor and ring wrap");
            return 0;
        }
        check(argc == 1, "unknown test argument");
        grow_shrink_grow();
        logical_size_and_alignment();
        invalidation_and_generation();
        collision_and_wrap();
        bounded_invalidation();
        long_predecessor_and_replacement();
        invalidation_edge_arithmetic();
        max_after_ring_wrap();
        variants_and_mutation();
        variants_invalidation();
        variant_front_collision_and_wrap();
        current_variant_lifetime();
        variant_budget_many_addresses();
        differential_invalidation();
        empty_inputs();
        std::printf("PASS dedup payload and zero padding: %016llx\n", (unsigned long long)digest);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL dedup: %s\n", error.what());
        return 1;
    }
}
