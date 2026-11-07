// Command memory/cache replay contracts. No guest memory is mutated while recording.
#pragma once
#include "nvn.h"

namespace nx::nvn {
struct CommandMemoryState {
    uint64_t scratch_pool = 0, scratch_offset = 0, scratch_size = 0, scratch_gpu = 0;
    bool tiled_cache_enabled = false, render_target_binning = false;
    uint32_t last_cache_action = 0;
    bool color_reduction_enabled = false;
    // Six SDK format classes; class 3 uses six-bit thresholds, others eight-bit.
    uint32_t color_reduction_thresholds[6][2] = {};
};
// Replay-thread state; resets at the start of a submit, persists across CallList.
CommandMemoryState& command_memory_state();
void reset_command_memory_state();
// Checks bounds without unsigned wrap. It does not touch scratch contents.
uint64_t checked_scratch_address(uint64_t pool, uint64_t offset, uint64_t size);
void replay_clear_buffer(VkCommandBuffer cb, const Cmd& command);
void replay_command_memory(VkCommandBuffer cb, const Cmd& command);
// Call at graphics draw execution, after all color/reference changes. ALWAYS
// ignores alpha_ref; other compares need fragment-shader alpha-test support.
void require_alpha_test_supported(const ColorStateInfo& color);
} // namespace nx::nvn
