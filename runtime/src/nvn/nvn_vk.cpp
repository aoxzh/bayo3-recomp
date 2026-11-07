// NVN's GPU side on Vulkan (docs/NVN.md, N3): the host device, a VkImage per NVN texture the GPU touches, the
// replay of recorded command lists, and presenting a window texture on the host swapchain.
//
// First version: every submit is recorded into one Vulkan command buffer and waited for (NX_PIPELINED, NX_ASYNC_GPU:
// into a slot's, not waited for, nvn_frames.cpp); image layouts are tracked per image, in submission order. The image
// behind a texture is nvn_image.cpp. On macOS this is MoltenVK: the instance enumerates portability drivers and the
// device enables VK_KHR_portability_subset.
#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>  // GetSystemInfo
#else
#  include <unistd.h>  // _exit and sysconf
#endif

#include <algorithm>
#include <cstdio>
#if defined(__unix__) || defined(__APPLE__)
#  include <dlfcn.h>  // NX_UPLOAD_HIST: the call site's module and offset (the shipped .so is stripped)
#endif
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "nvn_vk.h"
#include "nvn_compute_keep.h"
#include "nvn_draw_frame_stats.h"
#include "nvn_submit_diag.h"
#include "platform.h"

#include <nx/game.h>

namespace nx::nvn {

using namespace nx::vk;


namespace {

void host_import_diag_log(uint64_t frames);
std::recursive_mutex g_gpu;  // the whole GPU side: one submitting thread at a time
bool g_ready = false, g_failed = false;
VkInstance g_instance = VK_NULL_HANDLE;
VkPhysicalDevice g_physical = VK_NULL_HANDLE;
VkDevice g_device = VK_NULL_HANDLE;
VkQueue g_queue = VK_NULL_HANDLE;
uint32_t g_family = 0;
VkPhysicalDeviceMemoryProperties g_memory{};
// VK_EXT_external_memory_host (NX_HOST_IMPORT=1, nvn_written.cpp): whether the device has the extension, and the host
// pointer alignment its import requires.
bool g_host_import = false;
uint64_t g_host_align = 4096;
VkCommandPool g_pool = VK_NULL_HANDLE;
VkCommandBuffer g_cb = VK_NULL_HANDLE;
VkFence g_fence = VK_NULL_HANDLE;

VkSurfaceKHR g_surface = VK_NULL_HANDLE;
VkSwapchainKHR g_swapchain = VK_NULL_HANDLE;
std::vector<VkImage> g_sc_images;
VkExtent2D g_sc_extent{};
VkExtent2D g_sc_current{};  // the surface's currentExtent when the swapchain was made (SUBOPTIMAL: did it change?)
VkSurfaceTransformFlagBitsKHR g_sc_transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
VkFormat g_sc_format = VK_FORMAT_B8G8R8A8_UNORM;
VkSemaphore g_acquired = VK_NULL_HANDLE, g_rendered = VK_NULL_HANDLE;
VkBool32 g_depth_clip_control = VK_FALSE;
bool g_integrated = false;
// The device's properties, read once at device creation (device_properties()). The replay asks for limits per indirect
// draw and per descriptor batch; calling p_vkGetPhysicalDeviceProperties there cost 5.8 ms of a battle frame in the
// indexed indirect path alone (NX_REPLAY_PROF op 49: 712 calls/frame at 8.1 us each, two property reads per call).
VkPhysicalDeviceProperties g_props{};
bool g_require_sg32 = false;  // compute pipelines are made with a required subgroup size of 32 (Maxwell's warp)  // the chosen GPU is an integrated one (unified memory, the GPU is the long side)
VkPhysicalDeviceFeatures g_features{};  // the core features enabled (vk_features())
bool g_draw_indirect_count = false;
bool g_depth_range_unrestricted = false;
bool g_extended_dynamic = false, g_extended_dynamic_blend = false, g_pipeline_library = false;
ExtendedDynamicCommands g_dynamic_commands{};
// VK_EXT_vertex_attribute_divisor: NVN divides instanced streams by an arbitrary N, core Vulkan only 1.
bool g_vertex_divisor = false;
uint32_t g_vertex_divisor_max = 1;
VkDescriptorPool g_descriptors = VK_NULL_HANDLE;

// The per-submit upload buffer (nvn_vk.h): one host-visible buffer, bump-allocated, reset at every submit.
VkBuffer g_upload = VK_NULL_HANDLE;
VkDeviceMemory g_upload_memory = VK_NULL_HANDLE;
uint8_t* g_upload_ptr = nullptr;
bool g_upload_host_cached = false;
uint64_t g_upload_size = 0, g_upload_used = 0;
void make_upload_buffer(uint64_t size);

// Commands the Hades bridge intercepts are not in the generated table: ours are loaded here.
PFN_vkEnumerateInstanceExtensionProperties p_enum_instance_ext = nullptr;
PFN_vkCreateInstance p_create_instance = nullptr;
PFN_vkEnumeratePhysicalDevices p_enum_physical = nullptr;
PFN_vkCreateDevice p_create_device = nullptr;

}  // namespace

// A lost device means the GPU hung (a shader looping for ever): the process ends at once. Submitting more work, or
// waiting, keeps the GPU stuck, and on macOS that froze WindowServer and restarted the machine.
[[noreturn]] void gpu_hung(const char* why) {
    log("nvn/vk: GPU hang (%s): stopping", why);
    std::fprintf(stderr, "nvn/vk: GPU hang (%s): stopping\n", why);
    log_flush();
    _exit(4);
}

namespace {

bool check(VkResult r, const char* what) {
    if (r >= 0) return true;
    log("nvn/vk: %s failed (%d)", what, int(r));
    if (r == VK_ERROR_DEVICE_LOST) gpu_hung(what);
    return false;
}

bool has_extension(const std::vector<VkExtensionProperties>& list, const char* name) {
    for (auto& e : list)
        if (std::strcmp(e.extensionName, name) == 0) return true;
    return false;
}

uint64_t checked_host_import_alignment(uint64_t driver, uint64_t page) {
    auto valid = [](uint64_t n) { return n && !(n & (n - 1)) && n <= UINTPTR_MAX; };
    return valid(page) && valid(driver) ? std::max(page, driver) : 0;
}

uint64_t host_page_size() {
    // Importing different byte ranges of one host page can lock that physical page twice. If the OS query fails,
    // the optimization cannot be used safely.
    static const uint64_t page = []() -> uint64_t {
#if defined(_WIN32)
        SYSTEM_INFO info{};
        GetSystemInfo(&info);
        return info.dwPageSize;
#else
        const long value = sysconf(_SC_PAGESIZE);
        return value > 0 ? uint64_t(value) : 0;
#endif
    }();
    return page;
}

uint64_t host_import_alignment() {
    return checked_host_import_alignment(g_host_align, host_page_size());
}

// This runtime requests Vulkan 1.3. Do not use the core 1.1 query on a 1.0 physical device, even if a loader
// happens to return a non-null function pointer; we do not enable the corresponding KHR compatibility extensions.
bool host_import_buffer_supported(uint32_t api_version) {
    if (api_version < VK_API_VERSION_1_1 || !p_vkGetPhysicalDeviceExternalBufferProperties) return false;
    VkPhysicalDeviceExternalBufferInfo info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO};
    info.usage = kUploadUsage;
    info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    VkExternalBufferProperties props{VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
    p_vkGetPhysicalDeviceExternalBufferProperties(g_physical, &info, &props);
    const auto& external = props.externalMemoryProperties;
    return (external.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) &&
           (external.compatibleHandleTypes & VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT) &&
           !(external.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT);
}

void* g_gipa = nullptr;  // vkGetInstanceProcAddr, for surfaces made later (make_surface)
bool make_surface();

bool init() {
    if (g_ready || g_failed) return g_ready;
    g_failed = true;  // until everything below succeeds
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(plat::vulkan_get_instance_proc_addr());
    if (!gipa) {
        log("nvn/vk: no Vulkan loader");
        return false;
    }
    for (size_t k = 0; k < g_entry_count; k++) *g_entries[k].pfn = gipa(nullptr, g_entries[k].name);
    p_enum_instance_ext = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
        gipa(nullptr, "vkEnumerateInstanceExtensionProperties"));
#if defined(__linux__) && !defined(__ANDROID__)
    // RADV decodes ASTC on the GPU when asked, and a fight streams 4-5 GB of it: the texture bursts were the Deck's worst
    // frames on the replay thread (24-25 fps at their lowest, 26-27 with this). Other drivers ignore the variable.
    // vk_require_astc=false in the environment: decoded on the CPU, by nvn_astc.cpp.
    setenv("vk_require_astc", "true", 0);
#endif
    p_create_instance = reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));

    uint32_t n = 0;
    p_enum_instance_ext(nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> iext(n);
    p_enum_instance_ext(nullptr, &n, iext.data());
    std::vector<const char*> exts = {VK_KHR_SURFACE_EXTENSION_NAME, plat::vulkan_surface_extension()};
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    if (has_extension(iext, "VK_KHR_portability_enumeration")) {
        exts.push_back("VK_KHR_portability_enumeration");
        ici.flags |= 0x00000001;  // VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR
    }
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = nx::game::kInfo.title;
    app.apiVersion = VK_API_VERSION_1_3;
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = uint32_t(exts.size());
    ici.ppEnabledExtensionNames = exts.data();
    if (!check(p_create_instance(&ici, nullptr, &g_instance), "vkCreateInstance")) return false;
    for (size_t k = 0; k < g_entry_count; k++)
        if (auto p = gipa(g_instance, g_entries[k].name)) *g_entries[k].pfn = p;
    p_enum_physical = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(gipa(g_instance, "vkEnumeratePhysicalDevices"));
    p_create_device = reinterpret_cast<PFN_vkCreateDevice>(gipa(g_instance, "vkCreateDevice"));

    n = 0;
    p_enum_physical(g_instance, &n, nullptr);
    std::vector<VkPhysicalDevice> gpus(n);
    p_enum_physical(g_instance, &n, gpus.data());
    gpus.resize(n);
    g_physical = pick_physical_device(gpus);  // NX_GPU, default: the first discrete GPU (nvn_gpu_pick.cpp)
    if (!g_physical) {
        log("nvn/vk: no GPU");
        return false;
    }
    VkPhysicalDeviceProperties props{};
    p_vkGetPhysicalDeviceProperties(g_physical, &props);
    g_props = props;
    p_vkGetPhysicalDeviceMemoryProperties(g_physical, &g_memory);
    g_integrated = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
    // An integrated GPU shares the package's power budget with the CPU: ask the desktop for its performance profile
    // (NX_POWER_PROFILE=1: also with a discrete GPU; 0: never). frame_stats.cpp.
    if (g_integrated || (std::getenv("NX_POWER_PROFILE") && std::getenv("NX_POWER_PROFILE")[0] == '1'))
        plat::request_performance_profile();

    g_gipa = reinterpret_cast<void*>(gipa);
    if (!make_surface()) return false;

    p_vkGetPhysicalDeviceQueueFamilyProperties(g_physical, &n, nullptr);
    std::vector<VkQueueFamilyProperties> families(n);
    p_vkGetPhysicalDeviceQueueFamilyProperties(g_physical, &n, families.data());
    g_family = UINT32_MAX;
    for (uint32_t k = 0; k < n; k++) {
        VkBool32 present = VK_FALSE;
        p_vkGetPhysicalDeviceSurfaceSupportKHR(g_physical, k, g_surface, &present);
        if ((families[k].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
            g_family = k;
            break;
        }
    }
    if (g_family == UINT32_MAX) {
        log("nvn/vk: no graphics + present queue");
        return false;
    }

    p_vkEnumerateDeviceExtensionProperties(g_physical, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> dext(n);
    p_vkEnumerateDeviceExtensionProperties(g_physical, nullptr, &n, dext.data());
    std::vector<const char*> dexts = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    // VK_EXT_external_memory_host: NX_HOST_IMPORT binds a compute-written range to the guest bytes themselves instead
    // of a ring copy, so the GPU's results are already where the game reads them (nvn_written.cpp). Enabled whenever
    // the driver has it; nothing changes unless the switch asks for it.
    g_host_import = has_extension(dext, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME) &&
                    host_import_buffer_supported(props.apiVersion);
    if (g_host_import) dexts.push_back(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
    g_depth_range_unrestricted = has_extension(dext, VK_EXT_DEPTH_RANGE_UNRESTRICTED_EXTENSION_NAME);
    if (g_depth_range_unrestricted) dexts.push_back(VK_EXT_DEPTH_RANGE_UNRESTRICTED_EXTENSION_NAME);
    // VK_EXT_vertex_attribute_divisor (core in Vulkan 1.4 as VK_KHR_vertex_attribute_divisor): NVN's
    // nvnVertexStreamState divisor is "advance every N instances" for any N, while core Vulkan only expresses
    // per-vertex (1) and per-instance (also 1). Without the extension divisor 2/4/8 streams silently advanced once
    // per instance (nvn_pipeline.cpp), so every instanced batch -- bone palettes, particles, grass, UI quads -- read
    // the wrong slice. Enable it whenever the GPU has it, with the largest divisor it accepts.
    const bool has_divisor_ext = has_extension(dext, VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME);
    // Vulkan 1.4 folded the extension into core as VK_KHR_vertex_attribute_divisor; a driver may expose only that name.
    const bool has_divisor_khr = has_extension(dext, "VK_KHR_vertex_attribute_divisor");
    const bool has_divisor = has_divisor_ext || has_divisor_khr;
    VkPhysicalDeviceVertexAttributeDivisorFeatures fdivisor{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES};
    if (has_divisor) {
        // The maximum the game may ask for (bounded below by the core limit of 1 when the driver reports nothing).
        VkPhysicalDeviceVertexAttributeDivisorProperties dprops{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &dprops;
        p_vkGetPhysicalDeviceProperties2(g_physical, &p2);
        g_vertex_divisor_max = std::max(1u, dprops.maxVertexAttribDivisor);
        // Enable the name this driver actually exposes (the core KHR alias needs no separate enable on 1.4+).
        if (has_divisor_ext) dexts.push_back(VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME);
    }
    // Experimental pipeline optimizations. Extension presence alone is not a
    // feature: query each feature and enable exactly the bits used below.
    const char* opt_dynamic = std::getenv("NX_EXTENDED_DYNAMIC_STATE");
    const bool want_dynamic = opt_dynamic && opt_dynamic[0] == '1';
    const char* opt_gpl = std::getenv("NX_GRAPHICS_PIPELINE_LIBRARY");
    const bool want_gpl = opt_gpl && opt_gpl[0] == '1';
    const bool has_eds3 = want_dynamic && has_extension(dext, VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME);
    const bool has_gpl = want_gpl && has_extension(dext, VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME) &&
                         has_extension(dext, VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME);
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT feds3{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT fgpl{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT};
    if (has_eds3) dexts.push_back(VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME);
    if (has_gpl) {
        dexts.push_back(VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME);
        dexts.push_back(VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME);
    }
    // Features: everything core the GPU has, dynamic rendering (1.3), 8-bit indices (NVN has them), clip-space
    // depth -1..1 (NVN's default depth mode, what Wonder uses), and on MoltenVK the portability subset's own
    // features, which must be enabled to be used.
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceIndexTypeUint8FeaturesEXT fu8{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT};
    VkPhysicalDeviceDepthClipControlFeaturesEXT fclip{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT};
    struct {  // VkPhysicalDevicePortabilitySubsetFeaturesKHR, without its header (beta extensions)
        VkStructureType sType = VkStructureType(1000163000);
        void* pNext = nullptr;
        VkBool32 fields[15] = {};
    } fport;
    features.pNext = &f13;
    f13.pNext = &f12;
    void** tail = &f12.pNext;
    if (has_extension(dext, "VK_EXT_index_type_uint8")) {
        dexts.push_back("VK_EXT_index_type_uint8");
        *tail = &fu8, tail = &fu8.pNext;
    }
    if (has_extension(dext, "VK_EXT_depth_clip_control")) {
        dexts.push_back("VK_EXT_depth_clip_control");
        *tail = &fclip, tail = &fclip.pNext;
    }
    if (has_extension(dext, "VK_KHR_portability_subset")) {
        dexts.push_back("VK_KHR_portability_subset");
        *tail = &fport, tail = &fport.pNext;
    }
    // The divisor feature belongs in this chain so the query fills it and the device create enables it.
    if (has_divisor) *tail = &fdivisor, tail = &fdivisor.pNext;
    if (has_eds3) *tail = &feds3, tail = &feds3.pNext;
    if (has_gpl) *tail = &fgpl, tail = &fgpl.pNext;
    p_vkGetPhysicalDeviceFeatures2(g_physical, &features);
    const bool blend_features = has_eds3 && feds3.extendedDynamicState3ColorBlendEnable &&
                                feds3.extendedDynamicState3ColorBlendEquation &&
                                feds3.extendedDynamicState3ColorWriteMask;
    void* eds3_next = feds3.pNext;
    feds3 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
    feds3.pNext = eds3_next;
    feds3.extendedDynamicState3ColorBlendEnable = blend_features;
    feds3.extendedDynamicState3ColorBlendEquation = blend_features;
    feds3.extendedDynamicState3ColorWriteMask = blend_features;
    g_pipeline_library = has_gpl && fgpl.graphicsPipelineLibrary;
    {   // The translated shaders assume Maxwell's warp of 32 (TRANSLATOR.md). Intel runs subgroups of 8, 16 or 32 as its
        // compiler chooses; where the size can be pinned (core 1.3 subgroup size control), compute pipelines pin 32.
        VkPhysicalDeviceSubgroupSizeControlProperties sgc{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &sgc;
        p_vkGetPhysicalDeviceProperties2(g_physical, &p2);
        const char* e = std::getenv("NX_SUBGROUP32");
        bool can = f13.subgroupSizeControl && sgc.minSubgroupSize <= 32 && sgc.maxSubgroupSize >= 32 &&
                   (sgc.requiredSubgroupSizeStages & VK_SHADER_STAGE_COMPUTE_BIT);
        g_require_sg32 = can && e && e[0] == '1';  // experimental, off by default: no visible effect on the Intel silhouettes
        log("nvn/vk: subgroup sizes %u..%u, compute subgroup size %s", sgc.minSubgroupSize, sgc.maxSubgroupSize,
            g_require_sg32 ? "pinned to 32" : "left to the driver");
    }
    g_draw_indirect_count = f12.drawIndirectCount != VK_FALSE;
    // The chain still carries fdivisor into the device create, so setting it TRUE here is what enables the feature.
    g_vertex_divisor = has_divisor && fdivisor.vertexAttributeInstanceRateDivisor == VK_TRUE;
    fdivisor.vertexAttributeInstanceRateDivisor = g_vertex_divisor ? VK_TRUE : VK_FALSE;
    fdivisor.vertexAttributeInstanceRateZeroDivisor = VK_FALSE;  // NVN has no zero-divisor streams
    log("nvn/vk: vertex attribute divisor %s (max %u)%s", g_vertex_divisor ? "on" : "off", g_vertex_divisor_max,
        g_vertex_divisor ? "" : ": instanced streams advance once per instance");
    // Only the requested 1.2 feature belongs in the device-enable chain.
    f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, f12.pNext};
    f12.drawIndirectCount = g_draw_indirect_count ? VK_TRUE : VK_FALSE;
    g_depth_clip_control = fclip.depthClipControl;
    g_features = features.features;
    // NX_ASTC_DECODE=1|linear|srgb: the CPU decoder even where the GPU has ASTC (to compare the two on one GPU).
    set_astc_native(g_features.textureCompressionASTC_LDR);
    log("nvn/vk: ASTC %s", astc_native() ? "native" : "decoded to RGBA8 at upload (no textureCompressionASTC_LDR)");
    log("nvn/vk: depthClamp %u, depthBiasClamp %u, depthBounds %u, depth clip control %u (extension %u)",
        g_features.depthClamp, g_features.depthBiasClamp, g_features.depthBounds, g_depth_clip_control,
        has_extension(dext, "VK_EXT_depth_clip_control") ? 1u : 0u);
    log("nvn/vk: portability: format reinterpretation %u, format swizzle %u", fport.fields[2], fport.fields[3]);
    log_device_caps(g_physical, dext);  // nvn_caps.cpp
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = g_family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = uint32_t(dexts.size());
    dci.ppEnabledExtensionNames = dexts.data();
    dci.pNext = &features;
    if (!check(p_create_device(g_physical, &dci, nullptr, &g_device), "vkCreateDevice")) return false;
    auto gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(gipa(g_instance, "vkGetDeviceProcAddr"));
    if (want_dynamic && gdpa && props.apiVersion >= VK_API_VERSION_1_3) {
        g_dynamic_commands.cull = reinterpret_cast<PFN_vkCmdSetCullMode>(gdpa(g_device, "vkCmdSetCullMode"));
        g_dynamic_commands.front = reinterpret_cast<PFN_vkCmdSetFrontFace>(gdpa(g_device, "vkCmdSetFrontFace"));
        g_extended_dynamic = g_dynamic_commands.cull && g_dynamic_commands.front;
    }
    if (blend_features && gdpa) {
        g_dynamic_commands.blend_enable = reinterpret_cast<PFN_vkCmdSetColorBlendEnableEXT>(
            gdpa(g_device, "vkCmdSetColorBlendEnableEXT"));
        g_dynamic_commands.blend_equation = reinterpret_cast<PFN_vkCmdSetColorBlendEquationEXT>(
            gdpa(g_device, "vkCmdSetColorBlendEquationEXT"));
        g_dynamic_commands.color_mask = reinterpret_cast<PFN_vkCmdSetColorWriteMaskEXT>(
            gdpa(g_device, "vkCmdSetColorWriteMaskEXT"));
        g_extended_dynamic_blend = g_dynamic_commands.blend_enable && g_dynamic_commands.blend_equation &&
                                   g_dynamic_commands.color_mask;
    }
    log("nvn/vk: experimental dynamic cull/front %s, dynamic blend %s, pipeline libraries %s",
        g_extended_dynamic ? "on" : "off", g_extended_dynamic_blend ? "on" : "off",
        g_pipeline_library ? "on" : "off");
    p_vkGetDeviceQueue(g_device, g_family, 0, &g_queue);
    if (g_host_import) {
        VkPhysicalDeviceExternalMemoryHostPropertiesEXT hp{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &hp;
        p_vkGetPhysicalDeviceProperties2(g_physical, &p2);
        g_host_align = hp.minImportedHostPointerAlignment;
        const uint64_t alignment = host_import_alignment();
        g_host_import = p_vkGetMemoryHostPointerPropertiesEXT != nullptr && alignment != 0;
        log("nvn/vk: host memory import %s (VK_EXT_external_memory_host, alignment %llu)", g_host_import ? "available" : "unavailable",
            (unsigned long long)alignment);
    }

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = g_family;
    p_vkCreateCommandPool(g_device, &pci, nullptr, &g_pool);
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = g_pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    p_vkAllocateCommandBuffers(g_device, &cai, &g_cb);
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    p_vkCreateFence(g_device, &fci, nullptr, &g_fence);
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    p_vkCreateSemaphore(g_device, &sci, nullptr, &g_acquired);
    p_vkCreateSemaphore(g_device, &sci, nullptr, &g_rendered);

    // A busy submit reached 67 MB: the 2048x8192 glyph atlas's first upload (R8 widened to RGBA8), which now has a
    // staging buffer of its own (nvn_upload.cpp); the scripted run's other submits stay under ~12 MB. Android: 64 MB
    // (docs/archive/nvn/android-perf/PIPELINES-1810.md: 0 "upload buffer full" lines at 64 and at 32 MB on the PC; staging_wanted
    // spills bigger uploads to staging buffers in proportion to the ring).
    {
#if defined(__ANDROID__)
        uint64_t ring_mb = 64;
#else
        uint64_t ring_mb = 256;
#endif
        if (const char* e = std::getenv("NX_UPLOAD_RING_MB"))  // config.txt; 16..1024
            if (long v = std::atol(e); v >= 16 && v <= 1024) ring_mb = uint64_t(v);
        log("nvn/vk: upload ring %llu MB", (unsigned long long)ring_mb);
        make_upload_buffer(ring_mb << 20);
    }
    if (async_gpu())
        frame_init(g_pool, g_family, g_upload, g_upload_ptr, g_upload_size, g_upload_host_cached);  // the slots' ring
    else
        g_descriptors = make_descriptor_pool();
    log("nvn/vk: %s, Vulkan %u.%u, queue family %u", props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion),
        VK_API_VERSION_MINOR(props.apiVersion), g_family);
    g_failed = false;
    g_ready = true;
    return true;
}

// The window's surface, and the swapchain format from what the surface offers: B8G8R8A8_UNORM (the Mac), else
// R8G8B8A8_UNORM (Android surfaces offer no BGRA: a swapchain made with it showed nothing on an Adreno 740), else the
// first. A new Android window (the app back from home or the lock screen) needs a new surface (gpu_present).
void* g_window = nullptr;
uint64_t g_window_gen = 0;
bool make_surface() {
    if (g_surface) p_vkDestroySurfaceKHR(g_instance, g_surface, nullptr);
    if (g_window) plat::release_window(g_window);
    g_surface = VK_NULL_HANDLE;
    g_window = plat::create_window();  // waits for a window (Android); the generation is read once it is there
    g_window_gen = plat::window_generation();
    uint64_t surface = 0;
    if (!check(VkResult(plat::vulkan_create_surface(g_instance, g_window, &surface, g_gipa)), "surface")) return false;
    g_surface = reinterpret_cast<VkSurfaceKHR>(surface);
    uint32_t n = 0;
    p_vkGetPhysicalDeviceSurfaceFormatsKHR(g_physical, g_surface, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(n);
    p_vkGetPhysicalDeviceSurfaceFormatsKHR(g_physical, g_surface, &n, formats.data());
    auto offered = [&](VkFormat f) {
        for (const VkSurfaceFormatKHR& x : formats)
            if (x.format == f) return true;
        return false;
    };
    g_sc_format = offered(VK_FORMAT_B8G8R8A8_UNORM)   ? VK_FORMAT_B8G8R8A8_UNORM
                  : offered(VK_FORMAT_R8G8B8A8_UNORM) ? VK_FORMAT_R8G8B8A8_UNORM
                  : n                                  ? formats[0].format
                                                       : VK_FORMAT_B8G8R8A8_UNORM;
    log("nvn/vk: surface: %u formats offered, swapchain format %d", n, int(g_sc_format));
    return true;
}

bool make_swapchain() {
    if (g_swapchain) {
        if (async_gpu()) frame_wait_all();
        p_vkDeviceWaitIdle(g_device);
        present_pass_release();
        p_vkDestroySwapchainKHR(g_device, g_swapchain, nullptr);
        g_swapchain = VK_NULL_HANDLE;
    }
    VkSurfaceCapabilitiesKHR caps{};
    p_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_physical, g_surface, &caps);
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
        uint32_t w = 0, h = 0;
        plat::window_size(w, h);
        extent = {w ? w : 1280, h ? h : 720};
    }
    if (!extent.width || !extent.height) return false;  // minimized
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = g_surface;
    ci.minImageCount = std::max(caps.minImageCount, 3u);
    if (caps.maxImageCount) ci.minImageCount = std::min(ci.minImageCount, caps.maxImageCount);
    ci.imageFormat = g_sc_format;
    ci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    // The present pass (nvn_present.cpp) draws the picture pre-rotated: the images are in the display's own
    // orientation (portrait on a phone held sideways). Without it the images stay upright (the Mac: identity).
    bool sideways = caps.currentTransform & (VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR | VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR);
    bool pass = present_pass_wanted() && !present_pass_failed();
    g_sc_current = caps.currentExtent;
    if (pass && sideways && extent.width > extent.height) std::swap(extent.width, extent.height);
    if (!pass && (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR))
        ci.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    // Some drivers report maxImageExtent in the other orientation: the larger side bounds both.
    uint32_t most = std::max(caps.maxImageExtent.width, caps.maxImageExtent.height);
    if (most) extent.width = std::min(extent.width, most), extent.height = std::min(extent.height, most);
    ci.imageExtent = extent;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & ci.compositeAlpha))
        for (VkCompositeAlphaFlagBitsKHR a : {VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                                              VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR})
            if (caps.supportedCompositeAlpha & a) {
                ci.compositeAlpha = a;
                break;
            }
    log("nvn/vk: surface %ux%u (current %ux%u), transform %u of %#x, composite alpha %#x of %#x, usage %#x",
        extent.width, extent.height, caps.currentExtent.width, caps.currentExtent.height, unsigned(caps.currentTransform),
        caps.supportedTransforms, unsigned(ci.compositeAlpha), caps.supportedCompositeAlpha,
        caps.supportedUsageFlags);
    // The game paces itself on its vsync event (hle_vi.cpp, 60 Hz): a FIFO present also waited for the display's own
    // refresh, two clocks drifting against each other (stutter near 60 fps, docs/archive/nvn/DEBUG.md #33). MAILBOX never
    // blocks and shows the newest frame at each refresh; else IMMEDIATE (MoltenVK: no display sync in the Metal
    // layer, the macOS compositor still shows whole frames at its refresh), FIFO last. NX_FIFO=1: FIFO (DEBUG.md #62).
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    uint32_t modes_n = 0;
    p_vkGetPhysicalDeviceSurfacePresentModesKHR(g_physical, g_surface, &modes_n, nullptr);
    std::vector<VkPresentModeKHR> modes(modes_n);
    p_vkGetPhysicalDeviceSurfacePresentModesKHR(g_physical, g_surface, &modes_n, modes.data());
    auto has = [&](VkPresentModeKHR m) { return std::find(modes.begin(), modes.end(), m) != modes.end(); };
    if (!std::getenv("NX_FIFO")) {
        if (has(VK_PRESENT_MODE_MAILBOX_KHR))
            ci.presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
        else if (has(VK_PRESENT_MODE_IMMEDIATE_KHR))
            ci.presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    }
    static const char* const mode_names[] = {"IMMEDIATE", "MAILBOX", "FIFO", "FIFO_RELAXED"};
    log("nvn/vk: present mode %s (%u modes offered)", ci.presentMode <= 3 ? mode_names[ci.presentMode] : "?", modes_n);
    ci.clipped = VK_TRUE;
    if (!check(p_vkCreateSwapchainKHR(g_device, &ci, nullptr, &g_swapchain), "vkCreateSwapchainKHR")) return false;
    uint32_t n = 0;
    p_vkGetSwapchainImagesKHR(g_device, g_swapchain, &n, nullptr);
    g_sc_images.resize(n);
    p_vkGetSwapchainImagesKHR(g_device, g_swapchain, &n, g_sc_images.data());
    g_sc_extent = extent;
    g_sc_transform = ci.preTransform;
    if (n > 8) log("nvn/vk: %u swapchain images: more than the 8 present semaphores (nvn_frames.cpp)", n);
    bool drawn = pass && present_pass_targets(g_sc_format, g_sc_images, extent, ci.preTransform);
    if (pass && !drawn && present_pass_failed()) {  // the pass is out: an upright swapchain for the blit
        log("nvn/vk: present pass failed: swapchain made again for the blit");
        return make_swapchain();
    }
    log("nvn/vk: swapchain %ux%u, %u images, transform %u, %s", extent.width, extent.height, n,
        unsigned(ci.preTransform), drawn ? "drawn by the present pass" : "blitted");
    return true;
}

}  // namespace

std::recursive_mutex& gpu_lock() { return g_gpu; }
bool gpu_ensure_initialized() {
    std::lock_guard lk(g_gpu);
    return init();
}
void gpu_finish_all() {
    std::lock_guard lk(g_gpu);
    if (!g_ready) return;
    if (async_gpu()) frame_wait_all();
    if (check(p_vkQueueWaitIdle(g_queue), "vkQueueWaitIdle")) replay::written_kept_shutdown();
}
VkDevice vk_device() { return g_device; }
VkPhysicalDevice vk_physical() { return g_physical; }
VkQueue vk_queue() { return g_queue; }
uint32_t vk_queue_family() { return g_family; }
bool vk_depth_clip_control() { return g_depth_clip_control; }
const ExtendedDynamicCommands& vk_extended_dynamic_commands() { return g_dynamic_commands; }
bool vk_extended_dynamic() { return g_extended_dynamic; }
bool vk_extended_dynamic_blend() { return g_extended_dynamic_blend; }
bool vk_graphics_pipeline_library() { return g_pipeline_library; }
bool vk_draw_indirect_count() { return g_draw_indirect_count; }
bool vk_depth_range_unrestricted() { return g_depth_range_unrestricted; }
bool vk_vertex_divisor() { return g_vertex_divisor; }
uint32_t vk_vertex_divisor_max() { return g_vertex_divisor_max; }
const VkPhysicalDeviceFeatures& vk_features() { return g_features; }
const VkPhysicalDeviceProperties& device_properties() { return g_props; }
bool gpu_is_integrated() { return g_integrated; }
bool gpu_require_subgroup32() { return g_require_sg32; }

uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t k = 0; k < g_memory.memoryTypeCount; k++)
        if ((bits >> k) & 1 && (g_memory.memoryTypes[k].propertyFlags & want) == want) return k;
    return 0;
}

// NX_UPLOAD_HIST=1: which allocations make the frame's "upload" number. A constant multi-MB upload every frame is
// how a texture that only changed in part is noticed (nvn_upload.cpp uploads the written rows only); the histogram
// names it by size, and the sizes map back to textures and buffers.
namespace {
struct AllocBucket { uint64_t size, count; const void* site; };
AllocBucket g_alloc_hist[24];
int g_alloc_hist_n = 0;
// Where each size comes from, so the histogram names a call site and not just a number: "some 16.8 MB allocation
// every frame" is not actionable until it says which of the ten upload_alloc call sites made it.
void note_alloc_at(uint64_t size, const void* site) {
    if (size < 65536) return;  // the frame's number is made of the big ones
    for (int k = 0; k < g_alloc_hist_n; ++k)
        if (g_alloc_hist[k].size == size && g_alloc_hist[k].site == site)
            return void(++g_alloc_hist[k].count);
    if (g_alloc_hist_n < int(sizeof g_alloc_hist / sizeof g_alloc_hist[0]))
        g_alloc_hist[g_alloc_hist_n++] = {size, 1, site};
}
// Where the call site is, as an offset into the executable. Windows has no dladdr, but the module base is one call
// away and "exe+offset" resolves offline against the build's .map/PDB (or llvm-symbolizer on the unstripped exe)
// exactly as the POSIX "so+offset" does. Without this the histogram printed "via ?" and the question it exists to
// answer -- which call site allocates 16.8 MB every frame -- could not be asked at all on Windows.
void site_name(const void* site, char* out, size_t n) {
#if defined(__unix__) || defined(__APPLE__)
    Dl_info info{};
    if (site && dladdr(site, &info) && info.dli_fbase) {
        const uint64_t off = uint64_t(uintptr_t(site)) - uint64_t(uintptr_t(info.dli_fbase));
        std::snprintf(out, n, "%s+0x%llx", info.dli_fname ? info.dli_fname : "?",
                      (unsigned long long)off);
        return;
    }
#elif defined(_WIN32)
    if (site) {
        // The exe is the only module the runtime's own upload_alloc can be called from, and GetModuleHandleA(nullptr)
        // is its base; no SymInitialize/dbghelp needed for an offset that gets symbolised offline anyway.
        const uintptr_t base = uintptr_t(GetModuleHandleA(nullptr));
        if (base && uintptr_t(site) >= base) {
            std::snprintf(out, n, "exe+0x%llx", (unsigned long long)(uintptr_t(site) - base));
            return;
        }
    }
#endif
    (void)site;
    std::snprintf(out, n, "?");
}
}  // namespace

VkMemoryPropertyFlags memory_type_properties(uint32_t type) {
    return type < g_memory.memoryTypeCount ? g_memory.memoryTypes[type].propertyFlags : 0;
}

Upload upload_alloc(uint64_t size, uint64_t align) {
    // Read once: upload_alloc runs tens of thousands of times per frame from the replay thread, and std::getenv walks
    // the environment every call (1.02% of the replay thread in the battle profile, docs/deck-cpu-audit/replay-thread.perf).
    static const bool upload_hist = std::getenv("NX_UPLOAD_HIST") != nullptr;
    if (upload_hist) note_alloc_at(size, __builtin_return_address(0));
    if (async_gpu()) return frame_upload_alloc(size, align);
    uint64_t at = (g_upload_used + align - 1) & ~(align - 1);
    if (at + size > g_upload_size) {
        log("nvn/vk: upload buffer full (%llu + %llu bytes)", (unsigned long long)at, (unsigned long long)size);
        at = 0;  // wraps: this submit's earlier data may be overwritten (grow the buffer when this shows)
        replay::dedup_reset();  // shared copies may be among them
    }
    g_upload_used = at + size;
    return {g_upload, at, g_upload_ptr + at, false, g_upload_host_cached};
}

Upload upload(const void* data, uint64_t size, uint64_t align) {
    Upload u = upload_alloc(size, align);
    if (data && size) std::memcpy(u.ptr, data, size);
    return u;
}

VkDescriptorSet descriptor_set(VkDescriptorSetLayout layout) {
    if (async_gpu()) return frame_descriptor_set(layout);
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = g_descriptors;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (p_vkAllocateDescriptorSets(g_device, &ai, &set) != VK_SUCCESS) log("nvn/vk: descriptor pool exhausted");
    return set;
}

VkDescriptorPool make_descriptor_pool() {
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 65536}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16384},
                                    {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 65536}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4096},
                                    {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1024}, {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1024},
                                    {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 65536}};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 32768;
    dpi.poolSizeCount = uint32_t(sizeof sizes / sizeof sizes[0]);
    dpi.pPoolSizes = sizes;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    p_vkCreateDescriptorPool(g_device, &dpi, nullptr, &pool);
    return pool;
}

namespace {

void make_upload_buffer(uint64_t size) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = kUploadUsage;
    p_vkCreateBuffer(g_device, &bi, nullptr, &g_upload);
    VkMemoryRequirements req{};
    p_vkGetBufferMemoryRequirements(g_device, g_upload, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = upload_memory_type(req.memoryTypeBits);  // nvn_caps.cpp (NX_RING_CACHED)
    p_vkAllocateMemory(g_device, &ai, nullptr, &g_upload_memory);
    p_vkBindBufferMemory(g_device, g_upload, g_upload_memory, 0);
    void* p = nullptr;
    p_vkMapMemory(g_device, g_upload_memory, 0, VK_WHOLE_SIZE, 0, &p);
    g_upload_ptr = static_cast<uint8_t*>(p);
    g_upload_host_cached = p && (memory_type_properties(ai.memoryTypeIndex) & VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    g_upload_size = size;
}

bool begin(bool replay = false) {
    if (async_gpu()) return (g_cb = frame_begin(replay)) != VK_NULL_HANDLE;
    if (!check(p_vkResetCommandBuffer(g_cb, 0), "vkResetCommandBuffer")) return false;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    return check(p_vkBeginCommandBuffer(g_cb, &bi), "vkBeginCommandBuffer");
}

// Where the submitting thread's time goes, logged with the draw stats, per presented frame: the replay (recording
// into Vulkan), vkQueueSubmit (Metal encoding happens inside it), fences waited for, the rest of gpu_submit (object
// drain, image frees, write-backs), and gpu_present as a whole; bytes through the upload buffer.
struct Timing {
    uint64_t submits = 0, replay_ns = 0, encode_ns = 0, fence_ns = 0, submit_ns = 0, present_ns = 0, upload_bytes = 0;
} g_timing;

void end_and_wait(VkSemaphore wait, VkSemaphore signal) {
    // Nothing that follows a failed submit or wait may consume GPU output or
    // signal a guest fence. Some Vulkan errors are nonfatal in check(), but
    // there is no safe completion path for this command buffer after one.
    auto require_success = [](VkResult result, const char* operation) {
        if (result != VK_SUCCESS) {
            check(result, operation);  // device loss stops immediately
            fatal("nvn/vk: %s failed (%d); submit cannot complete", operation, int(result));
        }
    };
    uint64_t t0 = plat::monotonic_ns();
    require_success(p_vkEndCommandBuffer(g_cb), "vkEndCommandBuffer");
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &g_cb;
    if (wait) {
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &wait;
        si.pWaitDstStageMask = &stage;
    }
    if (signal) {
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &signal;
    }
    require_success(p_vkResetFences(g_device, 1, &g_fence), "vkResetFences");
    require_success(p_vkQueueSubmit(g_queue, 1, &si, g_fence), "vkQueueSubmit");
    uint64_t t1 = plat::monotonic_ns();
    // 2 s at most: a frame's work takes milliseconds; longer is a shader that does not end.
    VkResult w = p_vkWaitForFences(g_device, 1, &g_fence, VK_TRUE, 2'000'000'000ull);
    if (w == VK_TIMEOUT) gpu_hung("a submit did not finish in 2 s");
    require_success(w, "vkWaitForFences");
    frame_times().encode_ns += t1 - t0, frame_times().fence_ns += plat::monotonic_ns() - t1;
}

void log_timing(uint64_t frames) {
    auto ms = [&](uint64_t ns) { return double(ns) / 1e6 / double(frames); };
    static uint64_t last = 0;  // wall clock: the real frame rate
    uint64_t now = plat::monotonic_ns();
    double fps = last ? double(frames) * 1e9 / double(now - last) : 0.0;
    last = now;
    const Timing& t = g_timing;
    uint64_t known = t.replay_ns + t.encode_ns + t.fence_ns;
    log("nvn/vk: per frame over %llu: %.1f fps, %.1f submits, replay %.2f ms, submit (encode) %.2f ms, fence wait "
        "%.2f ms, other %.2f ms, present %.2f ms, upload %.2f MB",
        (unsigned long long)frames, fps, double(t.submits) / double(frames), ms(t.replay_ns), ms(t.encode_ns),
        ms(t.fence_ns), ms(t.submit_ns > known ? t.submit_ns - known : 0), ms(t.present_ns),
        double(t.upload_bytes) / 1e6 / double(frames));
    if (std::getenv("NX_UPLOAD_HIST") && g_alloc_hist_n) {
        std::sort(g_alloc_hist, g_alloc_hist + g_alloc_hist_n,
                  [](const AllocBucket& a, const AllocBucket& b) { return a.size * a.count > b.size * b.count; });
        char line[1024] = {};
        size_t at = 0;
        for (int k = 0; k < g_alloc_hist_n && at < sizeof line - 80; ++k) {
            char site[160];
            site_name(g_alloc_hist[k].site, site, sizeof site);
            const char* base = std::strrchr(site, '/');
            at += size_t(std::snprintf(line + at, sizeof line - at, "%s%llu B x%llu via %s", k ? ", " : "",
                                       (unsigned long long)g_alloc_hist[k].size,
                                       (unsigned long long)g_alloc_hist[k].count,
                                       base ? base + 1 : site));
        }
        log("nvn/vk: upload allocations over %llu frames: %s", (unsigned long long)frames, line);
        g_alloc_hist_n = 0;
    }
    host_import_diag_log(frames);
    worker_log(frames);  // nvn_worker.cpp: replay thread busy vs waiting for a job
    frame_log(frames);
    creation_log(frames);  // pipelines and ASTC decodes of the window; saves the pipeline cache now and then
    g_timing = {};
}

// gpu_present's time, whatever way it returns.
struct PresentTimer {
    uint64_t t0 = plat::monotonic_ns();
    ~PresentTimer() { g_timing.present_ns += plat::monotonic_ns() - t0; }
};

}  // namespace

// NX_HOST_IMPORT (VK_EXT_external_memory_host): the guest bytes themselves as a VkBuffer. One import per host-aligned
// guest range, cached because the same few ranges are written every frame. The GPU writes guest memory directly, so
// the range needs no ring copy, before-snapshot or write-back. This is experimental: the guest mapping must remain valid
// for the life of each import, and CPU/GPU accesses to the same memory require appropriate synchronization.
namespace {
struct GuestImport {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    const uint8_t* lo = nullptr;  // the host-aligned first byte
    uint64_t cap = 0;             // imported bytes, a multiple of the host alignment
};
std::map<uintptr_t, GuestImport> g_guest_imports;
uint64_t g_guest_import_bytes = 0;
// Past this many imported bytes a bind keeps the ring path instead. Imports are never freed: the GPU of an earlier
// submit may still read one, and the ranges that matter churn only at scene changes.
constexpr uint64_t kGuestImportMax = 512ull << 20;
struct ImportDiag {
    uint64_t attempts = 0, hits = 0, made = 0, lookup_ns = 0;
    uint64_t fallback[9] = {}; // unavailable, alignment, padding, grow, budget, buffer, properties, allocation/bind, overlap
} g_import_diag;
uint64_t interval_bytes(std::vector<std::pair<uint64_t, uint64_t>> ranges) {
    std::sort(ranges.begin(), ranges.end());
    uint64_t total = 0, end = 0;
    for (auto [b, e] : ranges) {
        if (e > std::max(b, end)) total += e - std::max(b, end);
        end = std::max(end, e);
    }
    return total;
}
void host_import_diag_log(uint64_t frames) {
    if (!submit_diag_on()) return;
    const auto w = plat::write_watch_snapshot();
    std::vector<std::pair<uint64_t, uint64_t>> ranges, overlap;
    uint64_t overlapping_objects = 0;
    for (const auto& [lo, g] : g_guest_imports) {
        const uint64_t b = uint64_t(lo), e = b + g.cap;
        ranges.emplace_back(b, e);
        bool hit = false;
        for (uint64_t k = 0; k < w.count; ++k) {
            const uint64_t wb = std::max(b, w.ranges[k].begin), we = std::min(e, w.ranges[k].end);
            if (wb < we) { overlap.emplace_back(wb, we); hit = true; }
        }
        overlapping_objects += hit;
    }
    const auto& d = g_import_diag;
    log("nvn/submit-diag: imports over %llu frames: attempts %llu hits %llu made %llu lookup %.3f ms/frame; "
        "fallback unavailable/alignment/padding/grow/budget/buffer/properties/allocate/overlap %llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu; "
        "cached objects %zu allocated %.3f MiB unique %.3f MiB (not residency)",
        (unsigned long long)frames, (unsigned long long)d.attempts, (unsigned long long)d.hits,
        (unsigned long long)d.made, double(d.lookup_ns) / 1e6 / frames,
        (unsigned long long)d.fallback[0], (unsigned long long)d.fallback[1], (unsigned long long)d.fallback[2],
        (unsigned long long)d.fallback[3], (unsigned long long)d.fallback[4], (unsigned long long)d.fallback[5],
        (unsigned long long)d.fallback[6], (unsigned long long)d.fallback[7], (unsigned long long)d.fallback[8],
        g_guest_imports.size(),
        double(g_guest_import_bytes) / (1u << 20), double(interval_bytes(ranges)) / (1u << 20));
    log("nvn/submit-diag: watch snapshot active %llu overlap objects %llu unique %.3f MiB; "
        "interval add-attempts %llu rearm %llu remove %llu faults %llu (sampled ranges, not protection history)",
        (unsigned long long)w.count, (unsigned long long)overlapping_objects, double(interval_bytes(overlap)) / (1u << 20),
        (unsigned long long)w.adds, (unsigned long long)w.takes, (unsigned long long)w.removes, (unsigned long long)w.faults);
    g_import_diag = {};
}
}  // namespace

bool vk_host_import() { return g_host_import; }

Upload host_import_upload(const void* guest, uint64_t bytes, uint64_t guest_bytes) {
    Upload none{};
    const bool diag = submit_diag_on();
    struct Timer {
        bool on;
        uint64_t start;
        ~Timer() { if (on) g_import_diag.lookup_ns += plat::monotonic_ns() - start; }
    } timer{diag, diag ? plat::monotonic_ns() : 0};
    if (diag) ++g_import_diag.attempts;
    auto reject = [&](unsigned reason) { if (diag) ++g_import_diag.fallback[reason]; return none; };
    if (!g_host_import || !guest || !bytes || !guest_bytes) return reject(0);
    const uintptr_t a = reinterpret_cast<uintptr_t>(guest);
    if (a % 4) return reject(1);  // a storage binding's offset must be a multiple of 4
    const uint64_t align = host_import_alignment();
    if (!align) return reject(1);
    // Check every endpoint and rounding operation before deriving cache keys or calling Vulkan. In particular, an
    // invalid guest range near the end of the address space must not wrap into an apparently small cached range.
    if (bytes > UINTPTR_MAX - a || guest_bytes > UINTPTR_MAX - a) return reject(2);
    const uint64_t off = uint64_t(a & (align - 1));
    const uintptr_t base = a - uintptr_t(off);
    const auto* lo = reinterpret_cast<const uint8_t*>(base);
    const auto rounded = [off, align](uint64_t size, uint64_t& result) {
        if (size > UINT64_MAX - off) return false;
        size += off;
        if (size > UINT64_MAX - (align - 1)) return false;
        result = (size + align - 1) & ~(align - 1);
        return true;
    };
    uint64_t cap = 0, guest_cap = 0;
    if (!rounded(bytes, cap) || !rounded(guest_bytes, guest_cap) || cap > UINTPTR_MAX - base) return reject(2);
    // Restrict the import to host pages touched by the guest range itself. Driver alignment may be much larger
    // than a host page, so rounding guest_cap to the driver alignment alone is insufficient for this check.
    const uint64_t page = host_page_size();  // validated by host_import_alignment above
    const uintptr_t end = base + uintptr_t(cap);
    const uintptr_t first_guest_page = a & ~uintptr_t(page - 1);
    const uintptr_t last_guest_page = (a + uintptr_t(guest_bytes) - 1) & ~uintptr_t(page - 1);
    if (cap > guest_cap || base != first_guest_page || ((end - 1) & ~uintptr_t(page - 1)) > last_guest_page)
        return reject(2);
    auto it = g_guest_imports.lower_bound(base);
    const bool hit = it != g_guest_imports.end() && it->first == base;
    if (hit && it->second.cap < cap) return reject(3);  // a bigger view: ring path
    if (hit && diag) ++g_import_diag.hits;
    if (!hit) {
        // Same-start covered hits are the only reuse. Reject overlapping virtual pages: the extension does not
        // guarantee repeated import is safe. Physical aliases at different virtual addresses cannot be detected here.
        // The cache is ordered and already nonoverlapping, so only immediate neighbors can intersect [base, end).
        if (it != g_guest_imports.end() && it->first < end) return reject(8);
        if (it != g_guest_imports.begin()) {
            const auto previous = std::prev(it);
            const uintptr_t p = previous->first;
            if (previous->second.cap > UINTPTR_MAX - p || p + previous->second.cap > base) return reject(8);
        }
        if (g_guest_import_bytes > kGuestImportMax || cap > kGuestImportMax - g_guest_import_bytes) return reject(4);
        GuestImport g;
        VkExternalMemoryBufferCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
        external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.pNext = &external;
        bi.size = cap;
        bi.usage = kUploadUsage;  // the same usage the ring has: a written range is also read as storage/indirect/...
        if (p_vkCreateBuffer(g_device, &bi, nullptr, &g.buffer) != VK_SUCCESS) return reject(5);
        VkMemoryRequirements req{};
        p_vkGetBufferMemoryRequirements(g_device, g.buffer, &req);
        if (req.size > cap) {  // the import must not cover guest pages the allocation does not own
            p_vkDestroyBuffer(g_device, g.buffer, nullptr);
            return reject(7);
        }
        VkMemoryHostPointerPropertiesEXT hp{VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
        if (p_vkGetMemoryHostPointerPropertiesEXT(g_device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, lo,
                                                  &hp) != VK_SUCCESS) {
            p_vkDestroyBuffer(g_device, g.buffer, nullptr);
            return reject(6);
        }
        uint32_t type = UINT32_MAX;
        for (uint32_t k = 0; k < g_memory.memoryTypeCount; k++) {
            if (!((req.memoryTypeBits >> k) & 1) || !((hp.memoryTypeBits >> k) & 1)) continue;
            const VkMemoryPropertyFlags f = g_memory.memoryTypes[k].propertyFlags;
            const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            if ((f & want) == want) {  // the direct path does not flush or invalidate imported memory
                type = k;
                break;
            }
        }
        if (type == UINT32_MAX) {
            p_vkDestroyBuffer(g_device, g.buffer, nullptr);
            return reject(6);
        }
        VkImportMemoryHostPointerInfoEXT imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT};
        imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
        imp.pHostPointer = const_cast<uint8_t*>(lo);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.pNext = &imp;
        // Host pointer imports require an allocation size aligned to minImportedHostPointerAlignment.
        // cap satisfies that alignment and covers req.size; the entire range must remain valid host memory.
        ai.allocationSize = cap;
        ai.memoryTypeIndex = type;
        if (p_vkAllocateMemory(g_device, &ai, nullptr, &g.memory) != VK_SUCCESS) {
            p_vkDestroyBuffer(g_device, g.buffer, nullptr);
            return reject(7);
        }
        if (p_vkBindBufferMemory(g_device, g.buffer, g.memory, 0) != VK_SUCCESS) {
            p_vkFreeMemory(g_device, g.memory, nullptr);
            p_vkDestroyBuffer(g_device, g.buffer, nullptr);
            return reject(7);
        }
        g.lo = lo;
        g.cap = cap;
        it = g_guest_imports.emplace(base, g).first;
        g_guest_import_bytes += cap;
        if (diag) ++g_import_diag.made;
        log("nvn/vk: host import %p %llu bytes as VkBuffer (memory type %u)", static_cast<const void*>(lo),
            (unsigned long long)cap, type);
    }
    Upload u{};
    u.buffer = it->second.buffer;
    u.offset = off;
    u.ptr = const_cast<uint8_t*>(static_cast<const uint8_t*>(guest));
    u.direct = true;
    return u;
}

uint64_t draw_take_dump_request();          // nvn_draw.cpp

namespace {
std::shared_ptr<const DeferredValues> g_deferred;  // the submit being replayed (replay thread only)
}
const DeferredValues* current_deferred() { return g_deferred.get(); }

void gpu_submit(const std::vector<std::shared_ptr<CmdList>>& lists, std::shared_ptr<const DeferredValues> deferred,
                std::shared_ptr<const CommandListTable> captured,
                std::vector<std::function<void()>> completions) {
    std::lock_guard lk(g_gpu);
    g_deferred = std::move(deferred);
    struct SubmitCleanup {
        ~SubmitCleanup() { submit_lists_capture({}); g_deferred.reset(); }
    } cleanup;
    uint64_t t_in = plat::monotonic_ns();
    const FrameTimes f_in = frame_times();
    uint64_t lists_epoch = 0;
    for (auto& l : lists) lists_epoch = std::max(lists_epoch, l->epoch);
    ReplayScope replay(lists_epoch);  // the texture mark (#55, #57), then drain_objects (under the GPU lock, #50)
    if (!init()) {
        if (strict_runtime() || captured) fatal("nvn: GPU initialization failed during command submission");
        return;
    }
    bool async = async_gpu();
    if (!async) {  // the previous submit was waited for: all of it is free
        free_retired_images(UINT64_MAX);
        if (!check(p_vkResetDescriptorPool(g_device, g_descriptors, 0), "vkResetDescriptorPool"))
            fatal("nvn/vk: descriptor pool reset failed; submit cannot proceed");
        g_upload_used = 0;
    } else if (pipelined()) {  // what the retired submits were the last to use (they free nothing themselves)
        free_retired_images(frame_completed());
    }
    if (!begin(true)) {
        if (strict_runtime() || captured) fatal("nvn: GPU command buffer begin failed during command submission");
        return;
    }
    draw_begin_submit();
    replay::written_carry_seed(g_cb);
    submit_lists_capture(std::move(captured));
    gpu_profile_begin(g_cb);
    zero_draws_begin_submit(g_cb);
    // Pipelined: the part up to the last dispatch is queued as soon as it is recorded (nvn_frames.cpp). Not with the
    // profile or the zero-draw queries: they span the submit, which is then waited for whole.
    size_t split_after = gpu_profile_on() || zero_draws_on() ? SIZE_MAX : frame_plan_split(lists);
    uint64_t t0 = plat::monotonic_ns();
    const FrameTimes f0 = frame_times();
    Targets st;
    for (size_t k = 0; k < lists.size(); k++) {
        replay_list(g_cb, st, *lists[k], 0);
        // After the last list with a dispatch, unless nothing follows it (replay_list may have split inside a list).
        g_cb = k == split_after && k + 1 < lists.size() ? frame_split(g_cb) : frame_cb(g_cb);
    }
    draw_end_rendering(g_cb);
    const FrameTimes f1 = frame_times();  // the first part's submit and ring waits are not replay time
    g_timing.submits++;
    g_timing.replay_ns += plat::monotonic_ns() - t0 - (f1.encode_ns - f0.encode_ns) - (f1.fence_ns - f0.fence_ns);
    g_timing.upload_bytes += async ? frame_upload_used() : g_upload_used;
    gpu_profile_mark(g_cb, "end");
    if (async)  // the profile, zero-draw queries and snapshots read this submit's results right below: waited for
        frame_submit(VK_NULL_HANDLE, VK_NULL_HANDLE, draw_take_write_backs(),
                     gpu_profile_on() || zero_draws_on() || debug_snapshots_pending(), std::move(completions));
    else {
        end_and_wait(VK_NULL_HANDLE, VK_NULL_HANDLE);
    }
    draw_end_submit();  // asynchronous write-backs went with the submit; synchronous ones are applied here
    // The guest fence covers both the GPU and its writes to guest memory.
    if (!async) for (auto& fn : completions) fn();
    gpu_profile_collect(), zero_draws_collect();
    debug_dump_requested(draw_take_dump_request());  // reads back in a submit of its own, which it waits for
    const FrameTimes f2 = frame_times();
    g_timing.encode_ns += f2.encode_ns - f_in.encode_ns, g_timing.fence_ns += f2.fence_ns - f_in.fence_ns;
    g_timing.submit_ns += plat::monotonic_ns() - t_in;
}

void gpu_finish_fenced_submit() {
    std::lock_guard lock(g_gpu);
    // Unlike gpu_wait(gpu_serial()), this also waits in NX_PIPELINED. The
    // command-buffer fence must cover attachment work as well as write-backs.
    if (!g_ready) fatal("nvn: cannot complete a fenced submit without an initialized GPU");
    if (async_gpu()) frame_finish_fenced();
}

// The settings panel (platform_win.cpp, F1) over the presented frame: its pixels go to an image of their own when
// they change (a staging buffer, freed with the submit), and that image is blitted into the middle of the swapchain
// image, scaled down if the window is smaller. `dst` is in PRESENT_SRC and is left there.
namespace {
GpuImage g_panel;
uint64_t g_panel_uploaded = 0;
}  // namespace

static void settings_panel_record(VkCommandBuffer cb, GpuImage& dst) {
    uint32_t w = 0, h = 0;
    uint64_t version = 0;
    const uint8_t* px = plat::settings_panel(g_sc_extent.height, w, h, version);
    if (!px || !w || !h) return;
    const bool srgb = g_sc_format == VK_FORMAT_B8G8R8A8_SRGB || g_sc_format == VK_FORMAT_R8G8B8A8_SRGB;
    const VkFormat format = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    if (g_panel.image && (g_panel.width != w || g_panel.height != h || g_panel.format != format)) {
        p_vkDeviceWaitIdle(g_device);  // a resize or a new swapchain: rare, and earlier frames may still read it
        p_vkDestroyImage(g_device, g_panel.image, nullptr);
        p_vkFreeMemory(g_device, g_panel.memory, nullptr);
        g_panel = GpuImage{};
    }
    if (!g_panel.image) {
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_2D, ci.format = format, ci.extent = {w, h, 1};
        ci.mipLevels = ci.arrayLayers = 1, ci.samples = VK_SAMPLE_COUNT_1_BIT, ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (p_vkCreateImage(g_device, &ci, nullptr, &g_panel.image) != VK_SUCCESS) return;
        VkMemoryRequirements req{};
        p_vkGetImageMemoryRequirements(g_device, g_panel.image, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size, ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (p_vkAllocateMemory(g_device, &ai, nullptr, &g_panel.memory) != VK_SUCCESS) {
            p_vkDestroyImage(g_device, g_panel.image, nullptr);
            g_panel = GpuImage{};
            return;
        }
        p_vkBindImageMemory(g_device, g_panel.image, g_panel.memory, 0);
        g_panel.width = w, g_panel.height = h, g_panel.format = format;
        g_panel_uploaded = 0;
    }
    if (g_panel_uploaded != version) {
        const uint64_t bytes = uint64_t(w) * h * 4;
        Upload u = staging_alloc(bytes);
        if (!u.ptr) return;
        std::memcpy(u.ptr, px, bytes);
        transition(cb, g_panel, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy r{};
        r.bufferOffset = u.offset;
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        r.imageExtent = {w, h, 1};
        p_vkCmdCopyBufferToImage(cb, u.buffer, g_panel.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
        g_panel_uploaded = version;
    }
    transition(cb, g_panel, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    transition(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const float scale = std::min({1.0f, float(g_sc_extent.width) * 0.94f / float(w), float(g_sc_extent.height) * 0.94f / float(h)});
    const int32_t bw = int32_t(float(w) * scale), bh = int32_t(float(h) * scale);
    const int32_t x = (int32_t(g_sc_extent.width) - bw) / 2, y = (int32_t(g_sc_extent.height) - bh) / 2;
    VkImageBlit blit{};
    blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {int32_t(w), int32_t(h), 1};
    blit.dstOffsets[0] = {x, y, 0}, blit.dstOffsets[1] = {x + bw, y + bh, 1};
    p_vkCmdBlitImage(cb, g_panel.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.image,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    transition(cb, dst, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    static uint64_t shown_version = 0;
    if (shown_version != version)
        log("nvn/vk: settings panel %ux%u shown at %d,%d as %dx%d on %ux%u (picture %llu)", w, h, x, y, bw, bh,
            g_sc_extent.width, g_sc_extent.height, (unsigned long long)version), shown_version = version;
}

// Headless: the presented texture is only counted, logged and dumped (no swapchain). Nothing is submitted unless a
// frame dump is due: the empty submit was a Metal command buffer and a fence wait per frame.
namespace {
uint64_t g_presented_texture = 0;
// The game presents from two textures in turn (1600x900 R8G8B8A8_UNORM, alternating every present), and renders the
// next frame into the other one: the draws of a frame name the target that will be presented at the end of it, not the
// one presented before it. Keep a short ring so a draw into either is recognised (NX_TRACE_PRESENT).
uint64_t g_presented_recent[4] = {};
uint32_t g_presented_next = 0;
}  // namespace
uint64_t presented_texture() { return g_presented_texture; }
// Every texture the game has ever presented (a handful: it rotates a few window textures). Atomic, because the replay
// that asks may be on another thread than the present; the last-four ring above is a hint, this is the exact set.
namespace {
std::atomic<uint64_t> g_presented_ever[32] = {};
std::atomic<uint32_t> g_presented_ever_next{0};
}  // namespace
bool presented_recently(uint64_t texture) {
    if (!texture) return false;
    for (const std::atomic<uint64_t>& t : g_presented_ever)
        if (t.load(std::memory_order_relaxed) == texture) return true;
    for (uint64_t t : g_presented_recent)
        if (t == texture) return true;
    return false;
}
// NX_TRACE_PRESENT: what the screen shows. When the picture is black/white while the scene targets hold content, the
// answer is in the draws into this texture (nvn_draw_trace.cpp) and the images they sample (nvn_draw.cpp dumps both).
// Logged when the game switches presentation targets, so the handles in a run are readable afterwards.
void note_presented_texture(uint64_t texture) {
    static const bool trace = std::getenv("NX_TRACE_PRESENT") != nullptr;
    static std::set<uint64_t> seen;
    if (trace && texture != g_presented_texture && seen.insert(texture).second && seen.size() <= 16) {
        GpuImage* img = image_of(texture);
        log("nvn: presented texture %llx (%ux%u format %#x) at present %llu: rendered %d uploaded %d layout %d "
            "rendered_at %llu", (unsigned long long)texture, img ? img->width : 0, img ? img->height : 0,
            img ? img->nvn_format : 0, (unsigned long long)g_frame.load(), img ? int(img->rendered) : 0,
            img ? int(img->uploaded) : 0, img ? int(img->layout) : -1, img ? (unsigned long long)img->rendered_at : 0ull);
    }
    g_presented_texture = texture;
    g_presented_recent[g_presented_next++ & 3] = texture;
    for (std::atomic<uint64_t>& t : g_presented_ever) {
        uint64_t v = t.load(std::memory_order_relaxed);
        if (v == texture) break;
        if (!v) { t.store(texture, std::memory_order_relaxed); break; }
    }
}
void present_headless(uint64_t texture) {
    GpuImage* src = image_of(texture);  // made here if new, as before (the targets rendered into it share it, #44)
    note_presented_texture(texture);
    static uint64_t presents = 0;
    presents++;
    fps_overlay_frame();  // the on-screen frame rate (nvn_frame_overlay.cpp): headless frames count too
    const uint64_t window = draw_frame_stats::log_window(presents);
    if (presents % window == 0) draw_log_stats(window);
    if (presents % 60 == 0) log_timing(60), gpu_profile_log(60);
    if (presents % 300 == 0) zero_draws_log();
    // NX_PRESENT_TEST_ALL=1 (with NX_PRESENT_TEST): the present pass runs for every frame, and its submit is timed.
    static const bool every = std::getenv("NX_PRESENT_TEST_ALL") != nullptr;
    bool due = frame_dump_due(presents);
    if (!due && !every) return;
    static uint64_t timed_ns = 0, timed = 0;
    const uint64_t t0 = plat::monotonic_ns();
    if (src && src->layout != VK_IMAGE_LAYOUT_UNDEFINED && begin()) {
        GpuImage* shown = src;  // NX_PRESENT_TEST: what the present pass would put on a display (nvn_present.cpp)
        if (GpuImage* test = present_pass_test_target(); test && present_pass_record(g_cb, *src, nullptr, 0))
            test->layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, shown = test;
        transition(g_cb, *shown, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        bool dump = due && frame_dump_record(g_cb, *shown);
        if (async_gpu())
            frame_submit(VK_NULL_HANDLE, VK_NULL_HANDLE, {}, dump);  // a dumped frame is waited for
        else
            end_and_wait(VK_NULL_HANDLE, VK_NULL_HANDLE);
        if (dump) frame_dump_write(*shown, presents);
        if (every && !due && ++timed % 120 == 0) {
            log("nvn/vk: present test: pass + wait %.2f ms/frame over the last %llu", double(timed_ns) / 1e6 / 120, 120ull);
            timed_ns = 0;
        } else if (every && !due) {
            timed_ns += plat::monotonic_ns() - t0;
        }
    }
    debug_dump_textures(presents);
}

// NX_PRESENT_PROFILE=1: where a present's time goes (the whole of gpu_present is `present` in the frame stats).
// Lock wait, object drain, image graveyard, acquire, the submit, and the queue present itself.
struct PresentPhases {
    uint64_t lock_ns = 0, drain_ns = 0, free_ns = 0, acquire_ns = 0, submit_ns = 0, queue_ns = 0, n = 0;
    bool on = std::getenv("NX_PRESENT_PROFILE") != nullptr;
    ~PresentPhases() {
        if (!on || !n || n % 60) return;
        auto ms = [&](uint64_t v) { return double(v) / 1e6 / double(n); };
        log("nvn/vk: present phases over %llu: lock %.2f, drain %.2f, free %.2f, acquire %.2f, submit %.2f, queue %.2f ms",
            (unsigned long long)n, ms(lock_ns), ms(drain_ns), ms(free_ns), ms(acquire_ns), ms(submit_ns), ms(queue_ns));
        *this = PresentPhases{};
    }
};
PresentPhases g_present_phases;

void draw_frame_log(uint64_t recorded);  // nvn_draw.cpp; called under the GPU lock

void gpu_present(uint64_t texture, const int32_t* crop) {
    plat::perf_hint_thread();  // the thread that presents takes part in the frame (perf_hint.cpp)
    PresentPhases& ph = g_present_phases;
    const uint64_t t_lock_all = plat::monotonic_ns();
    uint64_t t_lock = ph.on ? t_lock_all : 0;
    std::lock_guard lk(g_gpu);
    if (ph.on) ph.lock_ns += plat::monotonic_ns() - t_lock;
    PresentTimer timer;
    uint64_t t_phase = ph.on ? plat::monotonic_ns() : 0;
    drain_objects();  // under the GPU lock, as in gpu_submit
    if (ph.on) ph.drain_ns += plat::monotonic_ns() - t_phase, t_phase = plat::monotonic_ns();
    if (!init()) return;
    if (draw_frame_stats::enabled()) draw_frame_log(draw_frame_stats::guest_sample());
    note_presented_texture(texture);  // NX_TRACE_PRESENT: what the screen shows (headless notes it again below)
    if (!async_gpu() || pipelined()) free_retired_images(async_gpu() ? frame_completed() : UINT64_MAX);
    if (ph.on) ph.free_ns += plat::monotonic_ns() - t_phase, ph.n++;
    if (plat::headless()) return present_headless(texture);
    if (plat::window_minimized()) return;
    if (plat::window_generation() != g_window_gen) {  // Android: a new window after home or the lock screen
        if (async_gpu()) frame_wait_all();
        p_vkDeviceWaitIdle(g_device);
        present_pass_release();
        if (g_swapchain) p_vkDestroySwapchainKHR(g_device, g_swapchain, nullptr);
        g_swapchain = VK_NULL_HANDLE;
        log("nvn/vk: new window: surface and swapchain re-made");
        if (!make_surface()) return;
    }
    if (!g_swapchain && !make_swapchain()) return;
    uint32_t index = 0;
    VkSemaphore acquired = async_gpu() ? frame_acquire_semaphore() : g_acquired;
    if (ph.on) t_phase = plat::monotonic_ns();
    VkResult r = p_vkAcquireNextImageKHR(g_device, g_swapchain, UINT64_MAX, acquired, VK_NULL_HANDLE, &index);
    if (ph.on) ph.acquire_ns += plat::monotonic_ns() - t_phase, t_phase = plat::monotonic_ns();
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        make_swapchain();
        return;
    }
    if (!check(r, "vkAcquireNextImageKHR") || !begin()) return;
    GpuImage* src = image_of(texture);
    GpuImage dst;
    dst.image = g_sc_images[index];
    dst.format = g_sc_format;
    if (src) {
        if (src->layout == VK_IMAGE_LAYOUT_UNDEFINED) {  // never rendered: dark grey, to tell it from a failure
            transition(g_cb, *src, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkClearColorValue v{{0.08f, 0.08f, 0.1f, 1.0f}};
            VkImageSubresourceRange rr{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            p_vkCmdClearColorImage(g_cb, src->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &rr);
        }
    }
    bool drawn = src && present_pass_record(g_cb, *src, crop, index);  // nvn_present.cpp: leaves dst PRESENT_SRC
    if (src && !drawn) transition(g_cb, *src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    if (!drawn) transition(g_cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    if (src && !drawn) {
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {int32_t(src->width), int32_t(src->height), 1};
        if (crop && crop[2] > 0 && crop[3] > 0 && crop[0] + crop[2] <= int32_t(src->width) &&
            crop[1] + crop[3] <= int32_t(src->height))  // nvnWindowSetCrop
            blit.srcOffsets[0] = {crop[0], crop[1], 0}, blit.srcOffsets[1] = {crop[0] + crop[2], crop[1] + crop[3], 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstOffsets[1] = {int32_t(g_sc_extent.width), int32_t(g_sc_extent.height), 1};
        p_vkCmdBlitImage(g_cb, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    }
    if (!drawn) transition(g_cb, dst, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    dst.layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;  // either way (the present pass leaves it so too)
    settings_panel_record(g_cb, dst);
    static uint64_t presents = 0;
    presents++;
    const uint64_t window = draw_frame_stats::log_window(presents);
    if (presents % window == 0) draw_log_stats(window);
    if (presents % 60 == 0) log_timing(60), gpu_profile_log(60);
    if (presents % 300 == 0) zero_draws_log();
    frame_sig_flush(presents);
    const bool due = frame_dump_due(presents);
    bool dump = src && due;
    if (dump) transition(g_cb, *src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);  // the present pass left it sampled
    dump = dump && frame_dump_record(g_cb, *src);
    VkSemaphore rendered = async_gpu() ? frame_rendered_semaphore(index) : g_rendered;
    if (async_gpu())  // the blit's fence is not waited for (vkQueuePresentKHR waits on `rendered`); a dumped frame is
        frame_submit(acquired, rendered, {}, dump);
    else
        end_and_wait(acquired, rendered);
    if (ph.on) ph.submit_ns += plat::monotonic_ns() - t_phase, t_phase = plat::monotonic_ns();
    if (dump) frame_dump_write(*src, presents);
    if (due) debug_dump_textures(presents);
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &rendered;
    pi.swapchainCount = 1;
    pi.pSwapchains = &g_swapchain;
    pi.pImageIndices = &index;
    const uint64_t t_queue = plat::monotonic_ns();
    r = p_vkQueuePresentKHR(g_queue, &pi);
    if (ph.on) ph.queue_ns += plat::monotonic_ns() - t_phase;
    {  // A present more than 50 ms after the last: where this one's time went (lock .. queue), or else that it came late.
        static uint64_t last_end = 0;
        static int hitch_lines = 0;
        const uint64_t end = plat::monotonic_ns();
        if (last_end && end - last_end > 50000000ull && hitch_lines < 200) {
            hitch_lines++;
            log("present hitch: %.1f ms since the last present at present %llu; in this one %.1f ms (vkQueuePresent %.1f)",
                double(end - last_end) / 1e6, (unsigned long long)presents, double(end - t_lock_all) / 1e6,
                double(end - t_queue) / 1e6);
        }
        last_end = end;
    }
    plat::frame_presented();  // cores, CPU time, present intervals (frame_stats.cpp)
    fps_overlay_frame();      // the on-screen frame rate (nvn_frame_overlay.cpp)
    // SUBOPTIMAL re-makes the swapchain only when the surface size changed: Android returns it for every present when
    // the swapchain's transform is not the display's (an Adreno 740 did for Hades), a rebuild per frame.
    bool remake = r == VK_ERROR_OUT_OF_DATE_KHR;
    if (r == VK_SUBOPTIMAL_KHR) {  // compared with what the swapchain was made from (its extent may be turned)
        VkSurfaceCapabilitiesKHR caps{};
        p_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_physical, g_surface, &caps);
        remake = caps.currentExtent.width != g_sc_current.width || caps.currentExtent.height != g_sc_current.height ||
                 (present_pass_wanted() && !present_pass_failed() && caps.currentTransform != g_sc_transform);
    }
    if (remake) make_swapchain();
}

}  // namespace nx::nvn
