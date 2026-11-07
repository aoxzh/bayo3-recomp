// Exercises production BindProgram replay with separate stage programs and null unbinds.
#include "../runtime/src/nvn/nvn_draw_state.h"
#include <cstdio>
#include <map>
#include <stdexcept>

using namespace nx::nvn;
using namespace nx::nvn::replay;
namespace {
std::map<uint64_t, std::vector<const ShaderInfo*>> programs;
int forwarded_draws = 0;
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void bind(uint64_t program, uint32_t mask) {
    Cmd command{Cmd::BindProgram};
    command.a[0] = program;
    command.mask = mask;
    draw_command(VK_NULL_HANDLE, command, CmdList{}, Targets{});
}
}
namespace nx {
void log(const char*, ...) {}
}
namespace nx::plat {
uint64_t monotonic_ns() { return 0; }
}
namespace nx::nvn {
const std::vector<const ShaderInfo*>* program_shaders(uint64_t program) {
    auto found = programs.find(program);
    return found == programs.end() ? nullptr : &found->second;
}
const DeferredValues* current_deferred() { return nullptr; }
uint64_t submit_start_ns() { return 0; }
bool draw_call(VkCommandBuffer, const Targets&, uint32_t mode, bool indexed, const Cmd& command) {
    check(indexed && mode == command.n, "indirect dispatch preserves draw arguments");
    ++forwarded_draws;
    return false;
}
void dispatch_call(VkCommandBuffer, const Cmd&) {}
namespace replay {
State g_state;
Stats g_stats;
const uint8_t* guest_bytes(uint64_t) { return nullptr; }
void dedup_invalidate(const uint8_t*, uint64_t) {}
void written_cpu_write(const uint8_t*, uint64_t) {}
bool raster_command(const Cmd&) { return false; }
}
}
int main() {
    try {
        ShaderInfo vs{}, fs{}, other_vs{}, other_fs{}, cs{}, tcs{}, tes{}, unknown{};
        vs.stage = other_vs.stage = ShaderStage::Vertex;
        fs.stage = other_fs.stage = ShaderStage::Fragment;
        cs.stage = ShaderStage::Compute;
        tcs.stage = ShaderStage::TessellationControl;
        tes.stage = ShaderStage::TessellationEvaluation;
        programs[5] = {&tcs, &tes, &other_vs};
        programs[1] = {&vs};
        programs[2] = {&fs};
        programs[3] = {&other_vs, &other_fs, &cs, &unknown};
        programs[4] = {&cs};
        check(selected_program_shaders(g_state).empty(), "initial state unbound");
        // Actual B3 main+d0a4e4/d0a508/d0a52c/d0a354 sequence.
        bind(1, 1);
        bind(2, 2);
        bind(0, 8);
        bind(0, 16);
        auto selected = selected_program_shaders(g_state);
        check(selected == std::vector<const ShaderInfo*>{&vs, &fs}, "FS/null tess binds preserve VS");
        bind(5, 0x8);
        check(selected_program_shaders(g_state) == std::vector<const ShaderInfo*>{&vs, &fs, &tcs}, "mask8 selects tessellation control only");
        bind(5, 0x10);
        check(selected_program_shaders(g_state) == std::vector<const ShaderInfo*>{&vs, &fs, &tcs, &tes}, "mask10 selects tessellation evaluation independently");
        bind(0, 0x8);
        check(selected_program_shaders(g_state) == std::vector<const ShaderInfo*>{&vs, &fs, &tes}, "null control unbind preserves evaluation");
        bind(0, 0x10);
        check(selected_program_shaders(g_state) == selected, "null tessellation unbind preserves VS/FS");
        check(program_stage_slot(tcs.stage) == 3 && program_stage_slot(tes.stage) == 4, "tessellation NVN slots ignore enum ordinals");
        bind(4, 32);
        check(selected_program_shaders(g_state) == selected, "compute bind preserves graphics");
        check(selected_program_shaders(g_state, 32) == std::vector<const ShaderInfo*>{&cs}, "compute selects only compute slot");
        bind(3, 2);
        check(selected_program_shaders(g_state) == std::vector<const ShaderInfo*>{&vs, &other_fs}, "masked multistage program filters unselected shaders");
        bind(3, 0);
        bind(3, 0x80000000u);
        check(g_state.programs[0] == 1 && g_state.programs[1] == 3 && g_state.programs[5] == 4, "zero/unknown mask changes no stage");
        bind(0, 2);
        check(selected_program_shaders(g_state) == std::vector<const ShaderInfo*>{&vs}, "null unbind clears FS only");
        bind(3, 3);
        check(selected_program_shaders(g_state) == std::vector<const ShaderInfo*>{&other_vs, &other_fs}, "combined VS/FS binding remains supported");
        bind(0, 1);
        check(selected_program_shaders(g_state) == std::vector<const ShaderInfo*>{&other_fs}, "null VS never falls back to legacy last program");
        bind(999, 1);
        check(selected_program_shaders(g_state) == std::vector<const ShaderInfo*>{&other_fs}, "unknown program provides no stage");
        check(program_stage_slot(ShaderStage::Unknown) == -1, "unknown stage is not vertex");
        // Reuse the output allocation, but always read the current program's stages. A program can
        // change under the same guest handle without another BindProgram command.
        std::vector<const ShaderInfo*> scratch;
        scratch.reserve(16);
        const auto* scratch_data = scratch.data();
        const size_t scratch_capacity = scratch.capacity();
        bind(3, 3);
        selected_program_shaders(g_state, 0x1f, scratch);
        check(scratch == std::vector<const ShaderInfo*>{&other_vs, &other_fs}, "reused selection orders graphics stages");
        programs[3] = {&fs, nullptr, &vs, &other_vs, &cs};
        selected_program_shaders(g_state, 0x1f, scratch);
        check(scratch == std::vector<const ShaderInfo*>{&vs, &other_vs, &fs}, "same-handle mutation and duplicate stage order remain visible");
        selected_program_shaders(g_state, 0x20, scratch);
        check(scratch == std::vector<const ShaderInfo*>{&cs}, "reused output does not retain graphics stages during compute selection");
        selected_program_shaders(g_state, 0, scratch);
        check(scratch.empty(), "zero mask clears previous selection");
        bind(0, 3);
        selected_program_shaders(g_state, 0x1f, scratch);
        check(scratch.empty(), "reused output respects null stage unbinds");
        check(scratch.data() == scratch_data && scratch.capacity() == scratch_capacity,
              "warmed selection storage survives changing masks and programs");
        for (auto op : {Cmd::DrawElementsIndirect, Cmd::MultiDrawElementsIndirectCount}) {
            Cmd command{op};
            command.n = 5;
            draw_command(VK_NULL_HANDLE, command, CmdList{}, Targets{});
        }
        check(forwarded_draws == 2, "indirect operations reach draw replay");
        std::puts("PASS production stage-mask replay: separate VS/FS, null unbind, combined program, compute isolation");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL %s\n", error.what());
        return 1;
    }
}
