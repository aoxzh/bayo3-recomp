// Texture uploads (nvn_pipeline.h): guest pool memory into the host image, untiled, widened where the host stores a
// format wider than NVN does, and checked against what the copies read (docs/archive/nvn/DEBUG.md #50).
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "nvn_astc.h"
#include "nvn_astc_cache.h"
#include "nvn_copy_behind.h"
#include "nvn_pipeline.h"
#include "nvn_draw_state.h"
#include "nvn_tiling.h"
#include "platform.h"

namespace nx::nvn {

using namespace nx::vk;

namespace {

// R11G11B10F (NVN 0x3f) is stored as RGBA16F (nvn_format.cpp). Its channels are unsigned floats with half's 5-bit
// exponent and bias 15, and 6 (R, G) or 5 (B) mantissa bits: each is a half with the low mantissa bits zero and no
// sign, so shifting it into place is exact (Inf and NaN included). Alpha is 1. Copying the 4-byte texels as they are
// read twice the data: rows side by side, bits reinterpreted, the bottom half from other uploads (the garbage band).
void widen_r11g11b10(const uint8_t* src, uint8_t* dst, uint64_t texels) {
    for (uint64_t k = 0; k < texels; k++) {
        uint32_t v;
        std::memcpy(&v, src + k * 4, 4);
        uint16_t h[4] = {uint16_t((v & 0x7FF) << 4), uint16_t(((v >> 11) & 0x7FF) << 4),
                         uint16_t(((v >> 22) & 0x3FF) << 5), 0x3C00};
        std::memcpy(dst + k * 8, h, 8);
    }
}

// NX_FULL_UPLOAD=1: a pitch-linear texture the CPU wrote part of is uploaded whole again (bisecting).
bool full_upload() {
    static const bool on = std::getenv("NX_FULL_UPLOAD") != nullptr;
    return on;
}

// Bytes from `base` to the end of what vkCmdCopyBufferToImage reads for `r`, at `bytes` per bw x bh block.
uint64_t copy_end(const VkBufferImageCopy& r, uint64_t base, uint32_t bw, uint32_t bh, uint32_t bytes) {
    uint64_t row_blocks = ((r.bufferRowLength ? r.bufferRowLength : r.imageExtent.width) + bw - 1) / bw;
    uint64_t image_rows = ((r.bufferImageHeight ? r.bufferImageHeight : r.imageExtent.height) + bh - 1) / bh;
    uint64_t w = (r.imageExtent.width + bw - 1) / bw, h = (r.imageExtent.height + bh - 1) / bh;
    uint64_t slices = uint64_t(r.imageExtent.depth) * r.imageSubresource.layerCount;
    if (!w || !h || !slices) return r.bufferOffset - base;
    return r.bufferOffset - base + ((slices - 1) * image_rows + (h - 1)) * row_blocks * bytes + w * bytes;
}

// The copies read at most `staged` bytes from `base`, counted at the host format's size. A copy past it reads other
// uploads of the submit (or past the buffer): garbage, so the image is cleared instead. NX_NO_UPLOAD_GUARD=1: the old
// way (bisecting).
bool copies_fit(const Texture& t, const VkBufferImageCopy* r, uint32_t n, uint64_t base, uint64_t staged, uint32_t bw,
                uint32_t bh, uint32_t bytes) {
    static const bool off = std::getenv("NX_NO_UPLOAD_GUARD") != nullptr;
    uint64_t need = 0;
    for (uint32_t k = 0; k < n; k++) need = std::max(need, copy_end(r[k], base, bw, bh, bytes));
    if (off || need <= staged) return true;
    static std::set<uint32_t> logged;  // once per format
    if (logged.insert(t.format).second)
        log("nvn/vk: upload short: texture %ux%u format %#x reads %llu bytes, %llu staged: cleared instead", t.width,
            t.height, t.format, (unsigned long long)need, (unsigned long long)staged);
    return false;
}

// NX_ASTC_CACHE_MB=64 opts into a bounded decode-result cache; unset/0/invalid
// keeps it off, and values above 256 are rejected. This retains CPU byte arrays,
// not VkImages: host visibility, deswizzle, staging and image transitions still run.
void log_astc_cache(const AstcDecodeCache& cache) {
    static uint64_t requests = 0;
    if (!cache.budget() || ++requests % 200 != 1) return;
    const auto& s = cache.stats();
    log("nvn/vk: ASTC cache: %llu hits, %llu misses, %llu evicted, %llu bypassed (%.1f MiB input+RGBA), "
        "%.1f/%.1f MiB retained, %zu entries, %.1f MiB hits, %.2f ms lookup/restore, %.2f ms insert, "
        "%.2f ms original decode samples for hits (historical estimate)",
        (unsigned long long)s.hits, (unsigned long long)s.misses, (unsigned long long)s.evicted,
        (unsigned long long)s.bypassed, double(s.bypassed_bytes) / (1u << 20),
        double(cache.used()) / (1u << 20), double(cache.budget()) / (1u << 20), cache.size(),
        double(s.hit_bytes) / (1u << 20), double(s.lookup_ns) / 1e6, double(s.insert_ns) / 1e6,
        double(s.hit_original_decode_ns) / 1e6);
}

// Room for a texture in the upload ring or a retained staging buffer.
Upload texture_stage(uint64_t size);

// ASTC on a GPU without it (nvn_format.cpp): the untiled blocks of every level become RGBA8 texels, and the levels
// are re-laid out for them (each level's layers back to back, 16-byte aligned). Logged every 200 actual decodes:
// count, bytes and CPU time. Calls are serialized by the existing replay/upload path.
Upload decode_astc(uint32_t format, std::vector<uint8_t>& linear, std::vector<LinearLevel>& levels, uint32_t layers,
                   uint32_t bw, uint32_t bh, bool volume, size_t& staged) {
    static std::atomic<uint64_t> textures{0}, bytes{0}, ns{0}, invalid{0};
    // Actual decodes only, per timing window (nvn_pipeline_cache.cpp).
    auto count_decode = [](uint64_t size, uint64_t spent) {
        note_astc_decoded(spent);
        bytes += size, ns += spent;
        if (textures.fetch_add(1) % 200 == 0)
            log("nvn/vk: ASTC decoded on the CPU: %llu textures, %.1f MB, %.0f ms, %llu invalid blocks",
                (unsigned long long)textures.load(), double(bytes.load()) / 1e6, double(ns.load()) / 1e6,
                (unsigned long long)invalid.load());
    };
    static AstcDecodeCache cache(astc_cache_budget(std::getenv("NX_ASTC_CACHE_MB")));
    static const bool direct_enabled = [] {
        const char* e = std::getenv("NX_ASTC_DIRECT");
        return !(e && e[0] == '0' && !e[1]);
    }();
    const uint64_t t0 = plat::monotonic_ns();
    const bool srgb = format >= 0x87;
    const AstcDecodeSpec spec{format, bw, bh, layers, volume};
    // Validate before allocating or looking up a cached image.
    if (bw < 4 || bw > 12 || bh < 4 || bh > 12 || !layers)
        fatal("nvn/vk: invalid ASTC footprint %ux%u for format %#x", bw, bh, format);
    std::vector<LinearLevel> out;
    size_t at = 0;
    if (!astc_output_layout(spec, linear.size(), levels, out, at))
        fatal("nvn/vk: invalid or overflowing ASTC layout for format %#x (%zu levels, %zu staged bytes)",
              format, levels.size(), linear.size());

    // Allocation may retire work and reset other caches. Do it before looking up
    // an entry, and keep the resulting upload until the recorded copy completes.
    const uint64_t alloc_t0 = plat::monotonic_ns();
    Upload u = texture_stage(at);
    const uint64_t alloc_ns = plat::monotonic_ns() - alloc_t0;
    staged = at;
    const bool cacheable = cache.eligible(linear.size(), at, levels.size());
    uint64_t hash = 0;
    if (cacheable) {
        const uint64_t lookup_t0 = plat::monotonic_ns();
        hash = hash_bytes(linear.data(), linear.size());  // existing four-lane pipeline hash
        const auto* hit = cache.find(spec, linear, levels, hash);
        if (hit) {
            std::memcpy(u.ptr, hit->rgba.data(), at);
            levels.swap(out);  // the validated output layout is exactly the cached one
            cache.note_lookup(plat::monotonic_ns() - lookup_t0);
            log_astc_cache(cache);
            return u;
        }
        cache.note_lookup(plat::monotonic_ns() - lookup_t0);
    } else if (cache.budget()) {
        cache.note_bypass(linear.size(), at);  // skip hashing entries that cannot fit
    }

    const uint64_t decode_t0 = cacheable ? plat::monotonic_ns() : 0;
    // Direct stores are only appropriate in a known CPU-cached mapping. Keep
    // the previous vector + memcpy path for uncached/unknown mappings, and for
    // NX_ASTC_DIRECT=0. astc_decode_image joins its workers before returning.
    const bool direct = direct_enabled && u.host_cached;
    const bool srgb_format = srgb;
    auto decode_into = [=](const std::vector<uint8_t>& blocks, const std::vector<LinearLevel>& from,
                           const std::vector<LinearLevel>& to, uint8_t* rgba) {
        if (direct) {
            // The decoder writes every texel, but not the 16-byte mip padding.
            for (size_t k = 0; k < to.size(); ++k) {
                const size_t end = k + 1 < to.size() ? size_t(to[k + 1].offset) : at;
                const size_t data_end = size_t(to[k].offset + to[k].layer_size * (volume ? to[k].depth : layers));
                if (end != data_end) std::memset(rgba + data_end, 0, end - data_end);
            }
        }
        for (size_t k = 0; k < from.size(); k++)
            invalid += astc_decode_image(blocks.data() + from[k].offset, from[k].width, from[k].height,
                                         volume ? from[k].depth : layers, bw, bh, srgb_format, rgba + to[k].offset);
    };
    // NX_ASTC_BEHIND=1 (opt-in until it has run in the game; the unit test covers it): off the replay thread. A fight streams textures in lumps (700-840 ms of decoding in a 60-frame
    // window, 3-12 ms a frame): that was most of what was left between the slow windows and the rest. What the decode
    // needs is its own copy of the blocks and a destination nothing reads until the part that copies it is queued, which
    // waits for it (nvn_frames.cpp). A backlog past 64 MB, a scene load's, decodes here instead of holding the blocks.
    static const bool behind_enabled = [] {
        const char* e = std::getenv("NX_ASTC_BEHIND");
        return e && e[0] == '1' && !e[1];
    }();
    if (behind_enabled && !cacheable && async_gpu() && replay::copy_behind::helpers() &&
        replay::copy_behind::lane().backlog() < (64u << 20)) {
        auto blocks = std::make_shared<std::vector<uint8_t>>();
        blocks->swap(linear);
        const uint64_t held = blocks->size();
        replay::copy_behind::lane().push([=, from = levels, to = out, target = u.ptr] {
            const uint64_t start = plat::monotonic_ns();
            if (direct) {
                decode_into(*blocks, from, to, target);
            } else {
                std::vector<uint8_t> texels(at);
                decode_into(*blocks, from, to, texels.data());
                std::memcpy(target, texels.data(), at);
            }
            count_decode(at, plat::monotonic_ns() - start);
        }, held);
        levels.swap(out);
        return u;
    }
    std::vector<uint8_t> fallback;
    uint8_t* rgba = u.ptr;
    if (!direct) {
        fallback.resize(at);
        rgba = fallback.data();
    }
    decode_into(linear, levels, out, rgba);
    // NX_ASTC_DUMP=<count>: the first <count> decodes of 256x256 or larger write each level's layer 0 to
    // <data root>/astc/NNN_L<level>_<w>x<h>_<bw>x<bh>_<format>.astc (the standard .astc header and the untiled blocks)
    // and .rgba (this decoder's texels), so a reference decoder (astcenc -dl / -ds) can be compared with ours texel for
    // texel, and the levels with each other.
    static const uint64_t dump_count = [] {
        const char* e = std::getenv("NX_ASTC_DUMP");
        return e ? std::strtoull(e, nullptr, 10) : 0ull;
    }();
    static std::atomic<uint64_t> dumped{0};
    if (dump_count && !levels.empty() && levels[0].width >= 256 && levels[0].height >= 256 && dumped < dump_count) {
        const uint64_t n = dumped++;
        plat::make_dirs(plat::data_root() + "/astc");
        for (size_t k = 0; k < levels.size(); k++) {
            const uint32_t w = levels[k].width, h = levels[k].height;
            const size_t block_bytes = size_t((w + bw - 1) / bw) * ((h + bh - 1) / bh) * 16;
            const std::string base = plat::data_root() + "/astc/" + std::to_string(n) + "_L" + std::to_string(k) + "_" +
                                     std::to_string(w) + "x" + std::to_string(h) + "_" + std::to_string(bw) + "x" +
                                     std::to_string(bh) + "_" + std::to_string(format);
            if (FILE* f = plat::fopen_utf8(base + ".astc", "wb")) {
                const uint8_t header[16] = {0x13, 0xAB, 0xA1, 0x5C, uint8_t(bw), uint8_t(bh), 1,
                                            uint8_t(w), uint8_t(w >> 8), uint8_t(w >> 16),
                                            uint8_t(h), uint8_t(h >> 8), uint8_t(h >> 16), 1, 0, 0};
                std::fwrite(header, 1, 16, f);
                std::fwrite(linear.data() + levels[k].offset, 1, block_bytes, f);
                std::fclose(f);
            }
            if (FILE* f = plat::fopen_utf8(base + ".rgba", "wb")) {
                std::fwrite(rgba + out[k].offset, 1, size_t(w) * h * 4, f);
                std::fclose(f);
            }
        }
        log("nvn/vk: ASTC dump %llu (%zu levels)", (unsigned long long)n, levels.size());
    }
    if (cacheable) {
        const uint64_t insert_t0 = plat::monotonic_ns();
        if (!cache.insert(spec, linear, levels, hash, rgba, at, out, insert_t0 - decode_t0))
            cache.note_bypass(linear.size(), at);
        cache.note_insert(plat::monotonic_ns() - insert_t0);
    }
    uint64_t copy_ns = 0;
    if (!direct) {
        const uint64_t copy_t0 = plat::monotonic_ns();
        std::memcpy(u.ptr, rgba, at);
        copy_ns = plat::monotonic_ns() - copy_t0;
    }
    levels.swap(out);
    count_decode(at, plat::monotonic_ns() - t0 - alloc_ns - copy_ns);
    if (cache.budget()) log_astc_cache(cache);
    return u;
}

// Whether a 1 MB+ upload gets a staging buffer of its own (PHASE4.md U1). In steady state (a course frame stages ~30
// such textures, ~40 MB per second) the ring takes them: a fresh buffer each was an allocation (a kgsl ioctl that
// zero-fills), a page fault per 4 KB of the copy and a free at retire, on the replay thread. A scene load, which
// stages hundreds of MB in one submit, still gets its own buffers: past a quarter of the ring in this submit, or one
// upload over an eighth of it. NX_STAGING_MIN=0: a staging buffer for every 1 MB+ upload, as before.
bool staging_wanted(uint64_t size) {
    static const bool always = [] {
        const char* e = std::getenv("NX_STAGING_MIN");
        return e && e[0] == '0';
    }();
    uint64_t ring = frame_ring_size();
    return always || !ring || frame_upload_used() + size > ring / 4 || size > ring / 8;
}

// Which uploads may get a staging buffer at all: 1 MB and up, and, once this submit has put a quarter of the ring
// in the ring already (a scene load), 64 KB and up. NX_STAGING_LOAD=0: the 1 MB floor only, as before.
bool staging_candidate(uint64_t size) {
    static const bool load = [] {
        const char* e = std::getenv("NX_STAGING_LOAD");
        return !(e && e[0] == '0');
    }();
    if (size >= (1u << 20)) return true;
    uint64_t ring = frame_ring_size();
    return load && ring && size >= (64u << 10) && frame_upload_used() > ring / 4;
}

// Room for one texture's upload: a staging buffer of its own when it is big (or the submit already is), else the ring.
// NX_STAGING_RING=1: always the ring, as before (bisecting).
Upload texture_stage(uint64_t size) {
    static const bool ring_only = std::getenv("NX_STAGING_RING") != nullptr;
    if (!ring_only && staging_candidate(size) && staging_wanted(size)) return staging_alloc(size);
    return upload_alloc(size, 16);
}

}  // namespace

// NX_1D_IMAGES=1: NVN 1D textures as Vulkan 1D images again (they are 2D of height 1: Ryujinx samples 1D as 2D, G8).
bool vk_1d_images() {
    static const bool on = std::getenv("NX_1D_IMAGES") != nullptr;
    return on;
}

void clear_image(VkCommandBuffer cb, GpuImage& img, bool magenta) {
    transition(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageSubresourceRange r{img.aspect, 0, img.levels, 0, img.layers};
    if (img.aspect == VK_IMAGE_ASPECT_COLOR_BIT) {
        VkClearColorValue v{};
        if (magenta) v = {{1.0f, 0.0f, 1.0f, 1.0f}};
        p_vkCmdClearColorImage(cb, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &r);
    } else {
        VkClearDepthStencilValue v{1.0f, 0};  // far: the course passes test LESS
        p_vkCmdClearDepthStencilImage(cb, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &r);
    }
}

namespace {
// Texture upload time on the replay thread, per log period: the whole, and the parts that can be told apart.
uint64_t g_cost_total = 0, g_cost_untile = 0, g_cost_decode = 0, g_cost_stage = 0, g_cost_count = 0;
}  // namespace

void upload_cost_log(uint64_t frames) {
    if (g_cost_count && frames)
        log("nvn/vk: texture uploads: %llu, %.2f ms/frame (untile %.2f, stage %.2f, ASTC %.2f)",
            (unsigned long long)g_cost_count, double(g_cost_total) / 1e6 / double(frames),
            double(g_cost_untile) / 1e6 / double(frames), double(g_cost_stage) / 1e6 / double(frames),
            double(g_cost_decode) / 1e6 / double(frames));
    g_cost_total = g_cost_untile = g_cost_decode = g_cost_stage = g_cost_count = 0;
}

void upload_texture(VkCommandBuffer cb, const Texture& t, GpuImage& img, uint64_t lo, uint64_t hi) {
    struct CostScope {
        uint64_t t0 = plat::monotonic_ns();
        ~CostScope() { g_cost_total += plat::monotonic_ns() - t0, g_cost_count++; }
    } cost_scope;
    const bool was_uploaded = img.uploaded;
    if (gpu_profile_on()) gpu_profile_mark(cb, "texture uploads");
    if (!t.memory || !t.storage_size) return;
    // NX_TRACE_UPLOADS: every texture upload, with the reason it covers the rows it does (a whole re-upload every frame
    // is a frame-time bug: 16.8 MB per frame at 30 fps is half a gigabyte per second of staging copies).
    static const bool trace_uploads = std::getenv("NX_TRACE_UPLOADS") != nullptr;
    if (trace_uploads && t.storage_size >= (4u << 20)) {
        static int traced = 0;
        if (traced < 40) {
            ++traced;
            log("nvn: upload %ux%u fmt %#x stride %u storage %llu: was_uploaded %d, rows %llu..%llu, gpu_written %d, "
                "write_version %llu vs image %llu, alpha_from_r %d",
                t.width, t.height, t.format, t.stride, (unsigned long long)t.storage_size, int(was_uploaded),
                (unsigned long long)lo, (unsigned long long)hi,
                int(replay::written_overlaps(reinterpret_cast<const uint8_t*>(t.memory), t.storage_size)),
                (unsigned long long)replay::written_texture_version(reinterpret_cast<const uint8_t*>(t.memory), t.storage_size),
                (unsigned long long)img.uploaded_write_version, int(img.alpha_from_r));
        }
    }
    if (std::getenv("NX_TRACE_UPLOADS") && t.storage_size >= (1u << 20) &&
        !replay::written_overlaps(reinterpret_cast<const uint8_t*>(t.memory), t.storage_size)) {
        const uint8_t* m = reinterpret_cast<const uint8_t*>(t.memory);
        size_t nz = 0;
        for (uint64_t k = 0; k < t.storage_size; k += 64) nz += m[k] != 0;
        log("nvn: upload %ux%u fmt %#x stride %u: %zu of %llu sampled bytes non-zero", t.width, t.height, t.format,
            t.stride, nz, (unsigned long long)(t.storage_size / 64));
    }
    // NX_TRACE_UPLOADS: a big texture uploaded a SECOND time is exactly the bug named above; identify it (size,
    // memory, why the whole plane went up again) instead of guessing which one it is. Capped: in that state it
    // repeats every frame.
    if (trace_uploads && was_uploaded && t.storage_size >= (4u << 20)) {
        static int repeats = 0;
        if (repeats < 60) {
            ++repeats;
            log("nvn: REPEAT upload %ux%u fmt %#x stride %u storage %llu memory %llx rows %llu..%llu gpu_written %d, "
                "write_version %llu vs image %llu",
                t.width, t.height, t.format, t.stride, (unsigned long long)t.storage_size,
                (unsigned long long)t.memory, (unsigned long long)lo,
                (unsigned long long)hi,
                int(replay::written_overlaps(reinterpret_cast<const uint8_t*>(t.memory), t.storage_size)),
                (unsigned long long)replay::written_texture_version(reinterpret_cast<const uint8_t*>(t.memory), t.storage_size),
                (unsigned long long)img.uploaded_write_version);
        }
    }
    uint32_t bw = 1, bh = 1, bytes = 4;
    format_block(t.format, bw, bh, bytes);
    // What the copies read per block: the host format's size (RGBA16F for R11G11B10F), else the NVN one.
    uint32_t host_bytes = vk_block_bytes(img.format) ? vk_block_bytes(img.format) : bytes;
    // NX_R11G11B10_NARROW=1: R11G11B10F copied at 4 bytes per texel, as before (the guard then clears it unless
    // NX_NO_UPLOAD_GUARD=1 too; bisecting).
    static const bool narrow = std::getenv("NX_R11G11B10_NARROW") != nullptr;
    bool widen = !narrow && t.format == 0x3f && img.format == VK_FORMAT_R16G16B16A16_SFLOAT && !img.alpha_from_r;
    const auto* backing = reinterpret_cast<const uint8_t*>(t.memory);
    const uint64_t write_version = replay::written_texture_version(backing, t.storage_size);
    if (!t.stride || widen || img.alpha_from_r)
        replay::require_host_readable(backing, t.storage_size, "texture untile/format conversion");
    // A tracked write can reach rows outside a CPU dirty range. Include it in
    // this upload before remembering the revision for the image cache.
    if (write_version > img.uploaded_write_version) lo = 0, hi = UINT64_MAX;
    img.uploaded = true;
    img.uploaded_write_version = write_version;
    // Kept between textures: a burst's large ones each faulted a fresh allocation in. It is this thread's alone; the
    // background decode (NX_ASTC_BEHIND) swaps it away and the next texture starts a new one.
    static thread_local std::vector<uint8_t> linear;
    linear.clear();
    std::vector<VkBufferImageCopy> copies;
    uint32_t layers = img.view_type == VK_IMAGE_VIEW_TYPE_3D ? std::max(1u, t.depth) : img.layers;
    if (img.alpha_from_r && t.stride) {  // R8 pitch-linear -> RGBA8 (255, 255, 255, R), level 0
        const uint8_t* src = reinterpret_cast<const uint8_t*>(t.memory);
        uint64_t rows = uint64_t(img.height) * layers;
        if (uint64_t(t.stride) * rows > t.storage_size) rows = t.storage_size / t.stride;
        // Only the rows the CPU wrote (a single layer), when the texture is already there (NX_FULL_UPLOAD=1: all).
        uint64_t y0 = 0, y1 = rows;
        if (was_uploaded && layers == 1 && !full_upload() && hi != UINT64_MAX)
            y0 = std::min(rows, lo / t.stride), y1 = std::min(rows, (hi + t.stride - 1) / t.stride);
        if (y1 <= y0) return;
        Upload u = texture_stage((y1 - y0) * img.width * 4);  // the 2048x8192 glyph atlas: 64 MB at its first upload
        for (uint64_t y = y0; y < y1; y++) {
            const uint8_t* s = src + y * t.stride;
            uint32_t* d = reinterpret_cast<uint32_t*>(u.ptr + (y - y0) * img.width * 4);
            for (uint32_t x = 0; x < img.width; x++) d[x] = 0x00FFFFFFu | (uint32_t(s[x]) << 24);
        }
        VkBufferImageCopy r{};
        r.bufferOffset = u.offset;
        r.imageSubresource = {img.aspect, 0, 0, img.layers};
        r.imageOffset = {0, int32_t(y0), 0};
        r.imageExtent = {img.width, uint32_t(std::min<uint64_t>(y1, img.height) - y0), 1};
        transition(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        p_vkCmdCopyBufferToImage(cb, u.buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
        return;
    }
    bool decode = bc_host_decode(t.format);  // nvn_bc.cpp: no BC on this GPU
    if (decode && t.stride) {
        static bool logged = false;
        if (!logged) log("nvn/vk: pitch-linear BC texture (format %#x): not decoded, cleared", t.format), logged = true;
        return clear_image(cb, img);
    }
    if (t.stride && astc_decoded(t.format)) {  // never seen: pitch-linear ASTC would need the decoder too
        log("nvn/vk: pitch-linear ASTC texture %ux%u format %#x: cleared", img.width, img.height, t.format);
        return clear_image(cb, img);
    }
    if (t.stride) {  // pitch-linear: rows as they are
        uint64_t rows = (img.height + bh - 1) / bh;
        VkBufferImageCopy r{};
        r.imageSubresource = {img.aspect, 0, 0, img.layers};
        r.imageExtent = {img.width, img.height, 1};
        Upload u;
        uint64_t staged = 0;
        if (widen) {  // tightly packed RGBA16F rows; rows past the storage stay zero
            const uint8_t* src = reinterpret_cast<const uint8_t*>(t.memory);
            uint64_t n = rows * layers, have = std::min<uint64_t>(n, t.storage_size / t.stride);
            uint64_t texels = std::min<uint64_t>(img.width, t.stride / 4);
            staged = n * img.width * 8;
            u = texture_stage(staged);
            std::memset(u.ptr, 0, staged);
            for (uint64_t y = 0; y < have; y++) widen_r11g11b10(src + y * t.stride, u.ptr + y * img.width * 8, texels);
        } else {
            uint64_t y0 = 0, y1 = rows;  // only the rows the CPU wrote, for a texture already there
            if (was_uploaded && bw == 1 && bh == 1 && layers == 1 && !full_upload() && hi != UINT64_MAX) {
                y0 = std::min(rows, lo / t.stride), y1 = std::min(rows, (hi + t.stride - 1) / t.stride);
                if (y1 <= y0) return;
                r.imageOffset = {0, int32_t(y0), 0};
                r.imageExtent = {img.width, uint32_t(y1 - y0), 1};
            }
            staged = uint64_t(t.stride) * (y1 - y0) * layers;
            if (y0 * t.stride > t.storage_size || staged > t.storage_size - y0 * t.stride)
                fatal("nvn: pitch-linear texture upload exceeds pool backing");
            const uint8_t* src = backing + y0 * t.stride;
            // Keep the existing staging allocation policy for CPU-only data.
            // Pending GPU writes are copied/composed by written_read in command
            // order; mapped Upload::ptr is not CPU-visible GPU output yet.
            if (!replay::written_read(cb, src, staged, staged, std::max(4u, host_bytes), u)) {
                u = texture_stage(staged);
                std::memcpy(u.ptr, src, staged);
            }
            // NX_REPLAY_CUTS=barrier: u may be a composed copy; make it visible before the upload's copy reads it.
            replay::copy_write_barrier(cb);
            r.bufferRowLength = t.stride / bytes * bw;
        }
        r.bufferOffset = u.offset;
        if (!copies_fit(t, &r, 1, u.offset, staged, bw, bh, host_bytes)) return clear_image(cb, img);
        transition(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        p_vkCmdCopyBufferToImage(cb, u.buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
        return;
    }
    const bool volume = img.view_type == VK_IMAGE_VIEW_TYPE_3D;
    const uint64_t untile_t0 = plat::monotonic_ns();
    auto levels = volume ? deswizzle_volume(backing, t.storage_size, img.width, img.height, img.depth, img.levels,
                                            bw, bh, bytes, linear, t.block_height_gobs, t.block_depth_gobs, t.row_alignment_gobs)
                         : deswizzle_texture(backing, t.storage_size, img.width, img.height, layers, img.levels,
                                             bw, bh, bytes, linear, t.block_height_gobs, t.row_alignment_gobs);
    g_cost_untile += plat::monotonic_ns() - untile_t0;
    if (levels.empty()) {
        // Bisection: the layout the tiler wants against the bytes the game allocated. A tight allocation that comes
        // out smaller than the tiled layout means the block-height or pitch derivation disagrees with NVN, and the
        // texture renders as whatever the image was cleared to.
        TextureLayout want = volume ? block_linear_volume_layout(img.width, img.height, img.depth, img.levels, bw, bh,
                                                                 bytes, t.block_height_gobs, t.block_depth_gobs,
                                                                 t.row_alignment_gobs)
                                    : block_linear_layout(img.width, img.height, layers, img.levels, bw, bh, bytes,
                                                          t.block_height_gobs, t.row_alignment_gobs);
        static int failed = 0;
        if (failed++ < 16)
            log("nvn/vk: texture %ux%u format %#x: could not untile (%llu bytes): levels %u, layers %u, blocks %ux%u "
                "x%u bytes, block height gobs %u, layout wants %llu bytes (%llu linear)", img.width, img.height,
                t.format, (unsigned long long)t.storage_size, img.levels, layers, bw, bh, bytes,
                t.block_height_gobs, (unsigned long long)want.tiled_size, (unsigned long long)want.linear_size);
        return;
    }
    if (decode) {  // every level's blocks -> texels, tightly packed, then copied as an uncompressed image
        uint32_t tb = bc_decoded_bytes(t.format);
        uint64_t total = 0;
        for (const auto& l : levels) total += uint64_t(l.width) * l.height * (volume ? l.depth : img.layers) * tb;
        Upload u = upload_alloc(total, 16);
        uint64_t at = 0;
        for (uint32_t k = 0; k < levels.size(); k++) {
            const uint32_t slices = volume ? levels[k].depth : img.layers;
            bc_decode(t.format, linear.data() + levels[k].offset, levels[k].width, levels[k].height, slices, u.ptr + at);
            VkBufferImageCopy r{};
            r.bufferOffset = u.offset + at;
            r.imageSubresource = {img.aspect, k, 0, img.view_type == VK_IMAGE_VIEW_TYPE_3D ? 1u : img.layers};
            r.imageExtent = {levels[k].width, levels[k].height, volume ? levels[k].depth : 1u};
            copies.push_back(r);
            at += uint64_t(levels[k].width) * levels[k].height * slices * tb;
        }
        transition(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        p_vkCmdCopyBufferToImage(cb, u.buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(copies.size()),
                                 copies.data());
        return;
    }
    uint64_t scale = widen ? 2 : 1;
    uint64_t staged = 0;
    Upload u{};
    if (astc_decoded(t.format)) {  // RGBA8 texels from here on
        size_t decoded_size = 0;
        const uint64_t decode_t0 = plat::monotonic_ns();
        u = decode_astc(t.format, linear, levels, layers, bw, bh, volume, decoded_size);
        g_cost_decode += plat::monotonic_ns() - decode_t0;
        staged = decoded_size;
        bw = bh = 1, bytes = host_bytes = 4;
    } else {
        // Widened, every offset in the linear buffer doubles.
        staged = linear.size() * scale;
        const uint64_t stage_t0 = plat::monotonic_ns();
        u = texture_stage(staged);
        g_cost_stage += plat::monotonic_ns() - stage_t0;
        if (widen)
            widen_r11g11b10(linear.data(), u.ptr, linear.size() / 4);
        else
            std::memcpy(u.ptr, linear.data(), linear.size());
    }
    for (uint32_t k = 0; k < levels.size(); k++) {
        VkBufferImageCopy r{};
        r.bufferOffset = u.offset + levels[k].offset * scale;
        r.imageSubresource = {img.aspect, k, 0, img.view_type == VK_IMAGE_VIEW_TYPE_3D ? 1u : img.layers};
        r.imageExtent = {levels[k].width, levels[k].height, volume ? levels[k].depth : 1u};
        copies.push_back(r);
    }
    if (!copies_fit(t, copies.data(), uint32_t(copies.size()), u.offset, staged, bw, bh, host_bytes))
        return clear_image(cb, img);
    transition(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    p_vkCmdCopyBufferToImage(cb, u.buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(copies.size()),
                             copies.data());
}

}  // namespace nx::nvn
