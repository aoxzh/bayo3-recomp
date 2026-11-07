// Bounded CPU results of ASTC decoding. No Vulkan, guest addresses, or image ownership.
// Call astc_output_layout before lookup: malformed input must fail even on a cache hit.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <list>
#include <utility>
#include <vector>

#include "nvn_tiling.h"

namespace nx::nvn {

inline constexpr size_t kAstcCacheMaxBytes = size_t(256) << 20;
inline constexpr size_t kAstcCacheMaxEntries = 256;

// Strict decimal MiB; unset, zero, malformed, and out-of-range settings disable the cache.
inline size_t astc_cache_budget(const char* value) {
    if (!value || !*value) return 0;
    unsigned mb = 0;
    for (const char* p = value; *p; ++p) {
        if (*p < '0' || *p > '9') return 0;
        mb = mb * 10 + unsigned(*p - '0');
        if (mb > 256) return 0;
    }
    return size_t(mb) << 20;
}

struct AstcDecodeSpec {
    uint32_t format = 0, bw = 0, bh = 0, layers = 0;
    bool volume = false;
    bool operator==(const AstcDecodeSpec&) const = default;
};

inline bool astc_level_equal(const LinearLevel& a, const LinearLevel& b) {
    // Compare fields, never struct padding. Preserve all input/output layout metadata.
    return a.offset == b.offset && a.layer_size == b.layer_size && a.tiled_offset == b.tiled_offset &&
           a.width == b.width && a.height == b.height && a.width_blocks == b.width_blocks &&
           a.height_blocks == b.height_blocks && a.block_height_gobs == b.block_height_gobs &&
           a.tiled_pitch_gobs == b.tiled_pitch_gobs && a.depth == b.depth &&
           a.block_depth_gobs == b.block_depth_gobs;
}

inline bool astc_levels_equal(const std::vector<LinearLevel>& a, const std::vector<LinearLevel>& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), astc_level_equal);
}

// Validate the compressed extent independently of the format table, then build exactly the
// RGBA8 layout used by uploads. Checked arithmetic also prevents overflow before allocation.
inline bool astc_output_layout(const AstcDecodeSpec& spec, size_t input_bytes,
                               const std::vector<LinearLevel>& levels, std::vector<LinearLevel>& out,
                               size_t& output_bytes) {
    out.clear();
    output_bytes = 0;
    if (spec.bw < 4 || spec.bw > 12 || spec.bh < 4 || spec.bh > 12 || !spec.layers || levels.empty())
        return false;
    const uint64_t max = std::numeric_limits<size_t>::max();
    uint64_t at = 0;
    for (const auto& l : levels) {
        const uint64_t slices = spec.volume ? l.depth : spec.layers;
        // astc_decode_image uses uint32_t ceiling division for block dimensions.
        if (!l.width || !l.height || !slices || l.width > UINT32_MAX - (spec.bw - 1) ||
            l.height > UINT32_MAX - (spec.bh - 1)) return false;
        const uint64_t cols = (uint64_t(l.width) + spec.bw - 1) / spec.bw;
        const uint64_t rows = (uint64_t(l.height) + spec.bh - 1) / spec.bh;
        if (cols > UINT64_MAX / rows / 16) return false;
        const uint64_t need = cols * rows * 16;
        if (need != l.layer_size || l.offset > input_bytes || need > (input_bytes - l.offset) / slices)
            return false;
        const uint64_t pixels = uint64_t(l.width) * l.height;
        if (pixels > (max - 15) / 4 / slices) return false;
        const uint64_t padded = (pixels * 4 * slices + 15) & ~uint64_t(15);
        if (padded > max - at) return false;
        at += padded;
    }
    out = levels;
    at = 0;
    for (auto& l : out) {
        l.offset = at;
        l.layer_size = uint64_t(l.width) * l.height * 4;
        l.width_blocks = l.width;
        l.height_blocks = l.height;
        at += (l.layer_size * (spec.volume ? l.depth : spec.layers) + 15) & ~uint64_t(15);
    }
    output_bytes = size_t(at);
    return true;
}

class AstcDecodeCache {
public:
    struct Entry {
        AstcDecodeSpec spec;
        uint64_t hash = 0;
        std::vector<uint8_t> compressed, rgba;
        std::vector<LinearLevel> levels, output_levels;
        size_t charge = 0;
        uint64_t decode_ns = 0;
    };
    struct Stats {
        uint64_t hits = 0, misses = 0, evicted = 0, bypassed = 0;
        uint64_t hit_bytes = 0, bypassed_bytes = 0;
        uint64_t lookup_ns = 0, insert_ns = 0;
        // Sum of original decode durations for hit entries, not measured time saved.
        uint64_t hit_original_decode_ns = 0;
    };

    explicit AstcDecodeCache(size_t bytes, size_t entries = kAstcCacheMaxEntries)
        : budget_(std::min(bytes, kAstcCacheMaxBytes)), limit_(std::min(entries, kAstcCacheMaxEntries)) {}

    size_t budget() const { return budget_; }
    size_t used() const { return used_; }
    size_t size() const { return entries_.size(); }
    const Stats& stats() const { return stats_; }
    void note_lookup(uint64_t ns) { stats_.lookup_ns += ns; }
    void note_insert(uint64_t ns) { stats_.insert_ns += ns; }
    void note_bypass(size_t input, size_t output) {
        stats_.bypassed++;
        stats_.bypassed_bytes += uint64_t(input) + output;
    }

    // Vectors' payloads plus the entry, list links, and conservative allocator overhead.
    // The decoder's current working buffers are separate, as they were without caching.
    static size_t charge_for(size_t input, size_t output, size_t input_levels, size_t output_levels) {
        size_t n = sizeof(Entry) + 128;
        const size_t max = std::numeric_limits<size_t>::max();
        for (size_t bytes : {input, output}) {
            if (bytes > max - n) return max;
            n += bytes;
        }
        for (size_t count : {input_levels, output_levels}) {
            if (count > (max - n) / sizeof(LinearLevel)) return max;
            n += count * sizeof(LinearLevel);
        }
        return n;
    }
    bool eligible(size_t input, size_t output, size_t levels) const {
        return budget_ && limit_ && charge_for(input, output, levels, levels) <= budget_;
    }

    // hash is only a filter. The caller supplies the existing four-lane hash; tests
    // inject collisions. At most 256 entries are scanned, without a second allocation
    // or an unaccounted hash-table bucket array. Returned entry survives until insert.
    const Entry* find(const AstcDecodeSpec& spec, const std::vector<uint8_t>& compressed,
                      const std::vector<LinearLevel>& levels, uint64_t hash) {
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            if (it->hash != hash || !(it->spec == spec) || !astc_levels_equal(it->levels, levels) ||
                it->compressed != compressed) continue;
            stats_.hits++;
            stats_.hit_bytes += it->rgba.size();
            stats_.hit_original_decode_ns += it->decode_ns;
            entries_.splice(entries_.begin(), entries_, it);
            return &entries_.front();
        }
        stats_.misses++;
        return nullptr;
    }

    bool insert(const AstcDecodeSpec& spec, const std::vector<uint8_t>& compressed,
                const std::vector<LinearLevel>& levels, uint64_t hash,
                const std::vector<uint8_t>& rgba, const std::vector<LinearLevel>& output_levels,
                uint64_t decode_ns) {
        return insert(spec, compressed, levels, hash, rgba.data(), rgba.size(), output_levels, decode_ns);
    }

    // The upload mapping may be recycled while this cache entry is still live.
    bool insert(const AstcDecodeSpec& spec, const std::vector<uint8_t>& compressed,
                const std::vector<LinearLevel>& levels, uint64_t hash,
                const uint8_t* rgba, size_t rgba_size, const std::vector<LinearLevel>& output_levels,
                uint64_t decode_ns) {
        const size_t estimate = charge_for(compressed.size(), rgba_size, levels.size(), output_levels.size());
        if (!budget_ || !limit_ || estimate > budget_) return false;
        make_room(estimate);
        std::vector<uint8_t> owned_rgba;
        if (rgba_size) owned_rgba.assign(rgba, rgba + rgba_size);
        Entry entry{spec, hash, compressed, std::move(owned_rgba), levels, output_levels, 0, decode_ns};
        // Account actual capacities even if this standard library overallocates copies.
        entry.charge = charge_for(entry.compressed.capacity(), entry.rgba.capacity(),
                                  entry.levels.capacity(), entry.output_levels.capacity());
        if (entry.charge > budget_) return false;
        make_room(entry.charge);
        entries_.push_front(std::move(entry));
        used_ += entries_.front().charge;
        return true;
    }

private:
    void make_room(size_t charge) {
        while (!entries_.empty() && (entries_.size() >= limit_ || charge > budget_ - used_)) {
            used_ -= entries_.back().charge;
            entries_.pop_back();
            stats_.evicted++;
        }
    }
    size_t budget_ = 0, limit_ = 0, used_ = 0;
    std::list<Entry> entries_;  // most recently used first
    Stats stats_;
};

}  // namespace nx::nvn
