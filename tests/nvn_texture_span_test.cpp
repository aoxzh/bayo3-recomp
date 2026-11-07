// Differential test against the original pool->set linear scan. This test has no NVN or Vulkan dependencies.
#include "../runtime/src/nvn/nvn_texture_span.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <map>
#include <random>
#include <set>
#include <tuple>
#include <unordered_map>
#include <vector>

using nx::nvn::TextureSpanIndex;
using Touch = std::tuple<uint64_t, uint64_t, uint64_t>;

struct Object {
    bool texture = true, image = true;
    uint64_t offset = 0, size = 0;
};

struct Fixture {
    TextureSpanIndex index;
    std::map<uint64_t, std::set<uint64_t>> registrations;  // the old representation
    std::unordered_map<uint64_t, Object> objects;
    size_t reads = 0;
    uint64_t checks = 0;

    void insert(uint64_t pool, uint64_t key) {
        registrations[pool].insert(key);
        index.insert(pool, key);
    }
    void replace(uint64_t key, Object object) {
        objects[key] = object;
        index.invalidate_guest(key);
    }
    // Replacing/removing a texture with a non-texture does not change registrations in the real object table.
    void replace_with_other(uint64_t key) { objects[key] = {false, false, 0, 0}; }
    void destroy_other(uint64_t key) { objects.erase(key); }
    void erase(uint64_t key) {
        for (auto& [pool, set] : registrations) set.erase(key);
        index.erase_guest(key);
        objects.erase(key);
    }
    void move(uint64_t from, uint64_t to) {
        auto obj = objects.at(from);
        objects.erase(from);
        objects[to] = obj;
        for (auto& [pool, set] : registrations)
            if (set.erase(from)) set.insert(to);
        index.move_guest(from, to);
    }
    std::vector<Touch> scan(uint64_t pool, uint64_t off, uint64_t size) const {
        std::vector<Touch> result;
        auto p = registrations.find(pool);
        if (p == registrations.end()) return result;
        for (uint64_t key : p->second) {
            auto o = objects.find(key);
            if (o == objects.end() || !o->second.texture) continue;
            const Object& t = o->second;
            if (t.offset >= off + size || t.offset + t.size <= off || !t.image) continue;
            result.emplace_back(key, std::max(off, t.offset) - t.offset,
                                std::min(off + size, t.offset + t.size) - t.offset);
        }
        return result;
    }
    std::vector<Touch> lookup(uint64_t pool, uint64_t off, uint64_t size) {
        std::vector<Touch> result;
        index.visit(pool, off, size, [&](uint64_t key, uint64_t& lo, uint64_t& hi) {
            ++reads;
            auto o = objects.find(key);
            if (o == objects.end() || !o->second.texture) return false;
            lo = o->second.offset;
            hi = o->second.offset + o->second.size;
            return true;
        }, [&](uint64_t key) {
            auto o = objects.find(key);
            if (o == objects.end() || !o->second.texture) return;
            const Object& t = o->second;
            if (t.offset >= off + size || t.offset + t.size <= off || !t.image) return;
            result.emplace_back(key, std::max(off, t.offset) - t.offset,
                                std::min(off + size, t.offset + t.size) - t.offset);
        });
        return result;
    }
    void check(uint64_t pool, uint64_t off, uint64_t size) {
        auto expected = scan(pool, off, size);
        auto actual = lookup(pool, off, size);
        if (expected != actual) {
            std::fprintf(stderr, "mismatch pool=%llu offset=%llu size=%llu expected=%zu actual=%zu\n",
                         (unsigned long long)pool, (unsigned long long)off, (unsigned long long)size,
                         expected.size(), actual.size());
            std::abort();
        }
        ++checks;
    }
};

void targeted() {
    Fixture f;
    constexpr uint64_t max = UINT64_MAX;
    const Object data[] = {
        {true, true, 0, 0}, {true, true, 0, 1}, {true, true, 8, 0}, {true, true, 8, 10},
        {true, true, 8, 10}, {true, false, 8, 500}, {true, true, 15, 2},
        {true, true, max - 4, 9}, {true, true, max - 20, 18}, {true, true, 1, max - 1},
    };
    for (uint64_t i = 0; i < sizeof(data) / sizeof(*data); ++i) {
        f.replace(i + 1, data[i]);
        f.insert(10, i + 1);
        f.insert(10, i + 1);  // duplicate registration
    }
    f.check(999, 0, 2);
    const uint64_t offs[] = {0, 1, 7, 8, 9, 15, 17, 18, 20, max - 21, max - 5, max - 1, max};
    const uint64_t sizes[] = {0, 1, 2, 8, 10, 100, max};
    for (uint64_t off : offs) for (uint64_t size : sizes) f.check(10, off, size);
    // Old registrations can exist in more than one pool after re-initialization.
    f.insert(11, 4);
    f.check(10, 8, 4);
    f.check(11, 8, 4);
    f.replace(4, {true, true, 123, 50});
    f.check(10, 123, 20);
    f.check(11, 123, 20);
    f.replace_with_other(4);
    f.check(10, 123, 20);
    f.check(11, 123, 20);
    f.destroy_other(4);
    f.check(10, 123, 20);
    f.replace(4, {true, true, 42, 20});
    f.check(10, 42, 20);
    f.check(11, 42, 20);
    // A kept host key already exists in pool 11 and resolves to the new moved object.
    f.replace(200, {true, true, 500, 20});
    f.insert(11, 200);
    f.check(11, 500, 20);  // cache both pools before the collision
    f.move(4, 200);
    f.check(10, 42, 20);
    f.check(11, 42, 20);
    f.check(11, 500, 20);
    f.move(200, 200);  // host key can equal the guest key
    f.check(10, 42, 20);
    f.erase(200);
    f.check(10, 42, 20);
    f.check(11, 42, 20);
    // A moved source may have no registrations while the destination already does.
    f.replace(300, {true, true, 700, 20});
    f.replace(400, {true, true, 800, 20});
    f.insert(12, 400);
    f.check(12, 800, 20);
    f.move(300, 400);
    f.check(12, 700, 20);
    f.check(12, 800, 20);
    std::printf("targeted: %llu differential queries\n", (unsigned long long)f.checks);
}

void empty_pool_lifecycles() {
    Fixture f;
    // Removing one key can empty several pools while other pools still contain registrations.
    f.replace(1, {true, true, 100, 20});
    f.replace(2, {true, true, 100, 20});
    f.insert(10, 1);
    f.insert(11, 1);
    f.insert(11, 2);
    f.check(10, 100, 20);  // materialize both spans and matches
    f.check(11, 100, 20);
    assert(f.index.pool_count() == 2);
    f.erase(1);
    assert(f.index.pool_count() == 1);
    f.check(10, 100, 20);  // erased pool behaves like an empty old set
    f.check(11, 100, 20);
    f.erase(2);
    assert(f.index.pool_count() == 0);

    // Moving the only member keeps its pool; a destination collision must merge it rather than discard it.
    f.replace(3, {true, true, 300, 50});
    f.replace(4, {true, true, 400, 50});
    f.insert(20, 3);
    f.insert(21, 3);
    f.insert(21, 4);
    f.check(20, 300, 20);
    f.check(21, 400, 20);
    f.move(3, 4);
    assert(f.index.pool_count() == 2);
    f.check(20, 300, 20);
    f.check(21, 300, 20);
    std::vector<uint64_t> linear;
    f.index.for_each(20, [&](uint64_t key) { linear.push_back(key); });
    assert((linear == std::vector<uint64_t>{4}));
    f.move(4, 4);
    assert(f.index.pool_count() == 2);
    f.erase(4);
    assert(f.index.pool_count() == 0);
    linear.clear();
    f.index.for_each(20, [&](uint64_t key) { linear.push_back(key); });
    assert(linear.empty());

    // Every unique pool is populated, queried (allocating its caches), emptied and queried again. The count
    // proves the map nodes and their vector capacities are gone, rather than accumulating through pool churn.
    constexpr uint64_t cycles = 200, guests = 64;
    for (uint64_t cycle = 0; cycle < cycles; ++cycle) {
        const uint64_t pool = 1000 + cycle;
        for (uint64_t key = 1; key <= guests; ++key) {
            f.replace(key, {true, true, 2000, 100});
            f.insert(pool, key);
        }
        assert(f.index.pool_count() == 1);
        const size_t reads_before = f.reads;
        f.check(pool, 2000, 100);
        assert(f.reads - reads_before == guests);
        for (uint64_t key = 1; key < guests; ++key) f.erase(key);
        assert(f.index.pool_count() == 1);
        f.check(pool, 2000, 100);
        f.erase(guests);
        assert(f.index.pool_count() == 0);
        f.check(pool, 2000, 100);
    }
    // Reuse an erased pool key and ensure its index is constructed from fresh registrations.
    f.replace(1, {true, true, 3000, 100});
    f.insert(1000, 1);
    assert(f.index.pool_count() == 1);
    f.check(1000, 2000, 100);
    f.check(1000, 3000, 100);
    f.erase(1);
    assert(f.index.pool_count() == 0);
    std::printf("pool lifecycle: %llu warmed pools released, recreated pool differential checks passed\n",
                (unsigned long long)cycles);
}

void random_lifecycles() {
    Fixture f;
    std::mt19937_64 random(0x3c6ef372fe94f82bull);
    auto value = [&]() -> uint64_t {
        switch (random() % 8) {
        case 0: return UINT64_MAX - random() % 100;
        case 1: return random();
        case 2: return 0;
        case 3: return UINT64_MAX;
        default: return random() % 512;
        }
    };
    for (unsigned step = 0; step < 30000; ++step) {
        uint64_t key = 1 + random() % 250, pool = 1 + random() % 7;
        switch (random() % 10) {
        case 0: case 1: case 2:
            f.replace(key, {true, random() % 4 != 0, value(), value()});
            f.insert(pool, key);
            break;
        case 3: f.insert(pool, key); break;
        case 4: f.replace_with_other(key); break;
        case 5: f.destroy_other(key); break;
        case 6: f.erase(key); break;
        case 7: {
            uint64_t to = 1 + random() % 250;
            auto it = f.objects.find(key);
            if (it != f.objects.end() && it->second.texture) f.move(key, to);
            break;
        }
        default: break;
        }
        f.check(pool, value(), value());
        if (step % 31 == 0) {
            // Focus lookups on actual shared starts and boundaries as well as random 64-bit values.
            auto it = f.objects.find(key);
            if (it != f.objects.end()) {
                f.check(pool, it->second.offset, 1);
                f.check(pool, it->second.offset + it->second.size, 0);
            }
        }
    }
    std::printf("lifecycle fuzz: %llu differential queries, 30000 operations\n", (unsigned long long)f.checks);
}

void large_ranges() {
    Fixture f;
    constexpr uint64_t count = 40000;
    for (uint64_t i = 0; i < count; ++i) {
        // Numerous identical starts, long nesting, and sparse regions coexist in the same pool.
        uint64_t off = i < 512 ? 65536 : i * 4096;
        uint64_t size = i % 101 == 0 ? 500000 : 512;
        f.replace(i + 1, {true, true, off, size});
        f.insert(123, i + 1);
    }
    f.check(123, 65536, 64);
    assert(f.reads == count);
    for (uint64_t i = 0; i < 1000; ++i) f.check(123, i * 157123, 256);
    assert(f.reads == count);  // unchanged pools do not resolve all objects again
    f.check(123, 0, UINT64_MAX);
    f.check(123, UINT64_MAX - 200, 1000);
    std::printf("large pool: %llu differential queries over %llu registrations, cache reused\n",
                (unsigned long long)f.checks, (unsigned long long)count);
}

int main() {
    targeted();
    empty_pool_lifecycles();
    random_lifecycles();
    large_ranges();
}
