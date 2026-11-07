// Standalone mock tests: c++ -std=c++20 -Iprivate/toolchains/Vulkan-Headers-1.4.357/include
// tests/nvn_descriptor_templates_test.cpp -o /tmp/nvn_descriptor_templates_test && /tmp/nvn_descriptor_templates_test
#include "../runtime/src/nvn/nvn_descriptor_template.h"

#include <cassert>
#include <cstdio>
#include <map>
#include <tuple>
#include <type_traits>
#include <vector>

using namespace nx::nvn::replay;

template <class T> T h(uintptr_t value) {
    if constexpr (std::is_pointer_v<T>) return reinterpret_cast<T>(value);
    else return static_cast<T>(value);
}
template <class T> uintptr_t bits(T handle) {
    if constexpr (std::is_pointer_v<T>) return reinterpret_cast<uintptr_t>(handle);
    else return static_cast<uintptr_t>(handle);
}
using Location = std::tuple<uintptr_t, uint32_t, uint32_t>;
using State = std::map<Location, std::vector<unsigned char>>;
struct Mock {
    uint32_t creates = 0, updates = 0, fallbacks = 0, destroys = 0;
    bool fail = false;
    uintptr_t fail_layout = 0;
    uintptr_t next = 100;
    std::map<uintptr_t, std::vector<VkDescriptorUpdateTemplateEntry>> templates;
    std::vector<uintptr_t> destroyed;
    State state;
    uint32_t last_fallback_count = 0;
    const void* last_pnext = nullptr;
} mock;

void record(State& state, VkDescriptorSet set, uint32_t binding, uint32_t element, VkDescriptorType type,
            uint32_t count, size_t stride, const void* payload) {
    auto kind = descriptor_payload(type);
    const auto size = descriptor_data_size(kind);
    if (!payload || !size) return;
    for (uint32_t i = 0; i < count; ++i) {
        auto p = static_cast<const unsigned char*>(payload) + i * stride;
        state[{bits(set), binding, element + i}] = {p, p + size};
    }
}
void record_writes(State& state, uint32_t n, const VkWriteDescriptorSet* writes) {
    for (uint32_t i = 0; i < n; ++i) {
        const auto& w = writes[i];
        auto kind = descriptor_payload(w.descriptorType);
        record(state, w.dstSet, w.dstBinding, w.dstArrayElement, w.descriptorType, w.descriptorCount,
               descriptor_data_size(kind), descriptor_data(w, kind));
    }
}
VKAPI_ATTR VkResult VKAPI_CALL create(VkDevice, const VkDescriptorUpdateTemplateCreateInfo* ci,
                                       const VkAllocationCallbacks*, VkDescriptorUpdateTemplate* out) {
    ++mock.creates;
    assert(ci->sType == VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO);
    assert(ci->templateType == VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET);
    assert(ci->descriptorSetLayout);
    if (mock.fail || (mock.fail_layout && mock.fail_layout == bits(ci->descriptorSetLayout)))
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    *out = h<VkDescriptorUpdateTemplate>(++mock.next);
    mock.templates[bits(*out)] = {ci->pDescriptorUpdateEntries, ci->pDescriptorUpdateEntries + ci->descriptorUpdateEntryCount};
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL update(VkDevice, VkDescriptorSet set, VkDescriptorUpdateTemplate tmpl, const void* data) {
    ++mock.updates;
    const auto& entries = mock.templates.at(bits(tmpl));
    for (const auto& e : entries)
        record(mock.state, set, e.dstBinding, e.dstArrayElement, e.descriptorType, e.descriptorCount, e.stride,
               static_cast<const unsigned char*>(data) + e.offset);
}
VKAPI_ATTR void VKAPI_CALL destroy(VkDevice, VkDescriptorUpdateTemplate tmpl, const VkAllocationCallbacks*) {
    ++mock.destroys;
    mock.destroyed.push_back(bits(tmpl));
    assert(mock.templates.erase(bits(tmpl)) == 1);
}
VKAPI_ATTR void VKAPI_CALL update_sets(VkDevice, uint32_t count, const VkWriteDescriptorSet* writes,
                                        uint32_t copies, const VkCopyDescriptorSet*) {
    ++mock.fallbacks;
    assert(copies == 0);
    mock.last_fallback_count = count;
    mock.last_pnext = count && writes ? writes[0].pNext : nullptr;
    if (writes) record_writes(mock.state, count, writes);
}
DescriptorTemplateDispatch dispatch() { return {create, update, destroy, update_sets}; }
VkWriteDescriptorSet make_write(VkDescriptorSet set, uint32_t binding, VkDescriptorType type) {
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set; w.dstBinding = binding; w.descriptorCount = 1; w.descriptorType = type;
    return w;
}

int main() {
    assert(!descriptor_templates_requested(nullptr));
    assert(!descriptor_templates_requested(""));
    assert(!descriptor_templates_requested("0"));
    assert(!descriptor_templates_requested("true"));
    assert(!descriptor_templates_requested("10"));
    assert(descriptor_templates_requested("1"));
    const VkDevice dev = h<VkDevice>(1);
    const VkDescriptorSet sets[4] = {h<VkDescriptorSet>(11), h<VkDescriptorSet>(12), h<VkDescriptorSet>(13), {}};
    VkDescriptorSetLayout layouts[4] = {h<VkDescriptorSetLayout>(21), h<VkDescriptorSetLayout>(22),
                                        h<VkDescriptorSetLayout>(23), {}};
    VkDescriptorBufferInfo buffer[3] = {{h<VkBuffer>(31), 16, 128}, {h<VkBuffer>(32), 32, 256},
                                        {h<VkBuffer>(33), 48, 512}};
    VkDescriptorImageInfo image[2] = {{h<VkSampler>(41), h<VkImageView>(42), VK_IMAGE_LAYOUT_GENERAL},
                                      {h<VkSampler>(43), h<VkImageView>(44), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkBufferView texel[2] = {h<VkBufferView>(51), h<VkBufferView>(52)};
    auto dynamic = make_write(sets[0], 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC); dynamic.pBufferInfo = buffer;
    auto image_write = make_write(sets[1], 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); image_write.pImageInfo = image;
    auto view_write = make_write(sets[0], 2, VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER); view_write.pTexelBufferView = texel;
    auto buffer_write = make_write(sets[1], 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER); buffer_write.pBufferInfo = buffer + 1;
    VkWriteDescriptorSet mixed[] = {dynamic, image_write, view_write, buffer_write};
    {
        DescriptorTemplateCache cache(dev, VK_API_VERSION_1_1, dispatch());
        State expected; record_writes(expected, 4, mixed);
        cache.apply(layouts, sets, 4, mixed);
        assert(mock.state == expected && mock.creates == 2 && mock.updates == 2 && mock.fallbacks == 0);
        buffer[0].offset = 64; image[0] = image[1]; texel[0] = texel[1];
        record_writes(expected, 4, mixed);
        cache.apply(layouts, sets, 4, mixed);
        assert(mock.state == expected && mock.creates == 2 && mock.updates == 4);
        // Dynamic offsets are separate from this descriptor's buffer offset and are not folded into the template.
        assert(buffer[0].offset == 64);

        // Single writes can contain arrays and nonzero dstArrayElement for all three payload pointer types.
        auto array_buffer = dynamic; array_buffer.dstBinding = 5; array_buffer.dstArrayElement = 2;
        array_buffer.descriptorCount = 3; array_buffer.pBufferInfo = buffer;
        auto array_image = image_write; array_image.dstBinding = 6; array_image.dstArrayElement = 1;
        array_image.descriptorCount = 2; array_image.pImageInfo = image;
        auto array_texel = view_write; array_texel.dstBinding = 7; array_texel.dstArrayElement = 1;
        array_texel.descriptorCount = 2; array_texel.pTexelBufferView = texel;
        for (auto* w : {&array_buffer, &array_image, &array_texel}) {
            record_writes(expected, 1, w); cache.apply(layouts, sets, 1, w);
        }
        assert(mock.state == expected && mock.creates == 5 && mock.updates == 7);
        assert(!descriptor_write_cacheable(array_buffer));
        auto array_element = dynamic; array_element.dstArrayElement = 1;
        assert(!descriptor_write_cacheable(array_element));
        assert(descriptor_write_cacheable(dynamic) && descriptor_write_cacheable(image_write) &&
               descriptor_write_cacheable(view_write));

        // Template identity includes layout, binding, array element, count, type, and packed offsets/strides.
        auto key_write = dynamic; key_write.dstBinding = 50;
        cache.apply(layouts, sets, 1, &key_write); auto n = mock.creates;
        cache.apply(layouts, sets, 1, &key_write); assert(mock.creates == n);
        key_write.dstBinding++; cache.apply(layouts, sets, 1, &key_write); assert(mock.creates == ++n);
        key_write.dstArrayElement++; cache.apply(layouts, sets, 1, &key_write); assert(mock.creates == ++n);
        key_write.descriptorCount = 2; cache.apply(layouts, sets, 1, &key_write); assert(mock.creates == ++n);
        key_write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
        cache.apply(layouts, sets, 1, &key_write); assert(mock.creates == ++n);
        auto saved_layout = layouts[0]; layouts[0] = h<VkDescriptorSetLayout>(24);
        cache.apply(layouts, sets, 1, &key_write); assert(mock.creates == ++n); layouts[0] = saved_layout;
        VkWriteDescriptorSet reversed[] = {view_write, image_write, dynamic, buffer_write};
        cache.apply(layouts, sets, 4, reversed); assert(mock.creates == n + 1); // changed entry order for set 0

        // Conservative fallback passes the original batch through exactly once, preserving original order.
        const int extension = 123;
        VkWriteDescriptorSet bad[] = {dynamic, image_write}; bad[0].pNext = &extension;
        auto falls = mock.fallbacks; n = mock.creates;
        assert(!descriptor_write_cacheable(bad[0])); cache.apply(layouts, sets, 2, bad);
        assert(mock.fallbacks == ++falls && mock.last_fallback_count == 2 && mock.last_pnext == &extension);
        bad[0] = dynamic; bad[0].descriptorType = VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK;
        cache.apply(layouts, sets, 2, bad); assert(mock.fallbacks == ++falls);
        bad[0] = dynamic; bad[0].pBufferInfo = nullptr; bad[0].pImageInfo = image;
        cache.apply(layouts, sets, 2, bad); assert(mock.fallbacks == ++falls);
        bad[0] = dynamic; bad[0].dstSet = h<VkDescriptorSet>(99);
        cache.apply(layouts, sets, 2, bad); assert(mock.fallbacks == ++falls);
        bad[0] = dynamic; bad[0].descriptorCount = 0;
        cache.apply(layouts, sets, 2, bad); assert(mock.fallbacks == ++falls);
        bad[0] = array_buffer; bad[1] = dynamic; // counted write plus another write on the same set
        cache.apply(layouts, sets, 2, bad); assert(mock.fallbacks == ++falls);
        bad[0] = dynamic; bad[1] = dynamic; bad[1].pBufferInfo = buffer + 1;
        State overlap; record_writes(overlap, 2, bad); mock.state.clear();
        cache.apply(layouts, sets, 2, bad); assert(mock.fallbacks == ++falls && mock.state == overlap);
        assert(mock.creates == n);
        const VkDescriptorSet alias_sets[4] = {sets[0], sets[0], sets[2], {}};
        cache.apply(layouts, alias_sets, 1, &dynamic); assert(mock.fallbacks == ++falls);
        std::vector<VkWriteDescriptorSet> large(1025, dynamic);
        for (uint32_t i = 0; i < large.size(); ++i) large[i].dstBinding = i;
        cache.apply(layouts, sets, uint32_t(large.size()), large.data());
        assert(mock.fallbacks == ++falls && mock.last_fallback_count == large.size() && mock.creates == n);
        cache.apply(layouts, sets, 0, nullptr); assert(mock.fallbacks == falls);

        // Vulkan ignores the two irrelevant pointers; classification must use descriptorType, not pointer presence.
        for (VkDescriptorType type : {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
             VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC,
             VK_DESCRIPTOR_TYPE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
             VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,
             VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER}) {
            auto w = make_write(sets[0], 200, type);
            w.pBufferInfo = buffer; w.pImageInfo = image; w.pTexelBufferView = texel;
            State typed; record_writes(typed, 1, &w); mock.state.clear();
            cache.apply(layouts, sets, 1, &w);
            assert(mock.state == typed && mock.fallbacks == falls && descriptor_write_cacheable(w));
        }
    }
    assert(mock.templates.empty() && mock.creates == mock.destroys);

    mock = {};
    for (int mode = 0; mode < 6; ++mode) {
        auto fns = dispatch(); uint32_t version = VK_API_VERSION_1_1; bool enabled = true;
        if (mode == 0) version = VK_API_VERSION_1_0;
        if (mode == 1) fns.create = nullptr;
        if (mode == 2) fns.update = nullptr;
        if (mode == 3) fns.destroy = nullptr;
        if (mode == 4) enabled = false;
        if (mode == 5) version = VK_MAKE_API_VERSION(1, 1, 3, 0);
        DescriptorTemplateCache cache(dev, version, fns, enabled);
        cache.apply(layouts, sets, 1, &dynamic);
    }
    assert(mock.creates == 0 && mock.updates == 0 && mock.fallbacks == 6);
    mock = {}; mock.fail = true;
    {
        DescriptorTemplateCache cache(dev, VK_API_VERSION_1_1, dispatch());
        cache.apply(layouts, sets, 1, &dynamic); cache.apply(layouts, sets, 1, &dynamic);
        assert(mock.creates == 1 && mock.updates == 0 && mock.fallbacks == 2);
    }
    assert(mock.destroys == 0);
    // One set may fail template creation while another succeeds; each group still receives its own correct payload.
    mock = {}; mock.fail_layout = bits(layouts[1]);
    {
        DescriptorTemplateCache cache(dev, VK_API_VERSION_1_1, dispatch());
        State expected; record_writes(expected, 4, mixed);
        cache.apply(layouts, sets, 4, mixed);
        assert(mock.state == expected && mock.creates == 2 && mock.updates == 1 && mock.fallbacks == 1);
        cache.apply(layouts, sets, 4, mixed);
        assert(mock.state == expected && mock.creates == 2 && mock.updates == 2 && mock.fallbacks == 2);
    }
    assert(mock.destroys == 1 && mock.templates.empty());
    mock = {};
    {
        DescriptorTemplateCache cache(dev, VK_API_VERSION_1_1, dispatch(), true, 2);
        auto w = dynamic;
        cache.apply(layouts, sets, 1, &w); // 101
        w.dstBinding++; cache.apply(layouts, sets, 1, &w); // 102
        w.dstBinding--; cache.apply(layouts, sets, 1, &w); // refresh 101
        w.dstBinding += 2; cache.apply(layouts, sets, 1, &w); // evict 102
        assert(mock.creates == 3 && mock.destroys == 1 && mock.destroyed[0] == 102);
        cache.clear(); assert(mock.destroys == 3 && mock.templates.empty());
    }
    assert(mock.destroys == 3);
    std::puts("descriptor template mock tests passed");
}
