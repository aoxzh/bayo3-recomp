// Ordered NVN buffer copies. The caller dispatches Cmd::CopyBufferToBuffer here.
#pragma once
#include "nvn.h"

namespace nx::nvn {
void replay_buffer_copy(VkCommandBuffer cb, const Cmd& cmd);
}  // namespace nx::nvn
