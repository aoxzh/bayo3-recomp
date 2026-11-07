// Isolated EXT_external_memory_host residency probe; no game data is read.
// Build: clang++ -std=c++17 -O2 -Wall -Wextra -Iprivate/toolchains/Vulkan-Headers-1.4.357/include
//        tools/b3_host_import_probe.cpp -lvulkan -o private/bin/b3-host-import-probe
#include <vulkan/vulkan.h>
#include <sys/mman.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

static void check(VkResult r, const char* where) {
    if (r != VK_SUCCESS) throw std::runtime_error(std::string(where) + ": VkResult " + std::to_string(r));
}
static double now_ms() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}
struct Allocation {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* host = MAP_FAILED;
    size_t bytes = 0;
};
struct Probe {
    VkInstance instance = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memory{};
    PFN_vkGetMemoryHostPointerPropertiesEXT host_properties = nullptr;
    Allocation destination;
    Allocation readback;
    std::vector<Allocation> imports;
    size_t alignment = 4096;
    void release(Allocation& a) {
        if (a.buffer) vkDestroyBuffer(device, a.buffer, nullptr);
        if (a.memory) vkFreeMemory(device, a.memory, nullptr);
        if (a.host != MAP_FAILED) munmap(a.host, a.bytes);
        a = {};
    }
    void clear_imports() {
        for (auto& a : imports) release(a);
        imports.clear();
    }
    ~Probe() {
        if (device) {
            vkDeviceWaitIdle(device);
            clear_imports();
            if (pool) vkDestroyCommandPool(device, pool, nullptr);
            release(destination);
            release(readback);
            vkDestroyDevice(device, nullptr);
        }
        if (instance) vkDestroyInstance(instance, nullptr);
    }
    uint32_t type(uint32_t bits, VkMemoryPropertyFlags flags) {
        for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & flags) == flags) return i;
        throw std::runtime_error("No compatible memory type");
    }
    void init() {
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "b3-host-import-probe";
        app.apiVersion = VK_API_VERSION_1_2;
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.pApplicationInfo = &app;
        check(vkCreateInstance(&ici, nullptr, &instance), "vkCreateInstance");
        uint32_t count = 0;
        check(vkEnumeratePhysicalDevices(instance, &count, nullptr), "enumerate devices");
        std::vector<VkPhysicalDevice> devices(count);
        check(vkEnumeratePhysicalDevices(instance, &count, devices.data()), "enumerate devices");
        if (devices.empty()) throw std::runtime_error("No Vulkan device");
        physical = devices.front();
        for (auto d : devices) {
            VkPhysicalDeviceProperties prop{};
            vkGetPhysicalDeviceProperties(d, &prop);
            if (prop.vendorID == 0x1002) { physical = d; break; }
        }
        VkPhysicalDeviceExternalMemoryHostPropertiesEXT host{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
        VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        host.pNext = &driver;
        VkPhysicalDeviceProperties2 prop{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        prop.pNext = &host;
        vkGetPhysicalDeviceProperties2(physical, &prop);
        if (driver.driverID != VK_DRIVER_ID_MESA_RADV)
            throw std::runtime_error("Probe requires the Mesa RADV driver");
        alignment = std::max<size_t>(4096, host.minImportedHostPointerAlignment);
        if ((alignment & (alignment - 1)) || alignment > 128 * 1024)
            throw std::runtime_error("Import alignment exceeds the probe's 80MiB mapping budget");
        VkPhysicalDeviceExternalBufferInfo ebi{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO};
        ebi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        ebi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
        VkExternalBufferProperties ebp{VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
        vkGetPhysicalDeviceExternalBufferProperties(physical, &ebi, &ebp);
        if (!(ebp.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) ||
            !(ebp.externalMemoryProperties.compatibleHandleTypes & ebi.handleType))
            throw std::runtime_error("Host import unsupported for requested buffer usage");
        vkGetPhysicalDeviceMemoryProperties(physical, &memory);
        uint32_t ec = 0;
        check(vkEnumerateDeviceExtensionProperties(physical, nullptr, &ec, nullptr), "enumerate extensions");
        std::vector<VkExtensionProperties> extensions(ec);
        check(vkEnumerateDeviceExtensionProperties(physical, nullptr, &ec, extensions.data()), "enumerate extensions");
        if (std::none_of(extensions.begin(), extensions.end(), [](auto& e) {
                return !std::strcmp(e.extensionName, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
            })) throw std::runtime_error("VK_EXT_external_memory_host unsupported");
        uint32_t qc = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &qc, nullptr);
        std::vector<VkQueueFamilyProperties> families(qc);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &qc, families.data());
        uint32_t family = 0;
        while (family < qc && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) ++family;
        if (family == qc) throw std::runtime_error("No graphics queue");
        float priority = 1;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = family; qci.queueCount = 1; qci.pQueuePriorities = &priority;
        const char* ext = VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME;
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = 1; dci.ppEnabledExtensionNames = &ext;
        check(vkCreateDevice(physical, &dci, nullptr, &device), "vkCreateDevice");
        vkGetDeviceQueue(device, family, 0, &queue);
        host_properties = reinterpret_cast<PFN_vkGetMemoryHostPointerPropertiesEXT>(
            vkGetDeviceProcAddr(device, "vkGetMemoryHostPointerPropertiesEXT"));
        if (!host_properties) throw std::runtime_error("Missing host pointer properties function");
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = 4096; bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        check(vkCreateBuffer(device, &bi, nullptr, &destination.buffer), "destination buffer");
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(device, destination.buffer, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size; ai.memoryTypeIndex = type(req.memoryTypeBits, 0);
        check(vkAllocateMemory(device, &ai, nullptr, &destination.memory), "destination memory");
        check(vkBindBufferMemory(device, destination.buffer, destination.memory, 0), "destination bind");
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.queueFamilyIndex = family;
        check(vkCreateCommandPool(device, &pci, nullptr, &pool), "command pool");
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
        check(vkAllocateCommandBuffers(device, &cai, &command), "command buffer");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        check(vkBeginCommandBuffer(command, &begin), "begin command");
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 1, &barrier, 0, nullptr, 0, nullptr);
        vkCmdFillBuffer(command, destination.buffer, 0, 4, 0x12345678);
        check(vkEndCommandBuffer(command), "end command");
        std::fprintf(stderr, "device=%s driver=%u alignment=%zu\n",
                     prop.properties.deviceName, prop.properties.driverVersion, alignment);
    }
    void add_import(size_t requested_bytes) {
        imports.emplace_back();
        auto& a = imports.back();
        a.bytes = (requested_bytes + alignment - 1) & ~(alignment - 1);
        // Over-map and trim to satisfy imported pointer alignment above the OS page size.
        size_t total = a.bytes + alignment;
        void* raw = mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (raw == MAP_FAILED) throw std::runtime_error("mmap failed");
        uintptr_t start = reinterpret_cast<uintptr_t>(raw);
        uintptr_t aligned = (start + alignment - 1) & ~(alignment - 1);
        size_t prefix = aligned - start, suffix = total - prefix - a.bytes;
        if (prefix) munmap(raw, prefix);
        if (suffix) munmap(reinterpret_cast<void*>(aligned + a.bytes), suffix);
        a.host = reinterpret_cast<void*>(aligned);
        std::memset(a.host, 0x5a, a.bytes);
        VkExternalMemoryBufferCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
        external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.pNext = &external; bi.size = a.bytes;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        check(vkCreateBuffer(device, &bi, nullptr, &a.buffer), "import buffer");
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(device, a.buffer, &req);
        if (req.size > a.bytes) throw std::runtime_error("Import memory requirement exceeds mapping");
        VkMemoryHostPointerPropertiesEXT hp{VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
        check(host_properties(device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, a.host, &hp), "host properties");
        VkImportMemoryHostPointerInfoEXT imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT};
        imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT; imp.pHostPointer = a.host;
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.pNext = &imp; ai.allocationSize = req.size;
        ai.memoryTypeIndex = type(req.memoryTypeBits & hp.memoryTypeBits,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        check(vkAllocateMemory(device, &ai, nullptr, &a.memory), "import memory");
        check(vkBindBufferMemory(device, a.buffer, a.memory, 0), "import bind");
    }
    void verify_reads() {
        constexpr unsigned count = 640;
        for (unsigned i = 0; i < count; ++i) add_import(4096);
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = 4096; bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        check(vkCreateBuffer(device, &bi, nullptr, &readback.buffer), "readback buffer");
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(device, readback.buffer, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = type(req.memoryTypeBits,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        check(vkAllocateMemory(device, &ai, nullptr, &readback.memory), "readback memory");
        check(vkBindBufferMemory(device, readback.buffer, readback.memory, 0), "readback bind");
        check(vkResetCommandPool(device, pool, 0), "verification reset pool");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        check(vkBeginCommandBuffer(command, &begin), "verification begin");
        VkMemoryBarrier before{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        before.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        before.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 1, &before, 0, nullptr, 0, nullptr);
        for (unsigned i = 0; i < count; ++i) {
            VkBufferCopy copy{0, sizeof(uint32_t) * i, sizeof(uint32_t)};
            vkCmdCopyBuffer(command, imports[i].buffer, readback.buffer, 1, &copy);
        }
        VkMemoryBarrier after{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        after.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; after.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                             0, 1, &after, 0, nullptr, 0, nullptr);
        check(vkEndCommandBuffer(command), "verification end");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
        for (unsigned pass = 0; pass < 3; ++pass) {
            // Prior pass completed before either CPU update or command-buffer reuse.
            for (unsigned i = 0; i < count; ++i) {
                uint32_t value = 0x12340000u + pass * count + i;
                std::memcpy(imports[i].host, &value, sizeof(value));
            }
            check(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE), "verification submit");
            check(vkQueueWaitIdle(queue), "verification wait");
            void* mapped = nullptr;
            check(vkMapMemory(device, readback.memory, 0, VK_WHOLE_SIZE, 0, &mapped), "verification map");
            bool valid = true;
            for (unsigned i = 0; i < count; ++i) {
                uint32_t observed = 0;
                std::memcpy(&observed, static_cast<const char*>(mapped) + sizeof(uint32_t) * i, sizeof(observed));
                valid = valid && observed == 0x12340000u + pass * count + i;
            }
            vkUnmapMemory(device, readback.memory);
            if (!valid) throw std::runtime_error("Imported-memory GPU readback mismatch");
        }
        std::puts("{\"verified_imports\":640,\"passes\":3,\"cpu_updates_observed\":true}");
    }
    static void stats(const char* name, std::vector<double> values) {
        std::sort(values.begin(), values.end());
        size_t n = values.size();
        double median = (values[(n-1)/2] + values[n/2]) / 2;
        std::printf("\"%s_ms\":{\"median\":%.6f,\"p95\":%.6f,\"max\":%.6f,\"mean\":%.6f}",
                    name, median, values[(95*n+99)/100-1], values.back(),
                    std::accumulate(values.begin(), values.end(), 0.0)/n);
    }
    void run(unsigned samples, size_t requested_bytes) {
        std::fprintf(stderr, "mode=timing; imports are not referenced by command buffer\n");
        unsigned phase = 0;
        for (unsigned count : {0u, 64u, 256u, 640u, 0u}) {
            check(vkQueueWaitIdle(queue), "phase wait");
            clear_imports();
            for (unsigned i = 0; i < count; ++i) add_import(requested_bytes);
            std::fprintf(stderr, "phase=%u imports=%u begin\n", phase, count);
            VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
            std::vector<double> submits, waits;
            for (unsigned i = 0; i < samples + 10; ++i) {
                double t0 = now_ms();
                check(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE), "queue submit");
                double t1 = now_ms();
                check(vkQueueWaitIdle(queue), "queue wait");
                double t2 = now_ms();
                if (i >= 10) { submits.push_back(t1-t0); waits.push_back(t2-t1); }
            }
            std::printf("{\"phase\":%u,\"imports\":%u,\"bytes\":%zu,\"referenced_imports\":0,\"samples\":%u,",
                        phase++, count, imports.empty() ? 0 : count * imports.front().bytes, samples);
            stats("submit", submits); std::printf(","); stats("wait", waits); std::puts("}");
            std::fflush(stdout);
        }
    }
};
int main(int argc, char** argv) {
    try {
        unsigned samples = 100;
        size_t requested_bytes = 128 * 1024;
        if (argc == 2 && !std::strcmp(argv[1], "--verify-reads")) {
            Probe probe;
            probe.init(); probe.verify_reads();
            return 0;
        }
        if (argc == 2 && !std::strcmp(argv[1], "--help")) {
            std::puts("b3-host-import-probe [--samples N] [--bytes-per-import N]\nb3-host-import-probe --verify-reads (640 referenced imports, 3 CPU-update/readback passes)\nFixed 4-byte fill; 0,64,256,640,0 unreferenced host imports.\nDefault 128KiB per import; accepts 4096..131072, rounded to device alignment.\n10 warmups per phase; submit and queue-idle wall times measured separately.");
            return 0;
        }
        for (int i = 1; i < argc; i += 2) {
            if (i + 1 >= argc) throw std::runtime_error("Missing option value; use --help");
            std::string s = argv[i+1]; size_t end = 0;
            if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos)
                throw std::runtime_error("Option values must be positive decimal integers");
            unsigned long n = std::stoul(s, &end);
            if (end != s.size()) throw std::runtime_error("Invalid integer");
            if (!std::strcmp(argv[i], "--samples")) {
                if (n < 1 || n > 100000) throw std::runtime_error("samples must be 1..100000");
                samples = static_cast<unsigned>(n);
            } else if (!std::strcmp(argv[i], "--bytes-per-import")) {
                if (n < 4096 || n > 131072) throw std::runtime_error("bytes-per-import must be 4096..131072");
                requested_bytes = n;
            } else throw std::runtime_error("Unknown option; use --help");
        }
        Probe probe;
        probe.init(); probe.run(samples, requested_bytes);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "probe error: %s\n", e.what()); return 1;
    }
}
