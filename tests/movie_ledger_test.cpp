// Unit test for nx::movie_ledger (runtime/src/movie.h): the guest movie-memory-resource walk that the runtime uses to
// tell "the pool is genuinely full" from "the guest is leaking", and the size histogram that separates "a few big
// buffers" from "thousands of small ones" (docs/archive/MOVIE-EV0005.md §21).
//
// The walk reads guest memory in the real thing, so the only way to test it without a game run is the way the helper
// is built: it takes `read_u64` and `mapped` as callables. Here they are backed by a synthetic address space fed
// with the same intrusive-list layout the guest builds (node+0x38 = requested size, node+0x20 = older node, head at
// [resource+0x68], count at [resource+0x80] — main_00CD1F9C / main_00CD3B20, code_269.cpp:4989 and 6996).
//
// What is actually being asserted, and why each one matters:
//   * the numbers are the list's numbers (count, bytes, largest, per-class histogram);
//   * a corrupted list (bad link, self-cycle) ends the walk instead of running away — the ledger runs on the
//     gameplay thread while the guest mutates that list, so this must be a log line, never a hang or a fault;
//   * a walk that did not see everything says so (`disagrees()`), because a partial byte sum must never be quoted as
//     a measurement (that is exactly the "lower bound" caveat the log line carries);
//   * a "size" that cannot be a size (a pointer or vtable word) is not summed, but the node still counts as walked;
//   * `pool_room` refuses to invent a number when the two pool fields do not look like a (top, end) pair.
//
// Run: ./movie_ledger_test        (ctest name: movie_ledger)

#include <cstdint>
#include <cstdio>
#include <map>

#include "movie.h"

namespace movie_ledger = nx::movie_ledger;

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char* what, int line) {
    ++g_checks;
    if (ok) return;
    ++g_failures;
    std::printf("FAIL %s (movie_ledger_test.cpp:%d)\n", what, line);
}

#define CHECK(cond) check((cond), #cond, __LINE__)

// A synthetic guest address space: a word map is enough, because every field the walk touches is a uint64_t.
struct FakeGuest {
    std::map<uint64_t, uint64_t> words;
    mutable int reads_off_map = 0;

    void put(uint64_t addr, uint64_t value) { words[addr] = value; }
    bool mapped(uint64_t addr) const { return words.count(addr) != 0; }
    // Never called for an address `mapped` rejected; counting it proves the walk keeps its own contract.
    uint64_t read(uint64_t addr) const {
        const auto it = words.find(addr);
        if (it == words.end()) {
            ++reads_off_map;
            return 0;
        }
        return it->second;
    }
};

// One node as the guest's allocator lays it out: a block of at least 0x40 bytes with the size at +0x38 and the link
// to the older node at +0x20. 0x200 of spacing keeps every node's fields inside its own mapped block.
constexpr uint64_t kNode0 = 0x100000;
constexpr uint64_t kNodeSpacing = 0x200;

uint64_t node_addr(int i) { return kNode0 + uint64_t(i) * kNodeSpacing; }

uint64_t put_node(FakeGuest& g, int i, uint64_t size, uint64_t older) {
    const uint64_t n = node_addr(i);
    g.put(n, 0);  // the node's own address is mapped: that is the first thing the walk checks
    g.put(n + movie_ledger::kNodeSizeOffset, size);
    g.put(n + movie_ledger::kNodeOlderOffset, older);
    return n;
}

movie_ledger::Snapshot run(const FakeGuest& g, uint64_t head, uint32_t declared, int cap) {
    return movie_ledger::walk(
        head, declared, cap, [&g](uint64_t a) { return g.read(a); }, [&g](uint64_t a) { return g.mapped(a); });
}

// --- the numbers are the guest's numbers ---------------------------------------------------------------------------
void test_counts_bytes_and_histogram() {
    FakeGuest g;
    // Newest first, exactly like the guest's list: 8 MiB frame buffer, 2 MiB work memory, 3000 B sample, 512 B object,
    // 2000000 B (1.9 MiB) sample.
    uint64_t older = 0;
    older = put_node(g, 0, 512, older);
    older = put_node(g, 1, 3000, older);
    older = put_node(g, 2, 2000000, older);
    older = put_node(g, 3, 2ull * 1024 * 1024, older);
    const uint64_t head = put_node(g, 4, 8ull * 1024 * 1024, older);

    const movie_ledger::Snapshot s = run(g, head, 5, movie_ledger::kWalkCap);
    CHECK(s.walked == 5);
    CHECK(s.declared == 5);
    CHECK(!s.truncated);
    CHECK(!s.disagrees());
    CHECK(s.bytes == 512 + 3000 + 2000000 + 2ull * 1024 * 1024 + 8ull * 1024 * 1024);
    CHECK(s.largest == 8ull * 1024 * 1024);
    CHECK(s.bucket_count[0] == 1);  // 512       -> <1K
    CHECK(s.bucket_count[1] == 1);  // 3000      -> <4K
    CHECK(s.bucket_count[6] == 2);  // 2000000 and 2 MiB -> <4M
    CHECK(s.bucket_count[7] == 1);  // 8 MiB     -> <16M
    CHECK(s.bucket_bytes[0] == 512);
    CHECK(s.bucket_bytes[1] == 3000);
    CHECK(s.bucket_bytes[6] == 2000000 + 2ull * 1024 * 1024);
    CHECK(s.bucket_bytes[7] == 8ull * 1024 * 1024);
    CHECK(g.reads_off_map == 0);
}

void test_bucket_boundaries() {
    // A label means "size < bound", so a boundary value belongs to the bucket above it.
    CHECK(movie_ledger::bucket_of(0) == 0);
    CHECK(movie_ledger::bucket_of(1023) == 0);
    CHECK(movie_ledger::bucket_of(1024) == 1);
    CHECK(movie_ledger::bucket_of(4095) == 1);
    CHECK(movie_ledger::bucket_of(4096) == 2);
    CHECK(movie_ledger::bucket_of(16 * 1024 - 1) == 2);
    CHECK(movie_ledger::bucket_of(16 * 1024) == 3);
    CHECK(movie_ledger::bucket_of(256 * 1024 - 1) == 4);
    CHECK(movie_ledger::bucket_of(256 * 1024) == 5);
    CHECK(movie_ledger::bucket_of(1024 * 1024) == 6);
    CHECK(movie_ledger::bucket_of(4 * 1024 * 1024) == 7);
    CHECK(movie_ledger::bucket_of(16 * 1024 * 1024) == 8);
    CHECK(movie_ledger::bucket_of(1ull << 39) == 8);
}

// --- an empty pool is not a broken pool ---------------------------------------------------------------------------
void test_empty() {
    FakeGuest g;
    const movie_ledger::Snapshot s = run(g, 0, 0, movie_ledger::kWalkCap);
    CHECK(s.walked == 0);
    CHECK(s.bytes == 0);
    CHECK(s.largest == 0);
    CHECK(!s.truncated);
    CHECK(!s.disagrees());
}

// --- a list that is shorter than the count must say so -------------------------------------------------------------
void test_count_disagrees() {
    FakeGuest g;
    uint64_t older = 0;
    older = put_node(g, 0, 100, older);
    const uint64_t head = put_node(g, 1, 200, older);
    // The resource claims seven live allocations and the list yields two: a walk that raced a release (the count is
    // bumped/read outside the walk) or a stale head. Either way the byte sum is a lower bound and must be flagged.
    const movie_ledger::Snapshot s = run(g, head, 7, movie_ledger::kWalkCap);
    CHECK(s.walked == 2);
    CHECK(s.bytes == 300);
    CHECK(!s.truncated);
    CHECK(s.disagrees());
}

// --- a bad link ends the walk, it does not follow it ---------------------------------------------------------------
void test_bad_link() {
    FakeGuest g;
    uint64_t older = 0;
    older = put_node(g, 0, 100, older);
    const uint64_t head = put_node(g, 1, 200, older);
    // Node 0's link points at an address that was never written: `mapped` says no, so the walk stops before reading.
    g.put(node_addr(0) + movie_ledger::kNodeOlderOffset, 0xdead0000ull);
    const movie_ledger::Snapshot s = run(g, head, 2, movie_ledger::kWalkCap);
    CHECK(s.walked == 2);
    CHECK(s.bytes == 300);
    CHECK(s.truncated);
    CHECK(s.disagrees());
    CHECK(g.reads_off_map == 0);  // the unmapped address was never dereferenced
}

// --- a self-cycle is bounded by the cap ---------------------------------------------------------------------------
void test_cycle_is_capped() {
    FakeGuest g;
    const uint64_t a = put_node(g, 0, 1024, 0);
    const uint64_t b = put_node(g, 1, 1024, a);
    g.put(a + movie_ledger::kNodeOlderOffset, b);  // a -> b -> a, forever
    const movie_ledger::Snapshot s = run(g, b, 2, movie_ledger::kWalkCap);
    CHECK(s.walked == movie_ledger::kWalkCap);
    CHECK(s.truncated);
    CHECK(s.disagrees());
    // A run that walks a self-cycle must not report a plausible-looking "quiet" pool: truncated is the flag the log
    // line turns into "(walk truncated or the count moved: bytes are a lower bound)".
    CHECK(s.bytes == uint64_t(movie_ledger::kWalkCap) * 1024);
}

// --- a word that cannot be a size is not a size -------------------------------------------------------------------
void test_implausible_size() {
    FakeGuest g;
    uint64_t older = 0;
    older = put_node(g, 0, 1ull << 48, older);  // not a requested size: a pointer/vtable word
    older = put_node(g, 1, 4096, older);
    const uint64_t head = put_node(g, 2, 8192, older);
    const movie_ledger::Snapshot s = run(g, head, 3, movie_ledger::kWalkCap);
    CHECK(s.walked == 3);      // the node was still walked
    CHECK(s.bytes == 4096 + 8192);
    CHECK(s.largest == 8192);
    CHECK(s.bucket_count[0] == 0);
}

// --- the pool's tail ----------------------------------------------------------------------------------------------
void test_pool_room() {
    unsigned long long room = 0;
    CHECK(movie_ledger::pool_room(0x1000, 0x2000, &room));
    CHECK(room == 0x1000);
    CHECK(movie_ledger::pool_room(0x2000, 0x2000, &room));  // exactly full is a measurement, not a fault
    CHECK(room == 0);
    CHECK(!movie_ledger::pool_room(0x3000, 0x2000, &room));  // top above the end: fields moved, claim nothing
    CHECK(!movie_ledger::pool_room(0, 0x2000, &room));
    CHECK(!movie_ledger::pool_room(0x1000, 0, &room));
    CHECK(!movie_ledger::pool_room(0x1000, 1ull << 45, &room));  // not an address in this module: refuse
}

// --- the labels the log line prints -------------------------------------------------------------------------------
void test_labels() {
    for (int i = 0; i < movie_ledger::kBuckets; i++) CHECK(movie_ledger::bucket_label(i)[0] != '?');
    CHECK(movie_ledger::bucket_label(-1)[0] == '?');
    CHECK(movie_ledger::bucket_label(movie_ledger::kBuckets)[0] == '?');
}

}  // namespace

int main() {
    test_counts_bytes_and_histogram();
    test_bucket_boundaries();
    test_empty();
    test_count_disagrees();
    test_bad_link();
    test_cycle_is_capped();
    test_implausible_size();
    test_pool_room();
    test_labels();
    if (g_failures) {
        std::printf("movie_ledger: %d of %d checks FAILED\n", g_failures, g_checks);
        return 1;
    }
    std::printf("movie_ledger: all %d checks passed\n", g_checks);
    return 0;
}
