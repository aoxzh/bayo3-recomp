// Standalone production-handler test; no game execution and no CMake changes.
// Include the implementation to inspect side-table ownership, not a copied handler.
#include "../runtime/src/nvn/nvn_device.cpp"
#include <cstdarg>
#include <cstdio>
#include <stdexcept>

namespace {
std::map<std::string, nx::HleFn>& registry() { static std::map<std::string, nx::HleFn> r; return r; }
void check(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
bool getter_available = true, host_success = true, host_handles = true;
int init_calls = 0, getter_calls = 0, callback_calls = 0;
uint32_t builder_flags = 0;
std::vector<int> waits;
VkPhysicalDeviceProperties props{};
VkPhysicalDeviceFeatures features{};
VkFormatFeatureFlags stencil_features = 0;
uint64_t current_device = 0;
std::array<uint64_t, 6> callback_args{};
bool callback_unregister = false;
uint64_t invoke(const char* suffix, uint64_t a = 0, uint64_t b = 0, uint64_t d = 0, uint64_t e = 0) {
    nx::CPU c{};
    c.x[0] = a; c.x[1] = b; c.x[2] = d; c.x[3] = e;
    registry().at(std::string("nvnDevice") + suffix)(c);
    return c.x[0];
}
int32_t query(uint64_t device, uint32_t pname) {
    std::array<int32_t, 3> guarded{123, -1, 456};
    invoke("GetInteger", device, pname, reinterpret_cast<uint64_t>(&guarded[1]));
    check(guarded[0] == 123 && guarded[2] == 456, "GetInteger writes exactly 4 bytes");
    return guarded[1];
}
template<class F> void dies(F f) {
    try { f(); } catch (const std::logic_error&) { return; }
    throw std::runtime_error("expected fatal handler");
}
void VKAPI_PTR properties(VkPhysicalDevice, VkPhysicalDeviceProperties* out) { *out = props; }
void VKAPI_PTR format_properties(VkPhysicalDevice, VkFormat fmt, VkFormatProperties* out) {
    check(fmt == VK_FORMAT_S8_UINT, "queries stencil format");
    *out = {}; out->optimalTilingFeatures = stencil_features;
}
}
namespace nx {
HleRegistrar::HleRegistrar(const char* name, HleFn fn) { check(registry().emplace(name, fn).second, "unique handler"); }
void log(const char*, ...) {}
[[noreturn]] void fatal(const char* fmt, ...) {
    char text[512]; va_list args; va_start(args, fmt); std::vsnprintf(text, sizeof text, fmt, args); va_end(args);
    throw std::logic_error(text);
}
namespace vk {
PFN_vkGetPhysicalDeviceProperties p_vkGetPhysicalDeviceProperties = properties;
PFN_vkGetPhysicalDeviceFormatProperties p_vkGetPhysicalDeviceFormatProperties = format_properties;
}
namespace nvn {
uint64_t real_proc(CPU&, const char* name) {
    check(std::strcmp(name, "nvnDeviceBuilderGetFlags") == 0, "only SDK getter is called");
    return getter_available ? 0x4a6b20 : 0;
}
uint64_t guest_call(CPU& c, uint64_t fn, std::initializer_list<uint64_t> args) {
    if (fn == 0x4a6b20) {
        check(args.size() == 1 && *args.begin() == 0xbeef, "builder address passed to real getter");
        ++getter_calls; return builder_flags;
    }
    check(fn == 0xcafe, "registered guest callback target");
    check(args.size() == 6, "debug callback ABI argument count");
    ++callback_calls;
    std::copy(args.begin(), args.end(), callback_args.begin());
    check(std::string(reinterpret_cast<const char*>(callback_args[4])) == "invalid device coordinate mode", "callback message");
    if (callback_unregister) invoke("InstallDebugCallback", current_device, fn, 0xbabe, 0);
    c.x[9] = 0xdead; c.v[0].u64[0] = 0xdead; c.nf = 1;
    return 0;
}
bool gpu_ensure_initialized() { ++init_calls; return host_success; }
VkDevice vk_device() { return host_handles ? reinterpret_cast<VkDevice>(uintptr_t(1)) : VK_NULL_HANDLE; }
VkPhysicalDevice vk_physical() { return host_handles ? reinterpret_cast<VkPhysicalDevice>(uintptr_t(2)) : VK_NULL_HANDLE; }
const VkPhysicalDeviceFeatures& vk_features() { return features; }
void wait_submits_so_far(int site) { check(site == kWaitFinalize, "finalize wait site"); waits.push_back(1); }
void worker_finish() { waits.push_back(2); }
void gpu_finish_all() { waits.push_back(3); }
}
}

int main() try {
    using namespace nx;
    std::array<uint8_t, 0x1b88 + 32> guest;
    guest.fill(0xa5);
    current_device = reinterpret_cast<uint64_t>(guest.data() + 16);
    const auto device = current_device;
    check(query(0, 0) == 55 && query(0, 1) == kApiMinor, "B3 API 55.12 before init");
    check(query(0, 28) == 0 && query(0, 61) == 0, "no preinit host claims");
    check(init_calls == 0, "preinit query does not open host Vulkan");
    check(!invoke("Initialize", 0, 0xbeef) && !invoke("Initialize", device, 0), "reject null init");
    check(getter_calls == 0 && init_calls == 0, "invalid args do not initialize");
    getter_available = false;
    check(!invoke("Initialize", device, 0xbeef) && init_calls == 0, "missing SDK getter fails");
    getter_available = true;
    builder_flags = 0x100;
    check(!invoke("Initialize", device, 0xbeef) && init_calls == 0, "unsupported separate handles flag fails");
    builder_flags = 0;
    host_success = false;
    check(!invoke("Initialize", device, 0xbeef), "host initialization failure propagates");
    check(g_devices.empty(), "failure never publishes state");
    dies([&] { invoke("GetDepthMode", device); });
    host_success = true; host_handles = false;
    check(!invoke("Initialize", device, 0xbeef), "success without host handles rejected");
    check(g_devices.empty(), "partial backend never publishes state");
    host_handles = true;
    auto& l = props.limits;
    l.maxComputeWorkGroupSize[0] = 256; l.maxComputeWorkGroupSize[1] = 128; l.maxComputeWorkGroupSize[2] = 32;
    l.maxComputeWorkGroupInvocations = 256;
    l.maxComputeWorkGroupCount[0] = l.maxComputeWorkGroupCount[1] = l.maxComputeWorkGroupCount[2] = 65535;
    l.maxPerStageDescriptorUniformBuffers = 12; l.maxUniformBufferRange = 32768;
    l.maxPerStageDescriptorSampledImages = 16; l.maxPerStageDescriptorSamplers = 8;
    l.maxImageDimension2D = 8192; l.maxImageDimension3D = 1024; l.maxImageDimensionCube = 4096;
    l.maxImageArrayLayers = 1024; l.maxSamplerAnisotropy = 8; features.samplerAnisotropy = true;
    stencil_features = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    builder_flags = 0x88;
    check(invoke("Initialize", device, 0xbeef) == 1, "successful real host boundary publishes device");
    check(g_devices.at(device).flags == 0x88, "stores flags obtained through SDK getter");
    int before = init_calls;
    check(!invoke("Initialize", device, 0xbeef) && !invoke("Initialize", device + 8, 0xbeef), "duplicate and second device rejected");
    check(init_calls == before, "duplicate does not reinitialize host");
    check(query(device, 0) == 55 && query(device, 1) == kApiMinor, "live API version");
    check(query(device, 2) == 12 && query(device, 3) == 32768, "uniform limits clamped to host");
    check(query(device, 9) == 8 && query(device, 27) == 8, "sampler limits clamped");
    check(query(device, 28) == 256 && query(device, 29) == 128 && query(device, 30) == 32 && query(device, 31) == 256, "compute limits truthful");
    check(query(device, 41) == 1 && query(device, 42) == 1, "stencil features and ASTC decoder");
    check(query(device, 44) == 14 && query(device, 45) == 1024, "levels and layers from host");
    check(query(device, 61) == 8192 && query(device, 63) == 1024 && query(device, 64) == 4096, "image dimensions from host");
    for (auto pname : {40, 50, 59, 66, 67, 68, 69, 71, 72, 73, 74, 75, 76, 79, 87, 90, 97})
        check(query(device, pname) == 0, "unsupported path not advertised");
    check(query(device, 23) == 2 && query(device, 55) == 2048 && query(device, 24) == query(device, 23) * query(device, 55), "nonzero consistent virtual Maxwell scratch allocation contract");
    check(query(device, 99) == 256 && query(device, 104) == 16383, "B3 trailing SDK constants");
    dies([&] { query(device, 105); });
    dies([&] { invoke("GetInteger", device, 0, 0); });
    check(invoke("GetDepthMode", device) == 0 && invoke("GetWindowOriginMode", device) == 0, "SDK defaults");
    invoke("SetDepthMode", device, 1);
    check(waits == std::vector<int>({1,2,3}), "mode change drains replay and GPU before publish"); waits.clear();
    invoke("SetDepthMode", device, 1);
    check(waits.empty(), "same mode needs no drain");
    invoke("SetWindowOriginMode", device, 1); waits.clear();
    check(nvn::device_depth_mode() == 1 && nvn::device_window_origin_mode() == 1, "renderer exports actual B3 modes");
    char label[] = "B3 device";
    invoke("SetDebugLabel", device, reinterpret_cast<uint64_t>(label)); label[0] = '!';
    check(g_devices.at(device).label == "B3 device", "debug label is owned copy");
    invoke("InstallDebugCallback", device, 0xcafe, 0xbabe, 1);
    invoke("InstallDebugCallback", device, 0xcafe, 0xbabe, 1);
    check(g_devices.at(device).callbacks.size() == 1, "callback registration idempotent");
    CPU c{}; c.x[0] = device; c.x[1] = 2; c.x[9] = 42; c.v[0].u64[0] = 123;
    callback_unregister = true;
    dies([&] { registry().at("nvnDeviceSetDepthMode")(c); });
    check(callback_calls == 1 && callback_args[0] == 0 && callback_args[1] == 0 && callback_args[3] == 0 && callback_args[5] == 0xbabe, "guest callback invoked with API error and userdata");
    check(c.x[9] == 42 && c.v[0].u64[0] == 123 && c.nf == 0, "callback preserves CPU and SIMD");
    check(g_devices.at(device).callbacks.empty() && nvn::device_depth_mode() == 1, "reentrant unregister; invalid mode leaves state intact");
    auto id1 = invoke("GenerateDebugDomainId", device), id2 = invoke("GenerateDebugDomainId", device);
    check(id1 && id2 == id1 + 1, "unique debug domains");
    check(!invoke("IsExternalDebuggerAttached", device), "no external debugger");
    for (auto suffix : {"RegisterFastClearColor", "RegisterFastClearColori", "RegisterFastClearColorui", "RegisterFastClearDepth"})
        check(!invoke(suffix, device, 0xbad), "optional fast clear returns unsupported without dereferencing payload");
    invoke("Finalize", device);
    check(waits == std::vector<int>({1,2,3}), "finalize drains replay and GPU");
    check(g_devices.empty() && nvn::device_depth_mode() == 0 && nvn::device_window_origin_mode() == 0, "finalize removes lifecycle state");
    dies([&] { invoke("Finalize", device); });
    dies([&] { query(device, 0); });
    features.samplerAnisotropy = false; stencil_features = 0;
    check(invoke("Initialize", device, 0xbeef) == 1, "guest allocation reusable after finalize");
    check(query(device, 27) == 1 && query(device, 41) == 0, "weak host capability fallback");
    check(g_devices.at(device).label.empty() && g_devices.at(device).callbacks.empty(), "no stale label or callbacks after reuse");
    check(invoke("GenerateDebugDomainId", device) == id2 + 1, "domains not recycled");
    invoke("Finalize", device);
    check(std::all_of(guest.begin(), guest.end(), [](auto b) { return b == 0xa5; }), "device stays in side table, guest bytes untouched");
    std::puts("PASS: production NVN device lifecycle, SDK getter/host-init boundary, caps, modes, callbacks and failure paths");
    return 0;
} catch (const std::exception& e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
