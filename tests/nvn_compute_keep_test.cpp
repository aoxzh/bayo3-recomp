// Standalone production-path integration regression; no Vulkan driver or game.
// c++ -std=c++20 -O1 -ffunction-sections -fdata-sections -pthread -Iruntime/include -Iruntime/src \
//     -Iprivate/toolchains/Vulkan-Headers-1.4.357/include tests/nvn_compute_keep_test.cpp \
//     -Wl,--gc-sections -o /tmp/nvn-compute-keep-test
#include "../runtime/src/nvn/nvn_written.cpp"
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <memory>
#include <random>
#if !defined(_WIN32)
#include <cerrno>
#include <csignal>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#if defined(_WIN32)
// The rest of the standalone harness uses POSIX environment helpers.
static int setenv(const char* name, const char* value, int) { return _putenv_s(name, value); }
static int unsetenv(const char* name) { return _putenv_s(name, ""); }
#endif

namespace {
struct Buffer { VkDeviceSize size = 0; VkBufferUsageFlags usage = 0; uint8_t* ptr = nullptr; };
struct Memory { std::vector<uint8_t> bytes; };
struct Ring { Buffer buffer; std::vector<uint8_t> bytes; };
std::vector<std::unique_ptr<Ring>> ring;
std::vector<std::string> log_lines;
bool async_enabled = true, coherent_available = true;
bool use_memory_props = false;
VkPhysicalDeviceMemoryProperties memory_props{};
uint32_t req_type_bits = 1, last_memory_type = UINT32_MAX;
bool fail_allocate = false, fail_bind = false, fail_map = false;
uint64_t req_extra = 4096, req_override = 0;
unsigned creates = 0, destroys = 0, allocations = 0, frees = 0, unmaps = 0, waits = 0;
unsigned direct_import_attempts = 0, dedup_uploads = 0;
std::function<void()> finish_pending;
VKAPI_ATTR VkResult VKAPI_CALL create_buffer(VkDevice, const VkBufferCreateInfo* i, const VkAllocationCallbacks*, VkBuffer* out) {
    ++creates; *out = reinterpret_cast<VkBuffer>(new Buffer{i->size, i->usage}); return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL destroy_buffer(VkDevice, VkBuffer h, const VkAllocationCallbacks*) {
    ++destroys; delete reinterpret_cast<Buffer*>(h);
}
VKAPI_ATTR void VKAPI_CALL requirements(VkDevice, VkBuffer h, VkMemoryRequirements* out) {
    const auto n = reinterpret_cast<Buffer*>(h)->size;
    *out = VkMemoryRequirements{req_override ? req_override : n + req_extra, 1, req_type_bits};
}
VKAPI_ATTR VkResult VKAPI_CALL alloc_memory(VkDevice, const VkMemoryAllocateInfo* i, const VkAllocationCallbacks*, VkDeviceMemory* out) {
    last_memory_type = i->memoryTypeIndex;
    if (fail_allocate) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    auto* m = new Memory; m->bytes.resize(size_t(i->allocationSize)); *out = reinterpret_cast<VkDeviceMemory>(m); ++allocations; return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL free_memory(VkDevice, VkDeviceMemory h, const VkAllocationCallbacks*) {
    ++frees; delete reinterpret_cast<Memory*>(h);
}
VKAPI_ATTR VkResult VKAPI_CALL bind_memory(VkDevice, VkBuffer h, VkDeviceMemory m, VkDeviceSize) {
    if (fail_bind) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    reinterpret_cast<Buffer*>(h)->ptr = reinterpret_cast<Memory*>(m)->bytes.data(); return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL map_memory(VkDevice, VkDeviceMemory h, VkDeviceSize, VkDeviceSize, VkMemoryMapFlags, void** out) {
    if (fail_map) return VK_ERROR_MEMORY_MAP_FAILED;
    *out = reinterpret_cast<Memory*>(h)->bytes.data(); return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL unmap_memory(VkDevice, VkDeviceMemory) { ++unmaps; }
VKAPI_ATTR void VKAPI_CALL pipeline_barrier(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags, VkDependencyFlags,
    uint32_t, const VkMemoryBarrier*, uint32_t, const VkBufferMemoryBarrier*, uint32_t, const VkImageMemoryBarrier*) {}
VKAPI_ATTR void VKAPI_CALL physical_properties(VkPhysicalDevice, VkPhysicalDeviceProperties* out) {
    *out = {}; out->limits.minStorageBufferOffsetAlignment = 1; out->limits.minUniformBufferOffsetAlignment = 1;
}
VKAPI_ATTR void VKAPI_CALL physical_memory_properties(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties* out) {
    if (use_memory_props) { *out = memory_props; return; }
    *out = {}; out->memoryTypeCount = 1; out->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    if (coherent_available) out->memoryTypes[0].propertyFlags |= VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
}
VKAPI_ATTR void VKAPI_CALL copy_buffer(VkCommandBuffer, VkBuffer src, VkBuffer dst, uint32_t count, const VkBufferCopy* c) {
    auto* s = reinterpret_cast<Buffer*>(src); auto* d = reinterpret_cast<Buffer*>(dst);
    for (uint32_t i = 0; i < count; ++i) {
        assert(c[i].srcOffset + c[i].size <= s->size && c[i].dstOffset + c[i].size <= d->size);
        std::memcpy(d->ptr + c[i].dstOffset, s->ptr + c[i].srcOffset, size_t(c[i].size));
    }
}
}
namespace nx::vk {
PFN_vkGetPhysicalDeviceProperties p_vkGetPhysicalDeviceProperties = physical_properties;
PFN_vkGetPhysicalDeviceMemoryProperties p_vkGetPhysicalDeviceMemoryProperties = physical_memory_properties;
PFN_vkCreateBuffer p_vkCreateBuffer = create_buffer;
PFN_vkDestroyBuffer p_vkDestroyBuffer = destroy_buffer;
PFN_vkGetBufferMemoryRequirements p_vkGetBufferMemoryRequirements = requirements;
PFN_vkAllocateMemory p_vkAllocateMemory = alloc_memory;
PFN_vkFreeMemory p_vkFreeMemory = free_memory;
PFN_vkBindBufferMemory p_vkBindBufferMemory = bind_memory;
PFN_vkMapMemory p_vkMapMemory = map_memory;
PFN_vkUnmapMemory p_vkUnmapMemory = unmap_memory;
PFN_vkCmdPipelineBarrier p_vkCmdPipelineBarrier = pipeline_barrier;
PFN_vkCmdCopyBuffer p_vkCmdCopyBuffer = copy_buffer;
}
namespace nx {
void log(const char* format, ...) {
    char line[2048];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    log_lines.emplace_back(line);
}
[[noreturn]] void fatal(const char*, ...) { std::abort(); }
}
namespace nx::plat {
uint64_t monotonic_ns() { return 1; }
bool late_guard_on(){return false;} uint64_t late_guard_add(uint64_t,uint64_t){return 0;} uint32_t late_guard_open(LateGuardEvent*,uint32_t){return 0;}
void set_thread_name(const char*) {}
}
namespace nx::nvn {
VkDevice vk_device() { return VK_NULL_HANDLE; }
VkPhysicalDevice vk_physical() { return VK_NULL_HANDLE; }
bool async_gpu() { return async_enabled; }
void draw_end_rendering(VkCommandBuffer, int) { std::abort(); }  // No render pass is open in these tests.
void frame_wait_all() { ++waits; if (finish_pending) { auto fn = std::move(finish_pending); fn(); } }
unsigned carry_waits = 0;
std::function<void()> carry_lands;
void frame_carry_wait() { ++carry_waits; if (carry_lands) { auto fn = std::move(carry_lands); fn(); } }
bool vk_host_import() { return true; }  // Pretend the device supports import; callers must still fall back.
uint32_t memory_type(uint32_t, VkMemoryPropertyFlags) { return 0; }
Upload host_import_upload(const void* p, uint64_t, uint64_t) {
    ++direct_import_attempts;
    return {reinterpret_cast<VkBuffer>(uintptr_t(1)), 0, const_cast<uint8_t*>(static_cast<const uint8_t*>(p)), true};
}
Upload upload_alloc(uint64_t size, uint64_t) {
    auto r = std::make_unique<Ring>(); r->bytes.resize(size_t(size)); r->buffer = {size, 0, r->bytes.data()};
    Upload u{reinterpret_cast<VkBuffer>(&r->buffer), 0, r->bytes.data()}; ring.push_back(std::move(r)); return u;
}
}
namespace nx::nvn::replay {
Upload dedup_upload(const uint8_t* p, uint64_t size, uint64_t padded, uint64_t align, bool) {
    ++dedup_uploads;
    Upload u = upload_alloc(padded, align);
    std::memcpy(u.ptr, p, size);
    if (padded > size) std::memset(u.ptr + size, 0, padded - size);
    return u;
}
void dedup_invalidate(const uint8_t*, uint64_t) {}
void compute_write_barrier(VkCommandBuffer) {}
void trace_nan_write_back(const uint8_t*, const uint8_t*, uint64_t) {}
}
using nx::nvn::Upload;
namespace replay = nx::nvn::replay;
static Upload bind(uint8_t* guest, size_t n, size_t padded = 0) {
    replay::written_reset();
    Upload up = replay::written_bind(VK_NULL_HANDLE, guest, n, padded ? padded : n, false);
    replay::copy_behind::wait();  // these tests read and write the copy on the CPU, as the replay never does
    return up;
}
static Upload bind(std::vector<uint8_t>& guest) { return bind(guest.data(), guest.size()); }
static void land() {
    auto w = replay::written_take(); nx::nvn::write_backs_apply(w);
}
static void clean_submits(std::vector<uint8_t>& guest, int count) {
    for (int i = 0; i < count; ++i) {
        auto up = bind(guest); assert(std::memcmp(up.ptr, guest.data(), guest.size()) == 0); land();
    }
}
static void close_cache() {
    // The real caller holds gpu_lock and has waited all fences and queue idle.
    nx::nvn::frame_wait_all(); replay::written_kept_shutdown(); replay::written_kept_shutdown();
    assert(replay::g_kept.empty() && replay::g_kept_bytes == 0);
}
static void chunk_regression() {
    std::vector<uint8_t> guest(4 * 65536);
    clean_submits(guest, 2);
    auto up = bind(guest);
    auto& region = replay::g_regions.begin()->second;
    assert(region.before && region.before->size() == guest.size());
    assert(std::memcmp(region.before->data(), guest.data(), guest.size()) == 0);
    guest[0] = 42; up.ptr[4] = 77; land();
    assert(guest[0] == 42 && guest[4] == 77);
    assert(replay::g_kept.empty());
}
static void kept_regression() {
    std::vector<uint8_t> guest(64);
    clean_submits(guest, 3);
    assert(replay::g_kept.size() == 1 && replay::g_kept_hits >= 2);
    assert(replay::g_kept_bytes == replay::g_kept.begin()->second.cap + req_extra);
    assert((reinterpret_cast<Buffer*>(replay::g_kept.begin()->second.buffer)->usage & nx::nvn::kUploadUsage) == nx::nvn::kUploadUsage);
    guest[0] = 42;
    auto up = bind(guest); assert(up.ptr[0] == 42);
    guest[8] = 19; up.ptr[4] = 77; land();
    assert(guest[0] == 42 && guest[4] == 77 && guest[8] == 19);
    const auto cap = replay::g_kept.begin()->second.cap;
    std::memset(up.ptr + guest.size(), 0xaa, size_t(cap - guest.size()));
    up = bind(guest.data(), guest.size(), 100000); // larger padded requirement: no in-place destruction
    assert(replay::g_kept_evict_pending && destroys == 0); land();
    replay::written_reset(); assert(replay::g_kept.empty() && destroys == 1 && unmaps == 1 && frees == 1);
    up = replay::written_bind(VK_NULL_HANDLE, guest.data(), guest.size(), guest.size(), false);
    assert(replay::g_kept.size() == 1); land();
    up.ptr[guest.size() + 17] = 0xff;
    up = bind(guest);
    assert(up.ptr[guest.size() + 17] == 0); land();
    close_cache(); assert(creates == destroys && allocations == frees && unmaps == allocations);
}
static void default_or_sync() {
    std::vector<uint8_t> guest(32); auto up = bind(guest); up.ptr[4] = 22; land();
    assert(guest[4] == 22 && replay::g_kept.empty() && creates == 0 && waits == 0);
}
static void fence() {
    std::vector<uint8_t> guest(32); auto up = bind(guest);
    auto w = replay::written_take(); bool retired = false;
    finish_pending = [&, w = std::move(w)]() mutable { up.ptr[4] = 77; nx::nvn::write_backs_apply(w); retired = true; };
    assert(guest[4] == 0 && waits == 0);
    auto next = bind(guest);
    assert(retired && waits == 1 && guest[4] == 77 && next.ptr[4] == 77);
    land(); close_cache();
}
static void overlap() {
    std::vector<uint8_t> guest(128);
    auto a = bind(guest.data(), 64); a.ptr[4] = 77; guest[0] = 42;
    auto b = replay::written_bind(VK_NULL_HANDLE, guest.data() + 32, 64, 64, false);
    assert(b.ptr[0] == 0); b.ptr[48] = 88;
    assert(destroys == 0); land();
    assert(guest[0] == 42 && guest[4] == 77 && guest[80] == 88);
    close_cache();
}
static void failure_with_recorded_later_entry() {
    std::vector<uint8_t> guest(256);
    uint8_t* low = guest.data();
    uint8_t* high = guest.data() + 128;
    low[0] = 42;
    high[0] = 17;
    auto high_up = bind(high, 32);
    assert(high_up.ptr[0] == 17 && replay::g_kept.size() == 1);
    high_up.ptr[4] = 77; // an earlier recorded command already references this allocation
    fail_allocate = true;
    auto low_up = replay::written_bind(VK_NULL_HANDLE, low, 32, 32, false);
    assert(low_up.buffer != high_up.buffer && low_up.ptr[0] == 42);
    assert(high_up.ptr[0] == 17 && high_up.ptr[4] == 77 && destroys == 1);
    low_up.ptr[8] = 88;
    land();
    assert(low[0] == 42 && low[8] == 88 && high[0] == 17 && high[4] == 77);
    close_cache(); assert(allocations == frees && creates == destroys);
}
#if !defined(_WIN32)
static void mapping() {
    const size_t page = size_t(sysconf(_SC_PAGESIZE));
    auto* base = static_cast<uint8_t*>(mmap(nullptr, page * 4, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    assert(base != MAP_FAILED && mprotect(base + page, page * 2, PROT_READ | PROT_WRITE) == 0);
    uint8_t* guest = base + page * 2 - 3; constexpr size_t n = 10;
    std::memset(guest, 0, n); auto up = bind(guest, n); up.ptr[4] = 7; land(); assert(guest[4] == 7);
    int fds[2]; assert(pipe(fds) == 0); uint8_t byte = 0x5a;
    assert(write(fds[1], &byte, 1) == 1 && read(fds[0], guest + 6, 1) == 1); close(fds[0]); close(fds[1]);
    up = bind(guest, n); assert(up.ptr[6] == 0x5a); land();
    void* remapped = mmap(base + page, page * 2, PROT_READ | PROT_WRITE,
                          MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(remapped == base + page); guest[0] = 91; guest[9] = 92;
    up = bind(guest, n); assert(up.ptr[0] == 91 && up.ptr[4] == 0 && up.ptr[9] == 92);
    up.ptr[8] = 11; land(); assert(guest[0] == 91 && guest[8] == 11 && guest[9] == 92);
    close_cache(); assert(munmap(base, page * 4) == 0);
}
#else
static void mapping() {}  // main reports that this POSIX-only test was skipped
#endif
// Independent reference: 8-byte blocks comprise two 4-byte merge words;
// the final incomplete block is merged byte-by-byte, as in the baseline.
static void oracle(std::vector<uint8_t>& guest, const std::vector<uint8_t>& before, const uint8_t* gpu) {
    size_t k = 0;
    for (; k + 8 <= guest.size(); k += 8)
        for (size_t j = 0; j < 8; j += 4)
            if (std::memcmp(gpu + k + j, before.data() + k + j, 4))
                std::memcpy(guest.data() + k + j, gpu + k + j, 4);
    for (; k < guest.size(); ++k) if (gpu[k] != before[k]) guest[k] = gpu[k];
}

// Only a real full-range fill with no prior overlap can omit the snapshot.
// NX_WRITE_BACK_MERGE keeps the old control behavior of forcing a merge.
static void fill_regression(bool force_merge) {
    std::vector<uint8_t> guest(11, 0);
    const auto before = guest;
    replay::written_reset();
    auto up = replay::written_bind(VK_NULL_HANDLE, guest.data(), guest.size(), guest.size(), true);
    const auto& region = replay::g_regions.begin()->second;
    assert(region.whole == !force_merge);
    if (force_merge) {
        assert(region.before && *region.before == before);
        assert(replay::g_snap_bytes.load() == guest.size());
    } else {
        assert(!region.before && replay::g_snap_bytes.load() == 0);
    }
    guest[1] = 0x44;
    guest[10] = 0x55;
    std::memset(up.ptr, 0x71, guest.size());
    auto expected = guest;
    oracle(expected, before, up.ptr);
    auto backs = replay::written_take();
    assert(backs.size() == 1 && backs[0].whole == !force_merge);
    nx::nvn::write_backs_apply(backs);
    assert(guest == expected && std::all_of(guest.begin(), guest.end(), [](uint8_t b) { return b == 0x71; }));
    assert(replay::g_whole_writes.load() == unsigned(!force_merge));
    assert(replay::g_merge_writes.load() == unsigned(force_merge));
}

static void fill_overlap_regression() {
    std::vector<uint8_t> guest(96, 0);
    const auto before = guest;
    auto first = bind(guest.data(), 64);
    first.ptr[4] = 77;
    guest[0] = 42;
    auto fill = replay::written_bind(VK_NULL_HANDLE, guest.data() + 32, 64, 64, true);
    auto& region = replay::g_regions.begin()->second;
    assert(replay::g_regions.size() == 1 && !region.whole && region.before && *region.before == before);
    std::memset(fill.ptr, 0x51, 64);
    guest[8] = 19;
    guest[36] = 0xab;
    auto expected = guest;
    oracle(expected, before, region.up.ptr);
    auto backs = replay::written_take();
    assert(backs.size() == 1 && !backs[0].whole && backs[0].before);
    nx::nvn::write_backs_apply(backs);
    assert(guest == expected && guest[0] == 42 && guest[4] == 77 && guest[8] == 19 && guest[36] == 0x51);
}
#if !defined(_WIN32)
// When GPU bytes match the snapshot, the merge has no reason to touch the
// guest. PROT_NONE detects both stray reads and writes. The isolated checked
// kernel is a negative control: it must fault on the same protected range.
static void unchanged_guest_not_read() {
    const long page_value = sysconf(_SC_PAGESIZE);
    assert(page_value > 0);
    const size_t n = size_t(page_value);
    auto* guest = static_cast<uint8_t*>(mmap(nullptr, n, PROT_READ | PROT_WRITE,
                                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    assert(guest != MAP_FAILED);
    std::memset(guest, 0x99, n);
    std::vector<uint8_t> gpu(n, 0x37);
    auto before = std::make_shared<std::vector<uint8_t>>(gpu);
    assert(mprotect(guest, n, PROT_NONE) == 0);

    const pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        const rlimit no_core{0, 0};
        if (setrlimit(RLIMIT_CORE, &no_core) != 0) _exit(70);
        const bool clean = nx::nvn::merge_detail::merge<true>(gpu.data(), before->data(), guest, n, [](size_t) {});
        _exit(clean ? 0 : 1);
    }
    int status = 0;
    while (waitpid(child, &status, 0) < 0) assert(errno == EINTR);
    assert(WIFSIGNALED(status) && (WTERMSIG(status) == SIGSEGV || WTERMSIG(status) == SIGBUS));

    std::vector<nx::nvn::WriteBack> backs{{gpu.data(), guest, n, before, false}};
    nx::nvn::write_backs_apply(backs);
    assert(backs.empty() && mprotect(guest, n, PROT_READ | PROT_WRITE) == 0);
    assert(std::all_of(guest, guest + n, [](uint8_t b) { return b == 0x99; }));
    assert(munmap(guest, n) == 0);
}
#else
static void unchanged_guest_not_read() {}  // main reports this POSIX-only test as skipped
#endif

#if !defined(_WIN32)
// A real kernel copy, deliberately ordered after the production bind/snapshot
// and before production write-back. The two words straddle a host page: the
// GPU leaves the first alone and changes a byte in the second. All four bytes
// of a changed GPU word win, including bytes the kernel wrote in that word.
static void kernel_pread(bool kept, bool omit_snapshot) {
    const long page_value = sysconf(_SC_PAGESIZE);
    assert(page_value > 0);
    const size_t page = size_t(page_value);
    assert(page <= (SIZE_MAX - 39) / 2);
    const size_t n = (std::max<size_t>(4 * 65536, page * 2 + 32) + 7) & ~size_t(7);
    std::vector<uint8_t> guest(n);
    clean_submits(guest, 2);  // exercise a recycled full snapshot on the default path
    assert(replay::g_snap_pool.size() == 1);
    const auto* recycled = replay::g_snap_pool.front().get();
    const auto before = guest;
    size_t boundary = page - (uintptr_t(guest.data()) % page);
    if (boundary < 4) boundary += page;
    const size_t start = boundary - 4;
    assert(start % 4 == 0 && start + 20 <= n);
    assert((uintptr_t(guest.data() + start) % page) == page - 4);
    assert((uintptr_t(guest.data() + start + 4) % page) == 0);

    const uint8_t payload[] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    std::FILE* file = std::tmpfile();
    assert(file && std::fwrite(payload, 1, sizeof(payload), file) == sizeof(payload));
    assert(std::fflush(file) == 0);
    auto up = bind(guest);
    assert(std::memcmp(up.ptr, before.data(), n) == 0);
    const auto& region = replay::g_regions.begin()->second;
    assert(region.before && region.before->size() == n && region.before.get() == recycled);
    assert(std::memcmp(region.before->data(), before.data(), n) == 0);
    if (kept) {
        assert(replay::g_kept.size() == 1 && replay::g_kept_hits >= 2);
        assert(up.buffer == replay::g_kept.begin()->second.buffer);
    } else {
        assert(replay::g_kept.empty());
    }

    size_t done = 0;
    while (done < sizeof(payload)) {
        const ssize_t r = pread(fileno(file), guest.data() + start + done, sizeof(payload) - done, off_t(done));
        if (r < 0 && errno == EINTR) continue;
        assert(r > 0);
        done += size_t(r);
    }
    assert(std::fclose(file) == 0);
    assert(std::memcmp(guest.data() + start, payload, sizeof(payload)) == 0);
    up.ptr[start + 4] = 0xa5;
    up.ptr[start + 16] = 0x5a;
    auto expected = guest;
    oracle(expected, before, up.ptr);
    assert(expected[start] == payload[0] && expected[start + 3] == payload[3]);
    assert(expected[start + 4] == 0xa5 && expected[start + 5] == before[start + 5]);
    assert(expected[start + 6] == before[start + 6] && expected[start + 7] == before[start + 7]);
    assert(expected[start + 16] == 0x5a);

    // The former erroneous optimization compared the current guest bytes
    // directly against the GPU copy, ignoring the bind-time snapshot.
    auto bad_diff = guest;
    nx::nvn::merge_detail::diff_copy(up.ptr, bad_diff.data(), n);
    assert(bad_diff != expected && bad_diff[start] == before[start]);
    assert(bad_diff[start + 4] == expected[start + 4]);

    auto backs = replay::written_take();
    assert(backs.size() == 1 && backs[0].before);
    if (omit_snapshot) backs[0].before.reset();  // negative control: production whole-copy fallback
    nx::nvn::write_backs_apply(backs);
    if (omit_snapshot) {
        assert(guest != expected && guest[start] == before[start]);
        assert(guest[start + 4] == expected[start + 4]);
    } else {
        assert(guest == expected);
        assert(guest[start] == payload[0] && guest[start + 3] == payload[3]);
        assert(guest[start + 4] == 0xa5 && guest[start + 5] == before[start + 5]);
    }
    close_cache();
}
#else
static void kernel_pread(bool, bool) {}  // main reports that this POSIX-only test was skipped
#endif

// Exercise full snapshot reuse across large, small, then large ranges. The
// separate modes also cover the obsolete chunk switch unset and set to zero.
static void snapshot_cycle() {
    constexpr size_t big = 1024 * 1024, small = 32768;
    std::vector<uint8_t> guest(big);
    const std::vector<uint8_t>* first = nullptr;
    size_t first_capacity = 0;
    for (unsigned turn = 0; turn < 3; ++turn) {
        const size_t n = turn == 1 ? small : big;
        guest.resize(n);
        for (size_t i = 0; i < n; ++i) guest[i] = uint8_t((i * 37 + turn * 53) & 255);
        const auto before = guest;
        const auto up = bind(guest);
        auto& region = replay::g_regions.begin()->second;
        assert(region.before && std::memcmp(region.before->data(), before.data(), n) == 0);
        assert(region.before->size() == n);
        if (turn == 0) {
            first = region.before.get();
            first_capacity = region.before->capacity();
        } else {
            assert(region.before.get() == first && region.before->capacity() == first_capacity);
        }
        guest[0] ^= 0x80;  // kernel/CPU-only word must survive
        guest[4] ^= 0x40;  // GPU wins an overlapping 4-byte word
        up.ptr[4] ^= 0x02;
        up.ptr[8] ^= 0x20;
        auto expected = guest;
        oracle(expected, before, up.ptr);
        land();
        assert(guest == expected);
        assert(replay::g_snap_pool.size() == 1 && replay::g_snap_pool.front().get() == first);
        assert(replay::g_snap_pool_bytes == replay::g_snap_pool.front()->capacity());
    }
}

// A second owner outlives write-back and must never observe recycled bytes.
static void snapshot_owner() {
    std::vector<uint8_t> guest(2 * 1024 * 1024 + 19, 7);
    auto up = bind(guest);
    auto held = replay::g_regions.begin()->second.before;
    assert(held && held->size() == guest.size() && *held == guest);
    const auto held_contents = *held;
    auto before = guest;
    guest[0] = 42;
    up.ptr[4] = 77;
    auto expected = guest;
    oracle(expected, before, up.ptr);
    land();
    assert(guest == expected && replay::g_snap_pool.empty() && *held == held_contents);

    before = guest;
    up = bind(guest);
    auto& next = replay::g_regions.begin()->second.before;
    assert(next && next.get() != held.get() && *held == held_contents);
    guest[8] = 51;
    up.ptr[12] = 99;
    expected = guest;
    oracle(expected, before, up.ptr);
    land();
    assert(guest == expected && *held == held_contents && replay::g_snap_pool.size() == 1);
    held.reset();
}

static void snapshot_selection() {
    // The exact-length allocation wins even when a smaller sufficient
    // capacity exists; among non-exact allocations choose the smallest fit.
    auto small = std::make_shared<std::vector<uint8_t>>(128);
    auto middle = std::make_shared<std::vector<uint8_t>>(small->capacity() + 128);
    auto exact = std::make_shared<std::vector<uint8_t>>(middle->capacity() + 1024);
    const size_t max_capacity = exact->capacity();
    exact->resize(32);
    middle->resize(80);
    small->resize(100);
    assert(small->capacity() < middle->capacity() && middle->capacity() < exact->capacity());
    auto* exact_id = exact.get();
    auto* small_id = small.get();
    replay::snap_give(exact);
    replay::snap_give(middle);
    replay::snap_give(small);
    assert(!exact && !middle && !small && replay::g_snap_pool.size() == 3);
    auto chosen = replay::snap_take(32);
    assert(chosen.get() == exact_id && chosen->size() == 32);
    chosen->resize(64);
    replay::snap_give(chosen);
    chosen = replay::snap_take(90);
    assert(chosen.get() == small_id && chosen->size() == 90);
    replay::snap_give(chosen);
    assert(!replay::snap_take(max_capacity + 1));
    uint64_t capacity = 0;
    for (const auto& item : replay::g_snap_pool) capacity += item->capacity();
    assert(capacity == replay::g_snap_pool_bytes);

    // A second owner acquired while an entry is in the pool makes that entry
    // temporarily ineligible, even when no other allocation can fit.
    auto entry = std::find_if(replay::g_snap_pool.begin(), replay::g_snap_pool.end(),
                              [exact_id](const auto& p) { return p.get() == exact_id; });
    assert(entry != replay::g_snap_pool.end());
    auto observer = *entry;
    assert(!replay::snap_take(max_capacity));
    observer.reset();
    chosen = replay::snap_take(max_capacity);
    assert(chosen.get() == exact_id && chosen->size() == max_capacity);
    replay::snap_give(chosen);

    replay::g_snap_pool.clear();
    replay::g_snap_pool_bytes = 0;
    for (size_t i = 0; i < replay::kSnapPoolMax; ++i) {
        auto v = std::make_shared<std::vector<uint8_t>>(i + 1);
        replay::snap_give(v);
        assert(!v);
    }
    auto extra = std::make_shared<std::vector<uint8_t>>();
    replay::snap_give(extra);
    assert(extra && replay::g_snap_pool.size() == replay::kSnapPoolMax);
    capacity = 0;
    for (const auto& item : replay::g_snap_pool) capacity += item->capacity();
    assert(capacity == replay::g_snap_pool_bytes && capacity <= replay::kSnapPoolBytes);

    // Synthetic near-limit accounting checks the guard without allocating
    // 192 MiB, including a value that would wrap the former addition.
    replay::g_snap_pool.clear();
    replay::g_snap_pool_bytes = replay::kSnapPoolBytes - 1;
    auto too_large_for_remaining = std::make_shared<std::vector<uint8_t>>(2);
    replay::snap_give(too_large_for_remaining);
    assert(too_large_for_remaining && replay::g_snap_pool.empty());
    replay::g_snap_pool_bytes = UINT64_MAX;
    replay::snap_give(too_large_for_remaining);
    assert(too_large_for_remaining && replay::g_snap_pool.empty());
    replay::g_snap_pool_bytes = 0;
}

// Saturate with small snapshots before three large snapshots retire.
// Large entries must displace smaller ones, never active observers.
static void snapshot_saturation() {
    for (size_t i = 0; i < replay::kSnapPoolMax; ++i) {
        auto item = std::make_shared<std::vector<uint8_t>>(32, uint8_t(i));
        replay::snap_give(item);
        assert(!item);
    }
    assert(replay::g_snap_pool.size() == replay::kSnapPoolMax);
    const uint64_t actual_bytes = replay::g_snap_pool_bytes;
    auto observer = replay::g_snap_pool.front();
    const auto observed_contents = *observer;

    // Simulate an almost exhausted byte budget: the available small entries
    // cannot free enough for this 1 MiB candidate. A failed admission must
    // leave every entry and the accounting intact (no partial eviction).
    replay::g_snap_pool_bytes = replay::kSnapPoolBytes - 1;
    auto rejected = std::make_shared<std::vector<uint8_t>>(1024 * 1024, 9);
    replay::snap_give(rejected);
    assert(rejected && replay::g_snap_pool.size() == replay::kSnapPoolMax);
    assert(replay::g_snap_pool_bytes == replay::kSnapPoolBytes - 1 && *observer == observed_contents);
    replay::g_snap_pool_bytes = actual_bytes;

    const std::vector<uint8_t>* large_ids[3]{};
    for (unsigned i = 0; i < 3; ++i) {
        auto item = std::make_shared<std::vector<uint8_t>>(1024 * 1024, uint8_t(100 + i));
        large_ids[i] = item.get();
        replay::snap_give(item);
        assert(!item && replay::g_snap_pool.size() == replay::kSnapPoolMax);
        assert(*observer == observed_contents);
        uint64_t sum = 0;
        for (const auto& pooled : replay::g_snap_pool) sum += pooled->capacity();
        assert(sum == replay::g_snap_pool_bytes && sum <= replay::kSnapPoolBytes);
    }
    assert(std::find_if(replay::g_snap_pool.begin(), replay::g_snap_pool.end(),
                        [&](const auto& p) { return p.get() == observer.get(); }) != replay::g_snap_pool.end());
    for (const auto* id : large_ids)
        assert(std::find_if(replay::g_snap_pool.begin(), replay::g_snap_pool.end(),
                            [&](const auto& p) { return p.get() == id; }) != replay::g_snap_pool.end());
    auto small = std::make_shared<std::vector<uint8_t>>(1);
    replay::snap_give(small);
    assert(small && replay::g_snap_pool.size() == replay::kSnapPoolMax);
    auto reused = replay::snap_take(1024 * 1024);
    assert(reused && std::find(std::begin(large_ids), std::end(large_ids), reused.get()) != std::end(large_ids));
    assert(reused->front() >= 100 && reused->front() <= 102);
    replay::snap_give(reused);
    assert(!reused && *observer == observed_contents);
}

static void snapshot_overlap() {
    std::vector<uint8_t> guest(128, 5);
    auto initial = bind(guest.data(), 64);
    land();
    assert(replay::g_snap_pool.size() == 1);
    const auto* recycled = replay::g_snap_pool.front().get();
    auto a = bind(guest.data(), 64);
    assert(replay::g_regions.begin()->second.before.get() == recycled);
    guest[0] = 42;
    a.ptr[4] = 77;
    auto b = replay::written_bind(VK_NULL_HANDLE, guest.data() + 32, 64, 64, false);
    b.ptr[48] = 88;
    land();
    assert(guest[0] == 42 && guest[4] == 77 && guest[80] == 88);
    (void)initial;
}

static void differential() {
    std::mt19937 rng(0x435b7u);
    for (size_t n : {size_t(1), size_t(7), size_t(8), size_t(9), size_t(63), size_t(65536 * 4 + 9), size_t(2 * 1024 * 1024 + 19)}) {
        std::vector<uint8_t> guest(n);
        for (auto& b : guest) b = uint8_t(rng());
        for (unsigned turn = 0; turn < 5; ++turn) {
            auto before = guest; auto up = bind(guest);
            assert(std::memcmp(up.ptr, before.data(), n) == 0);
            for (unsigned j = 0; j < 250; ++j) {
                size_t idx = rng() % n; guest[idx] = uint8_t(rng());
                idx = rng() % n; up.ptr[idx] = uint8_t(rng());
            }
            auto expected = guest; oracle(expected, before, up.ptr); land(); assert(guest == expected);
        }
    }
    close_cache();
}
static void failure(const char* mode) {
    if (!std::strcmp(mode, "no-coherent")) coherent_available = false;
    else if (!std::strcmp(mode, "allocate-failure")) fail_allocate = true;
    else if (!std::strcmp(mode, "bind-failure")) fail_bind = true;
    else if (!std::strcmp(mode, "map-failure")) fail_map = true;
    else if (!std::strcmp(mode, "budget")) req_override = 129ull << 20;
    std::vector<uint8_t> guest(32); auto up = bind(guest); up.ptr[4] = 25; land();
    assert(guest[4] == 25 && replay::g_kept.empty() && replay::g_kept_bytes == 0);
    assert(creates == destroys && allocations == frees && unmaps == 0);
    if (req_override) assert(allocations == 0);
}
static void setup_memory_types() {
    use_memory_props = true;
    memory_props = {};
    memory_props.memoryTypeCount = 32;
    const auto visible = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    const auto coherent = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    const auto cached = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    memory_props.memoryTypes[2].propertyFlags = visible | coherent;
    memory_props.memoryTypes[5].propertyFlags = visible | coherent | cached;
    memory_props.memoryTypes[6].propertyFlags = visible | cached;  // not coherent
    memory_props.memoryTypes[7].propertyFlags = coherent | cached;  // not visible
    memory_props.memoryTypes[31].propertyFlags = visible | coherent | cached;
}
static size_t kept_type_logs() {
    return size_t(std::count_if(log_lines.begin(), log_lines.end(), [](const std::string& line) {
        return line.find("nvn: kept copies memory type ") != std::string::npos;
    }));
}
static void memory_types(bool fallback_log) {
    setup_memory_types();
    const auto bit = [](unsigned i) { return 1u << i; };
    std::vector<uint8_t> guest[5];
    for (auto& range : guest) range.resize(64);

    if (fallback_log) {
        // Cached but incompatible, noncoherent or nonvisible types cannot win.
        req_type_bits = bit(2) | bit(6) | bit(7);
        auto up = bind(guest[0]);
        assert(last_memory_type == 2 && up.buffer == replay::g_kept.begin()->second.buffer);
        land();
        assert(kept_type_logs() == 1);
        assert(log_lines.back() == "nvn: kept copies memory type 2 flags=0x00000006 (HOST_CACHED=no)");
        bind(guest[1]); land();
        assert(kept_type_logs() == 1);
        close_cache();
        return;
    }

    req_type_bits = bit(2) | bit(5) | bit(31);
    fail_allocate = true;
    bind(guest[0]); land();
    assert(last_memory_type == 5 && replay::g_kept.empty());
    assert(kept_type_logs() == 0);  // failed allocations are not reported
    fail_allocate = false;
    auto cached = bind(guest[1]);
    assert(last_memory_type == 5 && cached.buffer == replay::g_kept.begin()->second.buffer);
    land();
    assert(kept_type_logs() == 1);
    assert(log_lines.back() == "nvn: kept copies memory type 5 flags=0x0000000e (HOST_CACHED=yes)");

    // The cached type 5 exists but memoryTypeBits disallows it. The other
    // cached types fail coherence or visibility and the coherent type wins.
    req_type_bits = bit(2) | bit(6) | bit(7);
    auto fallback = bind(guest[2]);
    assert(last_memory_type == 2 && fallback.buffer != cached.buffer);
    assert(replay::g_kept.at(guest[2].data()).buffer == fallback.buffer);
    land();
    assert(kept_type_logs() == 1);

    // A host-cached type is not sufficient without both required flags.
    req_type_bits = bit(6) | bit(7);
    const auto prior_allocations = allocations;
    last_memory_type = UINT32_MAX;
    auto no_coherent = bind(guest[3]);
    assert(last_memory_type == UINT32_MAX && allocations == prior_allocations);
    assert(replay::g_kept.find(guest[3].data()) == replay::g_kept.end());
    assert(no_coherent.buffer != cached.buffer && no_coherent.buffer != fallback.buffer);
    land();

    // Exercise the highest Vulkan type bit and preference over type 2.
    req_type_bits = bit(2) | bit(31);
    auto high_bit = bind(guest[4]);
    assert(last_memory_type == 31 && replay::g_kept.at(guest[4].data()).buffer == high_bit.buffer);
    land();
    assert(kept_type_logs() == 1);
    close_cache();
    assert(creates == destroys && allocations == frees && unmaps == allocations);
}
static void direct_import_fallback(bool kept) {
    // The mock advertises working host imports. Requested features must still take their ordinary copy paths.
    std::vector<uint8_t> written(64), read(64, 7);
    auto up = bind(written);
    assert(direct_import_attempts == 0 && !up.direct && up.ptr != written.data());
    assert((!replay::g_kept.empty()) == kept);  // Exercise both the default ring and optional retained allocation.
    written[0] = 42;
    up.ptr[4] = 77;
    land();
    assert(written[0] == 42 && written[4] == 77);
    replay::written_reset();
    auto input = replay::read_upload(VK_NULL_HANDLE, read.data(), read.size(), read.size(), 4);
    assert(direct_import_attempts == 0 && dedup_uploads == 1 && !input.direct && input.ptr != read.data());
    assert(std::memcmp(input.ptr, read.data(), read.size()) == 0);
    const auto warnings = std::count_if(log_lines.begin(), log_lines.end(), [](const std::string& line) {
        return line.find("NX_HOST_IMPORT/NX_DIRECT_READS disabled") != std::string::npos;
    });
    assert(warnings == 1);  // Both presence-based switches share one warning.
    close_cache();
}
static void logging() {
    // The number of submits need not match the number of presents. Both
    // windows below use production logging and assert concrete per-frame rates.
    setenv("NX_COPYIN_TIMING", "1", 1);
    replay::g_log_submits = 120;
    replay::g_copyin_ns = 900000000;
    replay::g_copyin_bytes = 600000000;
    replay::g_wb_batches = 2;
    replay::g_wb_ranges = 10;
    replay::g_wb_wall_ns = 600000000;
    replay::g_merge_bytes = 300000000;
    replay::written_log(300);
    auto has = [](const char* text) {
        for (const auto& line : log_lines) if (line.find(text) != std::string::npos) return true;
        return false;
    };
    assert(has("copy-in 3.00 ms/frame, 2.0 MB/frame"));
    assert(has("120 submits over 300 presents"));
    assert(has("write-back wall 2.00 ms/frame per batch (5.00 ranges, 1.0 MB"));
    assert(has("window=300 presents"));
    log_lines.clear();
    // A delayed retirement can arrive in a window with no new submits.
    replay::g_wb_batches = 1;
    replay::g_wb_ranges = 4;
    replay::g_wb_wall_ns = 120000000;
    replay::g_merge_bytes = 60000000;
    replay::written_log(60);
    assert(has("write-back wall 2.00 ms/frame per batch (4.00 ranges, 1.0 MB"));
    assert(has("window=60 presents"));
}
// NX_CARRY: the range the later parts of a submit write goes on as the next submit's copy, and nothing is lost either way.
static void carry() {
    using nx::nvn::WriteBack;
    std::vector<uint8_t> early(64, 1), late(64, 2);
    replay::written_reset();
    Upload e = replay::written_bind(VK_NULL_HANDLE, early.data(), 64, 64, false);
    const uint64_t split = replay::written_writable_bind_count();
    Upload l = replay::written_bind(VK_NULL_HANDLE, late.data(), 64, 64, false);
    replay::copy_behind::wait();
    e.ptr[0] = 0x11, l.ptr[0] = 0x22, l.ptr[4] = 0x23;
    std::vector<WriteBack> all = replay::written_take(), now;
    assert(all.size() == 2);
    for (WriteBack& w : all) w.late = true;
    late[8] = 9;  // the guest changed a carried range: the next submit's copy would not have it
    assert(!replay::written_carry_begin(all, split, now) && now.empty() && all.size() == 2);
    late[8] = 2;
    assert(replay::written_carry_begin(all, split, now) && now.size() == 1 && all.size() == 1);
    nx::nvn::write_backs_apply(now);
    assert(early[0] == 0x11 && late[0] == 2);  // the finished range landed, the carried one did not
    replay::written_reset();
    replay::written_carry_seed(VK_NULL_HANDLE);
    Upload r;
    assert(replay::written_read(VK_NULL_HANDLE, late.data(), 64, 64, 16, r));
    assert(r.ptr[0] == 0x22 && r.ptr[4] == 0x23);  // the next submit reads the last one's result, not the guest's
    Upload n = replay::written_bind(VK_NULL_HANDLE, late.data(), 64, 64, false);
    assert(n.ptr == r.ptr);
    n.ptr[4] = 0x33;
    std::vector<WriteBack> next = replay::written_take();
    assert(next.size() == 1);
    next[0].late = true;
    late[16] = 7;  // after the seed: an ordinary guest write
    nx::nvn::write_backs_apply(all);
    assert(late[0] == 0x22 && late[4] == 0x23 && late[16] == 7);
    assert(replay::g_late_conflicts.load() == 0);
    nx::nvn::write_backs_apply(next);
    // A word the last submit wrote is no guest write to this landing, which only has its own change to make.
    assert(late[0] == 0x22 && late[4] == 0x33 && late[16] == 7);
    assert(replay::g_late_conflicts.load() == 0);
}

// What cannot stay a copy: a replay-time write, a CPU read, a write that spans more than the carried range.
static void carry_exits() {
    using nx::nvn::WriteBack;
    std::vector<uint8_t> guest(256, 2);
    auto carried = [&](uint8_t* p, uint64_t size, std::vector<WriteBack>& all) {
        replay::written_reset();
        const uint64_t split = replay::written_writable_bind_count();
        Upload u = replay::written_bind(VK_NULL_HANDLE, p, size, size, false);
        replay::copy_behind::wait();
        u.ptr[0] = 0x44;
        all = replay::written_take();
        all[0].late = true;
        std::vector<WriteBack> now;
        assert(replay::written_carry_begin(all, split, now) && now.empty());
        replay::written_reset();
        replay::written_carry_seed(VK_NULL_HANDLE);
        nx::nvn::carry_lands = [&] { nx::nvn::write_backs_apply(all); };
        assert(replay::written_overlaps(p, size));
    };
    std::vector<WriteBack> all;
    carried(guest.data(), 64, all);
    const unsigned before = nx::nvn::carry_waits;
    guest[8] = 5;  // UpdateUniformBuffer's memcpy, then the call
    replay::written_cpu_write(guest.data() + 8, 4);
    assert(nx::nvn::carry_waits == before + 1 && !replay::written_overlaps(guest.data(), 64));
    assert(guest[0] == 0x44 && guest[8] == 5);  // read from the guest from here on: both are there

    std::fill(guest.begin(), guest.end(), 2);
    carried(guest.data(), 64, all);
    replay::require_host_readable(guest.data(), 64, "test");
    assert(nx::nvn::carry_waits == before + 2 && guest[0] == 0x44);

    std::fill(guest.begin(), guest.end(), 2);
    carried(guest.data(), 64, all);
    Upload wide = replay::written_bind(VK_NULL_HANDLE, guest.data(), 128, 128, false);
    replay::copy_behind::wait();
    assert(nx::nvn::carry_waits == before + 3 && guest[0] == 0x44);
    wide.ptr[64] = 0x55;
    std::vector<WriteBack> next = replay::written_take();
    assert(next.size() == 1);
    assert((*next[0].before)[0] == 0x44);  // its snapshot holds the landed bytes: they are no change of its own
    nx::nvn::write_backs_apply(next);
    assert(guest[0] == 0x44 && guest[64] == 0x55);
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const char* mode = argv[1];
#if defined(_WIN32)
    if (!std::strcmp(mode, "mapping") || !std::strncmp(mode, "kernel-io", 9) ||
        !std::strncmp(mode, "clean-no-read", 13)) {
        std::printf("SKIP %s (requires POSIX mmap/pread)\n", mode);
        return 0;
    }
#endif
    unsetenv("NX_P2_KEPT_COPY"); unsetenv("NX_WRITE_BACK_WHOLE"); unsetenv("NX_HOST_IMPORT");
    unsetenv("NX_DIRECT_READS"); unsetenv("NX_WRITE_BACK_MERGE");
    const bool fill_mode = !std::strncmp(mode, "fill", 4);
    if (!std::strcmp(mode, "fill-merge")) setenv("NX_WRITE_BACK_MERGE", "1", 1);
    const bool simd_mode = std::strstr(mode, "-simd") != nullptr;
    const bool staging_mode = std::strstr(mode, "-staging") != nullptr;
    if (simd_mode && !nx::nvn::merge_detail::kHasAvx2) {
        std::printf("SKIP %s (requires an AVX2 build)\n", mode);
        return 0;
    }
    if (simd_mode) setenv("NX_WRITE_BACK_SIMD", "1", 1);
    if (staging_mode) setenv("NX_WRITE_BACK_DIRECT", "0", 1);
    const bool direct_mode = !std::strncmp(mode, "direct-import", 13);
    const bool host_only = std::strstr(mode, "-host") != nullptr;
    const bool read_only = std::strstr(mode, "-read") != nullptr;
    const bool kept_direct = direct_mode && !host_only && !read_only;
    if (direct_mode) {
        const char* value = std::strstr(mode, "-zero") ? "0" : "1";
        if (!read_only) setenv("NX_HOST_IMPORT", value, 1);
        if (!host_only) setenv("NX_DIRECT_READS", value, 1);
    }
    const bool ordinary_kernel = !std::strcmp(mode, "kernel-io") || !std::strcmp(mode, "kernel-io-no-snapshot");
    const bool snapshot_mode = !std::strncmp(mode, "snapshot-", 9);
    if (snapshot_mode) {
        unsetenv("NX_WRITE_BACK_CHUNKS");
        if (std::strstr(mode, "-plain")) setenv("NX_WRITE_BACK_CHUNKS", "0", 1);
    }
    if (std::strcmp(mode, "default") && std::strcmp(mode, "chunk") && std::strncmp(mode, "carry", 5) && !ordinary_kernel && !snapshot_mode && !fill_mode &&
        (!direct_mode || kept_direct)) setenv("NX_P2_KEPT_COPY", "1", 1);
    const bool parallel_mode = !std::strcmp(mode, "parallel") || !std::strcmp(mode, "snapshot-owner-parallel") ||
                               !std::strcmp(mode, "parallel-simd");
    setenv("NX_COPY_THREADS", parallel_mode ? "3" : "0", 1);
    if (!std::strcmp(mode, "sync")) async_enabled = false;
    if (direct_mode) direct_import_fallback(kept_direct);
    else if (!std::strcmp(mode, "log")) logging();
    else if (!std::strcmp(mode, "memory-types")) memory_types(false);
    else if (!std::strcmp(mode, "memory-types-fallback-log")) memory_types(true);
    else if (!std::strcmp(mode, "chunk")) chunk_regression();
    else if (!std::strcmp(mode, "kept")) kept_regression();
    else if (!std::strcmp(mode, "default") || !std::strcmp(mode, "sync")) default_or_sync();
    else if (!std::strcmp(mode, "fence")) fence();
    else if (!std::strcmp(mode, "fill")) fill_regression(false);
    else if (!std::strcmp(mode, "fill-merge")) fill_regression(true);
    else if (!std::strcmp(mode, "fill-overlap")) fill_overlap_regression();
    else if (!std::strcmp(mode, "overlap")) overlap();
    else if (!std::strcmp(mode, "carry")) carry();
    else if (!std::strcmp(mode, "carry-exits")) carry_exits();
    else if (!std::strcmp(mode, "failure-recorded")) failure_with_recorded_later_entry();
    else if (!std::strcmp(mode, "mapping")) mapping();
    else if (!std::strncmp(mode, "clean-no-read", 13)) unchanged_guest_not_read();
    else if (!std::strcmp(mode, "kernel-io")) kernel_pread(false, false);
    else if (!std::strcmp(mode, "kernel-io-kept")) kernel_pread(true, false);
    else if (!std::strcmp(mode, "kernel-io-no-snapshot")) kernel_pread(false, true);
    else if (!std::strcmp(mode, "kernel-io-kept-no-snapshot")) kernel_pread(true, true);
    else if (!std::strcmp(mode, "snapshot-cycle") || !std::strcmp(mode, "snapshot-cycle-plain")) snapshot_cycle();
    else if (!std::strcmp(mode, "snapshot-owner") || !std::strcmp(mode, "snapshot-owner-parallel") ||
             !std::strcmp(mode, "snapshot-owner-plain")) snapshot_owner();
    else if (!std::strcmp(mode, "snapshot-selection")) snapshot_selection();
    else if (!std::strcmp(mode, "snapshot-saturation")) snapshot_saturation();
    else if (!std::strcmp(mode, "snapshot-overlap")) snapshot_overlap();
    else if (!std::strncmp(mode, "differential", 12) || !std::strcmp(mode, "parallel") ||
             !std::strcmp(mode, "parallel-simd")) differential();
    else if (!std::strcmp(mode, "no-coherent") || !std::strcmp(mode, "allocate-failure") ||
             !std::strcmp(mode, "bind-failure") || !std::strcmp(mode, "map-failure") || !std::strcmp(mode, "budget")) failure(mode);
    else return 2;
    std::printf("PASS %s\n", mode);
    return 0;
}
