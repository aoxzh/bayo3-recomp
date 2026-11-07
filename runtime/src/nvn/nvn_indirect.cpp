// Retail B3 SDK sha256 2a89fa1c...57a20d28:
// sdk+4c6ad0 DrawElementsIndirect(x0 cmd,w1 mode,w2 type,x3 indices,x4 args).
// sdk+4c7170 MultiDrawElementsIndirectCount adds x5 count,w6 maxDraws,w7 stride.
// sdk+4c6e00 normalizes nonpositive stride to 20; records consume five words
// {indexCount, instanceCount, firstIndex, signed baseVertex, baseInstance}.
#include "nvn_indirect.h"
#include "nvn_draw_state.h"
#include <algorithm>
#include <cstring>
#include <limits>

namespace nx::nvn {
// nvn_objects.cpp: smallest containing live NVNbuffer allocation, or zero.
uint64_t buffer_remaining_at_gpu(uint64_t gpu);
using namespace nx::vk;
namespace {
constexpr uint32_t kArgumentBytes = sizeof(VkDrawIndexedIndirectCommand);
static_assert(kArgumentBytes == 20);

// NX_INDIRECT_PROF=1: prepare_indexed_indirect is the single most expensive replay path on the phone (527 calls,
// 21.7 ms a frame), so its parts are timed separately instead of guessed at.
enum IndPart { kIBacking, kIWritten, kIUpload, kIInputs, kIBarrier, kIEmit, kIPassEnd, kIPartCount };
const char* const kIndName[kIPartCount] = {"backing", "written", "upload", "inputs", "barrier", "emit", "passend"};
uint64_t g_ind_pass_ended = 0;  // how many of these calls closed an open render pass
uint64_t g_ind_draws_sum = 0, g_ind_draws_max = 0, g_ind_draws_calls = 0;  // max_draws handed to the driver
uint64_t g_ind_ns[kIPartCount], g_ind_calls[kIPartCount];
uint64_t g_ind_any_max_index = 0, g_ind_any_bytes = 0;
uint64_t g_ind_pass_kept = 0;  // indirect draws replayed without ending the open pass (NX_INDIRECT_STAY_IN_PASS)
bool ind_prof() {
    static const bool on = std::getenv("NX_INDIRECT_PROF") != nullptr;
    return on;
}
struct IndScope {
    explicit IndScope(IndPart p) : part(p) {
        if (!on) return;
        t0 = plat::monotonic_ns(), live = true;
    }
    ~IndScope() {
        if (!live) return;
        g_ind_ns[part] += plat::monotonic_ns() - t0, g_ind_calls[part]++;
    }
    IndPart part;
    uint64_t t0 = 0;
    const bool on = ind_prof();
    bool live = false;
};

void DrawElementsIndirect(CPU& cpu) {
    count_recorded_draw();  // one guest command; the actual GPU draw count is not known here
    Cmd c{Cmd::DrawElementsIndirect};
    c.n = uint32_t(cpu.x[1]); c.mask = uint32_t(cpu.x[2]);
    c.a[0] = cpu.x[3]; c.a[1] = cpu.x[4];
    c.i[0] = 1; c.i[1] = kArgumentBytes;
    record(cpu.x[0], c);
}
void MultiDrawElementsIndirectCount(CPU& cpu) {
    count_recorded_draw();  // one guest command; the GPU-side count is what the replay issues
    Cmd c{Cmd::MultiDrawElementsIndirectCount};
    c.n = uint32_t(cpu.x[1]); c.mask = uint32_t(cpu.x[2]);
    c.a[0] = cpu.x[3]; c.a[1] = cpu.x[4]; c.a[2] = cpu.x[5];
    c.i[0] = int32_t(cpu.x[6]);
    c.i[1] = int32_t(cpu.x[7]) > 0 ? int32_t(cpu.x[7]) : int32_t(kArgumentBytes);
    record(cpu.x[0], c);
}
struct Span { const uint8_t* p; uint64_t size; };
// One physical backing span, stopping at virtual mapping boundaries. This also
// gives a safe index-buffer upper bound when firstIndex/count are GPU-generated.
Span span_at(uint64_t gpu) {
    uint64_t offset = 0;
    auto* pool = pool_at_gpu(gpu, &offset);
    if (!pool || offset >= pool->size) fatal("nvn: indexed indirect GPU address is unmapped");
    uint64_t size = pool->size - offset, memory = pool->memory;
    if (!memory) {
        MemoryPool::Mapping mapping;
        uint64_t within;
        {
            std::shared_lock lock(objects_lock());
            auto it = pool->mappings.upper_bound(offset);
            if (it == pool->mappings.begin()) fatal("nvn: indexed indirect virtual mapping is absent");
            --it; mapping = it->second; within = offset - it->first;
        }
        if (within >= mapping.size) fatal("nvn: indexed indirect virtual mapping has a hole");
        auto* physical = get<MemoryPool>(mapping.physical);
        size = std::min(size, mapping.size - within);
        if (!physical || !physical->memory || mapping.physical_offset > physical->size ||
            within > physical->size - mapping.physical_offset ||
            size > physical->size - mapping.physical_offset - within)
            fatal("nvn: indexed indirect mapping exceeds physical backing");
        memory = physical->memory; offset = mapping.physical_offset + within;
    }
    if (memory > UINT64_MAX - offset || size > UINT64_MAX - memory - offset ||
        size > uint64_t(std::numeric_limits<ptrdiff_t>::max()) - 0x11000)
        fatal("nvn: indexed indirect host range overflows");
    return {reinterpret_cast<const uint8_t*>(memory + offset), size};
}
std::vector<Span> backing(uint64_t gpu, uint64_t size) {
    if (size > UINT64_MAX - gpu) fatal("nvn: indexed indirect GPU range overflows");
    std::vector<Span> spans;
    while (size) {
        Span s = span_at(gpu); s.size = std::min(s.size, size);
        spans.push_back(s); size -= s.size; gpu += s.size;
    }
    return spans;
}
bool gpu_written(const std::vector<Span>& spans) {
    for (auto s : spans) if (replay::written_overlaps(s.p, s.size)) return true;
    return false;
}
void host_read(const std::vector<Span>& spans, uint64_t offset, void* out, uint64_t bytes) {
    auto* dst = static_cast<uint8_t*>(out);
    for (auto s : spans) {
        if (offset >= s.size) { offset -= s.size; continue; }
        uint64_t n = std::min(bytes, s.size - offset);
        std::memcpy(dst, s.p + offset, n); dst += n; bytes -= n; offset = 0;
        if (!bytes) return;
    }
    fatal("nvn: indexed indirect host read exceeds backing");
}
void barrier(VkCommandBuffer cb) {
    VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_INDEX_READ_BIT |
                      VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                          VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
                          VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &b, 0, nullptr, 0, nullptr);
}
Upload read_spans(VkCommandBuffer cb, const std::vector<Span>& spans, uint64_t bytes) {
    if (spans.size() == 1) return replay::read_upload(cb, spans[0].p, bytes, bytes, 4);
    // Disjoint virtual mappings are gathered in GPU order; never CPU-copy a
    // pending shader/copy result out of its mapped-but-not-yet-executed upload.
    Upload out = upload_alloc(bytes, 16);
    uint64_t offset = 0;
    for (auto s : spans) {
        Upload src = replay::read_upload(cb, s.p, s.size, s.size, 4);
        // NX_REPLAY_CUTS=barrier: src may be a composed copy; make it visible before this transfer reads it.
        replay::copy_write_barrier(cb);
        VkBufferCopy copy{src.offset, out.offset + offset, s.size};
        p_vkCmdCopyBuffer(cb, src.buffer, out.buffer, 1, &copy);
        offset += s.size;
    }
    VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                          0, 1, &b, 0, nullptr, 0, nullptr);
    return out;
}
}  // namespace

bool is_indexed_indirect(const Cmd& c) {
    return c.op == Cmd::DrawElementsIndirect || c.op == Cmd::MultiDrawElementsIndirectCount;
}

bool prepare_indexed_indirect(VkCommandBuffer cb, const ShaderInfo* vs, const Cmd& c,
                              replay::DrawInputs& in, IndexedIndirect& indirect) {
    indirect = {};
    if (!is_indexed_indirect(c)) fatal("nvn: wrong command in indexed indirect preparation");
    if (c.i[0] <= 0) return false;
    if (c.mask > 2) fatal("nvn: indexed indirect invalid index type %u", c.mask);
    if (c.i[1] < int32_t(kArgumentBytes) || c.i[1] % 4 || (c.a[1] & 3))
        fatal("nvn: indexed indirect invalid argument alignment/stride");
    if (replay::written_separate())
        fatal("nvn: indexed indirect requires shared GPU writes (unset NX_COMPUTE_SEPARATE)");
    indirect.max_draws = uint32_t(c.i[0]); indirect.stride = uint32_t(c.i[1]);
    const bool counted = c.op == Cmd::MultiDrawElementsIndirectCount;
    bool count_unknown = false;
    std::vector<Span> counts;
    if (counted) {
        if (c.a[2] & 3) fatal("nvn: indexed indirect count address must be 4-byte aligned");
        counts = backing(c.a[2], 4);
        indirect.counted = vk_draw_indirect_count() && p_vkCmdDrawIndexedIndirectCount;
        if (!indirect.counted) {
            if (gpu_written(counts)) {
                // MoltenVK/Metal exposes no indirect count (VK_EXT_draw_indirect_count is absent), so a count the GPU
                // writes in this very submission cannot be read back without stopping the submission here. Draw the
                // whole max_draws interval instead: it is a superset of the requested draws, and a record beyond the
                // count is either an instanceCount of 0 (NVN clears the unused entries: a no-op draw) or stale
                // geometry from an earlier frame, which the pipeline clips and depth-tests like any other draw.
                // The alternative -- reading a value the GPU has not produced yet -- is not available.
                count_unknown = true;
                static bool warned = false;
                if (!warned) {
                    log("nvn: GPU-written multi-draw count: device has no drawIndirectCount, drawing all %u "
                        "records (stride %u) -- a superset of the requested draws",
                        indirect.max_draws, indirect.stride);
                    warned = true;
                }
            } else {
                uint32_t count;
                host_read(counts, 0, &count, 4);
                indirect.max_draws = std::min(count, indirect.max_draws);
                if (!indirect.max_draws) return false;
            }
        }
    }
    // device_properties(), not p_vkGetPhysicalDeviceProperties: this runs per indirect draw (712 a battle frame,
    // 8.1 us each with the driver call, NX_REPLAY_PROF op 49).
    const VkPhysicalDeviceProperties& props = device_properties();
    if (indirect.counted && (indirect.max_draws > props.limits.maxDrawIndirectCount ||
        (indirect.max_draws > 1 && !vk_features().multiDrawIndirect)))
        fatal("nvn: counted indexed indirect exceeds device draw-count capability");
    uint64_t bytes = uint64_t(indirect.max_draws - 1) * indirect.stride + kArgumentBytes;
    std::vector<Span> args;
    { IndScope sp(kIBacking); args = backing(c.a[1], bytes); }
    bool generated;
    { IndScope sp(kIWritten); generated = gpu_written(args); }
    uint64_t index_bytes = 0;
    const uint32_t isz = index_size(c.mask);
    if (c.a[0] % isz) fatal("nvn: indexed indirect index address is misaligned");
    if (generated || indirect.counted || count_unknown) {
        if (!vk_features().drawIndirectFirstInstance)
            fatal("nvn: GPU-generated/count indexed indirect needs drawIndirectFirstInstance");
        // Without a CPU-visible firstIndex/indexCount, bind all remaining indices
        // of the backing allocation. No arbitrary count clamp or stale CPU scan.
        uint64_t remaining = buffer_remaining_at_gpu(c.a[0]);
        if (!remaining) remaining = span_at(c.a[0]).size;
        index_bytes = remaining / isz * isz;
    } else {
        for (uint32_t k = 0; k < indirect.max_draws; ++k) {
            VkDrawIndexedIndirectCommand a;
            host_read(args, uint64_t(k) * indirect.stride, &a, sizeof a);
            if (!a.indexCount || !a.instanceCount) {
                // Bisection: the draw arguments the GPU will see, when the record is degenerate.
                static int dead = 0;
                if (dead++ < 10 || dead % 2000 == 0)
                    log("nvn: degenerate indirect record %u/%u: indexCount %u instanceCount %u firstIndex %u "
                        "vertexOffset %d firstInstance %u (vs %s)",
                        k + 1, indirect.max_draws, a.indexCount, a.instanceCount, a.firstIndex, a.vertexOffset,
                        a.firstInstance, vs ? vs->hash.c_str() : "-");
            }
            if (a.firstInstance && !vk_features().drawIndirectFirstInstance)
                fatal("nvn: indexed indirect baseInstance requires drawIndirectFirstInstance");
            if (a.indexCount && a.instanceCount)
                index_bytes = std::max(index_bytes, (uint64_t(a.firstIndex) + a.indexCount) * isz);
        }
    }
    index_bytes = std::max<uint64_t>(index_bytes, isz);
    g_ind_any_max_index = std::max(g_ind_any_max_index, index_bytes);
    std::vector<Span> indices;
    { IndScope sp(kIBacking); indices = backing(c.a[0], index_bytes); }
    const bool index_generated = gpu_written(indices);
    if (isz == 1 && index_generated)
        fatal("nvn: GPU-written 8-bit indirect indices require native indexTypeUint8 or GPU widening");
    // End the pass and record the barrier only when GPU commands are actually involved. Two things need them:
    // a dispatch of this submit writing the range (the composition copy), and a range that crosses virtual mapping
    // boundaries (read_spans gathers those with vkCmdCopyBuffer, and the barrier would then be recorded inside a pass,
    // where a TRANSFER-stage barrier is not valid). Everything else is host-visible ring memory the CPU wrote before
    // the submit, which the vertex and index paths read the same way without breaking a pass.
    //
    // Why it matters: the Prologue fight issues 539 indirect draws a frame with max_draws 1, and 532 of them broke the
    // open pass -- closing and reopening a render pass per single-primitive draw. Measured at 120 Hz, keeping the pass
    // cut the "passes ended by draw" counter from 3,085,236 to 148,710 over the same run and lifted the fight median
    // from 61.1 to 63.4 fps with the pictures identical and 0 fatal. NX_INDIRECT_STAY_IN_PASS=0 restores the break.
    static const bool stay_in_pass = !(std::getenv("NX_INDIRECT_STAY_IN_PASS") &&
                                       std::getenv("NX_INDIRECT_STAY_IN_PASS")[0] == '0');
    const bool needs_copy = generated || index_generated || args.size() > 1 || indices.size() > 1 || counts.size() > 1;
    if (!stay_in_pass || needs_copy) {
        // All preflight checks above precede recording. End rendering before copies
        // and barriers, including dependencies on earlier graphics storage writes.
        { IndScope sp(kIPassEnd);
          if (pass_open()) g_ind_pass_ended++;
          draw_end_rendering(cb, 1); }
        { IndScope sp(kIBarrier); barrier(cb); }
    } else if (pass_open()) {
        g_ind_pass_kept++;
    }
    { IndScope sp(kIUpload);
      indirect.args = read_spans(cb, args, bytes);
      if (indirect.counted) indirect.count = read_spans(cb, counts, 4); }
    { IndScope sp(kIInputs);
      if (!replay::draw_inputs_shared(cb, vs, false, c, in)) return false; }
    if (isz == 1) {
        // The shader's firstIndex remains in index units, so widening preserves
        // every indirect field (including negative baseVertex and zero instances).
        in.index = upload_alloc(index_bytes * 2, 16);
        auto* dst = reinterpret_cast<uint16_t*>(in.index.ptr);
        uint64_t at = 0;
        for (auto s : indices) for (uint64_t k = 0; k < s.size; ++k) dst[at++] = s.p[k];
        in.index_type = VK_INDEX_TYPE_UINT16;
    } else {
        in.index = read_spans(cb, indices, index_bytes);
        in.index_type = vk_index_type(c.mask);
    }
    return true;
}

void emit_indexed_indirect(VkCommandBuffer cb, const IndexedIndirect& indirect) {
    if (!indirect.max_draws) return;
    g_ind_draws_sum += indirect.max_draws, g_ind_draws_calls++;
    g_ind_draws_max = std::max<uint64_t>(g_ind_draws_max, indirect.max_draws);
    IndScope sp(kIEmit);  // the driver call itself: on Adreno a counted multi-draw is not free
    if (indirect.counted) {
        p_vkCmdDrawIndexedIndirectCount(cb, indirect.args.buffer, indirect.args.offset,
                                       indirect.count.buffer, indirect.count.offset,
                                       indirect.max_draws, indirect.stride);
    } else {
        // Single calls also implement host-count fallback on devices without
        // multiDrawIndirect, preserving GPU-supplied draw arguments themselves.
        // The batch size is a device limit, so read it once (device_properties()) rather than per emitted draw.
        static const uint32_t batch = vk_features().multiDrawIndirect
                                          ? std::max(1u, device_properties().limits.maxDrawIndirectCount)
                                          : 1;
        for (uint32_t at = 0; at < indirect.max_draws;) {
            uint32_t n = std::min(batch, indirect.max_draws - at);
            p_vkCmdDrawIndexedIndirect(cb, indirect.args.buffer,
                                      indirect.args.offset + uint64_t(at) * indirect.stride, n, indirect.stride);
            at += n;
        }
    }
}
// NX_INDIRECT_PROF: one line per part plus what the index range cost in bytes, which is the size the whole index
// buffer is bound at when the arguments are GPU-generated (the common case in this game).
void indirect_prof_log(uint64_t frames) {
    if (!ind_prof() || !frames) return;
    uint64_t total = 0;
    for (int k = 0; k < kIPartCount; k++) total += g_ind_ns[k];
    for (int k = 0; k < kIPartCount; k++)
        if (g_ind_calls[k])
            log("nvn/indirect-prof: %-8s %7.2f ms/frame (%5.1f %%) %8llu calls/frame %7.0f ns/call", kIndName[k],
                double(g_ind_ns[k]) / 1e6 / double(frames), 100.0 * double(g_ind_ns[k]) / double(total),
                (unsigned long long)(g_ind_calls[k] / frames), double(g_ind_ns[k]) / double(g_ind_calls[k]));
    log("nvn/indirect-prof: %llu of those calls closed an open render pass (%.0f%%); %llu stayed in a pass; "
        "largest index range bound %llu bytes",
        (unsigned long long)(g_ind_pass_ended / frames),
        g_ind_calls[kIPassEnd] ? 100.0 * double(g_ind_pass_ended) / double(g_ind_calls[kIPassEnd]) : 0.0,
        (unsigned long long)(g_ind_pass_kept / frames), (unsigned long long)g_ind_any_max_index);
    log("nvn/indirect-prof: parts total %7.2f ms/frame; largest index range bound %llu bytes; "
        "max_draws per call avg %.1f max %llu over %llu calls",
        double(total) / 1e6 / double(frames), (unsigned long long)g_ind_any_max_index,
        g_ind_draws_calls ? double(g_ind_draws_sum) / double(g_ind_draws_calls) : 0.0,
        (unsigned long long)g_ind_draws_max, (unsigned long long)(g_ind_draws_calls / frames));
    for (int k = 0; k < kIPartCount; k++) g_ind_ns[k] = 0, g_ind_calls[k] = 0;
    g_ind_any_max_index = 0;
    g_ind_draws_sum = g_ind_draws_max = g_ind_draws_calls = 0;
    g_ind_pass_ended = 0;
}

NX_HLE("nvnCommandBufferDrawElementsIndirect", DrawElementsIndirect);
NX_HLE("nvnCommandBufferMultiDrawElementsIndirectCount", MultiDrawElementsIndirectCount);
}  // namespace nx::nvn
