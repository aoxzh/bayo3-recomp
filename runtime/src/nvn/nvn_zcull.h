#pragma once
#include "nvn.h"

namespace nx::nvn {
// Cmd::ZCullBarrier: n=0 save / 1 restore, a[0]=GPU metadata address,
// a[1]=requested bytes. Vulkan owns hierarchical depth metadata, so only the
// zero-byte contract returned by TextureGetZCullStorageSize is supported.
void replay_zcull(VkCommandBuffer cb, const Cmd& command);
}  // namespace nx::nvn
