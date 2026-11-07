// Host NVN device lifecycle. Guest builders stay owned by the actual SDK; devices
// are side-table objects and never contain fabricated Tegra driver pointers.
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "nvn_vk.h"

namespace nx {
namespace {

struct DebugCallback { uint64_t function, data; };
struct Device {
    uint32_t flags = 0;
    bool finalizing = false;
    std::string label;
    std::vector<DebugCallback> callbacks;
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceFeatures features{};
    bool stencil8 = false;
};
std::mutex g_devices_lock;
std::map<uint64_t, Device> g_devices;
// The current backend has one shared VkDevice and global replay state. Reject a
// second live guest device until queues carry per-device ownership and modes.
std::atomic<uint32_t> g_origin{0}, g_depth{0}, g_domain{1};
#if defined(NX_BAYONETTA3)
constexpr int32_t kApiMinor = 12;
#else
constexpr int32_t kApiMinor = 15;  // preserve the upstream Wonder SDK contract
#endif

// B3 SDK SHA256 2a89fa1c0e677710e89ed29b578162affef530a0c9b8c93cda15d50a57a20d28.
// sdk+4a6db0 -> 4abd90 -> 499b60; audited by bayonetta3_nvn_device_sdk.py.
// These are SDK ABI constants only. Hardware-dependent fields and optional host
// features are handled below, never copied from an uninitialized SDK device.
const std::map<uint32_t, int32_t> kDeviceInfo = {
    {0, 55}, {1, kApiMinor}, {2, 14}, {3, 0x10000}, {4, 0x100}, {5, 8}, {6, 16},
    {7, 4}, {8, 16}, {9, 32}, {10, 16}, {11, 4}, {12, 4}, {13, 4}, {14, 16},
    {15, 32}, {16, 32}, {17, 256}, {18, 256}, {19, 4}, {20, 8}, {21, 512}, {22, 256},
    {25, 0x1000}, {26, 0x20000}, {27, 16}, {28, 1536}, {29, 1024}, {30, 64},
    {31, 1536}, {32, 65535}, {33, 65535}, {34, 65535}, {35, 8}, {36, 0x100000},
    {37, 0x1000}, {38, 16}, {39, 0x1000}, {40, 1}, {41, 1}, {42, 1}, {44, 15},
    {46, 1}, {47, 1}, {48, 16}, {49, 5}, {50, 1}, {51, 8}, {52, 8}, {53, 4},
    {54, 32}, {57, 128}, {59, 1}, {60, 4}, {66, 1}, {67, 1}, {68, 1}, {69, 1},
    {70, 5}, {71, 1}, {72, 1}, {73, 1}, {74, 1}, {75, 1}, {76, 1}, {77, 16},
    {78, 1024}, {79, 32}, {80, 4096}, {81, 65536}, {82, 65536}, {83, 256},
    {84, 16384}, {85, 262144}, {86, 4096}, {87, 1}, {88, 4}, {89, 2}, {90, 0},
    {91, 4096}, {92, 16384}, {93, 4096}, {94, 128}, {95, 32}, {96, 65535}, {97, 1},
    {99, 256}, {100, 16}, {101, 65535}, {102, 65535}, {103, 256}, {104, 16383},
};

// Host diagnostics use the NVN callback ABI (source, type, id, severity,
// message, user data). Copy registrations before calling guest code: callbacks
// can unregister themselves. Preserve the complete CPU, including FP registers.
void diagnostic(CPU& c, uint64_t device, const char* text) {
    std::vector<DebugCallback> callbacks;
    std::string label;
    {
        std::lock_guard lk(g_devices_lock);
        if (auto it = g_devices.find(device); it != g_devices.end()) {
            callbacks = it->second.callbacks;
            label = it->second.label;
        }
    }
    log("nvn device %llx [%s]: %s", (unsigned long long)device, label.c_str(), text);
    for (const auto& cb : callbacks) {
        CPU saved = c;
        try {
            nvn::guest_call(c, cb.function, {0, 0, 0, 0, reinterpret_cast<uint64_t>(text), cb.data});
        } catch (...) { c = saved; throw; }
        c = saved;
    }
}

void require_device(CPU& c) {
    bool valid;
    {
        std::lock_guard lk(g_devices_lock);
        auto it = g_devices.find(c.x[0]);
        valid = it != g_devices.end() && !it->second.finalizing;
    }
    if (!valid) fatal("nvn: device %llx is not initialized", (unsigned long long)c.x[0]);
}

void Initialize(CPU& c) {
    const uint64_t device = c.x[0], builder = c.x[1];
    c.x[0] = 0;
    if (!device || !builder) return;
    // B3's actual getter is sdk+4a6b20 (ldr w0,[x0,#4]); no hardcoded builder layout.
    const uint64_t getter = nvn::real_proc(c, "nvnDeviceBuilderGetFlags");
    if (!getter) { log("nvn: SDK has no DeviceBuilderGetFlags"); return; }
    const uint32_t flags = uint32_t(nvn::guest_call(c, getter, {builder}));
    // Deferred destruction is already provided by the replay object retirement.
    // Debug validation and separate sampler/texture handles are not implemented.
    if (flags & ~0x88u) { log("nvn: unsupported device flags 0x%x", flags); return; }
    std::lock_guard lk(g_devices_lock);
    if (!g_devices.empty()) { log("nvn: a host device is already initialized"); return; }
    if (!nvn::gpu_ensure_initialized() || !nvn::vk_device() || !nvn::vk_physical()) {
        log("nvn: Vulkan device initialization failed");
        return;
    }
    Device state;
    state.flags = flags;
    nx::vk::p_vkGetPhysicalDeviceProperties(nvn::vk_physical(), &state.properties);
    state.features = nvn::vk_features();
    VkFormatProperties format{};
    nx::vk::p_vkGetPhysicalDeviceFormatProperties(nvn::vk_physical(), VK_FORMAT_S8_UINT, &format);
    state.stencil8 = (format.optimalTilingFeatures &
                     (VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) ==
                    (VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
    g_origin = 0;
    g_depth = 0;
    g_devices.emplace(device, std::move(state));
    c.x[0] = 1;
    log("nvn: initialized host device %llx flags=0x%x (SDK 55.%d)", (unsigned long long)device, flags, kApiMinor);
}

void finish_work() {
    nvn::wait_submits_so_far(nvn::kWaitFinalize);
    nvn::worker_finish();
    nvn::gpu_finish_all();
}

void Finalize(CPU& c) {
    require_device(c);
    {
        std::lock_guard lk(g_devices_lock);
        g_devices.at(c.x[0]).finalizing = true;
    }
    finish_work();
    std::lock_guard lk(g_devices_lock);
    g_devices.erase(c.x[0]);
    g_origin = 0;
    g_depth = 0;
    // VkDevice is shared with images, pipelines and the replay worker. Its lifetime
    // is owned by the backend, not by this guest allocation.
}

void SetDebugLabel(CPU& c) {
    require_device(c);
    std::string label = c.x[1] ? reinterpret_cast<const char*>(c.x[1]) : "";
    std::lock_guard lk(g_devices_lock);
    g_devices.at(c.x[0]).label = std::move(label);
}

void InstallDebugCallback(CPU& c) {
    require_device(c);
    if (!c.x[1]) return;
    std::lock_guard lk(g_devices_lock);
    auto& list = g_devices.at(c.x[0]).callbacks;
    auto same = [&](const DebugCallback& cb) { return cb.function == c.x[1] && cb.data == c.x[2]; };
    auto it = std::find_if(list.begin(), list.end(), same);
    if (uint8_t(c.x[3])) {
        if (it == list.end()) list.push_back({c.x[1], c.x[2]});
    } else if (it != list.end()) list.erase(it);
}

void GenerateDebugDomainId(CPU& c) {
    require_device(c);
    // SDK sdk+4a6d20 is process-wide atomic fetch-add, not per guest allocation.
    uint32_t id = g_domain.fetch_add(1);
    if (id > 65535) fatal("nvn: debug domain IDs exhausted");
    c.x[0] = id;
}
void IsExternalDebuggerAttached(CPU& c) { require_device(c); c.x[0] = 0; }

void SetMode(CPU& c, bool depth) {
    require_device(c);
    uint32_t mode = uint32_t(c.x[1]);
    if (mode > 1) {
        diagnostic(c, c.x[0], "invalid device coordinate mode");
        fatal("nvn: invalid %s mode %u", depth ? "depth" : "window origin", mode);
    }
    auto& value = depth ? g_depth : g_origin;
    if (value.load() == mode) return;
    finish_work();
    value = mode;
}
void SetWindowOriginMode(CPU& c) { SetMode(c, false); }
void SetDepthMode(CPU& c) { SetMode(c, true); }
void GetWindowOriginMode(CPU& c) { require_device(c); c.x[0] = g_origin.load(); }
void GetDepthMode(CPU& c) { require_device(c); c.x[0] = g_depth.load(); }

// Registration is an optional Tegra ZBC compression optimization, not a clear.
// Vulkan's normal clear path remains usable when registration returns false.
void RegisterFastClear(CPU& c) { require_device(c); c.x[0] = 0; }

int32_t device_info(uint32_t pname, const Device* device) {
    auto it = kDeviceInfo.find(pname);
    int32_t value = it == kDeviceInfo.end() ? 0 : it->second;
    // Unsupported translation paths stay false even if the host extension exists.
    switch (pname) {
        case 7: case 40: case 50: case 52: case 59: case 66: case 67: case 68: case 69:
        case 71: case 72: case 73: case 74: case 75: case 76: case 77: case 79:
        case 87: case 90: case 94: case 95: case 97:
            return 0;
        case 38: return 1;  // replay currently sets one viewport/scissor
        // Virtual Maxwell scratch allocation contract, not the host's SM count.
        // SDK 4abe00/4abe08/4abe4c return dev+b4, (dev+b4)*(dev+ac), dev+ac.
        // Keep the established 2-SM, 2048-thread guest sizing model so callers can
        // divide/round allocations even though Vulkan owns the actual spill memory.
        case 23: return 2;
        case 24: return 2 * 2048;
        case 55: return 2048;
        case 43: return 0;  // Vulkan does not expose the device L2 size
        case 56: return 256;  // conservative pitch alignment, texture layout uses 0x100
        case 58: return 65536;  // guest pool page granularity
        case 98: return 0;  // SDK device field +0xbc; not implemented by this backend
        default: break;
    }
    if (!device) {
        // API/ABI queries are legal before Initialize. No host-dependent capability
        // is advertised until a Vulkan device has actually been initialized.
        switch (pname) {
            case 2: case 3: case 5: case 6: case 8: case 9: case 14: case 27:
            case 28: case 29: case 30: case 31: case 32: case 33: case 34: case 35:
            case 41: case 42: case 44: case 45: case 51: case 61: case 62: case 63:
            case 64: case 65: return 0;
            default: return value;
        }
    }
    const auto& l = device->properties.limits;
    auto cap = [&](uint32_t host) { return int32_t(std::min(uint32_t(value), host)); };
    switch (pname) {
        case 2: return cap(l.maxPerStageDescriptorUniformBuffers);
        case 3: return cap(l.maxUniformBufferRange);
        case 5: return cap(l.maxColorAttachments);
        case 6: return cap(l.maxVertexInputBindings);
        case 8: return cap(l.maxPerStageDescriptorStorageBuffers);
        case 9: return cap(std::min(l.maxPerStageDescriptorSampledImages, l.maxPerStageDescriptorSamplers));
        case 14: return cap(l.maxVertexInputAttributes);
        case 27: return device->features.samplerAnisotropy ? cap(uint32_t(l.maxSamplerAnisotropy)) : 1;
        case 28: case 29: case 30: return cap(l.maxComputeWorkGroupSize[pname - 28]);
        case 31: return cap(l.maxComputeWorkGroupInvocations);
        case 32: case 33: case 34: return cap(l.maxComputeWorkGroupCount[pname - 32]);
        case 35: return cap(l.maxPerStageDescriptorStorageImages);
        case 41: return device->stencil8;
        case 42: return 1;  // native ASTC or the existing CPU ASTC decoder
        case 44: { uint32_t n = std::min(16384u, l.maxImageDimension2D), levels = 0;
                   for (; n; n >>= 1) ++levels; return int32_t(levels); }
        case 45: return int32_t(std::min(2048u, l.maxImageArrayLayers));
        case 51: return cap(l.subPixelPrecisionBits);
        case 61: case 65: return int32_t(std::min(16384u, l.maxImageDimension2D));
        case 62: return 0;  // buffer textures have no implemented replay path
        case 63: return int32_t(std::min(2048u, l.maxImageDimension3D));
        case 64: return int32_t(std::min(16384u, l.maxImageDimensionCube));
        default: return value;
    }
}

void GetInteger(CPU& c) {
    const uint32_t pname = uint32_t(c.x[1]);
    if (!c.x[2]) fatal("nvn: DeviceGetInteger requires an output pointer");
    if (c.x[0]) require_device(c);
    if (pname > 104) {
        diagnostic(c, c.x[0], "unknown DeviceGetInteger query");
        fatal("nvn: unsupported DeviceGetInteger pname %u", pname);
    }
    std::lock_guard lk(g_devices_lock);
    auto it = g_devices.find(c.x[0]);
    st<int32_t>(c.x[2], device_info(pname, it == g_devices.end() ? nullptr : &it->second));
}

}  // namespace
namespace nvn {
uint32_t device_window_origin_mode() { return g_origin.load(); }
uint32_t device_depth_mode() { return g_depth.load(); }
}  // namespace nvn

NX_HLE("nvnDeviceInitialize", Initialize);
NX_HLE("nvnDeviceFinalize", Finalize);
NX_HLE("nvnDeviceGetInteger", GetInteger);
NX_HLE("nvnDeviceSetDebugLabel", SetDebugLabel);
NX_HLE("nvnDeviceInstallDebugCallback", InstallDebugCallback);
NX_HLE("nvnDeviceGenerateDebugDomainId", GenerateDebugDomainId);
NX_HLE("nvnDeviceIsExternalDebuggerAttached", IsExternalDebuggerAttached);
NX_HLE("nvnDeviceSetWindowOriginMode", SetWindowOriginMode);
NX_HLE("nvnDeviceSetDepthMode", SetDepthMode);
NX_HLE("nvnDeviceGetWindowOriginMode", GetWindowOriginMode);
NX_HLE("nvnDeviceGetDepthMode", GetDepthMode);
NX_HLE("nvnDeviceRegisterFastClearColor", RegisterFastClear);
NX_HLE("nvnDeviceRegisterFastClearColori", RegisterFastClear);
NX_HLE("nvnDeviceRegisterFastClearColorui", RegisterFastClear);
NX_HLE("nvnDeviceRegisterFastClearDepth", RegisterFastClear);

}  // namespace nx
