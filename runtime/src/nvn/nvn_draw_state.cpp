// The replay's state commands (split from nvn_draw.cpp): binds, viewports, uniform updates, counters; draws and
// dispatches go back to nvn_draw.cpp.
#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "nvn_copy_behind.h"
#include "nvn_draw_state.h"
#include "platform.h"

namespace nx::nvn {

using namespace replay;

namespace replay {

void bind_program_stages(State& state, uint64_t program, uint32_t stage_mask) {
    state.program = program;
    for (int stage = 0; stage < kStages; ++stage)
        if (stage_mask & (1u << stage)) state.programs[stage] = program;
}

int program_stage_slot(ShaderStage stage) {
    switch (stage) {
        case ShaderStage::Vertex: return 0;
        case ShaderStage::Fragment: return 1;
        case ShaderStage::TessellationControl: return 3;
        case ShaderStage::TessellationEvaluation: return 4;
        case ShaderStage::Compute: return 5;
        default: return -1;
    }
}

void selected_program_shaders(const State& state, uint32_t stage_mask, std::vector<const ShaderInfo*>& selected) {
    selected.clear();
    for (int stage = 0; stage < kStages; ++stage) {
        if (!(stage_mask & (1u << stage)) || !state.programs[stage]) continue;
        if (const auto* shaders = program_shaders(state.programs[stage]))
            for (const ShaderInfo* shader : *shaders)
                if (shader && program_stage_slot(shader->stage) == stage) selected.push_back(shader);
    }
}

std::vector<const ShaderInfo*> selected_program_shaders(const State& state, uint32_t stage_mask) {
    std::vector<const ShaderInfo*> selected;
    selected_program_shaders(state, stage_mask, selected);
    return selected;
}

}  // namespace replay

void draw_command(VkCommandBuffer cb, const Cmd& c, const CmdList& list, const Targets& targets) {
    // NX_REPLAY_PROF: the state commands are most of what a submit replays, and they all land here.
    replay_prof_cmd_begin();
    struct CmdScope {
        explicit CmdScope(uint32_t o) : op(o) {}
        ~CmdScope() { replay_prof_cmd_end(op); }
        const uint32_t op;
    } cmd_scope(c.op);
    State& s = g_state;
    auto blob = [&](size_t size) -> const uint8_t* {
        return c.blob < list.blobs.size() && list.blobs[c.blob].size() >= size ? list.blobs[c.blob].data() : nullptr;
    };
    switch (c.op) {
        case Cmd::SetViewport:
            std::memcpy(s.viewport, c.i, sizeof s.viewport), s.has_viewport = true;
            break;
        case Cmd::SetRasterizerDiscard: s.rasterizer_discard = c.n != 0; break;
        case Cmd::BindImage:
            if (c.i[0] >= 0 && c.i[0] < kStages && c.i[1] >= 0 && c.i[1] < 16) s.images[c.i[0]][c.i[1]] = c.a[0];
            break;
        case Cmd::DispatchCompute: dispatch_call(cb, c); break;
        case Cmd::SetRenderEnable: s.render_enable = c.n != 0; break;
        case Cmd::SetScissor:
            std::memcpy(s.scissor, c.i, sizeof s.scissor), s.has_scissor = true;
            break;
        case Cmd::SetDepthRange:
            s.depth_near = c.f[0], s.depth_far = c.f[1];
            break;
        case Cmd::BindProgram: bind_program_stages(s, c.a[0], c.mask); break;
        case Cmd::BindUniformBuffer:
            if (c.i[0] >= 0 && c.i[0] < kStages && c.i[1] >= 0 && c.i[1] < 18) s.uniforms[c.i[0]][c.i[1]] = {c.a[0], c.a[1]};
            break;
        case Cmd::BindStorageBuffer:
            if (c.i[0] >= 0 && c.i[0] < kStages && c.i[1] >= 0 && c.i[1] < 16) s.storage[c.i[0]][c.i[1]] = {c.a[0], c.a[1]};
            break;
        case Cmd::BindInline: {
            const int stage = c.i[0];
            if (c.mask != Cmd::BindVertexBuffer && (stage < 0 || stage >= kStages)) break;
            auto copy = [&](auto& slots) {
                const size_t stride = sizeof(slots[0]);
                const uint8_t* values = blob(size_t(c.n) * stride);
                if (!values) return;
                const int64_t first = c.i[1];
                const int64_t lo = std::max<int64_t>(0, first);
                const int64_t hi = std::min<int64_t>(sizeof(slots) / stride, first + c.n);
                if (lo < hi)
                    std::memcpy(slots + lo, values + size_t(lo - first) * stride, size_t(hi - lo) * stride);
            };
            switch (c.mask) {
                case Cmd::BindUniformBuffer: copy(s.uniforms[stage]); break;
                case Cmd::BindStorageBuffer: copy(s.storage[stage]); break;
                case Cmd::BindTexture: copy(s.textures[stage]); break;
                case Cmd::BindImage: copy(s.images[stage]); break;
                case Cmd::BindVertexBuffer: copy(s.vertex_buffers); break;
                default: break;
            }
            break;
        }
        case Cmd::BindDeferred: {
            const DeferredValues* dv = current_deferred();
            auto it = dv ? dv->find(c.a[0]) : DeferredValues::const_iterator{};
            uint32_t words = c.mask >= 2 ? 2 : 1;
            int st = c.i[0];
            if (st < 0 || st >= kStages) break;
            for (uint32_t k = 0; k < c.n; k++) {
                auto value = [&](uint32_t w) {  // the submit's snapshot, else the memory now
                    size_t at = size_t(k) * words + w;
                    return dv && it != dv->end() && at < it->second.size() ? it->second[at]
                                                                            : ld<uint64_t>(c.a[0] + 8 * at);
                };
                int slot = c.i[1] + int(k);
                if (c.mask == 0 && slot >= 0 && slot < 32) s.textures[st][slot] = value(0);
                if (c.mask == 1 && slot >= 0 && slot < 16) s.images[st][slot] = value(0);
                if (c.mask == 2 && slot >= 0 && slot < 18) s.uniforms[st][slot] = {value(0), value(1)};
                if (c.mask == 3 && slot >= 0 && slot < 16) s.storage[st][slot] = {value(0), value(1)};
            }
            break;
        }
        case Cmd::BindTexture:
            if (c.i[0] >= 0 && c.i[0] < kStages && c.i[1] >= 0 && c.i[1] < 32) s.textures[c.i[0]][c.i[1]] = c.a[0];
            break;
        case Cmd::BindVertexBuffer:
            if (c.i[0] >= 0 && c.i[0] < 16) s.vertex_buffers[c.i[0]] = {c.a[0], c.a[1]};
            break;
        case Cmd::SetTexturePool: s.texture_pool = c.a[0]; break;
        case Cmd::SetSamplerPool: s.sampler_pool = c.a[0]; break;
        case Cmd::BindVertexAttribs:
            if (auto* p = blob(c.n * sizeof(VertexAttrib)))
                s.attribs.assign(reinterpret_cast<const VertexAttrib*>(p), reinterpret_cast<const VertexAttrib*>(p) + c.n);
            break;
        case Cmd::BindVertexStreams:
            if (auto* p = blob(c.n * sizeof(VertexStream)))
                s.streams.assign(reinterpret_cast<const VertexStream*>(p), reinterpret_cast<const VertexStream*>(p) + c.n);
            break;
        case Cmd::BindBlendState:
            if (auto* p = blob(sizeof(BlendTarget))) {
                BlendTarget b;
                std::memcpy(&b, p, sizeof b);
                if (b.target < 8) s.blend[b.target] = b;
            }
            break;
        case Cmd::BindColorState:
            if (auto* p = blob(sizeof(ColorStateInfo))) std::memcpy(&s.color, p, sizeof s.color);
            break;
        case Cmd::BindChannelMaskState:
            if (auto* p = blob(sizeof(ChannelMaskInfo))) std::memcpy(&s.mask, p, sizeof s.mask);
            break;
        case Cmd::BindDepthStencilState:
            if (auto* p = blob(sizeof(DepthStencilInfo))) std::memcpy(&s.depth, p, sizeof s.depth);
            break;
        case Cmd::BindPolygonState:
            if (auto* p = blob(sizeof(PolygonInfo))) std::memcpy(&s.polygon, p, sizeof s.polygon);
            break;
        case Cmd::BindMultisampleState: s.alpha_to_coverage = c.n != 0; break;
        case Cmd::SetBlendColor: std::memcpy(s.blend_color, c.f, sizeof s.blend_color); break;
        case Cmd::UpdateUniformBuffer: {  // (buffer address, size, offset, data): written in order, like the GPU
            uint64_t off = c.a[2];
            // NX_TRACE_UUB=<first draw>: logs 40 updates from that draw on.
            static const char* uub = std::getenv("NX_TRACE_UUB");
            static const uint64_t uub_from = uub ? std::strtoull(uub, nullptr, 10) : 0;
            static int uub_n = 0;
            if (uub && g_stats.done >= uub_from && uub_n++ < 1500 && c.blob < list.blobs.size()) {
                const auto& b = list.blobs[c.blob];
                float f[8] = {};
                std::memcpy(f, b.data(), std::min<size_t>(b.size(), sizeof f));
                log("uub before draw %llu: buffer %llx size %llu offset %llu data %zu bytes: %g %g %g %g | %g %g %g %g",
                    (unsigned long long)g_stats.done, (unsigned long long)c.a[0], (unsigned long long)c.a[1],
                    (unsigned long long)off, b.size(), f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]);
            }
            if (c.blob < list.blobs.size())
                if (uint8_t* p = const_cast<uint8_t*>(guest_bytes(c.a[0] + off))) {
                    copy_behind::wait_for(p, list.blobs[c.blob].size());
                    std::memcpy(p, list.blobs[c.blob].data(), list.blobs[c.blob].size());
                    dedup_invalidate(p, list.blobs[c.blob].size());
                    written_cpu_write(p, list.blobs[c.blob].size());
                }
            break;
        }
        case Cmd::ReportCounter: {  // {value, timestamp}: the work before it is done when the replay gets here
            static uint64_t reports = 0, types_seen = 0;
            if (c.n < 64 && !(types_seen >> c.n & 1)) types_seen |= 1ull << c.n, log("nvn: first counter report of type %u", c.n);
            if (uint8_t* p = const_cast<uint8_t*>(guest_bytes(c.a[0]))) {
                // NX_FAKE_GPU_TIME=1: every report of a submit carries the submit's start (a GPU with no cost):
                // the game picks its present interval from the timings it measures (bisecting its 30 fps mode).
                static const bool fake = std::getenv("NX_FAKE_GPU_TIME") != nullptr;
                uint64_t v[2] = {++reports, fake ? submit_start_ns() : plat::monotonic_ns()};
                copy_behind::wait_for(p, sizeof v);
                std::memcpy(p, v, sizeof v);
                dedup_invalidate(p, sizeof v);
                written_cpu_write(p, sizeof v);
                // Bisection: an engine that culls with a counter reads one specific type; a made-up value of the wrong
                // kind makes it cull everything (the scene disappears while the UI keeps drawing).
                static int shown = 0;
                if (shown++ < 10 || shown % 5000 == 0)
                    log("nvn: counter report type %u -> %llu (report #%llu) at %llx", c.n, (unsigned long long)v[0],
                        (unsigned long long)reports, (unsigned long long)c.a[0]);
            }
            break;
        }
        case Cmd::DrawArrays: draw_call(cb, targets, c.n, false, c); break;
        case Cmd::DrawElements:
        case Cmd::DrawElementsIndirect:
        case Cmd::MultiDrawElementsIndirectCount: draw_call(cb, targets, c.n, true, c); break;
        default: raster_command(c); break;
    }
}

}  // namespace nx::nvn
