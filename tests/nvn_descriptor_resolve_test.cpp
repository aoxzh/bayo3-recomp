// Exercises production build_sets/resolve_sets/bind_sets with fake Vulkan and resources.
#include "../runtime/src/nvn/nvn_sets.cpp"
#include <cassert>
#include <cstdio>
#include <map>
#include <type_traits>
using namespace nx;
using namespace nx::nvn;
using namespace nx::nvn::replay;
template<class T> T h(uintptr_t v) { if constexpr (std::is_pointer_v<T>) return reinterpret_cast<T>(v); else return static_cast<T>(v); }
template<class T> uintptr_t bits(T v) { if constexpr (std::is_pointer_v<T>) return reinterpret_cast<uintptr_t>(v); else return static_cast<uintptr_t>(v); }
namespace test {
using Key = std::pair<uintptr_t, uint32_t>;
using Payload = std::pair<VkDescriptorType, std::vector<uint8_t>>;
std::map<Key, Payload> updates;
std::map<uintptr_t, std::vector<VkDescriptorUpdateTemplateEntry>> templates;
uint32_t allocations = 0, ordinary_calls = 0, template_calls = 0, creates = 0, upload_seq = 0;
uint32_t dynamic_bias = 0, dummy_views = 0, fail_allocations = 0;
VkDescriptorSetLayout fail_alloc_layout = VK_NULL_HANDLE;
uintptr_t next_template = 8000, next_layout = 300;
VkDescriptorSetLayout failed_layout = VK_NULL_HANDLE;
std::map<uint64_t, Object*> objects;
DescriptorPool texture_pool;
Texture texture_a, texture_b;
GpuImage fake_image;
bool ready = true, dummy_is_ready = true, pass_written = false, pending_clear = false;
unsigned ready_calls = 0;
std::vector<uint64_t> sampled_views, storage_views;
std::vector<const uint8_t*> read_sources, written_sources;
std::vector<uint8_t> read_values;
std::vector<char> events;
uint32_t dynamic_count = 0, dynamic_values[64] = {};
alignas(8) uint8_t guest[64] = {};
uint8_t upload_storage[1 << 20] = {};
uint64_t upload_head = 0, host_allocations = 0, generic_calls = 0;
bool wrap_next_upload = false, real_reads = false;
void record(VkDescriptorSet set, uint32_t binding, VkDescriptorType type, const void* p) {
    const size_t n = descriptor_data_size(descriptor_payload(type)); assert(set && p && n);
    const auto* bytes = static_cast<const uint8_t*>(p); updates[{bits(set), binding}] = {type, {bytes, bytes + n}};
}
VKAPI_ATTR void VKAPI_CALL update_sets(VkDevice, uint32_t count, const VkWriteDescriptorSet* writes, uint32_t copies, const VkCopyDescriptorSet*) {
    assert(!copies); ++ordinary_calls;
    for (uint32_t i = 0; i < count; ++i) {
        const auto& w = writes[i]; assert(w.descriptorCount == 1);
        record(w.dstSet, w.dstBinding, w.descriptorType, descriptor_data(w, descriptor_payload(w.descriptorType)));
    }
}
VKAPI_ATTR VkResult VKAPI_CALL create_template(VkDevice, const VkDescriptorUpdateTemplateCreateInfo* ci, const VkAllocationCallbacks*, VkDescriptorUpdateTemplate* out) {
    ++creates; if (ci->descriptorSetLayout == failed_layout) return VK_ERROR_OUT_OF_HOST_MEMORY;
    *out = h<VkDescriptorUpdateTemplate>(++next_template);
    templates[bits(*out)] = {ci->pDescriptorUpdateEntries, ci->pDescriptorUpdateEntries + ci->descriptorUpdateEntryCount}; return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL update_template(VkDevice, VkDescriptorSet set, VkDescriptorUpdateTemplate t, const void* data) {
    ++template_calls;
    for (const auto& e : templates.at(bits(t))) { assert(e.descriptorCount == 1); record(set, e.dstBinding, e.descriptorType, static_cast<const uint8_t*>(data) + e.offset); }
}
VKAPI_ATTR void VKAPI_CALL destroy_template(VkDevice, VkDescriptorUpdateTemplate t, const VkAllocationCallbacks*) { templates.erase(bits(t)); }
VKAPI_ATTR void VKAPI_CALL properties(VkPhysicalDevice, VkPhysicalDeviceProperties* p) { p->apiVersion = VK_API_VERSION_1_1; }
VKAPI_ATTR VkResult VKAPI_CALL create_set_layout(VkDevice, const VkDescriptorSetLayoutCreateInfo*, const VkAllocationCallbacks*, VkDescriptorSetLayout* out) {
    *out = h<VkDescriptorSetLayout>(++next_layout); return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL create_pipeline_layout(VkDevice, const VkPipelineLayoutCreateInfo* ci, const VkAllocationCallbacks*, VkPipelineLayout* out) {
    assert(ci->setLayoutCount == 4); *out = h<VkPipelineLayout>(++next_layout); return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL bind(VkCommandBuffer, VkPipelineBindPoint, VkPipelineLayout, uint32_t first, uint32_t count, const VkDescriptorSet* sets, uint32_t n, const uint32_t* offsets) {
    for (uint32_t i = 0; i < count; ++i) assert(sets[i]);
    if (first == 0) { dynamic_count = n; for (uint32_t i = 0; i < n; ++i) dynamic_values[i] = offsets[i]; } else assert(n == 0);
}
struct Expected { uint8_t set; uint32_t binding; Payload payload; };
std::vector<Expected> snapshot() {
    std::vector<Expected> out;
    if (set_cache_on()) {
        // No VkWriteDescriptorSet exists yet. The pending records name the current, fully resolved payloads.
        assert(g_sets.writes.empty());
        for (const PlannedWrite& p : g_sets.pending) {
            const void* data = p.set < 2 ? static_cast<const void*>(&g_sets.buffers[p.payload])
                                         : static_cast<const void*>(&g_sets.images[p.payload]);
            const size_t n = descriptor_data_size(descriptor_payload(p.type));
            assert(n);
            const auto* bytes = static_cast<const uint8_t*>(data);
            out.push_back({p.set, p.binding, {p.type, {bytes, bytes + n}}});
        }
        return out;
    }
    size_t b = 0, im = 0; assert(g_sets.writes.size() == g_sets.set_index.size());
    for (size_t i = 0; i < g_sets.writes.size(); ++i) {
        const auto& w = g_sets.writes[i]; const bool buffer = g_sets.set_index[i] < 2;
        const void* data = buffer ? static_cast<const void*>(&g_sets.buffers[b++]) : static_cast<const void*>(&g_sets.images[im++]);
        assert(data == descriptor_data(w, descriptor_payload(w.descriptorType)));
        const size_t n = descriptor_data_size(descriptor_payload(w.descriptorType)); const auto* p = static_cast<const uint8_t*>(data);
        out.push_back({g_sets.set_index[i], w.dstBinding, {w.descriptorType, {p, p + n}}});
    }
    assert(b == g_sets.buffers.size() && im == g_sets.images.size()); return out;
}
void verify(const std::vector<Expected>& expected, int only_set = -1) {
    size_t count = 0; for (const auto& e : expected) {
        if (only_set >= 0 && e.set != only_set) continue;
        ++count; auto it = updates.find({bits(g_sets.sets[e.set]), e.binding}); assert(it != updates.end() && it->second == e.payload);
    } assert(updates.size() == count);
}
void build(ShaderInfo& shader, const ProgramLayout& layout) {
    upload_seq = 0; events.clear(); updates.clear();
    gather_textures({&shader});
    build_sets(h<VkCommandBuffer>(1), {&shader}, layout); assert(!events.empty() && events.front() == 'w');
}
void bind_now(const ProgramLayout& layout) {
    bind_sets(h<VkCommandBuffer>(1), VK_PIPELINE_BIND_POINT_GRAPHICS, layout);
    assert(dynamic_count == 2 && dynamic_values[0] == 256 && dynamic_values[1] == 0);
}
void configure(ShaderInfo& s, unsigned buffers, unsigned textures) {
    s = {}; s.stage = ShaderStage::Vertex; s.constants.assign(16, 0x5a); auto& bs = s.refl.bindings; bs.reserve(buffers + textures + 4);
    DescriptorBinding dynamic{}; dynamic.set = 0; dynamic.binding = 1; dynamic.type = DescriptorType::UniformBuffer; dynamic.nvn.source = NvnSource::ConstantData; bs.push_back(dynamic);
    DescriptorBinding storage{}; storage.set = 1; storage.binding = 10; storage.type = DescriptorType::StorageBuffer; storage.nvn.source = NvnSource::StorageBuffer; storage.nvn.index = 0; bs.push_back(storage);
    for (unsigned i = 0; i < buffers; ++i) { DescriptorBinding b{}; b.set = i % 2; b.binding = 100 + i; b.type = DescriptorType::UniformBuffer; b.nvn.source = NvnSource::ConstantData; bs.push_back(b); }
    for (unsigned i = 0; i < textures; ++i) { DescriptorBinding b{}; b.set = 2; b.binding = 100 + i; b.type = DescriptorType::CombinedImageSampler; b.image.dim = uint8_t(i % 4); bs.push_back(b); }
    DescriptorBinding image{}; image.set = 3; image.binding = 1; image.type = DescriptorType::StorageImage; bs.push_back(image);
}
}
namespace nx { void log(const char*, ...) {} }
namespace nx::nvn {
ObjectsLock& objects_lock() { static ObjectsLock lock; return lock; }
Object* find_object(uint64_t address, uint64_t) { auto it = test::objects.find(address); return it == test::objects.end() ? nullptr : it->second; }
Texture::~Texture() = default;
bool resolve_cache_on() { return false; }
bool pool_entry_at(uint64_t, uint32_t, uint64_t, PoolEntry&) { return false; }
bool sampler_entry_at(uint64_t, uint32_t, uint64_t, SamplerSlot&) { return false; }
bool mark_empty_old() { return false; }
VkDevice vk_device() { return h<VkDevice>(1); }
VkPhysicalDevice vk_physical() { return h<VkPhysicalDevice>(2); }
const VkPhysicalDeviceFeatures& vk_features() {
    static VkPhysicalDeviceFeatures f = [] { VkPhysicalDeviceFeatures v{}; v.robustBufferAccess = VK_TRUE; return v; }();
    return f;
}
VkDescriptorSet descriptor_set(VkDescriptorSetLayout layout) {
    assert(layout);
    if (layout == test::fail_alloc_layout && test::fail_allocations) { --test::fail_allocations; return VK_NULL_HANDLE; }
    return h<VkDescriptorSet>(1000 + ++test::allocations);
}
Upload upload_alloc(uint64_t size, uint64_t align) {
    assert(size <= sizeof(test::upload_storage) && align && !(align & (align - 1)));
    uint64_t start = (test::upload_head + align - 1) & ~(align - 1);
    if (test::wrap_next_upload || start > sizeof(test::upload_storage) - size) {
        // The production synchronous and asynchronous ring allocators invalidate dedup before reusing ring bytes.
        replay::dedup_reset();
        std::memset(test::upload_storage, 0xcc, sizeof(test::upload_storage));
        test::upload_head = start = 0;
        test::wrap_next_upload = false;
    }
    test::upload_head = start + size;
    ++test::host_allocations;
    return {h<VkBuffer>(777), start, test::upload_storage + start, false, true};
}
SupportBuffer support_buffer_defaults() { return {}; }
void support_buffer_set_viewport(SupportBuffer& sb, float width, float height, bool) {
    sb.viewport_size[0] = width; sb.viewport_size[1] = height;
}
VkSampler sampler_for(const SamplerInfo&, bool) { return h<VkSampler>(44); }
VkImageView dummy_view(VkCommandBuffer, const ImageShape& s) { ++test::dummy_views; return h<VkImageView>(100 + s.dim); }
VkImageView sampled_view(VkCommandBuffer, uint64_t texture, const ImageShape&, const ViewInfo*, VkImageLayout*) { test::sampled_views.push_back(texture); return h<VkImageView>(200 + texture); }
VkImageView storage_view(VkCommandBuffer, uint64_t texture, const ImageShape&, const ViewInfo*) { test::storage_views.push_back(texture); return h<VkImageView>(300 + texture); }
bool sampled_ready(uint64_t) { ++test::ready_calls; return test::ready; }
bool dummy_ready(const ImageShape&) { return test::dummy_is_ready; }
GpuImage* image_of(uint64_t) { return &test::fake_image; }
bool clears_pending_image(const GpuImage*) { return test::pending_clear; }
uint64_t submit_start_ns() { return 12345; }
uint64_t written_storage_bindings(const ShaderInfo*) { return (1ull << 10) | (1ull << 11); }
uint64_t dynamic_uniform_bindings(const ShaderInfo*) { return 0; }
bool g_uw_wide = false;
void uw_span(int32_t&, int32_t&, uint32_t) {}
}
namespace nx::nvn::replay {
State g_state;
int program_stage_slot(ShaderStage stage) {
    switch (stage) { case ShaderStage::Vertex: return 0; case ShaderStage::Fragment: return 1;
        case ShaderStage::Compute: return 5; case ShaderStage::TessellationControl: return 3;
        case ShaderStage::TessellationEvaluation: return 4; default: return -1; }
}
const uint8_t* guest_bytes(uint64_t address) { return test::guest + (address == 124 ? 16 : 0); }
bool pass_wrote(const GpuImage*) { return test::pass_written; }
Upload descriptor_test_real_dedup_upload(const uint8_t*, uint64_t, uint64_t, uint64_t, bool = false);
Upload dedup_upload(const uint8_t* bytes, uint64_t size, uint64_t padded, uint64_t align, bool) {
    ++test::generic_calls;
    return descriptor_test_real_dedup_upload(bytes, size, padded, align);
}
bool written_separate() { return false; }
Upload written_bind(VkCommandBuffer, const uint8_t* p, uint64_t, uint64_t, bool) { test::events.push_back('w'); test::written_sources.push_back(p); return {h<VkBuffer>(66), 64, nullptr}; }
Upload read_upload(VkCommandBuffer, const uint8_t* p, uint64_t size, uint64_t padded, uint64_t align) {
    test::events.push_back('r'); test::read_sources.push_back(p); test::read_values.push_back(p ? *p : 0);
    // Production read_upload forwards empty inputs to real dedup_upload; selected tests also use it for real data.
    if (!p || !size || test::real_reads) return dedup_upload(p, size, padded, align);
    const uint32_t n = ++test::upload_seq;
    return {h<VkBuffer>(33), 256ull * n + (n == 1 ? test::dynamic_bias : 0), nullptr};
}
bool written_overlaps(const uint8_t*, uint64_t) { return false; }
void written_count_bind() {}
}
namespace test {
// Exercise real program_layout/gather_textures/build_sets across two immutable stage records. Only Vulkan and
// actual resource work are mocked. Mutable handles and ranges must be resolved anew on every draw.
void mixed_stages() {
    static ShaderInfo vertex, fragment, alternate;
    vertex.stage = ShaderStage::Vertex;
    fragment.stage = ShaderStage::Fragment;
    auto binding = [](uint32_t set, uint32_t slot, DescriptorType type, NvnSource source, int index) {
        DescriptorBinding b{}; b.set = set; b.binding = slot; b.type = type; b.nvn.source = source; b.nvn.index = index; return b;
    };
    vertex.refl.bindings = {
        binding(0, 1, DescriptorType::UniformBuffer, NvnSource::UniformBlock, 0),
        binding(1, 10, DescriptorType::StorageBuffer, NvnSource::StorageBuffer, 0),
        binding(2, 2, DescriptorType::CombinedImageSampler, NvnSource::SamplerBinding, 0),
    };
    auto uniform_handle = binding(2, 3, DescriptorType::CombinedImageSampler, NvnSource::HandleInUniform, 0);
    uniform_handle.nvn.bank = 3; uniform_handle.nvn.word = 1;
    fragment.refl.bindings = {
        binding(0, 2, DescriptorType::UniformBuffer, NvnSource::UniformBlock, 0),
        binding(1, 11, DescriptorType::StorageBuffer, NvnSource::StorageBuffer, 0), uniform_handle,
        binding(3, 4, DescriptorType::StorageImage, NvnSource::ImageBinding, 1),
    };
    alternate = fragment; alternate.refl.bindings.pop_back(); alternate.refl.bindings[2].binding = 5;
    const std::vector<const ShaderInfo*> pair{&vertex, &fragment};
    const auto& layout = program_layout(pair);
    assert(&program_layout(pair) == &layout);
    assert(layout.buffers.size() == 4 && layout.storage.size() == 2 && layout.sampled.size() == 2 && layout.images.size() == 1);
    assert(layout.buffers[0].binding == &vertex.refl.bindings[0] && layout.buffers[1].binding == &vertex.refl.bindings[1]);
    assert(layout.buffers[2].binding == &fragment.refl.bindings[0] && layout.storage[1].binding == &fragment.refl.bindings[1]);
    assert(layout.sampled[0].stage == 0 && layout.sampled[1].stage == 1 && layout.images[0].stage == 1);
    const auto& reversed = program_layout({&fragment, &vertex});
    assert(&reversed != &layout && reversed.sampled[0].shader == &fragment && reversed.sampled[1].shader == &vertex);
    const auto& replaced = program_layout({&vertex, &alternate});
    assert(&replaced != &layout && replaced.images.empty() && replaced.sampled[1].binding->binding == 5);
    assert(&program_layout(pair) == &layout);

    objects[9000] = &texture_pool; objects[9100] = &texture_a; objects[9200] = &texture_b;
    texture_pool.textures[1] = 9100; texture_pool.textures[2] = 9200;
    g_state.texture_pool = 9000;
    g_state.textures[0][0] = 1;
    g_state.images[1][1] = 2;
    g_state.uniforms[0][0] = {123, 16}; g_state.uniforms[1][0] = {124, 16};
    g_state.storage[0][0] = {123, 16}; g_state.storage[1][0] = {124, 16};
    uint32_t handle = 2; std::memcpy(guest + 20, &handle, sizeof(handle));
    sets_begin_submit();
    assert(gather_textures(pair) & kNeedBreak);  // storage image requires a pass break
    assert(g_textures.handles == std::vector<uint64_t>({1, 2}));
    assert(g_textures.resolved[0].texture == 9100 && g_textures.resolved[1].texture == 9200);
    assert(texture_a.last_sampled == 12345 && texture_b.last_sampled == 12345);
    events.clear(); written_sources.clear(); read_sources.clear(); sampled_views.clear(); storage_views.clear();
    build_sets(h<VkCommandBuffer>(1), pair, layout);
    assert(events.size() >= 2 && events[0] == 'w' && events[1] == 'w');
    assert(written_sources.size() >= 2 && written_sources[0] == guest && written_sources[1] == guest + 16);
    assert(read_sources == std::vector<const uint8_t*>({guest, guest + 16}));
    assert(sampled_views == std::vector<uint64_t>({9100, 9200}) && storage_views == std::vector<uint64_t>({9200}));
    auto first = snapshot();
    assert(first.size() == 7 && first[0].binding == 1 && first[1].binding == 10 && first[2].binding == 2 &&
           first[3].binding == 11 && first[4].binding == 2 && first[5].binding == 3 && first[6].binding == 4);

    // Same layout, new stage resources: no handles, guest addresses, views or hazards may be baked into it.
    g_state.textures[0][0] = 2; g_state.images[1][1] = 1;
    g_state.uniforms[0][0] = {124, 16}; g_state.storage[0][0] = {124, 16};
    handle = 1; std::memcpy(guest + 20, &handle, sizeof(handle));
    assert(gather_textures(pair) & kNeedBreak);
    assert(g_textures.handles == std::vector<uint64_t>({2, 1}));
    events.clear(); written_sources.clear(); read_sources.clear(); sampled_views.clear(); storage_views.clear();
    build_sets(h<VkCommandBuffer>(1), pair, layout);
    assert(written_sources[0] == guest + 16 && read_sources[0] == guest + 16);
    assert(sampled_views == std::vector<uint64_t>({9200, 9100}) && storage_views == std::vector<uint64_t>({9100}));
    snapshot();

    // Repeated real draws must repeat guest reads, alias prepass, readiness checks and sampled/storage view work,
    // even when the descriptor keys still hit. Changing one texture must update only its set.
    sets_begin_submit();
    auto draw = [&] {
        const auto ready_before = ready_calls;
        assert(gather_textures(pair) & kNeedBreak);
        assert(ready_calls == ready_before + 2);
        events.clear(); written_sources.clear(); read_sources.clear(); read_values.clear();
        sampled_views.clear(); storage_views.clear(); updates.clear(); upload_seq = 0;
        build_sets(h<VkCommandBuffer>(1), pair, layout);
        assert(events.size() >= 2 && events[0] == 'w' && events[1] == 'w');
        assert(written_sources.size() >= 2 && written_sources[0] == guest + 16 && written_sources[1] == guest + 16);
        assert(read_sources == std::vector<const uint8_t*>({guest + 16, guest + 16}));
        assert(storage_views == std::vector<uint64_t>({9100}) && sampled_views.size() == 2);
        auto expected = snapshot();
        bind_sets(h<VkCommandBuffer>(1), VK_PIPELINE_BIND_POINT_GRAPHICS, layout);
        return expected;
    };
    auto expected = draw(); verify(expected);
    const auto before_hit = allocations;
    guest[16] ^= 0x77;
    expected = draw();
    assert(read_values.size() == 2 && read_values[0] == guest[16] && read_values[1] == guest[16]);
    if (set_cache_on()) assert(allocations == before_hit && updates.empty() && g_sets.writes.empty());
    else verify(expected);
    g_state.textures[0][0] = 1;
    const auto before_changed_texture = allocations;
    expected = draw();
    assert(sampled_views == std::vector<uint64_t>({9100, 9100}));
    if (set_cache_on()) {
        assert(allocations == before_changed_texture + 1);
        verify(expected, 2);
        for (const auto& w : g_sets.writes) assert(w.dstSet == g_sets.sets[2]);
    } else verify(expected);

    // No storage image in this layout: each changing readiness/hazard condition independently requests a break.
    const std::vector<const ShaderInfo*> vertex_only{&vertex};
    ready = true; pass_written = false; pending_clear = false;
    const auto previous_calls = ready_calls;
    assert(gather_textures(vertex_only) == 0 && ready_calls == previous_calls + 1);
    ready = false; assert(gather_textures(vertex_only) & kNeedBreak);
    ready = true; pass_written = true; assert(gather_textures(vertex_only) & kNeedBreak);
    pass_written = false; pending_clear = true;
    assert((gather_textures(vertex_only) & (kNeedBreak | kSampledPending)) == (kNeedBreak | kSampledPending));
    pending_clear = false;
}

// Unsupported descriptor shapes must never become cache hits. These synthetic texel descriptors deliberately
// retain the old builder's buffer pointer, so inspect the fallback without submitting an invalid Vulkan write.
void unsupported_payload() {
    static ShaderInfo shader;
    shader.stage = ShaderStage::Vertex;
    shader.constants.assign(16, 1);
    DescriptorBinding b{}; b.set = 0; b.binding = 9; b.type = DescriptorType::UniformTexelBuffer;
    b.nvn.source = NvnSource::ConstantData;
    shader.refl.bindings = {b};
    const auto& layout = program_layout({&shader});
    sets_begin_submit();
    const auto before = allocations;
    for (int i = 0; i < 2; ++i) {
        upload_seq = 0;
        gather_textures({&shader});
        build_sets(h<VkCommandBuffer>(1), {&shader}, layout);
        if (set_cache_on()) {
            assert(!g_sets.plans[0].cacheable && g_sets.writes.empty());
            resolve_sets(layout);
        }
        assert(g_sets.writes.size() == 1 && g_sets.writes[0].dstSet &&
               g_sets.writes[0].descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER &&
               g_sets.writes[0].pBufferInfo && !descriptor_write_cacheable(g_sets.writes[0]));
    }
    const uint32_t expected_allocations = draw_slow(kSlowEmptySets) ? 8 : 2;
    assert(allocations == before + expected_allocations);
    if (set_cache_on()) assert(g_set_used == 0);
}

void cache_edge_cases(ShaderInfo& shader, const ProgramLayout& layout) {
    if (!set_cache_on()) return;

    // A failed set allocation drops that set's writes, never inserts it, and retries next draw.
    sets_begin_submit();
    fail_alloc_layout = layout.sets[1]; fail_allocations = 1;
    auto before = allocations;
    build(shader, layout); auto expected = snapshot(); bind_now(layout);
    assert(!g_sets.sets[1] && allocations == before + 3);
    size_t updated = 0;
    for (const auto& e : expected) {
        if (e.set == 1) continue;
        ++updated;
        auto it = updates.find({bits(g_sets.sets[e.set]), e.binding});
        assert(it != updates.end() && it->second == e.payload);
    }
    assert(updates.size() == updated);
    before = allocations;
    build(shader, layout); expected = snapshot(); bind_now(layout);
    assert(allocations == before + 1 && g_sets.sets[1]);
    verify(expected, 1);
    for (const auto& w : g_sets.writes) assert(w.dstSet == g_sets.sets[1]);
    fail_alloc_layout = VK_NULL_HANDLE;

    // Force the same hash/layout/count with a different stored key: full equality must reject the collision.
    sets_begin_submit();
    build(shader, layout); snapshot(); bind_now(layout);
    const auto& plan = g_sets.plans[2];
    CachedSet* found = nullptr;
    for (uint32_t probe = uint32_t(plan.hash >> 32);; ++probe) {
        auto& entry = g_set_cache[probe & (kSetSlots - 1)];
        assert(entry.generation == g_set_generation);
        if (entry.hash == plan.hash && entry.layout == layout.sets[2]) { found = &entry; break; }
    }
    assert(found && found->n && found->n == plan.key.size());
    g_set_arena[found->at].a ^= 1;
    before = allocations;
    build(shader, layout); expected = snapshot(); bind_now(layout);
    assert(allocations == before + 1);
    verify(expected, 2);
    for (const auto& w : g_sets.writes) assert(w.dstSet == g_sets.sets[2]);

    // A full cache keeps allocating and updating instead of caching incomplete or invalid entries.
    sets_begin_submit(); g_set_used = kSetSlots / 2;
    before = allocations;
    build(shader, layout); expected = snapshot(); bind_now(layout); verify(expected);
    build(shader, layout); expected = snapshot(); bind_now(layout); verify(expected);
    assert(allocations == before + 8 && g_set_used == kSetSlots / 2);
    sets_begin_submit();
}

// Empty descriptors exercise real build/bind, real dedup and the fake ring (including its generation resets).
void unbound_buffers(const char* mode) {
    State saved_state = g_state;  // keep the surrounding descriptor-cache tests independent
    const bool slow = std::strcmp(mode, "slow-uploads") == 0;
    const bool diagnostics = std::strcmp(mode, "no-dedup") == 0 || std::strcmp(mode, "stats") == 0;
    const bool fast = !slow && !diagnostics;
    const VkCommandBuffer cb = h<VkCommandBuffer>(1);
    auto zero_bytes = [](uint64_t offset, uint64_t count) {
        assert(offset % 256 == 0 && offset + count <= sizeof(upload_storage));
        assert(std::all_of(upload_storage + offset, upload_storage + offset + count, [](uint8_t b) { return b == 0; }));
    };
    auto next_submit = [&] { sets_begin_submit(); dedup_reset(); upload_head = 256; };

    static ShaderInfo shader;
    shader.stage = ShaderStage::Vertex;
    DescriptorBinding ubo{}; ubo.set = 0; ubo.binding = 0; ubo.type = DescriptorType::UniformBuffer;
    ubo.nvn.source = NvnSource::UniformBlock; ubo.nvn.index = 0;
    DescriptorBinding dynamic = ubo; dynamic.binding = 1; dynamic.nvn.index = 1;
    DescriptorBinding storage{}; storage.set = 1; storage.binding = 7; storage.type = DescriptorType::StorageBuffer;
    storage.nvn.source = NvnSource::StorageBuffer; storage.nvn.index = 0;  // not in written_storage_bindings
    shader.refl.bindings = {ubo, dynamic, storage};
    ProgramLayout layout = program_layout({&shader}); layout.dynamic = 1ull << 1;
    g_state.uniforms[0][0] = {}; g_state.uniforms[0][1] = {}; g_state.storage[0][0] = {};
    next_submit();

    auto draw = [&](bool bound = false) {
        updates.clear();
        gather_textures({&shader});
        build_sets(cb, {&shader}, layout);
        auto expected = snapshot();
        assert(g_sets.buffers.size() == 3);
        auto first = g_sets.buffers[0], dyn = g_sets.buffers[1], third = g_sets.buffers[2];
        assert(first.buffer == h<VkBuffer>(777) && dyn.buffer == first.buffer && third.buffer == first.buffer);
        assert(first.range == pad_min(0x1000) && dyn.range == pad_min(0x1000) && third.range == pad_min(0x10000));
        assert(dyn.offset == 0);
        const uint32_t dyn_offset = g_sets.dynamic_offset[1];
        zero_bytes(dyn_offset, dyn.range);
        if (bound) {
            for (auto info : {first, third}) {
                assert(info.offset % 256 == 0 && info.offset + info.range <= sizeof(upload_storage));
                assert(std::memcmp(upload_storage + info.offset, guest, 16) == 0);
                assert(std::all_of(upload_storage + info.offset + 16, upload_storage + info.offset + info.range,
                                   [](uint8_t b) { return b == 0; }));
            }
        } else {
            zero_bytes(first.offset, first.range); zero_bytes(third.offset, third.range);
        }
        bind_sets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout);
        assert(dynamic_count == 1 && dynamic_values[0] == dyn_offset);
        if (!updates.empty()) verify(expected);
        return std::array<uint64_t, 3>{first.offset, dyn_offset, third.offset};
    };
    auto before_alloc = host_allocations, before_generic = generic_calls;
    auto first = draw();
    assert(host_allocations == before_alloc + (fast ? (first[0] == first[2] ? 1 : 2) : 3));
    assert(generic_calls == before_generic + (fast ? (first[0] == first[2] ? 1 : 2) : 3));
    if (fast) assert(first[0] == first[1]);
    const VkDescriptorSet set0 = g_sets.sets[0], set1 = g_sets.sets[1];
    before_alloc = host_allocations; before_generic = generic_calls;
    const auto sets_before = allocations;
    auto second = draw();
    if (fast) {
        assert(second == first && host_allocations == before_alloc && generic_calls == before_generic);
        if (set_cache_on()) assert(allocations == sets_before && updates.empty() && g_sets.sets[0] == set0 && g_sets.sets[1] == set1);
    } else {
        assert(host_allocations == before_alloc + 3 && generic_calls == before_generic + 3);
        assert(first[0] != second[0]);
    }

    // The current guest bytes remain authoritative on null -> bound -> modified -> null transitions.
    // Guest writes invalidate dedup in production; use that same notification here.
    real_reads = true;
    g_state.uniforms[0][0] = {123, 16}; g_state.storage[0][0] = {123, 16};
    std::memset(guest, 0x35, 16);
    auto bound = draw(true);
    assert(bound[0] != first[0] && bound[2] != first[2]);
    std::memset(guest, 0x79, 16); dedup_invalidate(guest, 16);
    auto changed = draw(true);
    assert(changed[0] != bound[0] && changed[2] != bound[2]);
    g_state.uniforms[0][0] = {}; g_state.storage[0][0] = {};
    auto unbound_again = draw();
    if (fast) {
        assert(unbound_again == first);
        if (set_cache_on()) assert(g_sets.sets[0] == set0 && g_sets.sets[1] == set1 && updates.empty());
    }
    real_reads = false;

    // Exact size and 256-byte alignment: a larger request must not reuse a smaller allocation; the cache is bounded.
    next_submit();
    auto empty = [&](uint64_t n) { auto b = buffer_of(cb, nullptr, 0, n); zero_bytes(b.offset, b.range); return b; };
    auto small = empty(16);
    auto large = empty(4096);
    assert(small.range == 16 && large.range == 4096 && small.offset != large.offset);
    before_alloc = host_allocations;
    auto small_again = empty(16);
    assert((small_again.offset == small.offset) == fast);
    assert(host_allocations == before_alloc + (fast ? 0 : 1));
    if (fast) {
        next_submit();
        auto first_slot = empty(16);
        empty(32); empty(48); empty(64); empty(80);  // fifth distinct size evicts the first metadata slot
        before_alloc = host_allocations;
        auto evicted = empty(16);
        assert(evicted.offset != first_slot.offset && host_allocations == before_alloc + 1);
    }

    // A non-null zero-length input and a null nonzero-length input retain their original fresh allocation behavior.
    next_submit();
    before_alloc = host_allocations;
    auto not_unbound = buffer_of(cb, guest, 0, 16);
    auto not_unbound_again = buffer_of(cb, guest, 0, 16);
    auto null_nonzero = buffer_of(cb, nullptr, 16, 16);
    auto null_nonzero_again = buffer_of(cb, nullptr, 16, 16);
    assert(host_allocations == before_alloc + 4 && not_unbound.offset != not_unbound_again.offset &&
           null_nonzero.offset != null_nonzero_again.offset);
    zero_bytes(not_unbound.offset, 16); zero_bytes(null_nonzero.offset, 16);

    // Invalidation on a same-submit wrap, a wrap inside this allocation, and an explicit submit reset.
    next_submit(); small = empty(16);
    auto generation = dedup_generation();
    wrap_next_upload = true; (void)upload_alloc(16, 16);  // fake ring poisons old bytes and advances generation
    assert(dedup_generation() != generation);
    before_generic = generic_calls; small = empty(16);
    assert(generic_calls == before_generic + 1);
    if (fast) { before_generic = generic_calls; assert(empty(16).offset == small.offset && generic_calls == before_generic); }
    dedup_reset(); generation = dedup_generation(); wrap_next_upload = true;
    before_generic = generic_calls; small = empty(16);
    assert(dedup_generation() != generation && generic_calls == before_generic + 1 && !wrap_next_upload);
    before_generic = generic_calls; auto after_wrap = empty(16);
    assert(generic_calls == before_generic + (fast ? 0 : 1));
    if (fast) assert(after_wrap.offset == small.offset);
    before_alloc = host_allocations; sets_begin_submit();  // even if the caller has not advanced dedup yet
    (void)empty(16);
    assert(host_allocations == before_alloc + 1);

    // An unbound writable storage buffer is private even when a same-size zero upload already exists. Simulate a
    // GPU write into it, then check that the read-only bytes and the next writable allocation are still zero.
    next_submit();
    static ShaderInfo compute;
    compute.stage = ShaderStage::Compute;
    DescriptorBinding read = storage; read.binding = 7; read.nvn.index = 0;
    DescriptorBinding write = storage; write.binding = 10; write.nvn.index = 1;
    DescriptorBinding write2 = storage; write2.binding = 11; write2.nvn.index = 2;
    compute.refl.bindings = {read, write, write2};
    const ProgramLayout& compute_layout = program_layout({&compute});
    g_state.storage[5][0] = {}; g_state.storage[5][1] = {}; g_state.storage[5][2] = {};
    gather_textures({&compute}); build_sets(cb, {&compute}, compute_layout);
    assert(g_sets.buffers.size() == 3);
    auto read_info = g_sets.buffers[0], write_info = g_sets.buffers[1], write_info2 = g_sets.buffers[2];
    assert(read_info.buffer == write_info.buffer && write_info.buffer == write_info2.buffer &&
           read_info.offset != write_info.offset && read_info.offset != write_info2.offset &&
           write_info.offset != write_info2.offset && write_info.range == 0x10000 && write_info2.range == 0x10000);
    zero_bytes(read_info.offset, read_info.range); zero_bytes(write_info.offset, write_info.range);
    zero_bytes(write_info2.offset, write_info2.range);
    std::memset(upload_storage + write_info.offset, 0x7b, write_info.range);
    std::memset(upload_storage + write_info2.offset, 0xa5, write_info2.range);
    gather_textures({&compute}); build_sets(cb, {&compute}, compute_layout);
    auto read_after = g_sets.buffers[0], write_after = g_sets.buffers[1], write_after2 = g_sets.buffers[2];
    assert(read_after.offset != write_after.offset && read_after.offset != write_after2.offset &&
           write_after.offset != write_after2.offset && write_after.offset != write_info.offset &&
           write_after2.offset != write_info2.offset);
    if (fast) assert(read_after.offset == read_info.offset);
    zero_bytes(read_after.offset, read_after.range); zero_bytes(write_after.offset, write_after.range);
    zero_bytes(write_after2.offset, write_after2.range);
    g_state = std::move(saved_state);
}

// The existing diagnostic intentionally supplies ones, not the new zero cache, for an unbound uniform.
void unbound_ones() {
    static ShaderInfo shader;
    shader.stage = ShaderStage::Vertex;
    DescriptorBinding ubo{}; ubo.set = 0; ubo.binding = 0; ubo.type = DescriptorType::UniformBuffer;
    ubo.nvn.source = NvnSource::UniformBlock; ubo.nvn.index = 0;
    shader.refl.bindings = {ubo};
    const ProgramLayout& layout = program_layout({&shader});
    g_state.uniforms[0][0] = {};
    sets_begin_submit(); dedup_reset(); upload_head = 256;
    for (int draw = 0; draw < 2; ++draw) {
        gather_textures({&shader}); build_sets(h<VkCommandBuffer>(1), {&shader}, layout);
        assert(g_sets.buffers.size() == 1);
        const auto info = g_sets.buffers[0];
        assert(info.buffer == h<VkBuffer>(777) && info.range == 0x1000 && info.offset + info.range <= sizeof(upload_storage));
        for (size_t at = 0; at < info.range; at += sizeof(float)) {
            float value; std::memcpy(&value, upload_storage + info.offset + at, sizeof(value));
            assert(value == 1.0f);
        }
        updates.clear(); auto expected = snapshot();
        bind_sets(h<VkCommandBuffer>(1), VK_PIPELINE_BIND_POINT_GRAPHICS, layout); verify(expected);
    }
}

// Exercise both real descriptor construction and real dedup. Only the Vulkan calls and ring allocation are faked.
void host_blocks(const char* mode) {
    const bool slow = std::strcmp(mode, "slow-uploads") == 0;
    const bool diagnostics = std::strcmp(mode, "no-dedup") == 0 || std::strcmp(mode, "stats") == 0;
    const bool fast = !slow && !diagnostics;
    static ShaderInfo graphics, compute;
    graphics.stage = ShaderStage::Vertex;
    DescriptorBinding support{}; support.set = 0; support.binding = 0; support.type = DescriptorType::UniformBuffer;
    support.nvn.source = NvnSource::SupportBuffer;
    DescriptorBinding constants = support; constants.binding = 1; constants.nvn.source = NvnSource::DriverConstants;
    graphics.refl.bindings = {support, constants};
    compute.stage = ShaderStage::Compute; compute.refl.bindings = graphics.refl.bindings;
    ProgramLayout graphics_layout = program_layout({&graphics}); graphics_layout.dynamic = 3;
    ProgramLayout compute_layout = program_layout({&compute}); compute_layout.dynamic = 3;
    g_state.has_viewport = true; g_state.viewport[2] = 1280; g_state.viewport[3] = 720;
    g_state.storage[0][0] = {100, 64};
    sets_begin_submit(); dedup_reset(); upload_head = 0;

    auto draw = [&](ShaderInfo& shader, const ProgramLayout& layout) {
        gather_textures({&shader});
        build_sets(h<VkCommandBuffer>(1), {&shader}, layout);
        auto payloads = snapshot();
        assert(g_sets.buffers.size() == 2 && payloads.size() == 2);
        assert(payloads[0].payload.first == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC &&
               payloads[1].payload.first == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC);
        assert(g_sets.buffers[0].buffer == h<VkBuffer>(777) && g_sets.buffers[0].offset == 0 &&
               g_sets.buffers[1].buffer == h<VkBuffer>(777) && g_sets.buffers[1].offset == 0);
        const uint64_t a = g_sets.dynamic_offset[0], b = g_sets.dynamic_offset[1];
        assert(a + sizeof(SupportBuffer) <= sizeof(upload_storage) && b + 0x1000 <= sizeof(upload_storage));
        SupportBuffer expected = support_buffer_defaults();
        support_buffer_set_viewport(expected, float(g_state.viewport[2]), float(g_state.viewport[3]));
        auto c0 = driver_constants(shader.stage, program_stage_slot(shader.stage));
        assert(std::memcmp(upload_storage + a, &expected, sizeof(expected)) == 0);
        assert(std::memcmp(upload_storage + b, c0.data(), c0.size()) == 0);
        bind_sets(h<VkCommandBuffer>(1), shader.stage == ShaderStage::Compute ? VK_PIPELINE_BIND_POINT_COMPUTE :
                  VK_PIPELINE_BIND_POINT_GRAPHICS, layout);
        assert(dynamic_count == 2 && dynamic_values[0] == a && dynamic_values[1] == b);
    };
    auto delta = [](uint64_t before, uint64_t expected) { assert(generic_calls == before + expected); };
    uint64_t before = generic_calls;
    draw(graphics, graphics_layout); delta(before, slow ? 0 : 2);
    if (fast) assert(g_support[0]->cached.upload.host_cached && g_constants[0]->cached.upload.host_cached);
    before = generic_calls; const uint64_t allocated = host_allocations;
    draw(graphics, graphics_layout); delta(before, diagnostics ? 2 : 0);
    if (fast) assert(host_allocations == allocated);  // both host blocks bypass generic lookup and allocation
    if (slow || std::strcmp(mode, "no-dedup") == 0) assert(host_allocations == allocated + 2);

    before = generic_calls; g_state.viewport[2] = 1600;
    draw(graphics, graphics_layout); delta(before, slow ? 0 : diagnostics ? 2 : 1);
    before = generic_calls; g_state.storage[0][0] = {200, 96};
    draw(graphics, graphics_layout); delta(before, slow ? 0 : diagnostics ? 2 : 1);
    before = generic_calls; g_state.viewport[2] = 1280; g_state.storage[0][0] = {100, 64};
    draw(graphics, graphics_layout); delta(before, diagnostics ? 2 : 0);

    // A ring wrap in an unrelated allocation during this submit invalidates both old block uploads.
    const uint64_t old_generation = dedup_generation();
    wrap_next_upload = true; (void)upload_alloc(64, 16);
    assert(dedup_generation() != old_generation);
    before = generic_calls; draw(graphics, graphics_layout); delta(before, slow ? 0 : 2);

    // A wrap inside the first block's dedup allocation must record the NEW generation, not the old one.
    g_state.viewport[2] = 1800; wrap_next_upload = true;
    const uint64_t before_wrap = dedup_generation();
    before = generic_calls; draw(graphics, graphics_layout); delta(before, slow ? 0 : 2);
    assert(dedup_generation() != before_wrap && !wrap_next_upload);
    before = generic_calls; draw(graphics, graphics_layout); delta(before, diagnostics ? 2 : 0);

    // New submit repopulates the 32 interned blocks. The 33rd distinct viewport and constants must fall back.
    sets_begin_submit(); dedup_reset(); upload_head = 0;
    before = generic_calls;
    for (unsigned i = 0; i < 33; ++i) {
        g_state.viewport[2] = int32_t(2000 + i); g_state.storage[0][0] = {5000 + i, 100 + i};
        draw(graphics, graphics_layout);
    }
    delta(before, slow ? 0 : 64);
    if (!slow) assert(g_support_used == 32 && g_constants_used == 32);

    // Compute c[0] includes uniform bindings and is deliberately rebuilt every time.
    sets_begin_submit(); dedup_reset(); upload_head = 0;
    g_state.uniforms[5][0] = {7000, 128}; g_state.storage[5][0] = {8000, 256};
    before = generic_calls; draw(compute, compute_layout); delta(before, slow ? 0 : 1);
    const uint32_t old_offset = g_sets.dynamic_offset[1];
    g_state.uniforms[5][0].address = 9000;
    before = generic_calls; draw(compute, compute_layout); delta(before, diagnostics ? 1 : 0);
    assert(g_sets.dynamic_offset[1] != old_offset);
}
}
int main(int argc, char** argv) {
    assert(argc == 2);
    const bool template_mode = std::strcmp(argv[1], "template") == 0 || std::strcmp(argv[1], "template-no-cache") == 0;
    const bool cache = set_cache_on();
    using namespace nx::vk;
    p_vkUpdateDescriptorSets = test::update_sets; p_vkCreateDescriptorUpdateTemplate = test::create_template;
    p_vkUpdateDescriptorSetWithTemplate = test::update_template; p_vkDestroyDescriptorUpdateTemplate = test::destroy_template;
    p_vkGetPhysicalDeviceProperties = test::properties; p_vkCmdBindDescriptorSets = test::bind;
    p_vkCreateDescriptorSetLayout = test::create_set_layout; p_vkCreatePipelineLayout = test::create_pipeline_layout;
    if (std::strcmp(argv[1], "ones") == 0) {
        test::unbound_ones();
        std::puts("production descriptor resolve: ones passed");
        return 0;
    }
    ShaderInfo shader; test::configure(shader, 4, 2);
    ProgramLayout layout = program_layout({&shader});
    layout.dynamic = (1ull << 1) | (1ull << 5); test::failed_layout = layout.sets[3]; g_state.storage[0][0] = {123, sizeof(test::guest)};
    sets_begin_submit();
    test::build(shader, layout); auto expected = test::snapshot(); test::bind_now(layout); test::verify(expected); assert(test::allocations == 4);
    if (template_mode) assert(test::creates == 4 && test::template_calls == 3 && test::ordinary_calls == 1); else assert(test::creates == 0 && test::template_calls == 0 && test::ordinary_calls == 1);
    VkDescriptorSet prior[4]; std::copy(std::begin(g_sets.sets), std::end(g_sets.sets), prior); const auto ordinary_before = test::ordinary_calls, template_before = test::template_calls;
    const auto views_before = test::dummy_views;
    test::build(shader, layout); expected = test::snapshot(); test::bind_now(layout);
    assert(test::dummy_views == views_before + 2);  // texture conversion and writable alias prepass still run on hits
    if (cache) { assert(test::allocations == 4 && test::updates.empty() && g_sets.writes.empty()); assert(test::ordinary_calls == ordinary_before && test::template_calls == template_before); for (int k = 0; k < 4; ++k) assert(prior[k] == g_sets.sets[k]); }
    else { assert(test::allocations == 8); test::verify(expected); }

    // Only the dynamic offset changes: the cached set remains valid, yet the new offset must be bound.
    test::dynamic_bias = 1024;
    const auto before_dynamic = test::allocations;
    test::build(shader, layout); expected = test::snapshot();
    bind_sets(h<VkCommandBuffer>(1), VK_PIPELINE_BIND_POINT_GRAPHICS, layout);
    assert(test::dynamic_count == 2 && test::dynamic_values[0] == 1280 && test::dynamic_values[1] == 0);
    if (cache) assert(test::allocations == before_dynamic && test::updates.empty() && g_sets.writes.empty());
    else { assert(test::allocations == before_dynamic + 4); test::verify(expected); }
    test::dynamic_bias = 0;

    if (cache) {
        VkDescriptorSetLayout original_set2 = layout.sets[2];
        layout.sets[2] = h<VkDescriptorSetLayout>(999); test::build(shader, layout); expected = test::snapshot(); test::bind_now(layout);
        assert(test::allocations == 5); test::verify(expected, 2);
        const auto set2_count = std::count_if(expected.begin(), expected.end(), [](const auto& e) { return e.set == 2; });
        assert(g_sets.writes.size() == size_t(set2_count));
        for (const auto& w : g_sets.writes) assert(w.dstSet == g_sets.sets[2]);
        assert(prior[0] == g_sets.sets[0] && prior[1] == g_sets.sets[1] && prior[2] != g_sets.sets[2] && prior[3] == g_sets.sets[3]); layout.sets[2] = original_set2;
    }
    const auto before_submit = test::allocations; sets_begin_submit(); test::build(shader, layout); expected = test::snapshot(); test::bind_now(layout);
    assert(test::allocations == before_submit + 4); test::verify(expected);
    // Both payload vectors grow beyond their 1024 reserve; set 2 exceeds the template entry limit.
    ShaderInfo large_shader; test::configure(large_shader, 1200, 1100);
    ProgramLayout large_layout = program_layout({&large_shader}); large_layout.dynamic = layout.dynamic;
    sets_begin_submit(); test::build(large_shader, large_layout); expected = test::snapshot();
    assert(g_sets.buffers.size() > 1024 && g_sets.images.size() > 1024); const auto fallback_before = test::ordinary_calls;
    test::bind_now(large_layout); test::verify(expected); assert(test::ordinary_calls > fallback_before);
    test::mixed_stages();
    test::host_blocks(argv[1]);
    test::unbound_buffers(argv[1]);
    test::cache_edge_cases(shader, layout);
    test::unsupported_payload();
    std::printf("production descriptor resolve: %s passed (%zu writes)\n", argv[1], expected.size());
}
