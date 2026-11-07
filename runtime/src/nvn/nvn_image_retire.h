#pragma once

#include <memory>

namespace nx::nvn {
struct GpuImage;

// Transfer an image to the GPU retirement queue, through the submit currently
// being recorded. The caller may hold objects_lock(); this takes only the
// graveyard lock, so never call it while already holding that lock. Does not
// enter the image reuse pool. Vulkan handles are freed later under the GPU lock.
void retire_image(std::shared_ptr<GpuImage> image);
}  // namespace nx::nvn
