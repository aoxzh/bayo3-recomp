// Bit-exact AArch64 TBL lowering for generated code. Included only by TUs that use it.
#pragma once

#include <nx/cpu.h>

#if defined(__SSSE3__) && !defined(NX_TBL_SCALAR)
#include <tmmintrin.h>
#endif

namespace nx {

// TBL always reads 16 bytes from each of 1..4 table registers. The result has
// 8 or 16 lanes; out-of-range unsigned indices and unused high lanes are zero.
// Return by value so destination/index/table register aliases read the old values.
template <unsigned Lanes, class... Tables>
NX_INLINE V128 neon_tbl(const V128& indices, const Tables&... tables) {
    static_assert(Lanes == 8 || Lanes == 16);
    static_assert(sizeof...(Tables) >= 1 && sizeof...(Tables) <= 4);
    static_assert((std::is_same_v<Tables, V128> && ...));
    V128 result{};
#if defined(__SSSE3__) && !defined(NX_TBL_SCALAR)
    __m128i index;
    std::memcpy(&index, &indices, sizeof(index));
    __m128i value = _mm_setzero_si128();
    if constexpr (sizeof...(Tables) == 1) {
        // PSHUFB alone wraps indices 16..127. Unsigned saturation makes every
        // index >=16 set bit 7 while retaining the low nibble of valid indices.
        const __m128i control = _mm_adds_epu8(index, _mm_set1_epi8(0x70));
        auto lookup = [&](const V128& table) {
            __m128i bytes;
            std::memcpy(&bytes, &table, sizeof(bytes));
            value = _mm_shuffle_epi8(bytes, control);
        };
        (lookup(tables), ...);
    } else {
        const __m128i high = _mm_and_si128(index, _mm_set1_epi8(-16));
        unsigned base = 0;
        auto append = [&](const V128& table) {
            __m128i bytes;
            std::memcpy(&bytes, &table, sizeof(bytes));
            const __m128i selected = _mm_cmpeq_epi8(high, _mm_set1_epi8(static_cast<char>(base)));
            value = _mm_or_si128(value, _mm_and_si128(_mm_shuffle_epi8(bytes, index), selected));
            base += 16;
        };
        (append(tables), ...);
    }
    std::memcpy(&result, &value, sizeof(result));
    if constexpr (Lanes == 8) result.u64[1] = 0;
#else
    const V128 table[] = {tables...};
    for (unsigned lane = 0; lane < Lanes; ++lane) {
        const unsigned index = indices.u8[lane];
        if (index < sizeof...(Tables) * 16) result.u8[lane] = table[index / 16].u8[index % 16];
    }
#endif
    return result;
}

}  // namespace nx
