// Vulkan's image implementation owns hierarchical depth metadata. There is no
// separately addressable payload to save or restore; depth/stencil image bytes
// remain authoritative. Preserve command ordering at every metadata boundary.
#include "nvn_zcull.h"
#include "nvn_draw_state.h"

namespace nx::nvn {
namespace {
void TextureGetZCullStorageSize(CPU& cpu) {
    if (!get<Texture>(cpu.x[0])) fatal("nvn: TextureGetZCullStorageSize on an unknown texture");
    cpu.x[0] = 0;
}
void validate(const Cmd& command) {
    if (command.n > 1) fatal("nvn: invalid ZCull metadata operation");
    if (command.a[1])
        fatal("nvn: ZCull save/restore requested %llu metadata bytes; Vulkan exposes no transferable ZCull storage",
              static_cast<unsigned long long>(command.a[1]));
}
void zcull(CPU& cpu, uint32_t operation) {
    // Original SDK +0x4c9920 / +0x4c9a90: (cmd, GPU address, byte size).
    // main+cf23f0 / +cf2450 pass TextureGetZCullStorageSize directly as x2.
    Cmd command{Cmd::ZCullBarrier};
    command.n = operation;
    command.a[0] = cpu.x[1];
    command.a[1] = cpu.x[2];
    validate(command);  // fail before recording unsupported nonempty metadata
    record(cpu.x[0], command);
}
void SaveZCullData(CPU& cpu) { zcull(cpu, 0); }
void RestoreZCullData(CPU& cpu) { zcull(cpu, 1); }
}  // namespace

void replay_zcull(VkCommandBuffer cb, const Cmd& command) {
    validate(command);
    // Close/store attachments and materialize deferred clears before the
    // dependency. No image layouts or texels change and no storage is touched.
    // Ending rendering is required: this general memory dependency must not be
    // emitted inside a dynamic-rendering instance.
    draw_end_rendering(cb, 5);
    clears_flush(cb);
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT |
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    nx::vk::p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                 0, 1, &barrier, 0, nullptr, 0, nullptr);
}

NX_HLE("nvnTextureGetZCullStorageSize", TextureGetZCullStorageSize);
NX_HLE("nvnCommandBufferSaveZCullData", SaveZCullData);
NX_HLE("nvnCommandBufferRestoreZCullData", RestoreZCullData);
}  // namespace nx::nvn
