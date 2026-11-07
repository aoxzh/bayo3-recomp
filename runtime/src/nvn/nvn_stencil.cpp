// A depth/stencil texture read as its stencil index (NVN_TEXTURE_DEPTH_STENCIL_MODE_STENCIL, on the view or the
// texture). Maxwell returns the index as an integer and the translated shaders, which declare every such image as a
// float image, take it back with a bitcast of .x. Vulkan offers the stencil aspect only as an unsigned-integer image:
// bound to a float image it is undefined, and Intel's driver converts the index to a float (16 -> 16.0, whose low
// bits are 0). So the index is widened here into an R32_UINT image, by a small compute pass, and the shader samples
// that image through an R32_SFLOAT view: the 32 bits it reads are the index, as on Maxwell.
// Bayonetta Origins (program 1) tests stencil bit 4 per pixel in its storybook composite; read as depth, that bit was
// one of the depth's mantissa bits and the forest came out covered in zigzag moire (docs/CHAPTER1-AUTODRIVE.md 12.16).
#include <cstdlib>
#include <unordered_map>

#include "nvn_pipeline.h"

namespace nx::nvn {

using namespace nx::vk;

namespace {

// runtime/shaders/stencil_bits.spvasm, assembled (spirv-as --target-env vulkan1.0).
constexpr uint32_t kStencilBits[] = {
    0x07230203, 0x00010000, 0x00070000, 0x00000021, 0x00000000, 0x00020011, 0x00000001, 0x00020011, 0x00000032,
    0x0003000e, 0x00000000, 0x00000001, 0x0006000f, 0x00000005, 0x00000001, 0x6e69616d, 0x00000000, 0x00000002,
    0x00060010, 0x00000001, 0x00000011, 0x00000008, 0x00000008, 0x00000001, 0x00040047, 0x00000002, 0x0000000b,
    0x0000001c, 0x00040047, 0x00000003, 0x00000022, 0x00000000, 0x00040047, 0x00000003, 0x00000021, 0x00000000,
    0x00040047, 0x00000004, 0x00000022, 0x00000000, 0x00040047, 0x00000004, 0x00000021, 0x00000001, 0x00030047,
    0x00000004, 0x00000019, 0x00020013, 0x00000005, 0x00030021, 0x00000006, 0x00000005, 0x00040015, 0x00000007,
    0x00000020, 0x00000000, 0x00040015, 0x00000008, 0x00000020, 0x00000001, 0x00020014, 0x00000009, 0x00040017,
    0x0000000a, 0x00000007, 0x00000002, 0x00040017, 0x0000000b, 0x00000007, 0x00000003, 0x00040017, 0x0000000c,
    0x00000007, 0x00000004, 0x00040017, 0x0000000d, 0x00000008, 0x00000002, 0x00040017, 0x0000000e, 0x00000009,
    0x00000002, 0x0004002b, 0x00000008, 0x0000000f, 0x00000000, 0x00090019, 0x00000010, 0x00000007, 0x00000001,
    0x00000000, 0x00000000, 0x00000000, 0x00000001, 0x00000000, 0x00040020, 0x00000011, 0x00000000, 0x00000010,
    0x0004003b, 0x00000011, 0x00000003, 0x00000000, 0x00090019, 0x00000012, 0x00000007, 0x00000001, 0x00000000,
    0x00000000, 0x00000000, 0x00000002, 0x00000021, 0x00040020, 0x00000013, 0x00000000, 0x00000012, 0x0004003b,
    0x00000013, 0x00000004, 0x00000000, 0x00040020, 0x00000014, 0x00000001, 0x0000000b, 0x0004003b, 0x00000014,
    0x00000002, 0x00000001, 0x00050036, 0x00000005, 0x00000001, 0x00000000, 0x00000006, 0x000200f8, 0x00000015,
    0x0004003d, 0x0000000b, 0x00000016, 0x00000002, 0x0007004f, 0x0000000a, 0x00000017, 0x00000016, 0x00000016,
    0x00000000, 0x00000001, 0x0004007c, 0x0000000d, 0x00000018, 0x00000017, 0x0004003d, 0x00000010, 0x00000019,
    0x00000003, 0x00050067, 0x0000000d, 0x0000001a, 0x00000019, 0x0000000f, 0x000500b1, 0x0000000e, 0x0000001b,
    0x00000018, 0x0000001a, 0x0004009b, 0x00000009, 0x0000001c, 0x0000001b, 0x000300f7, 0x0000001d, 0x00000000,
    0x000400fa, 0x0000001c, 0x0000001e, 0x0000001d, 0x000200f8, 0x0000001e, 0x0007005f, 0x0000000c, 0x0000001f,
    0x00000019, 0x00000018, 0x00000002, 0x0000000f, 0x0004003d, 0x00000012, 0x00000020, 0x00000004, 0x00040063,
    0x00000020, 0x00000018, 0x0000001f, 0x000200f9, 0x0000001d, 0x000200f8, 0x0000001d, 0x000100fd, 0x00010038,
};

struct Pass {  // the compute pipeline, made once
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    bool failed = false;
};

// One per depth/stencil image read as stencil: level 0 / layer 0 of it, widened. Its views never change, so its
// descriptor set is written once.
struct Widened {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView storage = VK_NULL_HANDLE, sampled = VK_NULL_HANDLE, stencil = VK_NULL_HANDLE;
    VkImageLayout stencil_layout = VK_IMAGE_LAYOUT_UNDEFINED;  // the layout the set was written with
    VkDescriptorSet set = VK_NULL_HANDLE;
    bool initialized = false;  // moved out of UNDEFINED
};

Pass g_pass;
std::unordered_map<VkImage, Widened> g_widened;  // by the depth/stencil image; under the GPU lock like every view cache

bool make_pass() {
    if (g_pass.pipeline || g_pass.failed) return g_pass.pipeline != VK_NULL_HANDLE;
    VkDevice d = vk_device();
    g_pass.failed = true;
    VkDescriptorSetLayoutBinding b[2]{};
    b[0] = {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    b[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 2, li.pBindings = b;
    if (p_vkCreateDescriptorSetLayout(d, &li, nullptr, &g_pass.set_layout) != VK_SUCCESS) return false;
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1, pl.pSetLayouts = &g_pass.set_layout;
    if (p_vkCreatePipelineLayout(d, &pl, nullptr, &g_pass.layout) != VK_SUCCESS) return false;
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = sizeof kStencilBits, mi.pCode = kStencilBits;
    VkShaderModule module = VK_NULL_HANDLE;
    if (p_vkCreateShaderModule(d, &mi, nullptr, &module) != VK_SUCCESS) return false;
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT, ci.stage.module = module, ci.stage.pName = "main";
    ci.layout = g_pass.layout;
    VkResult r = p_vkCreateComputePipelines(d, VK_NULL_HANDLE, 1, &ci, nullptr, &g_pass.pipeline);
    p_vkDestroyShaderModule(d, module, nullptr);
    if (r != VK_SUCCESS) return false;
    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 64}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 64}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pi.maxSets = 64, pi.poolSizeCount = 2, pi.pPoolSizes = sizes;
    if (p_vkCreateDescriptorPool(d, &pi, nullptr, &g_pass.pool) != VK_SUCCESS) return false;
    g_pass.failed = false;
    return true;
}

void destroy(Widened& w) {
    VkDevice d = vk_device();
    if (w.set) p_vkFreeDescriptorSets(d, g_pass.pool, 1, &w.set);
    for (VkImageView v : {w.storage, w.sampled, w.stencil})
        if (v) p_vkDestroyImageView(d, v, nullptr);
    if (w.image) p_vkDestroyImage(d, w.image, nullptr);
    if (w.memory) p_vkFreeMemory(d, w.memory, nullptr);
    w = {};
}

bool make_widened(Widened& w, const GpuImage& img) {
    VkDevice d = vk_device();
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;  // written as R32_UINT, sampled as R32_SFLOAT
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R32_UINT;
    ii.extent = {img.width, img.height, 1};
    ii.mipLevels = ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (p_vkCreateImage(d, &ii, nullptr, &w.image) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    p_vkGetImageMemoryRequirements(d, w.image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (p_vkAllocateMemory(d, &ai, nullptr, &w.memory) != VK_SUCCESS) return false;
    p_vkBindImageMemory(d, w.image, w.memory, 0);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.image = w.image, vi.format = VK_FORMAT_R32_UINT;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (p_vkCreateImageView(d, &vi, nullptr, &w.storage) != VK_SUCCESS) return false;
    vi.format = VK_FORMAT_R32_SFLOAT;
    if (p_vkCreateImageView(d, &vi, nullptr, &w.sampled) != VK_SUCCESS) return false;
    vi.image = img.image, vi.format = img.format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
    if (p_vkCreateImageView(d, &vi, nullptr, &w.stencil) != VK_SUCCESS) return false;
    VkDescriptorSetAllocateInfo si{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    si.descriptorPool = g_pass.pool, si.descriptorSetCount = 1, si.pSetLayouts = &g_pass.set_layout;
    return p_vkAllocateDescriptorSets(d, &si, &w.set) == VK_SUCCESS;
}

}  // namespace

bool reads_stencil(uint64_t texture, const GpuImage& img, const ViewInfo* view) {
    static const bool off = std::getenv("NX_NO_STENCIL_READ") != nullptr;  // bisecting: the depth, as before
    if (off || img.aspect != (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) return false;
    if (view && view->depth_stencil) return view->depth_stencil == 2;
    Texture* t = get<Texture>(texture);
    return t && t->depth_stencil_mode == 1;
}

VkImageView stencil_view(VkCommandBuffer cb, GpuImage& img, VkImageLayout layout) {
    if (!make_pass()) return VK_NULL_HANDLE;
    Widened& w = g_widened[img.image];
    if (!w.image && !make_widened(w, img)) {
        log("nvn/vk: no stencil image for %ux%u depth/stencil: its depth is sampled instead", img.width, img.height);
        destroy(w);
        g_widened.erase(img.image);
        return VK_NULL_HANDLE;
    }
    if (w.stencil_layout != layout) {
        VkDescriptorImageInfo src{VK_NULL_HANDLE, w.stencil, layout}, dst{VK_NULL_HANDLE, w.storage, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet wr[2]{};
        for (int k = 0; k < 2; k++) {
            wr[k] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            wr[k].dstSet = w.set, wr[k].dstBinding = uint32_t(k), wr[k].descriptorCount = 1;
            wr[k].descriptorType = k ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            wr[k].pImageInfo = k ? &dst : &src;
        }
        p_vkUpdateDescriptorSets(vk_device(), 2, wr, 0, nullptr);
        w.stencil_layout = layout;
    }
    // The stencil's last writes (attachment, clear, copy) before the read; the widened image's last reads before the
    // write. Every draw that reads it widens it again: a few full-screen draws per frame in program 1.
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    VkImageMemoryBarrier ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    ib.srcAccessMask = VK_ACCESS_SHADER_READ_BIT, ib.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    ib.oldLayout = w.initialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
    ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.image = w.image, ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0,
                           nullptr, 1, &ib);
    w.initialized = true;
    p_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_pass.pipeline);
    p_vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_pass.layout, 0, 1, &w.set, 0, nullptr);
    p_vkCmdDispatch(cb, (img.width + 7) / 8, (img.height + 7) / 8, 1);
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0,
                           nullptr, 0, nullptr);
    static bool logged = false;
    if (!logged) log("nvn/vk: a %ux%u depth/stencil texture read as stencil: widened to R32 for the shader", img.width,
                     img.height), logged = true;
    return w.sampled;
}

void forget_stencil_view(VkImage image) {
    auto it = g_widened.find(image);
    if (it == g_widened.end()) return;
    destroy(it->second);
    g_widened.erase(it);
}

}  // namespace nx::nvn
