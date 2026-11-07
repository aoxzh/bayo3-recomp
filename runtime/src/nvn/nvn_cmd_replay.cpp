// B3 command memory operations. ClearBuffer uses the same ordered GPU copies as
// compute storage writes, so earlier draws retain their inputs and later draws
// see the fill. The normal submit fence/write-back path publishes guest bytes.
#include "nvn_cmd_replay.h"
#include "nvn_draw_state.h"
#include <algorithm>
#include <limits>

namespace nx::nvn {
using namespace nx::vk;
namespace {
CommandMemoryState state;
void memory_barrier(VkCommandBuffer cb, VkPipelineStageFlags src, VkPipelineStageFlags dst,
                    VkAccessFlags src_access, VkAccessFlags dst_access) {
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = src_access;
    barrier.dstAccessMask = dst_access;
    p_vkCmdPipelineBarrier(cb, src, dst, 0, 1, &barrier, 0, nullptr, 0, nullptr);
}
}
CommandMemoryState& command_memory_state() { return state; }
void reset_command_memory_state() { state = {}; }

void require_alpha_test_supported(const ColorStateInfo& color) {
#ifdef NX_BAYONETTA3
    // NVN compares 1..8 (NEVER through ALWAYS); there is no separate enable bit.
    // Vulkan has no fixed-function alpha test. Retain SetAlphaRef, but do not
    // silently draw enabled tests until fragment discard/compare is translated.
    if (color.alpha_test != 8)
        fatal("nvn: alpha test compare %u (reference %g) requires fragment shader support",
              color.alpha_test, replay::g_state.alpha_ref);
#endif
}

uint64_t checked_scratch_address(uint64_t pool, uint64_t offset, uint64_t size) {
    // A null, empty binding disables scratch. Otherwise the pool must exist,
    // ptrdiff_t offset must be nonnegative, and the complete range must fit.
    if (!pool && !offset && !size) return 0;
    auto* p = get<MemoryPool>(pool);
    if (!p || offset > uint64_t(INT64_MAX) || offset > p->size || size > p->size - offset ||
        offset > UINT64_MAX - p->gpu || size > UINT64_MAX - (p->gpu + offset))
        fatal("nvn: invalid shader scratch pool %llx offset %llx size %llx",
              (unsigned long long)pool, (unsigned long long)offset, (unsigned long long)size);
    return p->gpu + offset;
}

void replay_clear_buffer(VkCommandBuffer cb, const Cmd& c) {
    if (!c.a[1]) return;
    if ((c.a[0] | c.a[1]) & 3 || c.a[1] > UINT64_MAX - c.a[0])
        fatal("nvn: ClearBuffer requires a non-overflowing word-aligned range");
    if (replay::written_separate())
        fatal("nvn: ClearBuffer requires shared GPU write tracking (unset NX_COMPUTE_SEPARATE)");
    draw_end_rendering(cb, 5);
    clears_flush(cb);
    // Serialize every preceding reader/writer before transfer writes. written_bind
    // may also merge existing compute regions with GPU copies at this point.
    memory_barrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                   VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
    uint64_t address = c.a[0], remaining = c.a[1];
    while (remaining) {
        uint64_t offset = 0;
        MemoryPool* pool = pool_at_gpu(address, &offset);
        if (!pool || offset >= pool->size) fatal("nvn: ClearBuffer GPU address is not in a pool");
        uint64_t bytes = std::min(remaining, pool->size - offset);
        if (pool->memory && offset > UINT64_MAX - pool->memory)
            fatal("nvn: ClearBuffer host address overflows");
        uint64_t host = pool->memory ? pool->memory + offset : 0;
        // Virtual pools can map adjacent GPU ranges to unrelated physical memory.
        // Stop each fill at a mapping boundary rather than treating them as one pointer.
        if (!pool->memory) {
            MemoryPool::Mapping mapping;
            uint64_t within;
            {
                std::shared_lock lock(objects_lock());
                auto it = pool->mappings.upper_bound(offset);
                if (it == pool->mappings.begin()) fatal("nvn: ClearBuffer virtual range is unmapped");
                --it;
                mapping = it->second;
                within = offset - it->first;
            }
            if (within >= mapping.size) fatal("nvn: ClearBuffer virtual range has a hole");
            bytes = std::min(bytes, mapping.size - within);
            auto* physical = get<MemoryPool>(mapping.physical);
            if (!physical || !physical->memory || mapping.physical_offset > physical->size ||
                within > physical->size - mapping.physical_offset ||
                bytes > physical->size - mapping.physical_offset - within ||
                mapping.physical_offset + within > UINT64_MAX - physical->memory)
                fatal("nvn: ClearBuffer virtual range exceeds physical backing");
            host = physical->memory + mapping.physical_offset + within;
        }
        if (!host || !bytes || (bytes & 3) || host > UINT64_MAX - bytes ||
            bytes > uint64_t(std::numeric_limits<ptrdiff_t>::max()) - 0x11000)
            fatal("nvn: ClearBuffer backing range is invalid");
        auto* pointer = reinterpret_cast<const uint8_t*>(host);
        // The fill writes the whole range: no guest snapshot, and the write-back is the copy as it is.
        Upload dest = replay::written_bind(cb, pointer, bytes, bytes, /*fills_range=*/true);
        // NX_REPLAY_CUTS=barrier: a composed copy still reading this range has to finish before the fill writes it.
        replay::copy_write_barrier(cb);
        p_vkCmdFillBuffer(cb, dest.buffer, dest.offset, bytes, c.n);
        // Also orders a later overlapping mapping's merge copy after this fill.
        memory_barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                       VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
        address += bytes;
        remaining -= bytes;
    }
}

void replay_command_memory(VkCommandBuffer cb, const Cmd& c) {
    if (c.op == Cmd::SetColorReductionEnable || c.op == Cmd::SetColorReductionThresholds) {
        if (c.op == Cmd::SetColorReductionEnable) state.color_reduction_enabled = c.n != 0;
        else {
            if (c.n > 5) fatal("nvn: invalid color reduction format class");
            state.color_reduction_thresholds[c.n][0] = uint32_t(c.i[0]);
            state.color_reduction_thresholds[c.n][1] = uint32_t(c.i[1]);
        }
        // Disabled reduction, or zero thresholds, is native Vulkan attachment precision. Enabled nonzero thresholds
        // let Maxwell reduce the precision of blended color writes, an approximation Vulkan has no control for, so
        // they replay at native precision too: never less exact than the console. Program 1 (the Bayonetta Origins
        // prologue) enables them a couple of minutes in; this used to be fatal there. Logged once.
        static bool logged = false;
        if (state.color_reduction_enabled && !logged)
            for (const auto& thresholds : state.color_reduction_thresholds)
                if ((thresholds[0] || thresholds[1]) && !logged) {
                    logged = true;
                    log("nvn: color reduction enabled with nonzero thresholds: blending at native precision");
                }
        return;
    }
    if (c.op == Cmd::SetShaderScratchMemory) {
        uint64_t gpu = checked_scratch_address(c.a[0], c.a[1], c.a[2]);
        if (gpu != c.a[3]) fatal("nvn: shader scratch pool changed since recording");
        state.scratch_pool = c.a[0]; state.scratch_offset = c.a[1];
        state.scratch_size = c.a[2]; state.scratch_gpu = gpu;
        // NVN's scratch range backs Maxwell register spills. Translated SPIR-V
        // uses Vulkan compiler-managed private/spill memory. Keeping this range
        // contract does not alias shader-visible storage or pretend to clear it.
        return;
    }
    if (c.op != Cmd::SetTiledCacheAction || c.n < 1 || c.n > 6)
        fatal("nvn: invalid tiled cache action");
    // Vulkan owns hardware tiling. Complete/store the pass and pending clears,
    // then make attachment/other writes visible. This conservatively implements
    // flush and mode-transition boundaries without inventing a Tegra tile cache.
    draw_end_rendering(cb, 5);
    clears_flush(cb);
    memory_barrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                   VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
    switch (c.n) {
        case 1: state.tiled_cache_enabled = true; break; // ENABLE
        case 2: state.tiled_cache_enabled = false; break; // DISABLE
        case 3: break; // FLUSH preserves enable policy
        case 4: break; // FLUSH_NO_TILING: drain untiled; subsequent policy unchanged
        case 5: state.render_target_binning = true; break;
        case 6: state.render_target_binning = false; break;
    }
    state.last_cache_action = c.n;
}
} // namespace nx::nvn
