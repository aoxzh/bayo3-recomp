// Indexed indirect NVN record/replay. Arguments remain GPU data until Vulkan consumes them.
#pragma once
#include "nvn_vk.h"

namespace nx::nvn {
namespace replay { struct DrawInputs; }

// Enabled at device creation, not merely presence of the Vulkan entry point.
bool vk_draw_indirect_count();

struct IndexedIndirect {
    Upload args, count;
    uint32_t max_draws = 0, stride = sizeof(VkDrawIndexedIndirectCommand);
    bool counted = false;
};
bool is_indexed_indirect(const Cmd& c);
// NX_INDIRECT_PROF: where prepare_indexed_indirect's time goes, printed with the other per-window logs.
void indirect_prof_log(uint64_t frames);
// Call outside rendering, in place of draw_inputs_shared. Initializes vertex and
// index bindings plus argument/count uploads; false means a legitimate zero draw.
bool prepare_indexed_indirect(VkCommandBuffer cb, const ShaderInfo* vs, const Cmd& c,
                              replay::DrawInputs& in, IndexedIndirect& indirect);
// Call at the indexed draw emission site, after the ordinary pipeline, descriptor,
// dynamic, vertex and index bindings. Caller owns draw statistics/pass bookkeeping.
void emit_indexed_indirect(VkCommandBuffer cb, const IndexedIndirect& indirect);
}  // namespace nx::nvn
