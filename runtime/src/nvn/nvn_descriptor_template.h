// Descriptor update templates for the replay's per-program layouts. The caller serializes access with the GPU lock.
#pragma once

#include <vulkan/vulkan.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <unordered_map>
#include <vector>

namespace nx::nvn::replay {

// Only these descriptor types use the three pointer members of VkWriteDescriptorSet. Other types can carry data in
// pNext, or have extension-specific semantics, and must stay on vkUpdateDescriptorSets.
enum class DescriptorPayload { unsupported, buffer, image, texel };

inline DescriptorPayload descriptor_payload(VkDescriptorType type) {
    switch (type) {
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: return DescriptorPayload::buffer;
    case VK_DESCRIPTOR_TYPE_SAMPLER:
    case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
    case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
    case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
    case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT: return DescriptorPayload::image;
    case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
    case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: return DescriptorPayload::texel;
    default: return DescriptorPayload::unsupported;
    }
}

inline const void* descriptor_data(const VkWriteDescriptorSet& write, DescriptorPayload kind) {
    switch (kind) {
    case DescriptorPayload::buffer: return write.pBufferInfo;
    case DescriptorPayload::image: return write.pImageInfo;
    case DescriptorPayload::texel: return write.pTexelBufferView;
    default: return nullptr;
    }
}

inline size_t descriptor_data_size(DescriptorPayload kind) {
    switch (kind) {
    case DescriptorPayload::buffer: return sizeof(VkDescriptorBufferInfo);
    case DescriptorPayload::image: return sizeof(VkDescriptorImageInfo);
    case DescriptorPayload::texel: return sizeof(VkBufferView);
    default: return 0;
    }
}

// The existing per-submit set cache only represents a single descriptor at array element zero. Do not drop a write
// unless all of its semantic data is represented in that cache's key.
inline bool descriptor_write_cacheable(const VkWriteDescriptorSet& write) {
    const auto kind = descriptor_payload(write.descriptorType);
    return write.sType == VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET && !write.pNext && write.dstArrayElement == 0 &&
           write.descriptorCount == 1 && descriptor_data(write, kind);
}

inline bool descriptor_templates_requested(const char* value) {
    return value && std::strcmp(value, "1") == 0;
}

struct DescriptorTemplateDispatch {
    PFN_vkCreateDescriptorUpdateTemplate create = nullptr;
    PFN_vkUpdateDescriptorSetWithTemplate update = nullptr;
    PFN_vkDestroyDescriptorUpdateTemplate destroy = nullptr;
    PFN_vkUpdateDescriptorSets update_sets = nullptr;
};

// One instance belongs to one VkDevice. Entries include the actual descriptor-set layout and all template entry
// fields; descriptor values are supplied on each update and never cached here. clear() must be called under the GPU
// lock before destroying a device or its descriptor-set layouts. The runtime's device/layouts are process-lifetime.
class DescriptorTemplateCache {
public:
    DescriptorTemplateCache(VkDevice device, uint32_t api_version, DescriptorTemplateDispatch dispatch,
                            bool enabled = true, size_t capacity = 1024)
        : device_(device), dispatch_(dispatch), capacity_(capacity),
          enabled_(enabled && device && capacity && VK_API_VERSION_VARIANT(api_version) == 0 &&
                   api_version >= VK_API_VERSION_1_1 && dispatch.create && dispatch.update && dispatch.destroy &&
                   dispatch.update_sets) {}

    DescriptorTemplateCache(const DescriptorTemplateCache&) = delete;
    DescriptorTemplateCache& operator=(const DescriptorTemplateCache&) = delete;
    ~DescriptorTemplateCache() { clear(); }

    void clear() {
        for (const auto& pair : cache_)
            if (pair.second.handle) dispatch_.destroy(device_, pair.second.handle, nullptr);
        cache_.clear();
    }

    // Writes to distinct descriptor sets are independent. Preserve their original order within each set, and use one
    // template per set. If any write cannot be classified safely, retain the original single Vulkan update call.
    void apply(const VkDescriptorSetLayout (&layouts)[4], const VkDescriptorSet (&sets)[4], uint32_t count,
               const VkWriteDescriptorSet* writes) {
        if (!count) return;
        if (!enabled_ || !writes) return fallback(count, writes);
        for (auto& group : groups_) group.clear();
        for (uint32_t i = 0; i < count; ++i) {
            const auto& write = writes[i];
            const auto kind = descriptor_payload(write.descriptorType);
            if (write.sType != VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET || write.pNext || !write.descriptorCount ||
                !descriptor_data(write, kind)) return fallback(count, writes);
            int set_index = -1;
            for (int k = 0; k < 4; ++k) {
                if (!write.dstSet || write.dstSet != sets[k]) continue;
                if (!layouts[k] || (set_index >= 0 && layouts[k] != layouts[set_index]))
                    return fallback(count, writes);
                if (set_index < 0) set_index = k;
            }
            if (set_index < 0) return fallback(count, writes);
            auto& group = groups_[set_index];
            if (group.size() == kMaxEntries) return fallback(count, writes);
            group.push_back(&write);
        }
        for (const auto& group : groups_) {
            // A counted write can span consecutive layout bindings. Also, duplicate bindings may overlap. Keep the
            // original API call for those mixed sequences instead of assuming template entry ordering/overlap rules.
            if (group.size() > 1) {
                for (size_t i = 0; i < group.size(); ++i) {
                    if (group[i]->descriptorCount != 1) return fallback(count, writes);
                    for (size_t j = 0; j < i; ++j)
                        if (group[j]->dstBinding == group[i]->dstBinding) return fallback(count, writes);
                }
            }
        }
        for (size_t k = 0; k < groups_.size(); ++k)
            if (!groups_[k].empty()) apply_group(layouts[k], groups_[k]);
    }

private:
    // Multi-entry groups contain single descriptors only. These aligned slots hold mixed descriptor payload types;
    // a single counted write instead reads its original, contiguous Vulkan array directly.
    struct Slot { VkDescriptorBufferInfo buffer; VkDescriptorImageInfo image; VkBufferView texel; };
    struct Entry {
        uint32_t binding, array_element, count;
        VkDescriptorType type;
        size_t offset, stride;
        bool operator==(const Entry& o) const {
            return binding == o.binding && array_element == o.array_element && count == o.count && type == o.type &&
                   offset == o.offset && stride == o.stride;
        }
    };
    struct Key {
        VkDescriptorSetLayout layout = VK_NULL_HANDLE;
        std::vector<Entry> entries;
        bool operator==(const Key& o) const { return layout == o.layout && entries == o.entries; }
    };
    struct KeyHash {
        size_t operator()(const Key& key) const {
            size_t h = std::hash<VkDescriptorSetLayout>{}(key.layout);
            auto add = [&h](size_t v) { h ^= v + size_t(0x9e3779b9U) + (h << 6) + (h >> 2); };
            add(key.entries.size());
            for (const auto& e : key.entries) {
                add(e.binding); add(e.array_element); add(e.count); add(static_cast<size_t>(e.type));
                add(e.offset); add(e.stride);
            }
            return h;
        }
    };
    struct Cached { VkDescriptorUpdateTemplate handle = VK_NULL_HANDLE; uint64_t last_used = 0; };
    static constexpr size_t kMaxEntries = 1024;

    void fallback(uint32_t count, const VkWriteDescriptorSet* writes) const {
        dispatch_.update_sets(device_, count, writes, 0, nullptr);
    }

    void fallback_group(const std::vector<const VkWriteDescriptorSet*>& group) {
        fallback_writes_.clear();
        for (const auto* write : group) fallback_writes_.push_back(*write);
        fallback(static_cast<uint32_t>(fallback_writes_.size()), fallback_writes_.data());
    }

    void evict_if_full() {
        if (cache_.size() < capacity_) return;
        auto oldest = cache_.begin();
        for (auto it = cache_.begin(); it != cache_.end(); ++it)
            if (it->second.last_used < oldest->second.last_used) oldest = it;
        if (oldest->second.handle) dispatch_.destroy(device_, oldest->second.handle, nullptr);
        cache_.erase(oldest);
    }

    void apply_group(VkDescriptorSetLayout layout, const std::vector<const VkWriteDescriptorSet*>& group) {
        scratch_key_.layout = layout;
        scratch_key_.entries.clear();
        const bool direct = group.size() == 1;
        if (!direct) slots_.resize(group.size());
        for (size_t i = 0; i < group.size(); ++i) {
            const auto& w = *group[i];
            const auto kind = descriptor_payload(w.descriptorType);
            size_t member_offset = 0;
            if (!direct) {
                switch (kind) {
                case DescriptorPayload::buffer: member_offset = offsetof(Slot, buffer); break;
                case DescriptorPayload::image: member_offset = offsetof(Slot, image); break;
                case DescriptorPayload::texel: member_offset = offsetof(Slot, texel); break;
                default: break;  // validated before calling apply_group
                }
                auto* destination = reinterpret_cast<unsigned char*>(&slots_[i]) + member_offset;
                std::memcpy(destination, descriptor_data(w, kind), descriptor_data_size(kind));
            }
            scratch_key_.entries.push_back({w.dstBinding, w.dstArrayElement, w.descriptorCount, w.descriptorType,
                                            direct ? 0 : i * sizeof(Slot) + member_offset,
                                            direct ? descriptor_data_size(kind) : sizeof(Slot)});
        }
        auto it = cache_.find(scratch_key_);
        if (it == cache_.end()) {
            template_entries_.clear();
            for (const auto& e : scratch_key_.entries)
                template_entries_.push_back({e.binding, e.array_element, e.count, e.type, e.offset, e.stride});
            VkDescriptorUpdateTemplateCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO};
            info.descriptorUpdateEntryCount = static_cast<uint32_t>(template_entries_.size());
            info.pDescriptorUpdateEntries = template_entries_.data();
            info.templateType = VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET;
            info.descriptorSetLayout = layout;
            VkDescriptorUpdateTemplate handle = VK_NULL_HANDLE;
            const VkResult result = dispatch_.create(device_, &info, nullptr, &handle);
            if (result != VK_SUCCESS) handle = VK_NULL_HANDLE;
            // Also cache failures so a broken/unsupported driver is not called again on every draw.
            evict_if_full();
            it = cache_.emplace(scratch_key_, Cached{handle, 0}).first;
        }
        it->second.last_used = ++clock_;
        if (!it->second.handle) return fallback_group(group);
        const void* data = direct ? descriptor_data(*group[0], descriptor_payload(group[0]->descriptorType)) : slots_.data();
        dispatch_.update(device_, group[0]->dstSet, it->second.handle, data);
    }

    VkDevice device_;
    DescriptorTemplateDispatch dispatch_;
    size_t capacity_;
    bool enabled_;
    uint64_t clock_ = 0;
    std::unordered_map<Key, Cached, KeyHash> cache_;
    std::array<std::vector<const VkWriteDescriptorSet*>, 4> groups_;
    Key scratch_key_;
    std::vector<Slot> slots_;
    std::vector<VkDescriptorUpdateTemplateEntry> template_entries_;
    std::vector<VkWriteDescriptorSet> fallback_writes_;
};

}  // namespace nx::nvn::replay
