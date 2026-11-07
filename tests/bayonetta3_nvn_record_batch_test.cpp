// Exercise the production plural recording entry points, without a game or GPU.
#include "../runtime/src/nvn/nvn.h"
#include "../runtime/src/nvn/nvn_draw_state.h"
#undef NX_HLE
#define NX_HLE(sym, fn)
#include "../runtime/src/nvn/nvn_cmd.cpp"
#include "../runtime/src/nvn/nvn_binds.cpp"
#include <stdexcept>

using namespace nx;
using namespace nx::nvn;
namespace {
void check(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
constexpr uint64_t cb_address = 0x1000;
std::vector<Cmd> expected;
uint64_t digest = 1469598103934665603ull;
uint64_t registry_lookups = 0;
void hash(uint64_t value) { digest = (digest ^ value) * 1099511628211ull; }
void compare(const Cmd& a, const Cmd& b) {
    check(a.op == b.op && a.n == b.n && a.mask == b.mask && a.blob == b.blob, "command metadata/order");
    check(std::memcmp(a.a, b.a, sizeof a.a) == 0, "command addresses/zero fields");
    check(std::memcmp(a.i, b.i, sizeof a.i) == 0, "command indices/stage/zero fields");
    check(std::memcmp(a.f, b.f, sizeof a.f) == 0, "command float fields");
    hash(a.op); hash(a.n); hash(a.mask); hash(a.blob);
    for (uint64_t value : a.a) hash(value);
    for (int32_t value : a.i) hash(uint32_t(value));
}
bool compact_enabled() {
    const char* value = std::getenv("NX_RECORD_COMPACT");
#if defined(__linux__) && !defined(__ANDROID__)
    if (!value) return true;
#endif
    return value && std::strcmp(value, "1") == 0;
}
bool batch_enabled() {
    const char* value = std::getenv("NX_RECORD_BATCH");
    return compact_enabled() || (value && std::strcmp(value, "1") == 0);
}
void check_list(const CmdList& list) {
    check(list.command_count() == expected.size(), "logical command count");
    size_t index = 0;
    for (const Cmd& cmd : list.cmds) {
        if (cmd.op != Cmd::BindInline) { compare(cmd, expected.at(index++)); continue; }
        check(compact_enabled() && cmd.blob < list.blobs.size(), "inline owns its snapshot");
        const bool vertex = cmd.mask == Cmd::BindVertexBuffer;
        const bool range = vertex || cmd.mask == Cmd::BindUniformBuffer || cmd.mask == Cmd::BindStorageBuffer;
        const size_t stride = range ? 16 : 8;
        const auto& bytes = list.blobs[cmd.blob];
        check(bytes.size() == size_t(cmd.n) * stride, "snapshot extent");
        for (uint32_t k = 0; k < cmd.n; ++k) {
            Cmd scalar{Cmd::Op(cmd.mask)};
            if (vertex) scalar.i[0] = cmd.i[1] + int32_t(k);
            else scalar.i[0] = cmd.i[0], scalar.i[1] = cmd.i[1] + int32_t(k);
            std::memcpy(scalar.a, bytes.data() + size_t(k) * stride, stride);
            compare(scalar, expected.at(index++));
        }
    }
    check(index == expected.size(), "all commands compared");
    // The same production replay runs both encodings. Repeating the list also checks snapshot ownership.
    using namespace nx::nvn::replay;
    g_state = State{};
    for (const Cmd& cmd : expected) draw_command(VK_NULL_HANDLE, cmd, CmdList{}, Targets{});
    const State scalar = g_state;
    g_state = State{};
    for (const Cmd& cmd : list.cmds) draw_command(VK_NULL_HANDLE, cmd, list, Targets{});
    check(std::memcmp(scalar.uniforms, g_state.uniforms, sizeof scalar.uniforms) == 0, "uniform replay");
    check(std::memcmp(scalar.storage, g_state.storage, sizeof scalar.storage) == 0, "storage replay");
    check(std::memcmp(scalar.textures, g_state.textures, sizeof scalar.textures) == 0, "texture replay");
    check(std::memcmp(scalar.images, g_state.images, sizeof scalar.images) == 0, "image replay");
    check(std::memcmp(scalar.vertex_buffers, g_state.vertex_buffers, sizeof scalar.vertex_buffers) == 0, "vertex replay");
}
void check_recording() {
    auto* cb = get<CommandBuffer>(cb_address);
    check(cb != nullptr, "command buffer exists");
    check_list(*cb->recording);
    CPU query{};
    query.x[0] = cb_address; memory_query<true, 0>(query);
    check(query.x[0] == std::min(cb->command_base + expected.size() * 16,
                               cb->command_size > 64 ? cb->command_size - 64 : 0), "guest command usage");
    query.x[0] = cb_address; memory_query<false, 0>(query);
    check(query.x[0] == std::min(cb->control_base + expected.size() * 4,
                               cb->control_size > 64 ? cb->control_size - 64 : 0), "guest control usage");
}
void plural(void (*entry)(CPU&), Cmd::Op op, int32_t count, bool null_array = false,
            int32_t first = 7, int32_t stage = 3) {
    const bool vertex = op == Cmd::BindVertexBuffer;
    const bool range = vertex || op == Cmd::BindUniformBuffer || op == Cmd::BindStorageBuffer;
    std::vector<uint64_t> values(size_t(std::max(count, 1)) * (range ? 2 : 1));
    for (size_t k = 0; k < values.size(); ++k) values[k] = 0x12340000 + k * 31;
    CPU cpu{};
    cpu.x[0] = cb_address;
    const uint64_t address = null_array ? 0 : reinterpret_cast<uint64_t>(values.data());
    if (vertex) cpu.x[1] = first, cpu.x[2] = uint64_t(count), cpu.x[3] = address;
    else cpu.x[1] = stage, cpu.x[2] = first, cpu.x[3] = uint64_t(count), cpu.x[4] = address;
    for (int32_t k = 0; k < count && address; ++k) {
        Cmd cmd{op};
        if (vertex) cmd.i[0] = first + k;
        else cmd.i[0] = stage, cmd.i[1] = first + k;
        cmd.a[0] = values[size_t(k) * (range ? 2 : 1)];
        if (range) cmd.a[1] = values[size_t(k) * 2 + 1];
        expected.push_back(cmd);
    }
    const uint64_t lookups_before = registry_lookups;
    entry(cpu);
    if (batch_enabled())
        check(registry_lookups - lookups_before == uint64_t(count > 0 && address), "one lookup per nonempty batch");
    // All ordinary plural binds snapshot now, even when arrays are overwritten before EndRecording.
    std::fill(values.begin(), values.end(), UINT64_MAX);
    Cmd sentinel{Cmd::DrawArrays}; sentinel.i[0] = 99;
    record(cb_address, sentinel); expected.push_back(sentinel);
    check_recording();
}
}
namespace nx {
void log(const char*, ...) {}
[[noreturn]] void fatal(const char*, ...) { throw std::runtime_error("production fatal"); }
std::string describe(uint64_t) { return "test"; }
}
namespace nx::plat {
uint64_t monotonic_ns() { return 0; }
}
namespace nx::nvn {
const DeferredValues* current_deferred() { return nullptr; }
uint64_t submit_start_ns() { return 0; }
bool draw_call(VkCommandBuffer, const Targets&, uint32_t, bool, const Cmd&) { return false; }
void dispatch_call(VkCommandBuffer, const Cmd&) {}
namespace replay {
State g_state;
Stats g_stats;
const uint8_t* guest_bytes(uint64_t) { return nullptr; }
void dedup_invalidate(const uint8_t*, uint64_t) {}
void written_cpu_write(const uint8_t*, uint64_t) {}
bool raster_command(const Cmd&) { return false; }
}
ObjectsLock& objects_lock() { static ObjectsLock lock; return lock; }
std::unordered_map<uint64_t, std::unique_ptr<Object>>& objects() {
    static std::unordered_map<uint64_t, std::unique_ptr<Object>> registry; return registry;
}
bool resolve_cache_on() { return false; }
Object* find_object(uint64_t guest, uint64_t) {
    ++registry_lookups;
    auto it = objects().find(guest); return it == objects().end() ? nullptr : it->second.get();
}
void defer_object(std::unique_ptr<Object>) {}
uint64_t current_epoch() { return 1; }
const std::vector<const ShaderInfo*>* program_shaders(uint64_t) { return nullptr; }
ViewInfo read_view(CPU&, uint64_t) { return {}; }
}
int main() {
    try {
        check(record_bindings(0, Cmd::BindTexture, 0, 0, 0, 0) == batch_enabled(), "exact batch environment switch");
        CPU cpu{}; cpu.x[0] = cb_address; CommandBufferInitialize(cpu);
        auto* initial = get<CommandBuffer>(cb_address);
        initial->command_size = 1 << 20; initial->control_size = 1 << 20;
        const std::pair<void (*)(CPU&), Cmd::Op> entries[] = {
            {BindUniformBuffers, Cmd::BindUniformBuffer}, {BindStorageBuffers, Cmd::BindStorageBuffer},
            {BindTextures, Cmd::BindTexture}, {BindImages, Cmd::BindImage}, {BindVertexBuffers, Cmd::BindVertexBuffer}
        };
        for (auto [entry, op] : entries) {
            plural(entry, op, 0); plural(entry, op, -2); plural(entry, op, 3, true);
            plural(entry, op, 1); plural(entry, op, 5); plural(entry, op, 257);
            for (int32_t first : {-40, -3, 0, 14, 15, 16, 17, 18, 30, 31, 32, 40})
                plural(entry, op, 5, false, first);
            for (int32_t stage : {-1, 0, 5, 6}) plural(entry, op, 3, false, 1, stage);
        }
        // Deferred entry points retain their old opt-in semantics. When disabled they use the same immediate path.
        const bool deferred = std::getenv("NX_DEFERRED_BINDS") != nullptr;
        const std::pair<void (*)(CPU&), Cmd::Op> deferred_entries[] = {
            {bind_deferred<0>, Cmd::BindTexture}, {bind_deferred<1>, Cmd::BindImage},
            {bind_deferred<2>, Cmd::BindUniformBuffer}, {bind_deferred<3>, Cmd::BindStorageBuffer}
        };
        if (!deferred) {
            for (auto [entry, op] : deferred_entries) plural(entry, op, 4);
        } else {
            uint64_t values[] = {7, 8, 9, 10};
            for (uint32_t kind = 0; kind < 4; ++kind) {
                cpu = {}; cpu.x[0] = cb_address; cpu.x[1] = 2; cpu.x[2] = 9; cpu.x[3] = 2;
                cpu.x[4] = reinterpret_cast<uint64_t>(values);
                deferred_entries[kind].first(cpu);
                Cmd want{Cmd::BindDeferred}; want.mask = kind; want.n = 2;
                want.i[0] = 2; want.i[1] = 9; want.a[0] = cpu.x[4];
                auto& actual = get<CommandBuffer>(cb_address)->recording->cmds.back();
                // The deferred-mode result below omits the digest because this address depends on ASLR.
                compare(actual, want); expected.push_back(want);
            }
            values[0] = 123;
            check(ld<uint64_t>(get<CommandBuffer>(cb_address)->recording->cmds.back().a[0]) == 123,
                  "enabled deferred bind retains its source array");
        }
        // EndRecording transfers the list, and beginning/reinitializing must not append through stale cached pointers.
        cpu = {}; cpu.x[0] = cb_address; CommandBufferEndRecording(cpu);
        auto finished = command_list(cpu.x[0]);
        check(finished && finished->command_count() == expected.size(), "EndRecording transfers full batch");
        check(initial->command_base == (expected.size() + 1) * 16, "EndRecording command extent");
        check(initial->control_base == (expected.size() + 1) * 4, "EndRecording control extent");
        if (!deferred) {
            const CmdList copied = *finished;
            check_list(copied);
        }
        cpu = {}; cpu.x[0] = cb_address; CommandBufferBeginRecording(cpu);
        expected.clear(); plural(BindTextures, Cmd::BindTexture, 3);
        cpu = {}; cpu.x[0] = cb_address; CommandBufferFinalize(cpu);
        cpu = {}; cpu.x[0] = cb_address; CommandBufferInitialize(cpu);
        expected.clear(); plural(BindStorageBuffers, Cmd::BindStorageBuffer, 2);
        // Reinitialized command buffers still resolve command memory correctly.
        auto& pool = make<MemoryPool>(0x2000); pool.gpu = 0x100000; pool.size = 4096;
        cpu = {}; cpu.x[0] = cb_address; cpu.x[1] = 0x2000; cpu.x[2] = 64; cpu.x[3] = 1024;
        AddCommandMemory(cpu);
        check(get<CommandBuffer>(cb_address)->command_memory == 0x100040, "command memory pool resolution");
        uint64_t handles[] = {41, 42, 43};
        cpu = {}; cpu.x[0] = 0xdead; cpu.x[1] = 2; cpu.x[2] = 5; cpu.x[3] = 3;
        cpu.x[4] = reinterpret_cast<uint64_t>(handles);
        const uint64_t missing_before = g_no_cb.load();
        BindTextures(cpu);
        check(g_no_cb.load() - missing_before == 3, "unknown command buffer counts every skipped bind");
        if (deferred) puts("PASS deferred recording and lifecycle");
        else std::printf("PASS immediate recording and lifecycle: %llx\n", (unsigned long long)digest);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what()); return 1;
    }
    return 0;
}
