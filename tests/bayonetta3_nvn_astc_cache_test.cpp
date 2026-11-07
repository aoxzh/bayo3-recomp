// CPU-only checks of the production ASTC cache/layout helpers and real decoder.
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <random>
#include <vector>

#include "../runtime/src/nvn/nvn_astc.h"
#include "../runtime/src/nvn/nvn_astc_cache.h"

using namespace nx::nvn;

namespace {
int failures = 0;
void expect(bool good, const char* message) {
    if (!good) std::printf("FAIL: %s\n", message), ++failures;
}

struct Image {
    std::vector<uint8_t> bytes;
    std::vector<LinearLevel> levels;
};

Image image(const AstcDecodeSpec& spec) {
    Image result;
    std::mt19937 rng(29);
    uint32_t width = 13, height = 9, depth = 3;
    for (unsigned mip = 0; mip != 4; ++mip) {
        LinearLevel l;
        l.offset = result.bytes.size() + 16;  // padding is part of the exact input identity
        l.width = width;
        l.height = height;
        l.width_blocks = (width + spec.bw - 1) / spec.bw;
        l.height_blocks = (height + spec.bh - 1) / spec.bh;
        l.layer_size = uint64_t(l.width_blocks) * l.height_blocks * 16;
        l.depth = spec.volume ? depth : 1;
        l.tiled_offset = mip * 512;
        l.block_height_gobs = 4;
        l.tiled_pitch_gobs = 2;
        l.block_depth_gobs = spec.volume ? 2 : 1;
        result.bytes.resize(l.offset + l.layer_size * (spec.volume ? depth : spec.layers), 0xA7);
        for (size_t at = l.offset; at < result.bytes.size(); at += 16) {
            for (size_t byte = 0; byte != 16; ++byte) result.bytes[at + byte] = uint8_t(rng());
            // Include legal void extents, other random encodings, and invalid blocks.
            if ((at / 16) % 3 == 0) {
                std::memset(result.bytes.data() + at, 0xFF, 8);
                result.bytes[at] = 0xFC;
                result.bytes[at + 1] = 0xFD;
            } else if ((at / 16) % 7 == 0) {
                std::memset(result.bytes.data() + at, 0, 16);
            }
        }
        result.levels.push_back(l);
        width = std::max(1u, width / 2);
        height = std::max(1u, height / 2);
        depth = std::max(1u, depth / 2);
    }
    return result;
}

// Independent copy of the pre-cache upload layout/decode, for valid small inputs.
Image reference(const AstcDecodeSpec& spec, const Image& input) {
    Image result;
    result.levels = input.levels;
    uint64_t at = 0;
    for (auto& l : result.levels) {
        l.offset = at;
        l.layer_size = uint64_t(l.width) * l.height * 4;
        l.width_blocks = l.width;
        l.height_blocks = l.height;
        at += (l.layer_size * (spec.volume ? l.depth : spec.layers) + 15) & ~uint64_t(15);
    }
    result.bytes.resize(at);
    for (size_t k = 0; k != input.levels.size(); ++k) {
        const auto& l = input.levels[k];
        astc_decode_image(input.bytes.data() + l.offset, l.width, l.height,
                          spec.volume ? l.depth : spec.layers, spec.bw, spec.bh,
                          spec.format >= 0x87, result.bytes.data() + result.levels[k].offset);
    }
    return result;
}

void parser() {
    expect(astc_cache_budget(nullptr) == 0, "unset disables");
    for (const char* value : {"", "0", "-1", "+64", " 64", "64 ", "64x", "0x40", "257", "999999999999999999999999"})
        expect(astc_cache_budget(value) == 0, "malformed or out-of-range disables");
    expect(astc_cache_budget("1") == size_t(1) << 20, "one MiB accepted");
    expect(astc_cache_budget("64") == size_t(64) << 20, "experiment budget accepted");
    expect(astc_cache_budget("00064") == size_t(64) << 20, "leading zero decimal accepted");
    expect(astc_cache_budget("256") == kAstcCacheMaxBytes, "hard maximum accepted");
    AstcDecodeCache capped(std::numeric_limits<size_t>::max());
    expect(capped.budget() == kAstcCacheMaxBytes && capped.used() == 0, "constructor clamps without preloading");
    expect(AstcDecodeCache::charge_for(SIZE_MAX, 1, 1, 1) == SIZE_MAX, "byte accounting does not overflow");
    expect(AstcDecodeCache::charge_for(1, 1, SIZE_MAX, 1) == SIZE_MAX, "metadata accounting does not overflow");
}

void identity_and_collisions() {
    const AstcDecodeSpec spec{0x79, 4, 4, 2, false};
    const Image input = image(spec), output = reference(spec, input);
    AstcDecodeCache cache(1u << 20);
    constexpr uint64_t collision = 42;
    expect(cache.find(spec, input.bytes, input.levels, collision) == nullptr, "cold lookup misses");
    expect(cache.insert(spec, input.bytes, input.levels, collision, output.bytes, output.levels, 123), "store entry");
    auto changed = input.bytes;
    changed[0] ^= 1;  // unused padding must still be compared
    expect(cache.find(spec, changed, input.levels, collision) == nullptr, "forced hash collision cannot alias changed padding");
    changed = input.bytes;
    changed.back() ^= 1;
    expect(cache.find(spec, changed, input.levels, collision) == nullptr, "entire input tail compared");
    expect(cache.insert(spec, changed, input.levels, collision, output.bytes, output.levels, 456), "colliding entry can coexist");
    const auto* collision_hit = cache.find(spec, changed, input.levels, collision);
    expect(collision_hit && collision_hit->decode_ns == 456, "collision selects exact bytes");
    collision_hit = cache.find(spec, input.bytes, input.levels, collision);
    expect(collision_hit && collision_hit->decode_ns == 123, "original collision entry survives");
    changed.push_back(0);
    expect(cache.find(spec, changed, input.levels, collision) == nullptr, "input length belongs to identity");
    expect(cache.find(spec, input.bytes, input.levels, collision + 1) == nullptr, "hash filters nonmatching entries");

    using SpecChange = void (*)(AstcDecodeSpec&);
    const SpecChange spec_changes[] = {
        [](AstcDecodeSpec& s) { ++s.format; }, [](AstcDecodeSpec& s) { ++s.bw; },
        [](AstcDecodeSpec& s) { ++s.bh; }, [](AstcDecodeSpec& s) { ++s.layers; },
        [](AstcDecodeSpec& s) { s.volume = !s.volume; }};
    for (auto change : spec_changes) {
        auto other = spec;
        change(other);
        expect(cache.find(other, input.bytes, input.levels, collision) == nullptr, "every decode scalar belongs to identity");
    }
    using LevelChange = void (*)(LinearLevel&);
    const LevelChange level_changes[] = {
        [](LinearLevel& l) { ++l.offset; }, [](LinearLevel& l) { ++l.layer_size; },
        [](LinearLevel& l) { ++l.tiled_offset; }, [](LinearLevel& l) { ++l.width; },
        [](LinearLevel& l) { ++l.height; }, [](LinearLevel& l) { ++l.width_blocks; },
        [](LinearLevel& l) { ++l.height_blocks; }, [](LinearLevel& l) { ++l.block_height_gobs; },
        [](LinearLevel& l) { ++l.tiled_pitch_gobs; }, [](LinearLevel& l) { ++l.depth; },
        [](LinearLevel& l) { ++l.block_depth_gobs; }};
    for (auto change : level_changes) {
        auto levels = input.levels;
        change(levels.back());
        expect(cache.find(spec, input.bytes, levels, collision) == nullptr, "every field of every level belongs to identity");
    }
    auto levels = input.levels;
    levels.pop_back();
    expect(cache.find(spec, input.bytes, levels, collision) == nullptr, "mip count belongs to identity");
    const auto* hit = cache.find(spec, input.bytes, input.levels, collision);
    expect(hit && hit->rgba == output.bytes && astc_levels_equal(hit->output_levels, output.levels), "hit retains exact output");
    expect(cache.stats().hit_original_decode_ns == 456 + 123 + 123, "hit timing is explicitly historical original decode time");
    expect(cache.stats().hits == 3, "hit count excludes collisions and metadata mismatches");

    // Mutating caller storage after insertion cannot alter a retained snapshot.
    AstcDecodeCache owned(1u << 20);
    auto src = input, dst = output;
    expect(owned.insert(spec, src.bytes, src.levels, 0, dst.bytes, dst.levels, 1), "snapshot inserted");
    src.bytes[0] ^= 1;
    src.levels[0].width++;
    dst.bytes[0] ^= 1;
    dst.levels[0].width++;
    hit = owned.find(spec, input.bytes, input.levels, 0);
    expect(hit && hit->rgba == output.bytes && astc_levels_equal(hit->output_levels, output.levels), "cache owns input and output snapshots");
}

void bounds_and_lru() {
    const AstcDecodeSpec spec{0x79, 4, 4, 1, false};
    const Image input = image(spec), output = reference(spec, input);
    auto second = input.bytes, third = input.bytes;
    second[0] = 1;
    third[0] = 2;
    AstcDecodeCache two(1u << 20, 2);
    expect(two.insert(spec, input.bytes, input.levels, 1, output.bytes, output.levels, 0), "first LRU insert");
    expect(two.insert(spec, second, input.levels, 2, output.bytes, output.levels, 0), "second LRU insert");
    expect(two.find(spec, input.bytes, input.levels, 1) != nullptr, "touch first LRU entry");
    expect(two.insert(spec, third, input.levels, 3, output.bytes, output.levels, 0), "third LRU insert evicts oldest");
    expect(two.size() == 2 && two.stats().evicted == 1, "entry limit remains bounded");
    expect(two.find(spec, second, input.levels, 2) == nullptr, "least recently used entry evicted");
    expect(two.find(spec, input.bytes, input.levels, 1) != nullptr, "recent hit survives eviction");
    const auto* entry = two.find(spec, third, input.levels, 3);
    const size_t charge = AstcDecodeCache::charge_for(entry->compressed.capacity(), entry->rgba.capacity(),
                                                     entry->levels.capacity(), entry->output_levels.capacity());
    expect(entry->charge == charge && charge > input.bytes.size() + output.bytes.size(), "capacities and metadata charged");
    expect(two.used() == 2 * charge, "retained accounting matches live entries");

    AstcDecodeCache byte_limit(2 * charge);
    for (unsigned i = 0; i != 3; ++i) {
        auto bytes = input.bytes;
        bytes[0] = uint8_t(i);
        expect(byte_limit.insert(spec, bytes, input.levels, i, output.bytes, output.levels, 0), "byte-bounded insert");
        expect(byte_limit.used() <= byte_limit.budget(), "byte cap holds after every insert");
    }
    expect(byte_limit.size() == 2 && byte_limit.stats().evicted == 1, "byte budget evicts without entry cap pressure");
    const size_t minimum_charge = AstcDecodeCache::charge_for(input.bytes.size(), output.bytes.size(),
                                                             input.levels.size(), output.levels.size());
    AstcDecodeCache oversized(minimum_charge - 1), off(0), no_entries(1u << 20, 0);
    for (auto* cache : {&oversized, &off, &no_entries}) {
        expect(!cache->eligible(input.bytes.size(), output.bytes.size(), input.levels.size()), "oversized/off/zero-entry bypass eligibility");
        expect(!cache->insert(spec, input.bytes, input.levels, 0, output.bytes, output.levels, 0), "oversized/off/zero-entry insert bypass");
        expect(cache->used() == 0 && cache->size() == 0, "bypassed entries retain nothing");
    }
    AstcDecodeCache hard_cap(16u << 20, 9999);
    for (unsigned i = 0; i != 257; ++i) {
        auto bytes = input.bytes;
        bytes[0] = uint8_t(i);
        bytes[1] = uint8_t(i >> 8);
        expect(hard_cap.insert(spec, bytes, input.levels, i, output.bytes, output.levels, 0), "hard entry cap insert");
    }
    expect(hard_cap.size() == 256 && hard_cap.stats().evicted == 1, "256-entry hard maximum enforced");
    hard_cap.note_bypass(16, 64);
    hard_cap.note_lookup(11);
    hard_cap.note_insert(22);
    expect(hard_cap.stats().bypassed == 1 && hard_cap.stats().bypassed_bytes == 80 &&
           hard_cap.stats().lookup_ns == 11 && hard_cap.stats().insert_ns == 22, "counters distinguish bypass and cache work");
}

void malformed_layouts() {
    const AstcDecodeSpec spec{0x79, 4, 4, 1, false};
    const Image input = image(spec);
    std::vector<LinearLevel> out;
    size_t size = 0;
    expect(astc_output_layout(spec, input.bytes.size(), input.levels, out, size), "valid layout accepted");
    const auto reject = [&](const AstcDecodeSpec& s, size_t bytes, const std::vector<LinearLevel>& levels) {
        expect(!astc_output_layout(s, bytes, levels, out, size), "malformed layout rejected before lookup");
        expect(out.empty() && size == 0, "invalid layout cannot return a partial output");
    };
    for (unsigned which = 0; which != 5; ++which) {
        auto s = spec;
        if (which == 0) s.bw = 1;
        if (which == 1) s.bh = 13;
        if (which == 2) s.layers = 0;
        if (which == 3) s.bw = 13;
        if (which == 4) s.bh = 1;
        reject(s, input.bytes.size(), input.levels);
    }
    reject(spec, input.bytes.size(), {});
    for (unsigned which = 0; which != 7; ++which) {
        auto levels = input.levels;
        if (which == 0) levels[0].width = 0;
        if (which == 1) levels[0].height = 0;
        if (which == 2) levels[0].layer_size++;
        if (which == 3) levels[0].offset = input.bytes.size() + 1;
        if (which == 4) levels[0].offset = UINT64_MAX;
        if (which == 5) levels[0].width = UINT32_MAX;
        if (which == 6) levels[0].height = UINT32_MAX;
        reject(spec, input.bytes.size(), levels);
    }
    reject(spec, input.bytes.size() - 1, input.levels);
    auto volume = spec;
    volume.volume = true;
    auto levels = input.levels;
    levels[0].depth = 0;
    reject(volume, input.bytes.size(), levels);
    levels = input.levels;
    levels[0].width = UINT32_MAX - 15;
    levels[0].height = UINT32_MAX - 15;
    levels[0].offset = 0;
    levels[0].layer_size = uint64_t(levels[0].width / 4) * (levels[0].height / 4) * 16;
    reject(spec, SIZE_MAX, {levels[0]});  // decoded size overflows despite compressed extent fitting
    levels[0].width = 1u << 30;
    levels[0].height = 1u << 30;
    levels[0].layer_size = uint64_t(1) << 60;
    reject(spec, SIZE_MAX, std::vector<LinearLevel>(4, levels[0]));  // cumulative mip output overflow
    levels[0].width = 1u << 20;
    levels[0].height = 1u << 20;
    levels[0].layer_size = uint64_t(1) << 40;
    auto layers = spec;
    layers.layers = 1u << 22;
    reject(layers, SIZE_MAX, {levels[0]});  // RGBA layer product overflow
}

void real_decode_equivalence() {
    constexpr unsigned footprints[][2] = {{4, 4}, {5, 4}, {5, 5}, {6, 5}, {6, 6}, {8, 5}, {8, 6},
                                          {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};
    for (unsigned fmt = 0; fmt != 14; ++fmt) {
        for (bool srgb : {false, true}) {
            for (bool volume : {false, true}) {
                AstcDecodeSpec spec{(srgb ? 0x87u : 0x79u) + fmt, footprints[fmt][0], footprints[fmt][1], 3, volume};
                const Image input = image(spec), expected = reference(spec, input);
                Image decoded;
                size_t size = 0;
                expect(astc_output_layout(spec, input.bytes.size(), input.levels, decoded.levels, size), "real decode layout valid");
                expect(astc_levels_equal(decoded.levels, expected.levels) && size == expected.bytes.size(), "RGBA layout matches pre-cache behavior");
                decoded.bytes.resize(size);
                for (size_t k = 0; k != input.levels.size(); ++k) {
                    const auto& l = input.levels[k];
                    astc_decode_image(input.bytes.data() + l.offset, l.width, l.height,
                                      volume ? l.depth : spec.layers, spec.bw, spec.bh, srgb,
                                      decoded.bytes.data() + decoded.levels[k].offset);
                }
                expect(decoded.bytes == expected.bytes, "miss decode bytes match original decode");
                AstcDecodeCache cache(1u << 20);
                expect(cache.insert(spec, input.bytes, input.levels, 7, decoded.bytes, decoded.levels, 1), "real output retained");
                const auto* hit = cache.find(spec, input.bytes, input.levels, 7);
                expect(hit && hit->rgba == expected.bytes && astc_levels_equal(hit->output_levels, expected.levels),
                       "hit equals uncached decode: all footprints, linear/sRGB, mips, arrays/volume, invalid blocks");
            }
        }
    }
}
}  // namespace

int main() {
    parser();
    identity_and_collisions();
    bounds_and_lru();
    malformed_layouts();
    real_decode_equivalence();
    if (failures) return std::printf("FAIL %d ASTC cache checks\n", failures), 1;
    std::puts("PASS ASTC cache parser, exact identity/collisions, bounded LRU, malformed layouts and real decode equivalence");
    return 0;
}
