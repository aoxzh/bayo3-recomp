// Differential tests for the ASTC partition optimization. No Vulkan or game is needed.
// The production .cpp is included so its private partition helpers can be tested directly.
#ifndef ASTC_SOURCE
#define ASTC_SOURCE "../runtime/src/nvn/nvn_astc.cpp"
#endif
#include ASTC_SOURCE

#include <array>
#include <barrier>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <random>
#include <string_view>

namespace nx::nvn {
namespace {
struct AstcWorkersTestAccess {
    static unsigned count(const AstcWorkers& pool) { return pool.count_; }
    static std::mutex& use(AstcWorkers& pool) { return pool.use_; }
};
}  // namespace
}  // namespace nx::nvn

#ifndef EXPECTED_ASTC_WORKERS
#define EXPECTED_ASTC_WORKERS 4
#endif

namespace reference {
using namespace nx::nvn;

// Frozen pre-optimization formula, including the three-dimensional terms. Production passes z=0.
uint32_t hash52(uint32_t p) {
    p ^= p >> 15, p -= p << 17, p += p << 7, p += p << 4, p ^= p >> 5, p += p << 16, p ^= p >> 7, p ^= p >> 3;
    p ^= p << 6, p ^= p >> 17;
    return p;
}

int partition_of(uint32_t seed, uint32_t x, uint32_t y, uint32_t z, int count, bool small) {
    if (small) x <<= 1, y <<= 1, z <<= 1;
    seed += uint32_t(count - 1) * 1024;
    uint32_t rnum = hash52(seed);
    uint32_t s[12] = {rnum & 0xF,          (rnum >> 4) & 0xF,  (rnum >> 8) & 0xF,  (rnum >> 12) & 0xF,
                      (rnum >> 16) & 0xF,  (rnum >> 20) & 0xF, (rnum >> 24) & 0xF, (rnum >> 28) & 0xF,
                      (rnum >> 18) & 0xF,  (rnum >> 22) & 0xF, (rnum >> 26) & 0xF, ((rnum >> 30) | (rnum << 2)) & 0xF};
    for (uint32_t& v : s) v *= v;
    int sh1, sh2;
    if (seed & 1) sh1 = seed & 2 ? 4 : 5, sh2 = count == 3 ? 6 : 5;
    else sh1 = count == 3 ? 6 : 5, sh2 = seed & 2 ? 4 : 5;
    int sh3 = seed & 0x10 ? sh1 : sh2;
    for (int k = 0; k < 8; k++) s[k] >>= k & 1 ? sh2 : sh1;
    for (int k = 8; k < 12; k++) s[k] >>= sh3;
    uint32_t a = (s[0] * x + s[1] * y + s[10] * z + (rnum >> 14)) & 0x3F;
    uint32_t b = (s[2] * x + s[3] * y + s[11] * z + (rnum >> 10)) & 0x3F;
    uint32_t c = count < 3 ? 0 : (s[4] * x + s[5] * y + s[8] * z + (rnum >> 6)) & 0x3F;
    uint32_t d = count < 4 ? 0 : (s[6] * x + s[7] * y + s[9] * z + (rnum >> 2)) & 0x3F;
    if (a >= b && a >= c && a >= d) return 0;
    if (b >= c && b >= d) return 1;
    return c >= d ? 2 : 3;
}

// Frozen pre-optimization block decoder. Its unchanged ISE/mode/endpoint helpers are shared
// with production; partition selection and the complete block-level control flow are independent.
bool decode_block(const uint8_t* src, uint32_t bw, uint32_t bh, bool srgb, uint8_t* rgba) {
    Block b;
    std::memcpy(b.w, src, 16);
    const uint32_t mode = b.get(0, 11);
    if ((mode & 0x1FF) == 0x1FC) {
        if (mode & 0x200) return error_block(bw, bh, rgba);
        uint8_t c[4];
        for (int k = 0; k < 4; k++) c[k] = uint8_t(b.get(64 + 16 * k, 16) >> 8);
        for (uint32_t k = 0; k < bw * bh; k++) std::memcpy(rgba + 4 * k, c, 4);
        return true;
    }
    int wx = 0, wy = 0, wrange = 0;
    bool dual = false;
    if (!decode_mode(mode, wx, wy, dual, wrange) || wx > int(bw) || wy > int(bh)) return error_block(bw, bh, rgba);
    const int partitions = int(b.get(11, 2)) + 1, planes = dual ? 2 : 1, nweights = wx * wy * planes;
    const int wbits = ise_bits(nweights, wrange);
    if (nweights > 64 || wbits < 24 || wbits > 96 || (dual && partitions == 4)) return error_block(bw, bh, rgba);
    int below = 128 - wbits, color_start = 17;
    uint32_t cem[4] = {}, seed = 0;
    if (partitions == 1) {
        cem[0] = b.get(13, 4);
    } else {
        seed = b.get(13, 10), color_start = 29;
        uint32_t enc = b.get(23, 6);
        if ((enc & 3) == 0) {
            for (int p = 0; p < partitions; p++) cem[p] = enc >> 2;
        } else {
            const int extra = 3 * partitions - 4;
            below -= extra;
            uint32_t all = enc >> 2 | b.get(below, extra) << 4, base = (enc & 3) - 1;
            for (int p = 0; p < partitions; p++)
                cem[p] = (((all >> p) & 1) + base) << 2 | ((all >> (partitions + 2 * p)) & 3);
        }
    }
    uint32_t ccs = 0;
    if (dual) below -= 2, ccs = b.get(below, 2);
    int ncolors = 0;
    for (int p = 0; p < partitions; p++) ncolors += int((cem[p] >> 2) + 1) * 2;
    if (ncolors > 18 || below < color_start) return error_block(bw, bh, rgba);
    int crange = -1;
    for (int r = 20; r >= 0 && crange < 0; r--)
        if (ise_bits(ncolors, r) <= below - color_start) crange = r;
    if (crange < 4) return error_block(bw, bh, rgba);
    IseValue cv[18];
    ise_decode(b, color_start, ncolors, crange, cv);
    uint8_t colors[18], ep[4][2][4];
    for (int k = 0; k < ncolors; k++) colors[k] = unquantize_color(cv[k], crange);
    for (int p = 0, at = 0; p < partitions; at += int((cem[p] >> 2) + 1) * 2, p++)
        if (!endpoints(cem[p], colors + at, ep[p][0], ep[p][1])) return error_block(bw, bh, rgba);

    const Block rev{{reverse64(b.w[1]), reverse64(b.w[0])}};
    IseValue wv[64];
    ise_decode(rev, 0, nweights, wrange, wv);
    uint8_t w[64];
    for (int k = 0; k < nweights; k++) w[k] = unquantize_weight(wv[k], wrange);

    const int ds = (1024 + int(bw) / 2) / int(bw - 1), dt = (1024 + int(bh) / 2) / int(bh - 1);
    const bool small = bw * bh < 31;
    for (uint32_t t = 0; t < bh; t++)
        for (uint32_t s = 0; s < bw; s++) {
            const int gs = (ds * int(s) * (wx - 1) + 32) >> 6, gt = (dt * int(t) * (wy - 1) + 32) >> 6;
            const int js = std::min(gs >> 4, wx - 1), fs = gs & 15, jt = std::min(gt >> 4, wy - 1), ft = gt & 15;
            const int w11 = (fs * ft + 8) >> 4, w10 = ft - w11, w01 = fs - w11, w00 = 16 - fs - ft + w11;
            const int s1 = std::min(js + 1, wx - 1), t1 = std::min(jt + 1, wy - 1);
            int pw[2];
            for (int pl = 0; pl < planes; pl++) {
                auto at = [&](int x, int y) { return int(w[(y * wx + x) * planes + pl]); };
                pw[pl] = (at(js, jt) * w00 + at(s1, jt) * w01 + at(js, t1) * w10 + at(s1, t1) * w11 + 8) >> 4;
            }
            const int part = partitions > 1 ? reference::partition_of(seed, s, t, 0, partitions, small) : 0;
            uint8_t* o = rgba + 4 * (t * bw + s);
            for (int c = 0; c < 4; c++) {
                const int wt = dual && uint32_t(c) == ccs ? pw[1] : pw[0];
                const uint32_t e0 = ep[part][0][c], e1 = ep[part][1][c];
                const uint32_t c0 = srgb ? e0 << 8 | 0x80 : e0 * 257, c1 = srgb ? e1 << 8 | 0x80 : e1 * 257;
                o[c] = uint8_t(((c0 * uint32_t(64 - wt) + c1 * uint32_t(wt) + 32) >> 6) >> 8);
            }
        }
    return true;
}
}  // namespace reference

namespace {
using BlockBytes = std::array<uint8_t, 16>;
constexpr uint32_t footprints[][2] = {{4, 4}, {5, 4}, {5, 5}, {6, 5}, {6, 6}, {8, 5}, {8, 6},
                                      {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};

[[noreturn]] void fail(const char* what) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::exit(1);
}

void set_bits(BlockBytes& block, unsigned start, unsigned n, uint32_t value) {
    for (unsigned k = 0; k < n; ++k) {
        const unsigned bit = start + k;
        block[bit / 8] = uint8_t((block[bit / 8] & ~(1u << (bit % 8))) | (((value >> k) & 1u) << (bit % 8)));
    }
}

// Choose a legal mode whose CEM 0 endpoints fit. Fixed mode avoids relying on
// random bytes to happen to produce valid 3- and 4-partition blocks.
uint32_t find_mode(uint32_t bw, uint32_t bh, int count, bool dual) {
    for (uint32_t mode = 0; mode < 2048; ++mode) {
        if ((mode & 0x1FF) == 0x1FC) continue;
        int wx = 0, wy = 0, range = 0;
        bool mode_dual = false;
        if (!nx::nvn::decode_mode(mode, wx, wy, mode_dual, range) || mode_dual != dual ||
            wx > int(bw) || wy > int(bh) || (dual && count == 4)) continue;
        const int weights = wx * wy * (dual ? 2 : 1);
        if (weights > 64) continue;
        const int bits = nx::nvn::ise_bits(weights, range);
        const int color_start = count == 1 ? 17 : 29;
        const int below = 128 - bits - (dual ? 2 : 0);
        if (bits >= 24 && bits <= 96 && nx::nvn::ise_bits(2 * count, 4) <= below - color_start) return mode;
    }
    fail("no legal synthetic ASTC mode for footprint/count/plane combination");
}

BlockBytes synthetic(std::mt19937& rng, uint32_t mode, int count, uint32_t seed) {
    BlockBytes block;
    for (auto& b : block) b = uint8_t(rng());
    set_bits(block, 0, 11, mode);
    set_bits(block, 11, 2, uint32_t(count - 1));
    if (count == 1) set_bits(block, 13, 4, 0);  // LDR luminance endpoint mode
    else {
        set_bits(block, 13, 10, seed);
        set_bits(block, 23, 6, 0);  // identical CEM 0; independent endpoints for each partition
    }
    return block;
}

uint64_t partition_checks = 0, block_checks = 0;
uint64_t valid_counts[5] = {}, dual_valid = 0, invalid_count = 0, void_count = 0;

bool compare_block(const BlockBytes& block, uint32_t bw, uint32_t bh, bool srgb) {
    std::array<uint8_t, 12 * 12 * 4 + 16> expected, actual;
    expected.fill(0xA5), actual.fill(0xA5);
    const bool a = reference::decode_block(block.data(), bw, bh, srgb, expected.data());
    const bool b = nx::nvn::astc_decode_block(block.data(), bw, bh, srgb, actual.data());
    ++block_checks;
    if (a != b || expected != actual) {
        std::fprintf(stderr, "FAIL block %ux%u srgb=%d expected-valid=%d actual-valid=%d bytes:", bw, bh, srgb, a, b);
        for (auto v : block) std::fprintf(stderr, " %02x", unsigned(v));
        std::fputc('\n', stderr);
        std::exit(1);
    }
    const uint32_t mode = uint32_t(block[0]) | (uint32_t(block[1] & 7) << 8);
    if ((mode & 0x1FF) == 0x1FC) ++void_count;
    else if (!a) ++invalid_count;
    else {
        ++valid_counts[((block[1] >> 3) & 3) + 1];
        int wx, wy, range;
        bool dual;
        if (nx::nvn::decode_mode(mode, wx, wy, dual, range) && dual) ++dual_valid;
    }
    return a;
}

void exhaustive_partitions() {
    for (const auto& f : footprints) {
        const bool small = f[0] * f[1] < 31;
        for (int count = 2; count <= 4; ++count) {
            for (uint32_t seed = 0; seed < 1024; ++seed) {
                const auto p = nx::nvn::partition_params(seed, count, small);
                for (uint32_t y = 0; y < f[1]; ++y) {
                    for (uint32_t x = 0; x < f[0]; ++x) {
                        const int want = reference::partition_of(seed, x, y, 0, count, small);
                        const int got = nx::nvn::partition_of(p, x, y);
                        ++partition_checks;
                        if (want != got) {
                            std::fprintf(stderr, "FAIL partition %ux%u seed=%u count=%d x=%u y=%u: %d != %d\n",
                                         f[0], f[1], seed, count, x, y, want, got);
                            std::exit(1);
                        }
                    }
                }
            }
        }
    }
}

void block_differential() {
    std::mt19937 rng(0x94325A17);
    for (const auto& f : footprints) {
        for (int count = 1; count <= 4; ++count) {
            for (int plane = 0; plane <= (count == 4 ? 0 : 1); ++plane) {
                const uint32_t mode = find_mode(f[0], f[1], count, plane != 0);
                const uint32_t seeds = count == 1 ? 32 : 1024;
                for (uint32_t seed = 0; seed < seeds; ++seed) {
                    const auto b = synthetic(rng, mode, count, seed);
                    for (bool srgb : {false, true})
                        if (!compare_block(b, f[0], f[1], srgb)) fail("constructed legal block was rejected");
                }
            }
        }
        // Broad coverage of modes, endpoint encodings, invalid data, and partial/truncated ISE groups.
        for (unsigned i = 0; i < 10000; ++i) {
            BlockBytes b;
            for (auto& v : b) v = uint8_t(rng());
            for (bool srgb : {false, true}) compare_block(b, f[0], f[1], srgb);
        }
        BlockBytes b{};
        for (bool srgb : {false, true}) compare_block(b, f[0], f[1], srgb);  // reserved
        b.fill(0xFF), b[0] = 0xFC, b[1] = 0xFD;
        for (bool srgb : {false, true}) compare_block(b, f[0], f[1], srgb);  // LDR void
        b[1] = 0xFF;
        for (bool srgb : {false, true}) compare_block(b, f[0], f[1], srgb);  // HDR void
    }
    for (int count = 1; count <= 4; ++count) if (!valid_counts[count]) fail("missing valid partition class");
    if (!dual_valid || !invalid_count || !void_count) fail("missing dual-plane, invalid, or void coverage");
}

uint64_t reference_image(const uint8_t* blocks, uint32_t width, uint32_t height, uint32_t slices,
                         uint32_t bw, uint32_t bh, bool srgb, uint8_t* rgba) {
    const uint32_t bx = (width + bw - 1) / bw, by = (height + bh - 1) / bh;
    uint64_t bad = 0;
    uint8_t texels[12 * 12 * 4];
    for (uint32_t z = 0; z < slices; ++z) for (uint32_t y = 0; y < by; ++y) for (uint32_t x = 0; x < bx; ++x) {
        const uint64_t block = (uint64_t(z) * by + y) * bx + x;
        bad += !reference::decode_block(blocks + block * 16, bw, bh, srgb, texels);
        const uint32_t w = std::min(bw, width - x * bw), h = std::min(bh, height - y * bh);
        for (uint32_t r = 0; r < h; ++r)
            std::memcpy(rgba + ((uint64_t(z) * height + y * bh + r) * width + x * bw) * 4,
                        texels + r * bw * 4, w * 4);
    }
    return bad;
}

void image_differential(uint32_t width, uint32_t height, uint32_t slices, uint32_t bw, uint32_t bh, bool srgb,
                        bool concurrent = false, std::barrier<>* start = nullptr,
                        nx::nvn::AstcWorkers* workers = nullptr) {
    const uint64_t count = uint64_t((width + bw - 1) / bw) * ((height + bh - 1) / bh) * slices;
    std::vector<uint8_t> blocks(count * 16), expected(uint64_t(width) * height * slices * 4 + 16, 0xA5), actual = expected;
    std::mt19937 rng(0x87D15A21 + width + height + slices);
    uint32_t modes[4];
    for (int k = 0; k < 4; ++k) modes[k] = find_mode(bw, bh, k + 1, false);
    for (uint64_t i = 0; i < count; ++i) {
        auto b = synthetic(rng, modes[i % 4], int(i % 4) + 1, uint32_t(i % 1024));
        if (i % 13 == 0) b.fill(0);  // reserved mode exercises bad-block accounting
        if (i % 17 == 0) b.fill(0xFF), b[0] = 0xFC, b[1] = 0xFD;
        std::memcpy(blocks.data() + i * 16, b.data(), 16);
    }
    const uint64_t a = reference_image(blocks.data(), width, height, slices, bw, bh, srgb, expected.data());
    if (start) start->arrive_and_wait();
    const nx::nvn::AstcImageRows job{blocks.data(), actual.data(), width, height, bw, bh,
        (width + bw - 1) / bw, (height + bh - 1) / bh, uint64_t(slices) * ((height + bh - 1) / bh), srgb};
    const uint64_t b = workers ? workers->run(job) :
        nx::nvn::astc_decode_image(blocks.data(), width, height, slices, bw, bh, srgb, actual.data());
    if (a != b || expected != actual) fail("image bytes, canary, or invalid count differed");
    if (concurrent) {
        std::vector<uint8_t> one(expected.size(), 0xA5), two(expected.size(), 0xA5);
        uint64_t one_bad = 0, two_bad = 0;
        std::thread first([&] { one_bad = nx::nvn::astc_decode_image(blocks.data(), width, height, slices, bw, bh, srgb, one.data()); });
        std::thread second([&] { two_bad = nx::nvn::astc_decode_image(blocks.data(), width, height, slices, bw, bh, srgb, two.data()); });
        first.join();
        second.join();
        if (one_bad != a || two_bad != a || one != expected || two != expected) fail("concurrent image decodes differed");
    }
}

void worker_paths() {
    // Exercise actual worker publication independently of hardware_concurrency and
    // the image threshold. Fault-injection builds expect one successfully started helper.
    static auto* pool = new nx::nvn::AstcWorkers(4);
    if (nx::nvn::AstcWorkersTestAccess::count(*pool) != EXPECTED_ASTC_WORKERS)
        fail("worker creation did not reach the expected team size");
    for (uint32_t r = 0; r < 8; ++r) {
        image_differential(61 + r, 5, 1, 4, 4, r & 1, false, nullptr, pool);  // inactive helpers
        image_differential(63 + r, 39 + r, 3, 4, 4, r & 1, false, nullptr, pool);
    }
    image_differential(61, 1, 1, 4, 4, false, false, nullptr, pool);
    image_differential(0, 0, 1, 4, 4, false, false, nullptr, pool);

    // A separate thread holds the ownership lock throughout the decode, proving
    // that a contending caller completes with its own output instead of waiting.
    std::barrier locked(2), finished(2);
    std::thread holder([&] {
        std::lock_guard guard(nx::nvn::AstcWorkersTestAccess::use(*pool));
        locked.arrive_and_wait();
        finished.arrive_and_wait();
    });
    locked.arrive_and_wait();
    image_differential(67, 41, 2, 4, 4, true, false, nullptr, pool);
    finished.arrive_and_wait();
    holder.join();
}

void images() {
    // Align actual decode calls after preparing data and the reference output.
    std::barrier start(4);
    std::array<std::thread, 4> callers;
    for (uint32_t k = 0; k < callers.size(); ++k) callers[k] = std::thread([&, k] {
        for (uint32_t r = 0; r < 8; ++r)
            image_differential(511 + k * 8, 253 + r, 1 + k % 2, 4, 4, (r & 1) != 0, false, &start);
    });
    for (auto& caller : callers) caller.join();
    for (const auto& f : footprints) for (bool srgb : {false, true})
        image_differential(f[0] * 3 - 1, f[1] * 2 - 1, 3, f[0], f[1], srgb);
    for (bool srgb : {false, true}) {
        image_differential(127, 124, 1, 4, 4, srgb);  // small images stay serial
        image_differential(127, 125, 1, 4, 4, srgb);
        image_differential(32768, 1, 1, 4, 4, srgb);  // one block row
        image_differential(16384, 5, 1, 4, 4, srgb);  // two rows, fewer rows than helpers
        image_differential(511, 252, 1, 4, 4, srgb);  // 8064 blocks, below old threshold
        image_differential(511, 253, 1, 4, 4, srgb, !srgb);  // 8192 blocks, parallel and concurrent
        image_differential(255, 253, 2, 4, 4, srgb);  // 8192 blocks across two slices
        image_differential(193, 129, 4, 6, 5, srgb);  // sliced image with cropped edges
        image_differential(0, 0, 1, 4, 4, srgb);
        image_differential(16, 16, 0, 4, 4, srgb);
    }
}

using Decode = bool (*)(const uint8_t*, uint32_t, uint32_t, bool, uint8_t*);
struct Measure { double ns_per_block, cpu_ns_per_block; uint64_t digest; };

// Frozen image scheduling before the worker pool; both arms use the current block decoder.
uint64_t legacy_image(const uint8_t* blocks, uint32_t width, uint32_t height, uint32_t slices,
                      uint32_t bw, uint32_t bh, bool srgb, uint8_t* rgba) {
    const uint32_t bx = (width + bw - 1) / bw, by = (height + bh - 1) / bh;
    const uint64_t rows = uint64_t(slices) * by;
    auto decode_rows = [&](uint64_t first, uint64_t last) {
        uint64_t bad = 0;
        uint8_t texels[12 * 12 * 4];
        for (uint64_t row = first; row < last; row++) {
            const uint32_t z = uint32_t(row / by), y = uint32_t(row % by);
            for (uint32_t x = 0; x < bx; x++) {
                bad += !nx::nvn::astc_decode_block(blocks + (row * bx + x) * 16, bw, bh, srgb, texels);
                const uint32_t w = std::min(bw, width - x * bw), h = std::min(bh, height - y * bh);
                for (uint32_t r = 0; r < h; r++)
                    std::memcpy(rgba + ((uint64_t(z) * height + y * bh + r) * width + x * bw) * 4,
                                texels + r * bw * 4, w * 4);
            }
        }
        return bad;
    };
    unsigned n = rows * bx >= 8192 ? std::min(6u, std::max(1u, std::thread::hardware_concurrency() / 2)) : 1u;
    if (n <= 1) return decode_rows(0, rows);
    std::vector<std::thread> pool;
    std::vector<uint64_t> bad(n, 0);
    for (unsigned k = 1; k < n; k++)
        pool.emplace_back([&, k] { bad[k] = decode_rows(rows * k / n, rows * (k + 1) / n); });
    bad[0] = decode_rows(0, rows / n);
    for (auto& t : pool) t.join();
    uint64_t sum = 0;
    for (uint64_t b : bad) sum += b;
    return sum;
}

void benchmark_images() {
    std::printf("image scheduling benchmark: hardware threads=%u, workers including caller=%u; synthetic inputs\n",
                std::thread::hardware_concurrency(),
                std::min(6u, std::max(1u, std::thread::hardware_concurrency() / 2)));
    std::mt19937 rng(0x765BBEED);
    for (uint32_t bw : {4u, 6u}) for (bool constant : {false, true}) {
        uint32_t modes[4];
        for (int c = 1; c <= 4; ++c) modes[c - 1] = find_mode(bw, bw, c, false);
        for (uint32_t side : {64u, 128u, 256u, 512u, 1024u}) {
            const size_t count = size_t((side + bw - 1) / bw) * ((side + bw - 1) / bw);
            const unsigned repetitions = unsigned(std::max<size_t>(2, 32768 / count));
            std::vector<uint8_t> blocks(count * 16), out(size_t(side) * side * 4), expected(out.size());
            for (size_t i = 0; i < count; ++i) {
                auto b = synthetic(rng, modes[i % 4], int(i % 4) + 1, uint32_t(i % 1024));
                if (constant) b.fill(0xFF), b[0] = 0xFC, b[1] = 0xFD;
                std::memcpy(blocks.data() + i * 16, b.data(), 16);
            }
            const auto a = legacy_image(blocks.data(), side, side, 1, bw, bw, false, expected.data());
            const auto b = nx::nvn::astc_decode_image(blocks.data(), side, side, 1, bw, bw, false, out.data());
            if (a != b || expected != out) fail("image benchmark output differed");
            auto measure_image = [&](auto decode) {
                uint64_t digest = 0;
                const auto start = std::chrono::steady_clock::now();
                const auto cpu_start = std::clock();
                for (unsigned r = 0; r < repetitions; ++r) {
                    digest += decode(blocks.data(), side, side, 1, bw, bw, false, out.data());
                    digest += out[r % out.size()];
                }
                const auto cpu_end = std::clock();
                const auto end = std::chrono::steady_clock::now();
                return Measure{double(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count()) /
                                   repetitions,
                               double(cpu_end - cpu_start) * 1e9 / CLOCKS_PER_SEC / repetitions, digest};
            };
            std::array<double, 7> before, after, before_cpu, after_cpu;
            for (size_t r = 0; r < before.size(); ++r) {
                Measure old, candidate;
                if (r % 2 == 0) {
                    old = measure_image(legacy_image);
                    candidate = measure_image(nx::nvn::astc_decode_image);
                } else {
                    candidate = measure_image(nx::nvn::astc_decode_image);
                    old = measure_image(legacy_image);
                }
                if (old.digest != candidate.digest) fail("image benchmark checksum differed");
                before[r] = old.ns_per_block / 1e6, after[r] = candidate.ns_per_block / 1e6;
                before_cpu[r] = old.cpu_ns_per_block / 1e6, after_cpu[r] = candidate.cpu_ns_per_block / 1e6;
            }
            std::sort(before.begin(), before.end()), std::sort(after.begin(), after.end());
            std::sort(before_cpu.begin(), before_cpu.end()), std::sort(after_cpu.begin(), after_cpu.end());
            std::printf("%ux%u %ux%u %s %zu blocks: wall %.3f -> %.3f ms/image [%.3f..%.3f -> %.3f..%.3f]; "
                        "CPU %.3f -> %.3f ms/image\n", side, side, bw, bw, constant ? "void" : "mixed", count,
                        before[3], after[3], before[0], before[6], after[0], after[6], before_cpu[3], after_cpu[3]);
            std::fflush(stdout);
        }
    }
}

Measure measure(Decode decode, const std::vector<BlockBytes>& blocks, uint32_t bw, uint32_t bh) {
    constexpr unsigned repetitions = 16;
    std::array<uint8_t, 12 * 12 * 4> out;
    uint64_t digest = 0;
    const auto start = std::chrono::steady_clock::now();
    const std::clock_t cpu_start = std::clock();
    for (unsigned r = 0; r < repetitions; ++r) for (size_t i = 0; i < blocks.size(); ++i) {
        const bool valid = decode(blocks[i].data(), bw, bh, (r & 1) != 0, out.data());
        digest = digest * 1315423911u + out[(i + r) % (bw * bh * 4)] + valid;
    }
    const std::clock_t cpu_end = std::clock();
    const auto end = std::chrono::steady_clock::now();
    const double n = double(repetitions) * blocks.size();
    return {double(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count()) / n,
            double(cpu_end - cpu_start) * 1e9 / CLOCKS_PER_SEC / n, digest};
}

void benchmark() {
    std::mt19937 rng(0x765BBEED);
    for (const auto& f : {std::array<uint32_t, 2>{4, 4}, std::array<uint32_t, 2>{6, 6}}) {
        for (int group = 0; group <= 4; ++group) {  // 0: an even mixture of partition counts
            std::vector<BlockBytes> blocks;
            uint32_t modes[4];
            for (int c = 1; c <= 4; ++c) modes[c - 1] = find_mode(f[0], f[1], c, false);
            for (int i = 0; i < 1024; ++i) {
                const int c = group == 0 ? (i % 4) + 1 : group;
                blocks.push_back(synthetic(rng, modes[c - 1], c, uint32_t(i % 1024)));
            }
            std::array<double, 7> before, after, before_cpu, after_cpu;
            uint64_t digest = 0;
            // Warm both implementations once; alternate sample order to reduce drift bias.
            (void)measure(reference::decode_block, blocks, f[0], f[1]);
            (void)measure(nx::nvn::astc_decode_block, blocks, f[0], f[1]);
            for (size_t round = 0; round < before.size(); ++round) {
                Measure a, b;
                if (round % 2 == 0) {
                    a = measure(reference::decode_block, blocks, f[0], f[1]);
                    b = measure(nx::nvn::astc_decode_block, blocks, f[0], f[1]);
                } else {
                    b = measure(nx::nvn::astc_decode_block, blocks, f[0], f[1]);
                    a = measure(reference::decode_block, blocks, f[0], f[1]);
                }
                if (a.digest != b.digest) fail("benchmark checksum differed");
                before[round] = a.ns_per_block, after[round] = b.ns_per_block;
                before_cpu[round] = a.cpu_ns_per_block, after_cpu[round] = b.cpu_ns_per_block;
                digest = a.digest;
            }
            std::sort(before.begin(), before.end()), std::sort(after.begin(), after.end());
            std::sort(before_cpu.begin(), before_cpu.end()), std::sort(after_cpu.begin(), after_cpu.end());
            std::printf("%ux%u %s%d: median wall %.1f -> %.1f ns/block [%.1f..%.1f -> %.1f..%.1f]; "
                        "CPU %.1f -> %.1f ns/block; digest %llu\n",
                        f[0], f[1], group ? "partitions=" : "mixed=", group,
                        before[3], after[3], before[0], before[6], after[0], after[6],
                        before_cpu[3], after_cpu[3], (unsigned long long)digest);
        }
    }
}
}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--bench") {
        benchmark();
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--bench-images") {
        benchmark_images();
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--workers-only") {
        worker_paths();
        std::puts("PASS: worker publication, invalid counts, inactive helpers, serial contention and team size");
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--images-only") {
        worker_paths();
        images();
        std::puts("PASS: image bytes, invalid counts, clipping, empty images, concurrent callers and worker reuse");
        return 0;
    }
    const bool blocks_only = argc == 2 && std::string_view(argv[1]) == "--blocks-only";
    if (argc > 2 || (argc == 2 && !blocks_only)) fail("unknown option");
    if (!blocks_only) exhaustive_partitions();
    block_differential();
    if (!blocks_only) {
        worker_paths();
        images();
    }
    std::printf("PASS: %llu partition comparisons, %llu block comparisons; valid partitions: %llu/%llu/%llu/%llu; "
                "dual %llu, invalid %llu, void %llu\n",
                (unsigned long long)partition_checks, (unsigned long long)block_checks,
                (unsigned long long)valid_counts[1], (unsigned long long)valid_counts[2],
                (unsigned long long)valid_counts[3], (unsigned long long)valid_counts[4],
                (unsigned long long)dual_valid, (unsigned long long)invalid_count, (unsigned long long)void_count);
    return 0;
}
