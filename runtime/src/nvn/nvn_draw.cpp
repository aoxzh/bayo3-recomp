// Draws (docs/NVN.md, N6): replays the bind, state and draw commands of nvn_state.cpp into Vulkan. Everything a draw
// reads from guest memory (vertices, indices, uniform and storage buffers, textures) is copied at replay time into
// the submit's upload buffer: the replay runs right when the game submits and is waited for, so the memory holds
// what the GPU would have read. Render targets become a dynamic-rendering pass, begun at the first draw and ended
// before any transfer.
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <cstring>
#include <set>
#include <utility>
#include <vector>

#include "nvn_draw_state.h"
#include "nvn_draw_frame_stats.h"
#include "nvn_cmd_replay.h"
#include "nvn_indirect.h"
#include "nvn_ultrawide.h"
#include "platform.h"

namespace nx::nvn {

using namespace nx::vk;

// The replay state shared with nvn_draw_trace.cpp (nvn_draw_state.h).
namespace replay {

State g_state;
Stats g_stats;

const uint8_t* guest_bytes(uint64_t gpu) {
    uint64_t off = 0;
    MemoryPool* p = pool_at_gpu(gpu, &off);
    uint64_t host = p ? pool_host(*p, off) : 0;
    return host ? reinterpret_cast<const uint8_t*>(host) : nullptr;
}

// NX_TRACE_PRESENT: trace_draw sets this for the drawing being traced; draw() below then dumps its colour target and
// the images it sampled after the submit (debug_dump_add). Declared in nvn_draw_state.h.
bool g_trace_present_hit = false;

}  // namespace replay

using namespace replay;

namespace {

bool g_rendering = false;
bool g_render_pass = false;
// Like g_state, these buffers belong to the single replay thread. Definitions are selected anew
// for every draw/dispatch; only vector storage survives, so program mutation cannot leave stale stages.
std::vector<const ShaderInfo*> g_draw_stages, g_dispatch_stages;
bool g_vertex_pending = false;  // a narrow pass-end barrier since the last one that covers vertex stages (G4)
uint64_t g_pass_serial = 0, g_vertex_visible_at = 0;  // pass ends so far; the serial a full barrier covered
uint64_t g_vertex_barriers = 0;

// NX_LAZY_COMPUTE_BARRIER=1 (P3): a dispatch used to end with COMPUTE_SHADER -> ALL_COMMANDS, which on MoltenVK ends
// the Metal compute encoder and any open render pass (dispatch() ends the pass itself): ~900 per menu frame and
// ~4,500 per gameplay frame (logs/ch1/fps-analysis.md 2.5). Here the barrier is emitted once for a run of dispatches
// that do not meet each other, before the first command that can observe their writes: a render pass beginning (any
// draw, clear or pass that samples or loads them -- compute-write synchronisation on the compute stage for them ends
// there), a copy or merge of a written range (nvn_written.cpp calls compute_write_barrier), a WAR/WAW flush there, or
// the submit's own end barriers (submit_part's ALL_COMMANDS -> HOST before the fence, begin_part's full barrier for
// the next part). NX_LAZY_COMPUTE_BARRIER unset: exactly the old per-dispatch barrier.
bool lazy_compute_barrier() {
    static const bool on = std::getenv("NX_LAZY_COMPUTE_BARRIER") != nullptr;
    return on;
}
bool g_compute_writes = false;  // a dispatch wrote since the last barrier that covers it
uint64_t g_compute_barriers = 0, g_compute_ends = 0;  // barriers emitted, dispatch ends folded behind one

// NX_DRAW_HIST=1: how many draws each (vertex, fragment) shader pair got in one stats window. A fullscreen pass
// issued once per item, or a scene shader replayed more often than the frame explains, is then a number instead of
// a guess: a 1600x900 frame should not submit the same fullscreen pair hundreds of times.
std::map<std::pair<std::string, std::string>, uint64_t> g_draw_hist;

bool draw_hist_on() {
    static const bool on = std::getenv("NX_DRAW_HIST") != nullptr;
    return on;
}

void draw_hist_note(const ShaderInfo* vs, const ShaderInfo* fs) {
    if (!draw_hist_on()) return;
    // Check before copying hashes: the former conditional string arguments made
    // two temporaries on every draw, including when histogram logging was off.
    static const std::string empty;
    ++g_draw_hist[{vs ? vs->hash : empty, fs ? fs->hash : empty}];
}

void draw_hist_log() {
    if (!draw_hist_on() || g_draw_hist.empty()) return;
    std::vector<std::pair<uint64_t, std::pair<std::string, std::string>>> v;  // count, (vs, fs)
    v.reserve(g_draw_hist.size());
    for (const auto& [key, n] : g_draw_hist) v.push_back({n, key});
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    char line[800] = {};
    size_t at = 0;
    for (size_t k = 0; k < v.size() && k < 8 && at < sizeof line - 48; ++k)
        at += size_t(std::snprintf(line + at, sizeof line - at, "%svs %s fs %s x%llu", k ? ", " : "",
                                   v[k].second.first.c_str(), v[k].second.second.c_str(),
                                   (unsigned long long)v[k].first));
    log("nvn draw hist: %zu shader pairs, top: %s", g_draw_hist.size(), line);
    g_draw_hist.clear();
}

// Off by default (NX_PASS_BARRIER_NARROW=1 turns it on): narrowing this barrier can cause
// timing-dependent rendering artifacts.
bool narrow_pass_barrier() {
    static const bool on = std::getenv("NX_PASS_BARRIER_NARROW") != nullptr;
    return on;
}

// NX_REPLAY_PROF=1 buckets CPU time by draw phase and surrounding paths. Call counts help separate
// phase cost from the overhead of the clock reads used by the profiler.
namespace {
// kPNotes and kPOther cover work outside the per-draw phases: crash_nvn_note() and frame_chunk()
// run for every command, while clears, copies, dispatches and CallList bookkeeping are not draws.
enum Phase { kPProgram, kPSets, kPInputs, kPPipeline, kPState, kPEmit, kPGather, kPBuild, kPBegin, kPKey, kPNotes,
             kPOther, kPCmd, kPDraw, kPTrans, kPView, kPDrv, kPTotal, kPCount };
static_assert(kPCount == replay_prof::kPhases, "nvn_draw_state.h's replay_prof::kPhases must match this enum");
const char* const kPhaseName[kPCount] = {"program", "sets",  "inputs", "pipeline", "state", "emit",  "gather",
                                         "build",   "begin", "key",    "notes",    "other", "cmd",   "draw",
                                         "trans",   "view",  "driver", "loop"};
// The counters live in nvn_draw_state.h (replay_prof::phase_ns/...) because nvn_replay.cpp and nvn_draw_state.cpp
// accumulate into them from other translation units, and the single-source unit tests link those without this file.
uint64_t* const g_phase_ns = replay_prof::phase_ns();
uint64_t* const g_phase_calls = replay_prof::phase_calls();
uint64_t* const g_op_ns = replay_prof::op_ns();
uint64_t* const g_op_calls = replay_prof::op_calls();
bool replay_prof() { return replay_prof::on(); }
struct PhaseScope {
    explicit PhaseScope(Phase p) : phase(p) {
        if (!prof_on) return;
        t0 = plat::monotonic_ns(), on = true;
    }
    ~PhaseScope() { close(); }
    void close() {
        if (!on) return;
        on = false;
        g_phase_ns[phase] += plat::monotonic_ns() - t0, g_phase_calls[phase]++;
    }
    PhaseScope(const PhaseScope&) = delete;
    PhaseScope& operator=(const PhaseScope&) = delete;
    Phase phase;
    uint64_t t0 = 0;
    const bool prof_on = replay_prof();
    bool on = false;
};
}  // namespace

void replay_prof_log(uint64_t frames) {
    if (!replay_prof() || !frames) return;
    uint64_t total = 0;
    for (int k = 0; k < kPCount; k++) total += g_phase_ns[k];
    if (!total) return;
    for (int k = 0; k < kPCount; k++)
        if (g_phase_calls[k])
            log("nvn/replay-prof: %-8s %7.2f ms/frame (%5.1f %%) %8llu calls/frame %6.0f ns/call", kPhaseName[k],
                double(g_phase_ns[k]) / 1e6 / double(frames), 100.0 * double(g_phase_ns[k]) / double(total),
                (unsigned long long)(g_phase_calls[k] / frames), double(g_phase_ns[k]) / double(g_phase_calls[k]));
    log("nvn/replay-prof: bucketed total %7.2f ms/frame over %llu frames", double(total) / 1e6 / double(frames),
        (unsigned long long)frames);
    // The state commands, worst first: name and count so the line can be read without the source open.
    for (int k = 0; k < 64; k++) {
        if (!g_op_calls[k]) continue;
        log("nvn/replay-prof:   op %-2d %6.2f ms/frame %8llu calls/frame %6.0f ns/call", k,
            double(g_op_ns[k]) / 1e6 / double(frames), (unsigned long long)(g_op_calls[k] / frames),
            double(g_op_ns[k]) / double(g_op_calls[k]));
    }
    for (int k = 0; k < 64; k++) g_op_ns[k] = 0, g_op_calls[k] = 0;
    for (int k = 0; k < kPCount; k++) g_phase_ns[k] = 0, g_phase_calls[k] = 0;
}


// A vertex shader sampling a rendered image after a narrow pass-end barrier: the pass ends (it cannot take a barrier
// inside) and the writes so far are made visible to the vertex stages.
void vertex_read_barrier(VkCommandBuffer cb, const ShaderInfo* vs);  // the open pass is a render pass object (nvn_renderpass.cpp), not dynamic rendering
Targets g_pass;           // what the open pass renders to
GpuImage* g_pass_images[9];  // their images (8: depth), resolved once per pass for gather_textures
uint32_t g_pass_written = 0;  // targets the open pass's draws wrote: bit c colour c, bit 8 depth
uint64_t g_pass_ends[8];  // passes ended, by draw_end_rendering reason
uint64_t g_submit_ns = 0;  // when the submit being replayed started
VkExtent2D g_pass_extent{};

// What this submit's command buffer already has bound and set. Vulkan keeps the pipeline, vertex and index buffers
// and dynamic state of a command buffer across its dynamic-rendering passes, and most draws repeated what the draw
// before them set (the vertex streams and the viewport of a whole pass): only what differs is recorded. Forgotten
// at each submit, on another command buffer, and when something else binds a pipeline there (the masked clear's
// fullscreen draw, nvn_mask_clear.cpp). NX_DRAW_SLOW: forgotten before every draw.
struct Bound {
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    uint32_t vertex_set = 0;  // bit s: binding s holds vertex[s]
    Upload vertex[16];
    bool has_index = false, has_dynamic = false;
    Upload index;
    VkIndexType index_type = VK_INDEX_TYPE_UINT16;
    VkViewport viewport{};
    VkRect2D scissor{};
    float blend[4] = {};
    uint32_t raster[13] = {};  // stencil/masks, depth bias, depth-bounds enable and interval
} g_bound;
uint64_t g_state_set = 0, g_state_kept = 0;  // state commands recorded and left out, for the log

template <typename T>
bool same_bytes(const T& a, const T& b) { return std::memcmp(&a, &b, sizeof(T)) == 0; }

void bind_vertex(VkCommandBuffer cb, uint32_t binding, const Upload& u) {
    Upload& now = g_bound.vertex[binding];
    if ((g_bound.vertex_set >> binding & 1) && now.buffer == u.buffer && now.offset == u.offset) {
        g_state_kept++;
        return;
    }
    VkDeviceSize off = u.offset;
    p_vkCmdBindVertexBuffers(cb, binding, 1, &u.buffer, &off);
    now = u, g_bound.vertex_set |= 1u << binding, g_state_set++;
}

// Viewport, scissor, blend constants and the rasterizer's dynamic state (every pipeline declares them dynamic).
void set_dynamic(VkCommandBuffer cb, const VkViewport& vp, const VkRect2D& sc) {
    Bound& b = g_bound;
    const State& s = g_state;
    uint32_t raster[13] = {s.stencil_ref[0],        s.stencil_ref[1],        s.stencil_compare_mask[0],
                          s.stencil_compare_mask[1], s.stencil_write_mask[0], s.stencil_write_mask[1]};
    std::memcpy(raster + 6, &s.bias_factor, 4), std::memcpy(raster + 7, &s.bias_units, 4);
    std::memcpy(raster + 8, &s.bias_clamp, 4);
    bool slack = s.depth.depth_test && equal_slack_pass(s.depth);
    raster[9] = slack;
    raster[10] = s.depth_bounds;
    std::memcpy(raster + 11, s.depth_bounds_range, sizeof s.depth_bounds_range);
    bool all = !b.has_dynamic;
    b.has_dynamic = true;
    auto differs = [&](bool d) { return (all || d) ? (g_state_set++, true) : (g_state_kept++, false); };
    if (differs(!same_bytes(b.viewport, vp))) p_vkCmdSetViewport(cb, 0, 1, &vp), b.viewport = vp;
    if (differs(!same_bytes(b.scissor, sc))) p_vkCmdSetScissor(cb, 0, 1, &sc), b.scissor = sc;
    if (differs(std::memcmp(b.blend, s.blend_color, sizeof b.blend) != 0))
        p_vkCmdSetBlendConstants(cb, s.blend_color), std::memcpy(b.blend, s.blend_color, sizeof b.blend);
    if (differs(std::memcmp(b.raster, raster, sizeof raster) != 0))  // stencil reference and masks, depth bias
        raster_dynamic(cb, slack), std::memcpy(b.raster, raster, sizeof raster);  // (nvn_raster.cpp)
}

// The render area of a pass that has no colour and no depth attachment (see begin_rendering).
VkExtent2D no_attachment_extent() {
    auto sane = [](int64_t w, int64_t h) -> VkExtent2D {
        if (w <= 0 || h <= 0 || w > 16384 || h > 16384) return {0, 0};
        return {uint32_t(w), uint32_t(h)};
    };
    if (g_state.has_viewport) {
        VkExtent2D e = sane(g_state.viewport[2], g_state.viewport[3]);
        if (e.width) return e;
    }
    if (g_state.has_scissor) {
        VkExtent2D e = sane(g_state.scissor[2], g_state.scissor[3]);
        if (e.width) return e;
    }
    if (uint64_t presented = presented_texture())
        if (GpuImage* img = image_of(presented); img && img->width && img->height) return {img->width, img->height};
    return {1, 1};
}

void begin_rendering(VkCommandBuffer cb, const Targets& t) {
    compute_write_barrier(cb);  // NX_LAZY_COMPUTE_BARRIER: the dispatches before this pass are visible to it
    if (clears_pending_other(t)) clears_flush(cb);  // other targets' clears first, in order
    // An image never written (layout UNDEFINED) has nothing to load: loadOp CLEAR to zero, not a LOAD of whatever
    // the memory held (G3, docs/nvn/DEBUG.md #50); a pending clear still wins (clears_apply). Env: bisecting.
    static const bool load_undefined = std::getenv("NX_UNDEFINED_LOAD") != nullptr;
    uint32_t fresh = 0;  // bit c colour c, bit 8 depth: begun cleared to 0 (depth 0, stencil 0)
    VkRenderingAttachmentInfo colors[8]{};
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkExtent2D extent{UINT32_MAX, UINT32_MAX};
    VkFormat formats[8]{};  // the views' (render pass objects)
    for (uint32_t k = 0; k < t.color_count; k++) {
        colors[k].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        GpuImage* img = t.colors[k] ? image_of(t.colors[k]) : nullptr;
        g_pass_images[k] = img;
        if (!img) continue;
        if (img->layout == VK_IMAGE_LAYOUT_UNDEFINED && !load_undefined) fresh |= 1u << k;
        { PhaseScope p(kPTrans); transition(cb, *img, VK_IMAGE_LAYOUT_GENERAL); }  // rendered images stay GENERAL
        img->rendered = true;
        uint32_t level = std::min<uint32_t>(t.views.level[k], img->levels - 1);
        formats[k] = view_format(*img, t.views.format[k]);
        { PhaseScope p(kPView); colors[k].imageView = attachment_view(*img, level, t.views.layer[k], formats[k]); }
        // NX_TRACE_CUBES=<first frame>: writes into cube images from that frame on. With 400 from the start, the log
        // ran out at the first 3D scene (frame ~171) and never reached a course load.
        static const char* trace_cubes = std::getenv("NX_TRACE_CUBES");
        static const uint64_t cube_from = trace_cubes ? std::strtoull(trace_cubes, nullptr, 10) : 0;
        static int cube_logs = 0;
        if (trace_cubes && (img->view_type == VK_IMAGE_VIEW_TYPE_CUBE || img->view_type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) &&
            g_frame >= cube_from && cube_logs++ < 2000)
            log("cubes: render into %llx level %u layer %u (%ux%u, %u levels, %u layers) view %s",
                (unsigned long long)t.colors[k], level, t.views.layer[k], img->width, img->height, img->levels,
                img->layers, colors[k].imageView == img->view ? "WHOLE IMAGE (fallback)" : "ok");
        colors[k].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        colors[k].loadOp = fresh >> k & 1 ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        colors[k].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        extent = {std::min(extent.width, std::max(1u, img->width >> level)),
                  std::min(extent.height, std::max(1u, img->height >> level))};
    }
    GpuImage* dimg = t.depth ? image_of(t.depth) : nullptr;
    g_pass_images[8] = dimg;
    if (dimg && dimg->aspect != VK_IMAGE_ASPECT_COLOR_BIT) {
        if (dimg->layout == VK_IMAGE_LAYOUT_UNDEFINED && !load_undefined) fresh |= 1u << 8;
        { PhaseScope p(kPTrans); transition(cb, *dimg, VK_IMAGE_LAYOUT_GENERAL); }
        dimg->rendered = true;
        uint32_t level = std::min<uint32_t>(t.views.level[8], dimg->levels - 1);
        { PhaseScope p(kPView); depth.imageView = attachment_view(*dimg, level, t.views.layer[8]); }
        depth.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        depth.loadOp = fresh >> 8 & 1 ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        depth.clearValue.depthStencil = {1.0f, 0};  // far (a fresh depth buffer; the course passes test LESS)
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        extent = {std::min(extent.width, std::max(1u, dimg->width >> level)),
                  std::min(extent.height, std::max(1u, dimg->height >> level))};
    } else {
        dimg = nullptr;
    }
    // A pass with no colour and no depth attachment still runs on a GPU, and Bayonetta 3 computes its EXPOSURE in
    // exactly such passes: fragment draws that write a storage buffer and nothing else (two other Bayonetta 3 ports
    // document the same pass: theirs stayed zero and the scene could not composite). Sized 1x1 those fragments never
    // ran here either, so the buffer kept its old bytes and a later pass divided by a zero parameter (inf, then NaN:
    // docs/HDR-SCENE-2026-10-23.md §6). The area is the draw's own viewport (what it would rasterize into if it had an
    // attachment), else its scissor, else the window.
    if (extent.width == UINT32_MAX) extent = no_attachment_extent();
    VkRenderingAttachmentInfo stencil = depth;  // same view, its own loadOp
    uint32_t cleared = clears_apply(t, colors, depth, stencil);  // pending clears of these targets: loadOp CLEAR
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, extent};
    ri.layerCount = 1;
    ri.colorAttachmentCount = t.color_count;
    ri.pColorAttachments = colors;
    if (dimg) {
        if (dimg->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ri.pDepthAttachment = &depth;
        if (dimg->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) ri.pStencilAttachment = &stencil;
    }
    g_render_pass = render_pass_objects();
    { PhaseScope p(kPDrv);
      if (g_render_pass)
        begin_render_pass(cb, t.color_count, formats, colors, dimg ? dimg->format : VK_FORMAT_UNDEFINED, depth, stencil,
                          extent);
      else
        p_vkCmdBeginRendering(cb, &ri); }
    g_rendering = true;
    g_pass_written = cleared | fresh;  // as a clear inside the pass: sampling it needs it stored first
    // Bisection: a pass with no colour target (or a 1x1 area) discards every draw that goes into it. The game
    // setting targets to 0 and then submitting a few hundred draws is either a real headless pass or our miss.
    if (!t.colors[0] || extent.width == 1) {
        static int empty = 0;
        if (empty++ < 12 || empty % 2000 == 0) {
            std::string cols;  // the other colour slots, so a real MRT pass is told from a failed resolution
            for (uint32_t k = 1; k < t.color_count; k++) {
                GpuImage* ci = g_pass_images[k];
                char b[64];
                std::snprintf(b, sizeof b, " %u:%llx(%ux%u f%x)", k, (unsigned long long)t.colors[k], ci ? ci->width : 0,
                              ci ? ci->height : 0, ci ? ci->nvn_format : 0);
                cols += b;
            }
            log("nvn: pass with no colour target 0: %u colours, depth %llx (%ux%u f%x), extent %ux%u%s "
                "(pass %d so far)",
                t.color_count, (unsigned long long)t.depth, g_pass_images[8] ? g_pass_images[8]->width : 0,
                g_pass_images[8] ? g_pass_images[8]->height : 0,
                g_pass_images[8] ? g_pass_images[8]->nvn_format : 0, extent.width, extent.height, cols.c_str(), empty);
        }
    }
    trace_pass("begin: cleared, fresh", t, cleared, fresh);
    g_pass = t;
    g_pass_extent = extent;
    // NX_TRACE_PRESENT: is a texture the game presents drawn into by a pass? (nvn_draw_trace.cpp logs the first time.)
    if (t.color_count)
        for (uint32_t k = 0; k < t.color_count; k++) present_touch("pass colour target", t.colors[k]);
}

uint64_t g_dump_draws = 0, g_dump_request = 0;

// (debug_dump_request_presented is defined below the anonymous namespace: nvn_copy.cpp calls it across TUs.)

// DispatchCompute: the bound program's compute stage over (x, y, z) groups, outside render passes, then a barrier
// so what it wrote is visible to what follows.
void dispatch(VkCommandBuffer cb, const Cmd& c) {
    static const bool off = std::getenv("NX_NO_COMPUTE") != nullptr;  // debug: skip compute dispatches
    if (off) return;
    if (!program_require_complete(g_state.programs[5], 0x20)) return;  // no usable compute translation
    auto& selected = g_dispatch_stages;
    selected_program_shaders(g_state, 0x20, selected);
    const auto* shaders = &selected;
    const ShaderInfo* cs = nullptr;
    if (shaders)
        for (const ShaderInfo* s : *shaders)
            if (s->stage == ShaderStage::Compute) cs = s;
    uint32_t x = uint32_t(c.i[0]), y = uint32_t(c.i[1]), z = uint32_t(c.i[2]);
    if (!cs) {
        static int nocs = 0;
        if (nocs++ < 8 || nocs % 1000 == 0)
            log("nvn: dispatch with no compute program bound (dims %u,%u,%u)", x, y, z);
        return;
    }
    if (c.n != 1 && (!x || !y || !z)) {
        // The game dispatches (1,1,0): NVN drivers run a group where Vulkan would reject the 0. Clamp, but say so.
        static int clamped = 0;
        if (clamped++ < 8 || clamped % 1000 == 0)
            log("nvn: dispatch dims %u,%u,%u clamped to 1 (the game passes 0 where NVN still dispatches)", x, y, z);
        if (!x) x = 1;
        if (!y) y = 1;
        if (!z) z = 1;
    }
    const uint8_t* args = c.n == 1 ? guest_bytes(c.a[0]) : nullptr;  // indirect: {x, y, z}
    if (c.n == 1 && !args) return;
    // Written by an earlier dispatch of this submit: only the GPU knows them (vkCmdDispatchIndirect, below).
    bool gpu_args = args && !written_separate() && written_overlaps(args, 12);
    if (args && !gpu_args) std::memcpy(&x, args, 4), std::memcpy(&y, args + 4, 4), std::memcpy(&z, args + 8, 4);
    if (!gpu_args && (!x || !y || !z)) {
        // Bisection: are dispatches dropped on zero host-visible dims, and was a GPU write missed there?
        static int dropped = 0;
        if (dropped++ < 12 || dropped % 1000 == 0)
            log("nvn: dispatch skipped dims %u,%u,%u cs %s args@%llx gpu-write tracked %u", x, y, z, cs->hash.c_str(),
                (unsigned long long)c.a[0], args ? uint32_t(written_overlaps(args, 12)) : 2u);
        return;
    }
    trace_compute_once(cs, x, y, z);  // 0, 0, 0 for GPU arguments
    trace_nan_dispatch(cs);
    // Preserve the existing last-compute-stage selection without a second temporary allocation.
    selected.assign(1, cs);
    const auto& stages = selected;
    const ProgramLayout& layout = program_layout(stages);
    VkPipeline pipeline = compute_pipeline(cs, layout);  // kPKey (NX_REPLAY_PROF): a cache probe, once warmed
    if (!pipeline) return;
    { PhaseScope p(kPBegin); draw_end_rendering(cb, 2); }  // the open pass is flushed
    { PhaseScope p(kPGather); gather_textures(stages); }
    { PhaseScope p(kPBuild); build_sets(cb, stages, layout); }
    { PhaseScope p(kPState); p_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline); }
    { PhaseScope p(kPSets); bind_sets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout); }
    // NX_LAZY_COMPUTE_BARRIER: a storage image an earlier dispatch wrote is not tracked like a written buffer range,
    // so its write is made visible before a dispatch that binds one (an image write and a later read/sample of it).
    if (lazy_compute_barrier() && g_compute_writes) {
        bool storage_image = false;
        for (size_t k = 0; k < g_textures.resolved.size() && !storage_image; k++)
            if (g_textures.resolved[k].texture)
                if (GpuImage* img = image_of(g_textures.resolved[k].texture)) storage_image = img->storage;
        if (storage_image) compute_write_barrier(cb);
    }
    if (gpu_profile_on()) gpu_profile_mark(cb, "compute " + cs->hash);
    {
      PhaseScope pe(kPEmit);  // NX_REPLAY_PROF: the barriers and vkCmdDispatch, where a tiled GPU may flush
    copy_write_barrier(cb);  // NX_REPLAY_CUTS=barrier: make the descriptor copies above visible to this dispatch
    if (!gpu_args || !written_indirect(cb, args)) p_vkCmdDispatch(cb, x, y, z);
    if (gpu_profile_on()) gpu_profile_mark(cb, "outside passes (clears, uploads, copies)");
    if (lazy_compute_barrier()) {
        g_compute_writes = true, g_compute_ends++;  // one barrier for the run, before what can observe it
    } else {
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb,
                               0, nullptr, 0, nullptr);
    }
    }
    g_stats.dispatches++;
}

void vertex_read_barrier(VkCommandBuffer cb, const ShaderInfo* vs) {
    if (!g_vertex_pending || !vs) return;
    bool reads = false;
    for (size_t k = 0; k < g_textures.bindings.size() && !reads; k++)
        if (g_textures.bindings[k].first == vs && k < g_textures.resolved.size() && g_textures.resolved[k].texture)
            if (GpuImage* img = image_of(g_textures.resolved[k].texture))
                reads = img->rendered && img->rendered_at > g_vertex_visible_at;
    if (!reads) return;
    draw_end_rendering(cb, 1);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                       VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, 0, 1, &mb, 0,
                           nullptr, 0, nullptr);
    g_vertex_pending = false, g_vertex_barriers++, g_vertex_visible_at = g_pass_serial;
}

bool draw(VkCommandBuffer cb, const Targets& t, uint32_t mode, bool indexed, const Cmd& c) {
    // NX_REPLAY_PROF: the whole body, so the six phases can be subtracted from it.
    replay_prof_draw_begin();
    struct DrawScope {
        ~DrawScope() { replay_prof_draw_end(); }
    } draw_scope;
    g_stats.total++;
    if (g_state.rasterizer_discard || !g_state.render_enable) return false;  // nothing reaches the targets
    auto& selected = g_draw_stages;
    {
        PhaseScope p(kPProgram);  // program completeness and the selected stages (NX_REPLAY_PROF)
        selected.clear();
        bool complete = true;
        for (uint32_t stage = 0; stage < 5; ++stage)
            if (!program_require_complete(g_state.programs[stage], 1u << stage)) {
                complete = false;
                break;
            }
        if (complete) selected_program_shaders(g_state, 0x1f, selected);
    }
    const auto* shaders = &selected;
    if (shaders->empty()) {
        g_stats.no_program++;
        return false;
    }
    const ShaderInfo* vs = nullptr;
    const ShaderInfo* fs = nullptr;
    const ShaderInfo* tcs = nullptr;
    const ShaderInfo* tes = nullptr;
    for (const ShaderInfo* s : *shaders) {
        if (s->stage == ShaderStage::Vertex) vs = s;
        if (s->stage == ShaderStage::Fragment) fs = s;
        if (s->stage == ShaderStage::TessellationControl) tcs = s;
        if (s->stage == ShaderStage::TessellationEvaluation) tes = s;
    }
    if (!vs) {
        g_stats.no_vertex++;
        return false;
    }
    std::vector<const ShaderInfo*> swapped;  // NX_SWAP_FS / NX_SKIP_FS (nvn_draw_trace.cpp)
    if (!debug_shader_overrides(shaders, fs, swapped)) return false;
    const ProgramLayout& layout = program_layout(*shaders);

    // NX_SNAP_FS_BEFORE=<hash>[@<first draw>]: snapshot colour target 0 before draws with that fragment shader
    // (what a feedback pass like a tonemap reads), 8 at most.
    {
        static const std::string before = std::getenv("NX_SNAP_FS_BEFORE") ? std::getenv("NX_SNAP_FS_BEFORE") : "";
        static const uint64_t from = before.find('@') != std::string::npos
                                         ? std::strtoull(before.c_str() + before.find('@') + 1, nullptr, 10) : 0;
        static int taken = 0;
        bool match = (fs && before.compare(0, 16, fs->hash) == 0) || before.compare(0, 16, vs->hash) == 0;
        if (!before.empty() && match && g_stats.total >= from && taken < 8 &&
            t.color_count && t.colors[0])
            if (GpuImage* img = image_of(t.colors[0])) {
                taken++;
                draw_end_rendering(cb, 6);
                debug_snapshot(cb, *img, 0xB0000000ull + g_stats.total);
            }
    }
    // Textures first: an upload or a layout change cannot happen inside the render pass.
    uint32_t gathered;
    { PhaseScope p(kPGather); gathered = gather_textures(*shaders); }
    {   // NX_DUMP_DRAW_TEXTURES=<fs hash>@<draw>: the images the first such draw samples, snapshotted (frames/00000_c00000NN)
        static const char* env = std::getenv("NX_DUMP_DRAW_TEXTURES");
        static int done_count = 0;
        static bool said = false;
        if (env && !said && fs) said = true, log("nvn: draw-textures watch %s, first fs %s total %llu", env, fs->hash.c_str(), (unsigned long long)g_stats.total);
        if (env && done_count < 6 && fs && std::strncmp(env, fs->hash.c_str(), 16) == 0 && env[16] == '@' &&
            g_stats.total >= std::strtoull(env + 17, nullptr, 10)) {
            const uint64_t tag = uint64_t(done_count++) << 8;
            draw_end_rendering(cb, 6);
            for (size_t k = 0; k < g_textures.resolved.size(); k++) {
                const ResolvedTexture& r = g_textures.resolved[k];
                GpuImage* img = r.texture ? image_of(r.texture) : nullptr;
                log("draw textures %zu: texture %llx image %p %ux%u fmt %d levels %u layers %u uploaded %d rendered %d", k,
                    (unsigned long long)r.texture, (void*)img, img ? img->width : 0, img ? img->height : 0,
                    img ? int(img->format) : 0, img ? img->levels : 0, img ? img->layers : 0, img ? int(img->uploaded) : 0,
                    img ? int(img->rendered) : 0);
                if (img)
                    for (uint32_t l = 0; l < (img->layers > 1 ? std::min(img->layers, 12u) : 1u); l++) {
                        g_snap_layer = l, g_snap_level = 0;
                        debug_snapshot(cb, *img, 0xC0000000ull + tag * 4096 + k * 16 + l);
                    }
                g_snap_layer = 0;
            }
        }
    }
    // A pass that neither tests nor writes depth but keeps its depth image bound while sampling that same image (the
    // world map's depth-to-colour linearization, fs 0aa56d1b): on Metal the image is then attached and read in one
    // encoder, which is undefined, and the copy came out 0 everywhere; the composite then took the player for
    // occluded (a flat silhouette, issue #50; DEBUG-2.md #67). The depth is unused: the pass runs without it.
    // NX_KEEP_SAMPLED_DEPTH=1: as before.
    Targets detached;
    const Targets& tt = [&]() -> const Targets& {
        static const bool keep = std::getenv("NX_KEEP_SAMPLED_DEPTH") != nullptr;
        const DepthStencilInfo& d = g_state.depth;
        if (keep || !t.depth || d.depth_test || d.depth_write || d.stencil_test) return t;
        GpuImage* dimg = image_of(t.depth);
        if (!dimg) return t;
        for (const ResolvedTexture& r : g_textures.resolved)
            if (r.texture && image_of(r.texture) == dimg) {
                static uint64_t n = 0;
                if (n++ == 0) log("nvn: a pass samples its own unused depth image: drawn without it (#50)");
                detached = t, detached.depth = 0;
                return detached;
            }
        return t;
    }();
    vertex_read_barrier(cb, vs);  // G4
    g_uw_wide = false;  // NX_ULTRAWIDE: is this draw's target a widened one (viewport, scissor, clears, vs scale)
    if (uw_on()) {
        GpuImage* wi = t.color_count && t.colors[0] ? image_of(t.colors[0]) : (t.depth ? image_of(t.depth) : nullptr);
        g_uw_wide = wi && wi->uw;
    }
    bool need_break = gathered & kNeedBreak;
    if (gathered & kSampledPending) clears_flush(cb);  // cleared (and stored) before it is sampled
    static const bool bytes = std::getenv("NX_TARGETS_MEMCMP") != nullptr;  // bisecting
    bool targets_changed = !g_rendering || (bytes ? std::memcmp(&g_pass, &tt, sizeof tt) != 0 : !targets_same(g_pass, tt));
    if (g_rendering && (need_break || targets_changed)) {
        (targets_changed ? g_stats.breaks_targets : g_stats.breaks_textures)++;
        draw_end_rendering(cb, 1);
    }

    { PhaseScope p(kPBuild); build_sets(cb, *shaders, layout); }  // resolve_sets for every stage's sets
    // Vertex and index data before the pass begins: a range a dispatch of this submit wrote may need a GPU copy
    // (nvn_written.cpp), which cannot be recorded inside a pass.
    DrawInputs in;
    IndexedIndirect indirect;
    const bool is_indirect = is_indexed_indirect(c);
    {
        PhaseScope p(kPInputs);  // vertex/index ranges, the guest->ring copies of vertex data, indirect arguments
        if (!(is_indirect ? prepare_indexed_indirect(cb, vs, c, in, indirect)
                          : draw_inputs_shared(cb, vs, indexed, c, in))) {
            g_stats.no_indices++;
            return false;
        }
    }
    if (!is_indirect) check_vertex_ranges(vs, fs, indexed, c, in);  // indirect counts remain GPU data
    if (!is_indirect) input_bytes_note(vs, indexed, c, in);          // NX_INPUT_BYTES: bound vs fetched bytes
    // NX_REPLAY_CUTS=barrier: the copies this draw asked for while its sets and inputs were built (the composed reads of
    // build_sets and draw_inputs, nvn_written.cpp) become visible here, outside the pass, in one barrier instead of one
    // per copy. A composed read ends the pass itself (written_read's draw_end_rendering), so this is the usual place;
    // the call before the draw command below covers a marker left by a merge instead.
    copy_write_barrier(cb);
    if (!g_rendering) {
        if (gpu_profile_on()) {
            GpuImage* img = t.color_count && t.colors[0] ? image_of(t.colors[0]) : nullptr;
            char label[128];
            std::snprintf(label, sizeof label, "%ux%u L%u fs %s", img ? img->width : 0, img ? img->height : 0,
                          t.views.level[0], fs ? fs->hash.c_str() : "-");
            gpu_profile_mark(cb, label);
        }
        { PhaseScope p(kPBegin); begin_rendering(cb, tt); }  // a pass start per target change / pass break
    }

    // Pipeline.
    PhaseScope key_phase(kPKey);  // the key is ~1 KB of state: memset, the target formats, the shader pointers
    PipelineKey key;
    std::memset(&key, 0, sizeof key);  // the key is compared byte for byte
    key.vs = vs, key.fs = fs;
    key.tcs = tcs, key.tes = tes;
    key.patch_size = (tcs || tes) ? g_state.patch_size : 0;
    key.topology = mode;
    key.attrib_count = uint32_t(std::min<size_t>(g_state.attribs.size(), 16));
    std::copy_n(g_state.attribs.begin(), key.attrib_count, key.attribs);
    key.stream_count = uint32_t(std::min<size_t>(g_state.streams.size(), 16));
    std::copy_n(g_state.streams.begin(), key.stream_count, key.streams);
    std::copy_n(g_state.blend, 8, key.blend);
    key.color = g_state.color, key.mask = g_state.mask, key.depth = g_state.depth, key.polygon = g_state.polygon;
    key.color_count = t.color_count, key.depth_clamp = g_state.depth_clamp;
    key.depth_bounds = g_state.depth_bounds;
    key.alpha_to_coverage = g_state.alpha_to_coverage;
    key.line_width = g_state.line_width;
    key.alpha_ref = g_state.alpha_ref;
#ifdef NX_BAYONETTA3
    key.window_origin = device_window_origin_mode();
    key.depth_mode = device_depth_mode();
#else
    // Preserve the established Wonder translation until its device path tracks modes.
    key.window_origin = 1;
    key.depth_mode = 0;
#endif
    for (uint32_t k = 0; k < t.color_count; k++) {
        GpuImage* img = t.colors[k] ? image_of(t.colors[k]) : nullptr;
        key.color_formats[k] = img ? view_format(*img, t.views.format[k]) : unused_slot_format();
        if (img && (img->nvn_format == 0x3f || t.views.format[k] == 0x3f)) key.ufloat_colors |= 1u << k;  // G4
    }
    GpuImage* dimg = tt.depth ? image_of(tt.depth) : nullptr;
    key.depth_format = dimg && dimg->aspect != VK_IMAGE_ASPECT_COLOR_BIT ? dimg->format : VK_FORMAT_UNDEFINED;
    // NX_ULTRAWIDE. The world is widened by the camera aspect patch (nvn_ultrawide.cpp), so only the 16:9 UI is
    // squeezed by 1/k (centred safe area). The UI is drawn into small BGRA8 layer targets (430x180 for the HUD, never
    // widened) and composited into the scene target by a quad: the vertex shaders seen drawing into such layer targets
    // are the "UI" ones, and a UI quad into a widened target that does not sample a widened texture is squeezed.
    // NX_UW_CAMERA=0 (no camera patch): every geometry draw is squeezed, a world-and-UI shrink.
    static const bool cam_patch = !(std::getenv("NX_UW_CAMERA") && std::getenv("NX_UW_CAMERA")[0] == '0');
    if (uw_on() && cam_patch && !g_uw_wide && t.color_count && t.colors[0]) {
        const Texture* lt = get<Texture>(t.colors[0]);
        if (lt && lt->format == 0x25 && (lt->flags & 8)) uw_ui_vertex_shader(vs);
    }
    // NX_UW_UI=learn: only the vertex shaders learned above; default: every draw without depth test (screen-space UI).
    static const bool ui_learn = std::getenv("NX_UW_UI") && std::string(std::getenv("NX_UW_UI")) == "learn";
    const bool ui_pick = !cam_patch || (ui_learn ? uw_is_ui_vertex_shader(vs) : !g_state.depth.depth_test);
    if (g_uw_wide && uw_vs_scaled(vs) && ui_pick) {
        // A short draw (a triangle or a quad) that samples a widened render target is a screen-space pass (post
        // chain, composite): it already covers the widened target.
        bool samples_wide = false;
        const uint64_t count = indexed ? uint64_t(c.i[0]) : uint64_t(c.i[1]);
        if (count <= 6)
            for (const ResolvedTexture& r : g_textures.resolved)
                if (r.texture)
                    if (GpuImage* si = image_of(r.texture); si && si->uw) samples_wide = true;
        if (!samples_wide) key.uw_x = 1.0f / uw_factor();
    }
    if (uw_on()) {  // NX_UW_TRACE=<first draw>:<count>: draws (wide = a widened target)
        static const char* e = std::getenv("NX_UW_TRACE");
        static uint64_t from = 0, n = 0;
        // Parse into unsigned long long, not uint64_t: "%lu" is the wrong width for it on Windows (LLP64),
        // where it would write 4 bytes into an 8-byte object and leave the other half at whatever it was.
        static bool init = [&] {
            unsigned long long a = 0, b = 0;
            if (e && std::sscanf(e, "%llu:%llu", &a, &b) == 2) from = a, n = b;
            return true;
        }();
        (void)init;
        if (e && g_stats.total >= from && g_stats.total < from + n) {
            GpuImage* wi = t.color_count && t.colors[0] ? image_of(t.colors[0]) : nullptr;
            const Texture* tx = t.color_count && t.colors[0] ? get<Texture>(t.colors[0]) : nullptr;
            std::string smp;
            for (const ResolvedTexture& r : g_textures.resolved) {
                GpuImage* si = r.texture ? image_of(r.texture) : nullptr;
                char bb[48];
                const Texture* st = r.texture ? get<Texture>(r.texture) : nullptr;
                std::snprintf(bb, sizeof bb, " %ux%u%s(f%x,fmt%x,%llx)", si ? si->width : 0, si ? si->height : 0, si && si->uw ? "w" : "", st ? st->flags : 0, st ? st->format : 0, (unsigned long long)r.texture);
                smp += bb;
            }
            log("uwsamples:%s", smp.c_str());
            log("uwdraw %llu vs %s fs %s %s count %d tgt %ux%u fl %x fmt %x wide %d scaled %d dt %d dw %d bl %d vp %d %d %d %d", (unsigned long long)g_stats.total,
                vs->hash.c_str(), fs ? fs->hash.c_str() : "-", indexed ? "idx" : "arr", indexed ? c.i[0] : c.i[1],
                wi ? wi->width : 0, wi ? wi->height : 0, tx ? tx->flags : 0, wi ? wi->nvn_format : 0, g_uw_wide ? 1 : 0, key.uw_x > 0 ? 1 : 0,
                g_state.depth.depth_test, g_state.depth.depth_write, g_state.color.blend_enable[0],
                g_state.viewport[0], g_state.viewport[1], g_state.viewport[2], g_state.viewport[3]);
        }
    }
    VkPipeline pipeline;
    {
        PhaseScope p(kPPipeline);  // the canonical key lookup, the cache probe, a pending compile
        key_phase.close();
        pipeline = graphics_pipeline(key, layout);
    }
    if (!pipeline) {
        g_stats.no_pipeline++;
        return false;
    }

    if (g_bound.cb != cb || draw_slow(kSlowState)) g_bound = Bound{}, g_bound.cb = cb;
    if (g_bound.pipeline != pipeline) {
        p_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        g_bound.pipeline = pipeline, g_state_set++;
    } else {
        g_state_kept++;
    }
    {
        PhaseScope p(kPSets);  // resolve_sets: the cache probe, vkUpdateDescriptorSets, vkBindDescriptorSets
        bind_sets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout);
    }

    {
        PhaseScope p(kPState);  // sixteen vertex streams and the four dynamic-state compares
        // The vertex buffers resolved above, plus the zero stream (binding 15).
        for (uint32_t s = 0; s < 15; s++)
            if (in.used >> s & 1) bind_vertex(cb, s, in.vertex[s]);
        bind_vertex(cb, 15, zero_stream());
    }

    VkViewport vp{0, 0, float(g_pass_extent.width), float(g_pass_extent.height), 0, 1};
    if (g_state.has_viewport)
        vp = {float(g_state.viewport[0]), float(g_state.viewport[1]), float(g_state.viewport[2]),
              float(g_state.viewport[3]), g_state.depth_near, g_state.depth_far};
    // Scissors are already in window coordinates, independent of the NDC Y
    // direction. Intersect with the attachment after ultrawide expansion.
    int32_t sc_x = 0, sc_y = 0, sc_w = int32_t(g_pass_extent.width), sc_h = int32_t(g_pass_extent.height);
    if (g_state.has_scissor)
        sc_x = g_state.scissor[0], sc_y = g_state.scissor[1], sc_w = g_state.scissor[2], sc_h = g_state.scissor[3];
    if (g_uw_wide) {  // NX_ULTRAWIDE: the guest's 16:9 pixel spans, widened
        if (g_state.has_viewport) {
            int32_t x = int32_t(vp.x), w = int32_t(vp.width);
            uw_span(x, w, g_pass_extent.width);
            vp.x = float(x), vp.width = float(w);
        }
        if (g_state.has_scissor) uw_span(sc_x, sc_w, g_pass_extent.width);
    }
    vp = window_viewport(vp, key.window_origin);
    const VkRect2D sc = window_scissor(sc_x, sc_y, sc_w, sc_h, g_pass_extent);
    {
        PhaseScope p(kPState);  // set_dynamic: viewport, scissor, blend constants, raster (depth bias, stencil)
        set_dynamic(cb, vp, sc);
        // Emit optional states for every draw: a prior pipeline may have made
        // them static, or this may be a new command buffer.
        set_extended_dynamic(cb, key);
    }

    if (!is_indirect) trace_skin(indexed, c, vs, fs);
    // NX_REPLAY_CUTS=barrier: last point before the draw command; only a marker left by an in-pass merge copy is still
    // pending here (a composed read ends the pass and is normally flushed before it).
    {
        PhaseScope p(kPEmit);  // the pre-draw barrier, the index bind and the draw command itself
        copy_write_barrier(cb);
    if (indexed) {
        uint32_t count = uint32_t(c.i[0]);
        if (g_bound.has_index && g_bound.index.buffer == in.index.buffer && g_bound.index.offset == in.index.offset &&
            g_bound.index_type == in.index_type) {
            g_state_kept++;
        } else {
            p_vkCmdBindIndexBuffer(cb, in.index.buffer, in.index.offset, in.index_type);
            g_bound.has_index = true, g_bound.index = in.index, g_bound.index_type = in.index_type, g_state_set++;
        }
        bool q = zero_draws_on() && zero_draws_begin(cb, vs->hash + " " + (fs ? fs->hash : std::string("-")));
        if (is_indirect) emit_indexed_indirect(cb, indirect);
        else p_vkCmdDrawIndexed(cb, count, uint32_t(std::max(1, c.i[3])), 0, c.i[1], uint32_t(c.i[2]));
        if (q) zero_draws_end(cb);
    } else {
        bool q = zero_draws_on() && zero_draws_begin(cb, vs->hash + " " + (fs ? fs->hash : std::string("-")));
        p_vkCmdDraw(cb, uint32_t(c.i[1]), uint32_t(std::max(1, c.i[3])), uint32_t(c.i[0]), uint32_t(c.i[2]));
        if (q) zero_draws_end(cb);
    }
    }  // kPEmit (NX_REPLAY_PROF)
    g_stats.done++;
    g_pass_written |= ((1u << t.color_count) - 1) | (g_state.depth.depth_write || g_state.depth.stencil_test ? 1u << 8 : 0);
    // NX_SNAP_FS=<hash>,<hash>[@<first draw>]: also snapshot after draws with those fragment shaders (8 at most, or
    // NX_SNAP_FS_MAX).
    static const std::string snap_fs = std::getenv("NX_SNAP_FS") ? std::getenv("NX_SNAP_FS") : "";
    static const uint64_t snap_from = snap_fs.find('@') != std::string::npos
                                          ? std::strtoull(snap_fs.c_str() + snap_fs.find('@') + 1, nullptr, 10) : 0;
    static int snap_count = 0;
    static const int snap_max = std::getenv("NX_SNAP_FS_MAX") ? std::atoi(std::getenv("NX_SNAP_FS_MAX")) : 8;
    bool snap_by_fs = !snap_fs.empty() && g_stats.done >= snap_from && snap_count < snap_max &&
                      ((fs && snap_fs.find(fs->hash) != std::string::npos) ||
                       snap_fs.find(vs->hash) != std::string::npos) &&
                      ++snap_count;  // the vertex shader's hash matches too
    if ((snap_by_fs || snap_follow(t, vs, fs, g_textures.handles) || debug_snap_wanted(g_stats.done)) && t.color_count && t.colors[0])
        if (GpuImage* snap = image_of(t.colors[0])) draw_end_rendering(cb, 6), debug_snapshot(cb, *snap, g_stats.done);
    // NX_DUMP_ON_TARGET=<hex NVNtexture*>: every NX_DUMP_FRAMES-th draw into it asks for NX_DUMP_TEXTURES to be read
    // back right after this submit (before later frames change them).
    static const char* dump_env = std::getenv("NX_DUMP_ON_TARGET");
    static const uint64_t dump_target = dump_env ? std::strtoull(dump_env, nullptr, 16) : 0;
    if (dump_target && t.colors[0] == dump_target) g_dump_request = ++g_dump_draws;
    // NX_DUMP_ON_FS=<fs hash>[,...]: a draw with one of these fragment shaders dumps its target 0 and what it samples.
    static const std::string dump_fs = [] { const char* e = std::getenv("NX_DUMP_ON_FS"); return std::string(e ? e : ""); }();
    static const uint64_t dump_from = [] { const char* e = std::getenv("NX_DUMP_FROM"); return e ? std::strtoull(e, nullptr, 10) : 0; }();
    if (!dump_fs.empty() && fs && g_frame >= dump_from && dump_fs.find(fs->hash) != std::string::npos && g_dump_draws < 400 &&
        t.colors[0] && image_of(t.colors[0]) && (std::getenv("NX_DUMP_ANY") || image_of(t.colors[0])->view_type == VK_IMAGE_VIEW_TYPE_CUBE ||
                                                           image_of(t.colors[0])->view_type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY)) {
        debug_dump_add(t.colors[0]);
        for (const ResolvedTexture& r : g_textures.resolved) debug_dump_add(r.texture);
        g_dump_request = ++g_dump_draws;
    }
    draw_hist_note(vs, fs);
    trace_draw(t, mode, indexed, c, vs, fs, g_textures.bindings, g_textures.handles);
    // NX_TRACE_PRESENT: a draw into the presented texture. Dump what it wrote and what it sampled right after this
    // submit (NX_DUMP_TARGET_EVERY=1 makes every request dump, NX_DUMP_RAW the float images' real values too): that
    // separates "the composite writes nothing" from "it reads an empty or wrong image". Spread over the traced
    // draws (the first two, then every 100th), so the dumps cover the start of the black stretch and its steady state.
    static uint64_t present_hits = 0;
    if (g_trace_present_hit && (present_hits < 2 || present_hits % 100 == 0) && present_hits < 400) {
        if (t.colors[0]) debug_dump_add(t.colors[0]);
        for (const ResolvedTexture& r : g_textures.resolved)
            if (GpuImage* si = r.texture ? image_of(r.texture) : nullptr; si && si->width >= 256 && si->height >= 256)
                debug_dump_add(r.texture);
        g_dump_request = ++g_dump_draws;
    }
    present_hits += g_trace_present_hit ? 1 : 0;
    // A pass whose colour target 0 is null still runs; whether its draws matter depends on whether the game meant
    // targets 1..n (an MRT pass, or a depth-only prepass) or on our SetRenderTargets resolution. Name the shaders that
    // draw into it, once each, so the question is answered from the log (docs/BAYONETTA3_BUGS.md 2.4).
    if (!t.colors[0]) {
        // By pointer, and not at all once full: a fight has ~950 of these draws a frame, depth prepasses, and building a
        // string for each took three allocations and a tree walk to say nothing new.
        static std::set<std::pair<const ShaderInfo*, const ShaderInfo*>> into_null;
        if (into_null.size() < 40 && into_null.insert({vs, fs}).second)
            log("nvn: draw into a pass with no colour target 0: vs %s fs %s %s count %u x%d targets %u depth %llx",
                vs->hash.c_str(), fs ? fs->hash.c_str() : "-", indexed ? "indexed" : "arrays",
                indexed ? c.i[0] : c.i[1], c.i[3], t.color_count, (unsigned long long)t.depth);
    }
    return true;
}

}  // namespace

// NX_TRACE_PRESENT (nvn_copy.cpp): read these two images back after this submit — a texture copy into the presented
// texture is the other way the screen's picture is made, and its source has to be seen. A handful of dumps at most.
// Defined here, outside the anonymous namespace, because nvn_copy.cpp calls it across translation units.
void debug_dump_request_presented(uint64_t a, uint64_t b) {
    static int dumps = 0;
    if (dumps >= 6) return;
    dumps++;
    debug_dump_add(a);
    debug_dump_add(b);
    g_dump_request = ++g_dump_draws;
}

// NX_TRACE_VIDEO: what a sampled movie plane's pass draws into (nvn_pipeline.cpp logs it).
namespace replay {
const Targets& draw_open_pass() { return g_pass; }
GpuImage* draw_pass_image(uint32_t k) { return k < 9 ? g_pass_images[k] : nullptr; }
// NX_LAZY_COMPUTE_BARRIER: a dispatch's writes, made visible to what follows (nvn_written.cpp calls this before it
// copies or reads a written range). One barrier covers every dispatch since the last one.
void compute_write_barrier(VkCommandBuffer cb) {
    if (!g_compute_writes) return;
    g_compute_writes = false;
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0,
                           nullptr, 0, nullptr);
    g_compute_barriers++;
}
}  // namespace replay

void draw_end_submit() {  // after the submit's fence: compute results to guest memory, debug snapshots
    debug_snapshots_write();
    auto write_backs = draw_take_write_backs();
    write_backs_apply(write_backs);  // NX_COPY_THREADS, as the pipelined path's apply_write_backs
}

std::vector<WriteBack> draw_take_write_backs() {  // an asynchronous submit copies them when it is retired
    std::vector<WriteBack> v = written_take();  // one per written range (disjoint), then NX_COMPUTE_SEPARATE's
    v.insert(v.end(), separate_write_backs().begin(), separate_write_backs().end());
    separate_write_backs().clear();
    return v;
}

void draw_begin_submit() {
    g_rendering = false;
    copyin_count_frame();  // Count submits separately; the present window is passed to written_log.
    g_vertex_pending = false, g_vertex_visible_at = g_pass_serial;  // each command buffer begins with a full barrier
    g_compute_writes = false;  // a part begins with a full barrier (frame_begin/begin_part in nvn_frames.cpp)
    g_bound = Bound{};  // a new command buffer
    submit_lists_reset();
    sets_begin_submit();
    clears_begin_submit();
    g_submit_ns = plat::monotonic_ns();
    dedup_reset();
    written_reset();
    pipeline_begin_submit();
}

void draw_frame_log(uint64_t recorded) {
    const auto sample = draw_frame_stats::tracker().present(recorded, g_stats.total, g_stats.done);
    log("nvn draw frame %llu%s%s: recorded %llu, replayed %llu, emitted %llu; "
        "arrays %llu, elements %llu, indirect %llu, multi-indirect %llu; "
        "nested draws %llu, draws under repeated calls %llu, list calls %llu, repeated calls %llu",
        (unsigned long long)sample.sequence, sample.first ? " (first interval)" : "",
        sample.ordered ? "" : " (non-monotonic sample)",
        (unsigned long long)sample.recorded, (unsigned long long)sample.replayed,
        (unsigned long long)sample.emitted, (unsigned long long)sample.kinds[0],
        (unsigned long long)sample.kinds[1], (unsigned long long)sample.kinds[2],
        (unsigned long long)sample.kinds[3], (unsigned long long)sample.nested_draws,
        (unsigned long long)sample.repeated_branch_draws, (unsigned long long)sample.calls,
        (unsigned long long)sample.repeated_calls);
}

void draw_log_stats(uint64_t window) {
    draw_hist_log();
    if (g_compute_ends)
        log("nvn: compute barriers: %llu dispatch ends folded behind %llu barriers (NX_LAZY_COMPUTE_BARRIER)",
            (unsigned long long)g_compute_ends, (unsigned long long)g_compute_barriers);
    g_compute_barriers = g_compute_ends = 0;
    if (g_vertex_barriers) log("nvn passes: %llu vertex-read barriers (G4)", (unsigned long long)g_vertex_barriers);
    dedup_log();
    render_pass_log();
    written_log(window);
    sampled_ready_log();
    sets_log();
    if (g_state_set + g_state_kept)
        log("nvn: draw state: %llu binds and sets recorded, %llu already in place", (unsigned long long)g_state_set,
            (unsigned long long)g_state_kept);
    g_state_set = g_state_kept = 0;
    clears_log();
    log("nvn passes ended: %llu submit end, %llu draw, %llu dispatch, %llu clear of other targets, %llu clear larger than "
        "the pass, %llu copy, %llu snapshot, %llu SetRenderTargets", (unsigned long long)g_pass_ends[0],
        (unsigned long long)g_pass_ends[1], (unsigned long long)g_pass_ends[2], (unsigned long long)g_pass_ends[3],
        (unsigned long long)g_pass_ends[4], (unsigned long long)g_pass_ends[5], (unsigned long long)g_pass_ends[6],
        (unsigned long long)g_pass_ends[7]);
    Stats& t = g_stats;
    list_calls_log();
    log("nvn draws: %llu recorded, %llu replayed:", (unsigned long long)recorded_draws(), (unsigned long long)t.total);
    log("nvn draws: %llu total, %llu done, %llu no program, %llu no vertex shader, %llu no pipeline, %llu no indices",
        (unsigned long long)t.total, (unsigned long long)t.done, (unsigned long long)t.no_program,
        (unsigned long long)t.no_vertex, (unsigned long long)t.no_pipeline, (unsigned long long)t.no_indices);
    log("nvn passes: %llu ended by a draw's texture uploads or layout changes, %llu by new targets, %llu dispatches",
        (unsigned long long)t.breaks_textures, (unsigned long long)t.breaks_targets, (unsigned long long)t.dispatches);
    // The window is what the caller says it is: 60 presents early in a run, 300 later. Dividing by a fixed 300 made
    // every early combat window read 5x low.
    nx::nvn::replay_prof_log(window);
    nx::nvn::indirect_prof_log(window);  // NX_INDIRECT_PROF: the parts of the indexed indirect path
    nx::nvn::replay::input_prof_log(window);  // NX_INPUT_PROF: inside the inputs bucket
}

void pass_end_only(VkCommandBuffer cb, int reason) {
    if (!g_rendering) return;
    g_pass_ends[reason & 7]++, trace_pass("end: reason", g_pass, uint32_t(reason), 0);
    if (g_render_pass)
        p_vkCmdEndRenderPass(cb);
    else
        p_vkCmdEndRendering(cb);
    g_rendering = false;
    // What the pass wrote is read by what follows (sampled, copied, loaded by the next pass), and what it sampled may
    // be rendered over next. The images stay GENERAL, so transition() records no barrier. Metal (MoltenVK) and the
    // Intel driver ordered passes anyway; NVIDIA overlaps them, and later passes read half-written tiles: the course
    // HUD in blocks (DEBUG-2 #71). NX_NO_PASS_BARRIER=1: none, as before (bisecting).
    // Narrow (PHASE4.md G4; NX_PASS_BARRIER_NARROW=1): the later stages only, so the next pass's vertex work and
    // binning may start while this one finishes. A draw whose vertex shader samples a rendered image gets a barrier
    // of its own first (vertex_read_barrier). By default ALL_COMMANDS, as before.
    static const bool no_barrier = std::getenv("NX_NO_PASS_BARRIER") != nullptr;
    if (!no_barrier) {
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                           VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        VkPipelineStageFlags dst = narrow_pass_barrier()
                                       ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                             VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT |
                                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
                                       : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, dst, 0, 1, &mb, 0, nullptr, 0, nullptr);
        g_vertex_pending |= narrow_pass_barrier();
    }
    g_pass_serial++;
    for (uint32_t k = 0; k < 9; k++)  // this pass's targets only (the other slots may name freed images)
        if ((k < g_pass.color_count || (k == 8 && g_pass.depth)) && g_pass_images[k])
            g_pass_images[k]->rendered_at = g_pass_serial;
    gpu_profile_mark(cb, "outside passes (clears, uploads, copies)");
}
void draw_end_rendering(VkCommandBuffer cb, int reason) {  // and the pending clears (nvn_clears.cpp)
    pass_end_only(cb, reason);
    clears_flush(cb);
}
bool pass_open() { return g_rendering; }
bool pass_open_on(const Targets& t) { return g_rendering && targets_same(g_pass, t); }
VkExtent2D pass_extent() { return g_pass_extent; }
void pass_begin(VkCommandBuffer cb, const Targets& t) { begin_rendering(cb, t); }
// After a clear inside the pass (nvn_clears.cpp). A masked one is a fullscreen draw with its own pipeline, viewport
// and scissor (nvn_mask_clear.cpp): the next draw sets everything again.
void pass_mark_written(uint32_t bits) { g_pass_written |= bits, g_bound = Bound{}; }

bool replay::pass_wrote(const GpuImage* img) {
    if (!g_rendering || !img) return false;
    uint32_t hit = g_pass.depth && g_pass_images[8] == img ? 1u << 8 : 0;
    for (uint32_t c = 0; c < g_pass.color_count; c++)
        if (g_pass.colors[c] && g_pass_images[c] == img) hit |= 1u << c;
    return (hit & g_pass_written) != 0;
}

uint64_t submit_start_ns() { return g_submit_ns; }

// Entry points of the draws and dispatches for draw_command (nvn_draw_state.cpp).
bool draw_call(VkCommandBuffer cb, const Targets& t, uint32_t mode, bool indexed, const Cmd& c) {
    return draw(cb, t, mode, indexed, c);
}
void dispatch_call(VkCommandBuffer cb, const Cmd& c) { dispatch(cb, c); }

uint64_t draw_take_dump_request() {
    uint64_t r = g_dump_request;
    g_dump_request = 0;
    return r;
}

}  // namespace nx::nvn
