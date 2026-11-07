// NVN programs (docs/NVN.md, N5): the shader stages the game binds. Each stage arrives as Maxwell code at a GPU
// address in one of its memory pools, plus the NVN control section (docs/archive/nvn/SHADERS.md). The code was translated
// to SPIR-V offline (docs/nvn/TRANSLATOR.md); nvn_shaders.cpp finds each stage's by the hash of its code block.
//
// nvnProgramSetShaders(NVNprogram*, int count, const NVNshaderData*): {NVNbufferAddress code; const void* control}
// per stage (0x10 bytes); the Ext variant's entries have a third, zero word (0x18 bytes).
#include <cstdlib>
#include <string>
#include <vector>

#include "nvn.h"
#include "nvn_shaders.h"

namespace nx::nvn {

namespace {

struct Program : Object {
    std::vector<const ShaderInfo*> shaders;
    struct PendingStage {
        std::string hash;
        ShaderControl control;
        uint32_t sph_type, stage_mask;
    };
    std::vector<PendingStage> pending;  // valid originals, never executable as a partial program
};

void ProgramInitialize(CPU& c) {  // NVNboolean (NVNprogram*, NVNdevice*)
    make<Program>(c.x[0]);
    c.x[0] = 1;
}

void ProgramFinalize(CPU& c) { destroy(c.x[0]); }

void set_shaders(CPU& c, size_t stride) {
    std::vector<const ShaderInfo*> shaders;
    std::vector<Program::PendingStage> pending;
    for (int k = 0; k < int(c.x[1]); k++) {
        uint64_t entry = c.x[2] + uint64_t(k) * stride;
        uint64_t code = ld<uint64_t>(entry), control = ld<uint64_t>(entry + 8);
        uint64_t offset = 0;
        MemoryPool* pool = pool_at_gpu(code, &offset);
        if (!pool || !control) {
            log("nvn: program %llx stage %d: code %llx control %llx not found", (unsigned long long)c.x[0], k,
                (unsigned long long)code, (unsigned long long)control);
            if (strict_runtime()) fatal("nvn: program stage has no mapped shader code/control");
            continue;
        }
        if (const ShaderInfo* s = shader_for(pool->memory + offset, control)) {
            shaders.push_back(s);
        } else {
            // NVN resource initialization does not execute a shader. Preserve a
            // valid original's identity, and reject use of the whole program at
            // draw/dispatch rather than silently executing its translated subset.
            auto ctrl = read_control(reinterpret_cast<const uint8_t*>(control));
            if (!ctrl.valid || !ctrl.code_size || ctrl.code_size > (16u << 20) ||
                offset > pool->size || ctrl.code_size > pool->size - offset ||
                ctrl.program_start > ctrl.code_size || ctrl.code_size - ctrl.program_start < 4)
                fatal("nvn: program stage has invalid original shader extent");
            const auto* bytes = reinterpret_cast<const uint8_t*>(pool->memory + offset);
            uint32_t sph = ld<uint32_t>(pool->memory + offset + ctrl.program_start);
            const bool compute = ctrl.program_start != 0x30;
            const uint32_t type = compute ? 0 : (sph >> 10) & 15;
            const uint32_t mask = compute ? 0x20 : type == 1 ? 1 : type == 5 ? 2 : type == 4 ? 4 :
                                  type == 2 ? 8 : type == 3 ? 0x10 : 0;
            if (!mask) fatal("nvn: program stage has unknown original SPH type %u", type);
            pending.push_back({shader_hash(bytes, ctrl.code_size), ctrl, type, mask});
        }
    }
    static const bool trace = std::getenv("NX_TRACE_PROGRAMS") != nullptr;  // each program's stages when set
    if (trace) {
        std::string line;
        for (const ShaderInfo* s : shaders) line += " " + s->hash;
        log("nvn: program %llx set:%s", (unsigned long long)c.x[0], line.c_str());
    }
    if (auto* p = get<Program>(c.x[0])) {
        p->shaders = std::move(shaders);
        p->pending = std::move(pending);
    }
    c.x[0] = 1;
}

void ProgramSetShaders(CPU& c) { set_shaders(c, 0x10); }
void ProgramSetShadersExt(CPU& c) { set_shaders(c, 0x18); }

}  // namespace

bool program_require_complete(uint64_t program, uint32_t selected_mask) {
    // Zero unbinds a stage; there is no program to resolve or check.
    if (!program) return true;
    // A shader the translator could not handle stops the program from being usable. Failing hard here ends the whole
    // run on one draw of one scene, which is worse than the draw not happening: the caller skips it and the log names
    // the shader, so a translation gap shows as a missing object instead of a dead process. NX_STRICT_SHADERS=1 keeps
    // the old abort for translator work, where the first gap must not be papered over.
    static const bool strict = std::getenv("NX_STRICT_SHADERS") != nullptr;
    if (auto* p = get<Program>(program))
        for (const auto& stage : p->pending)
            if (stage.stage_mask & selected_mask) {
                if (strict)
                    fatal("nvn: program %llx cannot execute selected stage %#x: original shader %s (SPH %u, code %#x) "
                          "has no usable translation", (unsigned long long)program, stage.stage_mask,
                          stage.hash.c_str(), stage.sph_type, stage.control.code_size);
                static int skipped = 0;
                if (skipped++ < 20 || skipped % 1000 == 0)
                    log("nvn: program %llx skipped: stage %#x shader %s (SPH %u, code %#x) has no usable translation "
                        "(%d such draws)", (unsigned long long)program, stage.stage_mask, stage.hash.c_str(),
                        stage.sph_type, stage.control.code_size, skipped);
                return false;
            }
    return true;
}

const std::vector<const ShaderInfo*>* program_shaders(uint64_t program) {
    auto* p = get<Program>(program);
    return p ? &p->shaders : nullptr;
}

NX_HLE("nvnProgramInitialize", ProgramInitialize);
NX_HLE("nvnProgramFinalize", ProgramFinalize);
NX_HLE("nvnProgramSetShaders", ProgramSetShaders);
NX_HLE("nvnProgramSetShadersExt", ProgramSetShadersExt);

}  // namespace nx::nvn
