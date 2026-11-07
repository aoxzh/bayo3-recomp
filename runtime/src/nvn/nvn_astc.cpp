// ASTC LDR decoder (nvn_astc.h), written from the Khronos Data Format Specification, "ASTC Compressed Texture Image
// Formats": block mode, partitions, colour endpoint modes, integer sequence encoding, weight infill, void extent.
// HDR endpoint modes and HDR void extents decode to the error colour: Wonder's textures are LDR.
#include "nvn_astc.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <system_error>
#include <thread>
#include <vector>

#if defined(__SSE4_1__)
#include <smmintrin.h>
#endif

namespace nx::nvn {

namespace {

struct Block {
    uint64_t w[2];
    // Bits [pos, pos + n), n <= 32, LSB first.
    uint32_t get(int pos, int n) const {
        if (n <= 0) return 0;
        uint64_t v = pos >= 64 ? w[1] >> (pos - 64) : pos + n <= 64 ? w[0] >> pos : (w[0] >> pos) | (w[1] << (64 - pos));
        return uint32_t(v & ((1ull << n) - 1));
    }
};

uint64_t reverse64(uint64_t v) {
    v = ((v >> 1) & 0x5555555555555555ull) | ((v & 0x5555555555555555ull) << 1);
    v = ((v >> 2) & 0x3333333333333333ull) | ((v & 0x3333333333333333ull) << 2);
    v = ((v >> 4) & 0x0F0F0F0F0F0F0F0Full) | ((v & 0x0F0F0F0F0F0F0F0Full) << 4);
    v = ((v >> 8) & 0x00FF00FF00FF00FFull) | ((v & 0x00FF00FF00FF00FFull) << 8);
    v = ((v >> 16) & 0x0000FFFF0000FFFFull) | ((v & 0x0000FFFF0000FFFFull) << 16);
    return (v >> 32) | (v << 32);
}

// The 21 quantization ranges, in order: levels = (3 if trits, 5 if quints, else 1) << bits.
struct Range {
    uint8_t trits, quints, bits;
};
constexpr Range kRanges[21] = {{0, 0, 1}, {1, 0, 0}, {0, 0, 2}, {0, 1, 0}, {1, 0, 1}, {0, 0, 3}, {0, 1, 1},
                               {1, 0, 2}, {0, 0, 4}, {0, 1, 2}, {1, 0, 3}, {0, 0, 5}, {0, 1, 3}, {1, 0, 4},
                               {0, 0, 6}, {0, 1, 4}, {1, 0, 5}, {0, 0, 7}, {0, 1, 5}, {1, 0, 6}, {0, 0, 8}};

int ise_bits(int count, int r) {
    const Range& q = kRanges[r];
    return count * q.bits + (q.trits ? (8 * count + 4) / 5 : q.quints ? (7 * count + 2) / 3 : 0);
}

struct IseValue {
    uint8_t digit = 0;  // the trit or quint
    uint32_t low = 0;   // the plain bits below it
};

// Reads bits of one encoded sequence; bits past its end read as zero (a truncated last trit/quint block).
struct Reader {
    const Block& b;
    int pos, end;
    uint32_t read(int n) {
        int have = std::max(0, std::min(n, end - pos));
        uint32_t v = b.get(pos, have);
        pos += n;
        return v;
    }
};

void trits_of(uint32_t t, uint8_t* out) {
    auto bit = [](uint32_t v, int i) { return (v >> i) & 1u; };
    uint32_t c, t3, t4;
    if (((t >> 2) & 7) == 7) {
        c = ((t >> 5) & 7) << 2 | (t & 3), t4 = 2, t3 = 2;
    } else {
        c = t & 0x1F;
        if (((t >> 5) & 3) == 3) t4 = 2, t3 = bit(t, 7);
        else t4 = bit(t, 7), t3 = (t >> 5) & 3;
    }
    uint32_t t0, t1, t2;
    if ((c & 3) == 3) t2 = 2, t1 = bit(c, 4), t0 = bit(c, 3) << 1 | (bit(c, 2) & ~bit(c, 3) & 1u);
    else if (((c >> 2) & 3) == 3) t2 = 2, t1 = 2, t0 = c & 3;
    else t2 = bit(c, 4), t1 = (c >> 2) & 3, t0 = bit(c, 1) << 1 | (bit(c, 0) & ~bit(c, 1) & 1u);
    out[0] = uint8_t(t0), out[1] = uint8_t(t1), out[2] = uint8_t(t2), out[3] = uint8_t(t3), out[4] = uint8_t(t4);
}

void quints_of(uint32_t q, uint8_t* out) {
    auto bit = [](uint32_t v, int i) { return (v >> i) & 1u; };
    uint32_t q0, q1, q2;
    if (((q >> 1) & 3) == 3 && ((q >> 5) & 3) == 0) {
        q2 = bit(q, 0) << 2 | (bit(q, 4) & ~bit(q, 0) & 1u) << 1 | (bit(q, 3) & ~bit(q, 0) & 1u), q1 = 4, q0 = 4;
    } else {
        uint32_t c;
        if (((q >> 1) & 3) == 3) q2 = 4, c = ((q >> 3) & 3) << 3 | ((~q >> 5) & 3) << 1 | (q & 1);
        else q2 = (q >> 5) & 3, c = q & 0x1F;
        if ((c & 7) == 5) q1 = 4, q0 = (c >> 3) & 3;
        else q1 = (c >> 3) & 3, q0 = c & 7;
    }
    out[0] = uint8_t(q0), out[1] = uint8_t(q1), out[2] = uint8_t(q2);
}

struct DigitTables {
    uint8_t trits[256][5], quints[128][3];
    DigitTables() {
        for (uint32_t t = 0; t < 256; t++) trits_of(t, trits[t]);
        for (uint32_t q = 0; q < 128; q++) quints_of(q, quints[q]);
    }
};

void ise_decode(const Block& b, int pos, int count, int r, IseValue* out) {
    static const DigitTables digits;
    const Range& q = kRanges[r];
    const int m = q.bits;
    Reader rd{b, pos, pos + ise_bits(count, r)};
    if (q.trits) {
        for (int i = 0; i < count; i += 5) {
            uint32_t low[5], t = 0;
            low[0] = rd.read(m), t |= rd.read(2);
            low[1] = rd.read(m), t |= rd.read(2) << 2;
            low[2] = rd.read(m), t |= rd.read(1) << 4;
            low[3] = rd.read(m), t |= rd.read(2) << 5;
            low[4] = rd.read(m), t |= rd.read(1) << 7;
            const uint8_t* d = digits.trits[t];
            for (int j = 0; j < 5 && i + j < count; j++) out[i + j] = {d[j], low[j]};
        }
    } else if (q.quints) {
        for (int i = 0; i < count; i += 3) {
            uint32_t low[3], v = 0;
            low[0] = rd.read(m), v |= rd.read(3);
            low[1] = rd.read(m), v |= rd.read(2) << 3;
            low[2] = rd.read(m), v |= rd.read(2) << 5;
            const uint8_t* d = digits.quints[v];
            for (int j = 0; j < 3 && i + j < count; j++) out[i + j] = {d[j], low[j]};
        }
    } else {
        for (int i = 0; i < count; i++) out[i] = {0, rd.read(m)};
    }
}

uint32_t replicate(uint32_t v, int from, int to) {
    if (!from) return 0;
    uint32_t r = 0;
    for (int n = 0; n < to;) {
        int take = std::min(from, to - n);
        r = r << take | v >> (from - take);
        n += take;
    }
    return r;
}

uint8_t unquantize_color(const IseValue& v, int r) {
    const Range& q = kRanges[r];
    const int m = q.bits;
    if (!q.trits && !q.quints) return uint8_t(replicate(v.low, m, 8));
    const uint32_t a = v.low & 1 ? 0x1FF : 0, bits = v.low >> 1;
    uint32_t b = 0, c = 0;
    if (q.trits) {
        static constexpr uint32_t kC[7] = {0, 204, 93, 44, 22, 11, 5};
        c = kC[m];
        if (m == 2) b = (bits & 1) * 0x116;  // b000b0bb0
        else if (m == 3) b = bits << 7 | bits << 2 | bits;
        else if (m == 4) b = bits << 6 | bits;
        else if (m == 5) b = bits << 5 | bits >> 2;
        else if (m == 6) b = bits << 4 | bits >> 4;
    } else {
        static constexpr uint32_t kC[6] = {0, 113, 54, 26, 13, 6};
        c = kC[m];
        if (m == 2) b = (bits & 1) * 0x10C;  // b0000bb00
        else if (m == 3) b = bits << 7 | bits << 1 | bits >> 1;
        else if (m == 4) b = bits << 6 | bits >> 1;
        else if (m == 5) b = bits << 5 | bits >> 3;
    }
    uint32_t t = (v.digit * c + b) ^ a;
    return uint8_t((a & 0x80) | (t >> 2));
}

uint8_t unquantize_weight(const IseValue& v, int r) {
    const Range& q = kRanges[r];
    const int m = q.bits;
    uint32_t w;
    if (!q.trits && !q.quints) {
        w = replicate(v.low, m, 6);
    } else if (m == 0) {
        static constexpr uint8_t kT[3] = {0, 32, 63}, kQ[5] = {0, 16, 32, 47, 63};
        w = q.trits ? kT[v.digit] : kQ[v.digit];
    } else {
        const uint32_t a = v.low & 1 ? 0x7F : 0, bits = v.low >> 1;
        uint32_t b = 0, c;
        if (q.trits) {
            c = m == 1 ? 50 : m == 2 ? 23 : 11;
            if (m == 2) b = (bits & 1) * 0x45;  // b000b0b
            else if (m == 3) b = bits << 5 | bits;
        } else {
            c = m == 1 ? 28 : 13;
            if (m == 2) b = (bits & 1) * 0x42;  // b0000b0
        }
        uint32_t t = (v.digit * c + b) ^ a;
        w = (a & 0x20) | (t >> 2);
    }
    return uint8_t(w > 32 ? w + 1 : w);
}

bool decode_mode(uint32_t mode, int& x, int& y, bool& dual, int& range) {
    uint32_t r = (mode >> 4) & 1, h = (mode >> 9) & 1, d = (mode >> 10) & 1, a = (mode >> 5) & 3;
    if (mode & 3) {
        r |= (mode & 3) << 1;
        uint32_t b = (mode >> 7) & 3;
        switch ((mode >> 2) & 3) {
            case 0: x = int(b + 4), y = int(a + 2); break;
            case 1: x = int(b + 8), y = int(a + 2); break;
            case 2: x = int(a + 2), y = int(b + 8); break;
            default:
                b &= 1;
                if (mode & 0x100) x = int(b + 2), y = int(a + 2);
                else x = int(a + 2), y = int(b + 6);
        }
    } else {
        if (((mode >> 2) & 3) == 0) return false;
        r |= ((mode >> 2) & 3) << 1;
        uint32_t b = (mode >> 9) & 3;
        switch ((mode >> 7) & 3) {
            case 0: x = 12, y = int(a + 2); break;
            case 1: x = int(a + 2), y = 12; break;
            case 2: x = int(a + 6), y = int(b + 6), d = 0, h = 0; break;
            default:
                switch ((mode >> 5) & 3) {
                    case 0: x = 6, y = 10; break;
                    case 1: x = 10, y = 6; break;
                    default: return false;
                }
        }
    }
    dual = d != 0;
    range = int(r) - 2 + 6 * int(h);
    return r >= 2 && range <= 11;
}

uint8_t clamp8(int v) { return uint8_t(std::clamp(v, 0, 255)); }

// bit_transfer_signed: `a` becomes a signed 6-bit offset, `b` the base with a's top bit.
void transfer(int& a, int& b) {
    b = (b >> 1) | (a & 0x80);
    a = (a >> 1) & 0x3F;
    if (a & 0x20) a -= 0x40;
}

void set(uint8_t* e, int r, int g, int b, int a) { e[0] = clamp8(r), e[1] = clamp8(g), e[2] = clamp8(b), e[3] = clamp8(a); }
void blue_contract(uint8_t* e, int r, int g, int b, int a) { set(e, (r + b) >> 1, (g + b) >> 1, b, a); }

// Endpoints of one partition from its colour values (0..255). False for the HDR modes.
bool endpoints(uint32_t cem, const uint8_t* c, uint8_t* e0, uint8_t* e1) {
    int v[8];
    for (int k = 0; k < 8; k++) v[k] = k < int((cem >> 2) + 1) * 2 ? c[k] : 0;
    switch (cem) {
        case 0: set(e0, v[0], v[0], v[0], 255), set(e1, v[1], v[1], v[1], 255); return true;
        case 1: {
            int l0 = (v[0] >> 2) | (v[1] & 0xC0), l1 = std::min(255, l0 + (v[1] & 0x3F));
            set(e0, l0, l0, l0, 255), set(e1, l1, l1, l1, 255);
            return true;
        }
        case 4: set(e0, v[0], v[0], v[0], v[2]), set(e1, v[1], v[1], v[1], v[3]); return true;
        case 5:
            transfer(v[1], v[0]), transfer(v[3], v[2]);
            set(e0, v[0], v[0], v[0], v[2]), set(e1, v[0] + v[1], v[0] + v[1], v[0] + v[1], v[2] + v[3]);
            return true;
        case 6:
            set(e0, (v[0] * v[3]) >> 8, (v[1] * v[3]) >> 8, (v[2] * v[3]) >> 8, 255), set(e1, v[0], v[1], v[2], 255);
            return true;
        case 10:
            set(e0, (v[0] * v[3]) >> 8, (v[1] * v[3]) >> 8, (v[2] * v[3]) >> 8, v[4]), set(e1, v[0], v[1], v[2], v[5]);
            return true;
        case 8:
        case 12: {
            int a0 = cem == 12 ? v[6] : 255, a1 = cem == 12 ? v[7] : 255;
            if (v[1] + v[3] + v[5] >= v[0] + v[2] + v[4])
                set(e0, v[0], v[2], v[4], a0), set(e1, v[1], v[3], v[5], a1);
            else
                blue_contract(e0, v[1], v[3], v[5], a1), blue_contract(e1, v[0], v[2], v[4], a0);
            return true;
        }
        case 9:
        case 13: {
            transfer(v[1], v[0]), transfer(v[3], v[2]), transfer(v[5], v[4]);
            if (cem == 13) transfer(v[7], v[6]);
            else v[6] = 255, v[7] = 0;
            if (v[1] + v[3] + v[5] >= 0)
                set(e0, v[0], v[2], v[4], v[6]), set(e1, v[0] + v[1], v[2] + v[3], v[4] + v[5], v[6] + v[7]);
            else
                blue_contract(e0, v[0] + v[1], v[2] + v[3], v[4] + v[5], v[6] + v[7]),
                    blue_contract(e1, v[0], v[2], v[4], v[6]);
            return true;
        }
        default: return false;  // 2, 3, 7, 11, 14, 15: HDR
    }
}

uint32_t hash52(uint32_t p) {
    p ^= p >> 15, p -= p << 17, p += p << 7, p += p << 4, p ^= p >> 5, p += p << 16, p ^= p >> 7, p ^= p >> 3;
    p ^= p << 6, p ^= p >> 17;
    return p;
}

// The partition seed and coefficients are constant for all texels in a block.
// ASTC images here are 2D, so z is always zero and its four coefficients do not contribute.
struct PartitionParams {
    uint32_t s[8], rnum;
    int count;
    bool small;
};

PartitionParams partition_params(uint32_t seed, int count, bool small) {
    seed += uint32_t(count - 1) * 1024;
    const uint32_t rnum = hash52(seed);
    PartitionParams p{{rnum & 0xF,          (rnum >> 4) & 0xF,  (rnum >> 8) & 0xF,  (rnum >> 12) & 0xF,
                       (rnum >> 16) & 0xF, (rnum >> 20) & 0xF, (rnum >> 24) & 0xF, (rnum >> 28) & 0xF},
                      rnum, count, small};
    int sh1, sh2;
    if (seed & 1) sh1 = seed & 2 ? 4 : 5, sh2 = count == 3 ? 6 : 5;
    else sh1 = count == 3 ? 6 : 5, sh2 = seed & 2 ? 4 : 5;
    for (int k = 0; k < 8; k++) p.s[k] = (p.s[k] * p.s[k]) >> (k & 1 ? sh2 : sh1);
    return p;
}

int partition_of(const PartitionParams& p, uint32_t x, uint32_t y) {
    if (p.small) x <<= 1, y <<= 1;
    const uint32_t a = (p.s[0] * x + p.s[1] * y + (p.rnum >> 14)) & 0x3F;
    const uint32_t b = (p.s[2] * x + p.s[3] * y + (p.rnum >> 10)) & 0x3F;
    const uint32_t c = p.count < 3 ? 0 : (p.s[4] * x + p.s[5] * y + (p.rnum >> 6)) & 0x3F;
    const uint32_t d = p.count < 4 ? 0 : (p.s[6] * x + p.s[7] * y + (p.rnum >> 2)) & 0x3F;
    if (a >= b && a >= c && a >= d) return 0;
    if (b >= c && b >= d) return 1;
    return c >= d ? 2 : 3;
}


// A fight streams textures in bursts: 70-120 in 60 frames took 10-18 ms a frame on the replay's path, and most of the
// windows under 25 fps were those. Everything below that is the same per block is therefore decided once, per footprint.

// Unquantized values, by range, digit (trit or quint) and low bits.
struct Unquantized {
    uint8_t color[21][5][256], weight[12][5][32];
    Unquantized() {
        std::memset(this, 0, sizeof *this);
        for (int r = 0; r < 21; r++) {
            const Range& q = kRanges[r];
            const int digits = q.trits ? 3 : q.quints ? 5 : 1;
            for (int d = 0; d < digits; d++)
                for (uint32_t low = 0; low < (1u << q.bits); low++) {
                    const IseValue v{uint8_t(d), low};
                    color[r][d][low] = unquantize_color(v, r);
                    if (r < 12) weight[r][d][low] = unquantize_weight(v, r);
                }
        }
    }
};

struct BlockMode {
    uint8_t wx = 0, wy = 0, range = 0;
    bool dual = false, ok = false;
};
struct BlockModes {
    BlockMode mode[2048];
    BlockModes() {
        for (uint32_t m = 0; m < 2048; m++) {
            int x = 0, y = 0, r = 0;
            bool d = false;
            if (decode_mode(m, x, y, d, r)) mode[m] = {uint8_t(x), uint8_t(y), uint8_t(r), d, true};
        }
    }
};

// Weight infill for one footprint and weight grid: for each texel, the four weights it blends and their factors.
struct WeightGrid {
    uint8_t at[144][4], factor[144][4];
    bool direct;  // a weight per texel, in order
};
std::atomic<const WeightGrid*> g_grids[9][9][11][11];

const WeightGrid* weight_grid(int bw, int bh, int wx, int wy) {
    std::atomic<const WeightGrid*>& slot = g_grids[bw - 4][bh - 4][wx - 2][wy - 2];
    if (const WeightGrid* g = slot.load(std::memory_order_acquire)) return g;
    auto* g = new WeightGrid{};
    g->direct = wx == bw && wy == bh;
    const int ds = (1024 + bw / 2) / (bw - 1), dt = (1024 + bh / 2) / (bh - 1);
    for (int t = 0, n = 0; t < bh; t++)
        for (int s = 0; s < bw; s++, n++) {
            const int gs = (ds * s * (wx - 1) + 32) >> 6, gt = (dt * t * (wy - 1) + 32) >> 6;
            const int js = std::min(gs >> 4, wx - 1), fs = gs & 15, jt = std::min(gt >> 4, wy - 1), ft = gt & 15;
            const int w11 = (fs * ft + 8) >> 4;
            const int s1 = std::min(js + 1, wx - 1), t1 = std::min(jt + 1, wy - 1);
            g->at[n][0] = uint8_t(jt * wx + js), g->at[n][1] = uint8_t(jt * wx + s1);
            g->at[n][2] = uint8_t(t1 * wx + js), g->at[n][3] = uint8_t(t1 * wx + s1);
            g->factor[n][0] = uint8_t(16 - fs - ft + w11), g->factor[n][1] = uint8_t(fs - w11);
            g->factor[n][2] = uint8_t(ft - w11), g->factor[n][3] = uint8_t(w11);
            g->direct = g->direct && g->at[n][0] == n && g->factor[n][0] == 16;
        }
    const WeightGrid* seen = nullptr;
    if (slot.compare_exchange_strong(seen, g, std::memory_order_acq_rel)) return g;
    delete g;
    return seen;
}

// The partition of each texel, per footprint, partition count and seed.
std::atomic<const uint8_t*> g_partition_maps[9][9][3][1024];

const uint8_t* partition_map(int bw, int bh, int partitions, uint32_t seed) {
    std::atomic<const uint8_t*>& slot = g_partition_maps[bw - 4][bh - 4][partitions - 2][seed];
    if (const uint8_t* m = slot.load(std::memory_order_acquire)) return m;
    auto* m = new uint8_t[size_t(bw) * bh];
    const PartitionParams params = partition_params(seed, partitions, bw * bh < 31);
    for (int t = 0, n = 0; t < bh; t++)
        for (int s = 0; s < bw; s++, n++) m[n] = uint8_t(partition_of(params, uint32_t(s), uint32_t(t)));
    const uint8_t* seen = nullptr;
    if (slot.compare_exchange_strong(seen, m, std::memory_order_acq_rel)) return m;
    delete[] m;
    return seen;
}

inline void infill(const WeightGrid& g, const uint8_t* w, int stride, int plane, int texels, uint8_t* out) {
    if (g.direct) {
        for (int n = 0; n < texels; n++) out[n] = w[n * stride + plane];
        return;
    }
    for (int n = 0; n < texels; n++) {
        const uint8_t* at = g.at[n];
        const uint8_t* f = g.factor[n];
        out[n] = uint8_t((w[at[0] * stride + plane] * f[0] + w[at[1] * stride + plane] * f[1] +
                          w[at[2] * stride + plane] * f[2] + w[at[3] * stride + plane] * f[3] + 8) >> 4);
    }
}

bool error_block_to(uint32_t bw, uint32_t bh, uint8_t* rgba, size_t stride) {
    for (uint32_t t = 0; t < bh; t++)
        for (uint32_t s = 0; s < bw; s++) {
            uint8_t* o = rgba + t * stride + 4 * s;
            o[0] = 0xFF, o[1] = 0, o[2] = 0xFF, o[3] = 0xFF;
        }
    return false;
}

// `stride` bytes between rows, so a whole block goes straight into its image.
bool decode_block(const uint8_t* src, uint32_t bw, uint32_t bh, bool srgb, uint8_t* rgba, size_t stride) {
    static const Unquantized& unq = *new Unquantized();
    static const BlockModes& modes = *new BlockModes();
    Block b;
    std::memcpy(b.w, src, 16);
    const uint32_t mode = b.get(0, 11);
    if ((mode & 0x1FF) == 0x1FC) {  // void extent: one colour (the extent coordinates are only a hint)
        if (mode & 0x200) return error_block_to(bw, bh, rgba, stride);  // HDR
        uint8_t c[4];
        for (int k = 0; k < 4; k++) c[k] = uint8_t(b.get(64 + 16 * k, 16) >> 8);
        for (uint32_t t = 0; t < bh; t++)
            for (uint32_t s = 0; s < bw; s++) std::memcpy(rgba + t * stride + 4 * s, c, 4);
        return true;
    }
    const BlockMode& bm = modes.mode[mode];
    const int wx = bm.wx, wy = bm.wy, wrange = bm.range;
    const bool dual = bm.dual;
    if (!bm.ok || wx > int(bw) || wy > int(bh)) return error_block_to(bw, bh, rgba, stride);
    const int partitions = int(b.get(11, 2)) + 1, planes = dual ? 2 : 1, nweights = wx * wy * planes;
    const int wbits = ise_bits(nweights, wrange);
    if (nweights > 64 || wbits < 24 || wbits > 96 || (dual && partitions == 4))
        return error_block_to(bw, bh, rgba, stride);
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
    if (ncolors > 18 || below < color_start) return error_block_to(bw, bh, rgba, stride);
    int crange = -1;
    for (int r = 20; r >= 0 && crange < 0; r--)
        if (ise_bits(ncolors, r) <= below - color_start) crange = r;
    if (crange < 4) return error_block_to(bw, bh, rgba, stride);  // fewer than 6 levels: not a legal encoding
    IseValue cv[18];
    ise_decode(b, color_start, ncolors, crange, cv);
    uint8_t colors[18], ep[4][2][4];
    for (int k = 0; k < ncolors; k++) colors[k] = unq.color[crange][cv[k].digit][cv[k].low];
    for (int p = 0, at = 0; p < partitions; at += int((cem[p] >> 2) + 1) * 2, p++)
        if (!endpoints(cem[p], colors + at, ep[p][0], ep[p][1])) return error_block_to(bw, bh, rgba, stride);

    const Block rev{{reverse64(b.w[1]), reverse64(b.w[0])}};  // the weights are stored from bit 127 down
    IseValue wv[64];
    ise_decode(rev, 0, nweights, wrange, wv);
    uint8_t w[64];
    for (int k = 0; k < nweights; k++) w[k] = unq.weight[wrange][wv[k].digit][wv[k].low];

    const int texels = int(bw * bh);
    const WeightGrid& grid = *weight_grid(int(bw), int(bh), wx, wy);
    uint8_t pw[2][144];
    infill(grid, w, planes, 0, texels, pw[0]);
    if (dual) infill(grid, w, planes, 1, texels, pw[1]);
    const uint8_t* part = partitions > 1 ? partition_map(int(bw), int(bh), partitions, seed) : nullptr;

    // The blend of each channel, ((c0 * (64 - wt) + c1 * wt + 32) >> 6) >> 8, as c0 * 64 + 32 + (c1 - c0) * wt.
#if defined(__SSE4_1__)
    __m128i base[4], delta[4];
    for (int p = 0; p < partitions; p++) {
        alignas(16) int32_t c0[4], d[4];
        for (int c = 0; c < 4; c++) {
            const int32_t e0 = ep[p][0][c], e1 = ep[p][1][c];
            const int32_t a = srgb ? e0 << 8 | 0x80 : e0 * 257, z = srgb ? e1 << 8 | 0x80 : e1 * 257;
            c0[c] = a * 64 + 32, d[c] = z - a;
        }
        base[p] = _mm_load_si128(reinterpret_cast<const __m128i*>(c0));
        delta[p] = _mm_load_si128(reinterpret_cast<const __m128i*>(d));
    }
    const __m128i low_bytes = _mm_setr_epi8(0, 4, 8, 12, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1);
    const int ccs_lane = int(ccs);
    int n = 0;
    for (uint32_t t = 0; t < bh; t++) {
        uint8_t* o = rgba + t * stride;
        for (uint32_t s = 0; s < bw; s++, n++, o += 4) {
            const int p = part ? part[n] : 0;
            __m128i wt = _mm_set1_epi32(pw[0][n]);
            if (dual) {
                alignas(16) int32_t lanes[4] = {pw[0][n], pw[0][n], pw[0][n], pw[0][n]};
                lanes[ccs_lane] = pw[1][n];
                wt = _mm_load_si128(reinterpret_cast<const __m128i*>(lanes));
            }
            const __m128i v = _mm_srli_epi32(_mm_add_epi32(base[p], _mm_mullo_epi32(delta[p], wt)), 14);
            const uint32_t px = uint32_t(_mm_cvtsi128_si32(_mm_shuffle_epi8(v, low_bytes)));
            std::memcpy(o, &px, 4);
        }
    }
#else
    int32_t base[4][4], delta[4][4];
    for (int p = 0; p < partitions; p++)
        for (int c = 0; c < 4; c++) {
            const int32_t e0 = ep[p][0][c], e1 = ep[p][1][c];
            const int32_t a = srgb ? e0 << 8 | 0x80 : e0 * 257, z = srgb ? e1 << 8 | 0x80 : e1 * 257;
            base[p][c] = a * 64 + 32, delta[p][c] = z - a;
        }
    int n = 0;
    for (uint32_t t = 0; t < bh; t++) {
        uint8_t* o = rgba + t * stride;
        for (uint32_t s = 0; s < bw; s++, n++, o += 4) {
            const int p = part ? part[n] : 0;
            for (int c = 0; c < 4; c++) {
                const int32_t wt = dual && uint32_t(c) == ccs ? pw[1][n] : pw[0][n];
                o[c] = uint8_t(uint32_t(base[p][c] + delta[p][c] * wt) >> 14);
            }
        }
    }
#endif
    return true;
}

}  // namespace

bool astc_decode_block(const uint8_t* src, uint32_t bw, uint32_t bh, bool srgb, uint8_t* rgba) {
    return decode_block(src, bw, bh, srgb, rgba, size_t(bw) * 4);
}

namespace {

struct AstcImageRows {
    const uint8_t* blocks;
    uint8_t* rgba;
    uint32_t width, height, bw, bh, bx, by;
    uint64_t rows;
    bool srgb;

    uint64_t decode(uint64_t first, uint64_t last) const {
        uint64_t bad = 0;
        uint8_t texels[12 * 12 * 4];
        const size_t stride = size_t(width) * 4;
        for (uint64_t row = first; row < last; row++) {
            const uint32_t z = uint32_t(row / by), y = uint32_t(row % by);
            const uint32_t h = std::min(bh, height - y * bh);
            uint8_t* line = rgba + (uint64_t(z) * height + y * bh) * stride;
            for (uint32_t x = 0; x < bx; x++) {
                const uint8_t* src = blocks + (row * bx + x) * 16;
                const uint32_t w = std::min(bw, width - x * bw);
                uint8_t* to = line + size_t(x) * bw * 4;
                if (w == bw && h == bh) {
                    bad += !decode_block(src, bw, bh, srgb, to, stride);
                    continue;
                }
                bad += !decode_block(src, bw, bh, srgb, texels, size_t(bw) * 4);  // the image ends inside this block
                for (uint32_t r = 0; r < h; r++) std::memcpy(to + r * stride, texels + r * bw * 4, w * 4);
            }
        }
        return bad;
    }
};

// One bounded team for all uploads. Recreating threads for each image/mip adds
// scheduler and allocator work during a texture burst. The caller still waits
// for all texel stores; only the host scheduling changes, not GPU visibility.
class AstcWorkers {
public:
    explicit AstcWorkers(unsigned count) {
        for (unsigned k = 1; k < std::min(count, 6u); ++k) {
            try {
                threads_[k - 1] = std::thread([this, k] { worker(k); });
                ++count_;
            } catch (const std::system_error&) {
                break;  // Resource pressure: use only the helpers that started.
            } catch (const std::bad_alloc&) {
                break;  // std::thread may also fail to allocate its callable state.
            }
        }
    }

    uint64_t run(const AstcImageRows& image) {
        // Uploads normally have one caller. Independent callers keep their own
        // work rather than replacing an in-flight job or queueing behind its image.
        std::unique_lock use(use_, std::try_to_lock);
        if (!use.owns_lock() || count_ == 1 || image.rows < 2) return image.decode(0, image.rows);
        const unsigned active = unsigned(std::min<uint64_t>(count_, image.rows));
        std::unique_lock lk(mutex_);
        image_ = &image;
        active_ = active;
        pending_ = count_ - 1;
        ++generation_;
        ready_.notify_all();
        lk.unlock();
        uint64_t bad = image.decode(0, image.rows / active);
        lk.lock();
        done_.wait(lk, [&] { return pending_ == 0; });
        for (unsigned k = 1; k < active; ++k) bad += bad_[k - 1];
        image_ = nullptr;
        return bad;
    }

private:
    friend struct AstcWorkersTestAccess;
    void worker(unsigned k) {
        uint64_t seen = 0;
        for (;;) {
            std::unique_lock lk(mutex_);
            ready_.wait(lk, [&] { return generation_ != seen; });
            seen = generation_;
            const AstcImageRows* image = image_;
            const unsigned active = active_;
            lk.unlock();
            const uint64_t bad = k < active ? image->decode(image->rows * k / active,
                                                            image->rows * (k + 1) / active) : 0;
            lk.lock();
            bad_[k - 1] = bad;
            if (--pending_ == 0) done_.notify_one();
        }
    }

    std::mutex use_, mutex_;
    std::condition_variable ready_, done_;
    std::array<std::thread, 5> threads_;
    std::array<uint64_t, 5> bad_{};
    const AstcImageRows* image_ = nullptr;
    uint64_t generation_ = 0;
    unsigned count_ = 1, active_ = 1, pending_ = 0;
};

}  // namespace

uint64_t astc_decode_image(const uint8_t* blocks, uint32_t width, uint32_t height, uint32_t slices, uint32_t bw,
                           uint32_t bh, bool srgb, uint8_t* rgba) {
    const uint32_t bx = (width + bw - 1) / bw, by = (height + bh - 1) / bh;
    const AstcImageRows image{blocks, rgba, width, height, bw, bh, bx, by, uint64_t(slices) * by, srgb};
    const uint64_t total = image.rows * bx;
    if (total < 8192 || image.rows < 2) return image.decode(0, image.rows);
    static const unsigned count = std::min(6u, std::max(1u, std::thread::hardware_concurrency() / 2));
    if (count <= 1) return image.decode(0, image.rows);
    // NX_ASTC_POOL=0 restores the previous per-image threads for A/B. Retain
    // its 8192-block threshold: parallelizing smaller images cost more CPU time.
    static const bool pooled = [] {
        const char* e = std::getenv("NX_ASTC_POOL");
        return !(e && e[0] == '0' && !e[1]);
    }();
    if (pooled) {
        // The detached replay thread can still be decoding during process exit.
        // Keep the sleeping workers alive until process teardown, as for write-back.
        static AstcWorkers* pool = new AstcWorkers(count);
        return pool->run(image);
    }
    std::vector<std::thread> pool;
    std::vector<uint64_t> bad(count, 0);
    for (unsigned k = 1; k < count; k++)
        pool.emplace_back([&, k] { bad[k] = image.decode(image.rows * k / count, image.rows * (k + 1) / count); });
    bad[0] = image.decode(0, image.rows / count);
    for (auto& t : pool) t.join();
    uint64_t sum = 0;
    for (uint64_t b : bad) sum += b;
    return sum;
}

}  // namespace nx::nvn
