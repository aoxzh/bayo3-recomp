// The replay of recorded NVN command lists into a Vulkan command buffer (split from nvn_vk.cpp): render targets,
// clears, calls into other lists, copies; draws and state go to nvn_draw.cpp.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <unordered_map>
#include <vector>

#include "nvn_cmd_replay.h"
#include "nvn_copy_behind.h"
#include "nvn_buffer_copy.h"
#include "nvn_zcull.h"
#include "nvn_draw_state.h"
#include "nvn_draw_frame_stats.h"
#include "nvn_vk.h"

namespace nx::nvn {

using namespace nx::vk;

namespace {
// The lists called during one submit, by handle (PHASE4.md C5): command_list() takes a mutex and walks a map, and a
// course frame calls hundreds of lists, each looked up twice (the dispatch count, then the replay). The first lookup
// of a handle in a submit is kept for the rest of it: a list re-recorded meanwhile on the game's recording thread
// is not mixed into a submit already being replayed. NX_CALL_TABLE=0: every call looks up.
CommandListTable g_called;
std::shared_ptr<const CommandListTable> g_captured;
// Command lists may legitimately be replayed more than once after being recorded. Count calls and repeated handles
// per submit so the logs can distinguish list reuse from newly recorded commands; these counts do not establish that
// draws are redundant.
std::unordered_map<uint64_t, uint32_t> g_list_calls;  // this submit: handle -> calls
uint64_t g_calls_total = 0, g_calls_repeat = 0, g_calls_distinct = 0;
uint64_t g_repeat_top[4] = {0, 0, 0, 0};  // the four most-repeated handles, for the log line
uint32_t g_repeat_top_n[4] = {0, 0, 0, 0};
}  // namespace

std::shared_ptr<const CommandListTable> capture_command_lists(const std::vector<uint64_t>& handles) {
    auto captured = std::make_shared<CommandListTable>();
    // Breadth-first discovery reaches shared children at their shallowest depth.
    // A cycle is captured once; replay retains its existing depth limit of eight.
    std::vector<std::pair<uint64_t, int>> todo;
    for (uint64_t handle : handles) todo.emplace_back(handle, 0);
    for (size_t next = 0; next < todo.size(); ++next) {
        auto [handle, depth] = todo[next];
        if (captured->count(handle)) continue;
        auto list = command_list(handle);
        captured->emplace(handle, list);  // Freeze absence too: never resolve it later.
        if (list && depth < 8)
            for (const Cmd& command : list->cmds)
                if (command.op == Cmd::CallList) todo.emplace_back(command.a[0], depth + 1);
    }
    return captured;
}

void submit_lists_reset() { g_called.clear(); g_captured.reset(); reset_command_memory_state(); }
void submit_lists_capture(std::shared_ptr<const CommandListTable> captured) { g_captured = std::move(captured); }

std::shared_ptr<CmdList> submit_command_list(uint64_t handle) {
    if (g_captured) {
        auto found = g_captured->find(handle);
        return found == g_captured->end() ? nullptr : found->second;
    }
    static const bool off = [] {
        const char* e = std::getenv("NX_CALL_TABLE");
        return e && e[0] == '0';
    }();
    if (off) return command_list(handle);
    auto it = g_called.find(handle);
    if (it != g_called.end()) return it->second;
    auto l = command_list(handle);
    if (l) g_called.emplace(handle, l);
    return l;
}

// NX_FENCE_AT_CMD=0 turns this off; it needs NX_EARLY_COMPLETE. A FenceSync passes when the replay reaches it, which is what the hardware
// does when the GPU does. The fight's submits open with theirs, 0.3-4 ms into 22-29 ms of recording, and everything
// after it is the GPU's to read while the guest runs on: the game cannot overwrite that before a later fence, on the
// console or here. Waiting for the end of the recording instead put recording and the guest's frame end to end.
static std::vector<std::function<void()>>* g_fence_hooks = nullptr;
static size_t g_fence_hook_next = 0;
bool fence_at_command() {
    static const bool on = [] {
        const char* e = std::getenv("NX_FENCE_AT_CMD");
        return !(e && e[0] == '0') && early_complete() && pipelined();
    }();
    return on;
}
void replay_fence_hooks(std::vector<std::function<void()>>* hooks) { g_fence_hooks = hooks, g_fence_hook_next = 0; }
static void fence_reached() {
    if (!g_fence_hooks || g_fence_hook_next >= g_fence_hooks->size()) return;
    // What the commands before it read comes off guest memory first. The fight's dispatches queue ~20 MB right before
    // this fence, and waiting here put those copies back on the replay thread: 3 ms of its frame.
    std::function<void()> release = std::move((*g_fence_hooks)[g_fence_hook_next++]);
    if (plat::late_guard_on()) {  // protecting the pages has to wait for the copies that read them
        replay::copy_behind::wait();
        replay::late_guard_arm_written();
        return release();
    }
    replay::copy_behind::after_copies(std::move(release));
}

void replay_list(VkCommandBuffer cb, Targets& st, const CmdList& list, int depth) {
    const bool frame_counts = draw_frame_stats::enabled();
    // NX_REPLAY_PROF: the whole walk, so the buckets below can be subtracted from something real.
    const bool prof_depth0 = depth == 0;
    if (prof_depth0) replay_prof_total_begin();
    if (skin_lists_on()) skin_list_replayed(list, depth);
    for (const Cmd& c : list.cmds) {
        replay_prof_notes_begin();  // NX_REPLAY_PROF: the prologue every command pays, draw or not
        crash_nvn_note(c, list, depth, replay::g_stats.done);  // the last commands, for the crash report (nvn_debug.cpp)
        cb = frame_chunk(cb, replay::g_stats.done, pass_open());  // part of the submit queued early (PLAN.md R1)
        replay_prof_notes_end();
        // NX_TRACE_CMDS=<first draw>: the ops replayed from that draw on (20000), with n, mask and a[0].
        static const char* tc = std::getenv("NX_TRACE_CMDS");
        static const uint64_t tc_from = tc ? std::strtoull(tc, nullptr, 10) : 0;
        static int tc_n = 0;
        if (tc && replay::g_stats.done >= tc_from && tc_n++ < 20000)
            log("cmd after draw %llu: op %u n %u mask %x a0 %llx a1 %llx i %d %d %d %d f %g %g %g %g depth %d",
                (unsigned long long)replay::g_stats.done, unsigned(c.op), c.n, c.mask, (unsigned long long)c.a[0],
                (unsigned long long)c.a[1], c.i[0], c.i[1], c.i[2], c.i[3], c.f[0], c.f[1], c.f[2], c.f[3], depth);
        const bool is_draw = c.op != Cmd::SetRenderTargets && c.op != Cmd::ClearColor && c.op != Cmd::ClearDepthStencil &&
                             c.op != Cmd::ClearBuffer && c.op != Cmd::CopyBufferToBuffer && c.op != Cmd::ZCullBarrier &&
                             c.op != Cmd::FenceSync && c.op != Cmd::SetShaderScratchMemory &&
                             c.op != Cmd::SetColorReductionEnable && c.op != Cmd::SetColorReductionThresholds &&
                             c.op != Cmd::SetTiledCacheAction && c.op != Cmd::CallList &&
                             c.op != Cmd::CopyTextureToTexture && c.op != Cmd::DispatchCompute;
        if (!is_draw) replay_prof_other_begin();  // NX_REPLAY_PROF: the paths that never enter draw()
        switch (c.op) {
            case Cmd::SetRenderTargets: {  // the pass ends only when a draw or clear needs other targets
                static const bool always = std::getenv("NX_TARGETS_BREAK") != nullptr;  // bisecting: the old way
                if (always) draw_end_rendering(cb, 7);
                st.color_count = c.n;
                for (uint32_t k = 0; k < 8; k++) st.colors[k] = k < c.n ? c.a[k] : 0;
                st.depth = c.a[8];
                st.views = {};
                if (c.blob < list.blobs.size() && list.blobs[c.blob].size() == sizeof(TargetViews))
                    std::memcpy(&st.views, list.blobs[c.blob].data(), sizeof st.views);
                break;
            }
            case Cmd::ClearColor:
            case Cmd::ClearDepthStencil: draw_clear(cb, st, c); break;
            case Cmd::ClearBuffer: replay_clear_buffer(cb, c); break;
            case Cmd::CopyBufferToBuffer: replay_buffer_copy(cb, c); break;
            case Cmd::ZCullBarrier: replay_zcull(cb, c); break;
            // QueueSubmitCommands pre-arms these sync generations; its worker
            // publishes only after the containing submit's actual GPU fence.
            case Cmd::FenceSync:
                fence_reached();
                break;
            case Cmd::SetShaderScratchMemory:
            case Cmd::SetColorReductionEnable:
            case Cmd::SetColorReductionThresholds:
            case Cmd::SetTiledCacheAction: replay_command_memory(cb, c); break;
            case Cmd::CallList:
                if (depth < 8) {
                    if (auto l = submit_command_list(c.a[0])) {
                        if (frame_counts) draw_frame_stats::tracker().call(c.a[0], unsigned(depth));
                        {  // count the call, so draw_log_stats can report repeated handles
                            g_calls_total++;
                            uint32_t& n = g_list_calls[c.a[0]];
                            if (n++ == 0) {
                                g_calls_distinct++;
                            } else {
                                g_calls_repeat++;
                                for (int k = 0; k < 4; k++)
                                    if (n > g_repeat_top_n[k] && c.a[0] != g_repeat_top[k]) {
                                        for (int j = 3; j > k; j--)
                                            g_repeat_top[j] = g_repeat_top[j - 1], g_repeat_top_n[j] = g_repeat_top_n[j - 1];
                                        g_repeat_top[k] = c.a[0], g_repeat_top_n[k] = n;
                                        break;
                                    }
                            }
                        }
                        replay_list(cb, st, *l, depth + 1);
                        cb = frame_cb(cb);  // NX_PIPELINED: the called list may have split the submit
                    } else {
                        static uint64_t missing = 0;
                        if (++missing <= 20 || missing % 1000 == 0)
                            log("nvn: called handle %llx is unknown (%llu calls of unknown handles): %s",
                                (unsigned long long)c.a[0], (unsigned long long)missing,
                                explain_handle(c.a[0]).c_str());
                    }
                }
                break;
            case Cmd::CopyTextureToTexture:
                draw_end_rendering(cb, 5);
                if (gpu_profile_on()) gpu_profile_mark(cb, "texture copies");
                replay_copy(cb, c, list);
                if (gpu_profile_on()) gpu_profile_mark(cb, "outside passes (clears, uploads, copies)");
                break;
            default:
                if (frame_counts) {
                    using draw_frame_stats::Kind;
                    switch (c.op) {
                        case Cmd::DrawArrays: draw_frame_stats::tracker().draw(Kind::Arrays, unsigned(depth)); break;
                        case Cmd::DrawElements: draw_frame_stats::tracker().draw(Kind::Elements, unsigned(depth)); break;
                        case Cmd::DrawElementsIndirect: draw_frame_stats::tracker().draw(Kind::Indirect, unsigned(depth)); break;
                        case Cmd::MultiDrawElementsIndirectCount: draw_frame_stats::tracker().draw(Kind::MultiIndirect, unsigned(depth)); break;
                        default: break;
                    }
                }
                draw_command(cb, c, list, st);
                // NX_PIPELINED: after the submit's last dispatch the rest goes into a second command buffer.
                if (c.op == Cmd::DispatchCompute) cb = frame_after_dispatch(cb);
                break;
        }
        if (!is_draw) replay_prof_other_end();
    }
    if (prof_depth0) replay_prof_total_end();
}

void list_calls_log() {
    if (!g_calls_total) return;
    log("nvn calls: %llu CallList replays of %llu distinct handles, %llu repeated in the same submit (%.1f%%); top: "
        "%llx x%u, %llx x%u, %llx x%u, %llx x%u",
        (unsigned long long)g_calls_total, (unsigned long long)g_calls_distinct, (unsigned long long)g_calls_repeat,
        g_calls_total ? 100.0 * double(g_calls_repeat) / double(g_calls_total) : 0.0,
        (unsigned long long)g_repeat_top[0], g_repeat_top_n[0], (unsigned long long)g_repeat_top[1], g_repeat_top_n[1],
        (unsigned long long)g_repeat_top[2], g_repeat_top_n[2], (unsigned long long)g_repeat_top[3], g_repeat_top_n[3]);
    g_list_calls.clear();
    g_calls_total = g_calls_repeat = g_calls_distinct = 0;
    for (int k = 0; k < 4; k++) g_repeat_top[k] = 0, g_repeat_top_n[k] = 0;
}

}  // namespace nx::nvn
