// The Vulkan side of NVN, shared between nvn_vk.cpp (device, images, submit, present) and nvn_draw.cpp (pipelines,
// resources, draws). Everything here runs under the GPU lock taken by gpu_submit / gpu_present.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "nvn.h"
#include "vk_gen.h"

namespace nx::nvn {

struct GpuImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;  // every level and layer
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    uint32_t width = 0, height = 0, levels = 1, layers = 1, depth = 1;  // depth: 3D images only
    VkImageViewType view_type = VK_IMAGE_VIEW_TYPE_2D;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    bool rendered = false;  // written by the GPU: never re-uploaded from guest memory
    bool uploaded = false;  // filled from the texture's pool memory
    uint64_t uploaded_write_version = 0;  // latest tracked buffer write included in this upload
    // Given texels by nvnTextureWriteTexels: those never reach guest memory here, so it is not uploaded from there
    // again (a pool flush over it wiped the light probes' direction map, docs/nvn/DEBUG.md #49).
    bool texels = false;
    VkComponentMapping swizzle{};  // for sampling (attachment views stay identity)
    bool storage = false;          // created with VK_IMAGE_USAGE_STORAGE_BIT (compute shaders write it)
    bool mutable_format = true;
    uint64_t rendered_at = 0;      // the pass-end serial of the last pass that rendered into it (nvn_draw.cpp, G4)    // views may use another format (MUTABLE_FORMAT; nvn_image.cpp, ubwc_flags)
    // An R8 texture always read as (1, 1, 1, R) (a glyph cache): stored as RGBA8 (255, 255, 255, R) and sampled
    // without swizzle. MoltenVK returned alpha 0 for that view swizzle on a 2D-array view, so all text was invisible.
    bool alpha_from_r = false;
    uint32_t nvn_format = 0;  // the texture's NVN format
    bool uw = false;          // NX_ULTRAWIDE: a widened render target (nvn_ultrawide.cpp)
};

bool gpu_ensure_initialized();
void gpu_finish_all();
VkDevice vk_device();
// Optional commands are used only after their features and entry points were enabled.
struct ExtendedDynamicCommands {
    PFN_vkCmdSetCullMode cull = nullptr;
    PFN_vkCmdSetFrontFace front = nullptr;
    PFN_vkCmdSetColorBlendEnableEXT blend_enable = nullptr;
    PFN_vkCmdSetColorBlendEquationEXT blend_equation = nullptr;
    PFN_vkCmdSetColorWriteMaskEXT color_mask = nullptr;
};
const ExtendedDynamicCommands& vk_extended_dynamic_commands();
bool vk_extended_dynamic();
bool vk_extended_dynamic_blend();
bool vk_graphics_pipeline_library();
bool vk_depth_clip_control();
bool vk_draw_indirect_count();
bool vk_depth_range_unrestricted();
// VK_EXT_vertex_attribute_divisor: NVN divides instanced streams by any N. When on, per-binding divisors above 1 are
// expressed through VkPipelineVertexInputDivisorStateCreateInfo; the maximum is what the driver accepts.
bool vk_vertex_divisor();
uint32_t vk_vertex_divisor_max();
// The core features the device was created with (all the GPU has): depthClamp, depthBiasClamp, depthBounds, ...
const VkPhysicalDeviceFeatures& vk_features();
// The device's properties, read once when the device was made. Prefer this to p_vkGetPhysicalDeviceProperties: the
// replay reads limits per indirect draw and per descriptor batch, and the driver call there is not free.
const VkPhysicalDeviceProperties& device_properties();
bool gpu_require_subgroup32();  // compute pipelines pin the subgroup size to 32 (nvn_pipeline.cpp)
bool gpu_is_integrated();  // the device in use is an integrated GPU (set when the device is made)
VkPhysicalDevice vk_physical();
// nvn_gpu_pick.cpp: logs every device and returns the one NX_GPU asks for (default: the first discrete GPU).
VkPhysicalDevice pick_physical_device(const std::vector<VkPhysicalDevice>& devices);
uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want);
// Properties of the selected memory type; zero when the type is unknown.
VkMemoryPropertyFlags memory_type_properties(uint32_t type);

// The image behind an NVN texture (guest NVNtexture*), made on first use.
GpuImage* image_of(uint64_t texture);
// NX_1D_IMAGES=1 (bisecting, nvn_pipeline.cpp): NVN 1D and 1D-array textures as Vulkan 1D images, the old way. By
// default they are 2D / 2D-array images of height 1, which the translated shaders (1D sampled as 2D) can view (G8).
bool vk_1d_images();
void transition(VkCommandBuffer cb, GpuImage& img, VkImageLayout to);
// nvn_present.cpp: the window texture drawn into the swapchain (rotated, fit, alpha 1) instead of blitted.
bool present_pass_wanted();
bool present_pass_targets(VkFormat format, const std::vector<VkImage>& images, VkExtent2D extent,
                          VkSurfaceTransformFlagBitsKHR transform);
bool present_pass_record(VkCommandBuffer cb, GpuImage& src, const int32_t* crop, uint32_t index);
void present_pass_release();  // its views of the swapchain images, before the swapchain is destroyed
bool present_pass_failed();   // the pass could not be made: frames are blitted, upright
GpuImage* present_pass_test_target();  // NX_PRESENT_TEST: an offscreen "display" for the pass (headless dumps)
// nvn_frame_overlay.cpp: the frame rate the present pass draws in the top left corner (NX_FPS_OVERLAY, on by default
// on Android). fps_overlay_frame() is called once per presented frame; fps_overlay_rate() is 0 while the readout is
// off or has no full second of frames yet.
void fps_overlay_frame();
double fps_overlay_rate();
// nvn_bc.cpp: BC textures decoded on the CPU where the GPU has no textureCompressionBC (NX_FORCE_BC_DECODE=1 anyway).
bool bc_host_decode(uint32_t nvn_format);         // a BC format this device cannot sample (BC6H excepted)
VkFormat bc_decoded_format(uint32_t nvn_format);  // what it is stored as instead
uint32_t bc_decoded_bytes(uint32_t nvn_format);   // bytes per texel of that
void bc_decode(uint32_t nvn_format, const uint8_t* blocks, uint32_t width, uint32_t height, uint32_t slices,
               uint8_t* out);  // tightly packed blocks -> tightly packed texels
bool bc_view_format(const GpuImage& img, uint32_t nvn, VkFormat& out);  // a view of a decoded image

// Per-submit upload memory (host visible, coherent), reset at every submit: data the replayed commands read from
// guest memory is copied here at replay time (vertex, index, uniform, storage data, texture staging).
struct Upload {
    VkBuffer buffer = VK_NULL_HANDLE;
    uint64_t offset = 0;
    uint8_t* ptr = nullptr;
    // The GPU writes guest memory itself (NX_HOST_IMPORT): no ring copy, no write-back, and ptr is guest memory.
    bool direct = false;
    // True only when the allocator knows this mapping's actual type is HOST_CACHED.
    bool host_cached = false;
};
// What it is bound as, and GPU copies within it and dispatch arguments read from it (nvn_written.cpp).
constexpr VkBufferUsageFlags kUploadUsage =
    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
    VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
Upload upload_alloc(uint64_t size, uint64_t align);
// NX_HOST_IMPORT=1 (VK_EXT_external_memory_host): the guest bytes themselves as a VkBuffer, page-aligned and cached per
// guest page. Empty when the device lacks the extension or the range cannot be imported; the caller keeps its copy
// path then (nvn_written.cpp).
bool vk_host_import();
// bytes: the range the binding uses; guest_bytes: the range the guest allocation owns (the import never covers a page
// past it). Empty when the import would need unowned guest pages.
Upload host_import_upload(const void* guest, uint64_t bytes, uint64_t guest_bytes);
// nvn_frames.cpp: a buffer of its own for one big texture upload, freed when the submit being recorded retires.
Upload staging_alloc(uint64_t size);
// nvn_pipeline_cache.cpp: the on-disk pipeline cache, and the cost of what the replay creates (logged per window).
VkPipelineCache pipeline_cache();
void pipeline_cache_save();
void pipeline_cache_flush();  // normal exit: finish the queued writes and save this session's last pipelines
void note_pipeline_created(uint64_t ns, bool worker);  // worker: compiled off the replay thread
void note_pipeline_alias();               // a new raw key that maps to a pipeline already built (canonical_key)
void note_module_created(uint64_t ns);    // a shader module built (SPIR-V patches + vkCreateShaderModule)
void note_pipeline_waited(uint64_t ns);   // the replay waited this long for a compile in flight
void note_pipeline_skipped();             // a draw skipped because its pipeline compiles (NX_ASYNC_PIPELINES)
void note_pipeline_prewarmed();           // a pipeline of an earlier session sent to a worker
void note_astc_decoded(uint64_t ns);
void creation_log(uint64_t frames);
Upload upload(const void* data, uint64_t size, uint64_t align);

// Descriptor sets for this submit (freed when it completes).
VkDescriptorSet descriptor_set(VkDescriptorSetLayout layout);
VkDescriptorPool make_descriptor_pool();
bool descriptor_layout_counts(VkDescriptorSetLayout layout, uint32_t (&by_type)[16]);  // descriptors of a layout, by VkDescriptorType (nvn_pipeline.cpp)

// A lost device or a submit running past 2 s: the process ends at once (docs/nvn/DEBUG.md #22).
[[noreturn]] void gpu_hung(const char* why);

// A compute shader's storage buffer, copied back to guest memory once the submit completed: (upload copy, guest
// address, size).
struct WriteBack {
    uint8_t* from;
    uint8_t* to;
    uint64_t size;
    // The guest bytes as they were when the copy was made: only what the GPU changed goes back (nvn_written.cpp).
    std::shared_ptr<std::vector<uint8_t>> before;
    // The GPU wrote every word of the range: copy it back whole, and there is no snapshot to merge against.
    bool whole = false;
    // The guest was released before this landed (NX_EARLY_COMPLETE): what it wrote since comes after the GPU's write.
    bool late = false;
    // NX_CARRY (nvn_frames.cpp): the GPU's copy, how many bytes it spans, and written_writable_bind_count() at the last
    // bind that could write it. A count past the submit's split means the later parts may still be writing it.
    VkBuffer buffer = VK_NULL_HANDLE;
    uint64_t offset = 0, cap = 0, wrote = 0;
    // Filled with the GPU's bytes at the landing: the snapshot of the next submit's copy of this range.
    std::shared_ptr<std::vector<uint8_t>> after;
};
// nvn_written.cpp: every compute result of a submit, copied back on NX_COPY_THREADS helpers (slice-parallel; the
// clean/whole learning, the NaN trace and guest-memory order within a range stay correct).
void write_backs_apply(std::vector<WriteBack>& v);
// NX_EARLY_COMPLETE (nvn_frames.cpp): a submit releases the guest before the GPU has run it.
// On by default on Linux and Android; NX_EARLY_COMPLETE=0 disables it and =1 enables it elsewhere.
inline bool early_complete() {
    static const bool on = [] {
        const char* e = std::getenv("NX_EARLY_COMPLETE");
        if (e) return e[0] == '1' && !e[1];
#if defined(__linux__) || defined(__ANDROID__)
        return true;
#else
        return false;
#endif
    }();
    return on;
}
// Every range has a snapshot to merge against, so these can land after the guest has been released (nvn_frames.cpp):
// a word the guest changes in the meantime is told from one the GPU changed.
bool write_backs_can_land_late(const std::vector<WriteBack>& v);
std::vector<WriteBack> draw_take_write_backs();  // nvn_draw.cpp: this submit's, for an asynchronous submit
void debug_dump_textures(uint64_t frame);  // nvn_debug.cpp: NX_DUMP_TEXTURES and the requested ones
void debug_dump_requested(uint64_t n);      // nvn_debug.cpp: NX_DUMP_ON_TARGET / NX_DUMP_ON_FS
void debug_dump_add(uint64_t texture);  // nvn_debug.cpp: read back at the end of this submit
// nvn_vk.cpp: the NVNtexture the game presented last (NX_TRACE_PRESENT: what the screen shows; nvn_draw_trace.cpp
// traces the draws into it and nvn_draw.cpp dumps their target and the images they sample).
uint64_t presented_texture();
bool presented_recently(uint64_t texture);  // one of the last four presented (the game double-buffers presents)
void note_presented_texture(uint64_t texture);
// NX_TRACE_PRESENT, who writes the presented texture (nvn_draw_trace.cpp / nvn_copy.cpp / nvn_pipeline.cpp): the draws
// into it are traced, and every other way an image is used is named once, so a black or stale presented frame can be
// explained ("nothing touched it" reads very differently from "a copy of an empty image filled it").
bool present_trace_on();  // the start marker (`trace_present` file, or NX_TRACE_FROM) has fired
void present_touch(const char* what, uint64_t texture);
void debug_dump_request_presented(uint64_t a, uint64_t b);  // nvn_draw.cpp: read a and b back after this submit

// nvn_frames.cpp: submits that are not waited for (NX_PIPELINED, NX_ASYNC_GPU). Slots of command buffers, fences,
// descriptor pool and ring stretch; retired (fences waited, write-backs copied if not yet) on reuse, when the ring is
// full, or when something waits for them.
bool async_gpu();  // the slots are in use (either mode)
bool pipelined();  // NX_PIPELINED: fences not waited for, write-backs copied before gpu_submit returns
// The extensions and memory types that the Android performance plan depends on, logged once (nvn_caps.cpp).
void log_device_caps(VkPhysicalDevice physical, const std::vector<VkExtensionProperties>& extensions);
uint32_t upload_memory_type(uint32_t bits);
void upload_cost_log(uint64_t frames);  // nvn_upload.cpp: where texture uploads spent their time
bool native_r11g11b10();
bool ubwc_flags();  // MUTABLE_FORMAT only with a format list (nvn_caps.cpp; Android)  // NVN 0x3f as B10G11R11_UFLOAT (nvn_caps.cpp; Android)  // the upload ring's memory type (nvn_caps.cpp)
void frame_init(VkCommandPool pool, uint32_t family, VkBuffer ring, uint8_t* ring_ptr, uint64_t ring_size,
                bool ring_host_cached = false);
VkCommandBuffer frame_begin(bool replay);  // NX_ASYNC_GPU, replay: submits with write-backs retired first
VkSemaphore frame_acquire_semaphore();
VkSemaphore frame_rendered_semaphore(uint32_t image);
// `completions`: guest-visible work that must run once this submit's GPU work is done (the embedded command fences
// of arm_command_fences). They run on the retiring thread -- never on the replay thread right after queueing, which
// would wait for the submit that was just queued and serialise recording with the GPU.
void frame_submit(VkSemaphore wait, VkSemaphore signal, std::vector<WriteBack> write_backs, bool wait_done,
                  std::vector<std::function<void()>> completions = {});
// Drives retirement from a guest thread blocked on a deferred command fence (it cannot submit, so it must retire).
bool frame_retire_one();
void frame_wait_all();
void frame_carry_wait();  // NX_CARRY: the submit behind this one's carried ranges landed: their guest bytes can be read
void frame_finish_fenced(); // same completion semantics, separately measured fence tail
uint64_t frame_upload_used();
uint64_t frame_ring_size();  // the slots' upload ring (0: synchronous submits, no ring)
uint64_t frame_completed();  // the serial of the last retired submit (for free_retired_images)
Upload frame_upload_alloc(uint64_t size, uint64_t align);
VkDescriptorSet frame_descriptor_set(VkDescriptorSetLayout layout);
// Pipelined: a submit with dispatches is split after the last one; its first part is queued at once and carries the
// write-backs. frame_plan_split counts the submit's DispatchCompute commands and returns the last top-level list that
// has one (SIZE_MAX: no split). frame_split ends the first part and returns the command buffer of the rest (once per
// submit; outside the mode it returns `cb`). frame_after_dispatch is for replay_list, after each DispatchCompute: it
// splits right after the last one. frame_cb: the command buffer to go on with (after a call that may have split).
size_t frame_plan_split(const std::vector<std::shared_ptr<CmdList>>& lists);
VkCommandBuffer frame_split(VkCommandBuffer cb);
VkCommandBuffer frame_after_dispatch(VkCommandBuffer cb);
VkCommandBuffer frame_cb(VkCommandBuffer cb);
// Chunks (nvn_frames.cpp, NX_SUBMIT_CHUNK_DRAWS): for replay_list before each command, with the draws done so far;
// returns the command buffer to go on with (a new part once enough draws were recorded).
VkCommandBuffer frame_chunk(VkCommandBuffer cb, uint64_t draws, bool pass_open);
// Host time inside vkQueueSubmit (with vkEndCommandBuffer: Metal encoding) and inside vkWaitForFences, running
// totals in every mode (the synchronous path adds its own).
struct FrameTimes {
    uint64_t encode_ns = 0, fence_ns = 0;
};
FrameTimes& frame_times();
void frame_log(uint64_t frames);  // the mode's counters, with the timing log
void free_retired_images(uint64_t completed);
// A GPU-written image of a destroyed texture with the same memory and shape (nvn_frames.cpp), or null.
std::shared_ptr<GpuImage> reuse_image(const Texture& t);  // images of textures destroyed before submit `completed` + 1

// nvn_debug.cpp: NX_DUMP_FRAMES, the presented texture read back (recorded into the present's command buffer, written
// after its fence).
uint32_t frame_dump_every();
bool frame_dump_due(uint64_t presents);  // NX_DUMP_FRAMES, or a dump_frames file in the data folder
bool frame_dump_record(VkCommandBuffer cb, GpuImage& img);
void frame_dump_write(const GpuImage& img, uint64_t frame);
bool debug_snapshots_pending();  // NX_SNAP_*: this submit recorded snapshots (it is then waited for)

// nvn_draw.cpp: replays one draw-related command; ends the render pass it may have open when asked.
struct Targets {
    uint64_t colors[8] = {};
    uint32_t color_count = 0;
    uint64_t depth = 0;
    TargetViews views;  // level and layer of each (index 8: depth)
};
// Field by field: a memcmp also compared the padding after color_count, which a copy need not keep.
inline bool targets_same(const Targets& a, const Targets& b) {
    if (a.color_count != b.color_count || a.depth != b.depth) return false;
    for (uint32_t k = 0; k < 9; k++) {
        if (k < 8 && a.colors[k] != b.colors[k]) return false;
        if (a.views.level[k] != b.views.level[k] || a.views.layer[k] != b.views.layer[k] ||
            a.views.format[k] != b.views.format[k])
            return false;
    }
    return true;
}
void draw_begin_submit();
void draw_end_submit();  // after the GPU finished the submit
// window: the presents this block covers (60 early in a run, 300 after). It is passed rather than assumed: the
// per-frame figures in draw_log_stats were divided by a hardcoded 300 while the first windows are 60 presents, so
// early windows read 5x low.
void draw_log_stats(uint64_t window);
void draw_command(VkCommandBuffer cb, const Cmd& c, const CmdList& list, const Targets& targets);
// reason (counted in the `nvn passes ended` log): 0 submit end, 1 draw, 2 dispatch, 3 clear of other targets,
// 4 clear larger than the pass, 5 copy, 6 snapshot, 7 SetRenderTargets (NX_TARGETS_BREAK).
void draw_end_rendering(VkCommandBuffer cb, int reason = 0);  // before transfers (clears, copies, present) and at the end
// ClearColor / ClearDepthStencil inside the render pass of the current targets (nvn_draw.cpp).
void draw_clear(VkCommandBuffer cb, const Targets& targets, const Cmd& c);  // nvn_clears.cpp
// nvn_clears.cpp: clears pending as the next pass's loadOps; nvn_draw.cpp: the open pass, for it.
bool clears_pending_image(const GpuImage* img);
bool clears_pending_other(const Targets& t);
uint32_t clears_apply(const Targets& t, VkRenderingAttachmentInfo* colors, VkRenderingAttachmentInfo& depth,
                      VkRenderingAttachmentInfo& stencil);
void clears_flush(VkCommandBuffer cb);  // outside a pass: the pending clears in a pass of their own
void clears_begin_submit();
void clears_log();
bool pass_open();  // a render pass is being recorded
// Render pass and framebuffer objects instead of dynamic rendering (nvn_renderpass.cpp, NX_RENDER_PASS_OBJECTS).
bool render_pass_objects();
VkFormat unused_slot_format();  // a pipeline's format for a colour slot with no image
VkRenderPass compatible_render_pass(uint32_t count, const VkFormat* colors, VkFormat depth);
void begin_render_pass(VkCommandBuffer cb, uint32_t count, const VkFormat* formats,
                       const VkRenderingAttachmentInfo* colors, VkFormat depth_format,
                       const VkRenderingAttachmentInfo& depth, const VkRenderingAttachmentInfo& stencil,
                       VkExtent2D extent);
void forget_framebuffers(VkImageView view);
void render_pass_log();
void pass_end_only(VkCommandBuffer cb, int reason);  // draw_end_rendering without the pending clears
bool pass_open_on(const Targets& t);
VkExtent2D pass_extent();
void pass_begin(VkCommandBuffer cb, const Targets& t);
void pass_mark_written(uint32_t bits);
// nvn_mask_clear.cpp: colour target `slot` of the open pass on `t` cleared to `rgba` in `rect`, only the channels in
// `mask` (bit 0 r .. 3 a) written, by a fullscreen draw. False (nothing recorded) for integer targets or no pipeline.
bool mask_clear(VkCommandBuffer cb, const Targets& t, uint32_t slot, uint32_t mask, const float rgba[4],
                const VkRect2D& rect);
// nvn_replay.cpp: a command list into `cb` (`depth`: CallList nesting).
void replay_list(VkCommandBuffer cb, Targets& st, const CmdList& list, int depth);
// nvn_debug.cpp: the last replayed NVN commands, for the crash handler (kernel_watch.cpp). The ring is lock-free and
// allocation-free so a signal handler can read it. NX_CRASH_NVN=0 turns recording off.
void crash_nvn_note(const Cmd& command, const CmdList& list, int depth, uint64_t draws_so_far);
size_t crash_nvn_snapshot(char* out, size_t cap);  // formatted lines, oldest first; bytes written
std::shared_ptr<CmdList> submit_command_list(uint64_t handle);  // command_list(), once per handle per submit
void submit_lists_reset();  // at each submit's start (draw_begin_submit)
// nvn_replay.cpp: CallList bookkeeping for the recorded/replayed draw ratio in draw_log_stats (see nvn_replay.cpp).
void list_calls_log();
void submit_lists_capture(std::shared_ptr<const CommandListTable> captured);  // replay thread under GPU lock

void replay_copy(VkCommandBuffer cb, const Cmd& c, const CmdList& list);  // nvn_copy.cpp: texture copies
void forget_image_views(const GpuImage& img);
// nvn_debug.cpp: NX_SNAP_DRAWS mid-frame snapshots of colour target 0.
bool debug_snap_wanted(uint64_t draw);
// nvn_debug.cpp: NX_ZERO_DRAWS, shader pairs whose draws produce no pixel.
bool zero_draws_on();
void zero_draws_begin_submit(VkCommandBuffer cb);
bool zero_draws_begin(VkCommandBuffer cb, const std::string& label);
void zero_draws_end(VkCommandBuffer cb);
void zero_draws_collect();
void zero_draws_log();
void frame_sig_flush(uint64_t presents);  // NX_FRAME_SIG=1: the frame's draw signature, logged (nvn_draw_trace.cpp)
void debug_snapshot(VkCommandBuffer cb, GpuImage& img, uint64_t draw);
extern uint32_t g_snap_layer, g_snap_level;  // the layer and level debug_snapshot reads (default 0)
void debug_snapshots_write();  // nvn_pipeline.cpp: before an image is freed

// nvn_debug.cpp: GPU time per render pass (NX_GPU_PROFILE=1).
bool gpu_profile_on();  // NX_GPU_PROFILE=1: per-pass labels, every submit waited for (nvn_debug.cpp)
// NX_GPU_PROFILE=3: the GPU's busy time per frame without waits, from per-part timestamps (nvn_gpu_busy.cpp).
// The janitor thread (nvn_janitor.cpp, NX_JANITOR): pool resets of retired slots, off the replay thread.
bool janitor_on();
void janitor_run(std::function<void()> job);
void janitor_wait(const std::atomic<bool>& done);
bool gpu_busy_on();
void gpu_busy_part_begin(uint32_t slot, uint32_t part, VkCommandBuffer cb);
void gpu_busy_part_end(uint32_t slot, uint32_t part, VkCommandBuffer cb);
void gpu_busy_retired(uint32_t slot, uint32_t queued);
void gpu_busy_log(uint64_t frames);
void gpu_profile_begin(VkCommandBuffer cb);
void gpu_profile_mark(VkCommandBuffer cb, const std::string& label);
void gpu_profile_collect();
void gpu_profile_log(uint64_t frames);

namespace replay {
void written_carry_seed(VkCommandBuffer cb);  // NX_CARRY (nvn_written.cpp): the submit that began takes the carried ranges
void dedup_reset();  // nvn_dedup.cpp
uint64_t dedup_generation();  // changes when submit reset or ring reuse invalidates retained uploads
bool written_separate();         // nvn_written.cpp (nvn_draw_state.h)
uint64_t written_bind_count();   // nvn_written.cpp: written_bind calls so far (a dispatch bound storage it writes)
uint64_t written_writable_bind_count();  // the same count, only for a shader that writes the bound range
// nvn_written.cpp: the split guard in its exact form -- see the comment there.
uint64_t written_version();
uint64_t written_newest_version();  // the newest version among the ranges the last written_take() handed back
bool written_complete_since(uint64_t version);
// nvn_written.cpp, NX_REPLAY_CUTS: which replay-encoding cuts are on. Unset: the two proven ones (the composed read's
// deferred visibility barrier and its per-submit repeat cache). 0: none, the behaviour before the cuts. A value naming
// cuts ("barrier", "cache", "pipeline") enables exactly those, so one arm can test one cut at a time.
enum : uint32_t { kCutCopyBarrier = 1, kCutComposedCache = 2, kCutSplitGuard = 4 };
uint32_t replay_cuts();
void trace_nan_write_back(const uint8_t* from, const uint8_t* to, uint64_t size);  // nvn_draw_trace.cpp
}

}  // namespace nx::nvn
