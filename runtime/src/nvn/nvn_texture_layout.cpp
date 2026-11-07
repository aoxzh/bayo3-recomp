// Texture storage layout (docs/NVN.md, N4): how many bytes of a memory pool an NVN texture occupies, and how it is
// aligned. The SDK driver computes this through its device's surface-layout object, which only its own
// nvnDeviceInitialize (talking to the GPU kernel driver) sets up, so we compute it here: Maxwell block-linear.
//
// Format facts come from the driver's own format table (B3 sdk+0xcd0d90; SDK 16.2 +0xd34198; 0x30 bytes per format):
//   word 2: block width (byte 0), block height (byte 1); word 4: bytes per block (byte 0).
// Block linear: the image is cut into GOBs of 64 bytes x 8 rows; GOBs stack vertically into blocks of
// 1..16 GOBs (the block height, picked per mip level from the level's height); a level's rows of blocks are
// 64-byte aligned in width and block-aligned in height. Pre-tiled data (BNTX) is laid out the same way, so the
// sizes must not come out smaller than the real driver's.
#include <algorithm>

#include "nvn.h"
#include "nvn_tiling.h"

namespace nx::nvn {

namespace {

#if defined(NX_BAYONETTA3)
// B3 SDK's R_AARCH64_RELATIVE at +0xce8020 points here. Its texture
// layout evaluator (+0x4b6c3c..+0x4b6c78) indexes 0x30-byte records and
// reads block width/height at +8/+9 and bytes per block at +0x10.
constexpr uint64_t kFormatTable = 0xcd0d90;
#else
constexpr uint64_t kFormatTable = 0xd34198;  // in sdk (SDK 16.2)
#endif
constexpr uint32_t kFormatCount = 0xd0;

struct FormatInfo {
    uint32_t block_w = 1, block_h = 1, bytes = 4;
};

FormatInfo format_info(uint32_t format) {
    static uint64_t sdk = [] {
        uint64_t loader = find_symbol("nvnBootstrapLoader");
        const LoadedModule* m = module_at(loader);
        return m ? m->base : 0;
    }();
    FormatInfo f;
    if (!sdk || format >= kFormatCount) return f;
    const uint32_t* e = reinterpret_cast<const uint32_t*>(sdk + kFormatTable + uint64_t(format) * 0x30);
    uint32_t bw = e[2] & 0xFF, bh = (e[2] >> 8) & 0xFF, bytes = e[4] & 0xFF;
    if (bw && bh && bytes) f = {bw, bh, bytes};
    return f;
}

uint32_t div_up(uint32_t a, uint32_t b) { return (a + b - 1) / b; }

// Block height in GOBs for a level `rows` block rows tall: the smallest power of two >= rows / 8, at most 16.
uint32_t block_height_gobs(uint32_t rows) {
    uint32_t gobs = div_up(rows, 8), h = 1;
    while (h < gobs && h < 16) h <<= 1;
    return h;
}

struct Layout {
    uint64_t size = 0, alignment = 512;
};

Layout compute(CPU& c, uint64_t builder) {
    uint32_t format = uint32_t(driver(c, "nvnTextureBuilderGetFormat", {builder}));
    uint32_t width = std::max<uint32_t>(1, uint32_t(driver(c, "nvnTextureBuilderGetWidth", {builder})));
    uint32_t height = std::max<uint32_t>(1, uint32_t(driver(c, "nvnTextureBuilderGetHeight", {builder})));
    uint32_t depth = std::max<uint32_t>(1, uint32_t(driver(c, "nvnTextureBuilderGetDepth", {builder})));
    // A cube map's six faces are layers of one image. The builder may report depth 1 for it (the faces are implied by
    // the target), while nvn_image.cpp raises the image's layer count to 6 -- so a size computed from the raw depth
    // came out six times too small, and the texture could not be untiled into its own storage (a 64x64 BC6H cube with
    // 7 levels: 8192 bytes allocated against 48640 wanted, so it rendered as whatever the image was cleared to).
    if (uint32_t(driver(c, "nvnTextureBuilderGetTarget", {builder})) == 8) depth = std::max(depth, 6u);
    uint32_t levels = std::max<uint32_t>(1, uint32_t(driver(c, "nvnTextureBuilderGetLevels", {builder})));
    uint32_t samples = std::max<uint32_t>(1, uint32_t(driver(c, "nvnTextureBuilderGetSamples", {builder})));
    uint32_t stride = uint32_t(driver(c, "nvnTextureBuilderGetStride", {builder}));
    FormatInfo f = format_info(format);
    Layout l;
    if (stride) {  // linear (pitch) texture
        l.size = uint64_t(stride) * div_up(height, f.block_h) * depth;
        l.alignment = 0x100;
        return l;
    }
#ifdef NX_BAYONETTA3
    {
        const uint32_t target = uint32_t(driver(c, "nvnTextureBuilderGetTarget", {builder}));
        uint64_t output = scratch(c, 4); st<uint32_t>(output, 0);
        const bool packaged = driver(c, "nvnTextureBuilderGetPackagedTextureLayout", {builder, output}) != 0;
        const uint32_t packed = ld<uint32_t>(output);
        if (packaged && ((packed & ~0xFFFu) || (packed & 15) > 5 || ((packed >> 4) & 15) > 5 ||
                         ((packed >> 8) & 15) > 5 || (target != 2 && (packed & 0xF0))))
            fatal("nvn: unsupported packaged layout %#x", packed);
        const uint32_t row_alignment = packaged ? 1u << ((packed >> 8) & 15) : 1;
        const uint32_t bh = packaged ? 1u << (packed & 15) : default_block_height_gobs(div_up(height, f.block_h));
        uint32_t bd = packaged ? 1u << ((packed >> 4) & 15) : 1;
        if (target == 2) {
            if (!packaged) while (bd < depth && bd < 16) bd <<= 1;
            auto volume = block_linear_volume_layout(width, height, depth, levels, f.block_w, f.block_h, f.bytes, bh, bd, row_alignment);
            l.size = volume.tiled_size * samples;
            l.alignment = 512ull * bh * bd;
            return l;
        }
        if (packaged) {
            auto array = block_linear_layout(width, height, depth, levels, f.block_w, f.block_h, f.bytes, bh, row_alignment);
            // Storage allocations include the last layer's trailing block padding.
            uint64_t chain = array.tiled_size;
            l.size = ((chain + 512ull * bh - 1) / (512ull * bh) * (512ull * bh)) * samples;
            l.alignment = 512ull * bh;
            return l;
        }
    }
#endif
    uint64_t layer = 0;
    uint32_t bh0 = block_height_gobs(div_up(height, f.block_h));
    for (uint32_t level = 0; level < levels; level++) {
        uint32_t w = std::max(1u, width >> level), h = std::max(1u, height >> level);
        uint32_t cols = div_up(w, f.block_w), rows = div_up(h, f.block_h);
        uint32_t bh = std::min(bh0, block_height_gobs(rows));
        uint64_t pitch = (uint64_t(cols) * f.bytes + 63) & ~63ull;
        uint64_t tall = (uint64_t(rows) + 8 * bh - 1) / (8 * bh) * (8 * bh);
        layer += pitch * tall;
    }
    uint64_t block = 512ull * bh0;  // one block: 64 bytes x 8 rows x block height
    layer = (layer + block - 1) / block * block;
    l.size = layer * depth * samples;
    l.alignment = block;
    return l;
}

void GetStorageSize(CPU& c) { c.x[0] = compute(c, c.x[0]).size; }            // size_t (const NVNtextureBuilder*)
void GetStorageAlignment(CPU& c) { c.x[0] = compute(c, c.x[0]).alignment; }  // size_t (const NVNtextureBuilder*)
void GetStorageClass(CPU& c) { c.x[0] = 0; }  // NVNstorageClass: only matters for compression / sparse tiles

}  // namespace

uint64_t texture_storage_size(CPU& c, uint64_t builder) { return compute(c, builder).size; }

void format_block(uint32_t format, uint32_t& block_w, uint32_t& block_h, uint32_t& bytes) {
    FormatInfo f = format_info(format);
    block_w = f.block_w, block_h = f.block_h, bytes = f.bytes;
}

NX_HLE("nvnTextureBuilderGetStorageSize", GetStorageSize);
NX_HLE("nvnTextureBuilderGetStorageAlignment", GetStorageAlignment);
NX_HLE("nvnTextureBuilderGetStorageClass", GetStorageClass);
NX_HLE("nvnTextureBuilderGetRawStorageClass", GetStorageClass);

}  // namespace nx::nvn
