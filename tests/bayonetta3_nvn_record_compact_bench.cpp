// Isolated CPU benchmark of production recording/state replay, using the regression test's no-GPU stubs.
#define main regression_main
#include "bayonetta3_nvn_record_batch_test.cpp"
#undef main
#include <chrono>

int main() {
    CPU cpu{}; cpu.x[0] = cb_address; CommandBufferInitialize(cpu);
    uint64_t values[64];
    for (uint64_t k = 0; k < 64; ++k) values[k] = 0x100000 + k * 16;
    double record_ms = 0, replay_ms = 0;
    uint64_t physical = 0, logical = 0, snapshot = 0, checksum = 0;
    using Clock = std::chrono::steady_clock;
    for (int frame = 0; frame < 120; ++frame) {
        const auto start = Clock::now();
        cpu = {}; cpu.x[0] = cb_address; CommandBufferBeginRecording(cpu);
        for (int draw = 0; draw < 512; ++draw) {
            cpu = {}; cpu.x[0] = cb_address; cpu.x[1] = draw % 6;
            cpu.x[2] = 0; cpu.x[3] = 18; cpu.x[4] = reinterpret_cast<uint64_t>(values);
            BindUniformBuffers(cpu);
            cpu.x[3] = 16; BindTextures(cpu);
            cpu.x[3] = 4; BindStorageBuffers(cpu);
        }
        auto list = get<CommandBuffer>(cb_address)->recording;
        const auto recorded = Clock::now();
        for (const Cmd& cmd : list->cmds) draw_command(VK_NULL_HANDLE, cmd, *list, Targets{});
        const auto replayed = Clock::now();
        record_ms += std::chrono::duration<double, std::milli>(recorded - start).count();
        replay_ms += std::chrono::duration<double, std::milli>(replayed - recorded).count();
        physical = list->cmds.size(); logical = list->command_count(); snapshot = 0;
        for (const auto& b : list->blobs) snapshot += b.size();
        checksum += nx::nvn::replay::g_state.uniforms[0][17].address;
    }
    std::printf("logical=%llu physical=%llu bytes=%llu snapshot=%llu record_ms=%.4f replay_ms=%.4f checksum=%llu\n",
                (unsigned long long)logical, (unsigned long long)physical,
                (unsigned long long)(physical * sizeof(Cmd)), (unsigned long long)snapshot,
                record_ms / 120, replay_ms / 120, (unsigned long long)checksum);
}
