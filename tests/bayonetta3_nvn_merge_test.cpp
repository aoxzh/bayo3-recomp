// Standalone: c++ -std=c++17 -O2 -march=x86-64-v3 tests/bayonetta3_nvn_merge_test.cpp -o /tmp/b3-nvn-merge-test
// Also compile with -mno-avx2 to exercise the portable kernel fallback.
#include "../runtime/src/nvn/nvn_merge.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <vector>
#if defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace {
namespace kernel = nx::nvn::merge_detail;
constexpr size_t kChunk = 65536;
size_t cases = 0;
void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

// Independent byte-based reference: full eight-byte groups contain two owned
// four-byte words, but the final 0..7 bytes are independently owned bytes.
template <class Dirty>
bool reference(const uint8_t* src, const uint8_t* was, uint8_t* to, size_t n, bool checking, Dirty dirty) {
    bool clean = checking;
    for (size_t at = 0; at < n;) {
        const size_t width = at < (n & ~size_t(7)) ? 4 : 1;
        if (std::memcmp(src + at, was + at, width)) {
            std::memcpy(to + at, src + at, width);
        } else if (checking && std::memcmp(to + at, was + at, width)) {
            clean = false;
            dirty(at);
        }
        at += width;
    }
    return clean;
}

void compare(const uint8_t* src, const uint8_t* was, const uint8_t* guest, size_t n,
             bool checking, size_t alignment = 0, size_t offset = 0) {
    std::vector<uint8_t> actual(n + 64, 0xa5), expected = actual;
    uint8_t* to = actual.data() + alignment;
    std::memcpy(to, guest, n);
    std::memcpy(expected.data() + alignment, guest, n);
    const size_t chunks = (offset + n) / kChunk + 1;
    std::vector<uint8_t> actual_ok(chunks, 1), expected_ok(chunks, 1);
    auto dirty = [&](size_t at) { actual_ok[(offset + at) / kChunk] = 0; };
    const bool clean = checking ? kernel::merge<true>(src, was, to, n, dirty)
                                : kernel::merge<false>(src, was, to, n, dirty);
    const bool ref_clean = reference(src, was, expected.data() + alignment, n, checking,
                                    [&](size_t at) { expected_ok[(offset + at) / kChunk] = 0; });
    check(actual == expected, "merge bytes or surrounding canaries differ");
    check(clean == ref_clean, "merge clean result differs");
    check(actual_ok == expected_ok, "dirty chunk attribution differs");
    std::memcpy(to, guest, n);
    kernel::diff_copy(src, to, n);
    check(std::memcmp(to, src, n) == 0, "diff_copy result differs");
    check(std::all_of(actual.begin(), actual.begin() + alignment, [](uint8_t v) { return v == 0xa5; }),
          "diff_copy prefix canary differs");
    check(std::all_of(actual.begin() + alignment + n, actual.end(), [](uint8_t v) { return v == 0xa5; }),
          "diff_copy suffix canary differs");
    ++cases;
}

void exhaustive_masks() {
    std::array<uint8_t, 32> was{}, src{}, guest{};
    for (unsigned gpu = 0; gpu < 256; ++gpu) {
        src = was;
        for (unsigned lane = 0; lane < 8; ++lane)
            if (gpu & (1u << lane)) src[lane * 4 + lane % 4] = 0x73;
        for (unsigned cpu = 0; cpu < 256; ++cpu) {
            guest = was;
            for (unsigned lane = 0; lane < 8; ++lane)
                if (cpu & (1u << lane)) guest[lane * 4 + (lane + 1) % 4] = 0x91;
            compare(src.data(), was.data(), guest.data(), src.size(), false);
            compare(src.data(), was.data(), guest.data(), src.size(), true);
        }
    }
}

void lengths_and_alignment() {
    std::array<uint8_t, 256> src{}, was{}, guest{};
    for (size_t n = 0; n <= 135; ++n) {
        for (size_t a = 0; a < 32; ++a) {
            const size_t b = (a * 7) % 32, d = (a * 13) % 32;
            for (size_t k = 0; k < n; ++k) {
                was[b + k] = uint8_t(k * 37 + n);
                src[a + k] = was[b + k] ^ ((k % 11) == 0 ? 0x01 : 0);
                guest[d + k] = was[b + k] ^ ((k % 7) == 0 ? 0x80 : 0);
            }
            compare(src.data() + a, was.data() + b, guest.data() + d, n, false, d);
            compare(src.data() + a, was.data() + b, guest.data() + d, n, true, d);
        }
    }
    // Changing only byte zero of a 4..7-byte tail must preserve CPU writes in
    // every other byte of that tail; treating it as a four-byte word is wrong.
    for (size_t n = 4; n <= 39; ++n) {
        was.fill(0); src.fill(0); guest.fill(0x93);
        src[n & ~size_t(7)] = 0x21;
        compare(src.data(), was.data(), guest.data(), n, true, 3);
    }
}

void chunk_edges() {
    std::vector<uint8_t> was(2 * kChunk + 39, 0x41), src = was, guest = was;
    const size_t points[] = {0, 4, 31, kChunk - 4, kChunk, kChunk + 4, 2 * kChunk - 1, 2 * kChunk + 35};
    for (size_t at : points) guest[at] ^= 0x80;
    src[kChunk + 4] ^= 1;  // CPU change in a GPU-owned word must not mark the chunk dirty by itself.
    for (size_t offset : {size_t(0), kChunk, kChunk - 4})
        for (size_t n : {kChunk - 1, kChunk, kChunk + 7, was.size()}) {
            compare(src.data(), was.data(), guest.data(), n, false, 7, offset);
            compare(src.data(), was.data(), guest.data(), n, true, 7, offset);
        }
}

#if defined(__AVX2__)
void changes_after_comparison() {
    // Deterministic CPU interleaving: change guest bytes after the SIMD lane
    // comparison but before its store. No data-racy test thread is required.
    std::array<uint8_t, 32> src{}, was{}, to{}, expected{};
    for (unsigned mask = 0; mask < 256; ++mask) {
        src = was;
        for (unsigned lane = 0; lane < 8; ++lane)
            if (mask & (1u << lane)) src[lane * 4] = 1;
        const __m256i a = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src.data()));
        const __m256i b = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(was.data()));
        const __m256i equal = _mm256_cmpeq_epi32(a, b);
        const unsigned same = unsigned(_mm256_movemask_ps(_mm256_castsi256_ps(equal)));
        to.fill(0x97);  // the CPU writes every lane after comparison
        expected = to;
        for (unsigned lane = 0; lane < 8; ++lane)
            if (mask & (1u << lane)) std::memcpy(expected.data() + lane * 4, src.data() + lane * 4, 4);
        kernel::store_changed(to.data(), src.data(), a, same);
        check(to == expected, "store overwrote a CPU change in an unchanged lane");
        ++cases;
    }
}
#endif

#if defined(__unix__) || defined(__APPLE__)
struct Guarded {
    size_t page = size_t(sysconf(_SC_PAGESIZE));
    uint8_t* map = static_cast<uint8_t*>(mmap(nullptr, 3 * page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    Guarded() {
        check(map != MAP_FAILED, "mmap guard pages");
        check(mprotect(map + page, page, PROT_READ | PROT_WRITE) == 0, "mprotect data page");
    }
    ~Guarded() { munmap(map, 3 * page); }
    uint8_t* begin() { return map + page; }
    uint8_t* end() { return map + 2 * page; }
};
void protected_pages() {
    Guarded src, was, to;
    for (size_t n = 0; n < 96; ++n) {
        for (bool at_end : {false, true}) {
            uint8_t* s = at_end ? src.end() - n : src.begin();
            uint8_t* b = at_end ? was.end() - n : was.begin();
            uint8_t* d = at_end ? to.end() - n : to.begin();
            std::memset(s, 0, n); std::memset(b, 0, n); std::memset(d, 0x47, n);
            for (size_t i = 0; i < n; i += 13) s[i] = 1;
            auto dirty = [](size_t) {};
            kernel::merge<true>(s, b, d, n, dirty);
            kernel::merge<false>(s, b, d, n, dirty);
            kernel::diff_copy(s, d, n);
            check(std::memcmp(s, d, n) == 0, "guarded diff bytes");
            ++cases;
        }
    }
    // Read-only guest words beside writable ones prove that no store at all
    // reaches an equal lane, even when a block crosses a page boundary.
    check(mprotect(to.end(), to.page, PROT_READ | PROT_WRITE) == 0, "enable following page");
    std::array<uint8_t, 32> s{}, b{};
    for (size_t left = 4; left <= 28; left += 4) {
        check(mprotect(to.begin(), to.page, PROT_READ | PROT_WRITE) == 0, "reset readonly page");
        uint8_t* d = to.end() - left;
        std::memset(d, 0, 32); s.fill(0); s[left] = 1;
        check(mprotect(to.begin(), to.page, PROT_READ) == 0, "protect unchanged words");
        kernel::merge<true>(s.data(), b.data(), d, 32, [](size_t) {});
        d[left] = 0;
        kernel::merge<false>(s.data(), b.data(), d, 32, [](size_t) {});
        d[left] = 0;
        kernel::diff_copy(s.data(), d, 32);
        check(d[left] == 1, "selective store missed changed word");
        ++cases;
    }
}
#endif
// Frozen pre-SIMD hot loops for the optional benchmark. The correctness oracle
// above is intentionally independent of both these loops and the new kernels.
bool legacy_merge(const uint8_t* src, const uint8_t* was, uint8_t* to, size_t n, bool checking) {
    bool clean = true;
    auto word8 = [&](size_t at) {
        const uint64_t a = kernel::load64(src + at), b = kernel::load64(was + at);
        if (a == b) {
            if (checking && kernel::load64(to + at) != b) clean = false;
            return;
        }
        if (uint32_t(a) != uint32_t(b)) std::memcpy(to + at, src + at, 4);
        else if (checking && kernel::load32(to + at) != uint32_t(b)) clean = false;
        if (uint32_t(a >> 32) != uint32_t(b >> 32)) std::memcpy(to + at + 4, src + at + 4, 4);
        else if (checking && kernel::load32(to + at + 4) != uint32_t(b >> 32)) clean = false;
    };
    size_t at = 0;
    for (; at + 32 <= n; at += 32) {
        if (std::memcmp(src + at, was + at, 32) == 0) {
            if (checking && std::memcmp(to + at, was + at, 32)) clean = false;
        } else {
            for (size_t k = 0; k < 32; k += 8) word8(at + k);
        }
    }
    for (; at + 8 <= n; at += 8) word8(at);
    for (; at < n; ++at) {
        if (src[at] != was[at]) to[at] = src[at];
        else if (checking && to[at] != was[at]) clean = false;
    }
    return checking && clean;
}

bool legacy_diff(const uint8_t* src, const uint8_t*, uint8_t* to, size_t n) {
    auto word8 = [&](size_t at) {
        const uint64_t a = kernel::load64(src + at), b = kernel::load64(to + at);
        if (a == b) return;
        if (uint32_t(a) != uint32_t(b)) std::memcpy(to + at, src + at, 4);
        if (uint32_t(a >> 32) != uint32_t(b >> 32)) std::memcpy(to + at + 4, src + at + 4, 4);
    };
    size_t at = 0;
    for (; at + 32 <= n; at += 32) {
        if (std::memcmp(src + at, to + at, 32) == 0) continue;
        for (size_t k = 0; k < 32; k += 8) word8(at + k);
    }
    for (; at + 8 <= n; at += 8) word8(at);
    for (; at < n; ++at)
        if (src[at] != to[at]) to[at] = src[at];
    return false;
}

using BenchKernel = bool (*)(const uint8_t*, const uint8_t*, uint8_t*, size_t);
template <bool Checking>
bool old_merge(const uint8_t* s, const uint8_t* w, uint8_t* d, size_t n) {
    return legacy_merge(s, w, d, n, Checking);
}
template <bool Checking>
bool new_merge(const uint8_t* s, const uint8_t* w, uint8_t* d, size_t n) {
    return kernel::merge<Checking>(s, w, d, n, [](size_t) {});
}
bool new_diff(const uint8_t* s, const uint8_t*, uint8_t* d, size_t n) {
    kernel::diff_copy(s, d, n);
    return false;
}

// Run only in a benchmark window agreed with the game-run owner:
//   ./b3-nvn-merge-test --bench [bytes=1048576] [rounds=16]
// Destination reset is outside timing and identical in both arms. This measures
// scan kernels with warm inputs, not helper scheduling, Vulkan maps, or FPS.
void benchmark(size_t bytes, unsigned rounds) {
    check(bytes >= 32 && bytes <= (64u << 20) && rounds && rounds <= 1024, "benchmark bounds");
    const char* patterns[] = {"equal", "sparse1/64", "cluster8/512", "sparse1/8", "half", "all"};
    const char* modes[] = {"diff", "merge", "merge-check"};
    const BenchKernel old[] = {legacy_diff, old_merge<false>, old_merge<true>};
    const BenchKernel candidate[] = {new_diff, new_merge<false>, new_merge<true>};
    std::vector<uint8_t> was(bytes, 0x41), src(bytes), guest(bytes), actual(bytes), expected(bytes);
    uint64_t checksum = 0;
    std::printf("kernel microbenchmark bytes=%zu rounds=%u AVX2=%d; medians, reset excluded, warm inputs\n",
                bytes, rounds, int(kernel::kHasAvx2));
    for (unsigned pattern = 0; pattern < 6; ++pattern) {
        src = was;
        for (size_t word = 0; word < bytes / 4; ++word) {
            const bool change = pattern == 1 ? word % 64 == 0 : pattern == 2 ? word % 512 < 8 :
                                pattern == 3 ? word % 8 == 0 : pattern == 4 ? word % 2 == 0 : pattern == 5;
            if (change) src[word * 4 + word % 4] ^= 0x81;
        }
        for (unsigned mode = 0; mode < 3; ++mode) {
            guest = was;
            // Include occasional CPU-only changes in the checked merge. These
            // must survive in GPU-unchanged words and affect its clean result.
            if (mode == 2)
                for (size_t k = 4; k < bytes; k += 4096) guest[k] ^= 0x20;
            expected = guest;
            const bool expected_clean = old[mode](src.data(), was.data(), expected.data(), bytes);
            std::array<std::vector<double>, 2> samples;
            for (auto& arm : samples) arm.reserve(rounds);
            for (unsigned round = 0; round < rounds; ++round) {
                for (unsigned turn = 0; turn < 2; ++turn) {
                    const unsigned arm = (round + turn) & 1;
                    const BenchKernel fn = arm ? candidate[mode] : old[mode];
                    actual = guest;
                    const auto start = std::chrono::steady_clock::now();
                    const bool clean = fn(src.data(), was.data(), actual.data(), bytes);
                    const auto end = std::chrono::steady_clock::now();
                    samples[arm].push_back(std::chrono::duration<double, std::nano>(end - start).count());
                    check(clean == expected_clean && actual == expected, "benchmark arm differs");
                    checksum += actual[(round * 101) % bytes] + unsigned(clean);
                }
            }
            std::array<double, 2> median{};
            for (unsigned arm = 0; arm < 2; ++arm) {
                std::sort(samples[arm].begin(), samples[arm].end());
                median[arm] = (samples[arm][rounds / 2] + samples[arm][(rounds - 1) / 2]) / 2;
            }
            std::printf("%-11s %-11s legacy %8.3f us  candidate %8.3f us  ratio %.3f\n",
                        modes[mode], patterns[pattern], median[0] / 1000.0,
                        median[1] / 1000.0, median[1] / median[0]);
        }
    }
    std::printf("checksum=%llu (microbenchmark only; no game performance conclusion)\n",
                static_cast<unsigned long long>(checksum));
}
}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc > 1) {
            check(std::strcmp(argv[1], "--bench") == 0 && argc <= 4, "usage: [--bench [bytes] [rounds]]");
            benchmark(argc > 2 ? size_t(std::strtoull(argv[2], nullptr, 0)) : 1u << 20,
                      argc > 3 ? unsigned(std::strtoul(argv[3], nullptr, 0)) : 16);
            return 0;
        }
        exhaustive_masks();
        lengths_and_alignment();
        chunk_edges();
#if defined(__AVX2__)
        changes_after_comparison();
#endif
#if defined(__unix__) || defined(__APPLE__)
        protected_pages();
#endif
        std::printf("bayonetta3_nvn_merge_test: %zu cases passed (AVX2=%d)\n", cases, int(kernel::kHasAvx2));
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "bayonetta3_nvn_merge_test: %s\n", e.what());
        return 1;
    }
}
