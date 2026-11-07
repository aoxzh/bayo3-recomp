// Standalone host test. Includes the production builder to inspect the create info
// and caches without a Vulkan device or game process. Built by the Python runner.
#include <cassert>
#include <cmath>
#include <cstdarg>
#include <fstream>
#include <iostream>
#include <iterator>
#include <unordered_map>
#include "../runtime/src/nvn/nvn_pipeline.cpp"

namespace {
bool extension = false;
bool dynamic_cull = false, dynamic_blend = false, use_gpl = false;
VkPhysicalDeviceFeatures features{};
std::vector<uint32_t> source;
std::vector<std::vector<uint32_t>> modules;
std::vector<nx::nvn::WarmRec> warm_fixture;
const nx::nvn::ShaderInfo* warm_shader = nullptr;
VkCullModeFlags last_cull{};
VkFrontFace last_front{};
std::vector<VkBool32> last_enables;
std::vector<VkColorBlendEquationEXT> last_equations;
std::vector<VkColorComponentFlags> last_masks;
int cull_calls = 0, blend_calls = 0;
std::atomic<int> creates[5]{}, destroys{0};
int fail_kind = 0, null_kind = 0;
std::atomic<uintptr_t> next_pipeline{1};
std::mutex gpl_log_mutex;
std::vector<std::string> gpl_logs;
void VKAPI_PTR set_cull(VkCommandBuffer, VkCullModeFlags v) { last_cull = v; ++cull_calls; }
void VKAPI_PTR set_front(VkCommandBuffer, VkFrontFace v) { last_front = v; }
void VKAPI_PTR set_enable(VkCommandBuffer, uint32_t first, uint32_t n, const VkBool32* v) {
    assert(first == 0); last_enables.assign(v, v + n); ++blend_calls;
}
void VKAPI_PTR set_equation(VkCommandBuffer, uint32_t first, uint32_t n, const VkColorBlendEquationEXT* v) {
    assert(first == 0); last_equations.assign(v, v + n);
}
void VKAPI_PTR set_mask(VkCommandBuffer, uint32_t first, uint32_t n, const VkColorComponentFlags* v) {
    assert(first == 0); last_masks.assign(v, v + n);
}
VkResult VKAPI_PTR create_pipeline(VkDevice, VkPipelineCache, uint32_t n,
                                  const VkGraphicsPipelineCreateInfo* ci,
                                  const VkAllocationCallbacks*, VkPipeline* out) {
    assert(n == 1);
    int kind = 4;  // whole pipeline
    if (ci->flags & VK_PIPELINE_CREATE_LIBRARY_BIT_KHR) {
        const auto* part = static_cast<const VkGraphicsPipelineLibraryCreateInfoEXT*>(ci->pNext);
        assert(part && part->sType == VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT);
        assert(part->pNext && static_cast<const VkBaseInStructure*>(part->pNext)->sType ==
                                  VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO);
        kind = part->flags == VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT ? 1 : 2;
        if (kind == 1) assert(ci->pColorBlendState && !ci->pStages);
        else assert(ci->pVertexInputState && ci->pRasterizationState && !ci->pColorBlendState);
    } else if (ci->pNext && static_cast<const VkBaseInStructure*>(ci->pNext)->sType ==
                                VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR) {
        kind = 3;
        const auto* parts = static_cast<const VkPipelineLibraryCreateInfoKHR*>(ci->pNext);
        assert(parts->libraryCount == 2 && parts->pLibraries[0] && parts->pLibraries[1]);
    }
    ++creates[kind];
    *out = kind == null_kind ? VK_NULL_HANDLE : reinterpret_cast<VkPipeline>(next_pipeline++);
    return kind == fail_kind ? VK_ERROR_INITIALIZATION_FAILED : VK_SUCCESS;
}
void VKAPI_PTR destroy_pipeline(VkDevice, VkPipeline p, const VkAllocationCallbacks*) {
    assert(p); ++destroys;
}
VkResult VKAPI_PTR create_module(VkDevice, const VkShaderModuleCreateInfo* ci,
                                const VkAllocationCallbacks*, VkShaderModule* out) {
    modules.emplace_back(ci->pCode, ci->pCode + ci->codeSize / 4);
    *out = reinterpret_cast<VkShaderModule>(modules.size());
    return VK_SUCCESS;
}
VkResult VKAPI_PTR create_set_layout(VkDevice, const VkDescriptorSetLayoutCreateInfo*,
                                     const VkAllocationCallbacks*, VkDescriptorSetLayout* out) {
    *out = reinterpret_cast<VkDescriptorSetLayout>(1); return VK_SUCCESS;
}
VkResult VKAPI_PTR create_pipeline_layout(VkDevice, const VkPipelineLayoutCreateInfo*,
                                          const VkAllocationCallbacks*, VkPipelineLayout* out) {
    *out = reinterpret_cast<VkPipelineLayout>(1); return VK_SUCCESS;
}
void VKAPI_PTR get_physical_properties(VkPhysicalDevice, VkPhysicalDeviceProperties* props) { *props = {}; }
}
namespace nx {
bool g_trace = false;
void log(const char* fmt, ...) {
    if (std::strncmp(fmt, "nvn/vk: GPL ", 12) != 0) return;
    char message[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(message, sizeof message, fmt, args);
    va_end(args);
    std::lock_guard lock(gpl_log_mutex);
    gpl_logs.emplace_back(message);
}
[[noreturn]] void fatal(const char*, ...) { std::_Exit(86); }
bool strict_runtime() { return std::getenv("TEST_DIAGNOSTIC") == nullptr; }
}
namespace nx::plat { uint64_t monotonic_ns() { return 0; } }
namespace nx::vk {
PFN_vkCreateShaderModule p_vkCreateShaderModule = create_module;
PFN_vkCreateDescriptorSetLayout p_vkCreateDescriptorSetLayout = create_set_layout;
PFN_vkCreatePipelineLayout p_vkCreatePipelineLayout = create_pipeline_layout;
PFN_vkGetPhysicalDeviceProperties p_vkGetPhysicalDeviceProperties = get_physical_properties;
PFN_vkCreateGraphicsPipelines p_vkCreateGraphicsPipelines = create_pipeline;
PFN_vkDestroyPipeline p_vkDestroyPipeline = destroy_pipeline;
}
namespace nx::nvn {
VkDevice vk_device() { return {}; }
VkPhysicalDevice vk_physical() { return {}; }
bool vk_depth_clip_control() { return extension; }
bool vk_vertex_divisor() { return false; }
uint32_t vk_vertex_divisor_max() { return 1; }
bool vk_extended_dynamic() { return dynamic_cull; }
bool vk_extended_dynamic_blend() { return dynamic_blend; }
bool vk_graphics_pipeline_library() { return use_gpl; }
const ExtendedDynamicCommands& vk_extended_dynamic_commands() {
    static const ExtendedDynamicCommands commands{set_cull, set_front, set_enable, set_equation, set_mask};
    return commands;
}
const VkPhysicalDeviceFeatures& vk_features() { return features; }
VkPipelineCache pipeline_cache() { return {}; }
void note_module_created(uint64_t) {}
void note_pipeline_created(uint64_t, bool) {}
void note_pipeline_prewarmed() {}
void note_pipeline_alias() {}
void note_pipeline_waited(uint64_t) {}
void note_pipeline_skipped() {}
bool warm_load(std::vector<WarmRec>& records) { records = warm_fixture; return true; }
void warm_record(const PipelineKey&) {}
const ShaderInfo* shader_by_hash(const std::string& hash) {
    return warm_shader && warm_shader->hash == hash ? warm_shader : nullptr;
}
bool patch_ufloat_outputs(const std::vector<uint32_t>&, uint32_t, std::vector<uint32_t>&) { return false; }
float uw_factor() { return 1; }
bool patch_x_scale(const std::vector<uint32_t>&, std::vector<uint32_t>&) { return false; }
std::vector<uint32_t> patched_spirv(const ShaderInfo*, const uint8_t*, uint32_t, bool* patched) {
    *patched = false; return source;
}
uint32_t vertex_int_kind(uint32_t) { return 0; }
uint32_t unwritten_inputs(const ShaderInfo*, const ShaderInfo*) { return 0; }
VkFormat vk_vertex_format(uint32_t) { return VK_FORMAT_UNDEFINED; }
VkPrimitiveTopology vk_topology(uint32_t) { return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; }
VkCullModeFlags vk_cull(uint32_t v) { return VkCullModeFlags(v); }
VkCompareOp vk_compare(uint32_t) { return VK_COMPARE_OP_LESS; }
VkStencilOp vk_stencil_op(uint32_t) { return VK_STENCIL_OP_KEEP; }
VkBlendFactor vk_blend_factor(uint32_t v) { return VkBlendFactor(v); }
VkBlendOp vk_blend_op(uint32_t v) { return VkBlendOp(v); }
bool equal_slack_pass(const DepthStencilInfo&) { return false; }
bool render_pass_objects() { return false; }
VkRenderPass compatible_render_pass(uint32_t, const VkFormat*, VkFormat) { return {}; }
}
namespace nx::nvn::replay {
int program_stage_slot(ShaderStage stage) {
    switch (stage) {
        case ShaderStage::Vertex: return 0;
        case ShaderStage::Fragment: return 1;
        case ShaderStage::TessellationControl: return 3;
        case ShaderStage::TessellationEvaluation: return 4;
        case ShaderStage::Compute: return 5;
        default: return -1;
    }
}
}

// Evaluate the scalar arithmetic inserted by production patch_clip_z. Position
// loads are supplied by the test, so expected depth results are independent of
// the instruction sequence chosen by the patcher.
float evaluate_z(const std::vector<uint32_t>& code, float z, float w) {
    std::unordered_map<uint32_t, float> scalar;
    float result = z;
    for (size_t i = 5; i < code.size();) {
        auto* p = code.data() + i;
        uint32_t op = p[0] & 65535, n = p[0] >> 16;
        assert(n && i + n <= code.size());
        if (op == 43 && n == 4) std::memcpy(&scalar[p[2]], p + 3, 4);
        if (op == 81 && n == 5) scalar[p[2]] = p[4] == 2 ? z : w;
        if (op == 129) scalar[p[2]] = scalar.at(p[3]) + scalar.at(p[4]);
        if (op == 133) scalar[p[2]] = scalar.at(p[3]) * scalar.at(p[4]);
        if (op == 82 && p[5] == 2) result = scalar.at(p[3]);
        i += n;
    }
    return result;
}

int main(int argc, char** argv) {
    assert(argc == 5);
    extension = std::stoi(argv[3]) != 0;
    bool forced = std::stoi(argv[4]) != 0;
    std::ifstream input(argv[1], std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(input)), {});
    assert(bytes.size() >= 20 && bytes.size() % 4 == 0);
    source.resize(bytes.size() / 4);
    std::memcpy(source.data(), bytes.data(), bytes.size());
    using namespace nx::nvn;
    if (const char* mode_env = std::getenv("TEST_GPL_LOG_MODE")) {
        const int mode = std::atoi(mode_env);
        PipelineKey key{};
        key.color_count = 1;
        key.color_formats[0] = VK_FORMAT_R8G8B8A8_UNORM;
        key.polygon.mode = 2;
        ProgramLayout layout{};
        auto build = prepare_graphics_pipeline(key, layout);
        use_gpl = true;
        // Ineligible pipelines and a disabled feature are ordinary paths,
        // never reported as a GPL failure or success.
        build->ci.pTessellationState = &build->tess;
        assert(compile(*build));
        build->ci.pTessellationState = nullptr;
        build->ci.renderPass = reinterpret_cast<VkRenderPass>(1);
        assert(compile(*build));
        build->ci.renderPass = VK_NULL_HANDLE;
        use_gpl = false;
        assert(compile(*build));
        use_gpl = true;
        assert(gpl_logs.empty());
        auto parallel_compile = [&] {
            std::vector<std::thread> threads;
            VkPipeline results[8]{};
            for (int i = 0; i < 8; ++i)
                threads.emplace_back([&, i] { results[i] = compile(*build); });
            for (auto& thread : threads) thread.join();
            for (VkPipeline result : results) assert(result);
        };
        if (mode == 7) {
            nx::vk::p_vkDestroyPipeline = nullptr;
            assert(compile(*build));
            nx::vk::p_vkDestroyPipeline = destroy_pipeline;
        } else if (mode > 0) {
            if (mode <= 3) fail_kind = mode;
            else null_kind = mode - 3;
            parallel_compile();
        }
        if (mode) {
            assert(gpl_logs.size() == 1);
            const char* stages[] = {"", "fragment output library", "shader library", "pipeline link"};
            const char* stage = mode == 7 ? "vkDestroyPipeline unavailable" : stages[(mode - 1) % 3 + 1];
            assert(gpl_logs[0].find(stage) != std::string::npos);
            assert(gpl_logs[0].find("falling back to monolithic pipeline") != std::string::npos);
            if (mode <= 3) assert(gpl_logs[0].find("VkResult -3") != std::string::npos);
            if (mode >= 4 && mode <= 6) assert(gpl_logs[0].find("VkResult 0, null pipeline") != std::string::npos);
        } else {
            assert(gpl_logs.empty());
        }
        const int monolithic_count = creates[4];
        fail_kind = null_kind = 0;
        parallel_compile();
        parallel_compile();
        assert(creates[4] == monolithic_count && creates[3] > 0);
        assert(gpl_logs.size() == (mode ? 2u : 1u));
        assert(gpl_logs.back().find("GPL linked a graphics pipeline successfully") != std::string::npos);
        std::cout << "Concurrent GPL logging passed (mode " << mode << ")\n";
        return 0;
    }
    if (const char* mode_env = std::getenv("TEST_PREWARM_MODE")) {
        const int mode = std::atoi(mode_env);
        ShaderInfo vs{};
        vs.stage = ShaderStage::Vertex;
        vs.hash = "prewarm-fixture";
        warm_shader = &vs;
        PipelineKey base{};
        base.vs = &vs;
        base.color_count = 1;
        base.color_formats[0] = VK_FORMAT_R8G8B8A8_UNORM;
        base.color.blend_enable[0] = 1;
        base.mask.mask[0] = 15;
        base.polygon.mode = 2;
        base.depth_mode = 1;
        // Persist records with static state, as an older session would have.
        dynamic_cull = dynamic_blend = false;
        base = canonical_key(base);
        PipelineKey culled = base;
        culled.polygon.cull = VK_CULL_MODE_BACK_BIT;
        culled.polygon.front = 1;
        PipelineKey blended = base;
        blended.color.blend_enable[0] = 0;
        blended.mask.mask[0] = 3;
        const PipelineKey draws[] = {base, culled, blended};
        for (const PipelineKey& draw : draws) {
            WarmRec record{};
            std::strcpy(record.vs, "prewarm-fixture");
            record.key = canonical_key(draw);
            record.key.vs = nullptr;
            warm_fixture.push_back(record);
        }
        dynamic_cull = (mode & 1) != 0;
        dynamic_blend = (mode & 2) != 0;
        pipeline_warm_step();
        // With both states dynamic all old records collapse to one. With one
        // dynamic state two survive; with neither state three survive.
        const size_t expected = std::getenv("NX_PIPE_CANON") ? 3 :
            mode == 3 ? 1 : mode == 0 ? 3 : 2;
        assert(g_inflight.size() == expected);
        ProgramLayout layout{};
        for (const PipelineKey& draw : draws)
            assert(graphics_pipeline(draw, layout));
        assert(g_inflight.empty());
        assert(g_canon.size() == expected && creates[4] == int(expected));
        std::cout << "Persisted prewarm keys match draw lookup (mode " << mode << ")\\n";
        return 0;
    }
    if (std::getenv("TEST_DYNAMIC_GPL")) {
        PipelineKey key{};
        key.color_count = 2;
        key.color_formats[0] = VK_FORMAT_R8G8B8A8_UNORM;
        key.color_formats[1] = VK_FORMAT_B10G11R11_UFLOAT_PACK32;
        key.ufloat_colors = 2;
        key.polygon.mode = 2;
        key.polygon.cull = VK_CULL_MODE_BACK_BIT;
        key.polygon.front = 1;
        key.color.blend_enable[0] = key.color.blend_enable[1] = 1;
        key.blend[0].src_rgb = VK_BLEND_FACTOR_SRC_ALPHA;
        key.blend[0].dst_rgb = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        key.blend[0].src_a = VK_BLEND_FACTOR_ONE;
        key.blend[0].dst_a = VK_BLEND_FACTOR_ZERO;
        key.blend[1].src_rgb = VK_BLEND_FACTOR_DST_ALPHA;
        key.blend[1].dst_rgb = VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        key.blend[1].src_a = VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        key.blend[1].dst_a = VK_BLEND_FACTOR_ONE;
        key.mask.mask[0] = 5; key.mask.mask[1] = 7;
        ProgramLayout layout{};
        auto static_build = prepare_graphics_pipeline(key, layout);
        auto static_key = canonical_key(key);
        PipelineKey changed = key;
        changed.polygon.cull = VK_CULL_MODE_FRONT_BIT;
        changed.polygon.front = 0;
        changed.color.blend_enable[0] = 0;
        changed.blend[0].src_rgb = VK_BLEND_FACTOR_SRC1_COLOR;
        changed.blend[0].dst_rgb = VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
        changed.blend[0].src_a = VK_BLEND_FACTOR_SRC1_ALPHA;
        changed.blend[0].dst_a = VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
        changed.mask.mask[1] = 1;
        assert(!(bytes_of(static_key) == bytes_of(canonical_key(changed))));
        assert(static_build->att[1].srcColorBlendFactor == VK_BLEND_FACTOR_ONE);
        assert(static_build->att[1].dstColorBlendFactor == VK_BLEND_FACTOR_ZERO);
        assert(static_build->att[1].srcAlphaBlendFactor == VK_BLEND_FACTOR_ZERO);

        dynamic_cull = dynamic_blend = true;
        assert(bytes_of(canonical_key(key)) == bytes_of(canonical_key(changed)));
        auto dynamic_build = prepare_graphics_pipeline(key, layout);
        auto canon_build = prepare_graphics_pipeline(canonical_key(key), layout);
        assert(serialize_build(*dynamic_build) == serialize_build(*canon_build));
        assert(dynamic_build->dy.dynamicStateCount == 12);
        set_extended_dynamic(reinterpret_cast<VkCommandBuffer>(1), key);
        assert(last_cull == static_build->rs.cullMode && last_front == static_build->rs.frontFace);
        for (unsigned i = 0; i < 2; ++i) {
            auto& a = static_build->att[i];
            auto& e = last_equations[i];
            assert(last_enables[i] == a.blendEnable && last_masks[i] == a.colorWriteMask);
            assert(e.srcColorBlendFactor == a.srcColorBlendFactor && e.dstColorBlendFactor == a.dstColorBlendFactor);
            assert(e.colorBlendOp == a.colorBlendOp && e.srcAlphaBlendFactor == a.srcAlphaBlendFactor);
            assert(e.dstAlphaBlendFactor == a.dstAlphaBlendFactor && e.alphaBlendOp == a.alphaBlendOp);
        }
        set_extended_dynamic(reinterpret_cast<VkCommandBuffer>(2), changed);
        assert(cull_calls == 2 && blend_calls == 2 && last_cull == VK_CULL_MODE_FRONT_BIT);
        assert(last_front == window_front_face(changed.polygon.front, changed.window_origin));
        assert(!last_enables[0] && last_masks[1] == 1);
        assert(!features.dualSrcBlend);
        const auto& disabled_equation = last_equations[0];
        assert(disabled_equation.srcColorBlendFactor == VK_BLEND_FACTOR_ONE);
        assert(disabled_equation.dstColorBlendFactor == VK_BLEND_FACTOR_ZERO);
        assert(disabled_equation.colorBlendOp == VK_BLEND_OP_ADD);
        assert(disabled_equation.srcAlphaBlendFactor == VK_BLEND_FACTOR_ONE);
        assert(disabled_equation.dstAlphaBlendFactor == VK_BLEND_FACTOR_ZERO);
        assert(disabled_equation.alphaBlendOp == VK_BLEND_OP_ADD);
        dynamic_cull = false;
        auto blend_only = canonical_key(key);
        assert(!(bytes_of(blend_only) == bytes_of(canonical_key(changed))));
        dynamic_cull = true; dynamic_blend = false;
        assert(!(bytes_of(canonical_key(key)) == bytes_of(canonical_key(changed))));
        dynamic_blend = true;

        use_gpl = true;
        assert(compile(*dynamic_build));
        assert(creates[1] == 1 && creates[2] == 1 && creates[3] == 1 && creates[4] == 0 && destroys == 1);
        assert(compile(*dynamic_build));
        assert(creates[1] == 1 && creates[2] == 2 && creates[3] == 2 && destroys == 2);
        key.color_formats[0] = VK_FORMAT_R16G16B16A16_SFLOAT;
        auto distinct = prepare_graphics_pipeline(key, layout);
        fail_kind = 1;
        assert(compile(*distinct));
        assert(creates[1] == 2 && creates[4] == 1 && destroys == 3);
        fail_kind = 2;
        assert(compile(*dynamic_build));
        assert(creates[2] == 3 && creates[4] == 2 && destroys == 4);
        fail_kind = 3;
        assert(compile(*dynamic_build));
        assert(creates[3] == 3 && creates[4] == 3 && destroys == 6);
        fail_kind = 0;
        dynamic_build->ci.renderPass = reinterpret_cast<VkRenderPass>(1);
        assert(compile(*dynamic_build));
        assert(creates[4] == 4);
        dynamic_build->ci.renderPass = VK_NULL_HANDLE;
        dynamic_build->ci.pTessellationState = &dynamic_build->tess;
        assert(compile(*dynamic_build));
        assert(creates[4] == 5);
        use_gpl = false;
        dynamic_build->ci.pTessellationState = nullptr;
        assert(compile(*dynamic_build));
        assert(creates[4] == 6);
        std::cout << "Dynamic state and graphics pipeline library fallback passed\\n";
        return 0;
    }
    if (std::getenv("TEST_ALPHA_VARIANT")) {
        ShaderInfo fs; fs.stage = ShaderStage::Fragment; fs.hash = "alpha-fixture";
        PipelineKey alpha; alpha.fs = &fs; alpha.color.alpha_test = 5; alpha.alpha_ref = 1.0f / 255.0f;
        ProgramLayout layout;
        auto tested = prepare_graphics_pipeline(alpha, layout);
        assert(modules.size() == 1 && modules.back() != source);
        auto again = prepare_graphics_pipeline(alpha, layout);
        assert(modules.size() == 1 && again->stages[0].module == tested->stages[0].module);
        auto key = canonical_key(alpha);
        alpha.alpha_ref = .5f;
        assert(!(bytes_of(key) == bytes_of(canonical_key(alpha))));
        prepare_graphics_pipeline(alpha, layout);
        assert(modules.size() == 2 && modules[0] != modules[1]);
        alpha.color.alpha_test = 8;
        prepare_graphics_pipeline(alpha, layout);
        assert(modules.size() == 3 && modules.back() == source);
        auto always = canonical_key(alpha); alpha.alpha_ref = .75f;
        assert(bytes_of(always) == bytes_of(canonical_key(alpha)));
        std::ofstream out(argv[2], std::ios::binary);
        out.write(reinterpret_cast<const char*>(modules[0].data()), modules[0].size() * 4);
        std::cout << "Production alpha-test module variants and canonical cache keys passed\n";
        return 0;
    }
    const VkViewport vp{11, 23, 80, 40, .2f, .8f};
    auto upper = window_viewport(vp, 1), lower = window_viewport(vp, 0);
    assert(upper.y == 63 && upper.height == -40 && lower.y == 23 && lower.height == 40);
    assert(upper.minDepth == .2f && upper.maxDepth == .8f);
    // NDC +Y touches the upper edge in upper-left mode; lower-left uses the
    // opposite edge. Both origins address the same [23,63] window rows.
    assert(upper.y + upper.height == 23 && lower.y + lower.height == 63);
    assert(window_front_face(0, 1) == VK_FRONT_FACE_CLOCKWISE);
    assert(window_front_face(1, 1) == VK_FRONT_FACE_COUNTER_CLOCKWISE);
    assert(window_front_face(0, 0) == VK_FRONT_FACE_COUNTER_CLOCKWISE);
    assert(window_front_face(1, 0) == VK_FRONT_FACE_CLOCKWISE);
    auto sc = window_scissor(-4, 3, 8, 5, {100, 100});
    assert(sc.offset.x == 0 && sc.offset.y == 3 && sc.extent.width == 4 && sc.extent.height == 5);
    sc = window_scissor(98, 99, 8, 5, {100, 100});
    assert(sc.extent.width == 2 && sc.extent.height == 1);
    sc = window_scissor(INT32_MAX, INT32_MIN, INT32_MAX, INT32_MAX, {100, 100});
    assert(sc.extent.width == 0 && sc.extent.height == 0);

    ShaderInfo vs{};
    vs.stage = ShaderStage::Vertex; vs.hash = "fixture";
    PipelineKey key{}; key.vs = &vs; key.polygon.mode = 2;
    key.depth_mode = 1;
    ProgramLayout layout{};
    auto native = prepare_graphics_pipeline(key, layout);
    assert(native->clip.negativeOneToOne == VK_FALSE);
    assert(modules.size() == 1 && modules[0] == source);
    key.depth_mode = 0;
    auto minus_w = prepare_graphics_pipeline(key, layout);
    assert(minus_w->clip.negativeOneToOne == VK_TRUE);
    assert(modules.size() == 2);
    const bool patch = !extension || forced;
    assert((modules[1] != source) == patch);
    assert((minus_w->vp.pNext != nullptr) == (extension && !forced));
    assert(evaluate_z(modules[1], -2, 2) == (patch ? 0 : -2));
    assert(evaluate_z(modules[1], 0, 2) == (patch ? 1 : 0));
    assert(evaluate_z(modules[1], 2, 2) == 2);
    assert(evaluate_z(modules[1], -3, 2) == (patch ? -.5f : -3));
    key.depth_mode = 1;
    auto again = prepare_graphics_pipeline(key, layout);
    assert(modules.size() == 2 && again->stages[0].module == native->stages[0].module);
    auto base = canonical_key(key);
    key.window_origin = 0;
    auto other_origin = canonical_key(key);
    assert(!(bytes_of(base) == bytes_of(other_origin)));
    key.depth_mode = 0;
    assert(!(bytes_of(other_origin) == bytes_of(canonical_key(key))));
    if (std::getenv("TEST_UNSUPPORTED_WIDTH")) {
        key.line_width = 3;
        auto unsupported = prepare_graphics_pipeline(key, layout);
        assert(!nx::strict_runtime() && unsupported->rs.lineWidth == 1);
        return 0;
    }
    features.wideLines = VK_TRUE;
    key.line_width = 3;
    auto wide = prepare_graphics_pipeline(key, layout);
    assert(wide->rs.lineWidth == 3 && canonical_key(key).line_width == 3);
    assert(wide->rs.frontFace == VK_FRONT_FACE_COUNTER_CLOCKWISE);
    key.line_width = 1;
    auto narrow = prepare_graphics_pipeline(key, layout);
    assert(serialize_build(*wide) != serialize_build(*narrow));
    key.depth_format = VK_FORMAT_D32_SFLOAT;
    features.depthBounds = VK_TRUE;
    key.depth_bounds = 1;
    auto bounded = prepare_graphics_pipeline(key, layout);
    assert(bounded->ds.depthBoundsTestEnable == VK_TRUE);
    assert(bounded->dy.pDynamicStates[bounded->dy.dynamicStateCount-1] == VK_DYNAMIC_STATE_DEPTH_BOUNDS);
    auto bounds_key = canonical_key(key);
    key.depth_bounds = 0;
    assert(!(bytes_of(bounds_key) == bytes_of(canonical_key(key))));
    auto unbounded = prepare_graphics_pipeline(key, layout);
    assert(unbounded->ds.depthBoundsTestEnable == VK_FALSE);
    assert(serialize_build(*bounded) != serialize_build(*unbounded));
    // Real stage create-info/patch state, with the same original-sized three
    // control points used by HMFlower. No Vulkan device is required here.
    ShaderInfo tcs{}, tes{};
    tcs.stage = ShaderStage::TessellationControl; tcs.hash = "tcs-fixture";
    tes.stage = ShaderStage::TessellationEvaluation; tes.hash = "tes-fixture";
    features.tessellationShader = VK_TRUE;
    key.tcs = &tcs; key.tes = &tes; key.patch_size = 3;
    auto tess = prepare_graphics_pipeline(key, layout);
    assert(tess->nstages == 3 && tess->stages[0].stage == VK_SHADER_STAGE_VERTEX_BIT);
    assert(tess->stages[1].stage == VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT);
    assert(tess->stages[2].stage == VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT);
    assert(tess->ia.topology == VK_PRIMITIVE_TOPOLOGY_PATCH_LIST);
    assert(tess->ci.pTessellationState == &tess->tess && tess->tess.patchControlPoints == 3);
    assert(canonical_key(key).tcs == &tcs && canonical_key(key).tes == &tes);
    const auto tess_bytes = bytes_of(canonical_key(key));
    key.patch_size = 4;
    assert(!(tess_bytes == bytes_of(canonical_key(key))));
    auto tess4 = prepare_graphics_pipeline(key, layout);
    assert(serialize_build(*tess) != serialize_build(*tess4));
    std::ofstream out(argv[2], std::ios::binary);
    out.write(reinterpret_cast<const char*>(modules[1].data()), modules[1].size() * 4);
    std::cout << "NVN modes/cache/depth patch/line width passed (extension=" << extension << ", forced=" << forced << ")\n";
}
