// Host-only integration tests of the production ASTC texture upload path.
// Vulkan, guest tiling, and the upload allocator are mocked; nvn_astc.cpp is real.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <random>
#include <stdexcept>
#include <vector>

#include "../runtime/src/nvn/nvn_astc.h"
namespace nx::nvn {
uint64_t observed_astc_decode_image(const uint8_t*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, bool, uint8_t*);
}
#define astc_decode_image observed_astc_decode_image
#include "../runtime/src/nvn/nvn_upload.cpp"
#undef astc_decode_image

using namespace nx::nvn;

namespace {
struct Image { std::vector<uint8_t> bytes; std::vector<LinearLevel> levels; };
struct Allocation { std::vector<uint8_t> bytes; size_t size = 0; bool cached = false, staging = false; };
std::vector<std::unique_ptr<Allocation>> allocations;
Allocation* current = nullptr;
Image input;
AstcDecodeSpec input_spec;
std::vector<VkBufferImageCopy> regions;
std::vector<uint8_t> bytes_at_copy;
std::function<void()> on_alloc;
unsigned decodes = 0, direct_decodes = 0, fallback_decodes = 0, copies = 0;
unsigned ring_allocs = 0, staging_allocs = 0, fatal_count = 0;
bool ring_cached = true, staging_cached = true, mock_astc = true;
constexpr size_t guard = 16;
constexpr uint64_t buffer_offset = 32;

void expect(bool value, const char* what) {
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", what); throw std::runtime_error(what); }
}

Upload allocate(uint64_t size, bool staging) {
    if (on_alloc) { auto callback = std::move(on_alloc); on_alloc = {}; callback(); }
    auto allocation = std::make_unique<Allocation>();
    allocation->size = size_t(size);
    allocation->cached = staging ? staging_cached : ring_cached;
    allocation->staging = staging;
    allocation->bytes.assign(size_t(size) + guard * 2, 0xCD);
    current = allocation.get();
    allocations.push_back(std::move(allocation));
    if (staging) ++staging_allocs; else ++ring_allocs;
    Upload u{};
    u.buffer = reinterpret_cast<VkBuffer>(uintptr_t(0x1234));
    u.offset = buffer_offset;
    u.ptr = current->bytes.data() + guard;
    u.host_cached = current->cached;
    return u;
}

bool guards_intact(const Allocation& a) {
    return std::all_of(a.bytes.begin(), a.bytes.begin() + guard, [](uint8_t b) { return b == 0xCD; }) &&
           std::all_of(a.bytes.end() - guard, a.bytes.end(), [](uint8_t b) { return b == 0xCD; });
}

Image make_image(const AstcDecodeSpec& spec, uint32_t width, uint32_t height, uint32_t depth, unsigned mips) {
    Image image;
    std::mt19937 rng(29);
    for (unsigned mip = 0; mip != mips; ++mip) {
        LinearLevel l;
        l.offset = image.bytes.size() + 16;
        l.width = width; l.height = height; l.depth = spec.volume ? depth : 1;
        l.width_blocks = (width + spec.bw - 1) / spec.bw;
        l.height_blocks = (height + spec.bh - 1) / spec.bh;
        l.layer_size = uint64_t(l.width_blocks) * l.height_blocks * 16;
        l.tiled_offset = mip * 512;
        l.block_height_gobs = 4; l.tiled_pitch_gobs = 2; l.block_depth_gobs = spec.volume ? 2 : 1;
        image.bytes.resize(l.offset + l.layer_size * (spec.volume ? depth : spec.layers), 0xA7);
        for (size_t at = l.offset; at < image.bytes.size(); at += 16) {
            for (size_t i = 0; i != 16; ++i) image.bytes[at + i] = uint8_t(rng());
            // Mix valid void-extent blocks and invalid/random blocks, including cropped edges.
            if ((at / 16) % 3 == 0) {
                std::memset(image.bytes.data() + at, 0xFF, 8);
                image.bytes[at] = 0xFC; image.bytes[at + 1] = 0xFD;
            } else if ((at / 16) % 7 == 0) std::memset(image.bytes.data() + at, 0, 16);
        }
        image.levels.push_back(l);
        width = std::max(1u, width / 2); height = std::max(1u, height / 2); depth = std::max(1u, depth / 2);
    }
    return image;
}

// Independent reconstruction of the former vector layout and real decoder calls.
Image reference(const AstcDecodeSpec& spec, const Image& src) {
    Image dst; dst.levels = src.levels;
    uint64_t at = 0;
    for (auto& l : dst.levels) {
        l.offset = at; l.layer_size = uint64_t(l.width) * l.height * 4;
        l.width_blocks = l.width; l.height_blocks = l.height;
        at += (l.layer_size * (spec.volume ? l.depth : spec.layers) + 15) & ~uint64_t(15);
    }
    dst.bytes.resize(size_t(at));
    for (size_t i = 0; i != src.levels.size(); ++i) {
        const auto& l = src.levels[i];
        astc_decode_image(src.bytes.data() + l.offset, l.width, l.height,
                          spec.volume ? l.depth : spec.layers, spec.bw, spec.bh,
                          spec.format >= 0x87, dst.bytes.data() + dst.levels[i].offset);
    }
    return dst;
}

bool behind = false;  // async_gpu(): the decode goes to the lane, and the copy is recorded ahead of it
void upload(const AstcDecodeSpec& spec, const Image& src, const Image& expected, bool expect_decode, bool expect_direct,
            unsigned nested_decodes = 0) {
    input_spec = spec; input = src;
    Texture t{}; t.format = spec.format; t.depth = spec.layers; t.width = src.levels[0].width;
    t.height = src.levels[0].height; t.memory = reinterpret_cast<uint64_t>(input.bytes.data());
    t.storage_size = input.bytes.size();
    GpuImage img{}; img.format = VK_FORMAT_R8G8B8A8_UNORM; img.width = t.width; img.height = t.height;
    img.depth = spec.volume ? src.levels[0].depth : 1; img.layers = spec.volume ? 1 : spec.layers;
    img.levels = uint32_t(src.levels.size());
    img.view_type = spec.volume ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    const unsigned d = decodes, direct = direct_decodes, fallback = fallback_decodes, ncopy = copies;
    regions.clear(); bytes_at_copy.clear();
    upload_texture(VK_NULL_HANDLE, t, img, 0, UINT64_MAX);
    if (behind) replay::copy_behind::wait_all();  // what the part holding the image copy does before it is queued
    expect(copies == ncopy + 1, "one buffer-to-image command");
    expect(decodes == d + nested_decodes + (expect_decode ? src.levels.size() : 0), "expected hit/miss decode count");
    expect(direct_decodes == direct + (expect_direct ? nested_decodes : 0) + (expect_decode && expect_direct ? src.levels.size() : 0), "decoder targets cached mapping only");
    expect(fallback_decodes == fallback + (!expect_direct ? nested_decodes : 0) + (expect_decode && !expect_direct ? src.levels.size() : 0), "uncached/disabled decode uses vector");
    expect(current && current->size == expected.bytes.size() && guards_intact(*current), "allocation size and guards");
    expect(std::equal(expected.bytes.begin(), expected.bytes.end(), current->bytes.begin() + guard), "decoded texels and zero padding");
    if (!behind) expect(bytes_at_copy == expected.bytes, "all decoder stores complete before recording the image copy");
    expect(regions.size() == expected.levels.size(), "one copy per mip");
    for (size_t i = 0; i != regions.size(); ++i) {
        const auto& r = regions[i]; const auto& l = expected.levels[i];
        expect(r.bufferOffset == buffer_offset + l.offset, "copy uses returned offset plus mip offset");
        expect(r.imageSubresource.mipLevel == i && r.imageSubresource.layerCount == (spec.volume ? 1u : spec.layers), "copy mip/layers");
        expect(r.imageExtent.width == l.width && r.imageExtent.height == l.height &&
               r.imageExtent.depth == (spec.volume ? l.depth : 1u), "copy extents and volume depth");
    }
}
}  // namespace

namespace nx {
void log(const char*, ...) {}
[[noreturn]] void fatal(const char*, ...) { ++fatal_count; throw std::runtime_error("expected fatal"); }
}
namespace nx::plat {
uint64_t monotonic_ns() {
    return uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
}
}
namespace nx::nvn {
Texture::~Texture() = default;
uint64_t observed_astc_decode_image(const uint8_t* blocks, uint32_t w, uint32_t h, uint32_t slices,
                                    uint32_t bw, uint32_t bh, bool srgb, uint8_t* out) {
    ++decodes;
    const uintptr_t p = reinterpret_cast<uintptr_t>(out);
    const uintptr_t start = reinterpret_cast<uintptr_t>(current->bytes.data() + guard);
    if (p >= start && p < start + current->size) ++direct_decodes; else ++fallback_decodes;
    return astc_decode_image(blocks, w, h, slices, bw, bh, srgb, out);
}
Upload upload_alloc(uint64_t size, uint64_t) { return allocate(size, false); }
Upload staging_alloc(uint64_t size) { return allocate(size, true); }
bool async_gpu() { return behind; }
uint64_t frame_upload_used() { return 0; }
uint64_t frame_ring_size() { return 0; }
void note_astc_decoded(uint64_t) {}
bool gpu_profile_on() { return false; }
void gpu_profile_mark(VkCommandBuffer, const std::string&) {}
void format_block(uint32_t, uint32_t& bw, uint32_t& bh, uint32_t& bytes) { bw = input_spec.bw; bh = input_spec.bh; bytes = mock_astc ? 16 : 4; }
uint32_t vk_block_bytes(VkFormat) { return 4; }
bool astc_decoded(uint32_t) { return mock_astc; }
bool bc_host_decode(uint32_t) { return false; }
uint32_t bc_decoded_bytes(uint32_t) { return 0; }
void bc_decode(uint32_t, const uint8_t*, uint32_t, uint32_t, uint32_t, uint8_t*) { throw std::runtime_error("BC called"); }
void transition(VkCommandBuffer, GpuImage& img, VkImageLayout layout) { img.layout = layout; }
TextureLayout block_linear_layout(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) { return {}; }
TextureLayout block_linear_volume_layout(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) { return {}; }
std::vector<LinearLevel> deswizzle_texture(const uint8_t*, uint64_t, uint32_t, uint32_t, uint32_t, uint32_t,
                                          uint32_t, uint32_t, uint32_t, std::vector<uint8_t>& out, uint32_t, uint32_t) {
    out = input.bytes; return input.levels;
}
std::vector<LinearLevel> deswizzle_volume(const uint8_t*, uint64_t, uint32_t, uint32_t, uint32_t, uint32_t,
                                         uint32_t, uint32_t, uint32_t, std::vector<uint8_t>& out, uint32_t, uint32_t, uint32_t) {
    out = input.bytes; return input.levels;
}
namespace replay {
bool written_overlaps(const uint8_t*, uint64_t) { return false; }
uint64_t written_texture_version(const uint8_t*, uint64_t) { return 0; }
void require_host_readable(const uint8_t*, uint64_t, const char*) {}
bool written_read(VkCommandBuffer, const uint8_t*, uint64_t, uint64_t, uint64_t, Upload&) { return false; }
void copy_write_barrier(VkCommandBuffer) {}
}
}
namespace nx::vk {
VKAPI_ATTR void VKAPI_CALL mock_copy(VkCommandBuffer, VkBuffer, VkImage, VkImageLayout,
                                      uint32_t n, const VkBufferImageCopy* r) {
    ++copies; regions.assign(r, r + n);
    bytes_at_copy.assign(current->bytes.begin() + guard, current->bytes.end() - guard);
}
VKAPI_ATTR void VKAPI_CALL mock_clear_color(VkCommandBuffer, VkImage, VkImageLayout, const VkClearColorValue*,
                                             uint32_t, const VkImageSubresourceRange*) { throw std::runtime_error("unexpected clear"); }
VKAPI_ATTR void VKAPI_CALL mock_clear_depth(VkCommandBuffer, VkImage, VkImageLayout, const VkClearDepthStencilValue*,
                                             uint32_t, const VkImageSubresourceRange*) { throw std::runtime_error("unexpected clear"); }
PFN_vkCmdCopyBufferToImage p_vkCmdCopyBufferToImage = mock_copy;
PFN_vkCmdClearColorImage p_vkCmdClearColorImage = mock_clear_color;
PFN_vkCmdClearDepthStencilImage p_vkCmdClearDepthStencilImage = mock_clear_depth;
}

int main(int argc, char** argv) {
    try {
        if (argc > 1 && std::strcmp(argv[1], "behind") == 0) {
            behind = true;
            const AstcDecodeSpec spec{0x87, 5, 4, 3, false}, plain{0x79, 4, 4, 1, false};
            const Image array = make_image(spec, 13, 9, 1, 4), array_ref = reference(spec, array);
            upload(spec, array, array_ref, true, true);
            ring_cached = false;  // through the decoder's own buffer, then the copy
            upload(spec, array, array_ref, true, false);
            ring_cached = true;
            const Image big = make_image(plain, 516, 512, 1, 1), big_ref = reference(plain, big);
            upload(plain, big, big_ref, true, true);
            std::puts("nvn_astc_upload_test: decode behind the replay passed");
            return 0;
        }
        const bool disabled = argc > 1 && std::strcmp(argv[1], "disabled") == 0;
        const bool cache_enabled = !(argc > 1 && std::strcmp(argv[1], "no-cache") == 0);
        const AstcDecodeSpec array_spec{0x87, 5, 4, 3, false};
        const Image array = make_image(array_spec, 13, 9, 1, 4), array_ref = reference(array_spec, array);
        upload(array_spec, array, array_ref, true, !disabled);
        expect(!current->staging && ring_allocs, "small upload uses ring");
        // Recycling the old mapped upload must not corrupt the retained cache entry.
        std::fill(current->bytes.begin() + guard, current->bytes.end() - guard, 0xEE);
        upload(array_spec, array, array_ref, !cache_enabled, !disabled);

        const AstcDecodeSpec volume_spec{0x79, 4, 4, 1, true};
        const Image volume = make_image(volume_spec, 11, 7, 3, 3), volume_ref = reference(volume_spec, volume);
        ring_cached = false;
        upload(volume_spec, volume, volume_ref, true, false);
        ring_cached = true;
        std::fill(current->bytes.begin() + guard, current->bytes.end() - guard, 0xEE);
        upload(volume_spec, volume, volume_ref, !cache_enabled, !disabled);

        // More than 8192 blocks triggers the real worker path; the staging mock
        // copies immediately when recording, so unfinished stores would fail.
        const AstcDecodeSpec big_spec{0x79, 4, 4, 1, false};
        const Image big = make_image(big_spec, 516, 512, 1, 1), big_ref = reference(big_spec, big);
        staging_cached = true;
        upload(big_spec, big, big_ref, true, !disabled);
        expect(current->staging && staging_allocs, "large upload uses staging");
        staging_cached = false;
        upload(big_spec, big, big_ref, true, false);  // 1 MiB cache cannot retain this fixture

        // No allocator/cache lookup occurs when bounds are invalid.
        auto invalid = array.levels; invalid[0].offset = array.bytes.size() + 1;
        size_t staged = 0; const unsigned before = ring_allocs + staging_allocs;
        auto blocks = array.bytes;
        try { decode_astc(array_spec.format, blocks, invalid, array_spec.layers, array_spec.bw, array_spec.bh, false, staged); }
        catch (const std::runtime_error&) {}
        expect(fatal_count && ring_allocs + staging_allocs == before, "invalid input rejected before allocation");

        // Pointer-based cache insertion owns its bytes and collision-checks full input.
        AstcDecodeCache cache(1u << 20);
        auto scratch = array_ref.bytes; auto changed = array.bytes; changed.back() ^= 1;
        expect(cache.insert(array_spec, array.bytes, array.levels, 42, scratch.data(), scratch.size(), array_ref.levels, 1), "pointer cache insert");
        std::fill(scratch.begin(), scratch.end(), 0xEE);
        expect(!cache.find(array_spec, changed, array.levels, 42), "same hash cannot alias changed compressed bytes");
        const auto* hit = cache.find(array_spec, array.bytes, array.levels, 42);
        expect(hit && hit->rgba == array_ref.bytes, "cache owns mapped input after recycle");

        // Simulate an allocator that retires work and causes 257 nested inserts.
        // The original entry is evicted before allocation returns, so the outer
        // upload must do its own lookup afterwards and decode instead of dereferencing a stale hit.
        on_alloc = [&] {
            const AstcDecodeSpec tiny_spec{0x79, 4, 4, 1, false};
            auto tiny = make_image(tiny_spec, 1, 1, 1, 1);
            for (unsigned i = 0; i != 257; ++i) {
                tiny.bytes[0] = uint8_t(i); tiny.bytes[1] = uint8_t(i >> 8);
                auto levels = tiny.levels; size_t n = 0;
                decode_astc(tiny_spec.format, tiny.bytes, levels, 1, 4, 4, false, n);
            }
        };
        const unsigned outer_before = decodes;
        upload(array_spec, array, array_ref, true, !disabled, 257);
        expect(decodes >= outer_before + 257 + array.levels.size(), "allocator reentry evicts safely before cache lookup");

        // The adjacent uncompressed staging branch still copies its input once.
        mock_astc = false;
        const AstcDecodeSpec raw_spec{0x19, 1, 1, 1, false};
        Image raw; raw.bytes = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
        LinearLevel raw_level{};
        raw_level.width = raw_level.height = raw_level.width_blocks = raw_level.height_blocks = 2;
        raw_level.layer_size = raw.bytes.size(); raw.levels.push_back(raw_level);
        upload(raw_spec, raw, raw, false, false);
        std::puts("ASTC upload integration passed");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ASTC upload test: %s\n", e.what()); return 1;
    }
}
