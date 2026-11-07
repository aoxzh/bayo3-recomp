// Buffer copies use the submit's shared GPU-written ranges. Guest storage is
// updated by the normal fence/write-back path, never while recording/replaying.
#include "nvn_buffer_copy.h"
#include "nvn_draw_state.h"
#include <algorithm>
#include <limits>

namespace nx::nvn {
using namespace nx::vk;
namespace {
void validate(const Cmd& c) {
    // B3 SDK SHA 2a89fa1c...57a20d28, sdk+4a5b70..4a5cb4 does not read
    // flags (w4). The observed main+cf5000 calls pass zero. Only this contract
    // is supported: do not silently swallow an unknown memory-order flag or
    // assume that texture filtering/engine flags apply to a buffer copy.
    if (c.n) fatal("nvn: CopyBufferToBuffer unsupported flags %#x (only zero supported)", c.n);
    if (c.a[2] > UINT64_MAX - c.a[0] || c.a[2] > UINT64_MAX - c.a[1])
        fatal("nvn: CopyBufferToBuffer address range overflows");
    if (c.a[2] && c.a[0] < c.a[1] + c.a[2] && c.a[1] < c.a[0] + c.a[2])
        fatal("nvn: CopyBufferToBuffer source and destination overlap");
}
void CopyBufferToBuffer(CPU& cpu) {
    Cmd c{Cmd::CopyBufferToBuffer};
    c.a[0] = cpu.x[1]; c.a[1] = cpu.x[2]; c.a[2] = cpu.x[3]; c.n = uint32_t(cpu.x[4]);
    validate(c);
    record(cpu.x[0], c);
}
struct Span { uint64_t host, size; };
std::vector<Span> backing(uint64_t gpu, uint64_t size) {
    std::vector<Span> spans;
    while (size) {
        uint64_t offset = 0;
        auto* pool = pool_at_gpu(gpu, &offset);
        if (!pool || offset >= pool->size) fatal("nvn: CopyBufferToBuffer GPU range is unmapped");
        uint64_t bytes = std::min(size, pool->size - offset);
        uint64_t memory = pool->memory;
        if (!memory) {
            MemoryPool::Mapping mapping;
            uint64_t within;
            {
                std::shared_lock lock(objects_lock());
                auto it = pool->mappings.upper_bound(offset);
                if (it == pool->mappings.begin()) fatal("nvn: CopyBufferToBuffer virtual mapping is absent");
                --it;
                mapping = it->second;
                within = offset - it->first;
            }
            if (within >= mapping.size) fatal("nvn: CopyBufferToBuffer virtual mapping has a hole");
            bytes = std::min(bytes, mapping.size - within);
            auto* physical = get<MemoryPool>(mapping.physical);
            if (!physical || !physical->memory || mapping.physical_offset > physical->size ||
                within > physical->size - mapping.physical_offset ||
                bytes > physical->size - mapping.physical_offset - within)
                fatal("nvn: CopyBufferToBuffer virtual mapping exceeds physical backing");
            offset = mapping.physical_offset + within;
            memory = physical->memory;
        }
        if (!bytes || memory > UINT64_MAX - offset ||
            bytes > UINT64_MAX - (memory + offset) ||
            bytes > uint64_t(std::numeric_limits<ptrdiff_t>::max()) - 0x11000)
            fatal("nvn: CopyBufferToBuffer host backing range overflows");
        spans.push_back({memory + offset, bytes});
        gpu += bytes;
        size -= bytes;
    }
    return spans;
}
void reject_alias_overlap(std::vector<Span> src, std::vector<Span> dst) {
    auto order = [](const Span& a, const Span& b) { return a.host < b.host; };
    std::sort(src.begin(), src.end(), order);
    std::sort(dst.begin(), dst.end(), order);
    size_t s = 0, d = 0;
    while (s < src.size() && d < dst.size()) {
        if (src[s].host < dst[d].host + dst[d].size && dst[d].host < src[s].host + src[s].size)
            fatal("nvn: CopyBufferToBuffer source and destination host backing alias");
        if (src[s].host + src[s].size <= dst[d].host) ++s;
        else ++d;
    }
}
void barrier(VkCommandBuffer cb, VkPipelineStageFlags src, VkPipelineStageFlags dst,
             VkAccessFlags src_access, VkAccessFlags dst_access) {
    VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b.srcAccessMask = src_access; b.dstAccessMask = dst_access;
    p_vkCmdPipelineBarrier(cb, src, dst, 0, 1, &b, 0, nullptr, 0, nullptr);
}
}  // namespace

void replay_buffer_copy(VkCommandBuffer cb, const Cmd& c) {
    validate(c);
    if (!c.a[2]) return;
    if (replay::written_separate())
        fatal("nvn: CopyBufferToBuffer requires shared GPU write tracking (unset NX_COMPUTE_SEPARATE)");
    // Preflight the entire operation before any GPU command or tracking change.
    // Different GPU addresses can alias the same physical/host bytes, including
    // a later mapping's source overlapping an earlier mapping's destination.
    auto src = backing(c.a[0], c.a[2]);
    auto dst = backing(c.a[1], c.a[2]);
    reject_alias_overlap(src, dst);
    draw_end_rendering(cb, 5);
    clears_flush(cb);
    barrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
            VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
    size_t s = 0, d = 0;
    uint64_t so = 0, doff = 0;
    while (s < src.size()) {
        uint64_t bytes = std::min(src[s].size - so, dst[d].size - doff);
        auto* sp = reinterpret_cast<const uint8_t*>(src[s].host + so);
        auto* dp = reinterpret_cast<const uint8_t*>(dst[d].host + doff);
        // read_upload consults written_read first, composing partially GPU-written
        // input with vkCmdCopyBuffer. Only previously unwritten bytes are uploaded
        // from host memory. written_bind preserves adjacent writes when merging.
        Upload source = replay::read_upload(cb, sp, bytes, bytes, 1);
        // The copy writes the whole destination range: no guest snapshot for it, and the write-back copies it as it is.
        Upload dest = replay::written_bind(cb, dp, bytes, bytes, /*fills_range=*/true);
        // NX_REPLAY_CUTS=barrier: source may be a composed copy; make it visible before this transfer reads it.
        replay::copy_write_barrier(cb);
        VkBufferCopy region{source.offset, dest.offset, bytes};
        p_vkCmdCopyBuffer(cb, source.buffer, dest.buffer, 1, &region);
        // Orders subsequent draws/dispatches and the next segment's tracking
        // merge copies; includes WAR ordering for a later writer of our source.
        barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
        so += bytes; doff += bytes;
        if (so == src[s].size) { ++s; so = 0; }
        if (doff == dst[d].size) { ++d; doff = 0; }
    }
}
NX_HLE("nvnCommandBufferCopyBufferToBuffer", CopyBufferToBuffer);
}  // namespace nx::nvn
