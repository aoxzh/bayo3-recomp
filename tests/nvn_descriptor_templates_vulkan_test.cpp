// Real GPU comparison (short compute dispatch):
// glslangValidator -V tests/nvn_descriptor_templates_test.comp -o /tmp/nvn_descriptor_templates_test.spv
// c++ -std=c++20 -Iprivate/toolchains/Vulkan-Headers-1.4.357/include tests/nvn_descriptor_templates_vulkan_test.cpp -lvulkan -o /tmp/nvn_descriptor_templates_vulkan_test
// /tmp/nvn_descriptor_templates_vulkan_test /tmp/nvn_descriptor_templates_test.spv
#include "../runtime/src/nvn/nvn_descriptor_template.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

using namespace nx::nvn::replay;
#define CHECK(call) do { VkResult result_ = (call); if (result_ != VK_SUCCESS) { \
    std::fprintf(stderr, "%s: VkResult %d at line %d\n", #call, int(result_), __LINE__); std::abort(); } } while (0)

static PFN_vkCreateDescriptorUpdateTemplate real_create;
static PFN_vkUpdateDescriptorSetWithTemplate real_update;
static PFN_vkDestroyDescriptorUpdateTemplate real_destroy;
static uint32_t creates, updates, fallbacks;
VKAPI_ATTR VkResult VKAPI_CALL create(VkDevice d, const VkDescriptorUpdateTemplateCreateInfo* i,
                                       const VkAllocationCallbacks* a, VkDescriptorUpdateTemplate* t) {
    ++creates; return real_create(d, i, a, t);
}
VKAPI_ATTR void VKAPI_CALL update(VkDevice d, VkDescriptorSet s, VkDescriptorUpdateTemplate t, const void* p) {
    ++updates; real_update(d, s, t, p);
}
VKAPI_ATTR void VKAPI_CALL destroy(VkDevice d, VkDescriptorUpdateTemplate t, const VkAllocationCallbacks* a) {
    real_destroy(d, t, a);
}
VKAPI_ATTR void VKAPI_CALL fallback(VkDevice d, uint32_t n, const VkWriteDescriptorSet* w,
                                    uint32_t nc, const VkCopyDescriptorSet* c) {
    ++fallbacks; vkUpdateDescriptorSets(d, n, w, nc, c);
}
VkWriteDescriptorSet write(VkDescriptorSet s, uint32_t binding, VkDescriptorType type) {
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = s; w.dstBinding = binding; w.descriptorCount = 1; w.descriptorType = type; return w;
}
uint32_t memory_type(const VkPhysicalDeviceMemoryProperties& memory, uint32_t bits, VkMemoryPropertyFlags flags) {
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & flags) == flags) return i;
    std::fprintf(stderr, "no suitable memory type\n"); std::abort();
}
int main(int argc, char** argv) {
    if (argc != 2) { std::fprintf(stderr, "usage: %s shader.spv\n", argv[0]); return 2; }
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo = &app;
    VkInstance instance; CHECK(vkCreateInstance(&ici, nullptr, &instance));
    uint32_t physical_count = 0; CHECK(vkEnumeratePhysicalDevices(instance, &physical_count, nullptr));
    if (!physical_count) { std::puts("SKIP: no Vulkan physical device"); vkDestroyInstance(instance, nullptr); return 77; }
    std::vector<VkPhysicalDevice> physicals(physical_count);
    CHECK(vkEnumeratePhysicalDevices(instance, &physical_count, physicals.data()));
    VkPhysicalDevice physical = physicals[0]; VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical, &properties);
    if (properties.apiVersion < VK_API_VERSION_1_1) {
        std::puts("SKIP: Vulkan 1.1 unavailable"); vkDestroyInstance(instance, nullptr); return 77;
    }
    uint32_t family_count = 0; vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, families.data());
    uint32_t family = 0;
    while (family < family_count && !(families[family].queueFlags & VK_QUEUE_COMPUTE_BIT)) ++family;
    assert(family < family_count);
    float priority = 1;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = family; qci.queueCount = 1; qci.pQueuePriorities = &priority;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    VkDevice dev; CHECK(vkCreateDevice(physical, &dci, nullptr, &dev));
    VkQueue queue; vkGetDeviceQueue(dev, family, 0, &queue);
    real_create = reinterpret_cast<PFN_vkCreateDescriptorUpdateTemplate>(vkGetDeviceProcAddr(dev, "vkCreateDescriptorUpdateTemplate"));
    real_update = reinterpret_cast<PFN_vkUpdateDescriptorSetWithTemplate>(vkGetDeviceProcAddr(dev, "vkUpdateDescriptorSetWithTemplate"));
    real_destroy = reinterpret_cast<PFN_vkDestroyDescriptorUpdateTemplate>(vkGetDeviceProcAddr(dev, "vkDestroyDescriptorUpdateTemplate"));
    assert(real_create && real_update && real_destroy);
    VkPhysicalDeviceMemoryProperties memory; vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    const VkDeviceSize slot = std::max({VkDeviceSize(256), properties.limits.minUniformBufferOffsetAlignment,
                                        properties.limits.minStorageBufferOffsetAlignment,
                                        properties.limits.minTexelBufferOffsetAlignment});
    assert(slot <= 0x10000000 && slot * 8 < UINT32_MAX);
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bci.size = slot * 8;
    bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT;
    VkBuffer buffer; CHECK(vkCreateBuffer(dev, &bci, nullptr, &buffer));
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(dev, buffer, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; mai.allocationSize = req.size;
    mai.memoryTypeIndex = memory_type(memory, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory buffer_memory; CHECK(vkAllocateMemory(dev, &mai, nullptr, &buffer_memory));
    CHECK(vkBindBufferMemory(dev, buffer, buffer_memory, 0));
    unsigned char* mapped; CHECK(vkMapMemory(dev, buffer_memory, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&mapped)));
    auto set_value = [&](size_t i, uint32_t value) { std::memcpy(mapped + slot * i, &value, sizeof(value)); };
    set_value(1, 11); set_value(2, 22); set_value(3, 33); set_value(4, 44);
    VkBufferViewCreateInfo bvci{VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO};
    bvci.buffer = buffer; bvci.format = VK_FORMAT_R32_UINT; bvci.offset = slot * 4; bvci.range = 4;
    VkBufferView view; CHECK(vkCreateBufferView(dev, &bvci, nullptr, &view));

    VkImageCreateInfo image_ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_ci.imageType = VK_IMAGE_TYPE_2D; image_ci.format = VK_FORMAT_R32_UINT; image_ci.extent = {1, 1, 1};
    image_ci.mipLevels = 1; image_ci.arrayLayers = 1; image_ci.samples = VK_SAMPLE_COUNT_1_BIT;
    image_ci.tiling = VK_IMAGE_TILING_OPTIMAL; image_ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkImage image; CHECK(vkCreateImage(dev, &image_ci, nullptr, &image));
    vkGetImageMemoryRequirements(dev, image, &req); mai.allocationSize = req.size;
    mai.memoryTypeIndex = memory_type(memory, req.memoryTypeBits, 0);
    VkDeviceMemory image_memory; CHECK(vkAllocateMemory(dev, &mai, nullptr, &image_memory));
    CHECK(vkBindImageMemory(dev, image, image_memory, 0));
    VkImageViewCreateInfo ivci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; ivci.image = image;
    ivci.viewType = VK_IMAGE_VIEW_TYPE_2D; ivci.format = VK_FORMAT_R32_UINT;
    ivci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView image_view; CHECK(vkCreateImageView(dev, &ivci, nullptr, &image_view));
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST; sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkSampler sampler; CHECK(vkCreateSampler(dev, &sci, nullptr, &sampler));

    VkDescriptorSetLayoutBinding bindings0[] = {{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                                                {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutBinding bindings1[] = {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                                                {1, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                                                {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    VkDescriptorSetLayout layouts[4] = {}; lci.bindingCount = 2; lci.pBindings = bindings0;
    CHECK(vkCreateDescriptorSetLayout(dev, &lci, nullptr, &layouts[0]));
    lci.bindingCount = 3; lci.pBindings = bindings1;
    CHECK(vkCreateDescriptorSetLayout(dev, &lci, nullptr, &layouts[1]));
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 2}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 6},
                                    {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2}, {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 2},
                                    {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2}};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = 4; pci.poolSizeCount = 5; pci.pPoolSizes = sizes;
    VkDescriptorPool pool; CHECK(vkCreateDescriptorPool(dev, &pci, nullptr, &pool));
    VkDescriptorSetLayout allocations[] = {layouts[0], layouts[1], layouts[0], layouts[1]};
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = pool; ai.descriptorSetCount = 4; ai.pSetLayouts = allocations;
    VkDescriptorSet sets[4]; CHECK(vkAllocateDescriptorSets(dev, &ai, sets));
    const VkDescriptorSet ref_sets[4] = {sets[0], sets[1], {}, {}};
    const VkDescriptorSet tmpl_sets[4] = {sets[2], sets[3], {}, {}};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; plci.setLayoutCount = 2; plci.pSetLayouts = layouts;
    VkPipelineLayout pipeline_layout; CHECK(vkCreatePipelineLayout(dev, &plci, nullptr, &pipeline_layout));
    std::ifstream shader_file(argv[1], std::ios::binary | std::ios::ate); assert(shader_file);
    size_t shader_size = size_t(shader_file.tellg()); assert(shader_size && shader_size % 4 == 0);
    std::vector<uint32_t> shader_bytes(shader_size / 4); shader_file.seekg(0);
    shader_file.read(reinterpret_cast<char*>(shader_bytes.data()), shader_size); assert(shader_file);
    VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = shader_size; smci.pCode = shader_bytes.data();
    VkShaderModule shader; CHECK(vkCreateShaderModule(dev, &smci, nullptr, &shader));
    VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; cpci.stage.module = shader; cpci.stage.pName = "main";
    cpci.layout = pipeline_layout; VkPipeline pipeline;
    CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipeline));
    VkCommandPoolCreateInfo cmdpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; cmdpci.queueFamilyIndex = family;
    VkCommandPool cmdpool; CHECK(vkCreateCommandPool(dev, &cmdpci, nullptr, &cmdpool));
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = cmdpool; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
    VkCommandBuffer cb; CHECK(vkAllocateCommandBuffers(dev, &cbai, &cb));
    {
        DescriptorTemplateDispatch driver{create, update, destroy, fallback};
        DescriptorTemplateCache cache(dev, properties.apiVersion, driver, descriptor_templates_requested("1"));
        DescriptorTemplateCache disabled(dev, properties.apiVersion, driver, descriptor_templates_requested(nullptr));
        for (uint32_t round = 0; round < 2; ++round) {
            std::memset(mapped + 5 * slot, 0, 6 * sizeof(uint32_t));
            std::memset(mapped + 6 * slot, 0, 6 * sizeof(uint32_t));
            VkDescriptorBufferInfo dyn{buffer, 0, 4};
            VkDescriptorBufferInfo array[] = {{buffer, (round ? 3 : 2) * slot, 4}, {buffer, 2 * slot, 4},
                                              {buffer, (round ? 3 : 2) * slot, 4}};
            VkDescriptorBufferInfo second{buffer, slot, 4};
            VkDescriptorBufferInfo outputs[] = {{buffer, 5 * slot, 24}, {buffer, 6 * slot, 24}};
            VkDescriptorImageInfo img{sampler, image_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            for (uint32_t variant = 0; variant < 2; ++variant) {
                const VkDescriptorSet* target = variant ? tmpl_sets : ref_sets;
                VkWriteDescriptorSet writes[] = {write(target[0], 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC),
                    write(target[1], 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
                    write(target[1], 1, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER),
                    write(target[1], 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)};
                writes[0].pBufferInfo = &dyn; writes[1].pBufferInfo = &outputs[variant];
                writes[2].pTexelBufferView = &view; writes[3].pImageInfo = &img;
                auto arr_write = write(target[0], 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
                arr_write.descriptorCount = 3; arr_write.pBufferInfo = array;
                auto second_write = write(target[0], 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
                second_write.dstArrayElement = 1; second_write.pBufferInfo = &second;
                if (variant) {
                    cache.apply(layouts, tmpl_sets, 4, writes); cache.apply(layouts, tmpl_sets, 1, &arr_write);
                    cache.apply(layouts, tmpl_sets, 1, &second_write);
                } else {
                    disabled.apply(layouts, ref_sets, 4, writes); disabled.apply(layouts, ref_sets, 1, &arr_write);
                    disabled.apply(layouts, ref_sets, 1, &second_write);
                }
            }
            CHECK(vkResetCommandPool(dev, cmdpool, 0));
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; CHECK(vkBeginCommandBuffer(cb, &begin));
            if (!round) {
                VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.image = image; barrier.subresourceRange = ivci.subresourceRange; barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
                VkClearColorValue color{}; color.uint32[0] = 55;
                vkCmdClearColorImage(cb, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &ivci.subresourceRange);
                barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
            }
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
            uint32_t offset = uint32_t((round ? 2 : 1) * slot);
            for (const auto* target : {ref_sets, tmpl_sets}) {
                vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 2, target, 1, &offset);
                vkCmdDispatch(cb, 1, 1, 1);
            }
            VkMemoryBarrier host_barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            host_barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; host_barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host_barrier, 0, nullptr, 0, nullptr);
            CHECK(vkEndCommandBuffer(cb));
            VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &cb;
            CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE)); CHECK(vkQueueWaitIdle(queue));
            uint32_t expected[] = {round ? 22u : 11u, round ? 33u : 22u, 11, round ? 33u : 22u, 44, 55};
            assert(std::memcmp(mapped + 5 * slot, expected, sizeof(expected)) == 0);
            assert(std::memcmp(mapped + 6 * slot, expected, sizeof(expected)) == 0);
        }
        assert(creates == 4 && updates == 8 && fallbacks == 6);
    } // clear templates while the device and layouts are alive
    std::printf("Vulkan descriptor comparison passed on %s; 4 templates reused, 6 disabled-path fallbacks\n", properties.deviceName);
    vkDestroyCommandPool(dev, cmdpool, nullptr); vkDestroyPipeline(dev, pipeline, nullptr); vkDestroyShaderModule(dev, shader, nullptr);
    vkDestroyPipelineLayout(dev, pipeline_layout, nullptr); vkDestroyDescriptorPool(dev, pool, nullptr);
    vkDestroyDescriptorSetLayout(dev, layouts[1], nullptr); vkDestroyDescriptorSetLayout(dev, layouts[0], nullptr);
    vkDestroySampler(dev, sampler, nullptr); vkDestroyImageView(dev, image_view, nullptr);
    vkDestroyImage(dev, image, nullptr); vkFreeMemory(dev, image_memory, nullptr);
    vkDestroyBufferView(dev, view, nullptr); vkUnmapMemory(dev, buffer_memory);
    vkDestroyBuffer(dev, buffer, nullptr); vkFreeMemory(dev, buffer_memory, nullptr);
    vkDestroyDevice(dev, nullptr); vkDestroyInstance(instance, nullptr);
}
