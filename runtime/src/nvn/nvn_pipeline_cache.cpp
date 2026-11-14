// A VkPipelineCache kept on disk (<data root>/pipelines_<vendor>_<device>.bin), and the cost of what the replay
// creates on the fly. Pipelines are compiled the first time a draw needs them, potentially causing stutter.
// The driver checks the cache header itself (vendor, device, driver build): a stale file is ignored. Saved every
// 20 windows when it grew. The save itself is off the render thread: the render thread only snapshots the
// driver's cache image, and one background thread writes it.
// NX_PIPELINE_CACHE=0: no cache (bisecting, or to measure first-run stutter).
#include <cstdio>
#include <cstdlib>
#include <vector>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_set>

#include "nvn_pipeline.h"
#include "nvn_vk.h"
#include "platform.h"

namespace nx::nvn {

using namespace nx::vk;

namespace {

VkPipelineCache g_cache = VK_NULL_HANDLE;
bool g_tried = false;
std::atomic<size_t> g_saved_size{0};
std::string g_path;
uint64_t g_alias = 0, g_modules = 0, g_module_ns = 0, g_astc = 0, g_astc_ns = 0;
std::atomic<uint64_t> g_pipelines{0}, g_pipeline_ns{0}, g_worker_pipelines{0}, g_worker_ns{0};  // also counted from the compile worker threads
uint64_t g_waited = 0, g_waited_ns = 0, g_skipped = 0, g_prewarmed = 0;
std::unordered_set<uint64_t> g_known;  // hashes of the records in the .keys file or already queued to it
std::vector<WarmRec> g_new_recs;
constexpr uint32_t kKeysMagic = 0x314b5750;  // "PWK1"

// One writer thread, one pending snapshot of each file. The render thread never writes or closes a file.
std::mutex g_cache_lock;
std::condition_variable g_cache_cv;
std::condition_variable g_cache_idle;  // writer -> pipeline_cache_flush()
std::vector<uint8_t> g_pending_bin;
std::string g_pending_bin_path;
std::vector<WarmRec> g_pending_warm;
std::string g_pending_warm_path;
bool g_bin_pending = false, g_bin_writing = false;
bool g_warm_pending = false, g_warm_writing = false;
bool g_writer_started = false;

void cache_writer();

void ensure_cache_writer() {  // g_cache_lock held
    if (g_writer_started) return;
    g_writer_started = true;
    std::thread(cache_writer).detach();
}

void write_cache_file(const std::string& path, const std::vector<uint8_t>& data) {
    const uint64_t t0 = plat::monotonic_ns();
    std::string tmp = path + ".tmp";
    if (FILE* f = plat::fopen_utf8(tmp, "wb")) {
        bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
        ok = std::fclose(f) == 0 && ok;
        std::remove(path.c_str());
        if (ok && std::rename(tmp.c_str(), path.c_str()) == 0) g_saved_size.store(data.size());
    }
    const uint64_t ms = (plat::monotonic_ns() - t0) / 1'000'000;
    if (ms > 20)  // off the frame thread; the line says why a scene transition felt long
        log("nvn/vk: pipeline cache written in %llu ms (%zu bytes, %s)", (unsigned long long)ms, data.size(),
            path.c_str());
}

void write_warm_file(const std::string& path, const std::vector<WarmRec>& recs) {
    bool fresh = true;
    if (FILE* t = plat::fopen_utf8(path, "rb")) {
        uint32_t head[2] = {};
        fresh = !(std::fread(head, 4, 2, t) == 2 && head[0] == kKeysMagic && head[1] == sizeof(WarmRec));
        std::fclose(t);
    }
    if (FILE* f = plat::fopen_utf8(path, fresh ? "wb" : "ab")) {
        if (fresh) {
            uint32_t head[2] = {kKeysMagic, uint32_t(sizeof(WarmRec))};
            std::fwrite(head, 4, 2, f);
        }
        std::fwrite(recs.data(), sizeof(WarmRec), recs.size(), f);
        std::fclose(f);
    }
}

void cache_writer() {
    for (;;) {
        std::vector<uint8_t> bin;
        std::string bin_path;
        std::vector<WarmRec> warm;
        std::string warm_path;
        {
            std::unique_lock lk(g_cache_lock);
            g_cache_cv.wait(lk, [] { return g_bin_pending || g_warm_pending; });
            if (g_bin_pending) {
                bin.swap(g_pending_bin);
                bin_path.swap(g_pending_bin_path);
                g_bin_pending = false;
                g_bin_writing = true;
            }
            if (g_warm_pending) {
                warm.swap(g_pending_warm);
                warm_path.swap(g_pending_warm_path);
                g_warm_pending = false;
                g_warm_writing = true;
            }
        }
        if (!bin.empty()) write_cache_file(bin_path, bin);
        if (!warm.empty()) write_warm_file(warm_path, warm);
        {
            std::lock_guard lk(g_cache_lock);
            if (!bin.empty()) g_bin_writing = false;
            if (!warm.empty()) g_warm_writing = false;
            g_cache_idle.notify_all();
        }
    }
}

}  // namespace

VkPipelineCache pipeline_cache() {
    if (g_tried) return g_cache;
    g_tried = true;
    if (const char* e = std::getenv("NX_PIPELINE_CACHE"); e && e[0] == '0') return VK_NULL_HANDLE;
    VkPhysicalDeviceProperties p{};
    p_vkGetPhysicalDeviceProperties(vk_physical(), &p);
    char name[64];
    std::snprintf(name, sizeof name, "/pipelines_%04x_%04x.bin", p.vendorID, p.deviceID);
    g_path = plat::data_root() + name;
    std::vector<uint8_t> data;
    if (FILE* f = plat::fopen_utf8(g_path, "rb")) {
        plat::fseek64(f, 0, SEEK_END);
        int64_t n = plat::ftell64(f);
        plat::fseek64(f, 0, SEEK_SET);
        if (n > 0) data.resize(size_t(n)), data.resize(std::fread(data.data(), 1, data.size(), f));
        std::fclose(f);
    }
    VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    ci.initialDataSize = data.size();
    ci.pInitialData = data.empty() ? nullptr : data.data();
    if (p_vkCreatePipelineCache(vk_device(), &ci, nullptr, &g_cache) != VK_SUCCESS) {
        ci.initialDataSize = 0, ci.pInitialData = nullptr;  // a cache the driver refuses: start empty
        p_vkCreatePipelineCache(vk_device(), &ci, nullptr, &g_cache);
    }
    g_saved_size = data.size();
    log("nvn/vk: pipeline cache %s (%zu bytes loaded)", g_path.c_str(), data.size());
    return g_cache;
}

void pipeline_cache_save() {
    if (!g_cache) return;
    {
        std::lock_guard lk(g_cache_lock);
        if (g_bin_pending || g_bin_writing) return;  // a slower disk must not stack copies
    }
    const size_t saved = g_saved_size.load(std::memory_order_relaxed);
    const uint64_t t0 = plat::monotonic_ns();
    size_t n = 0;
    if (p_vkGetPipelineCacheData(vk_device(), g_cache, &n, nullptr) != VK_SUCCESS ||
        n <= saved + 64 * 1024)  // small growth is not worth another 12 MB copy
        return;
    std::vector<uint8_t> data(n);
    if (p_vkGetPipelineCacheData(vk_device(), g_cache, &n, data.data()) != VK_SUCCESS) return;
    const uint64_t snapshot_ms = (plat::monotonic_ns() - t0) / 1'000'000;
    if (snapshot_ms > 10)
        log("nvn/vk: pipeline cache snapshot %llu ms (%zu bytes, the write is off-thread)",
            (unsigned long long)snapshot_ms, n);
    {
        std::lock_guard lk(g_cache_lock);
        if (g_bin_pending || g_bin_writing || n <= g_saved_size.load(std::memory_order_relaxed)) return;
        g_pending_bin = std::move(data);
        g_pending_bin_path = g_path;
        g_bin_pending = true;
        ensure_cache_writer();
    }
    g_cache_cv.notify_one();
}

void note_pipeline_created(uint64_t ns, bool worker) {
    (worker ? g_worker_pipelines : g_pipelines)++, (worker ? g_worker_ns : g_pipeline_ns) += ns;
}
void note_pipeline_waited(uint64_t ns) { g_waited++, g_waited_ns += ns; }
void note_pipeline_skipped() { g_skipped++; }
void note_pipeline_prewarmed() { g_prewarmed++; }

// ---- the keys of earlier sessions (<cache path minus .bin>.keys) ---------------------------------------------------
// Header {magic, sizeof(WarmRec)} then WarmRec records. A file of another layout is ignored (and rewritten).

static std::string keys_path() {
    pipeline_cache();  // sets g_path
    return (g_path.size() > 4 ? g_path.substr(0, g_path.size() - 4) : g_path) + ".keys";
}

static uint64_t rec_hash(const WarmRec& r) { return hash_bytes(reinterpret_cast<const unsigned char*>(&r), sizeof r); }

bool warm_load(std::vector<WarmRec>& out) {
    if (!pipeline_cache()) return false;
    FILE* f = plat::fopen_utf8(keys_path(), "rb");
    if (!f) return false;
    uint32_t head[2] = {};
    bool ok = std::fread(head, 4, 2, f) == 2 && head[0] == kKeysMagic && head[1] == sizeof(WarmRec);
    WarmRec r;
    {
        std::lock_guard lk(g_cache_lock);
        while (ok && std::fread(&r, sizeof r, 1, f) == 1) {
            out.push_back(r);
            g_known.insert(rec_hash(r));
        }
    }
    std::fclose(f);
    return ok;
}

void warm_record(const PipelineKey& k) {
    // WarmRec persists only VS/FS identities. Do not serialize raw pointers to
    // tessellation stages; these pipelines still use the ordinary Vulkan cache.
    if (k.tcs || k.tes) return;
    WarmRec r;
    std::memset(&r, 0, sizeof r);
    if (!k.vs || k.vs->hash.size() >= sizeof r.vs || (k.fs && k.fs->hash.size() >= sizeof r.fs)) return;
    std::memcpy(r.vs, k.vs->hash.c_str(), k.vs->hash.size());
    if (k.fs) std::memcpy(r.fs, k.fs->hash.c_str(), k.fs->hash.size());
    r.key = k;
    r.key.vs = nullptr, r.key.fs = nullptr;
    std::lock_guard lk(g_cache_lock);  // pipeline workers record concurrently
    if (g_known.size() < 16384 && g_known.insert(rec_hash(r)).second) g_new_recs.push_back(r);  // ~700 bytes each
}

static void warm_save() {
    if (!g_cache) return;
    std::string path = keys_path();
    {
        std::lock_guard lk(g_cache_lock);
        if (g_new_recs.empty() || g_warm_pending || g_warm_writing) return;
        g_pending_warm.swap(g_new_recs);
        g_pending_warm_path = path;
        g_warm_pending = true;
        ensure_cache_writer();
    }
    g_cache_cv.notify_one();
}

// The normal exit path: queue the warm records learned since the last save and wait for the writer.
// (The driver cache itself is saved on the render thread in a quiet window, creation_log; a cache snapshot here could
// race pipeline creation from another thread, and the small .keys file is enough to rebuild it.)
void pipeline_cache_flush() {
    if (!g_cache) return;
    warm_save();
    std::unique_lock lk(g_cache_lock);
    g_cache_idle.wait(lk, [] {
        return !g_bin_pending && !g_warm_pending && !g_bin_writing && !g_warm_writing;
    });
}
void note_pipeline_alias() { g_alias++; }
void note_module_created(uint64_t ns) { g_modules++, g_module_ns += ns; }
void note_astc_decoded(uint64_t ns) { g_astc++, g_astc_ns += ns; }

// One line per timing window (nvn_vk.cpp log_timing): what the replay created, and the time it took.
void creation_log(uint64_t frames) {
    // Saving copies the whole driver cache (10+ MB, up to ~30 ms) on this thread. Do it in a window that made no
    // pipelines, when the scene has settled, not in the middle of a burst of new ones (B3-Express 0.2.7 found the
    // same stall); a burst that never settles still saves now and then.
    const bool quiet = !g_pipelines && !g_worker_pipelines && !g_prewarmed;
    if (g_pipelines || g_astc)
        log("nvn/vk: created over %llu frames: %llu pipelines (%.1f ms), %llu ASTC textures decoded (%.1f ms)",
            (unsigned long long)frames, (unsigned long long)g_pipelines.load(), double(g_pipeline_ns.load()) / 1e6,
            (unsigned long long)g_astc, double(g_astc_ns) / 1e6);
    if (g_alias || g_modules)
        log("nvn/vk: over %llu frames: %llu raw keys shared an existing pipeline, %llu shader modules (%.1f ms)",
            (unsigned long long)frames, (unsigned long long)g_alias, (unsigned long long)g_modules, double(g_module_ns) / 1e6);
    g_alias = g_modules = g_module_ns = 0;
    if (g_prewarmed || g_skipped || g_waited || g_worker_pipelines)
        log("nvn/vk: over %llu frames: %llu pipelines sent to workers, %llu compiled there (%.1f ms), the replay waited for %llu (%.1f ms), %llu draws skipped",
            (unsigned long long)frames, (unsigned long long)g_prewarmed, (unsigned long long)g_worker_pipelines.load(),
            double(g_worker_ns.load()) / 1e6, (unsigned long long)g_waited, double(g_waited_ns) / 1e6,
            (unsigned long long)g_skipped);
    g_worker_pipelines = 0, g_worker_ns = 0;
    g_prewarmed = g_waited = g_waited_ns = g_skipped = 0;
    g_pipelines = 0, g_pipeline_ns = 0, g_astc = g_astc_ns = 0;
    static uint64_t since_save = 0;
    if (++since_save >= 20 && (quiet || since_save >= 120)) {
        pipeline_cache_save();
        since_save = 0;
    }
    warm_save();  // small (hundreds of KB), off the frame thread: save what this scene learned right away
}

}  // namespace nx::nvn
