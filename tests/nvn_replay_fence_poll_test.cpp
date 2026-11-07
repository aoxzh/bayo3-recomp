// Exercise the production frame retirement code with deterministic Vulkan and janitor doubles.
#include "../runtime/src/nvn/nvn_frames.cpp"
#include <deque>
#include <stdexcept>
#include <string>
#include <unordered_map>

using namespace nx;
using namespace nx::nvn;

namespace {
template <class T> T handle(uintptr_t n) { return reinterpret_cast<T>(n); }
void check(bool b, const char* text) { if (!b) throw std::runtime_error(text); }
std::unordered_map<VkFence, VkResult> results;
std::vector<std::string> events;
std::deque<std::function<void()>> janitor_jobs;
uint32_t status_calls = 0, wait_calls = 0, queue_calls = 0, janitor_joins = 0, destroyed_buffers = 0;
uint32_t pool_creates = 0, pool_fail_at = 0, fence_creates = 0, semaphore_creates = 0, command_buffer_resets = 0;
VkResult pool_create_error = VK_ERROR_OUT_OF_HOST_MEMORY;
std::vector<VkCommandPool> created_pools, allocation_pools, reset_pools;
bool use_janitor = false;
VkResult wait_result = VK_SUCCESS, queue_result = VK_SUCCESS, end_result = VK_SUCCESS, reset_fence_result = VK_SUCCESS;
VkResult command_pool_result = VK_SUCCESS, descriptor_pool_result = VK_SUCCESS, command_buffer_result = VK_SUCCESS;
uint8_t ring[64] = {};
uint8_t staging_bytes[64] = {};
uint32_t staging_type = 0, staging_failure = 0;

VKAPI_ATTR VkResult VKAPI_CALL create_staging_buffer(VkDevice, const VkBufferCreateInfo*,
    const VkAllocationCallbacks*, VkBuffer* out) {
    if (staging_failure == 1) return VK_ERROR_OUT_OF_HOST_MEMORY;
    *out = handle<VkBuffer>(9000); return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL staging_requirements(VkDevice, VkBuffer, VkMemoryRequirements* out) {
    *out = {sizeof staging_bytes, 16, 3};
}
VKAPI_ATTR VkResult VKAPI_CALL allocate_staging_memory(VkDevice, const VkMemoryAllocateInfo* info,
    const VkAllocationCallbacks*, VkDeviceMemory* out) {
    check(info->memoryTypeIndex == staging_type, "staging uses selected memory type");
    if (staging_failure == 2) return VK_ERROR_OUT_OF_HOST_MEMORY;
    *out = handle<VkDeviceMemory>(9001); return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL bind_staging_memory(VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize) {
    return staging_failure == 3 ? VK_ERROR_MEMORY_MAP_FAILED : VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL map_staging_memory(VkDevice, VkDeviceMemory, VkDeviceSize, VkDeviceSize,
    VkMemoryMapFlags, void** out) {
    if (staging_failure == 4) return VK_ERROR_MEMORY_MAP_FAILED;
    *out = staging_bytes; return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL get_status(VkDevice, VkFence f) { ++status_calls; return results[f]; }
VKAPI_ATTR VkResult VKAPI_CALL wait_fences_stub(VkDevice, uint32_t n, const VkFence*, VkBool32 all, uint64_t) {
    ++wait_calls;
    check(n > 0 && all, "wait all submitted fences");
    return wait_result;
}
VKAPI_ATTR VkResult VKAPI_CALL create_command_pool(VkDevice, const VkCommandPoolCreateInfo* info,
    const VkAllocationCallbacks*, VkCommandPool* out) {
    check(info->queueFamilyIndex == 23 && info->flags == 0, "per-slot command pool flags and queue family");
    ++pool_creates;
    if (pool_creates == pool_fail_at) return pool_create_error;
    *out = handle<VkCommandPool>(1000 + pool_creates);
    created_pools.push_back(*out);
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL allocate_command_buffers(VkDevice, const VkCommandBufferAllocateInfo* info,
    VkCommandBuffer* buffers) {
    check(info->level == VK_COMMAND_BUFFER_LEVEL_PRIMARY && info->commandBufferCount > 0, "command buffer allocation");
    allocation_pools.push_back(info->commandPool);
    for (uint32_t i = 0; i < info->commandBufferCount; ++i)
        buffers[i] = handle<VkCommandBuffer>(2000 + 100 * allocation_pools.size() + i);
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL create_fence(VkDevice, const VkFenceCreateInfo*, const VkAllocationCallbacks*, VkFence* out) {
    *out = handle<VkFence>(3000 + ++fence_creates); return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL create_semaphore(VkDevice, const VkSemaphoreCreateInfo*, const VkAllocationCallbacks*, VkSemaphore* out) {
    *out = handle<VkSemaphore>(4000 + ++semaphore_creates); return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL reset_command_pool(VkDevice, VkCommandPool pool, VkCommandPoolResetFlags) {
    reset_pools.push_back(pool); events.push_back("command-reset"); return command_pool_result;
}
VKAPI_ATTR VkResult VKAPI_CALL reset_descriptor_pool(VkDevice, VkDescriptorPool, VkDescriptorPoolResetFlags) {
    events.push_back("descriptor-reset"); return descriptor_pool_result;
}
VKAPI_ATTR VkResult VKAPI_CALL reset_command_buffer(VkCommandBuffer, VkCommandBufferResetFlags) { ++command_buffer_resets; return command_buffer_result; }
VKAPI_ATTR VkResult VKAPI_CALL begin_command_buffer(VkCommandBuffer, const VkCommandBufferBeginInfo*) { return VK_SUCCESS; }
VKAPI_ATTR void VKAPI_CALL pipeline_barrier(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags, VkDependencyFlags,
    uint32_t, const VkMemoryBarrier*, uint32_t, const VkBufferMemoryBarrier*, uint32_t, const VkImageMemoryBarrier*) {}
VKAPI_ATTR VkResult VKAPI_CALL end_command_buffer(VkCommandBuffer) { return end_result; }
VKAPI_ATTR VkResult VKAPI_CALL reset_fences(VkDevice, uint32_t, const VkFence*) { return reset_fence_result; }
VKAPI_ATTR VkResult VKAPI_CALL queue_submit(VkQueue, uint32_t, const VkSubmitInfo*, VkFence) { ++queue_calls; return queue_result; }
VKAPI_ATTR void VKAPI_CALL destroy_buffer(VkDevice, VkBuffer, const VkAllocationCallbacks*) { ++destroyed_buffers; }
VKAPI_ATTR void VKAPI_CALL free_memory(VkDevice, VkDeviceMemory, const VkAllocationCallbacks*) {}
VKAPI_ATTR void VKAPI_CALL destroy_image_view(VkDevice, VkImageView, const VkAllocationCallbacks*) {}
VKAPI_ATTR void VKAPI_CALL destroy_image(VkDevice, VkImage, const VkAllocationCallbacks*) {}
}

namespace nx {
std::atomic<uint64_t> g_frame{0};
void log(const char*, ...) {}
}
namespace nx::plat {
bool late_guard_on() { return false; }
uint64_t monotonic_ns() { static uint64_t n = 0; return ++n; }
int64_t current_thread_id() { return 1; }
}
namespace nx::vk {
PFN_vkCreateBuffer p_vkCreateBuffer = create_staging_buffer;
PFN_vkGetBufferMemoryRequirements p_vkGetBufferMemoryRequirements = staging_requirements;
PFN_vkAllocateMemory p_vkAllocateMemory = allocate_staging_memory;
PFN_vkBindBufferMemory p_vkBindBufferMemory = bind_staging_memory;
PFN_vkMapMemory p_vkMapMemory = map_staging_memory;
PFN_vkCreateCommandPool p_vkCreateCommandPool = create_command_pool;
PFN_vkAllocateCommandBuffers p_vkAllocateCommandBuffers = allocate_command_buffers;
PFN_vkCreateFence p_vkCreateFence = create_fence;
PFN_vkCreateSemaphore p_vkCreateSemaphore = create_semaphore;
PFN_vkGetFenceStatus p_vkGetFenceStatus = get_status;
PFN_vkWaitForFences p_vkWaitForFences = wait_fences_stub;
PFN_vkResetCommandPool p_vkResetCommandPool = reset_command_pool;
PFN_vkResetDescriptorPool p_vkResetDescriptorPool = reset_descriptor_pool;
PFN_vkResetCommandBuffer p_vkResetCommandBuffer = reset_command_buffer;
PFN_vkBeginCommandBuffer p_vkBeginCommandBuffer = begin_command_buffer;
PFN_vkCmdPipelineBarrier p_vkCmdPipelineBarrier = pipeline_barrier;
PFN_vkEndCommandBuffer p_vkEndCommandBuffer = end_command_buffer;
PFN_vkResetFences p_vkResetFences = reset_fences;
PFN_vkQueueSubmit p_vkQueueSubmit = queue_submit;
PFN_vkDestroyBuffer p_vkDestroyBuffer = destroy_buffer;
PFN_vkFreeMemory p_vkFreeMemory = free_memory;
PFN_vkDestroyImageView p_vkDestroyImageView = destroy_image_view;
PFN_vkDestroyImage p_vkDestroyImage = destroy_image;
}
namespace nx::nvn {
VkDevice vk_device() { return handle<VkDevice>(1); }
uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want) {
    check((bits & (1u << staging_type)) != 0 &&
          want == (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
          "staging keeps memory selection requirements");
    return staging_type;
}
uint32_t upload_memory_type(uint32_t bits) {
    return memory_type(bits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
}
void upload_cost_log(uint64_t) {}
void worker_set_idle(void (*)()) {}
bool write_backs_can_land_late(const std::vector<WriteBack>&) { return false; }
namespace replay { void late_guard_arm(const std::vector<WriteBack>&) {} }
VkMemoryPropertyFlags memory_type_properties(uint32_t type) {
    check(type < 2, "staging queries actual selected type");
    return VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
           (type == 1 ? VK_MEMORY_PROPERTY_HOST_CACHED_BIT : 0);
}
Upload upload_alloc(uint64_t size, uint64_t align) { return frame_upload_alloc(size, align); }
VkQueue vk_queue() { return handle<VkQueue>(2); }
std::recursive_mutex& gpu_lock() { static std::recursive_mutex m; return m; }
bool gpu_is_integrated() { return false; }
[[noreturn]] void gpu_hung(const char* why) { throw std::runtime_error(why); }
void gpu_busy_retired(uint32_t, uint32_t) {}
void gpu_busy_part_begin(uint32_t, uint32_t, VkCommandBuffer) {}
void gpu_busy_part_end(uint32_t, uint32_t, VkCommandBuffer) {}
void pass_end_only(VkCommandBuffer, int) {}
void forget_image_views(const GpuImage&) {}
bool janitor_on() { return use_janitor; }
void janitor_run(std::function<void()> job) { janitor_jobs.push_back(std::move(job)); }
void janitor_wait(const std::atomic<bool>& done) {
    ++janitor_joins;
    while (!done.load(std::memory_order_acquire) && !janitor_jobs.empty()) {
        auto job = std::move(janitor_jobs.front()); janitor_jobs.pop_front(); job();
    }
    check(done.load(std::memory_order_acquire), "slot reuse waited for janitor");
}
void write_backs_apply(std::vector<WriteBack>& writes) {
    events.push_back("writeback");
    for (const auto& w : writes) std::memcpy(w.to, w.from, size_t(w.size));
    writes.clear();
}
namespace replay {
void dedup_reset() {}
uint32_t replay_cuts() { return 0; }
uint64_t written_bind_count() { return 0; }
uint64_t written_writable_bind_count() { return 0; }
uint64_t written_version() { return 0; }
bool carry_ok = true;
std::vector<WriteBack> carry_land;
bool written_carry_begin(std::vector<WriteBack>& all, uint64_t, std::vector<WriteBack>& land_now) {
    if (!carry_ok) return false;
    land_now = std::move(carry_land);
    carry_land.clear();
    (void)all;
    return true;
}
bool written_complete_since(uint64_t) { return true; }
void written_kept_invalidate_all() {}
}
}

namespace {
void reset() {
    results.clear(); events.clear(); janitor_jobs.clear();
    created_pools.clear(); allocation_pools.clear(); reset_pools.clear();
    status_calls = wait_calls = queue_calls = janitor_joins = destroyed_buffers = 0;
    pool_creates = pool_fail_at = fence_creates = semaphore_creates = command_buffer_resets = 0;
    pool_create_error = VK_ERROR_OUT_OF_HOST_MEMORY;
    wait_result = queue_result = end_result = reset_fence_result = VK_SUCCESS;
    command_pool_result = descriptor_pool_result = command_buffer_result = VK_SUCCESS; use_janitor = false;
    nx::vk::p_vkGetFenceStatus = get_status;
    g_slot_count = 4; g_part_count = 3; g_next = 0; g_current = &g_slots[3];
    g_submitted = g_completed = g_barrier = 0;
    g_chunking = false; g_slot_pools = true;
    g_replay_poll_commands = 0; g_replay_poll_failed = g_poll_images_pending = false;
    g_replay_poll_stats = {};
    g_ring = handle<VkBuffer>(7); g_ring_ptr = ring; g_ring_size = sizeof ring;
    g_ring_host_cached = false; staging_type = staging_failure = 0;
    g_head = 64; g_tail = g_begin = 0;
    g_landing = 0; g_carry_next = false; g_tail_hold = UINT64_MAX; g_tail_held_by = nullptr;
    g_grave.clear(); g_reusable.clear(); g_staging.clear(); g_staging_free.clear(); g_staging_free_bytes = 0;
    for (uint32_t i = 0; i < g_slot_count; ++i) {
        Slot& s = g_slots[i];
        s.serial = s.ring_end = 0; s.queued = s.part = s.waited = 0;
        s.split_part = -1; s.ok = true; s.replay = false; s.carried = false; s.ring_begin = 0;
        s.pool = handle<VkCommandPool>(10 + i); s.pool_used = false;
        s.descriptors = handle<VkDescriptorPool>(20 + i); s.descriptors_used = false;
        s.sets_used = 0; std::fill(std::begin(s.descs_used), std::end(s.descs_used), 0u);
        s.reset_done.store(true); s.write_backs.clear(); s.completions.clear();
        for (uint32_t p = 0; p < g_part_count; ++p) {
            s.cbs[p] = handle<VkCommandBuffer>(100 + 10*i + p);
            s.fences[p] = handle<VkFence>(200 + 10*i + p);
            results[s.fences[p]] = VK_SUCCESS;
        }
    }
}
Slot& submit_slot(uint32_t i, uint64_t serial, uint32_t queued, uint64_t ring_end) {
    Slot& s = g_slots[i]; s.serial = serial; s.queued = queued; s.replay = true; s.ring_end = ring_end;
    g_submitted = std::max(g_submitted.load(), serial); return s;
}
void chunks(unsigned count) {
    auto cb = g_current->cbs[g_current->part];
    while (count--) check(frame_chunk(cb, 0, false) == cb, "chunk preserves recording buffer");
}
void expect_hung(const std::function<void()>& f, const char* message) {
    bool threw = false;
    try { f(); } catch (const std::runtime_error&) { threw = true; }
    check(threw, message);
}
void test_disabled() {
    reset();
    Slot& s = submit_slot(0, 1, 1, 32);
    s.completions.push_back([] { events.push_back("done"); });
    chunks(512);
    check(status_calls == 0 && s.serial == 1 && events.empty(), "polling is opt-in");
}
void test_cadence_fifo_and_resources() {
    reset(); use_janitor = true;
    Slot& first = submit_slot(0, 1, 3, 32);
    Slot& second = submit_slot(1, 2, 1, 48);
    first.pool_used = first.descriptors_used = true;
    second.pool_used = second.descriptors_used = true;
    first.completions.push_back([] { events.push_back("first"); });
    second.completions.push_back([] { events.push_back("second"); });
    results[first.fences[1]] = VK_NOT_READY;
    g_staging.push_back({1, StagingBlock{handle<VkBuffer>(501), handle<VkDeviceMemory>(502), nullptr, 64, false}});
    chunks(255);
    check(status_calls == 0 && g_tail == 0, "no polling before 256 commands");
    chunks(1);
    check(status_calls == 2 && first.serial == 1 && second.serial == 2 && events.empty() && g_tail == 0,
          "all parts checked; unready FIFO head blocks later submit and ring reuse");
    results[first.fences[1]] = VK_SUCCESS;
    chunks(256);
    check(status_calls == 4 && first.serial == 0 && second.serial == 2 && g_completed == 1 && g_tail == 32,
          "completed head retires once and frees its ring stretch");
    check(events == std::vector<std::string>{"first"} && janitor_jobs.size() == 1 && !first.reset_done.load(),
          "completion follows FIFO; janitor reset remains gated");
    check(destroyed_buffers == 0 && g_poll_images_pending, "staging reclamation deferred through replay");
    const Upload u = frame_upload_alloc(32, 1);
    check(u.offset == 0 && u.ptr == ring && wait_calls == 0, "ring can reuse only retired region without a wait");
    chunks(256);
    check(second.serial == 0 && g_completed == 2 && events == std::vector<std::string>({"first", "second"}),
          "later FIFO submit retires only after first");
    g_current = nullptr; g_next = 0;
    frame_begin(true);
    check(janitor_joins == 1 && first.reset_done.load() && events[2] == "command-reset" &&
          events[3] == "descriptor-reset", "slot is not reused until asynchronous pool reset finishes");
    check(destroyed_buffers == 1 && !g_poll_images_pending, "deferred staging freed at next frame boundary");
}
void test_writeback_order() {
    reset();
    uint8_t from = 42, guest = 1;
    Slot& first = submit_slot(0, 1, 1, 16);
    Slot& second = submit_slot(1, 2, 1, 32);
    first.write_backs.push_back({&from, &guest, 1, {}, true});
    first.completions.push_back([&] { check(guest == 42, "guest data visible before completion"); events.push_back("first"); });
    second.completions.push_back([] { events.push_back("second"); });
    chunks(512);
    check(status_calls == 0 && guest == 1 && first.serial == 1 && second.serial == 2 && events.empty(),
          "pending write-back blocks polling and prevents skipping head");
    check(retire_oldest(), "blocking fallback retires head");
    check(events == std::vector<std::string>({"writeback", "first"}) && guest == 42,
          "blocking retirement publishes write-back before guest completion");
    chunks(256);
    check(events == std::vector<std::string>({"writeback", "first", "second"}), "later completion follows write-back");
}
// NX_CARRY: the part of the wait in front of a replay that is left, and what it does with the ring.
void test_carry() {
    reset();
    uint8_t from = 42, guest = 1, tail_from = 43, tail_guest = 2;
    Slot& older = submit_slot(0, 1, 1, 16);
    Slot& last = submit_slot(1, 2, 7, 48);
    Slot& next = g_slots[2];
    older.write_backs.push_back({&from, &guest, 1, {}, true});
    last.ring_begin = 16;
    last.split_part = 0;
    last.write_backs.push_back({&tail_from, &tail_guest, 1, {}, true});
    g_landing = 2;
    results[last.fences[2]] = VK_NOT_READY;  // the tail runs; the split's fence passed
    replay::carry_land.clear();
    land_or_carry();
    check(older.serial == 0 && guest == 42, "older submits land first, in order");
    check(last.serial == 2 && last.carried && g_carry_next && g_tail_hold == 16,
          "the running submit stays, and keeps its stretch of the ring");
    g_tail_held_by = &next, g_carry_next = false;
    results[last.fences[2]] = VK_SUCCESS;
    frame_carry_wait();
    check(last.serial == 0 && !last.carried && tail_guest == 43 && g_tail == 16,
          "the carried range lands at retire, and the ring stays held for the copies behind it");
    next.serial = 3; next.queued = 1; next.ring_end = 64; next.replay = true;
    g_submitted = 3;
    check(retire_oldest() && g_tail == 64 && g_tail_hold == UINT64_MAX && !g_tail_held_by,
          "the submit that ran the copies frees the ring");

    // Nothing to gain: already done, or nothing to split at.
    reset();
    Slot& done = submit_slot(0, 1, 3, 16);
    done.split_part = 0;
    g_landing = 1;
    land_or_carry();
    check(done.serial == 0 && !g_carry_next, "a finished submit just retires");
    reset();
    Slot& whole = submit_slot(0, 1, 1, 16);
    g_landing = 1;
    land_or_carry();
    check(whole.serial == 0 && !g_carry_next, "no split: waited for");
    reset();
    Slot& refused = submit_slot(0, 1, 3, 16);
    refused.split_part = 0;
    results[refused.fences[1]] = VK_NOT_READY;
    g_landing = 1;
    replay::carry_ok = false;
    land_or_carry();
    replay::carry_ok = true;
    check(refused.serial == 0 && !g_carry_next, "a refused carry waits as before");
}

void test_errors() {
    reset(); Slot& s = submit_slot(0, 1, 3, 32);
    s.completions.push_back([] { events.push_back("done"); });
    results[s.fences[0]] = VK_NOT_READY; results[s.fences[1]] = VK_ERROR_OUT_OF_HOST_MEMORY;
    chunks(256);
    check(g_replay_poll_failed && status_calls == 2 && s.serial == 1 && events.empty() && g_tail == 0,
          "unexpected status disables polling even after a not-ready part");
    results[s.fences[0]] = results[s.fences[1]] = VK_SUCCESS;
    chunks(256); check(status_calls == 2 && s.serial == 1, "error keeps polling disabled");
    check(retire_oldest() && events == std::vector<std::string>{"done"}, "blocking fallback remains available");

    reset(); Slot& missing = submit_slot(0, 1, 1, 8);
    nx::vk::p_vkGetFenceStatus = nullptr;
    chunks(256);
    check(g_replay_poll_failed && missing.serial == 1 && status_calls == 0, "missing entry point cannot release fence");

    reset(); Slot& lost = submit_slot(0, 1, 1, 8);
    results[lost.fences[0]] = VK_ERROR_DEVICE_LOST;
    expect_hung([] { chunks(256); }, "device loss must terminate retirement");
    check(lost.serial == 1 && g_completed == 0 && g_tail == 0, "device loss publishes nothing");

    reset(); Slot& waited = submit_slot(0, 1, 1, 8);
    waited.completions.push_back([] { events.push_back("done"); });
    wait_result = VK_ERROR_OUT_OF_HOST_MEMORY;
    expect_hung([] { retire_oldest(); }, "wait error must terminate retirement");
    check(waited.serial == 1 && g_completed == 0 && events.empty(), "failed wait publishes nothing");

    reset(); Slot& recording = submit_slot(3, 1, 1, 8);
    poll_oldest_ready();
    check(recording.serial == 1 && status_calls == 0, "current recording slot cannot retire");
}
void test_submission_and_reset_errors() {
    reset();
    Slot& end = *g_current;
    end_result = VK_ERROR_OUT_OF_HOST_MEMORY;
    bool completed = false;
    expect_hung([&] { frame_submit(VK_NULL_HANDLE, VK_NULL_HANDLE, {}, false, {[&] { completed = true; }}); },
                "failed command buffer end must abort");
    check(queue_calls == 0 && end.serial == 0 && g_submitted == 0 && !completed,
          "failed end never queues or publishes a submit");

    reset();
    Slot& fence = *g_current;
    reset_fence_result = VK_ERROR_DEVICE_LOST;
    expect_hung([&] { frame_submit(VK_NULL_HANDLE, VK_NULL_HANDLE, {}, false, {}); },
                "failed fence reset must abort");
    check(queue_calls == 0 && fence.serial == 0 && g_submitted == 0,
          "old signaled fence cannot be associated with a new submit");

    reset();
    Slot& slot = submit_slot(0, 1, 1, 16);
    slot.descriptors_used = true;
    slot.completions.push_back([] { events.push_back("done"); });
    descriptor_pool_result = VK_ERROR_OUT_OF_HOST_MEMORY;
    expect_hung([] { chunks(256); }, "failed descriptor pool reset must abort retirement");
    check(slot.serial == 1 && g_completed == 0 && g_tail == 0 && events == std::vector<std::string>{"descriptor-reset"},
          "failed synchronous reset cannot release resources or guest completion");

    reset();
    g_current = nullptr; g_next = 0; g_slots[0].pool_used = true;
    command_pool_result = VK_ERROR_DEVICE_LOST;
    expect_hung([] { frame_begin(true); }, "failed command pool reset must prevent slot reuse");
    check(g_current == nullptr, "slot remains unavailable after failed command pool reset");

    reset();
    g_current = nullptr; g_next = 0; g_slot_pools = false;
    command_buffer_result = VK_ERROR_OUT_OF_HOST_MEMORY;
    expect_hung([] { frame_begin(true); }, "failed command buffer reset must prevent recording");
    check(g_current == nullptr, "slot remains unavailable after failed command buffer reset");
}
void test_frame_init_command_pools() {
    const VkCommandPool shared = handle<VkCommandPool>(77);  // created with RESET_COMMAND_BUFFER in nvn_vk.cpp
    unsetenv("NX_CB_RESET");
    for (uint32_t fail_at : {1u, 2u, 3u}) {
        reset(); g_current = nullptr;
        pool_fail_at = fail_at;
        pool_create_error = fail_at == 2 ? VK_ERROR_DEVICE_LOST : VK_ERROR_OUT_OF_HOST_MEMORY;
        bool failed = false;
        try { frame_init(shared, 23, handle<VkBuffer>(7), ring, sizeof ring); }
        catch (const std::runtime_error& e) {
            check(std::string(e.what()) == "vkCreateCommandPool", "failed pool creation is fatal");
            failed = true;
        }
        check(failed && pool_creates == fail_at && g_slot_pools, "initialization stops at failed pool creation");
        check(allocation_pools == created_pools && allocation_pools.size() == fail_at - 1,
              "no shared-pool allocations or mixed reset strategies after a pool failure");
        check(queue_calls == 0 && command_buffer_resets == 0 && reset_pools.empty(),
              "pool creation failure does not reach recording, reset, or submission");
    }

    reset(); g_current = nullptr;
    frame_init(shared, 23, handle<VkBuffer>(7), ring, sizeof ring);
    check(g_slot_pools && pool_creates == g_slot_count && allocation_pools == created_pools,
          "normal mode allocates every slot from its own pool");
    check(frame_begin(true) != VK_NULL_HANDLE, "start recording from per-slot pool");
    g_current = nullptr; g_next = 0;
    check(frame_begin(true) != VK_NULL_HANDLE, "reuse per-slot pool");
    check(reset_pools == std::vector<VkCommandPool>{created_pools[0]} && command_buffer_resets == 0,
          "per-slot allocations are reset as pools, never individually");

    reset(); g_current = nullptr;
    setenv("NX_CB_RESET", "1", 1);
    frame_init(shared, 23, handle<VkBuffer>(7), ring, sizeof ring);
    check(!g_slot_pools && pool_creates == 0 && allocation_pools.size() == g_slot_count,
          "explicit single-buffer reset mode uses only the shared pool");
    check(std::all_of(allocation_pools.begin(), allocation_pools.end(), [&](VkCommandPool p) { return p == shared; }),
          "all individual-reset buffers belong to the reset-capable shared pool");
    check(frame_begin(true) != VK_NULL_HANDLE && command_buffer_resets == 1 && reset_pools.empty(),
          "explicit reset mode resets its command buffer individually");
    unsetenv("NX_CB_RESET");
}
void test_upload_memory_properties() {
    for (bool cached_ring : {false, true}) {
        for (uint32_t type : {0u, 1u}) {
            reset(); g_current = nullptr;
            frame_init(handle<VkCommandPool>(77), 23, handle<VkBuffer>(7), ring, sizeof ring, cached_ring);
            const Upload r = frame_upload_alloc(16, 16);
            check(r.host_cached == cached_ring && !r.direct && r.ptr == ring + r.offset,
                  "frame ring propagates its own mapping properties");
            staging_type = type;
            const Upload s = staging_alloc(16);
            check(s.host_cached == (type == 1) && !s.direct && s.ptr == staging_bytes,
                  "dedicated staging uses its selected type, not ring properties");
            check(g_staging.size() == 1, "staging retained for submit retirement");
            free_retired_images(0);
            check(g_staging.size() == 1 && destroyed_buffers == 0, "staging survives incomplete submit");
            free_retired_images(1);
            check(g_staging.empty() && destroyed_buffers == 1, "staging freed only after retirement");
        }
        for (uint32_t failure = 1; failure <= 4; ++failure) {
            reset(); g_current = nullptr;
            frame_init(handle<VkCommandPool>(77), 23, handle<VkBuffer>(7), ring, sizeof ring, cached_ring);
            staging_type = cached_ring ? 0 : 1; staging_failure = failure;
            const Upload fallback = staging_alloc(16);
            check(fallback.buffer == g_ring && fallback.host_cached == cached_ring && !fallback.direct,
                  "failed staging preserves fallback ring properties");
            check(g_staging.empty(), "failed staging retains no allocation");
        }
    }
    check(!Upload{}.host_cached, "unknown mapping never claims host caching");
}
// A retired buffer serves the next texture of about its size, and stays mapped until then.
void test_staging_pool() {
    reset(); g_current = nullptr;
    frame_init(handle<VkCommandPool>(77), 23, handle<VkBuffer>(7), ring, sizeof ring, false);
    const Upload first = staging_alloc(700u << 10);
    free_retired_images(0);
    check(g_staging_free.empty() && g_staging.size() == 1, "in flight: not reusable");
    free_retired_images(1);
    check(g_staging.empty() && g_staging_free.size() == 1 && destroyed_buffers == 0, "retired: pooled, not destroyed");
    const Upload small = staging_alloc(4096);
    check(g_staging_free.size() == 1 && g_staging.size() == 1, "a much smaller upload does not take a large block");
    const Upload again = staging_alloc(800u << 10);
    check(again.buffer == first.buffer && again.ptr == first.ptr && g_staging_free.empty(), "the same size reuses it");
    (void)small;
}
void test_incomplete_submission() {
    reset(); Slot& s = *g_current;
    s.part = 2; s.queued = 1;  // part 1 never queued, but the final part does
    bool completed = false;
    expect_hung([&] { frame_submit(VK_NULL_HANDLE, VK_NULL_HANDLE, {}, false, {[&] { completed = true; }}); },
                "missing intermediate part cannot complete submit");
    check(s.serial == 0 && g_submitted == 0 && !completed, "partial submit publishes no serial or completion");
    reset(); Slot& last = *g_current;
    last.part = 1; last.queued = 1; queue_result = VK_ERROR_OUT_OF_HOST_MEMORY;
    expect_hung([&] { frame_submit(VK_NULL_HANDLE, VK_NULL_HANDLE, {}, false, {}); },
                "failed final part cannot complete submit");
    check(last.serial == 0 && g_submitted == 0, "failed final part has no completed submit");
}
}

int main(int argc, char** argv) {
    try {
        setenv("NX_ASYNC_GPU", "2", 1); unsetenv("NX_PIPELINED");
        if (argc > 1 && std::string(argv[1]) == "pool") {
            test_staging_pool();
            std::puts("staging pool passed");
            return 0;
        }
        setenv("NX_STAGING_POOL_MB", "0", 1);  // the retirement tests below watch buffers being destroyed
        if (argc > 1 && std::string(argv[1]) == "carry") {
            test_carry();
            return 0;
        }
        const bool disabled = argc > 1 && std::string(argv[1]) == "off";
        if (disabled) unsetenv("NX_REPLAY_FENCE_POLL"); else setenv("NX_REPLAY_FENCE_POLL", "1", 1);
        test_frame_init_command_pools();
        test_upload_memory_properties();
        if (disabled) test_disabled();
        else { test_cadence_fifo_and_resources(); test_writeback_order(); test_errors(); test_incomplete_submission(); test_submission_and_reset_errors(); }
        std::puts("PASS: production replay fence polling, completion ordering, upload properties and resource lifetime");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1;
    }
}
