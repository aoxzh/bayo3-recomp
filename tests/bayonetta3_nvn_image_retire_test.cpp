// Actual production retirement, Texture destruction and snapshot expiry, with
// fake Vulkan calls. No Vulkan device or game is needed. Unused runtime paths
// are discarded by function/data section GC in the Python runner.
#include "../runtime/src/nvn/nvn_vk.h"
#include <cstdio>
#include <stdexcept>
#include <string>

namespace {
uint64_t now_ns = 1;
unsigned freed_images = 0, freed_memory = 0, freed_views = 0, forgotten_views = 0;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void VKAPI_PTR destroy_image(VkDevice, VkImage, const VkAllocationCallbacks*) { ++freed_images; }
void VKAPI_PTR free_memory(VkDevice, VkDeviceMemory, const VkAllocationCallbacks*) { ++freed_memory; }
void VKAPI_PTR destroy_view(VkDevice, VkImageView, const VkAllocationCallbacks*) { ++freed_views; }
void VKAPI_PTR destroy_buffer(VkDevice, VkBuffer, const VkAllocationCallbacks*) {}
}
namespace nx {
HleRegistrar::HleRegistrar(const char*, HleFn) {}
void log(const char*, ...) {}
[[noreturn]] void fatal(const char*, ...) { throw std::runtime_error("production fatal"); }
}
namespace nx::plat {
uint64_t monotonic_ns() { return now_ns; }
void write_watch_remove(uint64_t) {}
}
namespace nx::nvn {
VkDevice vk_device() { return VK_NULL_HANDLE; }
void forget_image_views(GpuImage&) { ++forgotten_views; }
std::unordered_map<uint64_t, std::unique_ptr<Object>>& objects() {
    static std::unordered_map<uint64_t, std::unique_ptr<Object>> map;
    return map;
}
}
namespace nx::vk {
PFN_vkDestroyImage p_vkDestroyImage = destroy_image;
PFN_vkFreeMemory p_vkFreeMemory = free_memory;
PFN_vkDestroyImageView p_vkDestroyImageView = destroy_view;
PFN_vkDestroyBuffer p_vkDestroyBuffer = destroy_buffer;
}
#ifndef B3_NVN_FRAMES_SOURCE
#define B3_NVN_FRAMES_SOURCE "../runtime/src/nvn/nvn_frames.cpp"
#endif
#ifndef B3_NVN_OBJECTS_SOURCE
#define B3_NVN_OBJECTS_SOURCE "../runtime/src/nvn/nvn_objects.cpp"
#endif
#include B3_NVN_FRAMES_SOURCE
#include B3_NVN_OBJECTS_SOURCE

namespace {
using namespace nx::nvn;
std::shared_ptr<GpuImage> image(unsigned id) {
    auto p = std::make_shared<GpuImage>();
    p->image = reinterpret_cast<VkImage>(uintptr_t(id * 3 + 1));
    p->memory = reinterpret_cast<VkDeviceMemory>(uintptr_t(id * 3 + 2));
    p->view = reinterpret_cast<VkImageView>(uintptr_t(id * 3 + 3));
    return p;
}
void clear_state() {
    objects().clear();
    g_grave.clear();
    g_reusable.clear();
    g_staging.clear();
    g_snapshots_of.clear();
    g_snapshot_images.clear();
    g_submitted = 10;
    g_reuse_clock = 0;
    freed_images = freed_memory = freed_views = forgotten_views = 0;
    now_ns = 1;
}
void require_one_free(const char* message) {
    require(freed_images == 1 && freed_memory == 1 && freed_views == 1 && forgotten_views == 1, message);
}
void duplicate_dead_entries() {
    clear_state();
    std::weak_ptr<GpuImage> observer;
    {
        Texture a, b, c;
        a.image = image(1);
        b.image = c.image = a.image;
        observer = a.image;
    }
    require(g_grave.size() == 3, "all actual Texture destructors enqueue the shared image");
    free_retired_images(10);
    require(freed_images == 0 && !observer.expired(), "in-flight serial remains live");
    free_retired_images(11);
    require(g_grave.empty() && observer.expired(), "all completed owners are released");
    require_one_free("duplicate completed references must free Vulkan handles exactly once");
    free_retired_images(100);
    require_one_free("repeated collection must not double free");
}
void owner_lifetimes() {
    clear_state();
    std::weak_ptr<GpuImage> observer;
    {
        Texture live;
        {
            Texture expired;
            expired.image = image(2);
            live.image = expired.image;
            observer = live.image;
        }
        free_retired_images(11);
        require(g_grave.empty() && freed_images == 0 && !observer.expired(), "live Texture protects image");
        g_submitted = 11;
    }
    free_retired_images(11);
    require(freed_images == 0 && !observer.expired(), "later Texture destruction waits for its own serial");
    free_retired_images(12);
    require(observer.expired(), "last Texture retirement releases image");
    require_one_free("live owner eventually frees once");

    clear_state();
    {
        Texture later;
        {
            Texture earlier;
            earlier.image = image(3);
            later.image = earlier.image;
            observer = later.image;
        }
        g_submitted = 11;
    }
    free_retired_images(11);
    require(g_grave.size() == 1 && freed_images == 0 && !observer.expired(), "future grave alias protects image");
    free_retired_images(12);
    require(observer.expired(), "future grave alias eventually expires");
    require_one_free("due and future aliases free exactly once after both complete");

    clear_state();
    auto held = image(4);
    observer = held;
    g_reusable[ReuseKey{}] = {g_reuse_clock, held};
    retire_image(std::move(held));
    free_retired_images(11);
    require(g_grave.empty() && freed_images == 0 && !observer.expired(), "reuse pool owner protects image");
    g_submitted = 11;
    for (unsigned i = 0; i < 8; ++i) free_retired_images(11);
    require(g_reusable.empty() && !g_grave.empty() && freed_images == 0, "expired reuse owner enters future grave");
    free_retired_images(12);
    require(observer.expired(), "reuse owner eventually retires");
    require_one_free("reuse policy still retires image once");
}
void snapshot_expiry() {
    clear_state();
    auto owner = std::make_unique<Texture>();
    owner->target = 8;
    owner->image = image(5);
    std::weak_ptr<GpuImage> observer = owner->image;
    objects()[100] = std::move(owner);
    const uint64_t snap_key = make_snapshot(100);
    hand_image_to_snapshots(100);
    auto* snapshot = dynamic_cast<Texture*>(objects().at(snap_key).get());
    require(snapshot && snapshot->image, "production cubemap snapshot inherits image");
    objects().erase(100);
    free_retired_images(11);
    require(g_grave.empty() && freed_images == 0 && !observer.expired(), "snapshot protects completed original image");
    now_ns = 3'000'000'001ull;
    g_submitted = 20;
    // A further registration/finalization enters the production expiry loop.
    objects()[200] = std::make_unique<Texture>();
    make_snapshot(200);
    hand_image_to_snapshots(200);
    require(!snapshot->image, "expired snapshot relinquishes its image");
    require(!observer.expired() && g_grave.size() == 1, "snapshot expiry must retain image in deferred grave");
    require(g_grave.front().first == 21 && g_reusable.empty(), "snapshot expiry uses current submit plus one without reuse");
    free_retired_images(20);
    require(!observer.expired() && freed_images == 0, "snapshot image waits through current replay serial");
    free_retired_images(21);
    require(observer.expired(), "snapshot image released after completed serial");
    require_one_free("expired snapshot frees Vulkan handles exactly once");
    objects().clear();
    free_retired_images(100);
    require_one_free("later snapshot Texture destruction must not double free");
}
void empty_handles() {
    clear_state();
    retire_image({});
    require(g_grave.empty(), "null image requires no retirement entry");
    retire_image(std::make_shared<GpuImage>());
    free_retired_images(11);
    require(g_grave.empty() && freed_images == 0 && freed_memory == 0 && freed_views == 0 && forgotten_views == 0,
            "already freed handles must be skipped");
    auto p = image(6);
    p->view = VK_NULL_HANDLE;
    p->memory = VK_NULL_HANDLE;
    retire_image(std::move(p));
    free_retired_images(11);
    require(freed_images == 1 && forgotten_views == 1 && freed_memory == 0 && freed_views == 0,
            "optional null memory and view handles are not freed");
}
}
int main(int argc, char** argv) {
    try {
        const std::string mode = argc > 1 ? argv[1] : "all";
        require(argc <= 2 && (mode == "all" || mode == "duplicate" || mode == "snapshot"), "unexpected test mode");
        if (mode == "all" || mode == "duplicate") duplicate_dead_entries();
        if (mode == "all") owner_lifetimes();
        if (mode == "all" || mode == "snapshot") snapshot_expiry();
        if (mode == "all") empty_handles();
        clear_state();
        std::puts("bayonetta3_nvn_image_retire_test: production retirement and ownership passed");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}
