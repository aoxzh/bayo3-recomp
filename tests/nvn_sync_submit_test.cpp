// Actual synchronous gpu_submit, draw_end_submit, and write_backs_apply.
// Vulkan, replay, profiling, and initialization are deterministic doubles.
#include "../runtime/src/nvn/nvn_vk.cpp"
#include "../runtime/src/nvn/nvn_draw_state.h"
#include <cstdarg>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
using namespace nx;
using namespace nx::nvn;
namespace test {
constexpr size_t kBytes = 32;
constexpr uint8_t kOld = 0x17, kGpu = 0xa9;
enum class Stage { none, descriptor, command, begin, end, reset, submit, wait };
struct Shared { uint8_t guest[kBytes], host[kBytes]; int completions, writebacks; };
Shared* data = nullptr;
Stage fail = Stage::none;
VkResult error = VK_SUCCESS;
std::vector<std::string> events;
std::vector<WriteBack> writes, separate;
std::shared_ptr<const CommandListTable> capture;
std::string fatal_message;
bool strict = false, expect_capture = false;
template <class T> T handle(uintptr_t value) { return reinterpret_cast<T>(value); }
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
VkResult result(Stage stage, const char* event) { events.emplace_back(event); return stage == fail ? error : VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL descriptor(VkDevice, VkDescriptorPool, VkDescriptorPoolResetFlags) { return result(Stage::descriptor, "descriptor"); }
VKAPI_ATTR VkResult VKAPI_CALL command(VkCommandBuffer, VkCommandBufferResetFlags) { return result(Stage::command, "command"); }
VKAPI_ATTR VkResult VKAPI_CALL begin(VkCommandBuffer, const VkCommandBufferBeginInfo*) { return result(Stage::begin, "begin"); }
VKAPI_ATTR VkResult VKAPI_CALL end(VkCommandBuffer) { return result(Stage::end, "end"); }
VKAPI_ATTR VkResult VKAPI_CALL reset_fence(VkDevice, uint32_t n, const VkFence*) {
    require(n == 1, "reset one fence"); return result(Stage::reset, "reset");
}
VKAPI_ATTR VkResult VKAPI_CALL queue(VkQueue, uint32_t n, const VkSubmitInfo* info, VkFence) {
    require(n == 1 && info->commandBufferCount == 1, "submit one command buffer"); return result(Stage::submit, "submit");
}
VKAPI_ATTR VkResult VKAPI_CALL wait(VkDevice, uint32_t n, const VkFence*, VkBool32 all, uint64_t timeout) {
    require(n == 1 && all == VK_TRUE && timeout == 2'000'000'000ull, "wait for submitted fence");
    VkResult r = result(Stage::wait, "wait");
    if (r == VK_SUCCESS) std::memset(data->host, kGpu, kBytes); // GPU output appears only after the wait.
    return r;
}
void reset() {
    std::memset(data->guest, kOld, kBytes); std::memset(data->host, 0, kBytes);
    data->completions = data->writebacks = 0;
    fail = Stage::none; error = VK_SUCCESS; strict = false;
    events.clear(); writes.clear(); separate.clear(); capture.reset(); fatal_message.clear();
    nx::nvn::g_ready = true; nx::nvn::g_failed = false;
    nx::nvn::g_device = handle<VkDevice>(1); nx::nvn::g_queue = handle<VkQueue>(2);
    nx::nvn::g_cb = handle<VkCommandBuffer>(3); nx::nvn::g_fence = handle<VkFence>(4);
    nx::nvn::g_descriptors = handle<VkDescriptorPool>(5);
    nx::vk::p_vkResetDescriptorPool = descriptor; nx::vk::p_vkResetCommandBuffer = command;
    nx::vk::p_vkBeginCommandBuffer = begin; nx::vk::p_vkEndCommandBuffer = end;
    nx::vk::p_vkResetFences = reset_fence; nx::vk::p_vkQueueSubmit = queue; nx::vk::p_vkWaitForFences = wait;
}
bool all_equal(const uint8_t* p, uint8_t value) {
    for (size_t i = 0; i < kBytes; ++i) if (p[i] != value) return false;
    return true;
}
void submit(bool captured = false) {
    expect_capture = captured;
    auto list = std::make_shared<CmdList>(); list->epoch = 7;
    auto deferred = std::make_shared<DeferredValues>(); (*deferred)[42] = {24};
    auto capture = captured ? std::make_shared<CommandListTable>() : nullptr;
    std::vector<std::function<void()>> callbacks;
    for (int i = 0; i < 2; ++i) callbacks.push_back([i] {
        ++data->completions; events.emplace_back("completion" + std::to_string(i));
        require(all_equal(data->guest, kGpu), "completion observed stale guest memory");
    });
    gpu_submit({list}, std::move(deferred), std::move(capture), std::move(callbacks));
}
void unchanged() {
    require(current_deferred() == nullptr && !capture, "failed submit retained replay state");
    require(data->completions == 0, "failed submit invoked completion");
    require(data->writebacks == 0, "failed submit took write-backs");
    require(all_equal(data->guest, kOld), "failed submit modified guest memory");
}
void success(bool captured = false) {
    reset(); submit(captured);
    require(current_deferred() == nullptr && !capture, "successful submit retained replay state");
    require(data->completions == 2 && data->writebacks == 1 && separate.empty() && all_equal(data->guest, kGpu), "missing write-back or callbacks");
    auto w = std::find(events.begin(), events.end(), "wait");
    auto t = std::find(events.begin(), events.end(), "take-writebacks");
    auto c1 = std::find(events.begin(), events.end(), "completion0");
    auto c2 = std::find(events.begin(), events.end(), "completion1");
    require(w != events.end() && t != events.end() && c1 != events.end() && c2 != events.end() && w < t && t < c1 && c1 < c2,
            "wait, write-back and completion order");
}
void failure(Stage stage, const char* event, const char* diagnostic, bool fatal_expected = true,
             bool strict_mode = false, bool captured = false) {
    reset(); fail = stage; error = VK_ERROR_OUT_OF_HOST_MEMORY; strict = strict_mode;
    bool threw = false;
    try { submit(captured); } catch (const std::runtime_error&) { threw = true; }
    require(threw == fatal_expected, "unexpected fatal behavior for Vulkan error");
    if (fatal_expected) require(fatal_message.find(diagnostic) != std::string::npos, "fatal did not name operation");
    unchanged();
    auto p = std::find(events.begin(), events.end(), event);
    require(p != events.end(), "failure stage not reached");
    require(std::find(p + 1, events.end(), "submit") == events.end() && std::find(p + 1, events.end(), "wait") == events.end(),
            "continued after Vulkan failure");
}
void init_failure(bool strict_mode, bool captured) {
    reset(); nx::nvn::g_ready = false; nx::nvn::g_failed = true; strict = strict_mode;
    bool threw = false; try { submit(captured); } catch (const std::runtime_error&) { threw = true; }
    require(threw == (strict_mode || captured), "incorrect GPU initialization failure behavior");
    if (threw) require(fatal_message.find("GPU initialization failed") != std::string::npos, "missing initialization failure diagnostic");
    require(events.empty(), "initialization failure started a command buffer");
    unchanged();
}
void fatal_hang(Stage stage, VkResult code) {
    reset(); fail = stage; error = code;
    pid_t child = fork(); require(child >= 0, "fork failed");
    if (child == 0) { try { submit(); } catch (...) { _exit(77); } _exit(78); }
    int status = 0;
    require(waitpid(child, &status, 0) == child, "waitpid failed");
    require(WIFEXITED(status) && WEXITSTATUS(status) == 4, "GPU hang did not exit with status 4");
    unchanged();
}
} // namespace test
namespace nx {
void log(const char*, ...) {}
bool strict_runtime() { return test::strict; }
[[noreturn]] void fatal(const char* fmt, ...) {
    char s[512]; va_list ap; va_start(ap, fmt); std::vsnprintf(s, sizeof s, fmt, ap); va_end(ap);
    test::fatal_message = s; throw std::runtime_error(s); // production terminates; test can inspect state
}
}
namespace nx::game { const Info kInfo{"test", "test", 0, "", ""}; void renderer_start() {} }
namespace nx::plat {
uint64_t monotonic_ns() { static uint64_t n = 0; return ++n; }
bool late_guard_on(){return false;} uint64_t late_guard_add(uint64_t,uint64_t){return 0;} uint32_t late_guard_open(LateGuardEvent*,uint32_t){return 0;}
void* vulkan_get_instance_proc_addr() { return nullptr; }
const char* vulkan_surface_extension() { return ""; }
int vulkan_create_surface(void*, void*, uint64_t*, void*) { return -1; }
void* create_window() { return nullptr; }
void release_window(void*) {}
uint64_t window_generation() { return 0; }
void set_thread_name(const char*) {}
void request_performance_profile() {}
}
namespace nx::nvn {
ReplayScope::ReplayScope(uint64_t) {}
ReplayScope::~ReplayScope() {}
VkPhysicalDevice pick_physical_device(const std::vector<VkPhysicalDevice>&) { return VK_NULL_HANDLE; }
void set_astc_native(bool) {}
bool astc_native() { return false; }
void log_device_caps(VkPhysicalDevice, const std::vector<VkExtensionProperties>&) {}
uint32_t upload_memory_type(uint32_t) { return 0; }
bool async_gpu() { return false; }
bool pipelined() { return false; }
void frame_init(VkCommandPool, uint32_t, VkBuffer, uint8_t*, uint64_t, bool) {}
VkCommandBuffer frame_begin(bool) { throw std::runtime_error("unexpected async begin"); }
uint64_t frame_completed() { return 0; }
uint64_t frame_upload_used() { return 0; }
void free_retired_images(uint64_t) {}
FrameTimes& frame_times() { static FrameTimes v; return v; }
size_t frame_plan_split(const std::vector<std::shared_ptr<CmdList>>&) { return SIZE_MAX; }
VkCommandBuffer frame_split(VkCommandBuffer cb) { return cb; }
VkCommandBuffer frame_cb(VkCommandBuffer cb) { return cb; }
void frame_submit(VkSemaphore, VkSemaphore, std::vector<WriteBack>, bool, std::vector<std::function<void()>>) {
    throw std::runtime_error("unexpected async submit");
}
void submit_lists_capture(std::shared_ptr<const CommandListTable> p) { test::capture = std::move(p); }
void replay_list(VkCommandBuffer, Targets&, const CmdList&, int) {
    test::require(current_deferred() && current_deferred()->at(42)[0] == 24, "missing deferred state");
    test::require(bool(test::capture) == test::expect_capture, "missing captured list table");
    test::events.emplace_back("replay");
    test::writes.push_back({test::data->host, test::data->guest, test::kBytes / 2, {}, true});
    test::separate.push_back({test::data->host + test::kBytes / 2, test::data->guest + test::kBytes / 2,
                              test::kBytes / 2, {}, true});
}
void draw_begin_submit() {}
void draw_end_rendering(VkCommandBuffer, int) {}
void debug_snapshots_write() { test::events.emplace_back("snapshots"); }
bool debug_snapshots_pending() { return false; }
bool gpu_profile_on() { return false; }
void gpu_profile_begin(VkCommandBuffer) {}
void gpu_profile_mark(VkCommandBuffer, const std::string&) {}
void gpu_profile_collect() {}
bool zero_draws_on() { return false; }
void zero_draws_begin_submit(VkCommandBuffer) {}
void zero_draws_collect() {}
void debug_dump_requested(uint64_t) {}
namespace replay {
void written_carry_seed(VkCommandBuffer) {}
std::vector<WriteBack> written_take() {
    ++test::data->writebacks; test::events.emplace_back("take-writebacks"); return std::move(test::writes);
}
std::vector<WriteBack>& separate_write_backs() { return test::separate; }
void trace_nan_write_back(const uint8_t*, const uint8_t*, uint64_t) {}
}
}
int main() {
    setenv("NX_COPY_THREADS", "0", 1);
    void* p = mmap(nullptr, sizeof(test::Shared), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return 2;
    test::data = static_cast<test::Shared*>(p);
    try {
        test::success(); test::success(true);
        test::init_failure(false, false); test::init_failure(true, false); test::init_failure(false, true);
        test::failure(test::Stage::descriptor, "descriptor", "descriptor pool reset");
        test::failure(test::Stage::command, "command", "", false);
        test::failure(test::Stage::command, "command", "command buffer begin failed", true, true);
        test::failure(test::Stage::command, "command", "command buffer begin failed", true, false, true);
        test::failure(test::Stage::begin, "begin", "", false);
        test::fatal_hang(test::Stage::descriptor, VK_ERROR_DEVICE_LOST);
        test::fatal_hang(test::Stage::command, VK_ERROR_DEVICE_LOST);
        test::fatal_hang(test::Stage::begin, VK_ERROR_DEVICE_LOST);
        for (auto [stage, event, diagnostic] : {
                 std::tuple{test::Stage::end, "end", "vkEndCommandBuffer"},
                 std::tuple{test::Stage::reset, "reset", "vkResetFences"},
                 std::tuple{test::Stage::submit, "submit", "vkQueueSubmit"},
                 std::tuple{test::Stage::wait, "wait", "vkWaitForFences"}}) {
            test::failure(stage, event, diagnostic);
            test::fatal_hang(stage, VK_ERROR_DEVICE_LOST);
        }
        test::failure(test::Stage::submit, "submit", "vkQueueSubmit", true, false, true);
        test::reset(); test::fail = test::Stage::wait; test::error = VK_NOT_READY;
        bool threw = false; try { test::submit(); } catch (const std::runtime_error&) { threw = true; }
        test::require(threw && test::fatal_message.find("vkWaitForFences") != std::string::npos, "positive result did not stop submit");
        test::unchanged();
        test::fatal_hang(test::Stage::wait, VK_TIMEOUT);
        std::puts("synchronous submit production-path regression tests passed");
    } catch (const std::exception& e) { std::fprintf(stderr, "synchronous submit test failed: %s\n", e.what()); return 1; }
    munmap(p, sizeof(test::Shared));
}
