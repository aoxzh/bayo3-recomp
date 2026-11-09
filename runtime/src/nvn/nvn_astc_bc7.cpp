// ASTC as BC7 through a disk cache (nvn_astc_bc7.h).
#include "nvn_astc_bc7.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_set>
#include <utility>

#include <nx/runtime.h>  // log

#include "nvn.h"
#include "nvn_astc.h"
#include "nvn_bc7_encode.h"
#include "nvn_pipeline.h"  // hash_bytes
#include "nvn_tiling.h"
#include "platform.h"

namespace nx::nvn {
namespace {

namespace fs = std::filesystem;

constexpr char kMagic[8] = {'B', '3', 'B', 'C', '7', 0, 0, 1};
struct Header {
    char magic[8];
    uint64_t key, size, reserved;
};
static_assert(sizeof(Header) == 32);

std::atomic<bool> g_on{false};

struct Cache {
    std::mutex lock;
    bool loaded = false;
    std::unordered_set<uint64_t> keys, pending;
    uint64_t queued_bytes = 0;
    std::deque<std::function<void()>> jobs;
    std::condition_variable wake;
    bool threads = false;
    // stats
    uint64_t written = 0, written_bytes = 0, hits = 0, hit_bytes = 0, dropped = 0;
};

Cache& cache() {
    static Cache* c = new Cache;  // lives until exit: its threads may still run then
    return *c;
}

std::string dir() { return plat::data_root() + "/texcache"; }

std::string path_of(uint64_t key) {
    char name[32];
    std::snprintf(name, sizeof name, "/%016llx.bc7", static_cast<unsigned long long>(key));
    return dir() + name;
}

uint64_t cap_bytes() {
    static const uint64_t cap = [] {
        const char* e = std::getenv("NX_TEXCACHE_MB");
        const unsigned long long mb = e ? std::strtoull(e, nullptr, 10) : 8192ull;
        return uint64_t(mb ? mb : 8192ull) << 20;
    }();
    return cap;
}

// Under c.lock: the folder's files, and the oldest removed while the folder is over its cap.
void load(Cache& c) {
    if (c.loaded) return;
    c.loaded = true;
    std::error_code ec;
    fs::create_directories(fs::u8path(dir()), ec);
    struct Entry {
        fs::path path;
        fs::file_time_type at;
        uint64_t size;
    };
    std::vector<Entry> files;
    uint64_t total = 0;
    for (auto& e : fs::directory_iterator(fs::u8path(dir()), ec)) {
        const auto name = e.path().filename().u8string();
        if (name.size() != 20 || e.path().extension() != ".bc7") continue;
        const uint64_t key = std::strtoull(std::string(name.begin(), name.begin() + 16).c_str(), nullptr, 16);
        const uint64_t size = e.file_size(ec);
        files.push_back({e.path(), e.last_write_time(ec), size});
        total += size;
        c.keys.insert(key);
    }
    if (total > cap_bytes()) {
        std::sort(files.begin(), files.end(), [](const Entry& a, const Entry& b) { return a.at < b.at; });
        for (auto& f : files) {
            if (total <= cap_bytes() / 10 * 9) break;
            const auto name = f.path.filename().u8string();
            c.keys.erase(std::strtoull(std::string(name.begin(), name.begin() + 16).c_str(), nullptr, 16));
            fs::remove(f.path, ec);
            total -= f.size;
        }
    }
    log("nvn/vk: ASTC as BC7: %zu textures cached (%.1f MB, cap %llu MB) in %s", c.keys.size(), double(total) / 1e6,
        static_cast<unsigned long long>(cap_bytes() >> 20), dir().c_str());
}

void write_file(uint64_t key, const uint8_t* data, uint64_t size) {
    const std::string final_path = path_of(key), tmp = final_path + ".tmp";
    FILE* f = plat::fopen_utf8(tmp, "wb");
    if (!f) return;
    Header h{};
    std::memcpy(h.magic, kMagic, sizeof kMagic);
    h.key = key, h.size = size;
    const bool ok = std::fwrite(&h, sizeof h, 1, f) == 1 && std::fwrite(data, 1, size, f) == size;
    std::fclose(f);
    std::error_code ec;
    if (!ok) {
        fs::remove(fs::u8path(tmp), ec);
        return;
    }
    fs::rename(fs::u8path(tmp), fs::u8path(final_path), ec);
    if (ec) return;
    Cache& c = cache();
    std::lock_guard lk(c.lock);
    c.keys.insert(key);
    c.pending.erase(key);
    c.written++, c.written_bytes += size;
    if (c.written % 200 == 1)
        log("nvn/vk: ASTC as BC7: %llu textures written to the cache (%.1f MB), %llu read from it (%.1f MB), "
            "%llu skipped (queue full)",
            static_cast<unsigned long long>(c.written), double(c.written_bytes) / 1e6,
            static_cast<unsigned long long>(c.hits), double(c.hit_bytes) / 1e6,
            static_cast<unsigned long long>(c.dropped));
}

void worker() {
    plat::set_thread_name("texcache");
    plat::lower_thread_priority();
    Cache& c = cache();
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock lk(c.lock);
            c.wake.wait(lk, [&] { return !c.jobs.empty(); });
            job = std::move(c.jobs.front());
            c.jobs.pop_front();
        }
        job();
    }
}

void start_threads(Cache& c) {  // under c.lock
    if (c.threads) return;
    c.threads = true;
    const unsigned n = std::clamp(std::thread::hardware_concurrency() / 4, 1u, 4u);
    for (unsigned i = 0; i < n; i++) std::thread(worker).detach();
}

}  // namespace

void set_astc_bc7(bool supported) {
    const char* e = std::getenv("NX_ASTC_BC7");
    const bool on = supported && !(e && std::strcmp(e, "0") == 0);
    g_on.store(on);
    log("nvn/vk: ASTC as BC7 %s", on ? "on (a disk cache of BC7 instead of RGBA8)"
                                     : supported ? "off (NX_ASTC_BC7=0)" : "not used (native ASTC, or no BC7)");
}

bool astc_bc7_on() { return g_on.load(std::memory_order_relaxed); }

uint64_t astc_bc7_key(const Texture& t) {
    uint64_t h = hash_bytes(reinterpret_cast<const unsigned char*>(t.memory), t.storage_size);
    const uint32_t shape[7] = {t.format, t.width, t.height, t.depth, t.levels, t.target, 1u /* layout version */};
    const uint64_t s = hash_bytes(reinterpret_cast<const unsigned char*>(shape), sizeof shape);
    h ^= s + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h ? h : 1;
}

bool astc_bc7_cached(uint64_t key) {
    Cache& c = cache();
    std::lock_guard lk(c.lock);
    load(c);
    return c.keys.count(key) != 0;
}

VkFormat astc_bc7_format(uint32_t nvn_format) {
    return nvn_format >= 0x87 ? VK_FORMAT_BC7_SRGB_BLOCK : VK_FORMAT_BC7_UNORM_BLOCK;
}

uint64_t astc_bc7_size(const std::vector<LinearLevel>& levels, uint32_t slices) {
    uint64_t total = 0;
    for (const auto& l : levels) total += bc7_level_bytes(l.width, l.height) * slices;
    return total;
}

bool astc_bc7_read(uint64_t key, uint8_t* dst, uint64_t size) {
    FILE* f = plat::fopen_utf8(path_of(key), "rb");
    if (!f) return false;
    Header h{};
    const bool ok = std::fread(&h, sizeof h, 1, f) == 1 && std::memcmp(h.magic, kMagic, sizeof kMagic) == 0 &&
                    h.key == key && h.size == size && std::fread(dst, 1, size, f) == size;
    std::fclose(f);
    Cache& c = cache();
    std::lock_guard lk(c.lock);
    if (ok) c.hits++, c.hit_bytes += size;
    else c.keys.erase(key);  // stale or broken: made again the next time it is seen
    return ok;
}

void astc_bc7_encode_now(const uint8_t* blocks, const std::vector<LinearLevel>& levels, uint32_t slices, uint32_t bw,
                         uint32_t bh, bool srgb, uint8_t* dst) {
    std::vector<uint8_t> rgba;
    for (const auto& l : levels) {
        rgba.resize(uint64_t(l.width) * l.height * 4 * slices);
        astc_decode_image(blocks + l.offset, l.width, l.height, slices, bw, bh, srgb, rgba.data());
        bc7_encode_image(rgba.data(), l.width, l.height, slices, dst);
        dst += bc7_level_bytes(l.width, l.height) * slices;
    }
}

void astc_bc7_queue(uint64_t key, const std::vector<uint8_t>& blocks, const std::vector<LinearLevel>& levels,
                    uint32_t slices, uint32_t bw, uint32_t bh, bool srgb) {
    if (!astc_bc7_on()) return;
    constexpr uint64_t kBudget = 256ull << 20;  // ASTC input held for the background threads
    Cache& c = cache();
    std::lock_guard lk(c.lock);
    load(c);
    if (c.keys.count(key) || c.pending.count(key)) return;
    if (c.queued_bytes + blocks.size() > kBudget) {
        c.dropped++;
        return;
    }
    c.pending.insert(key);
    c.queued_bytes += blocks.size();
    start_threads(c);
    auto data = std::make_shared<std::vector<uint8_t>>(blocks);
    c.jobs.push_back([=, lv = levels] {
        std::vector<uint8_t> out(astc_bc7_size(lv, slices));
        astc_bc7_encode_now(data->data(), lv, slices, bw, bh, srgb, out.data());
        {
            std::lock_guard l2(cache().lock);
            cache().queued_bytes -= data->size();
        }
        write_file(key, out.data(), out.size());
    });
    c.wake.notify_one();
}

void astc_bc7_store(uint64_t key, std::vector<uint8_t> bc7) {
    Cache& c = cache();
    std::lock_guard lk(c.lock);
    if (c.keys.count(key) || c.pending.count(key)) return;
    c.pending.insert(key);
    start_threads(c);
    auto data = std::make_shared<std::vector<uint8_t>>(std::move(bc7));
    c.jobs.push_back([=] { write_file(key, data->data(), data->size()); });
    c.wake.notify_one();
}

}  // namespace nx::nvn
