// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from Witch-Time commit beffd6bd250d2da359b25a289cbb5bc5bccc4750:
// runtime/game_shim_eh.c (Morton split and quadtree wrapper) and
// runtime/quad_walk.h (homogeneous-node classification). Game-specific guards
// and CPU-state handling below are for the Bayonetta 3 1.2 generated bodies.
#include <nx/b3_guest_fastpaths.h>
#include <nx/cpu.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>

namespace nx::b3 {
namespace {

uint32_t even_bits16(uint32_t v) {
    v &= 0x55555555u;
    v = (v | (v >> 1)) & 0x33333333u;
    v = (v | (v >> 2)) & 0x0f0f0f0fu;
    v = (v | (v >> 4)) & 0x00ff00ffu;
    return (v | (v >> 8)) & 0x0000ffffu;
}

// Half-open address intervals; callers first check that their ends fit.
bool overlaps(uint64_t a, uint64_t a_end, uint64_t b, uint64_t b_end) {
    return a < b_end && b < a_end;
}

}  // namespace

bool try_morton_split(CPU& c) {
    const uint64_t a = c.x[1], b = c.x[2], sp = c.x[31];
    constexpr uint64_t max = std::numeric_limits<uint64_t>::max();
    if (!a || !b || a > max - 4 || b > max - 4 || sp < 32 ||
        overlaps(a, a + 4, b, b + 4) ||
        overlaps(a, a + 4, sp - 32, sp) ||
        overlaps(b, b + 4, sp - 32, sp))
        return false;

    const uint32_t value = uint32_t(c.x[0]);
    const uint32_t even = even_bits16(value);
    const uint32_t odd = even_bits16(value >> 1);
    // The generated body writes the even result before the odd result.
    nx::st<uint32_t>(a, even);
    nx::st<uint32_t>(b, odd);
    c.x[0] = odd;
    c.x[8] = 32;
    c.x[9] = 0x80000000u;
    // Its final flag-setting instruction is cmp w8, #32.
    nx::subs32(c, 32, 32);
    return true;
}

bool try_quad_walk(CPU& c) {
    const int32_t depth = int32_t(uint32_t(c.x[7]));
    const uint64_t sp = c.x[31];
    constexpr uint64_t max = std::numeric_limits<uint64_t>::max();
    if (depth < 2 || depth >= 16 || sp > max - 24 ||
        sp < uint64_t(depth + 1) * 0x2d0 + 0x20)
        return false;

    const uint32_t index = nx::ld<uint32_t>(sp);
    // Each recursive child truncates (index << 2) to 32 bits. Do not use a
    // geometric node whose high index bits would disappear before the leaves.
    if (index >> (32 - 2 * depth))
        return false;
    const uint64_t a = nx::ld<uint64_t>(sp + 8);
    const uint64_t b = nx::ld<uint64_t>(sp + 16);
    const uint32_t width = uint32_t(c.x[1]), height = uint32_t(c.x[2]);
    const uint32_t size = 1u << depth;
    const uint32_t x0 = even_bits16(index) << depth;
    const uint32_t y0 = even_bits16(index >> 1) << depth;
    const uint32_t x_end = x0 + size - 1;
    const uint32_t y_end = y0 + size - 1;

    bool different = false;
    if (x0 <= width && y0 <= height) {
        const uint32_t x1 = std::min(x_end, width);
        const uint32_t y1 = std::min(y_end, height);
        const uint64_t first = uint64_t(y0) * width + x0;
        const uint64_t last = uint64_t(y1) * width + x1;
        // Generated madd w8 wraps at 32 bits. Avoid changing which bytes it
        // observes; also keep all host pointer arithmetic in range.
        if (!a || !b || last > UINT32_MAX || a > max - last - 1 ||
            b > max - last - 1)
            return false;
        const uint64_t stack_begin = sp - (uint64_t(depth + 1) * 0x2d0 + 0x20);
        if (overlaps(a + first, a + last + 1, stack_begin, sp) ||
            overlaps(b + first, b + last + 1, stack_begin, sp))
            return false;

        different = nx::ld<uint8_t>(a + first) != nx::ld<uint8_t>(b + first);
        if (different && (x_end > width || y_end > height))
            return false;
        const size_t len = size_t(x1 - x0) + 1;
        for (uint32_t y = y0; y <= y1; ++y) {
            const uint64_t off = uint64_t(y) * width + x0;
            const auto* left = reinterpret_cast<const uint8_t*>(a + off);
            const auto* right = reinterpret_cast<const uint8_t*>(b + off);
            if (!different) {
                if (std::memcmp(left, right, len) != 0)
                    return false;
            } else {
                for (size_t i = 0; i < len; ++i)
                    if (left[i] == right[i]) return false;
            }
        }
    }

    c.x[0] = different ? 1 : 0;
    // The generated epilogue restores d8..d15 via zero-extending loads.
    for (int i = 8; i <= 15; ++i) c.v[i].u64[1] = 0;
    if (different) {
        const uint64_t last = uint64_t(y_end) * width + x_end;
        nx::subs32(c, nx::ld<uint8_t>(a + last), nx::ld<uint8_t>(b + last));
    } else {
        // All-zero nodes finish their draw loop at cmp x25, #3.
        nx::subs64(c, 3, 3);
    }
    return true;
}

}  // namespace nx::b3
