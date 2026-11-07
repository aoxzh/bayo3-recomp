// The optional AVX2 write-back kernels. These compare this submit's bytes only;
// they do not establish guest-memory validity for a later bind or submit.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(__AVX2__)
#include <immintrin.h>
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#endif

namespace nx::nvn::merge_detail {

#if defined(__AVX2__)
inline constexpr bool kHasAvx2 = true;
#else
inline constexpr bool kHasAvx2 = false;
#endif

inline uint64_t load64(const uint8_t* p) {
    uint64_t value;
    std::memcpy(&value, p, sizeof(value));
    return value;
}

inline uint32_t load32(const uint8_t* p) {
    uint32_t value;
    std::memcpy(&value, p, sizeof(value));
    return value;
}

#if defined(__AVX2__)
// Unchanged lanes must not be written, even with their current guest value:
// a CPU writer may change one between our comparison and the store. Only an
// entirely changed block can use storeu. Partial blocks use the already computed
// lane mask to select scalar stores, without comparing the words again. The
// earlier vpmaskmovd variant regressed sparse writes in the Deck microbenchmark;
// this variant remains opt-in until independently measured in the same protocol.
inline void store_changed(uint8_t* to, const uint8_t* source, __m256i src, unsigned same) {
    if (same == 0xff) return;
    if (same == 0) {
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(to), src);
    } else {
        unsigned changed = same ^ 0xffu;
        do {
#if defined(_MSC_VER)
            unsigned long lane;
            _BitScanForward(&lane, changed);
#else
            const unsigned lane = unsigned(__builtin_ctz(changed));
#endif
            std::memcpy(to + lane * 4, source + lane * 4, 4);
            changed &= changed - 1;
        } while (changed);
    }
}
#endif

// Complete eight-byte groups have two independently owned four-byte words.
// The final 0..7 bytes retain the old bytewise semantics, including a 4..7-byte
// tail: rounding that tail into another word would overwrite CPU-owned bytes.
// Dirty receives a byte offset within the slice for each detected CPU change.
// Source and snapshot must be stable, disjoint from the writable destination.
template <bool CheckClean, class Dirty>
inline bool merge(const uint8_t* src, const uint8_t* was, uint8_t* to, size_t count, Dirty dirty) {
    bool clean = CheckClean;
    size_t at = 0;
#if defined(__AVX2__)
    for (; count - at >= 32; at += 32) {
        const __m256i a = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + at));
        const __m256i b = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(was + at));
        const __m256i equal = _mm256_cmpeq_epi32(a, b);
        const unsigned same = unsigned(_mm256_movemask_ps(_mm256_castsi256_ps(equal)));
        if constexpr (CheckClean) {
            if (same) {
                const __m256i guest = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(to + at));
                const unsigned guest_same = unsigned(_mm256_movemask_ps(
                    _mm256_castsi256_ps(_mm256_cmpeq_epi32(guest, b))));
                const unsigned cpu_changed = same & ~guest_same;
                if (cpu_changed) {
                    clean = false;
                    for (unsigned lane = 0; lane < 8; ++lane)
                        if (cpu_changed & (1u << lane)) dirty(at + lane * 4);
                }
            }
        }
        store_changed(to + at, src + at, a, same);
    }
#endif
    for (; count - at >= 8; at += 8) {
        const uint64_t a = load64(src + at), b = load64(was + at);
        for (unsigned half = 0; half < 2; ++half) {
            const uint32_t value = uint32_t(a >> (half * 32));
            const uint32_t before = uint32_t(b >> (half * 32));
            const size_t word = at + half * 4;
            if (value != before) {
                std::memcpy(to + word, src + word, 4);
            } else if constexpr (CheckClean) {
                if (load32(to + word) != before) {
                    clean = false;
                    dirty(word);
                }
            }
        }
    }
    for (; at < count; ++at) {
        if (src[at] != was[at]) {
            to[at] = src[at];
        } else if constexpr (CheckClean) {
            if (to[at] != was[at]) {
                clean = false;
                dirty(at);
            }
        }
    }
    return clean;
}

// Preserve diff_copy's existing comparison against current guest bytes. This
// kernel adds no new users of that path and makes no assertion that a previous
// clean write-back proves that guest memory is still unchanged.
inline void diff_copy(const uint8_t* src, uint8_t* to, size_t count) {
    size_t at = 0;
#if defined(__AVX2__)
    for (; count - at >= 32; at += 32) {
        const __m256i a = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + at));
        const __m256i b = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(to + at));
        const __m256i equal = _mm256_cmpeq_epi32(a, b);
        const unsigned same = unsigned(_mm256_movemask_ps(_mm256_castsi256_ps(equal)));
        store_changed(to + at, src + at, a, same);
    }
#endif
    for (; count - at >= 8; at += 8) {
        const uint64_t a = load64(src + at), b = load64(to + at);
        if (uint32_t(a) != uint32_t(b)) std::memcpy(to + at, src + at, 4);
        if (uint32_t(a >> 32) != uint32_t(b >> 32)) std::memcpy(to + at + 4, src + at + 4, 4);
    }
    for (; at < count; ++at)
        if (src[at] != to[at]) to[at] = src[at];
}

}  // namespace nx::nvn::merge_detail
