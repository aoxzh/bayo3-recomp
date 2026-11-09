// BC7 mode 6 encoder (nvn_bc7_encode.h). Written from the BC7 format description (Khronos Data Format
// Specification, "BC7"): mode 6 is bit 6 of the first byte set, then R0 R1 G0 G1 B0 B1 A0 A1 at 7 bits each, the
// p-bits P0 P1, and 16 four-bit indices whose first (the anchor) drops its top bit, which must be 0.
//
// Per block: the principal axis of the 16 colours (RGBA, a few power iterations of the covariance), the extremes of
// the projections on it as endpoints, quantized to 7 bits with the better p-bit, the nearest of the 16 interpolated
// colours per texel, then one least-squares refit of the endpoints to those indices, kept when it lowers the error.
// It runs on a background thread (nvn_astc_bc7.cpp), so it favours quality over speed only as far as one refit.
#include "nvn_bc7_encode.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace nx::nvn {
namespace {

constexpr int kWeights[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

struct Endpoints {
    int q[2][4];  // 7-bit values
    int p[2];     // p-bits
};

int endpoint8(const Endpoints& e, int i, int c) { return (e.q[i][c] << 1) | e.p[i]; }

// The best 7-bit value and p-bit for an 8-bit endpoint colour (one p-bit shared by its four channels).
void quantize(const float c[4], int q[4], int& p) {
    int best_err = 1 << 30;
    for (int pb = 0; pb < 2; pb++) {
        int err = 0, qq[4];
        for (int k = 0; k < 4; k++) {
            int v = int(std::lround((c[k] - pb) / 2.0f));
            v = std::clamp(v, 0, 127);
            qq[k] = v;
            int d = ((v << 1) | pb) - int(std::lround(std::clamp(c[k], 0.0f, 255.0f)));
            err += d * d;
        }
        if (err < best_err) {
            best_err = err;
            p = pb;
            std::memcpy(q, qq, sizeof qq);
        }
    }
}

// Palette of the 16 colours the endpoints give.
void palette(const Endpoints& e, int pal[16][4]) {
    for (int i = 0; i < 16; i++)
        for (int c = 0; c < 4; c++) {
            int a = endpoint8(e, 0, c), b = endpoint8(e, 1, c);
            pal[i][c] = ((64 - kWeights[i]) * a + kWeights[i] * b + 32) >> 6;
        }
}

// Nearest palette entry per texel; returns the total squared error.
int assign(const uint8_t* px, const Endpoints& e, int idx[16]) {
    int pal[16][4];
    palette(e, pal);
    int total = 0;
    for (int t = 0; t < 16; t++) {
        int best = 0, best_err = 1 << 30;
        for (int i = 0; i < 16; i++) {
            int err = 0;
            for (int c = 0; c < 4; c++) {
                int d = int(px[t * 4 + c]) - pal[i][c];
                err += d * d;
            }
            if (err < best_err) best_err = err, best = i;
        }
        idx[t] = best;
        total += best_err;
    }
    return total;
}

void pack(Endpoints e, int idx[16], uint8_t* out) {
    if (idx[0] >= 8) {  // the anchor's top bit is implied 0: swap the endpoints and mirror every index
        std::swap(e.q[0], e.q[1]);
        std::swap(e.p[0], e.p[1]);
        for (int t = 0; t < 16; t++) idx[t] = 15 - idx[t];
    }
    uint8_t b[16] = {};
    int pos = 0;
    auto put = [&](uint32_t v, int n) {
        for (int k = 0; k < n; k++, pos++)
            if (v >> k & 1) b[pos >> 3] |= uint8_t(1u << (pos & 7));
    };
    put(1u << 6, 7);  // mode 6
    for (int c = 0; c < 4; c++) {
        put(uint32_t(e.q[0][c]), 7);
        put(uint32_t(e.q[1][c]), 7);
    }
    put(uint32_t(e.p[0]), 1);
    put(uint32_t(e.p[1]), 1);
    put(uint32_t(idx[0]), 3);
    for (int t = 1; t < 16; t++) put(uint32_t(idx[t]), 4);
    std::memcpy(out, b, 16);
}

}  // namespace

void bc7_encode_block(const uint8_t* px, uint8_t* out) {
    float mean[4] = {};
    for (int t = 0; t < 16; t++)
        for (int c = 0; c < 4; c++) mean[c] += px[t * 4 + c];
    for (float& m : mean) m /= 16.0f;
    float cov[4][4] = {};
    for (int t = 0; t < 16; t++) {
        float d[4];
        for (int c = 0; c < 4; c++) d[c] = px[t * 4 + c] - mean[c];
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++) cov[i][j] += d[i] * d[j];
    }
    // Start the power iteration from the channel of largest spread: a single-channel block converges at once.
    float axis[4] = {0, 0, 0, 0};
    int widest = 0;
    for (int c = 1; c < 4; c++)
        if (cov[c][c] > cov[widest][widest]) widest = c;
    axis[widest] = 1.0f;
    for (int it = 0; it < 6; it++) {
        float n[4] = {};
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++) n[i] += cov[i][j] * axis[j];
        float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2] + n[3] * n[3]);
        if (len < 1e-6f) break;
        for (int i = 0; i < 4; i++) axis[i] = n[i] / len;
    }
    float lo = 1e30f, hi = -1e30f;
    for (int t = 0; t < 16; t++) {
        float s = 0;
        for (int c = 0; c < 4; c++) s += (px[t * 4 + c] - mean[c]) * axis[c];
        lo = std::min(lo, s), hi = std::max(hi, s);
    }
    float e0[4], e1[4];
    for (int c = 0; c < 4; c++) {
        e0[c] = std::clamp(mean[c] + axis[c] * lo, 0.0f, 255.0f);
        e1[c] = std::clamp(mean[c] + axis[c] * hi, 0.0f, 255.0f);
    }
    Endpoints e{};
    quantize(e0, e.q[0], e.p[0]);
    quantize(e1, e.q[1], e.p[1]);
    int idx[16];
    int err = assign(px, e, idx);
    // One least-squares refit: endpoints that best reproduce the texels with these weights.
    if (err > 0) {
        float aa = 0, ab = 0, bb = 0, ax[4] = {}, bx[4] = {};
        for (int t = 0; t < 16; t++) {
            float w = kWeights[idx[t]] / 64.0f, a = 1.0f - w;
            aa += a * a, ab += a * w, bb += w * w;
            for (int c = 0; c < 4; c++) ax[c] += a * px[t * 4 + c], bx[c] += w * px[t * 4 + c];
        }
        float det = aa * bb - ab * ab;
        if (std::fabs(det) > 1e-6f) {
            float r0[4], r1[4];
            for (int c = 0; c < 4; c++) {
                r0[c] = std::clamp((ax[c] * bb - bx[c] * ab) / det, 0.0f, 255.0f);
                r1[c] = std::clamp((bx[c] * aa - ax[c] * ab) / det, 0.0f, 255.0f);
            }
            Endpoints f{};
            quantize(r0, f.q[0], f.p[0]);
            quantize(r1, f.q[1], f.p[1]);
            int idx2[16];
            int err2 = assign(px, f, idx2);
            if (err2 < err) e = f, std::memcpy(idx, idx2, sizeof idx), err = err2;
        }
    }
    pack(e, idx, out);
}

void bc7_encode_image(const uint8_t* rgba, uint32_t width, uint32_t height, uint32_t slices, uint8_t* out) {
    const uint32_t bx = (width + 3) / 4, by = (height + 3) / 4;
    uint8_t block[64];
    for (uint32_t s = 0; s < slices; s++) {
        const uint8_t* layer = rgba + uint64_t(s) * width * height * 4;
        for (uint32_t y = 0; y < by; y++)
            for (uint32_t x = 0; x < bx; x++) {
                for (uint32_t j = 0; j < 4; j++)
                    for (uint32_t i = 0; i < 4; i++) {
                        const uint32_t sx = std::min(x * 4 + i, width - 1), sy = std::min(y * 4 + j, height - 1);
                        std::memcpy(block + (j * 4 + i) * 4, layer + (uint64_t(sy) * width + sx) * 4, 4);
                    }
                bc7_encode_block(block, out);
                out += 16;
            }
    }
}

}  // namespace nx::nvn
