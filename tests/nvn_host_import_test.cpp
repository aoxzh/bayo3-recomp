// Production host_import_upload exercised against deterministic Vulkan callbacks.
#include "../runtime/src/nvn/nvn_vk.cpp"
#include <array>
#include <stdexcept>

using namespace nx;
using namespace nx::nvn;

namespace test {
constexpr VkMemoryPropertyFlags kVisible = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
constexpr VkMemoryPropertyFlags kCoherent = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
constexpr VkMemoryPropertyFlags kLocal = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
constexpr size_t kFixtureAlignment = 65536;
alignas(kFixtureAlignment) std::array<uint8_t, 8 * kFixtureAlignment> guest{};
struct State {
    uint32_t req_bits = 0, host_bits = 0;
    uint32_t selected = UINT32_MAX;
    uint64_t buffer_size = 0, req_size = 0, allocation_size = 0;
    const void* last_pointer = nullptr;
    VkExternalMemoryFeatureFlags features = VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT;
    VkExternalMemoryHandleTypeFlags compatible = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    int queries = 0, creates = 0, destroys = 0, properties = 0, allocates = 0, binds = 0, frees = 0;
    bool create_fail = false, properties_fail = false, allocate_fail = false, bind_fail = false;
} state;
template <class T> T handle(uintptr_t n) { return reinterpret_cast<T>(n); }
void require(bool value, const char* what) { if (!value) throw std::runtime_error(what); }
VKAPI_ATTR void VKAPI_CALL external_properties(VkPhysicalDevice physical, const VkPhysicalDeviceExternalBufferInfo* info,
                                                 VkExternalBufferProperties* props) {
    ++state.queries;
    require(physical == handle<VkPhysicalDevice>(2) && info &&
            info->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO && !info->pNext &&
            info->flags == 0 && info->usage == kUploadUsage &&
            info->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT && props &&
            props->sType == VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES,
            "unexpected external buffer properties query");
    props->externalMemoryProperties.externalMemoryFeatures = state.features;
    props->externalMemoryProperties.compatibleHandleTypes = state.compatible;
}
VKAPI_ATTR VkResult VKAPI_CALL create_buffer(VkDevice, const VkBufferCreateInfo* info,
                                               const VkAllocationCallbacks*, VkBuffer* out) {
    ++state.creates;
    require(info && info->size && info->usage == kUploadUsage, "unexpected buffer request");
    auto* external = static_cast<const VkExternalMemoryBufferCreateInfo*>(info->pNext);
    require(external && external->sType == VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO && !external->pNext &&
            external->handleTypes == VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
            "buffer was not created for HOST_ALLOCATION import");
    state.buffer_size = info->size;
    if (state.create_fail) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    *out = handle<VkBuffer>(uintptr_t(0x1000 + state.creates));
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL destroy_buffer(VkDevice, VkBuffer b, const VkAllocationCallbacks*) {
    require(b != VK_NULL_HANDLE, "destroyed null buffer"); ++state.destroys;
}
VKAPI_ATTR void VKAPI_CALL buffer_requirements(VkDevice, VkBuffer, VkMemoryRequirements* req) {
    req->size = state.req_size ? state.req_size : state.buffer_size;
    req->alignment = 256; req->memoryTypeBits = state.req_bits;
}
VKAPI_ATTR VkResult VKAPI_CALL host_properties(VkDevice, VkExternalMemoryHandleTypeFlagBits type,
                                                const void* pointer, VkMemoryHostPointerPropertiesEXT* props) {
    ++state.properties;
    const uintptr_t begin = reinterpret_cast<uintptr_t>(guest.data());
    const uintptr_t at = reinterpret_cast<uintptr_t>(pointer);
    require(type == VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT && at >= begin &&
            at - begin <= guest.size() && state.buffer_size <= guest.size() - (at - begin) &&
            at % nx::nvn::host_import_alignment() == 0,
            "unexpected host pointer query");
    state.last_pointer = pointer;
    if (state.properties_fail) return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    props->memoryTypeBits = state.host_bits;
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL allocate(VkDevice, const VkMemoryAllocateInfo* info,
                                         const VkAllocationCallbacks*, VkDeviceMemory* out) {
    ++state.allocates;
    state.selected = info->memoryTypeIndex;
    state.allocation_size = info->allocationSize;
    require(info->allocationSize % nx::nvn::g_host_align == 0, "import allocation size is not host-pointer aligned");
    require(info->allocationSize >= (state.req_size ? state.req_size : state.buffer_size),
            "allocation is smaller than buffer memory requirements");
    require(info->allocationSize == state.buffer_size && info->memoryTypeIndex < 32,
            "unexpected import allocation");
    require((state.req_bits & state.host_bits & (1u << info->memoryTypeIndex)) != 0,
            "selected memory type is not compatible with both masks");
    auto* host = static_cast<const VkImportMemoryHostPointerInfoEXT*>(info->pNext);
    require(host && host->sType == VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT &&
            host->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT &&
            host->pHostPointer == state.last_pointer, "invalid import request");
    if (state.allocate_fail) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    *out = handle<VkDeviceMemory>(0x2000);
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL bind(VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize offset) {
    ++state.binds; require(offset == 0, "unexpected bind offset");
    return state.bind_fail ? VK_ERROR_OUT_OF_DEVICE_MEMORY : VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL free_memory(VkDevice, VkDeviceMemory m, const VkAllocationCallbacks*) {
    require(m != VK_NULL_HANDLE, "freed null memory"); ++state.frees;
}
void reset(std::initializer_list<VkMemoryPropertyFlags> types, uint32_t req, uint32_t host) {
    // These are fake handles, and no GPU work is submitted in this test.
    nx::nvn::g_guest_imports.clear(); nx::nvn::g_guest_import_bytes = 0;
    nx::nvn::g_memory = {};
    nx::nvn::g_memory.memoryTypeCount = uint32_t(types.size());
    uint32_t i = 0; for (VkMemoryPropertyFlags flags : types) nx::nvn::g_memory.memoryTypes[i++].propertyFlags = flags;
    nx::nvn::g_host_import = true; nx::nvn::g_host_align = 4096;
    nx::nvn::g_device = handle<VkDevice>(1); nx::nvn::g_physical = handle<VkPhysicalDevice>(2);
    nx::vk::p_vkGetPhysicalDeviceExternalBufferProperties = external_properties;
    state = {}; state.req_bits = req; state.host_bits = host;
}
Upload import(uint64_t offset = 0, uint64_t size = 256) {
    return host_import_upload(guest.data() + offset, size, size);
}
void rejected() {
    Upload u = import();
    require(u.buffer == VK_NULL_HANDLE && !u.direct && !u.ptr, "incompatible memory did not fall back");
    require(state.creates == 1 && state.destroys == 1 && state.allocates == 0 && state.binds == 0,
            "incompatible buffer was not destroyed before allocation");
    require(nx::nvn::g_guest_imports.empty(), "failed import was cached");
}
void test_allocation_size() {
    const auto coherent = kVisible | kCoherent;
    reset({coherent}, 1, 1);
    const uint64_t align = nx::nvn::host_import_alignment();
    require(align >= 4096 && align <= kFixtureAlignment &&
            reinterpret_cast<uintptr_t>(guest.data()) % align == 0,
            "fixture does not support the host pointer alignment");
    nx::nvn::g_host_align = align;
    const uint64_t cap = 2 * align;

    // At 4 KiB alignment this simulates req.size=6144 and cap=8192. The buffer
    // requirement is intentionally not a multiple of the host-pointer import alignment.
    state.req_size = align + align / 2;
    require(state.req_size % align != 0, "test requirement must be unaligned for host import");
    Upload u = import(0, cap);
    require(u.buffer && u.direct && state.buffer_size == cap && state.allocation_size == cap &&
            state.creates == 1 && state.properties == 1 && state.allocates == 1 && state.binds == 1 &&
            state.destroys == 0 && state.frees == 0,
            "smaller unaligned memory requirement did not allocate the aligned capacity");
    Upload hit = import(0, align);
    require(hit.buffer == u.buffer && hit.direct && state.creates == 1 && state.allocates == 1,
            "aligned allocation was not reused for a covered cache hit");

    reset({coherent}, 1, 1);
    nx::nvn::g_host_align = align;
    state.req_size = cap + 256;
    u = import(0, cap);
    require(!u.buffer && !u.direct && !u.ptr && state.buffer_size == cap &&
            state.creates == 1 && state.destroys == 1 && state.properties == 0 &&
            state.allocates == 0 && state.binds == 0 && state.frees == 0 &&
            nx::nvn::g_guest_imports.empty() && nx::nvn::g_guest_import_bytes == 0,
            "oversized buffer requirement did not fall back and destroy the buffer");
    state.req_size = 0;
    u = import(0, cap);
    require(u.buffer && u.direct && state.creates == 2 && state.destroys == 1 &&
            state.allocates == 1 && state.binds == 1 && state.allocation_size == cap,
            "oversized requirement left a cached import or prevented a valid retry");
}

void test_overlap_and_overflow() {
    const auto coherent = kVisible | kCoherent;
    reset({coherent}, 1, 1);
    const uint64_t page = nx::nvn::host_import_alignment();
    require(page && page <= kFixtureAlignment && reinterpret_cast<uintptr_t>(guest.data()) % page == 0,
            "fixture does not support the host page alignment");

    // Two import intervals may touch, but different starts may never cover the same host page.
    require(import(page, page).buffer, "middle-page import failed");
    require(import(0, page).buffer, "preceding adjacent page incorrectly rejected");
    require(import(2 * page, page).buffer, "following adjacent page incorrectly rejected");
    require(state.creates == 3, "adjacent ranges did not create three imports");

    reset({coherent}, 1, 1);
    require(import(0, 2 * page).buffer, "initial overlapping fixture failed");
    require(!import(page, page).buffer && state.creates == 1, "contained overlap was imported twice");
    require(!import(page, 2 * page).buffer && state.creates == 1, "overlap with previous import was not rejected");
    require(import(0, page).buffer && state.creates == 1, "covered same-start import should be reused");
    require(!import(0, 3 * page).buffer && state.creates == 1, "same-start growing import must fall back");

    reset({coherent}, 1, 1);
    require(import(page, 2 * page).buffer, "later overlapping fixture failed");
    require(!import(0, 2 * page).buffer && state.creates == 1, "overlap with next import was not rejected");
    require(!import(0, 4 * page).buffer && state.creates == 1, "enclosing import overlap was not rejected");

    // A driver requirement smaller than a host page must still coalesce sub-page imports onto one cached VkMemory.
    reset({coherent}, 1, 1);
    nx::nvn::g_host_align = 16;
    require(nx::nvn::host_import_alignment() >= page, "driver alignment ignored the physical page size");
    Upload first = import(4, 4), second = import(20, 4);
    require(first.buffer && first.buffer == second.buffer && state.creates == 1,
            "sub-page imports duplicated a physical host page");

    // A driver alignment larger than the OS page must not import whole neighboring pages the guest span never touches.
    reset({coherent}, 1, 1);
    nx::nvn::g_host_align = 4 * page;
    require(!import(0, page).buffer && state.creates == 0, "import included trailing guest-unowned host pages");
    require(!import(page, 3 * page).buffer && state.creates == 0, "import included leading guest-unowned host pages");
    require(!import(0, 3 * page).buffer && state.creates == 0, "large alignment rounded across an unowned host page");
    require(import(4, 4 * page - 4).buffer && state.creates == 1, "fully covered aligned range was rejected");

    reset({coherent}, 1, 1);
    auto no_import = [](const void* ptr, uint64_t bytes, uint64_t guest_bytes, const char* message) {
        Upload result = host_import_upload(ptr, bytes, guest_bytes);
        require(!result.buffer && !result.direct && state.creates == 0, message);
    };
    no_import(guest.data(), UINT64_MAX, UINT64_MAX, "wrapped requested size was accepted");
    no_import(guest.data(), 4, UINT64_MAX, "wrapped guest extent was accepted");
    no_import(guest.data(), 4, 0, "empty guest extent was accepted");
    const uintptr_t near_end = UINTPTR_MAX & ~uintptr_t(3);
    no_import(reinterpret_cast<const void*>(near_end), 8, 8, "address plus size wrapped");
    no_import(reinterpret_cast<const void*>(near_end), 1, 1, "rounded page endpoint wrapped");
    nx::nvn::g_host_align = 3;
    no_import(guest.data(), 4, 4, "invalid non-power-of-two alignment was accepted");
    nx::nvn::g_host_align = 0;
    no_import(guest.data(), 4, 4, "zero alignment was accepted");
    nx::nvn::g_host_align = 4096;
    nx::nvn::g_guest_import_bytes = UINT64_MAX;
    no_import(guest.data(), 4, 4, "import budget arithmetic wrapped");
}
void run() {
    reset({kVisible | kCoherent}, 1, 1);
    require(nx::nvn::checked_host_import_alignment(4096, 0) == 0 &&
            nx::nvn::checked_host_import_alignment(4096, 3) == 0 &&
            nx::nvn::checked_host_import_alignment(0, 4096) == 0 &&
            nx::nvn::checked_host_import_alignment(3, 4096) == 0 &&
            nx::nvn::checked_host_import_alignment(16, 4096) == 4096 &&
            nx::nvn::checked_host_import_alignment(16384, 4096) == 16384,
            "host page/driver alignment failure or maximum was handled incorrectly");
    require(!nx::nvn::host_import_buffer_supported(VK_API_VERSION_1_0) && state.queries == 0,
            "Vulkan 1.0 incorrectly used the core 1.1 capability query");
    nx::vk::p_vkGetPhysicalDeviceExternalBufferProperties = nullptr;
    nx::nvn::g_host_import = nx::nvn::host_import_buffer_supported(VK_API_VERSION_1_1);
    require(!nx::nvn::g_host_import && !import().buffer && state.queries == 0 && state.creates == 0,
            "missing query entry point did not fall back");
    nx::vk::p_vkGetPhysicalDeviceExternalBufferProperties = external_properties;
    state.features = 0;
    nx::nvn::g_host_import = nx::nvn::host_import_buffer_supported(VK_API_VERSION_1_1);
    require(!nx::nvn::g_host_import && !import().buffer && state.creates == 0,
            "non-importable external buffer did not fall back");
    state.features = VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT;
    state.compatible = 0;
    require(!nx::nvn::host_import_buffer_supported(VK_API_VERSION_1_1), "incompatible host handle type accepted");
    state.compatible = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    state.features |= VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT;
    require(!nx::nvn::host_import_buffer_supported(VK_API_VERSION_1_1), "dedicated-only host import accepted");
    state.features = VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT;
    require(nx::nvn::host_import_buffer_supported(VK_API_VERSION_1_1), "supported host import rejected");
    reset({kVisible}, 1, 1); rejected();  // host-visible but non-coherent
    reset({kLocal}, 1, 1); rejected();    // non-host-visible
    reset({kVisible | kCoherent, kVisible | kCoherent}, 1, 2); rejected(); // empty intersection
    // A coherent type eligible for only one mask cannot win. Type 3 is the first valid intersection.
    reset({kVisible, kVisible | kCoherent, kVisible | kCoherent, kVisible | kCoherent}, 0xb, 0xd);
    Upload first = import(128);
    require(first.buffer && first.direct && first.offset == 128 && first.ptr == guest.data() + 128,
            "compatible coherent import failed");
    require(state.selected == 3 && state.creates == 1 && state.allocates == 1 && state.binds == 1 &&
            state.destroys == 0, "wrong coherent type or import sequence");
    Upload hit = import(192);
    require(hit.buffer == first.buffer && hit.offset == 192 && hit.ptr == guest.data() + 192 && hit.direct &&
            state.creates == 1 && state.properties == 1 && state.allocates == 1 && state.binds == 1,
            "same-range cache hit did not reuse the import");
    Upload larger = import(0, 2 * nx::nvn::host_import_alignment());
    require(!larger.buffer && state.creates == 1 && state.allocates == 1, "growing a cached range must fall back");
    reset({kVisible, kVisible | kCoherent}, 3, 3);
    Upload selected = import();
    require(selected.buffer && selected.direct && state.selected == 1, "later coherent type was not selected");
    reset({kVisible | kCoherent}, 1, 1); state.create_fail = true;
    require(!import().buffer && state.creates == 1 && state.destroys == 0 && state.properties == 0,
            "buffer creation failure did not fall back");
    reset({kVisible | kCoherent}, 1, 1); state.properties_fail = true;
    require(!import().buffer && state.destroys == 1 && state.allocates == 0, "property failure did not fall back");
    reset({kVisible | kCoherent}, 1, 1); state.allocate_fail = true;
    require(!import().buffer && state.destroys == 1 && state.frees == 0 && state.binds == 0,
            "allocation failure cleanup");
    reset({kVisible | kCoherent}, 1, 1); state.bind_fail = true;
    require(!import().buffer && state.destroys == 1 && state.frees == 1 && state.binds == 1,
            "bind failure cleanup");
    test_allocation_size();
    test_overlap_and_overflow();
}
} // namespace test

namespace nx::vk {
PFN_vkGetPhysicalDeviceExternalBufferProperties p_vkGetPhysicalDeviceExternalBufferProperties = test::external_properties;
PFN_vkCreateBuffer p_vkCreateBuffer = test::create_buffer;
PFN_vkDestroyBuffer p_vkDestroyBuffer = test::destroy_buffer;
PFN_vkGetBufferMemoryRequirements p_vkGetBufferMemoryRequirements = test::buffer_requirements;
PFN_vkGetMemoryHostPointerPropertiesEXT p_vkGetMemoryHostPointerPropertiesEXT = test::host_properties;
PFN_vkAllocateMemory p_vkAllocateMemory = test::allocate;
PFN_vkBindBufferMemory p_vkBindBufferMemory = test::bind;
PFN_vkFreeMemory p_vkFreeMemory = test::free_memory;
}
namespace nx {
void log(const char*, ...) {}
}
namespace nx::plat {
uint64_t monotonic_ns() { return 0; }
}
int main() {
    try { test::run(); std::puts("host import production-path tests passed"); }
    catch (const std::exception& e) { std::fprintf(stderr, "host import test failed: %s\n", e.what()); return 1; }
}
