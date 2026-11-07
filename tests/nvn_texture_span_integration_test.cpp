// Exercise actual NVN initialization, finalization, snapshot, keep and flush entry points with a mock driver.
// The oracle below is the old independent pool->set scan, including its uint64_t wraparound semantics.
#include "../runtime/src/nvn/nvn.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
#include <thread>
#include <vector>

struct Builder { uint64_t pool, offset, size; };
static uint64_t clock_ns = 1;
static uint8_t scratch_bytes[64];
static std::vector<std::unique_ptr<nx::nvn::Object>> retired;
static bool inspect_partial = false;

namespace nx {
bool g_trace = false;
std::atomic<uint64_t> g_frame{0};
HleRegistrar::HleRegistrar(const char*, HleFn) {}
void log(const char*, ...) {}
[[noreturn]] void fatal(const char*, ...) { throw std::runtime_error("unexpected production fatal"); }
}
namespace nx::plat { uint64_t monotonic_ns() { return clock_ns; } }
namespace nx::nvn {
ObjectsLock& objects_lock() { static ObjectsLock lock; return lock; }
std::unordered_map<uint64_t, std::unique_ptr<Object>>& objects() {
    static std::unordered_map<uint64_t, std::unique_ptr<Object>> map;
    return map;
}
std::recursive_mutex& gpu_lock() { static std::recursive_mutex lock; return lock; }
Texture::~Texture() = default;
void defer_object(std::unique_ptr<Object> object) { retired.push_back(std::move(object)); }
void retire_image(std::shared_ptr<GpuImage>) {}
void note_replaced(uint64_t) {}
void note_pool_change(uint64_t, uint32_t, const PoolEntry&) {}
bool resolve_cache_on() { return false; }
bool texture_race_old() { static bool old = std::getenv("NX_TEXTURE_RACE_OLD") != nullptr; return old; }
Object* find_object(uint64_t guest, uint64_t) {
    auto it = objects().find(guest);
    return it == objects().end() ? nullptr : it->second.get();
}
uint64_t scratch(CPU&, size_t) { return reinterpret_cast<uint64_t>(scratch_bytes); }
uint64_t real_proc(CPU&, const char* name) { return reinterpret_cast<uint64_t>(name); }
uint64_t guest_call(CPU&, uint64_t proc, std::initializer_list<uint64_t> args) {
    const char* name = reinterpret_cast<const char*>(proc);
    const Builder* b = reinterpret_cast<const Builder*>(*args.begin());
    if (!std::strcmp(name, "nvnTextureBuilderGetMemoryPool")) return b->pool;
    if (!std::strcmp(name, "nvnTextureBuilderGetMemoryOffset")) return b->offset;
    if (!std::strcmp(name, "nvnTextureBuilderGetPackagedTextureLayout")) return 0;
    if (!std::strcmp(name, "nvnTextureBuilderGetSwizzle")) return 0;
    if (!std::strcmp(name, "nvnTextureBuilderGetTarget")) return 2;
    return 1;
}
uint64_t texture_storage_size(CPU&, uint64_t builder);
bool uw_widen_texture(Texture&) { return false; }
}

#ifndef B3_NVN_OBJECTS_SOURCE
#define B3_NVN_OBJECTS_SOURCE "../runtime/src/nvn/nvn_objects.cpp"
#endif
#include B3_NVN_OBJECTS_SOURCE

namespace {
using namespace nx;
using namespace nx::nvn;
std::map<uint64_t, std::set<uint64_t>> old_registrations;
unsigned checks = 0;

void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
Texture* texture(uint64_t key) {
    auto it = objects().find(key);
    return it == objects().end() ? nullptr : dynamic_cast<Texture*>(it->second.get());
}
void give_image(uint64_t key) {
    Texture* t = texture(key);
    require(t, "missing texture");
    static auto owner = std::make_shared<int>(42);
    t->image = std::shared_ptr<GpuImage>(owner, reinterpret_cast<GpuImage*>(uintptr_t{1}));
}
void init(uint64_t key, uint64_t pool, uint64_t off, uint64_t size) {
    Builder b{pool, off, size};
    CPU c{};
    c.x[0] = key;
    c.x[1] = reinterpret_cast<uint64_t>(&b);
    TextureInitialize(c);
    require(c.x[0] == 1, "texture initialize failed");
    old_registrations[pool].insert(key);
    give_image(key);
}
void finish(uint64_t key, uint64_t kept = 0) {
    CPU c{};
    c.x[0] = key;
    TextureFinalize(c);
    for (auto& [pool, keys] : old_registrations) {
        if (keys.erase(key) && kept) keys.insert(kept);
        keys.erase(key);  // TextureFinalize erased the original key after keep_for_slots
    }
}
void register_slot(uint64_t descriptor, uint32_t slot, uint64_t key) {
    CPU c{};
    c.x[0] = descriptor; c.x[1] = slot; c.x[2] = key; c.x[3] = 0;
    TexturePoolRegister(c);
}
void check(uint64_t pool, uint64_t off, uint64_t size) {
    struct Dirty { bool dirty = false; uint64_t lo = UINT64_MAX, hi = 0; };
    std::map<uint64_t, Dirty> expected;
    for (const auto& [key, obj] : objects()) {
        if (auto* t = dynamic_cast<Texture*>(obj.get())) {
            t->dirty = false; t->dirty_lo = UINT64_MAX; t->dirty_hi = 0;
            expected[key];
        }
    }
    auto p = old_registrations.find(pool);
    if (p != old_registrations.end()) for (uint64_t key : p->second) {
        auto* t = texture(key);
        if (!t || t->offset >= off + size || t->offset + t->storage_size <= off || !t->image) continue;
        auto& d = expected[key];
        d.dirty = true;
        d.lo = std::min(d.lo, std::max(off, t->offset) - t->offset);
        d.hi = std::max(d.hi, std::min(off + size, t->offset + t->storage_size) - t->offset);
    }
    CPU c{};
    c.x[0] = pool; c.x[1] = off; c.x[2] = size;
    MemoryPoolFlushMappedRange(c);
    for (auto& [key, d] : expected) {
        auto* t = texture(key);
        if (t->dirty != d.dirty || t->dirty_lo != d.lo || t->dirty_hi != d.hi) {
            std::fprintf(stderr, "mismatch pool=%llu off=%llu size=%llu key=%llx\n",
                         (unsigned long long)pool, (unsigned long long)off, (unsigned long long)size,
                         (unsigned long long)key);
            throw std::runtime_error("production flush differs from linear scan");
        }
    }
    ++checks;
}
void basic_lifecycle() {
    init(100, 10, 1000, 100);
    init(101, 10, 1050, 200);
    init(102, 10, 1000, 100);  // duplicate start
    init(103, 10, UINT64_MAX - 9, 20);
    texture(102)->image.reset();
    check(10, 1049, 2);
    check(10, 1000, 0);
    check(10, UINT64_MAX - 5, 10);
    check(10, 999, 101);
    init(100, 11, 3000, 200);  // no descriptor slot: old pool registration remains
    check(10, 3000, 30);
    check(11, 3000, 30);
    check(10, 1000, 30);
    make<Buffer>(100);  // a different kind of object can replace the registered key
    check(10, 3000, 30);
    check(11, 3000, 30);
    // Force both indexes to rebuild while the key holds a non-texture, then create a texture at the same key.
    init(104, 10, 12000, 50);
    init(105, 11, 13000, 50);
    check(10, 3000, 30);
    check(11, 3000, 30);
    init(100, 12, 4000, 50);
    check(10, 4000, 30);
    check(11, 4000, 30);
    check(12, 4000, 30);
    finish(100);
    check(10, 4000, 30);
    check(11, 4000, 30);
    check(12, 4000, 30);
    init(100, 10, 5000, 50);
    check(10, 5000, 30);
    check(11, 5000, 30);
    check(12, 5000, 30);
}
void snapshot_collision() {
    uint64_t collision = kSnapshot | (g_next_snapshot + 1);
    init(collision, 20, 6000, 100);
    init(201, 21, 7000, 100);
    check(20, 6000, 20);  // warm index at the overwritten key
    uint64_t snap;
    {
        std::lock_guard lk(objects_lock());
        snap = make_snapshot(201);
    }
    require(snap == collision, "snapshot collision setup failed");
    give_image(snap);
    check(20, 7000, 20);
    check(20, 6000, 20);
    {
        std::lock_guard lk(objects_lock());
        drop_snapshot(snap);
    }
    check(20, 7000, 20);
    init(collision, 22, 8000, 100);
    check(20, 8000, 20);  // its old registration still exists
    check(22, 8000, 20);
    finish(collision);
    check(20, 8000, 20);
}
void kept_collision_and_expiry() {
    const uint64_t descriptor = 30, guest = 301;
    uint64_t collision = kKeptTexture | (g_next_kept + 1);
    make<DescriptorPool>(descriptor);
    init(collision, 31, 9000, 100);
    init(guest, 32, 10000, 100);
    register_slot(descriptor, 5, guest);
    check(31, 9000, 1);
    check(32, 10000, 1);
    finish(guest, collision);
    auto* d = dynamic_cast<DescriptorPool*>(objects().at(descriptor).get());
    require(d && d->textures.at(5) == collision, "kept key collision setup failed");
    check(31, 10000, 1);
    check(31, 9000, 1);
    check(32, 10000, 1);
    // Slot replacement triggers release_kept and clears all registrations of the host key.
    const auto before_release = pool_textures().pool_count();
    register_slot(descriptor, 5, 0);
    require(pool_textures().pool_count() + 2 == before_release, "release_kept retained empty pools");
    for (auto& [pool, keys] : old_registrations) keys.erase(collision);
    check(31, 10000, 1);
    check(32, 10000, 1);

    init(302, 33, 11000, 100);
    register_slot(descriptor, 6, 302);
    check(33, 11000, 1);
    uint64_t kept = kKeptTexture | (g_next_kept + 1);
    finish(302, kept);
    check(33, 11000, 1);
    const auto before_expiry = pool_textures().pool_count();
    clock_ns = 1'000'000'001ull;
    {
        std::lock_guard lk(objects_lock());
        expire_kept();
    }
    require(pool_textures().pool_count() + 1 == before_expiry, "expire_kept retained an empty pool");
    for (auto& [pool, keys] : old_registrations) keys.erase(kept);
    check(33, 11000, 1);
}
void kept_reinitialize() {
    constexpr uint64_t descriptor = 40, guest = 401;
    make<DescriptorPool>(descriptor);
    init(guest, 41, 14000, 100);
    init(guest, 42, 15000, 100);
    register_slot(descriptor, 7, guest);
    check(41, 15000, 10);
    check(42, 15000, 10);
    uint64_t kept = kKeptTexture | (g_next_kept + 1);
    init(guest, 43, 16000, 100);
    for (uint64_t pool : {41ull, 42ull}) {
        require(old_registrations[pool].erase(guest) == 1, "missing old registration");
        old_registrations[pool].insert(kept);
    }
    check(41, 15000, 10);
    check(42, 15000, 10);
    check(41, 16000, 10);
    check(43, 16000, 10);
    const auto before_release = pool_textures().pool_count();
    register_slot(descriptor, 7, 0);
    require(pool_textures().pool_count() + 2 == before_release, "kept reinitialize retained empty pools");
    for (auto& [pool, keys] : old_registrations) keys.erase(kept);
    check(41, 15000, 10);
    check(42, 15000, 10);
    check(43, 16000, 10);

    // A stale descriptor registration with no live slot makes keep_for_slots erase the old memberships.
    init(402, 44, 16500, 100);
    register_slot(descriptor, 8, 402);
    check(44, 16500, 10);
    register_slot(descriptor, 8, 0);
    old_registrations[44].erase(402);
    init(402, 45, 16600, 100);
    check(44, 16600, 10);
    check(45, 16600, 10);
}
void released_pool_recreated() {
    constexpr uint64_t guest = 701, pool = 71;
    const std::size_t baseline = pool_textures().pool_count();
    for (unsigned cycle = 0; cycle != 3; ++cycle) {
        init(guest, pool, 21000 + cycle * 100, 50);
        require(pool_textures().pool_count() == baseline + 1, "new pool not indexed");
        check(pool, 21000 + cycle * 100, 50);  // allocate cached spans and matches in indexed mode
        finish(guest);
        require(pool_textures().pool_count() == baseline, "finalize retained an empty pool index");
        check(pool, 21000 + cycle * 100, 50);  // absent pool matches the old empty registration set
    }
}
void concurrent_flush_and_lifecycle() {
    if (texture_race_old()) return;  // that experimental mode deliberately writes fields outside objects_lock
    constexpr uint64_t guest = 601, pool = 61;
    std::atomic<bool> start{false};
    auto flush = [&] {
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        CPU c{};
        for (unsigned i = 0; i < 5000; ++i) {
            c.x[0] = pool; c.x[1] = 19000 + (i % 32); c.x[2] = 16;
            MemoryPoolFlushMappedRange(c);
        }
    };
    std::thread a(flush), b(flush);
    start.store(true, std::memory_order_release);
    for (unsigned i = 0; i < 2000; ++i) {
        Builder builder{pool, 19000 + (i % 32), 100};
        CPU c{};
        c.x[0] = guest; c.x[1] = reinterpret_cast<uint64_t>(&builder);
        TextureInitialize(c);
        c.x[0] = guest;
        TextureFinalize(c);
    }
    a.join(); b.join();
    check(pool, 19000, 100);
}
void legacy_partial_publication() {
    if (!texture_race_old()) return;
    init(501, 51, 17000, 100);
    check(51, 17000, 10);
    inspect_partial = true;
    init(501, 52, 18000, 200);
    check(51, 18000, 1);
    check(52, 18000, 1);
}
}

namespace nx::nvn {
uint64_t texture_storage_size(CPU&, uint64_t builder) {
    auto& b = *reinterpret_cast<const Builder*>(builder);
    if (inspect_partial) {
        inspect_partial = false;
        // In the old mode the new texture is already published, with its new offset but zero storage size.
        // A strict old scan still marks it for [offset-1, offset+1), as long as it has an image.
        give_image(501);
        check(51, b.offset - 1, 2);
        require(texture(501)->dirty, "partially initialized texture was skipped");
    }
    return b.size;
}
}

int main() {
    try {
        basic_lifecycle();
        snapshot_collision();
        if (std::getenv("NX_KEEP_TEXTURES")) {
            kept_collision_and_expiry();
            kept_reinitialize();
        }
        legacy_partial_publication();
        released_pool_recreated();
        concurrent_flush_and_lifecycle();
        std::printf("production lifecycle: %u oracle checks (old=%d keep=%d)\n", checks,
                    texture_race_old(), std::getenv("NX_KEEP_TEXTURES") != nullptr);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}
